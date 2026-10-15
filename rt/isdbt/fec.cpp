#include "fec.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
// 8bit 計量の AVX2 ビタビ（デパンクチャ融合、ライブ経路用）
// ---------------------------------------------------------------------------
// 64 状態を 8bit 符号なし「コスト」× 32 レーン × 2 ベクタで持つ。軟値は ±31 に量子化し、枝コストは
// (31 − sx·X) + (31 − sy·Y) ∈ [0, 124]。毎 2 ステップで最小コストを引いて 0 に戻すので、最良経路から
// 255 以上劣る経路だけが飽和する（そういう経路は生き残らないので判定に影響しない）。
// 判定ビットは cmpeq + movemask で 32 レーン分がそのまま取れ、16bit 版（packs で詰め直し）より速い。
#ifdef __AVX2__
namespace {

class ViterbiU8 {
public:
    static constexpr int kStates = 64;
    static constexpr size_t kWindow = 1 << 16;
    static constexpr size_t kTail = 192;
    static constexpr int kQ = kViterbiSoftQ;        // 軟値の量子化幅（±kQ）

    ViterbiU8() {
        // 枝コスト表 {k=0: (Q−X)+(Q−Y), 1: (Q−X)+(Q+Y), 2: (Q+X)+(Q−Y), 3: (Q+X)+(Q+Y)} を pshufb で引く添字。
        // k = sx_neg*2 + sy_neg（FeedScalar の bm_index と同じ定義）
        for (int s_new = 0; s_new < kStates; ++s_new) {
            const int u = s_new >> 5;
            for (int b = 0; b < 2; ++b) {
                const int s_old = ((s_new & 0x1F) << 1) | b;
                const unsigned r = static_cast<unsigned>((u << 6) | s_old);
                const int k = Parity(r & kConvG1) * 2 + Parity(r & kConvG2);
                shuf_[b][s_new >> 5][s_new & 31] = static_cast<uint8_t>(k);
            }
        }
        for (uint8_t& c : pm_) c = 0;
        // 枝コスト表: (X, Y) ∈ [−Q, Q]² → {k=0..3} の 4 バイト（ステップごとの組み立てを表引き 1 回にする）
        for (int x = -kQ; x <= kQ; ++x) {
            for (int y = -kQ; y <= kQ; ++y) {
                const uint32_t t0 = static_cast<uint32_t>((kQ - x) + (kQ - y));
                const uint32_t t1 = static_cast<uint32_t>((kQ - x) + (kQ + y));
                const uint32_t t2 = static_cast<uint32_t>((kQ + x) + (kQ - y));
                const uint32_t t3 = static_cast<uint32_t>((kQ + x) + (kQ + y));
                tab2d_[(x + kQ) * (2 * kQ + 1) + (y + kQ)] = t0 | (t1 << 8) | (t2 << 16) | (t3 << 24);
            }
        }
    }

