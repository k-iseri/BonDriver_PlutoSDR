// 受信側のフレーム処理（J.83 C.5 の逆）: 204 バイト周期の同期検出 → 畳み込みデインターリーブ →
// RS(204,188) → 8 パケットグループの逆ランダマイズ。ref/isdbc/framing.py の逐次版。
#pragma once
#include <cstddef>
#include <cstdint>

#include "params.h"

namespace isdbc {

// 204 バイト周期で同期バイト（0x47 / 0xB8）が最も揃うオフセットと一致率（ref framing.sync_score）
double SyncScore(const uint8_t* stream, size_t n, int* best_offset);

// 伝送 TSP（204 バイト、同期バイト先頭に整列済み）を 1 個ずつ入れ、デインターリーブ・RS 復号・
// 逆ランダマイズ済みの 188 バイト TS パケットを出す。先頭 11 TSP は履歴が無いので出力しない。
class FrameDecoder {
public:
    FrameDecoder();
    void Reset();
    // RS はまとめて復号する（AVX2 版が 32 TSP 同時なので、1 個ずつだと転置の分だけ無駄になる）。
    // Push はバッチが満ちたときだけ out（kBatch × 188 バイト）と nerr（kBatch 個。RS の訂正バイト数、
    // -1 = 訂正不能で TEI を立てる）に書いて個数を返す（0 または kBatch）。Flush は溜まっている分を出す
    static constexpr int kBatch = 32;
    int Push(const uint8_t* tsp, uint8_t* out, int* nerr);
    int Flush(uint8_t* out, int* nerr);

private:
    int Decode(uint8_t* out, int* nerr);
    uint8_t pend_[kBatch][kRsN];        // デインターリーブ済み、RS 待ち
    int pend_phase_[kBatch];
    int pend_n_ = 0;
    uint8_t ring_[kInterleaveI][kRsN];
    int ring_phase_[kInterleaveI];  // 各入力 TSP の 8 パケットグループ内位置（同期バイトはブランチ 0 で
                                    // 遅延なしなので、入力の 0xB8 から 11 TSP 先の出力位相が分かる）
    long long count_ = 0;
    int in_phase_ = -1;             // 直近の入力 TSP のグループ内位置（-1 = まだ 0xB8 を見ていない）
    uint8_t prbs_[kScrambleGroup][kTsPacket - 1];
};

}  // namespace isdbc
