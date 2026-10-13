#include "mapping.h"

#include <cmath>

namespace isdbc {

namespace {

// 第 1 象限の 4 LSB: I のレベル 1,3,5,7 ↔ (b2 b0) の Gray 00,01,11,10、Q ↔ (b3 b1)（図 C.7）
int GrayToLevelIndex(int g) {
    switch (g) {
        case 0b00: return 0;
        case 0b01: return 1;
        case 0b11: return 2;
        default: return 3;      // 0b10
    }
}
constexpr uint8_t kLevelIndexToGray[4] = {0b00, 0b01, 0b11, 0b10};
// 表 1: 象限 2 (10) = +π/2、象限 3 (11) = +π、象限 4 (01) = +3π/2。添字は I*2+Q
const cfloat kQuadrantRotation[4] = {{1, 0}, {0, -1}, {0, 1}, {-1, 0}};

struct Table {
    cfloat c[64];
    Table() {
        for (int w = 0; w < 64; ++w) {
            const int lsb = w & 0x0F;
            const int b3 = (lsb >> 3) & 1, b2 = (lsb >> 2) & 1, b1 = (lsb >> 1) & 1, b0 = lsb & 1;
            const float i = static_cast<float>(2 * GrayToLevelIndex((b2 << 1) | b0) + 1);
            const float q = static_cast<float>(2 * GrayToLevelIndex((b3 << 1) | b1) + 1);
            c[w] = kQuadrantRotation[w >> 4] * cfloat(i, q);
        }
    }
};

const Table& TableOf() {
    static const Table t;
    return t;
}

int DiffStep(int a, int b, int* i_prev, int* q_prev) {
    // I_k = ¬(A⊕B)·(A⊕I') + (A⊕B)·(A⊕Q'),  Q_k = ¬(A⊕B)·(B⊕Q') + (A⊕B)·(B⊕I')
    const int x = a ^ b, nx = 1 - x;
    const int i = (nx & (a ^ *i_prev)) | (x & (a ^ *q_prev));
    const int q = (nx & (b ^ *q_prev)) | (x & (b ^ *i_prev));
    *i_prev = i;
    *q_prev = q;
    return (i << 1) | q;
}

}  // namespace

const cfloat* Constellation() { return TableOf().c; }

uint8_t DemapHard(cfloat z) {
    const int i_bit = z.real() < 0.0f;
    const int q_bit = z.imag() < 0.0f;
    const int iq = i_bit * 2 + q_bit;
    const cfloat z1 = z * std::conj(kQuadrantRotation[iq]);
    // レベル 1,3,5,7 → 添字 0..3（(|v| − 1)/2 を四捨五入。非負に飽和させてから切り捨て = 四捨五入）
    const float fi = std::min(3.0f, std::max(0.0f, (std::fabs(z1.real()) - 1.0f) * 0.5f + 0.5f));
    const float fq = std::min(3.0f, std::max(0.0f, (std::fabs(z1.imag()) - 1.0f) * 0.5f + 0.5f));
    const int li = static_cast<int>(fi);
    const int lq = static_cast<int>(fq);
    const int gi = kLevelIndexToGray[li], gq = kLevelIndexToGray[lq];
    return static_cast<uint8_t>((iq << 4) | ((gq >> 1) << 3) | ((gi >> 1) << 2) | ((gq & 1) << 1) | (gi & 1));
}

void DiffEncode(const uint8_t* words, size_t n, uint8_t* out, int i_prev, int q_prev) {
    for (size_t k = 0; k < n; ++k) {
        const int a = (words[k] >> 5) & 1, b = (words[k] >> 4) & 1;
        const int iq = DiffStep(a, b, &i_prev, &q_prev);
        out[k] = static_cast<uint8_t>((iq << 4) | (words[k] & 0x0F));
    }
}

void BytesToMtuples(const uint8_t* bytes, size_t n_bytes, uint8_t* words) {
    size_t w = 0;
    for (size_t i = 0; i + 2 < n_bytes; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(bytes[i]) << 16) | (static_cast<uint32_t>(bytes[i + 1]) << 8) |
                           bytes[i + 2];
        words[w++] = static_cast<uint8_t>((v >> 18) & 63);
        words[w++] = static_cast<uint8_t>((v >> 12) & 63);
        words[w++] = static_cast<uint8_t>((v >> 6) & 63);
        words[w++] = static_cast<uint8_t>(v & 63);
    }
}

}  // namespace isdbc
