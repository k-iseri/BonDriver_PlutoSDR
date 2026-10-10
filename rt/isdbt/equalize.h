// SP による伝達関数推定とゼロフォーシング等化（ref/isdbt/demod.py channel_estimate / equalize）。
#pragma once
#include "mat.h"
#include "params.h"

namespace isdbt {

// SP（図 3-30、値は表 3-17）から各シンボルの伝達関数 H (n_sym, total) を推定する。
// アルゴリズムは ref/isdbt/demod.py channel_estimate と同一:
//   1. SP 位置の H = Y/P を前後 avg_symbols シンボルで平均（3 キャリアおきの格子）
//   1b. 自シンボルの SP が窓平均から大きく外れるシンボル（バースト雑音）を検出して平均から除く。
//       bad_out に検出結果を返す（CSI を 0 にして消失扱いにするため）
//   2. 格子の位相傾斜（バルク遅延）を除いて線形補間し、傾斜を戻す
//   3. 各シンボル自身の SP で共通位相誤差を補正
Mat<cfloat> ChannelEstimate(const Mat<cfloat>& Y, Mode mode, int frame_start, int avg_symbols = 8,
                            std::vector<char>* bad_out = nullptr);

// D = Y/H と CSI = |H|²（シンボルごとに平均 1 へ正規化）
void Equalize(const Mat<cfloat>& Y, const Mat<cfloat>& H, Mat<cfloat>* D, Mat<float>* csi);

}  // namespace isdbt
