// IsdbcDemod の実装。アルゴリズムの説明は demod.h 冒頭と docs/isdbc-spec-notes.md 4 章。
#include "demod.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef __AVX2__
#include <immintrin.h>
#endif

#include "../isdbt/fft.h"

namespace isdbc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr size_t kAcqSamples = 1 << 19;             // 取得バッファ（7.1 MSPS で 74 ms）
constexpr int kOmBlock = 4096;                      // O&M の更新間隔 [シンボル]
constexpr int kOmSpan = 1024;                       // O&M の積算長 [シンボル]（追従中）
constexpr int kFineCfoSymbols = 1 << 16;
constexpr int kAgcWarmup = 2048;                    // 追従開始直後に AGC を初期化するシンボル数
constexpr int kLsDecim = 32;                        // LS 蓄積のデシメーション（R の蓄積は 25×25/2 複素積和）
constexpr long long kLsSolveEvery = 2048;           // 蓄積数（デシメーション後）ごとにタップを解く（≈ 65k シンボル）
constexpr int kSyncSearchPackets = 32;
constexpr double kSyncAcceptRatio = 0.75;
constexpr int kSyncLossRun = 5;
constexpr long long kSyncTimeoutSymbols = 2'000'000;   // 約 0.38 s でビット同期が取れなければ反転/再取得
constexpr int kInitialPhaseBlocks = 8;              // 最初は 4 乗法で位相を初期化
constexpr double kTimingGainPos = 0.5, kTimingGainRate = 0.25;
constexpr double kPhaseRateBeta = 0.1;
constexpr int kStatusEveryBlocks = 16;
constexpr long long kLowMerWindows = 60;            // MER < 10 dB が約 1 s 続いたら再取得

inline double Wrap(double a) {
    while (a > kPi) a -= 2 * kPi;
    while (a <= -kPi) a += 2 * kPi;
    return a;
}

// エルミート正定値 A（n×n、行優先）の Cholesky で A w = b を解く。失敗なら false
bool SolveHermitian(std::vector<std::complex<double>> A, std::vector<std::complex<double>> b, int n,
                    std::vector<std::complex<double>>* w) {
    for (int j = 0; j < n; ++j) {
        double d = A[j * n + j].real();
        for (int k = 0; k < j; ++k) d -= std::norm(A[j * n + k]);
        if (d <= 0.0) return false;
        const double s = std::sqrt(d);
        A[j * n + j] = s;
        for (int i = j + 1; i < n; ++i) {
            std::complex<double> v = A[i * n + j];
            for (int k = 0; k < j; ++k) v -= A[i * n + k] * std::conj(A[j * n + k]);
            A[i * n + j] = v / s;
        }
    }
    // L y = b
    for (int i = 0; i < n; ++i) {
        std::complex<double> v = b[i];
        for (int k = 0; k < i; ++k) v -= A[i * n + k] * b[k];
        b[i] = v / A[i * n + i].real();
    }
    // L^H w = y
    for (int i = n - 1; i >= 0; --i) {
        std::complex<double> v = b[i];
        for (int k = i + 1; k < n; ++k) v -= std::conj(A[k * n + i]) * b[k];
        b[i] = v / A[i * n + i].real();
    }
    *w = b;
    return true;
}

double RrcValue(double t, double a) {
    if (std::fabs(t) < 1e-9) return 1 - a + 4 * a / kPi;
    if (a > 0 && std::fabs(std::fabs(t) - 1 / (4 * a)) < 1e-9) {
        return (a / std::sqrt(2.0)) * ((1 + 2 / kPi) * std::sin(kPi / (4 * a)) +
                                      (1 - 2 / kPi) * std::cos(kPi / (4 * a)));
    }
    return (std::sin(kPi * t * (1 - a)) + 4 * a * t * std::cos(kPi * t * (1 + a))) /
           (kPi * t * (1 - (4 * a * t) * (4 * a * t)));
}

}  // namespace

std::vector<float> RrcTaps(int sps, int span_symbols, double rolloff) {
    const int n = span_symbols * sps;
    std::vector<float> h(static_cast<size_t>(2 * n + 1));
    double e = 0.0;
    for (int i = -n; i <= n; ++i) {
        const double v = RrcValue(static_cast<double>(i) / sps, rolloff);
        h[static_cast<size_t>(i + n)] = static_cast<float>(v);
        e += v * v;
    }
    const float g = static_cast<float>(1.0 / std::sqrt(e));
    for (float& v : h) v *= g;
    return h;
}

