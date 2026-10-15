#include "deinterleave.h"

#include <cassert>
#include <cmath>
#include <map>
#include <mutex>

#include "pilots.h"

namespace isdbt {

namespace {

// 位相 p ごとの gather 添字: idx[p][s*dc + c] = 等化後行列の列番号
// （周波数デインターリーブ = データキャリア抽出 + セグメント番号順 + ランダマイズ・ローテーション解除）
struct GatherTable {
    std::vector<int> idx[kSpSymbolPeriod];
};

const GatherTable& GatherOf(Mode mode) {
    static std::map<int, GatherTable> cache;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(static_cast<int>(mode));
    if (it != cache.end()) return it->second;

    const Layout& lo = LayoutOf(mode);
    const int dc = lo.dc;
    int n_rand = 0;
    const int16_t* perm = CarrierRandomizeTable(mode, &n_rand);
    GatherTable gt;
    for (int p = 0; p < kSpSymbolPeriod; ++p) {
        gt.idx[p].resize(static_cast<size_t>(kNSegments) * dc);
        for (int s = 0; s < kNSegments; ++s) {
            const int f = kSegmentFreqIndex[s];
            for (int c = 0; c < dc; ++c) {
                int rot = (c - s) % dc;
                if (rot < 0) rot += dc;
                gt.idx[p][static_cast<size_t>(s) * dc + c] = lo.dpos4[p][static_cast<size_t>(f) * dc + perm[rot]];
            }
        }
    }
    return cache.emplace(static_cast<int>(mode), std::move(gt)).first->second;
}

}  // namespace

template <typename T>
Mat<T> DataToLogical(const Mat<T>& D, Mode mode, int frame_start) {
    const GatherTable& gt = GatherOf(mode);
    const size_t n_cols = gt.idx[0].size();
    Mat<T> out(D.rows, n_cols);
    for (size_t n = 0; n < D.rows; ++n) {
        int p = (static_cast<int>(n) - frame_start) % kSpSymbolPeriod;
        if (p < 0) p += kSpSymbolPeriod;
        const int* idx = gt.idx[p].data();
        const T* src = D[n];
        T* dst = out[n];
        for (size_t j = 0; j < n_cols; ++j) dst[j] = src[idx[j]];
    }
    return out;
}

template <typename T>
Mat<T> TimeDeinterleave(const Mat<T>& Y, Mode mode, const int I_per_segment[kNSegments]) {
    const int dc = LayoutOf(mode).dc;
    Mat<T> out(Y.rows, Y.cols);
    std::vector<int> delay(dc);
    for (int s = 0; s < kNSegments; ++s) {
        const int I = I_per_segment[s];
        for (int c = 0; c < dc; ++c) {
            delay[c] = I * (kTimeInterleaveMod - 1 - (kTimeInterleaveMult * c) % kTimeInterleaveMod);
        }
        for (size_t n = 0; n < Y.rows; ++n) {
            T* dst = out[n] + static_cast<size_t>(s) * dc;
            for (int c = 0; c < dc; ++c) {
                const long src = static_cast<long>(n) - delay[c];
                dst[c] = (src >= 0) ? Y[static_cast<size_t>(src)][static_cast<size_t>(s) * dc + c] : T{};
            }
        }
    }
    return out;
}

template <typename T>
void IntersegmentDeinterleave(Mat<T>* X, Mode mode, const std::vector<std::vector<int>>& groups) {
    const int dc = LayoutOf(mode).dc;
    std::vector<T> row_copy;
    for (const auto& group : groups) {
        const int n = static_cast<int>(group.size());
        if (n < 2) continue;
        row_copy.resize(static_cast<size_t>(n) * dc);
        for (size_t r = 0; r < X->rows; ++r) {
            T* row = (*X)[r];
            // S[q] = row[group[q mod n]*dc + q div n]、戻し: group[k'] のキャリア c' = S[k'*dc + c']
            for (int q = 0; q < n * dc; ++q) {
                row_copy[q] = row[static_cast<size_t>(group[q % n]) * dc + q / n];
            }
            for (int k = 0; k < n; ++k) {
                T* dst = row + static_cast<size_t>(group[k]) * dc;
                const T* src = row_copy.data() + static_cast<size_t>(k) * dc;
                for (int c = 0; c < dc; ++c) dst[c] = src[c];
            }
        }
    }
}

void DemapSoft(cfloat d, Modulation mod, float* out) {
    const float norm = ConstellationNorm(mod);
    const float i = d.real() * norm;
    const float q = d.imag() * norm;
    switch (mod) {
        case Modulation::DQPSK:
        case Modulation::QPSK:
            out[0] = i;
            out[1] = q;
            return;
        case Modulation::QAM16:
            out[0] = i;
            out[1] = q;
            out[2] = std::abs(i) - 2.0f;
            out[3] = std::abs(q) - 2.0f;
            return;
        case Modulation::QAM64: {
            const float ai = std::abs(i), aq = std::abs(q);
            out[0] = i;
            out[1] = q;
            out[2] = ai - 4.0f;
            out[3] = aq - 4.0f;
            out[4] = std::abs(ai - 4.0f) - 2.0f;
            out[5] = std::abs(aq - 4.0f) - 2.0f;
            return;
        }
    }
}

Mat<float> LayerSoftBits(const Mat<cfloat>& Xt, const Mat<float>& Wt,
                         const std::vector<int>& rows, const std::vector<int>& segments,
                         Modulation mod, Mode mode) {
    const int dc = LayoutOf(mode).dc;
    const int bps = BitsPerSymbol(mod);
    const size_t slots_per_row = segments.size() * static_cast<size_t>(dc);
    const size_t n_slots = rows.size() * slots_per_row;

    // デマップ × CSI 重み（ビットインターリーブ前の直列スロット順）
    Mat<float> soft(n_slots, bps);
    size_t t = 0;
    for (int r : rows) {
        const cfloat* xr = Xt[r];
        const float* wr = Wt[r];
        for (int s : segments) {
            for (int c = 0; c < dc; ++c, ++t) {
                float* o = soft[t];
                DemapSoft(xr[static_cast<size_t>(s) * dc + c], mod, o);
                const float w = wr[static_cast<size_t>(s) * dc + c];
                for (int i = 0; i < bps; ++i) o[i] *= w;
            }
        }
    }

    // ビットデインターリーブ: out[t][i] = in[t − (120 − d_i)][i]
    const int* delays = BitInterleaveDelays(mod);
    Mat<float> out(n_slots, bps);
    for (int i = 0; i < bps; ++i) {
        const size_t d = static_cast<size_t>(kBitInterleaveMaxDelay - delays[i]);
        for (size_t t2 = 0; t2 < n_slots; ++t2) {
            out[t2][i] = (t2 >= d) ? soft[t2 - d][i] : 0.0f;
        }
    }
    return out;
}

std::vector<int8_t> LayerSoftBitsQ(const Mat<cfloat>& Xt, const Mat<float>& Wt,
                                   const std::vector<int>& rows, const std::vector<int>& segments,
                                   Modulation mod, Mode mode, int q) {
    const int dc = LayoutOf(mode).dc;
    const int bps = BitsPerSymbol(mod);
    const size_t slots_per_row = segments.size() * static_cast<size_t>(dc);
    const size_t n_slots = rows.size() * slots_per_row;
    const int* delays = BitInterleaveDelays(mod);
    int d[8] = {};
    for (int i = 0; i < bps; ++i) d[i] = kBitInterleaveMaxDelay - delays[i];

    // 尺度: 61 スロットおきにデマップして |軟値| の平均を取る
    double acc = 0.0;
    size_t cnt = 0;
    {
        float tmp[8];
        size_t t = 0;
        for (int r : rows) {
            const cfloat* xr = Xt[r];
            const float* wr = Wt[r];
            for (int s : segments) {
                for (int c = 0; c < dc; ++c, ++t) {
                    if (t % 61 != 0) continue;
                    DemapSoft(xr[static_cast<size_t>(s) * dc + c], mod, tmp);
                    const float w = wr[static_cast<size_t>(s) * dc + c];
                    for (int i = 0; i < bps; ++i) acc += std::fabs(tmp[i] * w);
                    cnt += static_cast<size_t>(bps);
                }
            }
        }
    }
    const float scale = (acc > 0.0) ? static_cast<float>(12.0 * static_cast<double>(cnt) / acc) : 1.0f;
    const float qmax = static_cast<float>(q), qmin = -static_cast<float>(q);

    std::vector<int8_t> out(n_slots * static_cast<size_t>(bps));
    alignas(32) float ring[128][8];                 // 直近 128 スロットのデマップ値（遅延 ≤ 120）
    size_t t = 0;
    for (int r : rows) {
        const cfloat* xr = Xt[r];
        const float* wr = Wt[r];
        for (int s : segments) {
            for (int c = 0; c < dc; ++c, ++t) {
                float* cur = ring[t & 127];
                DemapSoft(xr[static_cast<size_t>(s) * dc + c], mod, cur);
                const float w = wr[static_cast<size_t>(s) * dc + c] * scale;
                for (int i = 0; i < bps; ++i) cur[i] *= w;
                int8_t* o = out.data() + t * static_cast<size_t>(bps);
                for (int i = 0; i < bps; ++i) {
                    const size_t di = static_cast<size_t>(d[i]);
                    float v = (t >= di) ? ring[(t - di) & 127][i] : 0.0f;
                    v = v > qmax ? qmax : (v < qmin ? qmin : v);
                    o[i] = static_cast<int8_t>(static_cast<int>(v + (v >= 0 ? 0.5f : -0.5f)));
                }
            }
        }
    }
    return out;
}

// 明示的実体化
template Mat<cfloat> DataToLogical<cfloat>(const Mat<cfloat>&, Mode, int);
template Mat<float> DataToLogical<float>(const Mat<float>&, Mode, int);
template Mat<cfloat> TimeDeinterleave<cfloat>(const Mat<cfloat>&, Mode, const int[kNSegments]);
template Mat<float> TimeDeinterleave<float>(const Mat<float>&, Mode, const int[kNSegments]);
template void IntersegmentDeinterleave<cfloat>(Mat<cfloat>*, Mode, const std::vector<std::vector<int>>&);
template void IntersegmentDeinterleave<float>(Mat<float>*, Mode, const std::vector<std::vector<int>>&);

}  // namespace isdbt
