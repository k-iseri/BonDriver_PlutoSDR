#include "demod.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>

#include "deinterleave.h"
#include "equalize.h"
#include "fec.h"
#include "pilots.h"

namespace isdbt {

namespace {

constexpr int kEqContextSymbols = 4;    // 等化の平均窓（前後 8 シンボル）のための前後文脈

std::vector<RxLayer> LayersFromTmcc(const TmccInfo& info) {
    static const char* names[3] = {"A", "B", "C"};
    std::vector<RxLayer> layers;
    int nxt = 0;
    for (int i = 0; i < 3; ++i) {
        const TmccLayer& l = info.layers[i];
        if (!l.used || l.segments == 0) continue;
        RxLayer rl;
        rl.name = names[i];
        rl.mod = l.mod;
        rl.rate = l.rate;
        rl.I = l.interleave;
        for (int s = 0; s < l.segments; ++s) rl.segments.push_back(nxt + s);
        nxt += l.segments;
        layers.push_back(std::move(rl));
    }
    if (nxt != kNSegments) throw std::runtime_error("TMCC の階層構成が不正");
    return layers;
}

std::vector<std::vector<int>> InterleaveGroups(bool partial_reception) {
    std::vector<int> partial, coherent;
    if (partial_reception) partial.push_back(0);
    for (int s = 0; s < kNSegments; ++s) {
        if (!(partial_reception && s == 0)) coherent.push_back(s);
    }
    std::vector<std::vector<int>> groups;
    if (partial.size() >= 2) groups.push_back(partial);
    if (coherent.size() >= 2) groups.push_back(coherent);
    return groups;
}

int CeilDiv(int a, int b) { return (a + b - 1) / b; }

inline float MaxLevelOf(Modulation mod) {
    return mod == Modulation::QAM64 ? 7.0f : (mod == Modulation::QAM16 ? 3.0f : 1.0f);
}
// 最近傍の奇数レベルへスライス
inline float SliceLevel(float v, float max_level) {
    const float lv = 2.0f * std::floor(v / 2.0f) + 1.0f;
    return std::clamp(lv, -max_level, max_level);
}

// 最近傍コンスタレーション点との誤差を積算する（MER 用）
void AccumulateMer(const Mat<cfloat>& Xt, const std::vector<int>& rows,
                   const std::vector<int>& segments, Modulation mod, int dc,
                   double* sig, double* err, const Mat<float>* W) {
    const float norm = ConstellationNorm(mod);
    const float max_level = MaxLevelOf(mod);
    for (int r : rows) {
        const cfloat* row = Xt[r];
        const float* wrow = W ? (*W)[r] : nullptr;
        for (int s : segments) {
            const cfloat* p = row + static_cast<size_t>(s) * dc;
            const float* w = wrow ? wrow + static_cast<size_t>(s) * dc : nullptr;
            for (int c = 0; c < dc; ++c) {
                if (w && w[c] == 0.0f) continue;        // 消失扱い（干渉・DC スパー）のキャリアは MER に含めない
                const float i = p[c].real() * norm;
                const float q = p[c].imag() * norm;
                const float si = SliceLevel(i, max_level), sq = SliceLevel(q, max_level);
                *err += (i - si) * (i - si) + (q - sq) * (q - sq);
                *sig += si * si + sq * sq;
            }
        }
    }
}

// 狭帯域妨害（LO リークの残り等）が常時乗っているキャリアの CSI を 0 にする。
// 行方向は中央値（録音先頭の時間インターリーブ無音区間を誤検出しないため）。
// アルゴリズムの詳細は ref/isdbt/demod.py null_interfered_carriers のコメントを参照
int NullInterferedCarriers(const Mat<cfloat>& D, Mat<float>* csi, Mode mode, int frame_start,
                           const std::vector<RxLayer>& layers) {
    const Layout& lo = LayoutOf(mode);
    const ModeParams& mp = ModeParamsOf(mode);
    const int dc = lo.dc;
    Modulation mod_of[kNSegments];
    bool has_mod[kNSegments] = {};
    for (const RxLayer& lyr : layers) {
        for (int s : lyr.segments) {
            mod_of[kSegmentFreqIndex[s]] = lyr.mod;
            has_mod[kSegmentFreqIndex[s]] = true;
        }
    }
    int nulled = 0;
    std::vector<int> rows;
    std::vector<double> err(static_cast<size_t>(mp.total_carriers));
    for (int p = 0; p < kSpSymbolPeriod; ++p) {
        rows.clear();
        for (int r = 0; r < static_cast<int>(D.rows); ++r) {
            int ph = (r - frame_start) % kSpSymbolPeriod;
            if (ph < 0) ph += kSpSymbolPeriod;
            if (ph == p) rows.push_back(r);
        }
        if (rows.size() < 8) continue;
        std::fill(err.begin(), err.end(), -1.0);
        for (int f = 0; f < kNSegments; ++f) {
            if (!has_mod[f]) continue;
            const Modulation mod = mod_of[f];
            const float norm = ConstellationNorm(mod);
            const float max_level = MaxLevelOf(mod);
            std::vector<double> es(rows.size());
            for (int c = 0; c < dc; ++c) {
                const int k = lo.dpos4[p][static_cast<size_t>(f) * dc + c];
                for (size_t ri = 0; ri < rows.size(); ++ri) {
                    const int r = rows[ri];
                    const float i = D[r][k].real() * norm;
                    const float q = D[r][k].imag() * norm;
                    const float si = SliceLevel(i, max_level), sq = SliceLevel(q, max_level);
                    es[ri] = static_cast<double>(i - si) * (i - si) + static_cast<double>(q - sq) * (q - sq);
                }
                std::nth_element(es.begin(), es.begin() + es.size() / 2, es.end());
                err[static_cast<size_t>(k)] = es[es.size() / 2];
            }
        }
        std::vector<double> valid;
        valid.reserve(err.size());
        for (double v : err) {
            if (v >= 0.0) valid.push_back(v);
        }
        if (valid.empty()) continue;
        std::nth_element(valid.begin(), valid.begin() + valid.size() / 2, valid.end());
        const double med = valid[valid.size() / 2];
        for (int k = 0; k < mp.total_carriers; ++k) {
            if (err[static_cast<size_t>(k)] > 10.0 * med) {
                for (int r : rows) (*csi)[r][k] = 0.0f;
                ++nulled;
            }
        }
    }
    return nulled;
}

}  // namespace

FrameSyncInfo FrameSync(ISymbolSource* src, Mode mode) {
    FrameSyncInfo out;
    const int head = std::min(src->n_sym(), 3 * kSymbolsPerFrame);
    Mat<cfloat> Yh = src->Symbols(0, head);
    std::vector<uint8_t> b = TmccBitsFromCarriers(Yh.data(), Yh.rows, mode);
    out.frame_start = FindFrameStart(b, &out.tmcc_parity_ok);
    if (out.frame_start < 0) throw std::runtime_error("TMCC 同期語が見つからない");
    out.tmcc = ParseTmccInfo(mode, b.data() + out.frame_start + 20);
    out.layers = LayersFromTmcc(out.tmcc);
    out.groups = InterleaveGroups(out.tmcc.partial_reception);
    out.n_frames = (src->n_sym() - out.frame_start) / kSymbolsPerFrame;
    return out;
}

void EqualizeAndDeinterleave(const Mat<cfloat>& Y, Mode mode, int frame_start,
                             const std::vector<RxLayer>& layers,
                             const std::vector<std::vector<int>>& groups,
                             Mat<cfloat>* H_out, Mat<cfloat>* Xt, Mat<float>* Wt, bool fast, int null_center) {
    static const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    const auto t0 = now();
    std::vector<char> bad;
    Mat<cfloat> H = ChannelEstimate(Y, mode, frame_start, 8, &bad);
    const auto t1 = now();
    Mat<cfloat> D;
    Mat<float> csi;
    Equalize(Y, H, &D, &csi);
    for (size_t r = 0; r < csi.rows; ++r) {     // バースト検出シンボルは消失扱い
        if (bad[r]) std::fill(csi[r], csi[r] + csi.cols, 0.0f);
    }
    if (null_center >= 0) {                     // 受信機の DC スパーが乗るキャリアとその周囲は消失扱い
        const int k0 = std::max(0, null_center - kNullDcHalfWidth);
        const int k1 = std::min(static_cast<int>(csi.cols) - 1, null_center + kNullDcHalfWidth);
        for (size_t r = 0; r < csi.rows; ++r) {
            for (int k = k0; k <= k1; ++k) csi[r][static_cast<size_t>(k)] = 0.0f;
        }
    }
    if (H_out) *H_out = std::move(H);
    const auto t2 = now();
    DeinterleaveEqualized(D, csi, mode, frame_start, layers, groups, Xt, Wt, fast);
    if (profile) {
        std::fprintf(stderr, "[profile] eq %zu rows: chest %.3f equalize %.3f deint %.3f\n", Y.rows, sec(t0, t1), sec(t1, t2), sec(t2, now()));
    }
}

// {I8, Q8, CSI8} 詰め行列用の補助
namespace {
inline cfloat PackedIq(uint32_t p) {
    return cfloat(static_cast<int8_t>(p & 0xFF) * (1.0f / 64.0f), static_cast<int8_t>((p >> 8) & 0xFF) * (1.0f / 64.0f));
}

class PackedSource : public ISymbolSource {
public:
    explicit PackedSource(const Mat<uint32_t>* m) : m_(m) {}
    int n_sym() const override { return static_cast<int>(m_->rows); }
    Mat<cfloat> Symbols(int i0, int i1) override {
        i0 = std::max(0, i0);
        i1 = std::min(i1, static_cast<int>(m_->rows));
        Mat<cfloat> out(static_cast<size_t>(std::max(0, i1 - i0)), m_->cols);
        for (int r = i0; r < i1; ++r) {
            const uint32_t* src = (*m_)[static_cast<size_t>(r)];
            cfloat* dst = out[static_cast<size_t>(r - i0)];
            for (size_t k = 0; k < m_->cols; ++k) dst[k] = PackedIq(src[k]);
        }
        return out;
    }
private:
    const Mat<uint32_t>* m_;
};

void AccumulateMerPacked(const Mat<uint32_t>& Xt, const std::vector<int>& rows,
                         const std::vector<int>& segments, Modulation mod, int dc, double* sig, double* err) {
    const float norm = ConstellationNorm(mod);
    const float max_level = MaxLevelOf(mod);
    for (int r : rows) {
        const uint32_t* row = Xt[r];
        for (int s : segments) {
            const uint32_t* p = row + static_cast<size_t>(s) * dc;
            for (int c = 0; c < dc; ++c) {
                if (((p[c] >> 16) & 0xFFu) == 0) continue;     // CSI 0（消失扱い）のキャリアは MER に含めない
                const cfloat x = PackedIq(p[c]);
                const float i = x.real() * norm, q = x.imag() * norm;
                const float si = SliceLevel(i, max_level), sq = SliceLevel(q, max_level);
                *err += (i - si) * (i - si) + (q - sq) * (q - sq);
                *sig += si * si + sq * sq;
            }
        }
    }
}

// NullInterferedCarriersFast の詰め行列版: 行 [row0, row1) を見て、干渉キャリアの CSI バイトを 0 にする
int NullInterferedCarriersPacked(Mat<uint32_t>* P, size_t row0, size_t row1, Mode mode, int frame_start,
                                 const std::vector<RxLayer>& layers) {
    const Layout& lo = LayoutOf(mode);
    const ModeParams& mp = ModeParamsOf(mode);
    const int dc = lo.dc;
    const int total = mp.total_carriers;
    Modulation mod_of[kNSegments];
    bool has_mod[kNSegments] = {};
    for (const RxLayer& lyr : layers) {
        for (int s : lyr.segments) {
            mod_of[kSegmentFreqIndex[s]] = lyr.mod;
            has_mod[kSegmentFreqIndex[s]] = true;
        }
    }
    int nulled = 0;
    std::vector<int> rows;
    std::vector<float> acc1(static_cast<size_t>(total)), acc2(static_cast<size_t>(total));
    std::vector<double> stat(static_cast<size_t>(total));
    row1 = std::min(row1, P->rows);
    for (int p = 0; p < kSpSymbolPeriod; ++p) {
        rows.clear();
        for (size_t r = row0; r < row1; ++r) {
            int ph = (static_cast<int>(r) - frame_start) % kSpSymbolPeriod;
            if (ph < 0) ph += kSpSymbolPeriod;
            if (ph == p) rows.push_back(static_cast<int>(r));
        }
        if (rows.size() < 8) continue;
        std::fill(acc1.begin(), acc1.end(), 0.0f);
        std::fill(acc2.begin(), acc2.end(), 0.0f);
        const size_t half = rows.size() / 2;
        for (size_t ri = 0; ri < rows.size(); ++ri) {
            const uint32_t* dr = (*P)[static_cast<size_t>(rows[ri])];
            float* a = (ri < half) ? acc1.data() : acc2.data();
            for (int f = 0; f < kNSegments; ++f) {
                if (!has_mod[f]) continue;
                const float norm = ConstellationNorm(mod_of[f]);
                const float max_level = MaxLevelOf(mod_of[f]);
                const int* kp = &lo.dpos4[p][static_cast<size_t>(f) * dc];
                for (int c = 0; c < dc; ++c) {
                    const int k = kp[c];
                    const cfloat x = PackedIq(dr[k]);
                    const float i = x.real() * norm, q = x.imag() * norm;
                    const float si = SliceLevel(i, max_level), sq = SliceLevel(q, max_level);
                    a[k] += (i - si) * (i - si) + (q - sq) * (q - sq);
                }
            }
        }
        std::fill(stat.begin(), stat.end(), -1.0);
        std::vector<double> valid;
        valid.reserve(static_cast<size_t>(total));
        for (int f = 0; f < kNSegments; ++f) {
            if (!has_mod[f]) continue;
            const int* kp = &lo.dpos4[p][static_cast<size_t>(f) * dc];
            for (int c = 0; c < dc; ++c) {
                const int k = kp[c];
                const double m1 = acc1[static_cast<size_t>(k)] / static_cast<double>(half);
                const double m2 = acc2[static_cast<size_t>(k)] / static_cast<double>(rows.size() - half);
                stat[static_cast<size_t>(k)] = std::min(m1, m2);
                valid.push_back(stat[static_cast<size_t>(k)]);
            }
        }
        if (valid.empty()) continue;
        std::nth_element(valid.begin(), valid.begin() + valid.size() / 2, valid.end());
        const double med = valid[valid.size() / 2];
        for (int k = 0; k < total; ++k) {
            if (stat[static_cast<size_t>(k)] > 10.0 * med) {
                for (int r : rows) (*P)[static_cast<size_t>(r)][k] &= ~0x00FF0000u;
                ++nulled;
            }
        }
    }
    return nulled;
}
}  // namespace

// NullInterferedCarriers の速い版（ライブの範囲復号用）: 行優先の 1 パスで、キャリアごとの誤差を行の前半と後半で
// 別々に平均し、小さい方を統計量にする（バーストで片方が汚れても引きずられない）。中央値版は列方向アクセス +
// nth_element で 2,100 行 × 5,617 キャリアに 0.15 s かかっていた（2026-10-15）
int NullInterferedCarriersFast(const Mat<cfloat>& D, Mat<float>* csi, Mode mode, int frame_start,
                               const std::vector<RxLayer>& layers) {
    const Layout& lo = LayoutOf(mode);
    const ModeParams& mp = ModeParamsOf(mode);
    const int dc = lo.dc;
    const int total = mp.total_carriers;
    Modulation mod_of[kNSegments];
    bool has_mod[kNSegments] = {};
    for (const RxLayer& lyr : layers) {
        for (int s : lyr.segments) {
            mod_of[kSegmentFreqIndex[s]] = lyr.mod;
            has_mod[kSegmentFreqIndex[s]] = true;
        }
    }
    int nulled = 0;
    std::vector<int> rows;
    std::vector<float> acc1(static_cast<size_t>(total)), acc2(static_cast<size_t>(total));
    std::vector<double> stat(static_cast<size_t>(total));
    for (int p = 0; p < kSpSymbolPeriod; ++p) {
        rows.clear();
        for (int r = 0; r < static_cast<int>(D.rows); ++r) {
            int ph = (r - frame_start) % kSpSymbolPeriod;
            if (ph < 0) ph += kSpSymbolPeriod;
            if (ph == p) rows.push_back(r);
        }
        if (rows.size() < 8) continue;
        std::fill(acc1.begin(), acc1.end(), 0.0f);
        std::fill(acc2.begin(), acc2.end(), 0.0f);
        const size_t half = rows.size() / 2;
        for (size_t ri = 0; ri < rows.size(); ++ri) {
            const cfloat* dr = D[rows[ri]];
            float* a = (ri < half) ? acc1.data() : acc2.data();
            for (int f = 0; f < kNSegments; ++f) {
                if (!has_mod[f]) continue;
                const float norm = ConstellationNorm(mod_of[f]);
                const float max_level = MaxLevelOf(mod_of[f]);
                const int* kp = &lo.dpos4[p][static_cast<size_t>(f) * dc];
                for (int c = 0; c < dc; ++c) {
                    const int k = kp[c];
                    const float i = dr[k].real() * norm;
                    const float q = dr[k].imag() * norm;
                    const float si = SliceLevel(i, max_level), sq = SliceLevel(q, max_level);
                    a[k] += (i - si) * (i - si) + (q - sq) * (q - sq);
                }
            }
        }
        std::fill(stat.begin(), stat.end(), -1.0);
        std::vector<double> valid;
        valid.reserve(static_cast<size_t>(total));
        for (int f = 0; f < kNSegments; ++f) {
            if (!has_mod[f]) continue;
            const int* kp = &lo.dpos4[p][static_cast<size_t>(f) * dc];
            for (int c = 0; c < dc; ++c) {
                const int k = kp[c];
                const double m1 = acc1[static_cast<size_t>(k)] / static_cast<double>(half);
                const double m2 = acc2[static_cast<size_t>(k)] / static_cast<double>(rows.size() - half);
                stat[static_cast<size_t>(k)] = std::min(m1, m2);
                valid.push_back(stat[static_cast<size_t>(k)]);
            }
        }
        if (valid.empty()) continue;
        std::nth_element(valid.begin(), valid.begin() + valid.size() / 2, valid.end());
        const double med = valid[valid.size() / 2];
        for (int k = 0; k < total; ++k) {
            if (stat[static_cast<size_t>(k)] > 10.0 * med) {
                for (int r : rows) (*csi)[r][k] = 0.0f;
                ++nulled;
            }
        }
    }
    return nulled;
}

void DeinterleaveEqualized(const Mat<cfloat>& D, const Mat<float>& csi_in, Mode mode, int frame_start,
                           const std::vector<RxLayer>& layers,
                           const std::vector<std::vector<int>>& groups,
                           Mat<cfloat>* Xt, Mat<float>* Wt, bool fast_null, size_t out_row0) {
    static const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    auto pnow = [] { return std::chrono::steady_clock::now(); };
    const auto q0 = pnow();
    Mat<float> csi = csi_in;
    if (fast_null) {
        NullInterferedCarriersFast(D, &csi, mode, frame_start, layers);
    } else {
        NullInterferedCarriers(D, &csi, mode, frame_start, layers);
    }
    const auto q1 = pnow();

    const auto q2 = pnow();
    if (fast_null) {
        // 論理変換 + セグメント間を 1 回の gather で、時間デインターリーブは必要な出力行だけ
        int I_seg2[kNSegments] = {};
        for (const RxLayer& lyr : layers) {
            for (int s : lyr.segments) I_seg2[s] = lyr.I;
        }
        Mat<cfloat> Yl2 = DataToLogicalDeint(D, mode, frame_start, groups);
        Mat<float> Wl2 = DataToLogicalDeint(csi, mode, frame_start, groups);
        const auto q3b = pnow();
        *Xt = TimeDeinterleaveFrom(Yl2, mode, I_seg2, out_row0);
        *Wt = TimeDeinterleaveFrom(Wl2, mode, I_seg2, out_row0);
        if (profile && D.rows > 1000) {
            auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
            std::fprintf(stderr, "[profile] deint %zu rows: null %.3f logical+interseg %.3f time(from %zu) %.3f\n",
                         D.rows, sec(q0, q1), sec(q2, q3b), out_row0, sec(q3b, pnow()));
        }
        return;
    }
    Mat<cfloat> Yl = DataToLogical(D, mode, frame_start);
    Mat<float> Wl = DataToLogical(csi, mode, frame_start);
    int I_seg[kNSegments] = {};
    for (const RxLayer& lyr : layers) {
        for (int s : lyr.segments) I_seg[s] = lyr.I;
    }
    // 逆変換の順序は送信チェーン（時間 3.11 → 周波数 3.12: セグメント間 → 回転 → ランダマイズ）の逆:
    // セグメント間デインターリーブを時間デインターリーブより先に行う（ref/isdbt/demod.py と同順、
    // 2026-10-10 実放送で検証）。
    IntersegmentDeinterleave(&Yl, mode, groups);
    IntersegmentDeinterleave(&Wl, mode, groups);
    const auto q3 = pnow();
    *Xt = TimeDeinterleave(Yl, mode, I_seg);
    *Wt = TimeDeinterleave(Wl, mode, I_seg);
    const auto q4 = pnow();
    if (profile && D.rows > 1000) {
        auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
        std::fprintf(stderr, "[profile] deint %zu rows: null %.3f logical %.3f interseg %.3f time %.3f\n",
                     D.rows, sec(q0, q1), sec(q1, q2), sec(q2, q3), sec(q3, q4));
    }
}

Mat<cfloat> MatSource::Symbols(int i0, int i1) {
    i0 = std::max(0, i0);
    i1 = std::min(i1, static_cast<int>(m_->rows));
    Mat<cfloat> out(static_cast<size_t>(std::max(0, i1 - i0)), m_->cols);
    if (i1 > i0) std::copy((*m_)[i0], (*m_)[i1 - 1] + m_->cols, out.data());
    return out;
}

int LayerFirstFrame(const RxLayer& lyr, int frame_start) {
    const int need = std::max(0, TimeDeinterleaveMaxDelay(lyr.I) - frame_start);
    return CeilDiv(need, kSymbolsPerFrame);
}

namespace {
// ビタビ後の硬判定ビット列 → パック → バイトデインターリーブ → 逆拡散 → RS → TS（DecodeSoftBits の共通部）
void DecodeBitsToTs(const std::vector<uint8_t>& bits, const RxLayer& lyr, Mode mode,
                    std::vector<uint8_t>* ts, std::vector<int>* nerr, double* t_rs) {
    const size_t n_bytes = bits.size() / 8;
    std::vector<uint8_t> onair(n_bytes);
    PackBits(bits.data(), n_bytes * 8, onair.data());

    const int tsp_pf = TspPerFrame(mode, lyr.mod, lyr.rate, static_cast<int>(lyr.segments.size()));
    const size_t T = (n_bytes / kRsN) / tsp_pf * tsp_pf;
    std::vector<uint8_t> deint(T * kRsN);
    ByteDeinterleave(onair.data(), T, deint.data());
    std::vector<uint8_t> tsps(T * kRsN);
    EnergyDescrambleLayer(deint.data(), T, tsp_pf, tsps.data());
    nerr->resize(T);
    const auto tr0 = std::chrono::steady_clock::now();
    RsDecode(tsps.data(), T, nerr->data());
    *t_rs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tr0).count();

