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
// 同上、出力行 [row0, rows) だけ計算する（それより前は 0 のまま。ライブの範囲復号用）
template <typename T>
Mat<T> TimeDeinterleaveFrom(const Mat<T>& Y, Mode mode, const int I_per_segment[kNSegments], size_t row0);
// DataToLogical + IntersegmentDeinterleave を 1 回の gather で（列の並べ替えを合成した表を使う）。
// 行範囲版: D の行 [row0, row1) だけを出力行 0.. に（SP 位相は D の行番号で決める）
template <typename T>
Mat<T> DataToLogicalDeintRange(const Mat<T>& D, Mode mode, int frame_start,
                               const std::vector<std::vector<int>>& groups, size_t row0, size_t row1);
template <typename T>
Mat<T> DataToLogicalDeint(const Mat<T>& D, Mode mode, int frame_start, const std::vector<std::vector<int>>& groups);

// セグメント間デインターリーブ（図 3-26 の逆）。groups は 2 セグメント以上のグループのみ。
template <typename T>
void IntersegmentDeinterleave(Mat<T>* X, Mode mode, const std::vector<std::vector<int>>& groups);

// 階層のセグメント列から直列シンボルを取り出し（layer_stream）、軟判定デマップ × CSI 重み →
// ビットデインターリーブまでを行う。rows[r] は Xt/Wt の行番号。出力 (len(rows)*nseg*dc, bps)。
// 同上を 1 パスで int8（±q、ビタビの量子化幅）に直接書く版（ライブ経路用）。デマップ → CSI 重み →
// ビットデインターリーブ（遅延 ≤ 120 スロットなのでリングバッファ）→ 量子化。量子化の尺度は 1/61 の
// 間引きで平均振幅を先に測って決める（平均 → 12）。float 行列 2 枚（各 n_slots × bps × 4 B）の往復を省く
// 同上、FPGA 前段の {I8, Q8, CSI8} を 32bit に詰めた行列から（I/Q/CSI とも 1/64 単位）
std::vector<int8_t> LayerSoftBitsQPacked(const Mat<uint32_t>& Xt, const std::vector<int>& rows,
                                         const std::vector<int>& segments, Modulation mod, Mode mode, int q);

std::vector<int8_t> LayerSoftBitsQ(const Mat<cfloat>& Xt, const Mat<float>& Wt,
                                   const std::vector<int>& rows, const std::vector<int>& segments,
                                   Modulation mod, Mode mode, int q);

Mat<float> LayerSoftBits(const Mat<cfloat>& Xt, const Mat<float>& Wt,
                         const std::vector<int>& rows, const std::vector<int>& segments,
                         Modulation mod, Mode mode);

// 軟判定デマップ単体（正規化を戻した複素 → bps 値、正 = ビット 0 らしい）。テスト用に公開
void DemapSoft(cfloat d, Modulation mod, float* out);

}  // namespace isdbt
