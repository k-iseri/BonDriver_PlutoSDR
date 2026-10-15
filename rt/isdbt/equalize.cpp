#include "equalize.h"


#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>

#include "pilots.h"

namespace isdbt {

namespace {

// 前後 avg シンボルの移動平均（中央寄せ、端は窓を内側へ寄せる）を全行分作る
void WindowedAverage(const Mat<std::complex<double>>& E, const Mat<double>& C, int avg,
                     Mat<std::complex<double>>* Hg, Mat<char>* known) {
    const int n_sym = static_cast<int>(E.rows);
    const int M = static_cast<int>(E.cols);
    std::vector<std::complex<double>> cs(static_cast<size_t>(n_sym + 1) * M);
    std::vector<double> cc(static_cast<size_t>(n_sym + 1) * M);
    for (int r = 0; r < n_sym; ++r) {
        for (int j = 0; j < M; ++j) {
            cs[static_cast<size_t>(r + 1) * M + j] = cs[static_cast<size_t>(r) * M + j] + E[r][j];
            cc[static_cast<size_t>(r + 1) * M + j] = cc[static_cast<size_t>(r) * M + j] + C[r][j];
        }
    }
    *Hg = Mat<std::complex<double>>(n_sym, M);
    *known = Mat<char>(n_sym, M);
    for (int r = 0; r < n_sym; ++r) {
        const int a = std::clamp(r - avg / 2, 0, n_sym - avg);
        for (int j = 0; j < M; ++j) {
            const std::complex<double> Es = cs[static_cast<size_t>(a + avg) * M + j] - cs[static_cast<size_t>(a) * M + j];
            const double Cs = cc[static_cast<size_t>(a + avg) * M + j] - cc[static_cast<size_t>(a) * M + j];
            (*known)[r][j] = Cs > 0.0;
            (*Hg)[r][j] = Cs > 0.0 ? Es / Cs : 0.0;
        }
    }
}

}  // namespace

Mat<cfloat> ChannelEstimate(const Mat<cfloat>& Y, Mode mode, int frame_start, int avg_symbols,
                            std::vector<char>* bad_out) {
    const Layout& lo = LayoutOf(mode);
    const int K = ModeParamsOf(mode).total_carriers;
    const int n_sym = static_cast<int>(Y.rows);
    const int M = (K - 1) / 3 + 1;              // 格子 k = 3j（右端 CP を含む）
    assert(3 * (M - 1) == K - 1);
    const float* pv = lo.pilot_values.data();

    // 格子位置の生推定 E（既知位置のみ）と既知フラグ C
    Mat<std::complex<double>> E(n_sym, M);
    Mat<double> C(n_sym, M);
    for (int r = 0; r < n_sym; ++r) {
        int p = (r - frame_start) % kSpSymbolPeriod;
        if (p < 0) p += kSpSymbolPeriod;
        for (int k : lo.sp_positions[p]) {
            E[r][k / 3] = std::complex<double>(Y[r][k]) / static_cast<double>(pv[k]);
            C[r][k / 3] = 1.0;
        }
        E[r][M - 1] = std::complex<double>(Y[r][K - 1]) / static_cast<double>(pv[K - 1]);
        C[r][M - 1] = 1.0;
    }

    const int avg = std::min(avg_symbols, n_sym);
    Mat<std::complex<double>> HgM;
    Mat<char> knownM;
    WindowedAverage(E, C, avg, &HgM, &knownM);

    // バースト検出: 自シンボルの SP（瞬時推定 E）が窓平均からどれだけ外れるか。
    // 位相雑音による共通位相のずれはバーストではないので、共通位相を合わせてから比べる
    std::vector<char> bad(n_sym, 0);
    {
        std::vector<double> err(n_sym);
        for (int r = 0; r < n_sym; ++r) {
            std::complex<double> num = 0.0;
            double sig = 0.0;
            for (int j = 0; j < M; ++j) {
                if (C[r][j] <= 0.0) continue;
                num += E[r][j] * std::conj(HgM[r][j]);
                sig += std::norm(HgM[r][j]);
            }
            const std::complex<double> theta = std::abs(num) > 0 ? std::conj(num) / std::abs(num)
                                                                 : std::complex<double>(1.0);
            double e = 0.0;
            for (int j = 0; j < M; ++j) {
                if (C[r][j] <= 0.0) continue;
                e += std::norm(E[r][j] * theta - HgM[r][j]);
            }
            err[r] = e / std::max(sig, 1e-30);
        }
        std::vector<double> sorted = err;
        std::nth_element(sorted.begin(), sorted.begin() + n_sym / 2, sorted.end());
        const double thresh = std::max(10.0 * sorted[static_cast<size_t>(n_sym) / 2], 0.05);
        bool any = false;
        for (int r = 0; r < n_sym; ++r) {
            bad[r] = err[r] > thresh;
            any = any || bad[r];
        }
        if (any) {
            for (int r = 0; r < n_sym; ++r) {
                if (!bad[r]) continue;
                for (int j = 0; j < M; ++j) {
                    E[r][j] = 0.0;
                    C[r][j] = 0.0;
                }
            }
            WindowedAverage(E, C, avg, &HgM, &knownM);
        }
    }
    if (bad_out) *bad_out = bad;

    Mat<cfloat> H(n_sym, K);
    std::vector<std::complex<double>> flat(M);
    std::vector<char> known(M);
    for (int r = 0; r < n_sym; ++r) {
        int n_known = 0;
        std::complex<double> slope_acc = 0.0;
        std::complex<double> prev{};
        bool prev_known = false;
        for (int j = 0; j < M; ++j) {
            known[j] = knownM[r][j];
            const std::complex<double> hg = HgM[r][j];
            if (known[j] && prev_known) slope_acc += hg * std::conj(prev);
            if (known[j]) ++n_known;
            prev = hg;
            prev_known = known[j];
            flat[j] = hg;
        }
        // バルク遅延（格子 1 点 = 3 キャリアあたりの位相回転）を除く。回転子は格子ごとの cos/sin ではなく
        // double の複素乗算で進め、256 点ごとに正確な位相から取り直す（差は double の丸め程度）
        const double dphi = std::arg(slope_acc);
        {
            const std::complex<double> step = std::polar(1.0, -dphi);
            std::complex<double> rot;
            for (int j = 0; j < M; ++j) {
                if (j % 256 == 0) rot = std::polar(1.0, -dphi * j);
                flat[j] *= rot;
                rot *= step;
            }
        }
        // 既知でない格子点を線形補間（端は端の値）
        if (n_known < M && n_known >= 2) {
            int lo_j = 0;
            while (!known[lo_j]) ++lo_j;
            int hi_j = M - 1;
            while (!known[hi_j]) --hi_j;
            int left = lo_j;
            for (int j = 0; j < M; ++j) {
                if (known[j]) {
                    left = j;
                    continue;
                }
                if (j < lo_j) {
                    flat[j] = flat[lo_j];
                } else if (j > hi_j) {
                    flat[j] = flat[hi_j];
                } else {
                    int right = j + 1;
                    while (!known[right]) ++right;
                    const double t = static_cast<double>(j - left) / (right - left);
                    flat[j] = flat[left] * (1.0 - t) + flat[right] * t;
                }
            }
        }
        // キャリア方向の線形補間（格子 3j の間）と傾斜の復元（回転子 exp(i dphi k / 3) も同じく漸化式）
        cfloat* Hr = H[r];
        {
            const std::complex<double> step = std::polar(1.0, dphi / 3.0);
            std::complex<double> rot;
            for (int j = 0; j < M - 1; ++j) {
                const std::complex<double> f0 = flat[j];
                const std::complex<double> df = (flat[j + 1] - flat[j]) / 3.0;
                for (int d = 0; d < 3; ++d) {
                    const int k = 3 * j + d;
                    if (k % 768 == 0) rot = std::polar(1.0, dphi * k / 3.0);
                    const std::complex<double> v = f0 + df * static_cast<double>(d);
                    Hr[k] = static_cast<cfloat>(v * rot);
                    rot *= step;
                }
            }
            const int k = K - 1;
            Hr[k] = static_cast<cfloat>(flat[M - 1] * std::polar(1.0, dphi * k / 3.0));
        }
    }

    // 共通位相誤差（各シンボル自身の SP）
    for (int r = 0; r < n_sym; ++r) {
        int p = (r - frame_start) % kSpSymbolPeriod;
        if (p < 0) p += kSpSymbolPeriod;
        std::complex<double> num = 0.0;
        for (int k : lo.sp_positions[p]) {
            num += std::complex<double>(Y[r][k]) * std::conj(std::complex<double>(H[r][k])) *
                   static_cast<double>(pv[k]);
        }
        const double ph = std::arg(num);
        const cfloat rot(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
        cfloat* Hr = H[r];
        for (int k = 0; k < K; ++k) Hr[k] *= rot;
    }
    return H;
}

void Equalize(const Mat<cfloat>& Y, const Mat<cfloat>& H, Mat<cfloat>* D, Mat<float>* csi) {
    const size_t n_sym = Y.rows, K = Y.cols;
    *D = Mat<cfloat>(n_sym, K);
    *csi = Mat<float>(n_sym, K);
    for (size_t r = 0; r < n_sym; ++r) {
        const cfloat* y = Y[r];
        const cfloat* h = H[r];
        cfloat* d = (*D)[r];
        float* w = (*csi)[r];
        double sum = 0.0;
        for (size_t k = 0; k < K; ++k) {
            // H = 0（瞬断などで推定不能）は D も CSI も 0（NaN を残すと Viterbi 全体を汚染する）。
            // 除算は y·conj(h) / |h|² で書く（std::complex の operator/ はスケーリング付きで数倍遅い）
            const float hr = h[k].real(), hi = h[k].imag();
            const float hn = hr * hr + hi * hi;
            if (hn > 0.0f && std::isfinite(hn)) {
                const float yr = y[k].real(), yi = y[k].imag();
                const float inv = 1.0f / hn;
                d[k] = cfloat((yr * hr + yi * hi) * inv, (yi * hr - yr * hi) * inv);
                w[k] = hn;
            } else {
                d[k] = cfloat(0.0f, 0.0f);
                w[k] = 0.0f;
            }
            sum += w[k];
        }
        const float inv_mean = sum > 0.0 ? static_cast<float>(K / sum) : 0.0f;
        for (size_t k = 0; k < K; ++k) w[k] *= inv_mean;
    }
}

}  // namespace isdbt