    ts->resize(T * kTsPacket);
    for (size_t t = 0; t < T; ++t) {
        std::copy(tsps.begin() + t * kRsN, tsps.begin() + t * kRsN + kTsPacket,
                  ts->begin() + t * kTsPacket);
    }
}
}  // namespace

void DecodeSoftBits(const Mat<float>& soft, const RxLayer& lyr, Mode mode,
                    std::vector<uint8_t>* ts, std::vector<int>* nerr) {
    static const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    const auto tp0 = now();
    std::vector<float> pairs;
    const size_t steps = Depuncture(soft.data(), soft.size(), lyr.rate, &pairs);
    const auto tp1 = now();
    std::vector<uint8_t> bits = ViterbiDecode(pairs.data(), steps);
    const auto tp2 = now();
    double t_rs = 0.0;
    DecodeBitsToTs(bits, lyr, mode, ts, nerr, &t_rs);
    if (profile) {
        std::fprintf(stderr, "[profile] %s: depunct %.3f viterbi %.3f (%zu steps) pack+deint+descr+rs %.3f (rs %.3f, %zu tsp)\n",
                     lyr.name.c_str(), sec(tp0, tp1), sec(tp1, tp2), steps, sec(tp2, now()), t_rs, nerr->size());
    }
}

void DecodeSoftBitsQ(const std::vector<int8_t>& qsoft, const RxLayer& lyr, Mode mode,
                     std::vector<uint8_t>* ts, std::vector<int>* nerr) {
    static const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    const auto tp0 = now();
    std::vector<uint8_t> bits = ViterbiDecodeQ(qsoft.data(), qsoft.size(), lyr.rate);
    const auto tp1 = now();
    double t_rs = 0.0;
    DecodeBitsToTs(bits, lyr, mode, ts, nerr, &t_rs);
    if (profile) {
        std::fprintf(stderr, "[profile] %s: viterbi8 %.3f (%zu steps) pack+deint+descr+rs %.3f (rs %.3f, %zu tsp)\n",
                     lyr.name.c_str(), sec(tp0, tp1), bits.size(), sec(tp1, now()), t_rs, nerr->size());
    }
}

