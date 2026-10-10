// パイロット・制御キャリアの配置と値（STD-B31 3.12）。ref/isdbt/pilots.py の C++ 版。
// モードごとに一度だけ計算してキャッシュする Layout にまとめる。
#pragma once
#include <vector>

#include "params.h"

namespace isdbt {

struct Layout {
    Mode mode;
    int dc;                                  // データキャリア数/セグメント
    std::vector<float> pilot_values;         // キャリア k の BPSK 値 (4/3)(1-2 w_k)（total_carriers 要素）
    // SP の絶対キャリア番号（位相 p = n mod 4 ごと。右端 CP は含まない）
    std::vector<int> sp_positions[kSpSymbolPeriod];
    // データキャリア絶対番号 dpos4[p][f * dc + i]: 位相 p、周波数順セグメント f、セグメント内データ番号 i
    std::vector<int> dpos4[kSpSymbolPeriod];
    // キャリアランダマイズの逆写像 inv[perm[i]] = i
    std::vector<int> randomize_inv;
    // TMCC / AC1 絶対位置
    std::vector<int> tmcc_abs;
    std::vector<int> ac1_abs;

    int cp_position() const { return ModeParamsOf(mode).total_carriers - 1; }
};

// モードごとの Layout（初回に構築して以後キャッシュ）
const Layout& LayoutOf(Mode mode);

// キャリア絶対番号 k ごとの PRBS 出力 w_k（STD-B31 図 3-31、全 1 初期化で帯域左端から連続生成）
std::vector<uint8_t> PilotPrbs(Mode mode);

}  // namespace isdbt
