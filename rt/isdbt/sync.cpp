#include "sync.h"

#include <algorithm>
#include <cassert>
#include <cfenv>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>

#include "pilots.h"
#include "simd.h"

namespace isdbt {

namespace {

// --- 数値ユーティリティ ---

double RoundHalfEven(double v) { return std::nearbyint(v); }   // 既定の丸めモード = 偶数丸め（np.round と同じ）

long long FloorDiv(long long a, long long b) {
    long long q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}

// 第 1 種変形ベッセル関数 I0（カイザー窓用）
double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-18 * sum) break;
    }
    return sum;
}

std::vector<double> KaiserWindow(int n, double beta) {
    std::vector<double> w(n);
    const double denom = BesselI0(beta);
    for (int i = 0; i < n; ++i) {
        double t = 2.0 * i / (n - 1) - 1.0;
        w[i] = BesselI0(beta * std::sqrt(std::max(0.0, 1.0 - t * t))) / denom;
    }
    return w;
}

double Sinc(double x) {
    if (x == 0.0) return 1.0;
    const double px = kPi * x;
    return std::sin(px) / px;
}

// Python Fraction(x).limit_denominator(md) 相当（連分数 + 準収束分数）
void LimitDenominator(double x, long long md, long long* num, long long* den) {
    // x を正確な分数 n/d にする（d は 2 のべき）
    int e = 0;
    double m = std::frexp(x, &e);           // x = m * 2^e、|m| ∈ [0.5, 1)
    long long n = static_cast<long long>(std::ldexp(m, 53));
    long long d = 1LL << 53;
    e -= 0;
    if (e > 0) {
        n <<= std::min(e, 10);              // ratio は 1 近傍なので小さい（オーバーフロー対策で制限）
        e -= std::min(e, 10);
        while (e-- > 0) n *= 2;
    } else {
        for (int i = 0; i < -e; ++i) d *= 2;
    }
    long long g = 0;
    {
        long long a = n < 0 ? -n : n, b = d;
        while (b) { long long t = a % b; a = b; b = t; }
        g = a ? a : 1;
    }
    n /= g;
    d /= g;
    if (d <= md) { *num = n; *den = d; return; }
    long long p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    long long nn = n, dd = d;
    while (true) {
        long long a = FloorDiv(nn, dd);
        long long q2 = q0 + a * q1;
        if (q2 > md) break;
        long long p2 = p0 + a * p1;
        p0 = p1; q0 = q1; p1 = p2; q1 = q2;
        long long t = nn - a * dd;
        nn = dd;
        dd = t;
        if (dd == 0) break;
    }
    if (dd == 0) { *num = p1; *den = q1; return; }
    long long k = (md - q0) / q1;
    double b1 = static_cast<double>(p0 + k * p1) / (q0 + k * q1);
    double b2 = static_cast<double>(p1) / q1;
    if (std::abs(b2 - x) <= std::abs(b1 - x)) {
        *num = p1; *den = q1;
    } else {
        *num = p0 + k * p1; *den = q0 + k * q1;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 有理数比リサンプル（scipy.signal.resample_poly 相当）
// ---------------------------------------------------------------------------
std::vector<cfloat> ResampleToNominal(const std::vector<cfloat>& x, double fs) {
    long long up = 1, down = 1;
    LimitDenominator(kSampleRateHz / fs, 10000, &up, &down);
    if (up == down) return x;

    const long long max_rate = std::max(up, down);
    const long long half_len = 10 * max_rate;
    const long long n_taps = 2 * half_len + 1;
    const double fc = 1.0 / max_rate;           // ナイキスト = 1 の正規化カットオフ
    std::vector<double> h(n_taps);
    std::vector<double> w = KaiserWindow(static_cast<int>(n_taps), 5.0);
    double sum = 0.0;
    for (long long i = 0; i < n_taps; ++i) {
        h[i] = fc * Sinc(fc * (i - half_len)) * w[i];
        sum += h[i];
    }
    for (double& v : h) v = v / sum * up;       // DC ゲイン 1 に正規化してから up 倍

    const long long n_in = static_cast<long long>(x.size());
    const long long n_out = (n_in * up + down - 1) / down;
    std::vector<cfloat> y(static_cast<size_t>(n_out));
    for (long long m2 = 0; m2 < n_out; ++m2) {
        // y[m] = Σ_n h[m*down + half_len - up*n] * x[n]
        const long long center = m2 * down + half_len;
        long long n_min = FloorDiv(center - (n_taps - 1) + up - 1, up);   // ceil((center-nt+1)/up)
        long long n_max = FloorDiv(center, up);
        n_min = std::max(n_min, 0LL);
        n_max = std::min(n_max, n_in - 1);
        std::complex<double> acc = 0.0;
        for (long long n3 = n_min; n3 <= n_max; ++n3) {
            acc += std::complex<double>(x[static_cast<size_t>(n3)]) * h[static_cast<size_t>(center - up * n3)];
        }
        y[static_cast<size_t>(m2)] = static_cast<cfloat>(acc);
    }
    return y;
}

// ---------------------------------------------------------------------------
// 分数リサンプル（多相 sinc、ref/isdbt/sync.py と同じ係数）
// ---------------------------------------------------------------------------
namespace {

constexpr int kFracQ = 256;
constexpr int kFracTaps = 48;

struct FracFilters {
    std::vector<float> h;   // (Q, taps)
    std::vector<float> hd;  // (Q, 2*taps): 各タップを 2 回並べたもの（交互配置 (re, im) の列と 4 レーンで積和するため）
    FracFilters() {
        const double fc = 0.45, beta = 8.0;
        const int c = kFracTaps / 2 - 1;
        std::vector<double> kw = KaiserWindow(kFracTaps, beta);
        h.resize(static_cast<size_t>(kFracQ) * kFracTaps);
        for (int q = 0; q < kFracQ; ++q) {
            const double mu = static_cast<double>(q) / kFracQ;
            double sum = 0.0;
            std::vector<double> row(kFracTaps);
            for (int i = 0; i < kFracTaps; ++i) {
                const double t = i - c - mu;
                // np.interp(t, i - c, kaiser): 整数位置のカイザー窓を線形補間（端はクランプ）
                const double pos = t + c;       // 0..taps-1 の座標系
                double w;
                if (pos <= 0.0) {
                    w = kw[0];
                } else if (pos >= kFracTaps - 1) {
                    w = kw[kFracTaps - 1];
                } else {
                    const int i0 = static_cast<int>(pos);
                    const double fr = pos - i0;
                    w = kw[i0] * (1.0 - fr) + kw[i0 + 1] * fr;
                }
                row[i] = 2.0 * fc * Sinc(2.0 * fc * t) * w;
                sum += row[i];
            }
            for (int i = 0; i < kFracTaps; ++i) {
                h[static_cast<size_t>(q) * kFracTaps + i] = static_cast<float>(row[i] / sum);
            }
        }
        hd.resize(h.size() * 2);
        for (size_t i = 0; i < h.size(); ++i) hd[2 * i] = hd[2 * i + 1] = h[i];
    }
};

const FracFilters& FracOf() {
    static const FracFilters f;
    return f;
}

}  // namespace

namespace {

inline cfloat InterpOne(const std::vector<cfloat>& x, double t, const FracFilters& ff) {
    const int c = kFracTaps / 2 - 1;
    const long long n_in = static_cast<long long>(x.size());
    long long n3 = static_cast<long long>(std::floor(t));
    long long q = static_cast<long long>(RoundHalfEven((t - n3) * kFracQ));
    n3 += q / kFracQ;
    q %= kFracQ;
    const float* hq = &ff.h[static_cast<size_t>(q) * kFracTaps];
    const long long base = n3 - c;
    if (base >= 0 && base + kFracTaps <= n_in) {
        // 内側（大半）: 複素列を float 列 (re, im, re, im, …) と見て、同じ係数を 2 回並べた hd と積和する。
        // 4 本の累算器に分けて加算の依存連鎖を切る（和の順序が変わるだけで、ref/ との差は float の丸め程度）
        const float* xp = reinterpret_cast<const float*>(&x[static_cast<size_t>(base)]);
        const float* hdq = &ff.hd[static_cast<size_t>(q) * 2 * kFracTaps];
#ifdef ISDBT_NEON
        // 96 float = 24 ベクタを 8 本の累算器に（FMA のレイテンシ隠し）
        float32x4_t a0 = vdupq_n_f32(0.0f), a1 = a0, a2 = a0, a3 = a0, a4 = a0, a5 = a0, a6 = a0, a7 = a0;
        for (int i = 0; i < 2 * kFracTaps; i += 32) {
            a0 = vfmaq_f32(a0, vld1q_f32(xp + i), vld1q_f32(hdq + i));
            a1 = vfmaq_f32(a1, vld1q_f32(xp + i + 4), vld1q_f32(hdq + i + 4));
            a2 = vfmaq_f32(a2, vld1q_f32(xp + i + 8), vld1q_f32(hdq + i + 8));
            a3 = vfmaq_f32(a3, vld1q_f32(xp + i + 12), vld1q_f32(hdq + i + 12));
            a4 = vfmaq_f32(a4, vld1q_f32(xp + i + 16), vld1q_f32(hdq + i + 16));
            a5 = vfmaq_f32(a5, vld1q_f32(xp + i + 20), vld1q_f32(hdq + i + 20));
            a6 = vfmaq_f32(a6, vld1q_f32(xp + i + 24), vld1q_f32(hdq + i + 24));
            a7 = vfmaq_f32(a7, vld1q_f32(xp + i + 28), vld1q_f32(hdq + i + 28));
        }
        const float32x4_t a = vaddq_f32(vaddq_f32(vaddq_f32(a0, a1), vaddq_f32(a2, a3)),
                                        vaddq_f32(vaddq_f32(a4, a5), vaddq_f32(a6, a7)));
        return cfloat(vgetq_lane_f32(a, 0) + vgetq_lane_f32(a, 2), vgetq_lane_f32(a, 1) + vgetq_lane_f32(a, 3));
#else
        float a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int i = 0; i < 2 * kFracTaps; i += 8) {
            for (int k = 0; k < 8; ++k) a[k] += xp[i + k] * hdq[i + k];
        }
        return cfloat(a[0] + a[2] + a[4] + a[6], a[1] + a[3] + a[5] + a[7]);
#endif
    }
    cfloat acc(0.0f, 0.0f);
    for (int i = 0; i < kFracTaps; ++i) {
        const long long idx = std::clamp(base + i, 0LL, n_in - 1);
        acc += x[static_cast<size_t>(idx)] * hq[i];
    }
    return acc;
}

}  // namespace

void ResampleAt(const std::vector<cfloat>& x, const double* t, size_t n, cfloat* out) {
    const FracFilters& ff = FracOf();
    for (size_t k = 0; k < n; ++k) out[k] = InterpOne(x, t[k], ff);
}

std::vector<cfloat> ResampleFractional(const std::vector<cfloat>& x, double ratio,
                                       long long m0, long long m1) {
    const FracFilters& ff = FracOf();
    std::vector<cfloat> y(static_cast<size_t>(std::max(0LL, m1 - m0)));
    for (long long m2 = m0; m2 < m1; ++m2) {
        y[static_cast<size_t>(m2 - m0)] = InterpOne(x, m2 * ratio, ff);
    }
    return y;
}

// ---------------------------------------------------------------------------
// GI 相関・タイミング・CFO
// ---------------------------------------------------------------------------
namespace {

void RemoveDc(std::vector<cfloat>* x) {
    std::complex<double> acc = 0.0;
    for (const cfloat& v : *x) acc += std::complex<double>(v);
    const cfloat mean = static_cast<cfloat>(acc / static_cast<double>(x->size()));
    for (cfloat& v : *x) v -= mean;
}

// 整数 CFO 探索用: 最強 5 ビン（LO リークなどの狭帯域妨害）を除外する。
// 閾値方式（中央値比）は録音先頭の低電力区間でパイロットまで除外してしまう（ref/isdbt/sync.py 参照）
void ExcludeNarrowbandSpikes(std::vector<double>* pw) {
    constexpr int kDrop = 5;
    std::vector<size_t> idx(pw->size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::partial_sort(idx.begin(), idx.begin() + kDrop, idx.end(),
                      [&](size_t a, size_t b) { return (*pw)[a] > (*pw)[b]; });
    for (int i = 0; i < kDrop; ++i) (*pw)[idx[static_cast<size_t>(i)]] = 0.0;
}

}  // namespace

namespace {

// s[n] = Σ_{i<Ng} conj(x[n+i]) x[n+i+N]（長さ len(x) - N - Ng + 1、double 累積）
std::vector<std::complex<double>> GiCorrelation(const cfloat* x, size_t len, int N, int Ng) {
    const size_t nc = len - N;
    std::vector<std::complex<double>> s(nc - Ng + 1);
    std::complex<double> acc = 0.0;
    for (int i = 0; i < Ng; ++i) acc += std::complex<double>(std::conj(x[i])) * std::complex<double>(x[i + N]);
    s[0] = acc;
    for (size_t n = 1; n < s.size(); ++n) {
        acc -= std::complex<double>(std::conj(x[n - 1])) * std::complex<double>(x[n - 1 + N]);
        acc += std::complex<double>(std::conj(x[n + Ng - 1])) * std::complex<double>(x[n + Ng - 1 + N]);
        s[n] = acc;
    }
    return s;
}

struct FoldPeaks {
    std::vector<double> idx, pos, pwr;
    std::vector<std::complex<double>> val;
    int chunk = 0;
};

// 信号を chunk_syms シンボルずつ区切り、GI 相関をシンボル周期 T で複素のまま畳んでピークを探す
FoldPeaks GiFoldChunks(const std::vector<cfloat>& x, int N, int Ng, int chunk_syms = 64,
                       bool with_power = false) {
    const int T = N + Ng;
    const long long n_sym = (static_cast<long long>(x.size()) - N - Ng) / T;
    const int chunk = static_cast<int>(std::min<long long>(chunk_syms, n_sym));
    if (chunk < 1) throw std::runtime_error("信号が短すぎる");
    FoldPeaks out;
    out.chunk = chunk;
    std::vector<std::complex<double>> fold(T);
    for (long long j0 = 0; j0 + chunk <= n_sym; j0 += chunk) {
        const size_t seg_len = static_cast<size_t>(chunk) * T + N + Ng;
        auto s = GiCorrelation(&x[static_cast<size_t>(j0) * T], seg_len, N, Ng);
        std::fill(fold.begin(), fold.end(), std::complex<double>(0.0));
        for (int j = 0; j < chunk; ++j) {
            const std::complex<double>* sp = &s[static_cast<size_t>(j) * T];
            for (int r = 0; r < T; ++r) fold[r] += sp[r];
        }
        int best = 0;
        double best_mag = -1.0;
        for (int r = 0; r < T; ++r) {
            const double m = std::norm(fold[r]);
            if (m > best_mag) { best_mag = m; best = r; }
        }
        out.idx.push_back(j0 + (chunk - 1) / 2.0);
        out.pos.push_back(best);
        out.val.push_back(fold[best]);
        if (with_power) {
            double p = 0.0;
            const size_t np = static_cast<size_t>(chunk) * T;
            for (size_t k = 0; k < np; ++k) p += std::norm(x[static_cast<size_t>(j0) * T + k]);
            out.pwr.push_back(p / np);
        }
    }
    return out;
}

// 線形補間（端は端値、np.interp と同じ）
double Interp1(double xq, const std::vector<double>& xs, const std::vector<double>& ys) {
    if (xs.empty()) return 0.0;
    if (xq <= xs.front()) return ys.front();
    if (xq >= xs.back()) return ys.back();
    const auto it = std::upper_bound(xs.begin(), xs.end(), xq);
    const size_t j = static_cast<size_t>(it - xs.begin());
    const double t = (xq - xs[j - 1]) / (xs[j] - xs[j - 1]);
    return ys[j - 1] * (1.0 - t) + ys[j] * t;
}

// チャンクごとのピーク位置を直線フィット: シンボル i の GI 先頭 ≈ r0 + i*T*(1+delta)
void FitSymbolTiming(const FoldPeaks& fp, int T, double* r0, double* delta) {
    const size_t n = fp.pos.size();
    std::vector<double> pos(n);
    for (size_t i = 0; i < n; ++i) {
        double d = std::fmod(fp.pos[i] - fp.pos[0] + T / 2.0, static_cast<double>(T));
        if (d < 0) d += T;
        pos[i] = fp.pos[0] + d - T / 2.0;
    }
    if (n >= 2) {
        double mx = 0, my = 0;
        for (size_t i = 0; i < n; ++i) { mx += fp.idx[i]; my += pos[i]; }
        mx /= n;
        my /= n;
        double sxx = 0, sxy = 0;
        for (size_t i = 0; i < n; ++i) {
            sxx += (fp.idx[i] - mx) * (fp.idx[i] - mx);
            sxy += (fp.idx[i] - mx) * (pos[i] - my);
        }
        const double slope = sxy / sxx;
        *r0 = my - slope * mx;
        *delta = slope / T;
    } else {
        *r0 = pos[0];
        *delta = 0.0;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// CarrierSource
// ---------------------------------------------------------------------------
CarrierSource::CarrierSource(std::vector<cfloat> iq, double fs, Mode mode, Gi gi, int cfo_search)
    : mode_(mode) {
    const ModeParams& mp = ModeParamsOf(mode);
    N_ = mp.fft_size;
    Ng_ = GuardSamples(mode, gi);
    T_ = N_ + Ng_;
    margin_ = Ng_ / 8;
    x_ = ResampleToNominal(iq, fs);
    fft_ = std::make_unique<Fft>(N_);

    // GI 相関にだけ DC 除去したコピーを使う。復調本体の x から平均は引かない:
    // DC ビンは 4 シンボルごとに SP が乗り信号自体がコヒーレントな DC を持つ（ref/isdbt/sync.py 参照）
    FoldPeaks fp;
    {
        std::vector<cfloat> x_sync = x_;
        RemoveDc(&x_sync);
        fp = GiFoldChunks(x_sync, N_, Ng_);
    }
    double r0 = 0.0, delta = 0.0;
    FitSymbolTiming(fp, T_, &r0, &delta);
    ratio_ = std::abs(delta) > 2e-7 ? 1.0 + delta : 1.0;
    std::complex<double> vsum = 0.0;
    for (const auto& v : fp.val) vsum += v;
    f_frac_ = std::arg(vsum) / (2.0 * kPi) * kSampleRateHz / N_;

    // 補正後タイムラインでのシンボル 0 の GI 先頭（FFT 窓が収まる最初のシンボルに正規化）
    double r0c = r0 / ratio_;
    r0c = std::fmod(r0c + Ng_ - margin_, static_cast<double>(T_));
    if (r0c < 0) r0c += T_;
    r0c -= (Ng_ - margin_);
    const long long n_corrected = ratio_ != 1.0
        ? static_cast<long long>((static_cast<double>(x_.size()) - kFracTaps) / ratio_)
        : static_cast<long long>(x_.size());
    const long long r0r = static_cast<long long>(RoundHalfEven(r0c));
    n_sym_ = static_cast<int>(std::max(0LL, FloorDiv(n_corrected - r0r - Ng_ + margin_ - N_, T_) + 1));
    start0_ = r0r + Ng_ - margin_;

    info_.clock_ppm = delta * 1e6;
    info_.gi_start0 = static_cast<double>(start0_ - (Ng_ - margin_));
    info_.abs_symbol0 = n_sym_ ? static_cast<long long>(RoundHalfEven(
        static_cast<double>(start0_ - (Ng_ - margin_)) / T_)) : 0;
    info_.fft_size = N_;
    info_.n_sym = n_sym_;

    // 整数 CFO: 固定パイロット（TMCC/AC1/CP）の電力が最大になるビンずれ
    k_int_ = 0;
    if (n_sym_ > 0) {
        const int n_use = std::min(8, n_sym_);
        const long long m0 = start0_;
        const long long m1 = start0_ + static_cast<long long>(n_use - 1) * T_ + N_;
        std::vector<cfloat> xs = Samples(m0, m1);
        const Layout& lo = LayoutOf(mode);
        std::vector<int> bins;
        for (int k : lo.tmcc_abs) bins.push_back(CarrierToFftBin(mode, k));
        for (int k : lo.ac1_abs) bins.push_back(CarrierToFftBin(mode, k));
        bins.push_back(CarrierToFftBin(mode, mp.total_carriers - 1));
        // n_use シンボルの |FFT|² を合算してからシフト探索
        std::vector<double> pw(static_cast<size_t>(N_), 0.0);
        std::vector<cfloat> buf(static_cast<size_t>(N_));
        for (int i = 0; i < n_use; ++i) {
            const long long off = static_cast<long long>(i) * T_;
            std::copy(xs.begin() + off, xs.begin() + off + N_, buf.begin());
            fft_->Forward(buf.data());
            for (int k = 0; k < N_; ++k) pw[k] += std::norm(buf[k]);
        }
        ExcludeNarrowbandSpikes(&pw);
        double best = -1.0;
        for (int sh = -cfo_search; sh <= cfo_search; ++sh) {
            double acc = 0.0;
            for (int b : bins) {
                int kb = (b + sh) % N_;
                if (kb < 0) kb += N_;
                acc += pw[kb];
            }
            if (acc > best) { best = acc; k_int_ = sh; }
        }
    }
    info_.cfo_frac_hz = f_frac_;
    info_.cfo_int_bins = k_int_;
}

std::vector<cfloat> CarrierSource::Samples(long long m0, long long m1) const {
    std::vector<cfloat> xs;
    if (ratio_ != 1.0) {
        xs = ResampleFractional(x_, ratio_, m0, m1);
    } else {
        const long long n_in = static_cast<long long>(x_.size());
        xs.resize(static_cast<size_t>(std::max(0LL, m1 - m0)));
        for (long long m2 = m0; m2 < m1; ++m2) {
            const long long i = std::clamp(m2, 0LL, n_in - 1);
            xs[static_cast<size_t>(m2 - m0)] = x_[static_cast<size_t>(i)];
        }
    }
    if (f_frac_ != 0.0) {
        // 小数 CFO の打ち消し。回転子はサンプルごとに cos/sin を引かず、double の複素乗算で進める。
        // 4096 サンプルごとに正確な位相から取り直すので、ref/（サンプルごとに exp）との差は double の丸め程度
        constexpr size_t kReanchor = 4096;
        const double dphi = -2.0 * kPi * f_frac_ / kSampleRateHz;
        const std::complex<double> step = std::polar(1.0, dphi);
        std::complex<double> rot;
        for (size_t k = 0; k < xs.size(); ++k) {
            if (k % kReanchor == 0) rot = std::polar(1.0, dphi * static_cast<double>(m0 + static_cast<long long>(k)));
            xs[k] *= cfloat(static_cast<float>(rot.real()), static_cast<float>(rot.imag()));
            rot *= step;
        }
    }
    return xs;
}

// ---------------------------------------------------------------------------
// TrackedCarrierSource（ref/isdbt/sync.py の同名クラスの移植）
// ---------------------------------------------------------------------------
namespace {

// 移動平均（中央寄せ、幅 width、端は窓が縮む）
std::vector<double> BoxSmooth(const std::vector<double>& v, int width) {
    const int n = static_cast<int>(v.size());
    width = std::min(width, n);
    std::vector<double> out(v);
    for (int i = 0; i < n; ++i) {
        double num = 0.0;
        int cnt = 0;
        for (int k = i - width / 2; k <= i + (width - 1) / 2; ++k) {
            if (k < 0 || k >= n) continue;
            num += v[k];
            ++cnt;
        }
        out[i] = num / cnt;
    }
    return out;
}

}  // namespace

TrackedCarrierSource::TrackedCarrierSource(std::vector<cfloat> iq, double fs, Mode mode, Gi gi,
                                           int cfo_search, int chunk_syms, int smooth_chunks,
                                           double lock_threshold)
    : mode_(mode), cfo_search_(cfo_search) {
    const ModeParams& mp = ModeParamsOf(mode);
    N_ = mp.fft_size;
    Ng_ = GuardSamples(mode, gi);
    T_ = N_ + Ng_;
    margin_ = Ng_ / 8;
    x_ = ResampleToNominal(iq, fs);
    fft_ = std::make_unique<Fft>(N_);

    // GI 相関にだけ DC 除去したコピーを使う（理由は CarrierSource と同じ）
    FoldPeaks fp;
    {
        std::vector<cfloat> x_sync = x_;
        RemoveDc(&x_sync);
        fp = GiFoldChunks(x_sync, N_, Ng_, chunk_syms, /*with_power=*/true);
    }
    chunk_syms_ = fp.chunk;
    const size_t nc = fp.idx.size();
    chunk_idx_ = fp.idx;

    // ロック判定: |fold| / (chunk · Ng · チャンク電力)。選択理由は ref/isdbt/sync.py のコメント
    locked_.resize(nc);
    lock_metric_.resize(nc);
    for (size_t k = 0; k < nc; ++k) {
        lock_metric_[k] = std::abs(fp.val[k]) /
                          std::max(fp.chunk * static_cast<double>(Ng_) * fp.pwr[k], 1e-30);
        locked_[k] = lock_metric_[k] > lock_threshold;
    }

    n_sym_ = static_cast<int>(std::max(0LL, (static_cast<long long>(x_.size()) - N_ - T_) / T_));

    // タイミングのアンラップと CFO（チャンクごとの生値）
    std::vector<double> pos_u(nc), f(nc);
    for (size_t k = 0; k < nc; ++k) {
        if (k == 0) {
            pos_u[k] = fp.pos[k];
        } else {
            double d = std::fmod(fp.pos[k] - pos_u[k - 1] + T_ / 2.0, static_cast<double>(T_));
            if (d < 0) d += T_;
            pos_u[k] = pos_u[k - 1] + d - T_ / 2.0;
        }
        f[k] = std::arg(fp.val[k]) / (2.0 * kPi) * kSampleRateHz / N_;
    }

    // アンカーの平滑化・補間は「ロック区間内で完結」させる（理由は ref/isdbt/sync.py のコメント）
    int run0 = -1;
    auto add_run = [&](int k0, int k1) {        // チャンク番号 [k0, k1)
        const int i0 = k0 * chunk_syms_;
        const int i1 = std::min(k1 * chunk_syms_, n_sym_);
        if (i1 - i0 >= 2) segments_.emplace_back(i0, i1);
        std::vector<double> pv(pos_u.begin() + k0, pos_u.begin() + k1);
        std::vector<double> fv(f.begin() + k0, f.begin() + k1);
        pv = BoxSmooth(pv, smooth_chunks);
        fv = BoxSmooth(fv, smooth_chunks);
        interp_x_.push_back(i0);
        interp_p_.push_back(pv.front());
        interp_f_.push_back(fv.front());
        for (int k = k0; k < k1; ++k) {
            interp_x_.push_back(chunk_idx_[k]);
            interp_p_.push_back(pv[k - k0]);
            interp_f_.push_back(fv[k - k0]);
        }
        interp_x_.push_back(i1);
        interp_p_.push_back(pv.back());
        interp_f_.push_back(fv.back());
        if (k1 - k0 >= 2) {
            slope_x_.push_back(i0);
            slope_v_.push_back((pv[1] - pv[0]) / ((chunk_idx_[k0 + 1] - chunk_idx_[k0]) * T_));
            for (int k = k0; k + 1 < k1; ++k) {
                slope_x_.push_back((chunk_idx_[k] + chunk_idx_[k + 1]) / 2.0);
                slope_v_.push_back((pv[k + 1 - k0] - pv[k - k0]) /
                                   ((chunk_idx_[k + 1] - chunk_idx_[k]) * T_));
            }
            slope_x_.push_back(i1);
            slope_v_.push_back(slope_v_.back());
        } else {
            slope_x_.push_back(i0);
            slope_v_.push_back(0.0);
            slope_x_.push_back(i1);
            slope_v_.push_back(0.0);
        }
    };
    for (size_t k = 0; k < nc; ++k) {
        if (locked_[k] && run0 < 0) run0 = static_cast<int>(k);
        if (!locked_[k] && run0 >= 0) {
            add_run(run0, static_cast<int>(k));
            run0 = -1;
        }
    }
    if (run0 >= 0) add_run(run0, static_cast<int>(nc));

    // 時変 CFO の累積位相（アンカーのサンプル時刻による台形積分）。
    // 「時変 f × 絶対時刻」で回すと位相誤差の微分が f'·t で録音後半ほど増大し
    // 見かけの CFO ずれ → ICI で等化が崩壊する（ref/isdbt/sync.py と同修正、実放送で確認）。
    interp_phi_.assign(interp_x_.size(), 0.0);
    for (size_t k = 1; k < interp_x_.size(); ++k) {
        const double t0 = interp_x_[k - 1] * T_ + interp_p_[k - 1];
        const double t1 = interp_x_[k] * T_ + interp_p_[k];
        interp_phi_[k] = interp_phi_[k - 1] +
                         (interp_f_[k] + interp_f_[k - 1]) / 2.0 * (t1 - t0) * (2.0 * kPi / kSampleRateHz);
    }
}

double TrackedCarrierSource::POf(double i) const { return Interp1(i, interp_x_, interp_p_); }
double TrackedCarrierSource::FOf(double i) const { return Interp1(i, interp_x_, interp_f_); }
double TrackedCarrierSource::DeltaOf(double i) const { return Interp1(i, slope_x_, slope_v_); }

Mat<cfloat> TrackedCarrierSource::FftWindow(int i0, int i1, int k_int, bool raw_bins) {
    const ModeParams& mp = ModeParamsOf(mode_);
    const int n = i1 - i0;
    Mat<cfloat> out(static_cast<size_t>(n), raw_bins ? N_ : mp.total_carriers);
    std::vector<double> t(static_cast<size_t>(N_));
    std::vector<cfloat> buf(static_cast<size_t>(N_));
    std::vector<int> bins;
    if (!raw_bins) {
        bins.resize(mp.total_carriers);
        for (int k = 0; k < mp.total_carriers; ++k) bins[k] = CarrierToFftBin(mode_, k);
    }
    for (int i = i0; i < i1; ++i) {
        const double w0 = static_cast<double>(i) * T_ + POf(i) + (Ng_ - margin_);
        const double r = 1.0 + DeltaOf(i);
        for (int k = 0; k < N_; ++k) t[static_cast<size_t>(k)] = w0 + k * r;
        ResampleAt(x_, t.data(), t.size(), buf.data());
        // 周波数補正。整数 CFO は定数なので f·t でよいが、時変の小数 CFO は
        // 累積位相 ∫f dt を基準に窓内だけ f(i) の線形位相を掛ける（コンストラクタ末尾を参照）。
        const double f_const = static_cast<double>(k_int) * kSampleRateHz / N_;
        const double f_var = FOf(i);
        const double phi0 = Interp1(i, interp_x_, interp_phi_);
        for (int k = 0; k < N_; ++k) {
            const double tk = t[static_cast<size_t>(k)];
            const double ph = -(2.0 * kPi / kSampleRateHz) * (f_const * tk + f_var * (tk - w0)) - phi0;
            buf[static_cast<size_t>(k)] *= cfloat(static_cast<float>(std::cos(ph)),
                                                  static_cast<float>(std::sin(ph)));
        }
        fft_->Forward(buf.data());
        cfloat* dst = out[static_cast<size_t>(i - i0)];
        if (raw_bins) {
            std::copy(buf.begin(), buf.end(), dst);
        } else {
            for (int k = 0; k < mp.total_carriers; ++k) dst[k] = buf[static_cast<size_t>(bins[k])];
        }
    }
    return out;
}

int TrackedCarrierSource::KIntFor(int seg_start) {
    auto it = k_int_.find(seg_start);
    if (it != k_int_.end()) return it->second;
    // 区間の端は瞬断の残り（死んだシンボル）を含みうるが、ロック区間の中央は
    // 構成上必ず生きているので、区間中央の 8 シンボルから推定する
    std::pair<int, int> seg{seg_start, n_sym_};
    for (const auto& s : segments_) {
        if (s.first <= seg_start && seg_start < s.second) { seg = s; break; }
    }
    int start = std::max(seg_start, (seg.first + seg.second) / 2 - 4);
    start = std::min(start, std::max(0, n_sym_ - 8));
    const int n_use = std::min(8, n_sym_ - start);
    Mat<cfloat> X = FftWindow(start, start + n_use, 0, /*raw_bins=*/true);
    const Layout& lo = LayoutOf(mode_);
    std::vector<int> bins;
    for (int k : lo.tmcc_abs) bins.push_back(CarrierToFftBin(mode_, k));
    for (int k : lo.ac1_abs) bins.push_back(CarrierToFftBin(mode_, k));
    bins.push_back(CarrierToFftBin(mode_, ModeParamsOf(mode_).total_carriers - 1));
    std::vector<double> pw(static_cast<size_t>(N_), 0.0);
    for (size_t r = 0; r < X.rows; ++r) {
        for (int k = 0; k < N_; ++k) pw[static_cast<size_t>(k)] += std::norm(X[r][k]);
    }
    ExcludeNarrowbandSpikes(&pw);
    int best_sh = 0;
    double best = -1.0;
    for (int sh = -cfo_search_; sh <= cfo_search_; ++sh) {
        double acc = 0.0;
        for (int b : bins) {
            int kb = (b + sh) % N_;
            if (kb < 0) kb += N_;
            acc += pw[static_cast<size_t>(kb)];
        }
        if (acc > best) { best = acc; best_sh = sh; }
    }
    k_int_[seg_start] = best_sh;
    return best_sh;
}

Mat<cfloat> TrackedCarrierSource::Symbols(int i0, int i1) {
    i0 = std::max(0, i0);
    i1 = std::min(i1, n_sym_);
    if (i1 <= i0) return Mat<cfloat>(0, ModeParamsOf(mode_).total_carriers);
    int seg0 = i0;
    for (const auto& [a, b] : segments_) {
        if (a <= i0 && i0 < b) { seg0 = a; break; }
    }
    return FftWindow(i0, i1, KIntFor(seg0), /*raw_bins=*/false);
}

Mat<cfloat> CarrierSource::Symbols(int i0, int i1) {
    const ModeParams& mp = ModeParamsOf(mode_);
    i0 = std::max(0, i0);
    i1 = std::min(i1, n_sym_);
    if (i1 <= i0) return Mat<cfloat>(0, mp.total_carriers);

    static const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    const long long base = start0_ + static_cast<long long>(i0) * T_;
    std::vector<cfloat> xs = Samples(base, start0_ + static_cast<long long>(i1 - 1) * T_ + N_);
    const auto t1 = std::chrono::steady_clock::now();

    std::vector<int> bins(mp.total_carriers);
    for (int k = 0; k < mp.total_carriers; ++k) {
        int b = (CarrierToFftBin(mode_, k) + k_int_) % N_;
        if (b < 0) b += N_;
        bins[k] = b;
    }
    Mat<cfloat> Y(static_cast<size_t>(i1 - i0), mp.total_carriers);
    std::vector<cfloat> buf(static_cast<size_t>(N_));
    for (int i = i0; i < i1; ++i) {
        const long long st = start0_ + static_cast<long long>(i) * T_;   // 絶対サンプル番号
        const long long off = st - base;
        std::copy(xs.begin() + off, xs.begin() + off + N_, buf.begin());
        fft_->Forward(buf.data());
        // 整数 CFO のビンずれに伴う、窓開始位置に応じた位相を打ち消す
        const double ph = -2.0 * kPi * k_int_ * static_cast<double>(st) / N_;
        const cfloat rot(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
        cfloat* dst = Y[static_cast<size_t>(i - i0)];
        for (int k = 0; k < mp.total_carriers; ++k) dst[k] = buf[bins[k]] * rot;
    }
    if (profile) {
        std::fprintf(stderr, "[profile] symbols %d rows: samples(resample+cfo) %.3f fft %.3f (ratio %.6f)\n", i1 - i0,
                     std::chrono::duration<double>(t1 - t0).count(),
                     std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count(), ratio_);
    }
    return Y;
}

}  // namespace isdbt