// ---------------------------------------------------------------------------
IsdbcDemod::IsdbcDemod(const IsdbcConfig& cfg, PacketFn on_packets)
    : cfg_(cfg), on_packets_(std::move(on_packets)) {
    if (cfg_.eq_taps % 2 == 0) ++cfg_.eq_taps;
    // 多相 RRC カーネル: 位相 q/kPhases の遅れで入力レート上の RRC を標本化。DC ゲインを揃える
    const double sym_per_sample = cfg_.symbol_rate / cfg_.fs;
    // 係数は実部・虚部用に 2 つずつ複製して置く（複素入力を float 列として一本のベクトル積和で
    // 処理するため。Interp 参照）
    kernel_.assign(static_cast<size_t>(kPhases) * 2 * kTaps, 0.0f);
    for (int q = 0; q < kPhases; ++q) {
        const double mu = static_cast<double>(q) / kPhases;
        double sum = 0.0;
        std::vector<double> h(kTaps);
        for (int i = 0; i < kTaps; ++i) {
            const double t = (i - (kTaps / 2 - 1) - mu) * sym_per_sample;
            h[static_cast<size_t>(i)] = RrcValue(t, kRolloff);
            sum += h[static_cast<size_t>(i)];
        }
        float* row = &kernel_[static_cast<size_t>(q) * 2 * kTaps];
        for (int i = 0; i < kTaps; ++i) row[2 * i] = row[2 * i + 1] = static_cast<float>(h[static_cast<size_t>(i)] / sum);
    }
    fft4096_ = new isdbt::Fft(4096);
    fft64k_ = new isdbt::Fft(kFineCfoSymbols);
    tsmf_.SetSelect(cfg_.select);
    Reset();
}

IsdbcDemod::~IsdbcDemod() {
    delete fft4096_;
    delete fft64k_;
}

void IsdbcDemod::Reset() {
    // Feed() と同じスレッドから呼ぶこと（LiveIsdbc は reset 要求をワーカースレッドで消化する）
    x_.clear();
    x_abs0_ = 0;
    in_total_ = 0;
    nco_freq_hz_ = 0.0;
    nco_phase_ = 0.0;
    level_acc_ = 0.0;
    level_n_ = 0;
    sym_period_ = cfg_.fs / cfg_.symbol_rate;
    next_pos_ = 0.0;
    inverted_ = false;
    inversion_tried_ = false;
    out_.clear();
    {
        std::lock_guard<std::mutex> lk(mu_);
        const long long reacq = st_.reacquisitions;
        st_ = IsdbcStatus();
        st_.reacquisitions = reacq;
    }
    StartTrack();                 // 追従側の内部状態の初期化を共用
    state_ = State::kAcquire;
    std::lock_guard<std::mutex> lk(mu_);
    st_.acquired = false;
}

IsdbcStatus IsdbcDemod::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    return st_;
}

// ---------------------------------------------------------------------------
// 入力
// ---------------------------------------------------------------------------
void IsdbcDemod::Rotate(const cfloat* x, size_t n) {
    const size_t base = x_.size();
    x_.resize(base + n);
    const double dphi = -2 * kPi * nco_freq_hz_ / cfg_.fs;
    std::complex<double> rot = std::polar(1.0, nco_phase_);
    const std::complex<double> step = std::polar(1.0, dphi);
    double lacc = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const std::complex<double> v(x[i].real(), x[i].imag());
        x_[base + i] = static_cast<cfloat>(v * rot);
        rot *= step;
        if ((i & 255) == 0) {
            rot /= std::abs(rot);
            lacc += std::norm(v);
        }
    }
    nco_phase_ = Wrap(nco_phase_ + dphi * static_cast<double>(n));
    level_acc_ += lacc;
    level_n_ += static_cast<long long>((n + 255) / 256);
    in_total_ += static_cast<long long>(n);
}

void IsdbcDemod::Feed(const cfloat* x, size_t n) {
    Rotate(x, n);
    Process();
    FlushOutput();
}

void IsdbcDemod::Process() {
    for (;;) {
        if (state_ == State::kAcquire) {
            if (!Acquire()) return;
            continue;
        }
        if (!TrackStep()) break;
    }
    // 消費済みの入力を捨てる（カーネル幅 + 余裕を残す）
    const long long keep_from = static_cast<long long>(std::floor(next_pos_)) - x_abs0_ - 64;
    if (keep_from > (1 << 16)) {
        x_.erase(x_.begin(), x_.begin() + keep_from);
        x_abs0_ += keep_from;
    }
}

cfloat IsdbcDemod::Interp(double t) const {
    const double idx = t - static_cast<double>(x_abs0_);
    long long i0 = static_cast<long long>(std::floor(idx));
    int q = static_cast<int>((idx - static_cast<double>(i0)) * kPhases + 0.5);
    if (q >= kPhases) {
        q = 0;
        ++i0;
    }
    const long long base = i0 - (kTaps / 2 - 1);
    const float* h = &kernel_[static_cast<size_t>(q) * 2 * kTaps];
    // 複素列を float 列（re, im, re, im, ...）として 8 レーンで積和する（偶数レーンが実部、奇数レーンが虚部）
    const float* xp = reinterpret_cast<const float*>(&x_[static_cast<size_t>(base)]);
    return DotInterleaved(xp, h, 2 * kTaps);
}