    double t_scale = 0.0, t_tb = 0.0;       // ISDBT_PROFILE 用
    void Run(const float* soft, size_t n, const PuncturePattern* pat, std::vector<uint8_t>* out) {
        const auto tq0 = std::chrono::steady_clock::now();
        // 量子化: 平均振幅を 12 に（±31 で飽和 = 平均の約 2.6 倍）
        double acc = 0.0;
        {
            const __m256 absmask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
            size_t i = 0;
            for (; i + 4096 <= n; i += 4096) {          // 4096 値ごとに double へ足す（float 累積の丸めを抑える）
                __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
                for (size_t k = 0; k < 4096; k += 16) {
                    a0 = _mm256_add_ps(a0, _mm256_and_ps(_mm256_loadu_ps(soft + i + k), absmask));
                    a1 = _mm256_add_ps(a1, _mm256_and_ps(_mm256_loadu_ps(soft + i + k + 8), absmask));
                }
                alignas(32) float tmp[8];
                _mm256_store_ps(tmp, _mm256_add_ps(a0, a1));
                for (float v : tmp) acc += v;
            }
            for (; i < n; ++i) acc += std::fabs(soft[i]);
        }
        const float scale = (acc > 0.0) ? static_cast<float>(12.0 * n / acc) : 1.0f;
        // 一括量子化（32 値ずつ: 乗算 → 丸めて int32 → int16 → int8 に飽和 → ±Q にクランプ）
        std::vector<int8_t> qs(n + 32);
        {
            const __m256 sc = _mm256_set1_ps(scale);
            const __m256i qmax = _mm256_set1_epi8(static_cast<char>(kQ)), qmin = _mm256_set1_epi8(static_cast<char>(-kQ));
            const __m256i fix = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);
            size_t i = 0;
            for (; i + 32 <= n; i += 32) {
                const __m256i a = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(soft + i), sc));
                const __m256i b = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(soft + i + 8), sc));
                const __m256i c = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(soft + i + 16), sc));
                const __m256i d = _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_loadu_ps(soft + i + 24), sc));
                // packs は 128bit レーン内で詰めるので、最後に 32bit 単位で並べ直す
                const __m256i ab = _mm256_packs_epi32(a, b);           // [a0-3 b0-3 | a4-7 b4-7]（16bit）
                const __m256i cd = _mm256_packs_epi32(c, d);
                __m256i v = _mm256_packs_epi16(ab, cd);                // [a0-3 b0-3 c0-3 d0-3 | a4-7 b4-7 c4-7 d4-7]
                v = _mm256_permutevar8x32_epi32(v, fix);               // → a0-3 a4-7 b0-3 b4-7 c... d...
                v = _mm256_min_epi8(_mm256_max_epi8(v, qmin), qmax);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(&qs[i]), v);
            }
            for (; i < n; ++i) {
                const float x = soft[i] * scale;
                int r = static_cast<int>(x + (x >= 0 ? 0.5f : -0.5f));
                r = r > kQ ? kQ : (r < -kQ ? -kQ : r);
                qs[i] = static_cast<int8_t>(r);
            }
        }
        t_scale += std::chrono::duration<double>(std::chrono::steady_clock::now() - tq0).count();
        RunQ(qs.data(), n, pat, out);
    }

    // 量子化済みの軟値列（±kQ の int8）を復号する本体
    void RunQ(const int8_t* qp, size_t n, const PuncturePattern* pat, std::vector<uint8_t>* out) {
        const size_t period = std::strlen(pat->x);
        size_t sent = 0;
        for (size_t i = 0; i < period; ++i) sent += (pat->x[i] == '1') + (pat->y[i] == '1');
        const size_t blocks = n / sent;

        __m256i pm0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&pm_[0]));
        __m256i pm1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(&pm_[32]));
        const __m256i deint_sh = _mm256_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15,
                                                  0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15);
        const __m256i sh00 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shuf_[0][0]));
        const __m256i sh01 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shuf_[0][1]));
        const __m256i sh10 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shuf_[1][0]));
        const __m256i sh11 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shuf_[1][1]));
        const __m256i zero = _mm256_setzero_si256();
        int since_norm = 0;
        __m256i norm_v = zero;                  // 前ステップのコストの最小値（全レーン）
        size_t s = 0;
        for (size_t blk = 0; blk < blocks; ++blk) {
            for (size_t i = 0; i < period; ++i) {
                const int X = (pat->x[i] == '1') ? qp[s++] : 0;
                const int Y = (pat->y[i] == '1') ? qp[s++] : 0;
                const __m256i tab = _mm256_set1_epi32(static_cast<int>(tab2d_[(X + kQ) * (2 * kQ + 1) + (Y + kQ)]));
                // 旧コストを偶数 state（A = old[2j]）と奇数 state（B = old[2j+1]）に分離（j = 0..31）
                const __m256i a = _mm256_shuffle_epi8(pm0, deint_sh);      // 128bit レーンごとに [偶 8 | 奇 8]
                const __m256i b = _mm256_shuffle_epi8(pm1, deint_sh);
                const __m256i A = _mm256_permute4x64_epi64(_mm256_unpacklo_epi64(a, b), 0xD8);
                const __m256i B = _mm256_permute4x64_epi64(_mm256_unpackhi_epi64(a, b), 0xD8);
                // 新 state 0..31（u=0）と 32..63（u=1）。どちらも前者は同じ (A, B)、枝コストだけ違う
                const __m256i c00 = _mm256_adds_epu8(A, _mm256_shuffle_epi8(tab, sh00));
                const __m256i c10 = _mm256_adds_epu8(B, _mm256_shuffle_epi8(tab, sh10));
                const __m256i c01 = _mm256_adds_epu8(A, _mm256_shuffle_epi8(tab, sh01));
                const __m256i c11 = _mm256_adds_epu8(B, _mm256_shuffle_epi8(tab, sh11));
                pm0 = _mm256_min_epu8(c00, c10);
                pm1 = _mm256_min_epu8(c01, c11);
                // pick1 = c1 < c0 ⇔ c0 − c1 > 0（飽和減算）⇔ NOT (c0 −sat c1 == 0)
                const uint32_t le0 = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_subs_epu8(c00, c10), zero)));
                const uint32_t le1 = static_cast<uint32_t>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_subs_epu8(c01, c11), zero)));
                dec_.push_back(static_cast<uint64_t>(static_cast<uint32_t>(~le0)) | (static_cast<uint64_t>(static_cast<uint32_t>(~le1)) << 32));
                // 正規化: 2 ステップで最大 248 増えるので 2 ステップごとに最小値を引く。最小値は 1 ステップ前の
                // コスト（min_prev）から求めておき、今のコストから引く（フォールドの遅延を ACS と重ねる）。
                // 最良経路のコストは min_prev + 124 以下、その他は飽和しても判定に影響しない
                ++since_norm;
                if (since_norm == 1) {
                    __m256i m = _mm256_min_epu8(pm0, pm1);
                    __m128i m8 = _mm_min_epu8(_mm256_castsi256_si128(m), _mm256_extracti128_si256(m, 1));
                    m8 = _mm_min_epu8(m8, _mm_srli_si128(m8, 8));
                    m8 = _mm_min_epu8(m8, _mm_srli_si128(m8, 4));
                    m8 = _mm_min_epu8(m8, _mm_srli_si128(m8, 2));
                    m8 = _mm_min_epu8(m8, _mm_srli_si128(m8, 1));
                    norm_v = _mm256_broadcastb_epi8(m8);
                } else {
                    pm0 = _mm256_subs_epu8(pm0, norm_v);
                    pm1 = _mm256_subs_epu8(pm1, norm_v);
                    since_norm = 0;
                }
                if (dec_.size() >= kWindow + kTail) {
                    _mm256_store_si256(reinterpret_cast<__m256i*>(&pm_[0]), pm0);
                    _mm256_store_si256(reinterpret_cast<__m256i*>(&pm_[32]), pm1);
                    Traceback(kWindow, out);
                }
            }
        }
        _mm256_store_si256(reinterpret_cast<__m256i*>(&pm_[0]), pm0);
        _mm256_store_si256(reinterpret_cast<__m256i*>(&pm_[32]), pm1);
        if (!dec_.empty()) Traceback(dec_.size(), out);
    }