// 量子化済み軟値（LayerSoftBitsQ）から。vref が非 null なら float 版（Depuncture + ViterbiDecode）と突き合わせる
void DecodeSoftBitsRangeQ(const std::vector<int8_t>& qsoft, const RxLayer& lyr, Mode mode, int head_tsp, int n_frames,
                          std::vector<uint8_t>* ts, std::vector<int>* nerr, const Mat<float>* vref) {
    static const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    const auto tp0 = now();
    const auto tp1 = now();
    std::vector<uint8_t> bits = ViterbiDecodeQ(qsoft.data(), qsoft.size(), lyr.rate);
    const size_t steps = bits.size();
    const auto tp2 = now();
    if (vref) {
        const Mat<float>& soft = *vref;
        std::vector<float> pairs;
        const size_t st = Depuncture(soft.data(), soft.size(), lyr.rate, &pairs);
        std::vector<uint8_t> ref = ViterbiDecode(pairs.data(), st);
        size_t diff = 0;
        const size_t m = std::min(ref.size(), bits.size());
        for (size_t i = 0; i < m; ++i) diff += (ref[i] != bits[i]);
        std::fprintf(stderr, "[vcheck] %s: %zu bits, %zu differ (sizes %zu / %zu)\n", lyr.name.c_str(), m, diff, bits.size(), ref.size());
    }

    const size_t n_bytes = bits.size() / 8;
    std::vector<uint8_t> onair(n_bytes);
    PackBits(bits.data(), n_bytes * 8, onair.data());

    const int tsp_pf = TspPerFrame(mode, lyr.mod, lyr.rate, static_cast<int>(lyr.segments.size()));
    const size_t T_in = n_bytes / kRsN;
    std::vector<uint8_t> deint(T_in * kRsN);
    ByteDeinterleave(onair.data(), T_in, deint.data());     // 先頭 11 TSP は履歴が無く壊れる → 助走に含める
    const size_t head = static_cast<size_t>(std::max(0, head_tsp));
    size_t T = (T_in > head) ? ((T_in - head) / tsp_pf) * tsp_pf : 0;
    T = std::min(T, static_cast<size_t>(std::max(0, n_frames)) * tsp_pf);
    std::vector<uint8_t> tsps(T * kRsN);
    if (T > 0) EnergyDescrambleLayer(deint.data() + head * kRsN, T, tsp_pf, tsps.data());
    nerr->resize(T);
    const auto tp3 = now();
    if (T > 0) RsDecode(tsps.data(), T, nerr->data());
    const auto tp4 = now();

    ts->resize(T * kTsPacket);
    for (size_t t = 0; t < T; ++t) {
        std::copy(tsps.begin() + t * kRsN, tsps.begin() + t * kRsN + kTsPacket,
                  ts->begin() + t * kTsPacket);
    }
    if (profile) {
        std::fprintf(stderr, "[profile] %s: depunct %.3f viterbi %.3f (%zu steps) pack+deint+descr %.3f rs %.3f (%zu tsp)\n",
                     lyr.name.c_str(), sec(tp0, tp1), sec(tp1, tp2), steps, sec(tp2, tp3), sec(tp3, tp4), T);
    }
}