// 交互配置（re, im, ...）の列同士の 8 レーン積和。n は 8 の倍数。戻り値 = (偶数レーン和, 奇数レーン和)
cfloat IsdbcDemod::DotInterleaved(const float* x, const float* h, int n) {
#ifdef __AVX2__
    __m256 acc = _mm256_setzero_ps();
    for (int i = 0; i < n; i += 8) {
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(x + i), _mm256_loadu_ps(h + i), acc);
    }
    alignas(32) float a[8];
    _mm256_store_ps(a, acc);
    return {a[0] + a[2] + a[4] + a[6], a[1] + a[3] + a[5] + a[7]};
#else
    float a[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < n; i += 8) {
        for (int k = 0; k < 8; ++k) a[k] += x[i + k] * h[i + k];
    }
    return {a[0] + a[2] + a[4] + a[6], a[1] + a[3] + a[5] + a[7]};
#endif
}

// Interp(t) が計算できる範囲か
static inline bool AvailAt(double t, long long abs0, size_t n, int taps) {
    const double idx = t - static_cast<double>(abs0);
    return idx >= taps / 2 + 1 && idx + taps / 2 + 2 < static_cast<double>(n);
}

double IsdbcDemod::OerderMeyr(double t_base, int n_symbols) const {
    // ε̂ = −(1/2π) arg Σ_m |y(t_base + m T/4)|² e^{−j2πm/4}
    double re = 0.0, im = 0.0;
    const double q = sym_period_ / 4.0;
    for (int m = 0; m < 4 * n_symbols; ++m) {
        const float p = std::norm(Interp(t_base + m * q));
        switch (m & 3) {
            case 0: re += p; break;
            case 1: im -= p; break;
            case 2: re -= p; break;
            default: im += p; break;
        }
    }
    return -std::atan2(im, re) / (2 * kPi);
}

// ---------------------------------------------------------------------------
// 取得
// ---------------------------------------------------------------------------
double IsdbcDemod::CoarseCfoHz(const std::vector<cfloat>& buf) const {
    constexpr int N = 4096;
    const size_t n_seg = buf.size() / N;
    if (n_seg < 8) return std::nan("");
    const size_t use = std::min<size_t>(n_seg, 128);
    std::vector<double> P(N, 0.0);
    std::vector<cfloat> seg(N);
    std::vector<float> win(N);
    for (int i = 0; i < N; ++i) win[i] = static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / N));
    for (size_t s = 0; s < use; ++s) {
        const size_t off = (use == 1) ? 0 : (s * (n_seg - 1) / (use - 1)) * N;
        for (int i = 0; i < N; ++i) seg[i] = buf[off + i] * win[i];
        fft4096_->Forward(seg.data());
        for (int i = 0; i < N; ++i) P[i] += std::norm(seg[i]);
    }
    // 周波数昇順に並べ替え（添字 k ↔ 周波数 (k − N/2) fs / N）
    std::vector<double> Ps(N);
    for (int k = 0; k < N; ++k) Ps[k] = P[(k + N / 2) % N];
    const double df = cfg_.fs / N;
    const int kw = std::max(1, static_cast<int>(200e3 / df));
    std::vector<double> PdB(N);
    for (int k = 0; k < N; ++k) {
        double acc = 0.0;
        int cnt = 0;
        for (int j = k - kw / 2; j <= k + kw / 2; ++j) {
            if (j >= 0 && j < N) { acc += Ps[j]; ++cnt; }
        }
        PdB[k] = 10 * std::log10(acc / cnt + 1e-30);
    }
    // 中央 ±0.25 Rs の中央値
    std::vector<double> mid;
    for (int k = 0; k < N; ++k) {
        if (std::fabs((k - N / 2) * df) < 0.25 * cfg_.symbol_rate) mid.push_back(PdB[k]);
    }
    std::nth_element(mid.begin(), mid.begin() + mid.size() / 2, mid.end());
    const double thr = mid[mid.size() / 2] - 3.0;
    auto edge = [&](int step) -> double {
        int i = N / 2;
        while (i > 0 && i < N - 1 && PdB[i] > thr) i += step;
        const int j = i - step;
        if (PdB[j] == PdB[i]) return (i - N / 2) * df;
        const double t = (PdB[j] - thr) / (PdB[j] - PdB[i]);
        return (j - N / 2) * df + t * step * df;
    };
    const double hi = edge(+1), lo = edge(-1);
    const double bw = hi - lo;
    if (bw < 0.6 * cfg_.symbol_rate || bw > 1.4 * cfg_.symbol_rate) return std::nan("");
    return 0.5 * (hi + lo);
}

