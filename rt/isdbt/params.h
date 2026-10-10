// ISDB-T 物理層パラメータ（ARIB STD-B31 準拠）。ref/isdbt/params.py の C++ 版。
// 値の根拠（章・表番号）は ref/isdbt/params.py と docs/isdbt-spec-notes.md を参照。
// ref/ と食い違うとテスト（rt_test）が落ちるので、変更は必ず ref/ 側と同時に行う。
#pragma once
#include <array>
#include <cassert>
#include <complex>
#include <cstdint>
#include <string>

#include "isdbt_tables.h"

namespace isdbt {

using cfloat = std::complex<float>;

inline constexpr double kPi = 3.141592653589793238462643383279502884;

// --- 基本諸元（STD-B31 3.3 / 3.4） ---
inline constexpr double kSampleRateHz = 512.0e6 / 63.0;   // 8.126984... MSPS
inline constexpr int kTsPacket = 188;
inline constexpr uint8_t kTsSync = 0x47;
inline constexpr int kNSegments = 13;
inline constexpr int kSymbolsPerFrame = 204;

// 周波数軸上のセグメント並び（低い周波数から）。中央がセグメント 0（3.14 図 3-32）
inline constexpr std::array<int, kNSegments> kSegmentOrderInFreq = {11, 9, 7, 5, 3, 1, 0, 2, 4, 6, 8, 10, 12};

inline constexpr std::array<int, kNSegments> MakeSegmentFreqIndex() {
    std::array<int, kNSegments> inv{};
    for (int f = 0; f < kNSegments; ++f) inv[kSegmentOrderInFreq[f]] = f;
    return inv;
}
inline constexpr std::array<int, kNSegments> kSegmentFreqIndex = MakeSegmentFreqIndex();  // セグメント番号 → 周波数順位置

enum class Mode : int { M1 = 1, M2 = 2, M3 = 3 };

struct ModeParams {
    Mode mode;
    int fft_size;
    int carriers_per_segment;
    int total_carriers;             // 13 セグメント + 右端 CP 1 本
    int data_carriers_per_segment;
    int sp_per_segment;
    int tmcc_per_segment;
    int ac1_per_segment;
    std::array<int, 4> time_interleave_choices;

