// 誤り訂正の復号側（STD-B31 3.5〜3.8 の逆）: デパンクチャ → Viterbi → バイトデインターリーブ
// → エネルギー逆拡散 → RS(204,188) 復号。ref/isdbt/fec.py の復号側と同じ規約。
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "params.h"

namespace isdbt {

// --- デパンクチャ（3.8 の逆） ---
// 直列軟判定ビット列（正 = 0 らしい）→ (n_steps, 2) の (X, Y)。送られなかった位置は 0。
// 入力は周期あたり送出ビット数の倍数に切り詰める。戻り値はステップ数（pairs は 2*steps 要素）。
size_t Depuncture(const float* soft, size_t n, CodeRate rate, std::vector<float>* pairs);

// --- 軟判定 Viterbi（K=7、171/133） ---
// pairs は (X, Y) の対の連続列（正 = 0 らしい、0 = 消失）。逐次供給でき、窓付きトレースバックで
// 確定したビットを out に追記する。Flush() で残りを全部出す（末尾は最尤state からの打ち切り）。
class ViterbiDecoder {
public:
    ViterbiDecoder();
    void Reset();
    void Feed(const float* pairs, size_t n_steps, std::vector<uint8_t>* out);
    void Flush(std::vector<uint8_t>* out);

private:
    void Traceback(size_t n_emit, std::vector<uint8_t>* out);
    void FeedScalar(const float* pairs, size_t n_steps, std::vector<uint8_t>* out);
#ifdef __AVX2__
    void FeedAvx2(const float* pairs, size_t n_steps, std::vector<uint8_t>* out);
#endif

    static constexpr int kStates = 64;
    static constexpr size_t kWindow = 1 << 16;      // まとめてトレースバックする間隔
    static constexpr size_t kTail = 192;            // パス合流に十分な残し幅（> 5K）
    alignas(32) std::array<float, kStates> pm_;
    std::array<uint8_t, kStates> bm_index_[2];      // [b][s_new] → 枝メトリック {X+Y, X-Y, -X+Y, -X-Y} の添字
    alignas(32) float sx_[2][kStates];              // 枝メトリックの符号（±1）: bm = sx*X + sy*Y
    alignas(32) float sy_[2][kStates];
    std::vector<uint64_t> dec_;                     // ステップごとの決定ビット（state ごと 1 ビット）
    size_t steps_since_norm_ = 0;
};

// 一括復号（ref/isdbt/fec.py viterbi_decode 相当の使い方）。bits は 0/1 を 1 バイトずつ。
std::vector<uint8_t> ViterbiDecode(const float* pairs, size_t n_steps);

// ライブ経路用の速い版: デパンクチャを融合し（パンクチャ後の軟値列 soft をそのまま受ける）、
// 軟値を ±31 に量子化して 8bit コスト × 32 レーンの AVX2 で復号する。出力は ViterbiDecode と同じ形式。
// 量子化（平均振幅を 12、約 2.6 倍で飽和）で判定が変わるのはごく稀で、AVX2 が無いビルドでは
// Depuncture + ViterbiDecode に落ちる
std::vector<uint8_t> ViterbiDecodeSoft(const float* soft, size_t n, CodeRate rate);

// --- バイトデインターリーブ（3.7 の逆: ブランチ b を 11 - b TSP 遅延） ---
// in/out は (n, 204)。先頭 11 TSP は履歴が無く不定（0 由来）。
void ByteDeinterleave(const uint8_t* in, size_t n_tsp, uint8_t* out);

// --- エネルギー逆拡散（3.6 の逆） ---
// PRBS を n バイト生成（x^15 + x^14 + 1、初期値 100101010000000、バイトは MSB から詰める）
void EnergyDispersalPrbs(size_t n_bytes, uint8_t* out);
// 伝送順 (n, 204) [情報 203, 同期] → TSP (n, 204) [同期, 情報 203]。tsp_per_frame ごとに PRBS 初期化。
void EnergyDescrambleLayer(const uint8_t* onair, size_t n_tsp, int tsp_per_frame, uint8_t* out);

// --- RS(204,188) 復号（Berlekamp-Massey + Chien + Forney） ---
// tsps (n, 204) を in place で訂正し、nerr[i] に訂正バイト数（訂正不能は -1）を書く。
void RsDecode(uint8_t* tsps, size_t n_tsp, int* nerr);

// GF(2^8) テーブル（テストで使う）
const uint8_t* GfExpTable();   // 512 要素
const uint8_t* GfLogTable();   // 256 要素

// --- ビット詰め（np.packbits 相当、MSB から） ---
void PackBits(const uint8_t* bits, size_t n_bits, uint8_t* bytes);

}  // namespace isdbt