double IsdbcDemod::FineCfoCycles(const std::vector<cfloat>& z) const {
    const int N = kFineCfoSymbols;
    std::vector<cfloat> w(N);
    for (int i = 0; i < N; ++i) {
        const cfloat v = z[i];
        const cfloat v2 = v * v;
        w[i] = v2 * v2 * static_cast<float>(0.5 - 0.5 * std::cos(2 * kPi * i / N));
    }
    fft64k_->Forward(w.data());
    int k = 0;
    double best = -1.0;
    std::vector<double> mag(N);
    for (int i = 0; i < N; ++i) {
        mag[i] = std::abs(w[i]);
        if (mag[i] > best) { best = mag[i]; k = i; }
    }
    const double a = mag[(k - 1 + N) % N], b = mag[k], c = mag[(k + 1) % N];
    const double den = a - 2 * b + c;
    const double delta = den != 0.0 ? 0.5 * (a - c) / den : 0.0;
    double f4 = (k + delta) / N;
    if (f4 > 0.5) f4 -= 1.0;
    return f4 / 4.0;
}

bool IsdbcDemod::Acquire() {
    if (x_.size() < kAcqSamples) return false;
    // 1. 粗 CFO（帯域端の中点）
    const double coarse = CoarseCfoHz(x_);
    if (std::isnan(coarse)) {
        // 信号なし: バッファを捨てて待つ
        x_.erase(x_.begin(), x_.end() - 64);
        x_abs0_ = in_total_ - 64;
        return false;
    }
    auto apply_df = [&](double df) {
        // バッファ全体に −df の回転を追加し、NCO の周波数・位相を更新する
        std::complex<double> rot = 1.0;
        const std::complex<double> step = std::polar(1.0, -2 * kPi * df / cfg_.fs);
        for (size_t i = 0; i < x_.size(); ++i) {
            x_[i] = static_cast<cfloat>(std::complex<double>(x_[i]) * rot);
            rot *= step;
            if ((i & 255) == 255) rot /= std::abs(rot);
        }
        nco_phase_ = Wrap(nco_phase_ - 2 * kPi * df / cfg_.fs * static_cast<double>(x_.size()));
        nco_freq_hz_ += df;
    };
    if (std::fabs(coarse) > 1.0) apply_df(coarse);

    // 2. タイミング: 固定格子（公称シンボル周期）でブロックごとに O&M → 直線フィット
    const double T0 = cfg_.fs / cfg_.symbol_rate;
    const double t_first = static_cast<double>(x_abs0_) + kTaps;
    const double t_last = static_cast<double>(x_abs0_) + static_cast<double>(x_.size()) - kTaps;
    const int nb = static_cast<int>((t_last - t_first) / (kOmBlock * T0));
    if (nb < 4) return false;
    std::vector<double> eps(nb);
    double prev = 0.0;
    for (int b = 0; b < nb; ++b) {
        double e = OerderMeyr(t_first + b * kOmBlock * T0, kOmBlock);
        if (b > 0) {                                   // 1 シンボル周期で unwrap
            while (e - prev > 0.5) e -= 1.0;
            while (e - prev < -0.5) e += 1.0;
        }
        eps[b] = e;
        prev = e;
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (int b = 0; b < nb; ++b) {
        const double xk = static_cast<double>(b) * kOmBlock;     // ブロック先頭のシンボル番号
        sx += xk; sy += eps[b]; sxx += xk * xk; sxy += xk * eps[b];
    }
    const double den = nb * sxx - sx * sx;
    const double slope = den != 0.0 ? (nb * sxy - sx * sy) / den : 0.0;      // シンボルあたりの ε 変化
    const double a0 = (sy - slope * sx) / nb;
    sym_period_ = T0 * (1.0 + slope);
    next_pos_ = t_first + a0 * T0;
    while (next_pos_ < t_first) next_pos_ += sym_period_;

    // 3. 微 CFO: シンボルを抜き出して z^4 の FFT
    std::vector<cfloat> z;
    z.reserve(kFineCfoSymbols);
    for (double t = next_pos_; static_cast<int>(z.size()) < kFineCfoSymbols &&
                               AvailAt(t, x_abs0_, x_.size(), kTaps); t += sym_period_) {
        z.push_back(Interp(t));
    }
    if (static_cast<int>(z.size()) < kFineCfoSymbols) return false;
    const double f4 = FineCfoCycles(z);
    const double rs_actual = cfg_.fs / sym_period_;
    if (std::fabs(f4) > 1e-7) apply_df(f4 * rs_actual);

    StartTrack();
    return true;
}

void IsdbcDemod::StartTrack() {
    state_ = State::kTrack;
    sym_count_ = 0;
    track_symbols_ = 0;
    agc_pow_ = 0.0;
    eq_len_ = ((2 * cfg_.eq_taps + 7) / 8) * 8;                // 8 の倍数に切り上げ（末尾はゼロ係数）
    zbuf_.assign(static_cast<size_t>(4 * cfg_.eq_taps + eq_len_ + 16), 0.0f);
    c1_.assign(static_cast<size_t>(eq_len_), 0.0f);
    c2_.assign(static_cast<size_t>(eq_len_), 0.0f);
    zpos_ = 0;
    w_.assign(static_cast<size_t>(cfg_.eq_taps), 0.0);
    w_[static_cast<size_t>(cfg_.eq_taps / 2)] = 1.0;
    UpdateTapArrays();
    solves_ = 0;
    R_.assign(static_cast<size_t>(cfg_.eq_taps) * cfg_.eq_taps, 0.0);
    p_.assign(static_cast<size_t>(cfg_.eq_taps), 0.0);
    ls_count_ = 0;
    ls_decim_ = 0;
    ublk_.clear();
    zblk_.clear();
    zblk_idx_.clear();
    blk_n_ = 0;
    blocks_done_ = 0;
    theta_ = theta_rate_ = 0.0;
    mer_err_acc_ = mer_ref_acc_ = 0.0;
    mer_n_ = 0;
    mer_db_ = 0.0;
    low_mer_blocks_ = 0;
    diff_.Reset();
    bitbuf_.clear();
    aligned_ = false;
    bitacc_ = 0;
    bitacc_n_ = 0;
    tspbuf_.clear();
    bad_sync_run_ = 0;
    frame_.Reset();
    tsmf_.Reset();
    std::lock_guard<std::mutex> lk(mu_);
    st_.acquired = true;
    st_.byte_sync = false;
    st_.locked = false;
    st_.mer_db = 0.0;
    st_.inverted = inverted_;
    st_.cfo_hz = nco_freq_hz_;
    st_.clock_ppm = (sym_period_ * cfg_.symbol_rate / cfg_.fs - 1.0) * 1e6;   // channel.clock_ppm と同じ向き
}

void IsdbcDemod::SetInverted(bool inv) {
    inverted_ = inv;
    StartTrack();
}

// ---------------------------------------------------------------------------
// 追従
// ---------------------------------------------------------------------------
bool IsdbcDemod::TrackStep() {
    if (sym_count_ % kOmBlock == 0) {
        if (!AvailAt(next_pos_ + (kOmSpan + 1) * sym_period_, x_abs0_, x_.size(), kTaps)) return false;
        TimingUpdate();
    }
    if (!AvailAt(next_pos_, x_abs0_, x_.size(), kTaps)) return false;
    const cfloat z = Interp(next_pos_);
    next_pos_ += sym_period_;
    ++sym_count_;
    ++track_symbols_;
    OnSymbol(z);
    return true;
}

void IsdbcDemod::TimingUpdate() {
    const double eps = OerderMeyr(next_pos_, kOmSpan);      // 真のシンボル点の格子からのずれ [シンボル]
    next_pos_ += kTimingGainPos * eps * sym_period_;
    sym_period_ *= 1.0 + kTimingGainRate * eps / kOmBlock;
}

void IsdbcDemod::OnSymbol(cfloat z) {
    if (inverted_) z = std::conj(z);
    // AGC（立ち上がりは単純平均、以後は EWMA）
    const double p = std::norm(z);
    if (track_symbols_ <= kAgcWarmup) {
        agc_pow_ += (p - agc_pow_) / static_cast<double>(track_symbols_);
        if (track_symbols_ < kAgcWarmup) return;
    } else {
        agc_pow_ += 1e-4 * (p - agc_pow_);
    }
    z *= static_cast<float>(std::sqrt(kQam64Rms * kQam64Rms / std::max(agc_pow_, 1e-12)));
    // T 間隔 FFE: y = Σ_m w[m] z_{k−m}（出力は L シンボル遅れの位置に対応）。
    // 履歴は 2 重長の float 列（re, im, ...）に 2 か所書いて、直近 N シンボルが常に連続になるようにし、
    // 実部用・虚部用の係数列（UpdateTapArrays）との 2 本の積和で求める
    const int N = cfg_.eq_taps;
    zbuf_[2 * static_cast<size_t>(zpos_)] = z.real();
    zbuf_[2 * static_cast<size_t>(zpos_) + 1] = z.imag();
    zbuf_[2 * static_cast<size_t>(zpos_ + N)] = z.real();
    zbuf_[2 * static_cast<size_t>(zpos_ + N) + 1] = z.imag();
    const float* win = &zbuf_[2 * static_cast<size_t>(zpos_ + 1)];     // 古い順に N シンボル（最後が今）
    const cfloat d1 = DotInterleaved(win, c1_.data(), eq_len_);        // (Σ wr·zr, Σ −wi·zi)
    const cfloat d2 = DotInterleaved(win, c2_.data(), eq_len_);        // (Σ wi·zr, Σ wr·zi)
    const float yr = d1.real() + d1.imag();
    const float yi = d2.real() + d2.imag();
    // LS 用に等化器入力ベクトルをデシメーションして保存（R は即時蓄積、p は位相確定後）
    if (++ls_decim_ >= kLsDecim) {
        ls_decim_ = 0;
        zblk_idx_.push_back(blk_n_);              // このベクトルに対応する等化出力のブロック内位置
        const size_t base = zblk_.size();
        zblk_.resize(base + static_cast<size_t>(N));
        for (int m = 0; m < N; ++m) {              // zblk_[m] = z_{k−m}（新しい順）
            const float* s = win + 2 * (N - 1 - m);
            zblk_[base + static_cast<size_t>(m)] = cfloat(s[0], s[1]);
        }
        for (int a = 0; a < N; ++a) {
            const std::complex<double> ca = std::conj(std::complex<double>(zblk_[base + a]));
            for (int b = a; b < N; ++b) {          // エルミートなので上三角だけ
                R_[static_cast<size_t>(a) * N + b] += ca * std::complex<double>(zblk_[base + b]);
            }
        }
    }
    zpos_ = (zpos_ + 1 == N) ? 0 : zpos_ + 1;
    ublk_.push_back(cfloat(yr, yi));
    if (++blk_n_ >= kPhaseBlock) FinishPhaseBlock();
}

// 等化器タップ（新しい順 w[m]）→ 履歴窓（古い順）に合わせた実部用・虚部用の係数列。
// y_re = Σ (wr·zr − wi·zi)、y_im = Σ (wr·zi + wi·zr) を、交互配置の履歴との単純な積和にする
void IsdbcDemod::UpdateTapArrays() {
    const int N = cfg_.eq_taps;
    std::fill(c1_.begin(), c1_.end(), 0.0f);
    std::fill(c2_.begin(), c2_.end(), 0.0f);
    for (int i = 0; i < N; ++i) {
        const std::complex<double> w = w_[static_cast<size_t>(N - 1 - i)];
        c1_[2 * static_cast<size_t>(i)] = static_cast<float>(w.real());
        c1_[2 * static_cast<size_t>(i) + 1] = static_cast<float>(-w.imag());
        c2_[2 * static_cast<size_t>(i)] = static_cast<float>(w.imag());
        c2_[2 * static_cast<size_t>(i) + 1] = static_cast<float>(w.real());
    }
}

void IsdbcDemod::FinishPhaseBlock() {
    const cfloat* C = Constellation();
    double theta = Wrap(theta_ + theta_rate_);
    if (blocks_done_ < kInitialPhaseBlocks) {
        // 4 乗法で初期位相（64QAM は E[z^4] < 0 なので −Σu^4）
        std::complex<double> acc = 0.0;
        const std::complex<double> r = std::polar(1.0, -theta);
        for (const cfloat& u : ublk_) {
            const std::complex<double> v = std::complex<double>(u) * r;
            const std::complex<double> v2 = v * v;
            acc -= v2 * v2;
        }
        theta = Wrap(theta + std::arg(acc) / 4.0);
    }
    // 判定指向の残差位相（2 回繰り返して収束させる）
    for (int it = 0; it < 2; ++it) {
        std::complex<double> acc = 0.0;
        const std::complex<float> r = static_cast<cfloat>(std::polar(1.0, -theta));
        for (const cfloat& u : ublk_) {
            const cfloat v = u * r;
            acc += std::complex<double>(v * std::conj(C[DemapHard(v)]));
        }
        theta = Wrap(theta + std::arg(acc));
    }
    const double advance = Wrap(theta - theta_);
    theta_rate_ += kPhaseRateBeta * (advance - theta_rate_);
    theta_ = theta;

    // 判定 → MER → 差動復号 → ビット
    const cfloat r = static_cast<cfloat>(std::polar(1.0, -theta));
    const std::complex<double> rt = std::polar(1.0, theta);
    std::vector<uint8_t> words(ublk_.size());
    for (size_t i = 0; i < ublk_.size(); ++i) {
        const cfloat v = ublk_[i] * r;
        const uint8_t wd = DemapHard(v);
        words[i] = wd;
        mer_err_acc_ += std::norm(v - C[wd]);
        mer_ref_acc_ += std::norm(C[wd]);
    }
    mer_n_ += static_cast<long long>(ublk_.size());
    // LS の右辺: 判定点を位相で戻した目標値 d e^{jθ}
    const int N = cfg_.eq_taps;
    for (size_t i = 0; i < zblk_idx_.size(); ++i) {
        const int sym = zblk_idx_[i];
        if (sym < 0 || sym >= static_cast<int>(words.size())) continue;
        const std::complex<double> t = std::complex<double>(C[words[static_cast<size_t>(sym)]]) * rt;
        const cfloat* xv = &zblk_[i * static_cast<size_t>(N)];
        for (int a = 0; a < N; ++a) p_[static_cast<size_t>(a)] += std::conj(std::complex<double>(xv[a])) * t;
    }
    ls_count_ += static_cast<long long>(zblk_idx_.size());
    zblk_idx_.clear();
    zblk_.clear();
    ublk_.clear();
    blk_n_ = 0;
    ++blocks_done_;
    if (ls_count_ >= kLsSolveEvery) SolveEqualizer();

    for (uint8_t wd : words) OnBits(diff_.Decode(wd));

    if (blocks_done_ % kStatusEveryBlocks == 0) {
        mer_db_ = mer_err_acc_ > 0 ? 10 * std::log10(mer_ref_acc_ / mer_err_acc_) : 60.0;
        mer_err_acc_ = mer_ref_acc_ = 0.0;
        mer_n_ = 0;
        low_mer_blocks_ = (mer_db_ < 10.0) ? low_mer_blocks_ + 1 : 0;
        {
            std::lock_guard<std::mutex> lk(mu_);
            st_.mer_db = mer_db_;
            st_.byte_sync = aligned_;
            st_.locked = aligned_ && mer_db_ > 15.0;
            st_.cfo_hz = nco_freq_hz_ + theta_rate_ / (2 * kPi) / kPhaseBlock * (cfg_.fs / sym_period_);
            st_.clock_ppm = (sym_period_ * cfg_.symbol_rate / cfg_.fs - 1.0) * 1e6;   // channel.clock_ppm と同じ向き
            st_.inverted = inverted_;
            if (level_n_ > 0) st_.level_dbfs = 10 * std::log10(level_acc_ / level_n_ + 1e-30);
            level_acc_ = 0.0;
            level_n_ = 0;
            st_.tsmf_frames = tsmf_.frames();
            st_.tsmf_bad_headers = tsmf_.bad_headers();
            st_.tsmf_selected = tsmf_.selected();
            st_.tsmf_streams.clear();
            if (tsmf_.has_header()) {
                const TsmfHeader& h = tsmf_.header();
                for (int i = 0; i < kTsmfMaxTs; ++i) {
                    if (h.available[i]) st_.tsmf_streams.push_back({i + 1, h.ts_id[i], h.onid[i]});
                }
            }
        }
        if (low_mer_blocks_ >= kLowMerWindows) {
            // 信号が消えた/変わった: 取得からやり直す
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++st_.reacquisitions;
            }
            inverted_ = false;
            inversion_tried_ = false;
            x_.erase(x_.begin(), x_.end() - std::min<size_t>(x_.size(), 64));
            x_abs0_ = in_total_ - static_cast<long long>(x_.size());
            nco_freq_hz_ = 0.0;
            StartTrack();
            state_ = State::kAcquire;
            return;
        }
    }
    // ビット同期が取れないまま時間が経ったら、スペクトル反転を試し、それでも駄目なら再取得
    if (!aligned_ && track_symbols_ > kSyncTimeoutSymbols) {
        if (cfg_.allow_inversion && !inversion_tried_) {
            inversion_tried_ = true;
            SetInverted(!inverted_);
        } else {
            {
                std::lock_guard<std::mutex> lk(mu_);
                ++st_.reacquisitions;
            }
            inverted_ = false;
            inversion_tried_ = false;
            x_.erase(x_.begin(), x_.end() - std::min<size_t>(x_.size(), 64));
            x_abs0_ = in_total_ - static_cast<long long>(x_.size());
            nco_freq_hz_ = 0.0;
            StartTrack();
            state_ = State::kAcquire;
        }
    }
}