namespace {

// 1 本のソースをフレーム同期からブロック復号まで処理する。
// 前段（Symbols → 等化 → デインターリーブ）を別スレッドで 1 ブロック先行して走らせ、
// 復号（Viterbi〜RS）と重ねる。ソースへのアクセスは常に前段スレッド 1 本だけ。
// csi_full が非 null なら src は等化済み D を返すものとして、等化を飛ばしてデインターリーブだけ行う
void DecodeSource(ISymbolSource* src, Mode mode, int block_frames,
                  FrameSyncInfo* fsi_out, std::map<std::string, LayerResult>* layers_out,
                  const Mat<float>* csi_full = nullptr, bool fast_fec = false, int first_fec_frame = 0,
                  int* emit_first = nullptr, int* emit_end = nullptr, bool null_dc = false) {
    const int null_center = null_dc ? src->dc_carrier() : -1;
    *fsi_out = FrameSync(src, mode);
    const FrameSyncInfo& fsi = *fsi_out;
    const int f0 = fsi.frame_start;
    const int n_frames = fsi.n_frames;

    int hist = 0;
    std::map<std::string, int> fstart;
    for (const RxLayer& lyr : fsi.layers) {
        hist = std::max(hist, CeilDiv(TimeDeinterleaveMaxDelay(lyr.I), kSymbolsPerFrame));
        fstart[lyr.name] = LayerFirstFrame(lyr, f0);
        (*layers_out)[lyr.name] = LayerResult{};
        (*layers_out)[lyr.name].first_frame = -1;
    }

    struct BlockFE {
        int b0, b1, s0;
        Mat<cfloat> Xt;
        Mat<float> Wt;
    };
    const int step = std::max(1, block_frames);
    auto front_end = [&](int b0) {
        BlockFE fe;
        fe.b0 = b0;
        fe.b1 = std::min(n_frames, b0 + step);
        const int fa = std::max(0, b0 - 1);             // Viterbi/バイト履歴の立ち上がり用に 1 フレーム重ねる
        fe.s0 = std::max(0, f0 + (fa - hist) * kSymbolsPerFrame - kEqContextSymbols);
        // fast_fec の範囲復号はブロック末尾の後ろに kFecTailSymbols 行が要る
        const int s1 = std::min(src->n_sym(), f0 + fe.b1 * kSymbolsPerFrame + std::max(kEqContextSymbols, kFecTailSymbols));
        static const bool profile_fe = std::getenv("ISDBT_PROFILE") != nullptr;
        const auto ts0 = std::chrono::steady_clock::now();
        Mat<cfloat> Y = src->Symbols(fe.s0, s1);
        if (profile_fe) {
            std::fprintf(stderr, "[profile] symbols %d rows: %.3f\n", s1 - fe.s0,
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - ts0).count());
        }
        if (csi_full) {
            Mat<float> csi(static_cast<size_t>(s1 - fe.s0), csi_full->cols);
            std::copy((*csi_full)[fe.s0], (*csi_full)[s1 - 1] + csi_full->cols, csi.data());
            DeinterleaveEqualized(Y, csi, mode, f0 - fe.s0, fsi.layers, fsi.groups, &fe.Xt, &fe.Wt);
        } else {
            // ライブ（fast_fec）は干渉検出も 1 パス版（FPGA 前段経路と同じ）
            EqualizeAndDeinterleave(Y, mode, f0 - fe.s0, fsi.layers, fsi.groups, nullptr, &fe.Xt, &fe.Wt, fast_fec,
                                    null_center);
        }
        return fe;
    };

