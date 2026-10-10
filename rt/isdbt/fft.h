// 2 のべき乗長の複素 FFT（Stockham 自動整列、自前実装・外部依存なし）。
// 必要な速度は Mode 3 で 992 回/秒の 8192 点なので、SIMD 化しなくても十分軽い。
// 精度は float（ref/ の scipy.fft と許容誤差内で一致することを rt_test で確認する）。
#pragma once
#include <complex>
#include <vector>

namespace isdbt {

class Fft {
public:
    explicit Fft(int n);
    int size() const { return n_; }
    // in-place 前方 FFT（X[k] = Σ x[n] e^{-2πi nk/N}、正規化なし = numpy/scipy と同じ）
    void Forward(std::complex<float>* x);

private:
    void Step(int n, int s, bool eo, std::complex<float>* x, std::complex<float>* y);
    int n_;
    std::vector<std::complex<float>> work_;
    std::vector<std::complex<float>> tw_;   // tw_[k] = exp(-2πi k / n_)、k < n_/2
};

}  // namespace isdbt
