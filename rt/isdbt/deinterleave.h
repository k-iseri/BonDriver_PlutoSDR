// デインターリーブ（STD-B31 3.9〜3.11 の逆）と軟判定デマップ。ref/isdbt/deinterleave.py の C++ 版。
// 受信側の補遅延: 時間 I·(95−m_c)、ビット 120−d_i、バイト 11−b（docs/isdbt-spec-notes.md 10 章）。
// 行列は (n_sym, 13*dc)（列 = セグメント番号 s * dc + 論理キャリア c）で持つ。
#pragma once
#include <vector>

#include "mat.h"
#include "params.h"

namespace isdbt {

// 等化後キャリア行列 (n_sym, total) → 論理キャリア (n_sym, 13*dc)。
// シンボル n の SP 位相は (n − frame_start) mod 4。T = cfloat（データ）と float（CSI）の両方に使う。
template <typename T>
Mat<T> DataToLogical(const Mat<T>& D, Mode mode, int frame_start);

// 時間デインターリーブ: out[n][s*dc+c] = in[n − I_s·(95 − m_c)][s*dc+c]。参照先が無い先頭は 0。
template <typename T>
Mat<T> TimeDeinterleave(const Mat<T>& Y, Mode mode, const int I_per_segment[kNSegments]);

// セグメント間デインターリーブ（図 3-26 の逆）。groups は 2 セグメント以上のグループのみ。
template <typename T>
void IntersegmentDeinterleave(Mat<T>* X, Mode mode, const std::vector<std::vector<int>>& groups);

// 階層のセグメント列から直列シンボルを取り出し（layer_stream）、軟判定デマップ × CSI 重み →
// ビットデインターリーブまでを行う。rows[r] は Xt/Wt の行番号。出力 (len(rows)*nseg*dc, bps)。
Mat<float> LayerSoftBits(const Mat<cfloat>& Xt, const Mat<float>& Wt,
                         const std::vector<int>& rows, const std::vector<int>& segments,
                         Modulation mod, Mode mode);

// 軟判定デマップ単体（正規化を戻した複素 → bps 値、正 = ビット 0 らしい）。テスト用に公開
void DemapSoft(cfloat d, Modulation mod, float* out);

}  // namespace isdbt
