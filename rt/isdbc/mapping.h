// バイト → m タプル、2 MSB 差動符号、64QAM 配置（J.83 C.6.1〜C.6.3）。ref/isdbc/mapping.py の C++ 版。
// 表の値は ref 側が仕様の論理式から導出したものを定数化し、rt_test が ref のベクタと照合する。
#pragma once
#include <cstddef>
#include <cstdint>

#include "params.h"

namespace isdbc {

// 象限ラベル (I_k Q_k) → 巡回位相（反時計回り 00 → 10 → 11 → 01）。添字は I*2+Q
inline constexpr uint8_t kPhaseOfIq[4] = {0, 3, 1, 2};
inline constexpr uint8_t kIqOfPhase[4] = {0, 2, 3, 1};
// (A_k B_k) → 位相差 d: d(00)=0, d(10)=1, d(11)=2, d(01)=3。添字は A*2+B
inline constexpr uint8_t kDeltaOfAb[4] = {0, 3, 1, 2};
inline constexpr uint8_t kAbOfDelta[4] = {0, 2, 3, 1};

// 差動符号化後の語 (I Q b3 b2 b1 b0) → 複素点（レベル ±1..±7）。64 要素
const cfloat* Constellation();

// 硬判定: 複素シンボル（レベル ±1..±7 スケール）→ 最近傍点の語
uint8_t DemapHard(cfloat z);

// 2 MSB の差動復号（逐次）。先頭は位相 0 基準
class DiffDecoder {
public:
    void Reset() { prev_ = 0; }
    uint8_t Decode(uint8_t enc_word) {
        const int p = kPhaseOfIq[(enc_word >> 4) & 3];
        const int d = (p - prev_ + 4) & 3;
        prev_ = p;
        return static_cast<uint8_t>((kAbOfDelta[d] << 4) | (enc_word & 0x0F));
    }

private:
    int prev_ = 0;
};

// 差動符号化（テスト・ベクタ照合用）。words は (A B b3..b0)、state は (I_{-1}, Q_{-1})
void DiffEncode(const uint8_t* words, size_t n, uint8_t* out, int i_prev = 0, int q_prev = 0);

// 3k バイト → 4k 語（MSB ファースト詰め）
void BytesToMtuples(const uint8_t* bytes, size_t n_bytes, uint8_t* words);

}  // namespace isdbc
