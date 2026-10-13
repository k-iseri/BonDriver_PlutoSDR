#include "framing.h"

#include <cstring>

#include "../isdbt/fec.h"

namespace isdbc {

double SyncScore(const uint8_t* stream, size_t n, int* best_offset) {
    const size_t rows = n / kRsN;
    if (rows < 3) {
        if (best_offset) *best_offset = 0;
        return 0.0;
    }
    const size_t cnt = rows - 1;
    int best = 0, best_hits = -1;
    for (int off = 0; off < kRsN; ++off) {
        int hits = 0;
        for (size_t r = 0; r < cnt; ++r) {
            const uint8_t b = stream[off + r * kRsN];
            hits += (b == kSyncByte || b == kSyncByteInverted);
        }
        if (hits > best_hits) {
            best_hits = hits;
            best = off;
        }
    }
    if (best_offset) *best_offset = best;
    return static_cast<double>(best_hits) / static_cast<double>(cnt);
}

FrameDecoder::FrameDecoder() {
    // ランダマイズ PRBS: グループ内パケット i のペイロードは PRBS[188 i : 188 i + 187]（C.5.1）
    uint8_t prbs[kPrbsPeriodBytes];
    isdbt::EnergyDispersalPrbs(kPrbsPeriodBytes, prbs);
    for (int i = 0; i < kScrambleGroup; ++i) std::memcpy(prbs_[i], prbs + kTsPacket * i, kTsPacket - 1);
    Reset();
}

void FrameDecoder::Reset() {
    std::memset(ring_, 0, sizeof(ring_));
    for (int& p : ring_phase_) p = -1;
    count_ = 0;
    in_phase_ = -1;
}

bool FrameDecoder::Push(const uint8_t* tsp, uint8_t* out, int* nerr) {
    const int slot = static_cast<int>(count_ % kInterleaveI);
    std::memcpy(ring_[slot], tsp, kRsN);
    // 入力側のグループ位相（同期バイトは遅延なしで通るので、デインターリーブ前でも分かる）。
    // 最初の 0xB8 を見た時点で、それ以前の TSP の位相も遡って確定する
    if (tsp[0] == kSyncByteInverted) {
        if (in_phase_ < 0) {
            for (int k = 1; k < kInterleaveI && k <= count_; ++k) {
                ring_phase_[(slot - k + kInterleaveI) % kInterleaveI] = (kScrambleGroup - k % kScrambleGroup) % kScrambleGroup;
            }
        }
        in_phase_ = 0;
    } else if (in_phase_ >= 0) {
        in_phase_ = (in_phase_ + 1) % kScrambleGroup;
    }
    ring_phase_[slot] = in_phase_;
    ++count_;
    if (count_ < kInterleaveI) return false;         // 履歴不足（先頭 11 TSP）
    // デインターリーブ: 出力 TSP t のバイト j は入力 TSP t − 11 + b（b = j mod 12）のバイト j
    uint8_t de[kRsN];
    const long long t = count_ - 1;
    for (int j = 0; j < kRsN; ++j) {
        const int b = j % kInterleaveI;
        const long long src = t - (kInterleaveI - 1) + b;
        de[j] = ring_[src % kInterleaveI][j];
    }
    int err = 0;
    isdbt::RsDecode(de, 1, &err);
    // 出力 TSP t の同期バイト（位相）は入力 TSP t − 11 のもの。RS 訂正後の 0xB8 が最優先
    int phase = ring_phase_[(t - (kInterleaveI - 1) + kInterleaveI) % kInterleaveI];
    if (err >= 0 && de[0] == kSyncByteInverted) phase = 0;
    out[0] = kSyncByte;
    if (phase >= 0) {
        for (int i = 0; i < kTsPacket - 1; ++i) out[1 + i] = de[1 + i] ^ prbs_[phase][i];
    } else {
        std::memcpy(out + 1, de + 1, kTsPacket - 1);
        err = -1;                                     // 位相未確定のうちは信用しない
    }
    if (err < 0) out[1] |= kTeiMask;
    *nerr = err;
    return true;
}

}  // namespace isdbc