private:
    void Traceback(size_t n_emit, std::vector<uint8_t>* out) {
        const auto tt0 = std::chrono::steady_clock::now();
        const size_t n = dec_.size();
        int state = static_cast<int>(std::min_element(pm_, pm_ + kStates) - pm_);   // コスト最小
        std::vector<uint8_t> bits(n);
        for (size_t t = n; t-- > 0;) {
            bits[t] = static_cast<uint8_t>(state >> 5);
            const int b = static_cast<int>((dec_[t] >> state) & 1);
            state = ((state & 0x1F) << 1) | b;
        }
        out->insert(out->end(), bits.begin(), bits.begin() + n_emit);
        dec_.erase(dec_.begin(), dec_.begin() + n_emit);
        t_tb += std::chrono::duration<double>(std::chrono::steady_clock::now() - tt0).count();
    }

    alignas(32) uint8_t pm_[kStates];
    alignas(32) uint8_t shuf_[2][2][32];
    uint32_t tab2d_[(2 * kQ + 1) * (2 * kQ + 1)];
    std::vector<uint64_t> dec_;
};

}  // namespace
#endif  // __AVX2__

std::vector<uint8_t> ViterbiDecodeQ(const int8_t* qs, size_t n, CodeRate rate) {
    const PuncturePattern* pat = FindPuncture(rate);
    assert(pat);
    (void)pat;
#ifdef __AVX2__
    std::vector<uint8_t> bits;
    ViterbiU8 dec;
    dec.RunQ(qs, n, pat, &bits);
    return bits;
#else
    std::vector<float> soft(n);
    for (size_t i = 0; i < n; ++i) soft[i] = static_cast<float>(qs[i]);
    std::vector<float> pairs;
    const size_t steps = Depuncture(soft.data(), n, rate, &pairs);
    return ViterbiDecode(pairs.data(), steps);
#endif
}

std::vector<uint8_t> ViterbiDecodeSoft(const float* soft, size_t n, CodeRate rate) {
    const PuncturePattern* pat = FindPuncture(rate);
    assert(pat);
    (void)pat;
#ifdef __AVX2__
    std::vector<uint8_t> bits;
    ViterbiU8 dec;
    const auto tr0 = std::chrono::steady_clock::now();
    dec.Run(soft, n, pat, &bits);
    static const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    if (profile && n > 1000000) {
        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - tr0).count();
        std::fprintf(stderr, "[viterbi] total %.3f scale %.3f traceback %.3f acs %.3f (%zu bits)\n", total, dec.t_scale, dec.t_tb, total - dec.t_scale - dec.t_tb, bits.size());
    }
    return bits;
#else
    std::vector<float> pairs;
    const size_t steps = Depuncture(soft, n, rate, &pairs);
    return ViterbiDecode(pairs.data(), steps);
#endif
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

