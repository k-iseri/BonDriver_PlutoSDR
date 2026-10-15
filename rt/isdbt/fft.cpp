#include "fft.h"

#include "params.h"

#include <cassert>
#include <cmath>

namespace isdbt {

namespace {

// std::complex<float> の乗算は NaN 回復（C99 Annex G）の分岐が入り遅いので、バタフライは実部・虚部で書く
struct C2 {
    float re, im;
};
inline C2 Ld(const std::complex<float>* p) { return {p->real(), p->imag()}; }
inline void St(std::complex<float>* p, C2 v) { *p = std::complex<float>(v.re, v.im); }
inline C2 Add(C2 a, C2 b) { return {a.re + b.re, a.im + b.im}; }
inline C2 Sub(C2 a, C2 b) { return {a.re - b.re, a.im - b.im}; }
inline C2 Mul(C2 a, C2 b) { return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }
inline C2 MulJ(C2 a) { return {-a.im, a.re}; }       // j·a

}  // namespace

Fft::Fft(int n) : n_(n), work_(n) {
    assert(n > 0 && (n & (n - 1)) == 0);
    tw_.resize(n);
    for (int k = 0; k < n; ++k) {
        double a = -2.0 * kPi * k / n;
        tw_[k] = std::complex<float>(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
}

// Stockham 自動整列の基数 4（長さが 2 × 4^m なら最後に基数 2 を 1 段）。各段で間引きと並べ替えを
// 同時に行うのでビット反転が要らず、アクセスが連続する。n: この段の列長、s: ストライド、
// eo: 偶数段なら結果が y 側にあるので最後に x へ写す
void Fft::Step(int n, int s, bool eo, std::complex<float>* x, std::complex<float>* y) {
    if (n == 1) {
        if (eo) {
            for (int q = 0; q < s; ++q) y[q] = x[q];
        }
        return;
    }
    if (n == 2) {
        for (int q = 0; q < s; ++q) {
            const C2 a = Ld(x + q), b = Ld(x + q + s);
            St(y + q, Add(a, b));
            St(y + q + s, Sub(a, b));
        }
        Step(1, 2 * s, !eo, y, x);
        return;
    }
    const int n1 = n / 4, n2 = n / 2, n3 = n1 + n2;
    const int stride = n_ / n;          // twiddle 表の間引き（表は exp(-2πi k / n_)）
    for (int p = 0; p < n1; ++p) {
        const C2 w1 = Ld(&tw_[static_cast<size_t>(p) * stride]);
        const C2 w2 = Ld(&tw_[static_cast<size_t>(2 * p) * stride]);
        const C2 w3 = Ld(&tw_[static_cast<size_t>(3 * p) * stride]);
        const std::complex<float>* xa = x + static_cast<size_t>(s) * p;
        const std::complex<float>* xb = x + static_cast<size_t>(s) * (p + n1);
        const std::complex<float>* xc = x + static_cast<size_t>(s) * (p + n2);
        const std::complex<float>* xd = x + static_cast<size_t>(s) * (p + n3);
        std::complex<float>* y0 = y + static_cast<size_t>(s) * 4 * p;
        std::complex<float>* y1 = y0 + s;
        std::complex<float>* y2 = y1 + s;
        std::complex<float>* y3 = y2 + s;
        for (int q = 0; q < s; ++q) {
            const C2 a = Ld(xa + q), b = Ld(xb + q), c = Ld(xc + q), d = Ld(xd + q);
            const C2 apc = Add(a, c), amc = Sub(a, c);
            const C2 bpd = Add(b, d), jbmd = MulJ(Sub(b, d));
            St(y0 + q, Add(apc, bpd));
            St(y1 + q, Mul(w1, Sub(amc, jbmd)));
            St(y2 + q, Mul(w2, Sub(apc, bpd)));
            St(y3 + q, Mul(w3, Add(amc, jbmd)));
        }
    }
    Step(n1, 4 * s, !eo, y, x);
}

void Fft::Forward(std::complex<float>* x) {
    if (n_ == 1) return;
    Step(n_, 1, false, x, work_.data());
}

}  // namespace isdbt