void IsdbcDemod::SolveEqualizer() {
    const int N = cfg_.eq_taps;
    // 上三角だけ蓄積しているので下三角を埋める
    for (int a = 0; a < N; ++a) {
        for (int b = 0; b < a; ++b) R_[static_cast<size_t>(a) * N + b] = std::conj(R_[static_cast<size_t>(b) * N + a]);
    }
    std::vector<std::complex<double>> A(R_.size()), b(p_.size());
    double tr = 0.0;
    for (int a = 0; a < N; ++a) tr += R_[static_cast<size_t>(a) * N + a].real();
    const double reg = 1e-3 * tr / N;
    for (size_t i = 0; i < R_.size(); ++i) A[i] = R_[i] / static_cast<double>(ls_count_);
    for (int a = 0; a < N; ++a) A[static_cast<size_t>(a) * N + a] += reg / static_cast<double>(ls_count_);
    for (size_t i = 0; i < p_.size(); ++i) b[i] = p_[i] / static_cast<double>(ls_count_);
    std::vector<std::complex<double>> w;
    if (SolveHermitian(A, b, N, &w)) {
        const double mix = (solves_ < 3) ? 1.0 : 0.3;
        for (int m = 0; m < N; ++m) w_[static_cast<size_t>(m)] = (1 - mix) * w_[static_cast<size_t>(m)] + mix * w[static_cast<size_t>(m)];
        ++solves_;
        UpdateTapArrays();
    }
    std::fill(R_.begin(), R_.end(), 0.0);
    std::fill(p_.begin(), p_.end(), 0.0);
    ls_count_ = 0;
}

