#include "fft.h"

#include "params.h"


#include <cassert>
#include <cmath>

namespace isdbt {

Fft::Fft(int n) : n_(n), work_(n) {
    assert(n > 0 && (n & (n - 1)) == 0);
    tw_.resize(n / 2);
    for (int k = 0; k < n / 2; ++k) {
        double a = -2.0 * kPi * k / n;
        tw_[k] = std::complex<float>(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
}

// Stockham: 各段で間引きと並べ替えを同時に行う（ビット反転不要、アクセスが連続）
void Fft::Step(int n, int s, bool eo, std::complex<float>* x, std::complex<float>* y) {
    if (n == 1) {
        if (eo) {
            for (int q = 0; q < s; ++q) y[q] = x[q];
        }
        return;
    }
    const int m = n / 2;
    const int stride = n_ / n;          // twiddle 表の間引き
    for (int p = 0; p < m; ++p) {
        const std::complex<float> wp = tw_[static_cast<size_t>(p) * stride];
        const std::complex<float>* xa = x + static_cast<size_t>(s) * p;
        const std::complex<float>* xb = x + static_cast<size_t>(s) * (p + m);
        std::complex<float>* ya = y + static_cast<size_t>(s) * 2 * p;
        std::complex<float>* yb = ya + s;
        for (int q = 0; q < s; ++q) {
            const std::complex<float> a = xa[q];
            const std::complex<float> b = xb[q];
            ya[q] = a + b;
            yb[q] = (a - b) * wp;
        }
    }
    Step(m, 2 * s, !eo, y, x);
}

void Fft::Forward(std::complex<float>* x) {
    if (n_ == 1) return;
    Step(n_, 1, false, x, work_.data());
}

}  // namespace isdbt
