#include "fec.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace isdbt {

// ---------------------------------------------------------------------------
// GF(2^8)（原始多項式 x^8+x^4+x^3+x^2+1）
// ---------------------------------------------------------------------------
namespace {

struct GfTables {
    uint8_t exp[512];
    uint8_t log[256];
    GfTables() {
        int x = 1;
        log[0] = 0;
        for (int i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= kRsPrimPoly;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
};
const GfTables g_gf;

inline int GMul(int a, int b) {
    if (a == 0 || b == 0) return 0;
    return g_gf.exp[g_gf.log[a] + g_gf.log[b]];
}
inline int GInv(int a) { return g_gf.exp[(255 - g_gf.log[a]) % 255]; }
inline int GPow2(int k) { return g_gf.exp[k % 255]; }   // α^k（k ≥ 0）

}  // namespace

const uint8_t* GfExpTable() { return g_gf.exp; }
const uint8_t* GfLogTable() { return g_gf.log; }

// ---------------------------------------------------------------------------
// デパンクチャ
// ---------------------------------------------------------------------------
size_t Depuncture(const float* soft, size_t n, CodeRate rate, std::vector<float>* pairs) {
    const PuncturePattern* pat = FindPuncture(rate);
    assert(pat);
    const size_t period = std::strlen(pat->x);
    size_t sent = 0;
    for (size_t i = 0; i < period; ++i) sent += (pat->x[i] == '1') + (pat->y[i] == '1');
    const size_t blocks = n / sent;
    pairs->assign(blocks * period * 2, 0.0f);
    float* out = pairs->data();
    size_t s = 0;
    for (size_t blk = 0; blk < blocks; ++blk) {
        for (size_t i = 0; i < period; ++i) {
            if (pat->x[i] == '1') out[(blk * period + i) * 2 + 0] = soft[s++];
            if (pat->y[i] == '1') out[(blk * period + i) * 2 + 1] = soft[s++];
        }
    }
    return blocks * period;
}

// ---------------------------------------------------------------------------
// Viterbi
// ---------------------------------------------------------------------------
namespace {
inline int Parity(unsigned v) {
#ifdef _MSC_VER
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return v & 1;
#else
    return __builtin_parity(v);
#endif
}
}  // namespace

ViterbiDecoder::ViterbiDecoder() {
    // 枝メトリック sx*X + sy*Y は 4 通りしか無い: index = (sx<0)*2 + (sy<0)
    for (int s_new = 0; s_new < kStates; ++s_new) {
        int u = s_new >> 5;
        for (int b = 0; b < 2; ++b) {
            int s_old = ((s_new & 0x1F) << 1) | b;
            unsigned r = (u << 6) | s_old;      // bit6 = 現在入力 … bit0 = 6 個前
            int sx_neg = Parity(r & kConvG1);   // 符号器出力 1 → 受信軟値は負が一致
            int sy_neg = Parity(r & kConvG2);
            bm_index_[b][s_new] = static_cast<uint8_t>(sx_neg * 2 + sy_neg);
            sx_[b][s_new] = sx_neg ? -1.0f : 1.0f;
            sy_[b][s_new] = sy_neg ? -1.0f : 1.0f;
        }
    }
    Reset();
}

void ViterbiDecoder::Reset() {
    pm_.fill(0.0f);
    dec_.clear();
    steps_since_norm_ = 0;
}

void ViterbiDecoder::Feed(const float* pairs, size_t n_steps, std::vector<uint8_t>* out) {
#ifdef __AVX2__
    FeedAvx2(pairs, n_steps, out);
#else
    FeedScalar(pairs, n_steps, out);
#endif
}

void ViterbiDecoder::FeedScalar(const float* pairs, size_t n_steps, std::vector<uint8_t>* out) {
    for (size_t t = 0; t < n_steps; ++t) {
        const float X = pairs[2 * t];
        const float Y = pairs[2 * t + 1];
        const float bm[4] = {X + Y, X - Y, -X + Y, -X - Y};
        std::array<float, kStates> np;
        uint64_t word = 0;
        for (int s = 0; s < kStates; ++s) {
            const float c0 = pm_[(s & 0x1F) << 1] + bm[bm_index_[0][s]];
            const float c1 = pm_[((s & 0x1F) << 1) | 1] + bm[bm_index_[1][s]];
            const bool pick1 = c1 > c0;
            np[s] = pick1 ? c1 : c0;
            word |= static_cast<uint64_t>(pick1) << s;
        }
        pm_ = np;
        dec_.push_back(word);
        if (++steps_since_norm_ >= 4096) {
            float m = *std::max_element(pm_.begin(), pm_.end());
            for (float& p : pm_) p -= m;
            steps_since_norm_ = 0;
        }
        if (dec_.size() >= kWindow + kTail) Traceback(kWindow, out);
    }
}

#ifdef __AVX2__
// FeedScalar と同じ遷移を 8 状態 × 8 ベクトルで並列計算する。
// 新 state s の前者は old 2(s&31) と 2(s&31)+1 なので、旧メトリックを偶数/奇数に分離して使う。
void ViterbiDecoder::FeedAvx2(const float* pairs, size_t n_steps, std::vector<uint8_t>* out) {
    __m256 pm[8];
    for (int v = 0; v < 8; ++v) pm[v] = _mm256_load_ps(&pm_[static_cast<size_t>(v) * 8]);
    const __m256i lane_fix = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    for (size_t t = 0; t < n_steps; ++t) {
        const __m256 X = _mm256_set1_ps(pairs[2 * t]);
        const __m256 Y = _mm256_set1_ps(pairs[2 * t + 1]);
        __m256 A[4], B[4];      // A[k] = old[2j], B[k] = old[2j+1]（j = 8k..8k+7）
        for (int k = 0; k < 4; ++k) {
            const __m256 lo = pm[2 * k];
            const __m256 hi = pm[2 * k + 1];
            const __m256 ev = _mm256_shuffle_ps(lo, hi, _MM_SHUFFLE(2, 0, 2, 0));
            const __m256 od = _mm256_shuffle_ps(lo, hi, _MM_SHUFFLE(3, 1, 3, 1));
            A[k] = _mm256_permutevar8x32_ps(ev, lane_fix);
            B[k] = _mm256_permutevar8x32_ps(od, lane_fix);
        }
        uint64_t word = 0;
        for (int v = 0; v < 8; ++v) {
            const int k = v & 3;
            const __m256 bm0 = _mm256_fmadd_ps(_mm256_load_ps(&sx_[0][v * 8]), X,
                                               _mm256_mul_ps(_mm256_load_ps(&sy_[0][v * 8]), Y));
            const __m256 bm1 = _mm256_fmadd_ps(_mm256_load_ps(&sx_[1][v * 8]), X,
                                               _mm256_mul_ps(_mm256_load_ps(&sy_[1][v * 8]), Y));
            const __m256 c0 = _mm256_add_ps(A[k], bm0);
            const __m256 c1 = _mm256_add_ps(B[k], bm1);
            pm[v] = _mm256_max_ps(c0, c1);
            const __m256 pick = _mm256_cmp_ps(c1, c0, _CMP_GT_OQ);
            word |= static_cast<uint64_t>(static_cast<uint32_t>(_mm256_movemask_ps(pick))) << (v * 8);
        }
        dec_.push_back(word);
        if (++steps_since_norm_ >= 4096) {
            __m256 m = pm[0];
            for (int v = 1; v < 8; ++v) m = _mm256_max_ps(m, pm[v]);
            __m128 m4 = _mm_max_ps(_mm256_castps256_ps128(m), _mm256_extractf128_ps(m, 1));
            m4 = _mm_max_ps(m4, _mm_movehl_ps(m4, m4));
            m4 = _mm_max_ss(m4, _mm_shuffle_ps(m4, m4, 1));
            const __m256 mv = _mm256_set1_ps(_mm_cvtss_f32(m4));
            for (int v = 0; v < 8; ++v) pm[v] = _mm256_sub_ps(pm[v], mv);
            steps_since_norm_ = 0;
        }
        if (dec_.size() >= kWindow + kTail) {
            for (int v = 0; v < 8; ++v) _mm256_store_ps(&pm_[static_cast<size_t>(v) * 8], pm[v]);
            Traceback(kWindow, out);
        }
    }
    for (int v = 0; v < 8; ++v) _mm256_store_ps(&pm_[static_cast<size_t>(v) * 8], pm[v]);
}
#endif

void ViterbiDecoder::Flush(std::vector<uint8_t>* out) {
    if (!dec_.empty()) Traceback(dec_.size(), out);
    Reset();
}

// 現在の最良 state から dec_ 全体を遡り、先頭 n_emit ビットを out へ。残りは次回に持ち越す。
void ViterbiDecoder::Traceback(size_t n_emit, std::vector<uint8_t>* out) {
    const size_t n = dec_.size();
    int state = static_cast<int>(std::max_element(pm_.begin(), pm_.end()) - pm_.begin());
    std::vector<uint8_t> bits(n);
    for (size_t t = n; t-- > 0;) {
        bits[t] = static_cast<uint8_t>(state >> 5);
        int b = static_cast<int>((dec_[t] >> state) & 1);
        state = ((state & 0x1F) << 1) | b;
    }
    out->insert(out->end(), bits.begin(), bits.begin() + n_emit);
    dec_.erase(dec_.begin(), dec_.begin() + n_emit);
}

std::vector<uint8_t> ViterbiDecode(const float* pairs, size_t n_steps) {
    ViterbiDecoder dec;
    std::vector<uint8_t> bits;
    bits.reserve(n_steps);
    dec.Feed(pairs, n_steps, &bits);
    dec.Flush(&bits);
    return bits;
}

// ---------------------------------------------------------------------------
// バイトデインターリーブ
// ---------------------------------------------------------------------------
void ByteDeinterleave(const uint8_t* in, size_t n_tsp, uint8_t* out) {
    for (size_t t = 0; t < n_tsp; ++t) {
        for (int j = 0; j < kRsN; ++j) {
            int b = j % kByteInterleaveBranches;
            // 受信側はブランチ b を 11 - b TSP 遅延: out[t][j] = in[t + b - 11][j]
            long src = static_cast<long>(t) + b - (kByteInterleaveBranches - 1);
            out[t * kRsN + j] = (src >= 0) ? in[static_cast<size_t>(src) * kRsN + j] : 0;
        }
    }
}

// ---------------------------------------------------------------------------
// エネルギー逆拡散
// ---------------------------------------------------------------------------
void EnergyDispersalPrbs(size_t n_bytes, uint8_t* out) {
    // reg[i]（i=0 が段 1）を整数のビット i に対応させる。初期値は "100101010000000" を先頭から
    unsigned reg = 0;
    for (int i = 0; i < 15; ++i) {
        reg |= ((kEnergyDispersalInit >> (14 - i)) & 1u) << i;
    }
    for (size_t i = 0; i < n_bytes; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 8; ++k) {
            unsigned fb = ((reg >> 13) ^ (reg >> 14)) & 1u;    // 段 14 と段 15 の XOR
            v = (v << 1) | fb;
            reg = ((reg << 1) | fb) & 0x7FFF;
        }
        out[i] = static_cast<uint8_t>(v);
    }
}

void EnergyDescrambleLayer(const uint8_t* onair, size_t n_tsp, int tsp_per_frame, uint8_t* out) {
    assert(n_tsp % tsp_per_frame == 0);
    std::vector<uint8_t> prbs(static_cast<size_t>(tsp_per_frame) * kRsN);
    for (size_t f = 0; f < n_tsp / tsp_per_frame; ++f) {
        EnergyDispersalPrbs(prbs.size(), prbs.data());      // フレーム先頭で初期化
        for (int t = 0; t < tsp_per_frame; ++t) {
            const uint8_t* src = onair + (f * tsp_per_frame + t) * kRsN;
            const uint8_t* pr = prbs.data() + static_cast<size_t>(t) * kRsN;
            uint8_t* dst = out + (f * tsp_per_frame + t) * kRsN;
            dst[0] = src[kRsN - 1];                          // 同期バイトは末尾に居る
            for (int j = 0; j < kRsN - 1; ++j) dst[1 + j] = src[j] ^ pr[j];
        }
    }
}

// ---------------------------------------------------------------------------
// RS(204,188) 復号
// ---------------------------------------------------------------------------
namespace {

// 1 TSP のシンドローム S_i = c(α^i)。誤りなしなら全 0。戻り値: どれかが非 0 か
bool Syndromes(const uint8_t* c, int S[kRsNRoots]) {
    bool any = false;
    for (int i = 0; i < kRsNRoots; ++i) {
        int root = GPow2(kRsFirstRoot + i);
        int acc = 0;
        for (int j = 0; j < kRsN; ++j) acc = GMul(acc, root) ^ c[j];
        S[i] = acc;
        any = any || acc != 0;
    }
    return any;
}

int PolyEval(const std::vector<int>& poly, int x) {
    int acc = 0;
    for (size_t i = poly.size(); i-- > 0;) acc = GMul(acc, x) ^ poly[i];
    return acc;
}

// Berlekamp-Massey + Chien + Forney。訂正バイト数を返す（訂正不能は -1）。ref/isdbt/fec.py の移植
int DecodeOne(uint8_t* c, const int S[kRsNRoots]) {
    const int t2 = kRsNRoots;
    std::vector<int> C = {1}, B = {1};
    int L = 0, m = 1, b = 1;
    for (int n = 0; n < t2; ++n) {
        int d = S[n];
        for (int i = 1; i <= L && i < static_cast<int>(C.size()); ++i) d ^= GMul(C[i], S[n - i]);
        if (d == 0) { ++m; continue; }
        std::vector<int> T = C;
        int coef = GMul(d, GInv(b));
        size_t need = B.size() + m;
        if (C.size() < need) C.resize(need, 0);
        for (size_t i = 0; i < B.size(); ++i) C[i + m] ^= GMul(coef, B[i]);
        if (2 * L <= n) {
            L = n + 1 - L;
            B = T;
            b = d;
            m = 1;
        } else {
            ++m;
        }
    }
    if (L > t2 / 2) return -1;
    C.resize(L + 1, 0);
    // Chien 探索: バイト位置 j の根は X_j = α^(203-j)、Λ(X_j^-1) = 0
    std::vector<int> pos;
    for (int j = 0; j < kRsN; ++j) {
        if (PolyEval(C, GPow2((255 - (kRsN - 1 - j)) % 255)) == 0) pos.push_back(j);
    }
    if (static_cast<int>(pos.size()) != L) return -1;
    // Ω(x) = S(x) Λ(x) mod x^2t、Forney: e_j = X_j Ω(X_j^-1) / Λ'(X_j^-1)（fcr = 0）
    std::vector<int> Om(t2, 0);
    for (int i = 0; i < t2; ++i) {
        for (size_t k = 0; k < C.size(); ++k) {
            if (i + static_cast<int>(k) < t2) Om[i + k] ^= GMul(S[i], C[k]);
        }
    }
    for (int j : pos) {
        int X = GPow2(kRsN - 1 - j);
        int Xinv = GInv(X);
        int num = GMul(X, PolyEval(Om, Xinv));
        int den = 0;
        int xpow = 1;                       // Xinv^(i-1)、i = 1, 3, 5, ...
        for (size_t i = 1; i < C.size(); i += 2) {
            den ^= GMul(C[i], xpow);
            xpow = GMul(xpow, GMul(Xinv, Xinv));
        }
        if (den == 0) return -1;
        c[j] = static_cast<uint8_t>(c[j] ^ GMul(num, GInv(den)));
    }
    return L;
}

}  // namespace

void RsDecode(uint8_t* tsps, size_t n_tsp, int* nerr) {
    for (size_t i = 0; i < n_tsp; ++i) {
        uint8_t* c = tsps + i * kRsN;
        int S[kRsNRoots];
        if (!Syndromes(c, S)) {
            nerr[i] = 0;
            continue;
        }
        std::vector<uint8_t> saved(c, c + kRsN);
        int r = DecodeOne(c, S);
        if (r >= 0 && Syndromes(c, S)) r = -1;      // 訂正後もシンドロームが残る = 誤訂正
        if (r < 0) std::copy(saved.begin(), saved.end(), c);
        nerr[i] = r;
    }
}

// ---------------------------------------------------------------------------
// ビット詰め
// ---------------------------------------------------------------------------
void PackBits(const uint8_t* bits, size_t n_bits, uint8_t* bytes) {
    size_t n_bytes = n_bits / 8;
    for (size_t i = 0; i < n_bytes; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 1) | (bits[i * 8 + k] & 1);
        bytes[i] = static_cast<uint8_t>(v);
    }
}

}  // namespace isdbt
