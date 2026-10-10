#include "tmcc.h"

#include <cstring>

#include "pilots.h"

namespace isdbt {

namespace {

int GetBits(const uint8_t* b, int pos, int width) {
    int v = 0;
    for (int i = 0; i < width; ++i) v = (v << 1) | (b[pos + i] & 1);
    return v;
}

// 102 ビット → 82 ビットのパリティ（系統的、m(x)·x^82 mod g(x)）。ref/isdbt/tmcc.py dscc_parity
void DsccParity(const uint8_t* info102, uint8_t* parity82) {
    uint8_t g[83] = {};
    for (int e : kTmccDsccExponents) g[82 - e] = 1;     // g[0] が x^82 の係数
    uint8_t r[184];
    std::memcpy(r, info102, 102);
    std::memset(r + 102, 0, 82);
    for (int i = 0; i < 102; ++i) {
        if (r[i]) {
            for (int k = 0; k < 83; ++k) r[i + k] ^= g[k];
        }
    }
    std::memcpy(parity82, r + 102, 82);
}

TmccLayer ParseLayer(Mode mode, int v) {
    TmccLayer out;
    int mod_c = (v >> 10) & 7, rate_c = (v >> 7) & 7, il_c = (v >> 4) & 7, segs = v & 15;
    if (segs == 0b1111 || mod_c == 0b111) return out;   // 未使用
    if (!TmccModFromCode(mod_c, &out.mod)) return out;
    if (!TmccRateFromCode(rate_c, &out.rate)) return out;
    if (!TmccInterleaveFromCode(mode, il_c, &out.interleave)) return out;
    out.segments = segs;
    out.used = segs > 0;
    return out;
}

}  // namespace

bool DsccCheck(const uint8_t* code_bits184) {
    uint8_t parity[82];
    DsccParity(code_bits184, parity);
    return std::memcmp(parity, code_bits184 + 102, 82) == 0;
}

int SyncWordOf(const uint8_t* bits) {
    int v = GetBits(bits, 1, 16);
    if (v == kTmccSyncW0) return 0;
    if (v == kTmccSyncW1) return 1;
    return -1;
}

TmccInfo ParseTmccInfo(Mode mode, const uint8_t* b) {
    // フィールド位置は ref/isdbt/params.py TMCC_FIELDS（B20 起点の相対位置）
    TmccInfo info;
    info.system_id = GetBits(b, 0, 2);
    info.switching_indicator = GetBits(b, 2, 4);
    info.emergency_alarm = GetBits(b, 6, 1);
    // current（B27-66 相当）: 部分受信 1 + 階層 A/B/C 各 13 ビット
    info.partial_reception = b[7] != 0;
    for (int i = 0; i < 3; ++i) {
        info.layers[i] = ParseLayer(mode, GetBits(b, 8 + 13 * i, 13));
    }
    return info;
}

std::vector<uint8_t> TmccBitsFromCarriers(const cfloat* Y, size_t n_sym, Mode mode) {
    const Layout& lo = LayoutOf(mode);
    const int total = ModeParamsOf(mode).total_carriers;
    std::vector<uint8_t> b(n_sym, 0);
    for (size_t n = 1; n < n_sym; ++n) {
        const cfloat* cur = Y + n * total;
        const cfloat* prev = cur - total;
        double acc = 0.0;
        for (int k : lo.tmcc_abs) {
            const cfloat p = cur[k] * std::conj(prev[k]);
            acc += p.real();
        }
        b[n] = acc < 0.0 ? 1 : 0;
    }
    return b;
}

int FindFrameStart(const std::vector<uint8_t>& b, bool* parity_ok) {
    const int n = static_cast<int>(b.size());
    int first_sync = -1;
    for (int n0 = 0; n0 + kTmccBitsPerFrame <= n; ++n0) {
        if (SyncWordOf(b.data() + n0) < 0) continue;
        if (first_sync < 0) first_sync = n0;
        if (DsccCheck(b.data() + n0 + 20)) {
            *parity_ok = true;
            return n0;
        }
    }
    *parity_ok = false;
    return first_sync;
}

}  // namespace isdbt
