// ISDB-C（ケーブル TV の 64QAM トランスモジュレーション、ITU-T J.83 Annex C + J.183 TSMF）の
// 固定パラメータ。ref/isdbc/params.py の C++ 版。根拠は docs/isdbc-spec-notes.md。
#pragma once
#include <complex>
#include <cstdint>
#include <string>

namespace isdbc {

using cfloat = std::complex<float>;

// --- 変調（J.83 C.6） ---
inline constexpr double kSymbolRateHz = 5'274'000.0;   // VERIFY: JCTEA STD-002（6 MHz チャンネル）
inline constexpr double kRolloff = 0.13;               // C.6.4
inline constexpr int kQam64Bits = 6;
inline constexpr double kQam64Rms = 6.48074069840786;  // sqrt(42)

// --- フレーム構成（J.83 C.4/C.5 = Annex A） ---
inline constexpr int kTsPacket = 188;
inline constexpr int kRsN = 204;
inline constexpr uint8_t kSyncByte = 0x47;
inline constexpr uint8_t kSyncByteInverted = 0xB8;
inline constexpr int kScrambleGroup = 8;
inline constexpr int kPrbsPeriodBytes = 8 * kTsPacket - 1;   // 1503
inline constexpr int kInterleaveI = 12;
inline constexpr uint8_t kTeiMask = 0x80;

// 有効 TS レート（188 バイト換算）≈ 29.16 Mbps
inline constexpr double kTsBitrateBps = kSymbolRateHz * kQam64Bits * 188.0 / 204.0;

// --- TSMF（J.183 Appendix I） ---
inline constexpr int kTsmfSlots = 53;
inline constexpr int kTsmfDataSlots = 52;
inline constexpr int kTsmfMaxTs = 15;
inline constexpr int kTsmfHeaderPid = 0x002F;     // VERIFY（受信機実装・実放送で確認）
inline constexpr int kTsmfSync = 0x1A86;
inline constexpr int kTsmfSyncMask = 0x1FFF;
inline constexpr int kTsmfSyncInverted = (~kTsmfSync) & kTsmfSyncMask;

// --- ケーブル TV チャンネル表（BonDriver_BDA の TransModulation 自動生成と同じ通し番号・周波数） ---
//   0..2 VHF 1..3ch (93 MHz+), 3..11 VHF 4..12ch (171 MHz+), 12..61 UHF 13..62ch (473 MHz+),
//   62..71 CATV C13..C22 (111 MHz+), 72..112 CATV C23..C63 (225 MHz+)。6 MHz 間隔、+1/7 MHz オフセット無し。
inline constexpr int kCatvChannels = 113;

inline bool CatvChannel(int index, double* freq_hz, std::string* name) {
    struct Band { int first_index, first_ch, count; double base_hz; const char* fmt; };
    static const Band bands[] = {
        {0, 1, 3, 93e6, "%dch"}, {3, 4, 9, 171e6, "%dch"}, {12, 13, 50, 473e6, "%dch"},
        {62, 13, 10, 111e6, "C%dch"}, {72, 23, 41, 225e6, "C%dch"},
    };
    for (const Band& b : bands) {
        if (index >= b.first_index && index < b.first_index + b.count) {
            const int k = index - b.first_index;
            if (freq_hz) *freq_hz = b.base_hz + 6e6 * k;
            if (name) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), b.fmt, b.first_ch + k);
                *name = buf;
            }
            return true;
        }
    }
    return false;
}

}  // namespace isdbc