    const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    const int dc = ModeParamsOf(mode).data_carriers_per_segment;
    std::map<std::string, std::pair<double, double>> mer_acc;      // name -> (sig, err)
    double t_front = 0.0, t_soft = 0.0, t_fec = 0.0;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };

    const int b_first = std::max(0, first_fec_frame);
    int reliable_first = b_first;               // fast_fec: 全層が完全なフレームの範囲 [reliable_first, complete_end)
    int complete_end = n_frames;
    std::future<BlockFE> pending;
    if (b_first < n_frames) pending = std::async(std::launch::async, front_end, b_first);
    for (int b0 = b_first; b0 < n_frames; b0 += step) {
        auto t0 = now();
        BlockFE fe = pending.get();
        t_front += sec(t0, now());      // 復号側から見た前段の待ち時間（重なった分は見えない）
        if (b0 + step < n_frames) pending = std::async(std::launch::async, front_end, b0 + step);
        if (fast_fec) {
            // 範囲復号（DecodeRange と同じ）: ブロックのフレーム [bstart, b1) の前後に助走（kFecHeadSymbols /
            // kFecTailSymbols）だけ付けて復号し、ブロック内の全フレームを完全な形で出す（1 フレームの重ねや
            // 末尾フレームの捨てが無いぶん、ライブのウィンドウでは FEC が約 2 割減る）
            for (const RxLayer& lyr : fsi.layers) {
                const int bstart = std::max(b0, fstart[lyr.name]);
                if (bstart >= fe.b1) continue;
                const int tsp_pf = TspPerFrame(mode, lyr.mod, lyr.rate, static_cast<int>(lyr.segments.size()));
                const int avail = static_cast<int>(fe.Xt.rows) - (f0 - fe.s0);     // 受信フレーム 0 からの行数
                int head_rows = kFecHeadSymbols;
                int r0 = bstart * kSymbolsPerFrame - head_rows;
                if (r0 < 0) {              // 先頭に助走が無い: 最初のフレームは Emit から外す（emit_first で）
                    head_rows = 0;
                    r0 = 0;
                }
                const int r1 = std::min(avail, fe.b1 * kSymbolsPerFrame + kFecTailSymbols);
                if (r1 <= r0) continue;
                std::vector<int> rows;
                rows.reserve(static_cast<size_t>(r1 - r0));
                for (int r = r0; r < r1; ++r) rows.push_back((f0 - fe.s0) + r);
                auto t1 = now();
                {
                    std::vector<int> mrows;
                    for (int r = bstart * kSymbolsPerFrame; r < std::min(avail, fe.b1 * kSymbolsPerFrame); ++r) {
                        mrows.push_back((f0 - fe.s0) + r);
                    }
                    auto& acc = mer_acc[lyr.name];
                    AccumulateMer(fe.Xt, mrows, lyr.segments, lyr.mod, dc, &acc.first, &acc.second, &fe.Wt);
                }
                std::vector<int8_t> qsoft = LayerSoftBitsQ(fe.Xt, fe.Wt, rows, lyr.segments, lyr.mod, mode, kViterbiSoftQ);
                auto t2 = now();
                std::vector<uint8_t> ts;
                std::vector<int> nerr;
                const int head_tsp = head_rows * tsp_pf / kSymbolsPerFrame;
                DecodeSoftBitsRangeQ(qsoft, lyr, mode, head_tsp, fe.b1 - bstart, &ts, &nerr, nullptr);
                t_soft += sec(t1, t2);
                t_fec += sec(t2, now());
                LayerResult& lr = (*layers_out)[lyr.name];
                if (lr.first_frame < 0) {
                    lr.first_frame = bstart;
                    reliable_first = std::max(reliable_first, bstart + (head_rows == 0 ? 1 : 0));
                }
                lr.ts.insert(lr.ts.end(), ts.begin(), ts.end());
                lr.rs_err.insert(lr.rs_err.end(), nerr.begin(), nerr.end());
            }
            continue;
        }
        const int fa = std::max(0, b0 - 1);
        for (const RxLayer& lyr : fsi.layers) {
            const int fa_l = std::max(fa, fstart[lyr.name]);
            if (fa_l >= fe.b1) continue;
            std::vector<int> rows;
            rows.reserve(static_cast<size_t>(fe.b1 - fa_l) * kSymbolsPerFrame);
            for (int r = fa_l * kSymbolsPerFrame; r < fe.b1 * kSymbolsPerFrame; ++r) {
                rows.push_back((f0 - fe.s0) + r);
            }
            auto t1 = now();
            // MER は出力区間（重ね分を除く）の行だけで積算する
            {
                std::vector<int> mrows;
                for (int r = std::max(b0, fa_l) * kSymbolsPerFrame; r < fe.b1 * kSymbolsPerFrame; ++r) {
                    mrows.push_back((f0 - fe.s0) + r);
                }
                auto& acc = mer_acc[lyr.name];
                AccumulateMer(fe.Xt, mrows, lyr.segments, lyr.mod, dc, &acc.first, &acc.second, &fe.Wt);
            }
            std::vector<uint8_t> ts;
            std::vector<int> nerr;
            std::chrono::steady_clock::time_point t2;
            if (fast_fec) {
                std::vector<int8_t> qsoft = LayerSoftBitsQ(fe.Xt, fe.Wt, rows, lyr.segments, lyr.mod, mode, kViterbiSoftQ);
                t2 = now();
                DecodeSoftBitsQ(qsoft, lyr, mode, &ts, &nerr);
            } else {
                Mat<float> soft = LayerSoftBits(fe.Xt, fe.Wt, rows, lyr.segments, lyr.mod, mode);
                t2 = now();
                DecodeSoftBits(soft, lyr, mode, &ts, &nerr);
            }
            t_soft += sec(t1, t2);
            t_fec += sec(t2, now());
            const int tsp_pf = TspPerFrame(mode, lyr.mod, lyr.rate, static_cast<int>(lyr.segments.size()));
            const size_t skip = static_cast<size_t>(std::max(b0, fa_l) - fa_l) * tsp_pf;
            LayerResult& lr = (*layers_out)[lyr.name];
            if (skip * kTsPacket < ts.size()) {
                lr.ts.insert(lr.ts.end(), ts.begin() + skip * kTsPacket, ts.end());
                lr.rs_err.insert(lr.rs_err.end(), nerr.begin() + skip, nerr.end());
            }
            if (lr.first_frame < 0) lr.first_frame = std::max(b0, fa_l);
        }
    }
    for (auto& [name, acc] : mer_acc) {
        if (acc.second > 0.0) {
            (*layers_out)[name].mer_db = 10.0 * std::log10(acc.first / acc.second);
        }
    }
    if (fast_fec && emit_first && emit_end) {
        // 全層が完全なフレームの範囲（末尾は tail の助走が足りず短くなった層に合わせる）
        for (const RxLayer& lyr : fsi.layers) {
            const LayerResult& lr = (*layers_out)[lyr.name];
            if (lr.first_frame < 0) continue;
            const int tsp_pf = TspPerFrame(mode, lyr.mod, lyr.rate, static_cast<int>(lyr.segments.size()));
            complete_end = std::min(complete_end, lr.first_frame + static_cast<int>(lr.rs_err.size()) / tsp_pf);
        }
        *emit_first = reliable_first;
        *emit_end = std::max(reliable_first, complete_end);
    }
    if (profile) {
        std::fprintf(stderr, "[profile] front(wait)=%.2fs soft=%.2fs fec=%.2fs\n", t_front, t_soft, t_fec);
    }
}

}  // namespace