    int data_carriers_total() const { return data_carriers_per_segment * kNSegments; }
};

inline const ModeParams& ModeParamsOf(Mode m) {
    static const ModeParams tbl[3] = {
        {Mode::M1, 2048, 108, 1405, 96, 9, 1, 2, {0, 4, 8, 16}},
        {Mode::M2, 4096, 216, 2809, 192, 18, 2, 4, {0, 2, 4, 8}},
        {Mode::M3, 8192, 432, 5617, 384, 36, 4, 8, {0, 1, 2, 4}},
    };
    return tbl[static_cast<int>(m) - 1];
}

// GI は分数 1/den で表す（1/4, 1/8, 1/16, 1/32）
struct Gi {
    int den;                        // 4, 8, 16, 32
    std::string str() const { return "1/" + std::to_string(den); }
};
inline bool ParseGi(const std::string& s, Gi* out) {
    for (int den : {4, 8, 16, 32}) {
        if (s == "1/" + std::to_string(den)) { *out = Gi{den}; return true; }
    }
    return false;
}

inline int GuardSamples(Mode m, Gi gi) { return ModeParamsOf(m).fft_size / gi.den; }
inline int SymbolSamples(Mode m, Gi gi) { return ModeParamsOf(m).fft_size + GuardSamples(m, gi); }

// 有効キャリア絶対番号 k → FFT ビン番号（中央キャリアが DC、3.14.1）
inline int CarrierToFftBin(Mode m, int k) {
    const ModeParams& mp = ModeParamsOf(m);
    int center = (mp.total_carriers - 1) / 2;
    int b = (k - center) % mp.fft_size;
    return b < 0 ? b + mp.fft_size : b;
}

// --- 変調・符号化（STD-B31 3.5〜3.8） ---
enum class Modulation { DQPSK, QPSK, QAM16, QAM64 };

inline int BitsPerSymbol(Modulation m) {
    switch (m) {
        case Modulation::DQPSK:
        case Modulation::QPSK: return 2;
        case Modulation::QAM16: return 4;
        case Modulation::QAM64: return 6;
    }
    return 0;
}
inline const char* ModulationName(Modulation m) {
    switch (m) {
        case Modulation::DQPSK: return "DQPSK";
        case Modulation::QPSK: return "QPSK";
        case Modulation::QAM16: return "16QAM";
        case Modulation::QAM64: return "64QAM";
    }
    return "?";
}

// 符号化率は分数 num/den
struct CodeRate {
    int num, den;
    std::string str() const { return std::to_string(num) + "/" + std::to_string(den); }
};

// パンクチャパターン（STD-B31 3.8 表 3-8）。x/y は送出マスク、送出順は位置ごとに X, Y の順
struct PuncturePattern {
    CodeRate rate;
    const char* x;
    const char* y;
};
inline constexpr PuncturePattern kPuncturePatterns[] = {
    {{1, 2}, "1", "1"},
    {{2, 3}, "10", "11"},
    {{3, 4}, "101", "110"},
    {{5, 6}, "10101", "11010"},
    {{7, 8}, "1000101", "1111010"},
};
inline const PuncturePattern* FindPuncture(CodeRate r) {
    for (const auto& p : kPuncturePatterns) {
        if (p.rate.num == r.num && p.rate.den == r.den) return &p;
    }
    return nullptr;
}

// 畳み込み符号: 拘束長 7、G1=171(8) → X、G2=133(8) → Y
inline constexpr int kConvK = 7;
inline constexpr int kConvG1 = 0171;
inline constexpr int kConvG2 = 0133;

// 外符号 RS(204,188)
inline constexpr int kRsN = 204;
inline constexpr int kRsK = 188;
inline constexpr int kRsPrimPoly = 0x11D;
inline constexpr int kRsFirstRoot = 0;
inline constexpr int kRsNRoots = 16;

// バイトインターリーブ: 12 ブランチ × 17 バイト（STD-B31 3.7）
inline constexpr int kByteInterleaveBranches = 12;
inline constexpr int kByteInterleaveDepth = 17;

// エネルギー拡散 PRBS: x^15 + x^14 + 1、初期値 100101010000000（STD-B31 3.6）
inline constexpr int kEnergyDispersalInit = 0b100101010000000;

// ビットインターリーブ遅延（単位: キャリアシンボル）。b0 が遅延 0（STD-B31 3.9.3）
inline constexpr int kBitInterleaveMaxDelay = 120;
inline const int* BitInterleaveDelays(Modulation m) {
    static const int qpsk[] = {0, 120};
    static const int qam16[] = {0, 40, 80, 120};
    static const int qam64[] = {0, 24, 48, 72, 96, 120};
    switch (m) {
        case Modulation::DQPSK:
        case Modulation::QPSK: return qpsk;
        case Modulation::QAM16: return qam16;
        case Modulation::QAM64: return qam64;
    }
    return nullptr;
}

// 時間インターリーブ: キャリア c の送信側遅延 = I * ((5c) mod 96) シンボル（STD-B31 3.11.1）
inline constexpr int kTimeInterleaveMod = 96;
inline constexpr int kTimeInterleaveMult = 5;
inline int TimeInterleaveDelay(int c, int I) { return I * ((kTimeInterleaveMult * c) % kTimeInterleaveMod); }
inline int TimeInterleaveAdjust(int I) {
    int r = (-95 * I) % kSymbolsPerFrame;
    return r < 0 ? r + kSymbolsPerFrame : r;
}
inline int TimeDeinterleaveMaxDelay(int I) { return I * (kTimeInterleaveMod - 1); }

// 1 階層・1 OFDM フレームあたりの TSP 数（必ず整数になる）
inline int TspPerFrame(Mode mode, Modulation mod, CodeRate rate, int n_segments) {
    long long bits = 1LL * n_segments * ModeParamsOf(mode).data_carriers_per_segment *
                     kSymbolsPerFrame * BitsPerSymbol(mod) * rate.num;
    assert(bits % (static_cast<long long>(rate.den) * kRsN * 8) == 0);
    return static_cast<int>(bits / (static_cast<long long>(rate.den) * kRsN * 8));
}

// マッピングの正規化係数（STD-B31 3.9）
inline float ConstellationNorm(Modulation m) {
    switch (m) {
        case Modulation::DQPSK:
        case Modulation::QPSK: return 1.4142135623730951f;   // √2
        case Modulation::QAM16: return 3.1622776601683795f;  // √10
        case Modulation::QAM64: return 6.48074069840786f;    // √42
    }
    return 1.0f;
}

// --- パイロット（STD-B31 3.12） ---
inline constexpr int kSpSpacing = 12;
inline constexpr int kSpSymbolPeriod = 4;
inline constexpr int kPilotPrbsInit = 0b11111111111;   // 図 3-31、全 1 初期化
inline constexpr double kPilotAmplitude = 4.0 / 3.0;   // 表 3-17

// TMCC / AC1 の絶対キャリア位置（同期変調部、表 3-15）
inline const int16_t* TmccPositionsAbs(Mode m, int* n) {
    switch (m) {
        case Mode::M1: *n = static_cast<int>(tables::kTmccAbsMode1.size()); return tables::kTmccAbsMode1.data();
        case Mode::M2: *n = static_cast<int>(tables::kTmccAbsMode2.size()); return tables::kTmccAbsMode2.data();
        case Mode::M3: *n = static_cast<int>(tables::kTmccAbsMode3.size()); return tables::kTmccAbsMode3.data();
    }
    *n = 0;
    return nullptr;
}
inline const int16_t* Ac1PositionsAbs(Mode m, int* n) {
    switch (m) {
        case Mode::M1: *n = static_cast<int>(tables::kAc1AbsMode1.size()); return tables::kAc1AbsMode1.data();
        case Mode::M2: *n = static_cast<int>(tables::kAc1AbsMode2.size()); return tables::kAc1AbsMode2.data();
        case Mode::M3: *n = static_cast<int>(tables::kAc1AbsMode3.size()); return tables::kAc1AbsMode3.data();
    }
    *n = 0;
    return nullptr;
}
inline const int16_t* CarrierRandomizeTable(Mode m, int* n) {
    switch (m) {
        case Mode::M1: *n = static_cast<int>(tables::kCarrierRandomizeMode1.size()); return tables::kCarrierRandomizeMode1.data();
        case Mode::M2: *n = static_cast<int>(tables::kCarrierRandomizeMode2.size()); return tables::kCarrierRandomizeMode2.data();
        case Mode::M3: *n = static_cast<int>(tables::kCarrierRandomizeMode3.size()); return tables::kCarrierRandomizeMode3.data();
    }
    *n = 0;
    return nullptr;
}

// --- TMCC（STD-B31 3.15） ---
inline constexpr int kTmccBitsPerFrame = 204;
inline constexpr uint16_t kTmccSyncW0 = 0b0011010111101110;
inline constexpr uint16_t kTmccSyncW1 = 0b1100101000010001;
// 差集合巡回符号 (273,191) の生成多項式（次数 82）: x^82 + x^77 + ... + 1（3.15.6.9）
inline constexpr int kTmccDsccExponents[] = {82, 77, 76, 71, 67, 66, 56, 52, 48, 40,
                                             36, 34, 24, 22, 18, 10, 4, 0};

// TMCC の 3 ビット変調符号・符号化率符号（表 3-23 / 3-24）
inline bool TmccModFromCode(int code, Modulation* out) {
    switch (code) {
        case 0b000: *out = Modulation::DQPSK; return true;
        case 0b001: *out = Modulation::QPSK; return true;
        case 0b010: *out = Modulation::QAM16; return true;
        case 0b011: *out = Modulation::QAM64; return true;
    }
    return false;
}
inline bool TmccRateFromCode(int code, CodeRate* out) {
    switch (code) {
        case 0b000: *out = {1, 2}; return true;
        case 0b001: *out = {2, 3}; return true;
        case 0b010: *out = {3, 4}; return true;
        case 0b011: *out = {5, 6}; return true;
        case 0b100: *out = {7, 8}; return true;
    }
    return false;
}
inline bool TmccInterleaveFromCode(Mode mode, int code, int* out) {
    const auto& choices = ModeParamsOf(mode).time_interleave_choices;
    if (code < 0 || code >= static_cast<int>(choices.size())) return false;
    *out = choices[code];
    return true;
}

}  // namespace isdbt
