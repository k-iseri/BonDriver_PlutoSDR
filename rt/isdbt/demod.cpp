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
                   double* sig, double* err) {
    const float norm = ConstellationNorm(mod);
    const float max_level = MaxLevelOf(mod);
    for (int r : rows) {
        const cfloat* row = Xt[r];
        for (int s : segments) {
            const cfloat* p = row + static_cast<size_t>(s) * dc;
            for (int c = 0; c < dc; ++c) {
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
                             Mat<cfloat>* H_out, Mat<cfloat>* Xt, Mat<float>* Wt) {
    std::vector<char> bad;
    Mat<cfloat> H = ChannelEstimate(Y, mode, frame_start, 8, &bad);
    Mat<cfloat> D;
    Mat<float> csi;
    Equalize(Y, H, &D, &csi);
    for (size_t r = 0; r < csi.rows; ++r) {     // バースト検出シンボルは消失扱い
        if (bad[r]) std::fill(csi[r], csi[r] + csi.cols, 0.0f);
    }
    NullInterferedCarriers(D, &csi, mode, frame_start, layers);
    if (H_out) *H_out = std::move(H);

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
    *Xt = TimeDeinterleave(Yl, mode, I_seg);
    *Wt = TimeDeinterleave(Wl, mode, I_seg);
}

int LayerFirstFrame(const RxLayer& lyr, int frame_start) {
    const int need = std::max(0, TimeDeinterleaveMaxDelay(lyr.I) - frame_start);
    return CeilDiv(need, kSymbolsPerFrame);
}

void DecodeSoftBits(const Mat<float>& soft, const RxLayer& lyr, Mode mode,
                    std::vector<uint8_t>* ts, std::vector<int>* nerr) {
    std::vector<float> pairs;
    const size_t steps = Depuncture(soft.data(), soft.size(), lyr.rate, &pairs);
    std::vector<uint8_t> bits = ViterbiDecode(pairs.data(), steps);

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
    RsDecode(tsps.data(), T, nerr->data());

    ts->resize(T * kTsPacket);
    for (size_t t = 0; t < T; ++t) {
        std::copy(tsps.begin() + t * kRsN, tsps.begin() + t * kRsN + kTsPacket,
                  ts->begin() + t * kTsPacket);
    }
}

namespace {

// 1 本のソースをフレーム同期からブロック復号まで処理する。
// 前段（Symbols → 等化 → デインターリーブ）を別スレッドで 1 ブロック先行して走らせ、
// 復号（Viterbi〜RS）と重ねる。ソースへのアクセスは常に前段スレッド 1 本だけ。
void DecodeSource(ISymbolSource* src, Mode mode, int block_frames,
                  FrameSyncInfo* fsi_out, std::map<std::string, LayerResult>* layers_out) {
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
        const int s1 = std::min(src->n_sym(), f0 + fe.b1 * kSymbolsPerFrame + kEqContextSymbols);
        Mat<cfloat> Y = src->Symbols(fe.s0, s1);
        EqualizeAndDeinterleave(Y, mode, f0 - fe.s0, fsi.layers, fsi.groups, nullptr, &fe.Xt, &fe.Wt);
        return fe;
    };

    const bool profile = std::getenv("ISDBT_PROFILE") != nullptr;
    const int dc = ModeParamsOf(mode).data_carriers_per_segment;
    std::map<std::string, std::pair<double, double>> mer_acc;      // name -> (sig, err)
    double t_front = 0.0, t_soft = 0.0, t_fec = 0.0;
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [](auto a, auto b) { return std::chrono::duration<double>(b - a).count(); };

    std::future<BlockFE> pending;
    if (n_frames > 0) pending = std::async(std::launch::async, front_end, 0);
    for (int b0 = 0; b0 < n_frames; b0 += step) {
        auto t0 = now();
        BlockFE fe = pending.get();
        t_front += sec(t0, now());      // 復号側から見た前段の待ち時間（重なった分は見えない）
        if (b0 + step < n_frames) pending = std::async(std::launch::async, front_end, b0 + step);
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
                AccumulateMer(fe.Xt, mrows, lyr.segments, lyr.mod, dc, &acc.first, &acc.second);
            }
            Mat<float> soft = LayerSoftBits(fe.Xt, fe.Wt, rows, lyr.segments, lyr.mod, mode);
            auto t2 = now();
            std::vector<uint8_t> ts;
            std::vector<int> nerr;
            DecodeSoftBits(soft, lyr, mode, &ts, &nerr);
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
    if (profile) {
        std::fprintf(stderr, "[profile] front(wait)=%.2fs soft=%.2fs fec=%.2fs\n", t_front, t_soft, t_fec);
    }
}

}  // namespace

DemodResult Demodulate(std::vector<cfloat> iq, double fs, Mode mode, Gi gi, int block_frames) {
    CarrierSource src(std::move(iq), fs, mode, gi);
    DemodResult out;
    out.sync = src.info();
    DecodeSource(&src, mode, block_frames, &out.fsi, &out.layers);
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