namespace {

// 範囲復号: フレーム同期のあと、受信フレーム [F0, F1) だけをデマップ〜RS にかける（デインターリーブは
// 履歴込みの行だけ）。F0 は「時間デインターリーブの履歴 + 先頭助走がウィンドウ内に収まる最小のフレーム」。
// 出力は全層とも完全なフレームだけ（先頭の壊れた 11 TSP も末尾の未完成も無い）
void DecodeRange(const Mat<cfloat>& D, const Mat<float>& csi_full, Mode mode, int fec_frames,
                 FrameSyncInfo* fsi_out, std::map<std::string, LayerResult>* layers_out,
                 int* emit_first, int* emit_end) {
    MatSource src(&D);
    const auto tfs0 = std::chrono::steady_clock::now();
    *fsi_out = FrameSync(&src, mode);
    const double t_fs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tfs0).count();
    const FrameSyncInfo& fsi = *fsi_out;
    const int f0 = fsi.frame_start;
    const int n_sym = static_cast<int>(D.rows);
    *emit_first = *emit_end = -1;
    for (const RxLayer& lyr : fsi.layers) {
        (*layers_out)[lyr.name] = LayerResult{};
        (*layers_out)[lyr.name].first_frame = -1;
    }
    int max_delay = 0;
    for (const RxLayer& lyr : fsi.layers) max_delay = std::max(max_delay, TimeDeinterleaveMaxDelay(lyr.I));
    // 先頭助走の前に時間デインターリーブの履歴（max_delay）と等化文脈が要る
    const int need_before = kFecHeadSymbols + max_delay + kEqContextSymbols;
    int F0 = 0;
    while (f0 + F0 * kSymbolsPerFrame < need_before) ++F0;
    // 末尾助走まで収まるフレーム数
    int F1 = F0 + std::max(0, fec_frames);
    while (F1 > F0 && f0 + F1 * kSymbolsPerFrame + kFecTailSymbols > n_sym) --F1;
    if (F1 <= F0) return;
    const int s0 = f0 + F0 * kSymbolsPerFrame - need_before;
    const int s1 = std::min(n_sym, f0 + F1 * kSymbolsPerFrame + kFecTailSymbols);
    Mat<cfloat> Y = src.Symbols(s0, s1);
    Mat<float> csi(static_cast<size_t>(s1 - s0), csi_full.cols);
    std::copy(csi_full[s0], csi_full[s1 - 1] + csi_full.cols, csi.data());
    Mat<cfloat> Xt;
    Mat<float> Wt;
    const auto tfe0 = std::chrono::steady_clock::now();
    // 時間デインターリーブの出力は FEC が読む行（先頭助走の少し前）から
    const size_t out_row0 = static_cast<size_t>(std::max(0, (f0 - s0) + F0 * kSymbolsPerFrame - kFecHeadSymbols - 8));
    DeinterleaveEqualized(Y, csi, mode, f0 - s0, fsi.layers, fsi.groups, &Xt, &Wt, true, out_row0);
    const double t_fe = std::chrono::duration<double>(std::chrono::steady_clock::now() - tfe0).count();