// ---------------------------------------------------------------------------
// ビット → バイト → TSP
// ---------------------------------------------------------------------------
void IsdbcDemod::OnBits(uint8_t word) {
    if (!aligned_) {
        for (int k = 5; k >= 0; --k) bitbuf_.push_back(static_cast<uint8_t>((word >> k) & 1));
        const size_t need = 24 + 8 * static_cast<size_t>(kRsN) * (kSyncSearchPackets + 1);
        if (bitbuf_.size() < need) return;
        // 24 通りのビット境界 × 204 バイト周期で同期バイトが最も揃うものを探す
        const size_t n_bytes = static_cast<size_t>(kRsN) * (kSyncSearchPackets + 1);
        std::vector<uint8_t> bytes(n_bytes);
        int best_b = -1, best_off = 0;
        double best_ratio = 0.0;
        for (int b = 0; b < 24; ++b) {
            for (size_t i = 0; i < n_bytes; ++i) {
                unsigned v = 0;
                const uint8_t* p = &bitbuf_[static_cast<size_t>(b) + i * 8];
                for (int k = 0; k < 8; ++k) v = (v << 1) | p[k];
                bytes[i] = static_cast<uint8_t>(v);
            }
            int off = 0;
            const double ratio = SyncScore(bytes.data(), n_bytes, &off);
            if (ratio > best_ratio) {
                best_ratio = ratio;
                best_b = b;
                best_off = off;
            }
        }
        if (best_ratio < kSyncAcceptRatio) {
            bitbuf_.erase(bitbuf_.begin(), bitbuf_.begin() + static_cast<long long>(bitbuf_.size() / 2));
            return;
        }
        // 整列: 先頭 TSP のビット位置から詰め直す
        aligned_ = true;
        bad_sync_run_ = 0;
        tspbuf_.clear();
        bitacc_ = 0;
        bitacc_n_ = 0;
        size_t pos = static_cast<size_t>(best_b) + 8 * static_cast<size_t>(best_off);
        for (; pos + 8 <= bitbuf_.size(); pos += 8) {
            unsigned v = 0;
            for (int k = 0; k < 8; ++k) v = (v << 1) | bitbuf_[pos + k];
            tspbuf_.push_back(static_cast<uint8_t>(v));
            if (tspbuf_.size() == static_cast<size_t>(kRsN)) {
                OnTsp(tspbuf_.data());
                tspbuf_.clear();
                if (!aligned_) return;
            }
        }
        for (; pos < bitbuf_.size(); ++pos) {
            bitacc_ = (bitacc_ << 1) | bitbuf_[pos];
            ++bitacc_n_;
        }
        bitbuf_.clear();
        std::lock_guard<std::mutex> lk(mu_);
        st_.byte_sync = true;
        return;
    }
    bitacc_ = (bitacc_ << 6) | word;
    bitacc_n_ += 6;
    while (bitacc_n_ >= 8) {
        tspbuf_.push_back(static_cast<uint8_t>((bitacc_ >> (bitacc_n_ - 8)) & 0xFF));
        bitacc_n_ -= 8;
        bitacc_ &= (1u << bitacc_n_) - 1u;
        if (tspbuf_.size() == static_cast<size_t>(kRsN)) {
            OnTsp(tspbuf_.data());
            tspbuf_.clear();
            if (!aligned_) return;
        }
    }
}