namespace {

// 1 TSP を訂正する（シンドローム計算済み）。訂正不能なら元に戻す
int RsCorrectOne(uint8_t* c, int S[kRsNRoots]) {
    std::vector<uint8_t> saved(c, c + kRsN);
    int r = DecodeOne(c, S);
    if (r >= 0 && Syndromes(c, S)) r = -1;      // 訂正後もシンドロームが残る = 誤訂正
    if (r < 0) std::copy(saved.begin(), saved.end(), c);
    return r;
}

#ifdef __AVX2__
// 32 TSP 同時のシンドローム計算。GF(256) の定数倍 x·α^i をニブル表 2 つの pshufb で求める
// （x = hi·16 + lo → x·r = T_hi[hi] ⊕ T_lo[lo]）。誤りの無い TSP（大半）はこれだけで済み、
// 非 0 の TSP だけスカラーの Berlekamp-Massey へ回す。ホーナー法: S_i = (…(c_0·r_i + c_1)·r_i + …)·r_i + c_203
struct RsSyndromeTables {
    alignas(32) uint8_t lo[kRsNRoots][32];
    alignas(32) uint8_t hi[kRsNRoots][32];
    RsSyndromeTables() {
        for (int i = 0; i < kRsNRoots; ++i) {
            const int root = GPow2(kRsFirstRoot + i);
            for (int x = 0; x < 16; ++x) {
                lo[i][x] = lo[i][x + 16] = static_cast<uint8_t>(GMul(x, root));
                hi[i][x] = hi[i][x + 16] = static_cast<uint8_t>(GMul(x << 4, root));
            }
        }
    }
};

void RsDecodeAvx2(uint8_t* tsps, size_t n_tsp, int* nerr) {
    static const RsSyndromeTables tabs;
    const __m256i nib = _mm256_set1_epi8(0x0F);
    alignas(32) uint8_t col[kRsN][32];          // col[j][lane] = TSP(base + lane) のバイト j
    alignas(32) uint8_t Sv[kRsNRoots][32];
    for (size_t base = 0; base < n_tsp; base += 32) {
        const size_t cnt = std::min<size_t>(32, n_tsp - base);
        // 転置（使わないレーンは 0 → シンドロームも 0）
        for (size_t l = 0; l < 32; ++l) {
            if (l < cnt) {
                const uint8_t* c = tsps + (base + l) * kRsN;
                for (int j = 0; j < kRsN; ++j) col[j][l] = c[j];
            } else {
                for (int j = 0; j < kRsN; ++j) col[j][l] = 0;
            }
        }
        __m256i any = _mm256_setzero_si256();
        for (int i = 0; i < kRsNRoots; ++i) {
            const __m256i tlo = _mm256_load_si256(reinterpret_cast<const __m256i*>(tabs.lo[i]));
            const __m256i thi = _mm256_load_si256(reinterpret_cast<const __m256i*>(tabs.hi[i]));
            __m256i acc = _mm256_load_si256(reinterpret_cast<const __m256i*>(col[0]));
            for (int j = 1; j < kRsN; ++j) {
                const __m256i lo = _mm256_and_si256(acc, nib);
                const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(acc, 4), nib);
                const __m256i prod = _mm256_xor_si256(_mm256_shuffle_epi8(tlo, lo), _mm256_shuffle_epi8(thi, hi));
                acc = _mm256_xor_si256(prod, _mm256_load_si256(reinterpret_cast<const __m256i*>(col[j])));
            }
            _mm256_store_si256(reinterpret_cast<__m256i*>(Sv[i]), acc);
            any = _mm256_or_si256(any, acc);
        }
        const uint32_t nonzero = static_cast<uint32_t>(
            ~_mm256_movemask_epi8(_mm256_cmpeq_epi8(any, _mm256_setzero_si256())));
        for (size_t l = 0; l < cnt; ++l) {
            if (!((nonzero >> l) & 1u)) {
                nerr[base + l] = 0;
                continue;
            }
            int S[kRsNRoots];
            for (int i = 0; i < kRsNRoots; ++i) S[i] = Sv[i][l];
            nerr[base + l] = RsCorrectOne(tsps + (base + l) * kRsN, S);
        }
    }
}
#endif  // __AVX2__

}  // namespace

void RsDecode(uint8_t* tsps, size_t n_tsp, int* nerr) {
#ifdef __AVX2__
    RsDecodeAvx2(tsps, n_tsp, nerr);
#else
    for (size_t i = 0; i < n_tsp; ++i) {
        uint8_t* c = tsps + i * kRsN;
        int S[kRsNRoots];
        if (!Syndromes(c, S)) {
            nerr[i] = 0;
            continue;
        }
        nerr[i] = RsCorrectOne(c, S);
    }
#endif
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