    const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    const int dc = ModeParamsOf(mode).data_carriers_per_segment;
    double t_soft = 0.0, t_fec = 0.0;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    const int base = f0 - s0;           // 受信フレーム 0 の行（Xt 内）
    for (const RxLayer& lyr : fsi.layers) {
        const int tsp_pf = TspPerFrame(mode, lyr.mod, lyr.rate, static_cast<int>(lyr.segments.size()));
        const int head_tsp = kFecHeadSymbols * tsp_pf / kSymbolsPerFrame;
        std::vector<int> rows;
        const int r0 = base + F0 * kSymbolsPerFrame - kFecHeadSymbols;
        const int r1 = std::min(static_cast<int>(Xt.rows), base + F1 * kSymbolsPerFrame + kFecTailSymbols);
        rows.reserve(static_cast<size_t>(r1 - r0));
        for (int r = r0; r < r1; ++r) rows.push_back(r);
        auto t1 = now();
        {
            std::vector<int> mrows;
            for (int r = base + F0 * kSymbolsPerFrame; r < base + F1 * kSymbolsPerFrame; ++r) mrows.push_back(r);
            double sig = 0.0, err = 0.0;
            AccumulateMer(Xt, mrows, lyr.segments, lyr.mod, dc, &sig, &err, &Wt);
            if (err > 0.0) (*layers_out)[lyr.name].mer_db = 10.0 * std::log10(sig / err);
        }
        static const bool vcheck = std::getenv("ISDBT_VITERBI_CHECK") != nullptr;
        std::vector<int8_t> qsoft = LayerSoftBitsQ(Xt, Wt, rows, lyr.segments, lyr.mod, mode, kViterbiSoftQ);
        auto t2 = now();
        LayerResult& lr = (*layers_out)[lyr.name];
        if (vcheck) {
            Mat<float> soft = LayerSoftBits(Xt, Wt, rows, lyr.segments, lyr.mod, mode);
            DecodeSoftBitsRangeQ(qsoft, lyr, mode, head_tsp, F1 - F0, &lr.ts, &lr.rs_err, &soft);
        } else {
            DecodeSoftBitsRangeQ(qsoft, lyr, mode, head_tsp, F1 - F0, &lr.ts, &lr.rs_err, nullptr);
        }
        t_soft += sec(t1, t2);
        t_fec += sec(t2, now());
        lr.first_frame = F0;
    }
    *emit_first = F0;
    *emit_end = F1;
    if (profile) {
        std::fprintf(stderr, "[profile] range frames %d..%d of %d: framesync=%.3fs deint=%.3fs (rows %d) soft=%.2fs fec=%.2fs\n",
                     F0, F1, fsi.n_frames, t_fs, t_fe, s1 - s0, t_soft, t_fec);
    }
}

}  // namespace

namespace {

void DecodeRangePacked(Mat<uint32_t>* P, Mode mode, int fec_frames,
                       FrameSyncInfo* fsi_out, std::map<std::string, LayerResult>* layers_out,
                       int* emit_first, int* emit_end, int null_center = -1) {
    PackedSource src(P);
    const auto tfs0 = std::chrono::steady_clock::now();
    *fsi_out = FrameSync(&src, mode);
    const double t_fs = std::chrono::duration<double>(std::chrono::steady_clock::now() - tfs0).count();
    const FrameSyncInfo& fsi = *fsi_out;
    const int f0 = fsi.frame_start;
    const int n_sym = static_cast<int>(P->rows);
    *emit_first = *emit_end = -1;
    for (const RxLayer& lyr : fsi.layers) {
        (*layers_out)[lyr.name] = LayerResult{};
        (*layers_out)[lyr.name].first_frame = -1;
    }
    int max_delay = 0;
    int I_seg[kNSegments] = {};
    for (const RxLayer& lyr : fsi.layers) {
        max_delay = std::max(max_delay, TimeDeinterleaveMaxDelay(lyr.I));
        for (int s : lyr.segments) I_seg[s] = lyr.I;
    }
    const int need_before = kFecHeadSymbols + max_delay + kEqContextSymbols;
    int F0 = 0;
    while (f0 + F0 * kSymbolsPerFrame < need_before) ++F0;
    int F1 = F0 + std::max(0, fec_frames);
    while (F1 > F0 && f0 + F1 * kSymbolsPerFrame + kFecTailSymbols > n_sym) --F1;
    if (F1 <= F0) return;
    const int s0 = f0 + F0 * kSymbolsPerFrame - need_before;
    const int s1 = std::min(n_sym, f0 + F1 * kSymbolsPerFrame + kFecTailSymbols);

    static const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };
    const auto q0 = now();
    NullInterferedCarriersPacked(P, static_cast<size_t>(s0), static_cast<size_t>(s1), mode, f0, fsi.layers);
    if (null_center >= 0) {                     // 受信機の DC スパー（中央 − δ）の周囲: CSI バイトを 0 に
        const int k0 = std::max(0, null_center - kNullDcHalfWidth);
        const int k1 = std::min(static_cast<int>(P->cols) - 1, null_center + kNullDcHalfWidth);
        for (size_t r = static_cast<size_t>(s0); r < static_cast<size_t>(s1) && r < P->rows; ++r) {
            uint32_t* row = (*P)[r];
            for (int k = k0; k <= k1; ++k) row[k] &= 0xFF00FFFFu;
        }
    }
    const auto q1 = now();
    Mat<uint32_t> Yl = DataToLogicalDeintRange(*P, mode, f0, fsi.groups, static_cast<size_t>(s0), static_cast<size_t>(s1));
    const auto q2 = now();
    const int base = f0 - s0;           // 受信フレーム 0 の行（Yl / Xt 内）
    const size_t out_row0 = static_cast<size_t>(std::max(0, base + F0 * kSymbolsPerFrame - kFecHeadSymbols - 8));
    Mat<uint32_t> Xt = TimeDeinterleaveFrom(Yl, mode, I_seg, out_row0);
    const auto q3 = now();