void IsdbcDemod::OnTsp(const uint8_t* tsp) {
    if (tsp[0] == kSyncByte || tsp[0] == kSyncByteInverted) {
        bad_sync_run_ = 0;
    } else if (++bad_sync_run_ >= kSyncLossRun) {
        aligned_ = false;
        bitbuf_.clear();
        frame_.Reset();
        tsmf_.Reset();
        std::lock_guard<std::mutex> lk(mu_);
        ++st_.sync_losses;
        st_.byte_sync = false;
        st_.locked = false;
        return;
    }
    uint8_t pkt[kTsPacket];
    int nerr = 0;
    if (!frame_.Push(tsp, pkt, &nerr)) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++st_.rs_packets;
        if (nerr < 0) ++st_.rs_uncorrectable;
    }
    if (tsmf_.Push(pkt)) {
        out_.insert(out_.end(), pkt, pkt + kTsPacket);
    }
}

void IsdbcDemod::FlushOutput() {
    if (out_.empty()) return;
    const size_t n = out_.size() / kTsPacket;
    {
        std::lock_guard<std::mutex> lk(mu_);
        st_.ts_packets += static_cast<long long>(n);
    }
    if (on_packets_) on_packets_(out_.data(), n);
    out_.clear();
}

}  // namespace isdbc
