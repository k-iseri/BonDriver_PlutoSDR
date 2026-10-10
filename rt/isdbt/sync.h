// 同期（ref/isdbt/sync.py の C++ 版）: 任意レート → 公称 512/63 MHz へ有理数比リサンプル →
// GI 相関でシンボル位置とクロック偏差 → 小数 CFO → 固定パイロット電力で整数 CFO。
// CarrierSource が必要なシンボル範囲だけ分数リサンプル + 周波数補正 + FFT して有効キャリアを返す。
#pragma once
#include <algorithm>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "fft.h"
#include "mat.h"
#include "params.h"

namespace isdbt {

// 任意レート fs の I/Q を公称レートへ有理数比でリサンプルする（scipy.resample_poly 相当の
// ポリフェーズ FIR、カイザー窓 beta=5.0）。fs が公称に等しければコピーを返す。
std::vector<cfloat> ResampleToNominal(const std::vector<cfloat>& x, double fs);

// 出力サンプル m（m0 ≤ m < m1）を入力位置 m*ratio に置く分数リサンプル（ppm 級の補正用）。
// ref/isdbt/sync.py resample_fractional と同じ多相 sinc（1/256 サンプル分解能、48 タップ）。
std::vector<cfloat> ResampleFractional(const std::vector<cfloat>& x, double ratio,
                                       long long m0, long long m1);

// 任意の実数位置 t[k]（サンプル単位）で多相 sinc 補間する（ref/isdbt/sync.py resample_at）。
void ResampleAt(const std::vector<cfloat>& x, const double* t, size_t n, cfloat* out);

struct SyncInfo {
    double clock_ppm = 0.0;
    double gi_start0 = 0.0;
    long long abs_symbol0 = 0;
    double cfo_frac_hz = 0.0;
    int cfo_int_bins = 0;
    int n_sym = 0;
    double cfo_hz() const { return cfo_frac_hz + cfo_int_bins * kSampleRateHz / fft_size; }
    int fft_size = 0;
};

// 有効キャリア行列の供給元（CarrierSource / TrackedCarrierSource の区間ビュー）
class ISymbolSource {
public:
    virtual ~ISymbolSource() = default;
    virtual int n_sym() const = 0;
    virtual Mat<cfloat> Symbols(int i0, int i1) = 0;
};

// 同期パラメータを一度だけ推定し、Symbols(i0, i1) で有効キャリア行列を返す。
class CarrierSource : public ISymbolSource {
public:
    CarrierSource(std::vector<cfloat> iq, double fs, Mode mode, Gi gi, int cfo_search = 80);

    int n_sym() const override { return n_sym_; }
    const SyncInfo& info() const { return info_; }
    Mode mode() const { return mode_; }

    // シンボル i0..i1-1 の有効キャリア行列 (i1-i0, total_carriers)
    Mat<cfloat> Symbols(int i0, int i1) override;

private:
    // 補正後タイムラインの [m0, m1) を分数リサンプル + 周波数補正して返す
    std::vector<cfloat> Samples(long long m0, long long m1) const;

    Mode mode_;
    int N_, Ng_, T_, margin_;
    std::vector<cfloat> x_;
    double ratio_ = 1.0;        // 補正後 m ↔ 入力位置 m*ratio
    double f_frac_ = 0.0;
    int k_int_ = 0;
    long long start0_ = 0;      // starts[i] = start0_ + i*T
    int n_sym_ = 0;
    SyncInfo info_;
    std::unique_ptr<Fft> fft_;
};

// タイミングと CFO を区分線形で追従する CarrierSource（長時間録音・ドリフト・瞬断向け）。
// アルゴリズムは ref/isdbt/sync.py TrackedCarrierSource と同一（詳細コメントはそちら）。
class TrackedCarrierSource {
public:
    TrackedCarrierSource(std::vector<cfloat> iq, double fs, Mode mode, Gi gi,
                         int cfo_search = 80, int chunk_syms = 64,
                         int smooth_chunks = 5, double lock_threshold = 0.5);

    int n_sym() const { return n_sym_; }
    Mode mode() const { return mode_; }
    const std::vector<std::pair<int, int>>& segments() const { return segments_; }

    // シンボル i0..i1-1（録音全体の番号）。i0 を含むロック区間の整数 CFO を使う
    Mat<cfloat> Symbols(int i0, int i1);

    double ppm_at(double i) const { return DeltaOf(i) * 1e6; }

private:
    double POf(double i) const;
    double FOf(double i) const;
    double DeltaOf(double i) const;
    int KIntFor(int seg_start);
    Mat<cfloat> FftWindow(int i0, int i1, int k_int, bool raw_bins);

    Mode mode_;
    int N_, Ng_, T_, margin_, cfo_search_, chunk_syms_;
    std::vector<cfloat> x_;
    std::vector<double> chunk_idx_, lock_metric_;
    std::vector<char> locked_;
    // 区間内で完結するアンカー補間表（区間端はクランプ。ref/isdbt/sync.py 参照）
    std::vector<double> interp_x_, interp_p_, interp_f_, slope_x_, slope_v_;
    std::vector<double> interp_phi_;   // 時変 CFO の累積位相 [rad]（ref/isdbt/sync.py 参照）
    std::vector<std::pair<int, int>> segments_;
    std::map<int, int> k_int_;
    int n_sym_ = 0;
    std::unique_ptr<Fft> fft_;
};

// TrackedCarrierSource のロック区間を先頭 0 の ISymbolSource として見せる
class SegmentView : public ISymbolSource {
public:
    SegmentView(TrackedCarrierSource* src, int i0, int i1) : src_(src), i0_(i0), n_(i1 - i0) {}
    int n_sym() const override { return n_; }
    Mat<cfloat> Symbols(int a, int b) override {
        a = std::max(0, a);
        b = std::min(b, n_);
        return src_->Symbols(i0_ + a, i0_ + b);
    }

private:
    TrackedCarrierSource* src_;
    int i0_, n_;
};

}  // namespace isdbt