    const int dc = ModeParamsOf(mode).data_carriers_per_segment;
    double t_soft = 0.0, t_fec = 0.0;
    for (const RxLayer& lyr : fsi.layers) {
        const int tsp_pf = TspPerFrame(mode, lyr.mod, lyr.rate, static_cast<int>(lyr.segments.size()));
        const int head_tsp = kFecHeadSymbols * tsp_pf / kSymbolsPerFrame;
        std::vector<int> rows;
        const int r0 = base + F0 * kSymbolsPerFrame - kFecHeadSymbols;
        const int r1 = std::min(static_cast<int>(Xt.rows), base + F1 * kSymbolsPerFrame + kFecTailSymbols);
        rows.reserve(static_cast<size_t>(r1 - r0));
        for (int r = r0; r < r1; ++r) rows.push_back(r);
        auto t1 = now();
        {
            std::vector<int> mrows;
            for (int r = base + F0 * kSymbolsPerFrame; r < base + F1 * kSymbolsPerFrame; ++r) mrows.push_back(r);
            double sig = 0.0, err = 0.0;
            AccumulateMerPacked(Xt, mrows, lyr.segments, lyr.mod, dc, &sig, &err);
            if (err > 0.0) (*layers_out)[lyr.name].mer_db = 10.0 * std::log10(sig / err);
        }
        std::vector<int8_t> qsoft = LayerSoftBitsQPacked(Xt, rows, lyr.segments, lyr.mod, mode, kViterbiSoftQ);
        auto t2 = now();
        LayerResult& lr = (*layers_out)[lyr.name];
        DecodeSoftBitsRangeQ(qsoft, lyr, mode, head_tsp, F1 - F0, &lr.ts, &lr.rs_err, nullptr);
        t_soft += sec(t1, t2);
        t_fec += sec(t2, now());
        lr.first_frame = F0;
    }
    *emit_first = F0;
    *emit_end = F1;
    if (profile) {
        std::fprintf(stderr, "[profile] packed frames %d..%d of %d: framesync=%.3fs null=%.3f logical=%.3f time=%.3f (rows %d) soft=%.2fs fec=%.2fs\n",
                     F0, F1, fsi.n_frames, t_fs, sec(q0, q1), sec(q1, q2), sec(q2, q3), s1 - s0, t_soft, t_fec);
    }
}

}  // namespace

DemodResult DemodulateEqualizedPacked(Mat<uint32_t>* P, Mode mode, int fec_frames, int null_center) {
    DemodResult out;
    out.sync.gi_start0 = 0.0;
    out.sync.n_sym = static_cast<int>(P->rows);
    out.sync.fft_size = ModeParamsOf(mode).fft_size;
    DecodeRangePacked(P, mode, fec_frames, &out.fsi, &out.layers, &out.emit_first, &out.emit_end, null_center);
    return out;
}

DemodResult DemodulateEqualized(const Mat<cfloat>& D, const Mat<float>& csi, Mode mode, int block_frames,
                                int fec_frames) {
    DemodResult out;
    out.sync.gi_start0 = 0.0;
    out.sync.n_sym = static_cast<int>(D.rows);
    out.sync.fft_size = ModeParamsOf(mode).fft_size;
    if (fec_frames > 0) {
        DecodeRange(D, csi, mode, fec_frames, &out.fsi, &out.layers, &out.emit_first, &out.emit_end);
        return out;
    }
    MatSource src(&D);
    DecodeSource(&src, mode, block_frames, &out.fsi, &out.layers, &csi);
    return out;
}

DemodResult Demodulate(std::vector<cfloat> iq, double fs, Mode mode, Gi gi, int block_frames, bool fast_fec,
                       int first_fec_frame, bool null_dc) {
    CarrierSource src(std::move(iq), fs, mode, gi);
    DemodResult out;
    out.sync = src.info();
    DecodeSource(&src, mode, block_frames, &out.fsi, &out.layers, nullptr, fast_fec, first_fec_frame,
                 &out.emit_first, &out.emit_end, null_dc);
    return out;
}

TrackedResult DemodulateTracked(std::vector<cfloat> iq, double fs, Mode mode, Gi gi,
                                int block_frames) {
    TrackedCarrierSource src(std::move(iq), fs, mode, gi);
    TrackedResult out;
    for (const auto& [i0, i1] : src.segments()) {
        TrackedSegment seg;
        seg.i0 = i0;
        seg.i1 = i1;
        if (i1 - i0 < 3 * kSymbolsPerFrame) {
            seg.skip_reason = "too short";
            out.segments.push_back(std::move(seg));
            continue;
        }
        SegmentView view(&src, i0, i1);
        try {
            DecodeSource(&view, mode, block_frames, &seg.fsi, &seg.layers);
        } catch (const std::exception& e) {
            seg.skip_reason = e.what();
            out.segments.push_back(std::move(seg));
            continue;
        }
        seg.decoded = true;
        for (auto& [name, lr] : seg.layers) {
            LayerResult& dst = out.layers[name];
            if (dst.ts.empty()) dst.first_frame = lr.first_frame;
            dst.ts.insert(dst.ts.end(), lr.ts.begin(), lr.ts.end());
            dst.rs_err.insert(dst.rs_err.end(), lr.rs_err.begin(), lr.rs_err.end());
        }
        out.segments.push_back(std::move(seg));
    }
    return out;
}

}  // namespace isdbt
