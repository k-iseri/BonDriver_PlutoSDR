// rt/isdbt の検証: tools/gen_rt_vectors.py が ref/ から作ったベクタと段ごとに突き合わせる。
// 使い方: rt_test <vectors_dir>
// 整数段（RS、PRBS、バイトデインターリーブ、TMCC、Viterbi 判定）はビット一致、
// 浮動小数段（FFT、リサンプル、等化）は相対 RMS 誤差で判定する。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../demod/rate_monitor.h"
#include "../isdbc/demod.h"
#include "../isdbc/framing.h"
#include "../isdbc/mapping.h"
#include "../isdbc/tsmf.h"
#include "../isdbt/deinterleave.h"
#include "../isdbt/demod.h"
#include "../isdbt/equalize.h"
#include "../isdbt/fec.h"
#include "../isdbt/fft.h"
#include "../isdbt/io.h"
#include "../isdbt/npy.h"
#include "../isdbt/pilots.h"
#include "../isdbt/sync.h"
#include "../isdbt/tmcc.h"
#include "../mirakurun/channel_spec.h"

using namespace isdbt;

namespace {

int g_fail = 0;
int g_pass = 0;

void Report(bool ok, const std::string& name, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        std::printf("ok   %s\n", name.c_str());
    } else {
        ++g_fail;
        std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    }
}

std::string g_dir;
std::string Path(const std::string& name) { return g_dir + "/" + name; }

std::string g_json;
double JNum(const std::string& key, double def = -1e18) {
    size_t p = g_json.find("\"" + key + "\"");
    if (p == std::string::npos) return def;
    p = g_json.find(':', p);
    return std::strtod(g_json.c_str() + p + 1, nullptr);
}
bool JBool(const std::string& key) {
    size_t p = g_json.find("\"" + key + "\"");
    if (p == std::string::npos) return false;
    p = g_json.find(':', p);
    while (p < g_json.size() && (g_json[p] == ':' || g_json[p] == ' ')) ++p;
    return g_json.compare(p, 4, "true") == 0;
}
std::string JStr(const std::string& key) {
    size_t p = g_json.find("\"" + key + "\"");
    if (p == std::string::npos) return {};
    p = g_json.find(':', p);
    p = g_json.find('"', p);
    size_t q = g_json.find('"', p + 1);
    return g_json.substr(p + 1, q - p - 1);
}

template <typename T>
double RelRmsError(const T* got, const T* want, size_t n) {
    double err = 0.0, ref = 0.0;
    for (size_t i = 0; i < n; ++i) {
        err += std::norm(std::complex<double>(got[i]) - std::complex<double>(want[i]));
        ref += std::norm(std::complex<double>(want[i]));
    }
    return ref > 0 ? std::sqrt(err / ref) : std::sqrt(err);
}
double RelRmsError(const float* got, const float* want, size_t n) {
    double err = 0.0, ref = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(got[i]) - want[i];
        err += d * d;
        ref += static_cast<double>(want[i]) * want[i];
    }
    return ref > 0 ? std::sqrt(err / ref) : std::sqrt(err);
}

// ---------------------------------------------------------------------------
// 単体ベクタ
// ---------------------------------------------------------------------------
void TestPrbs() {
    NpyArray want = NpyRead(Path("prbs.npy"));
    std::vector<uint8_t> got(want.elements());
    EnergyDispersalPrbs(got.size(), got.data());
    bool ok = std::memcmp(got.data(), want.data.data(), got.size()) == 0;
    ok = ok && got[0] == 0x03 && got[1] == 0xF6 && got[2] == 0x08 && got[3] == 0x34;
    Report(ok, "energy_dispersal_prbs");
}

void TestRs() {
    NpyArray in = NpyRead(Path("rs_in.npy"));
    NpyArray want = NpyRead(Path("rs_expected.npy"));
    NpyArray want_nerr = NpyRead(Path("rs_nerr.npy"));
    std::vector<uint8_t> buf(in.data);
    std::vector<int> nerr(in.rows());
    RsDecode(buf.data(), in.rows(), nerr.data());
    bool ok = std::memcmp(buf.data(), want.data.data(), buf.size()) == 0;
    int bad = -1;
    const int64_t* wn = want_nerr.as<int64_t>();
    for (size_t i = 0; i < nerr.size(); ++i) {
        if (nerr[i] != wn[i]) { ok = false; bad = static_cast<int>(i); break; }
    }
    Report(ok, "rs_decode", bad >= 0 ? "nerr mismatch at row " + std::to_string(bad) : "bytes mismatch");
    if (ok) std::printf("     (%zu TSP, up to 10 errors injected)\n", in.rows());
}

void TestViterbi() {
    NpyArray soft = NpyRead(Path("viterbi_soft.npy"));
    NpyArray want = NpyRead(Path("viterbi_bits.npy"));
    CodeRate rate{3, 4};
    std::vector<float> pairs;
    size_t steps = Depuncture(soft.as<float>(), soft.elements(), rate, &pairs);
    std::vector<uint8_t> bits = ViterbiDecode(pairs.data(), steps);
    const size_t head = static_cast<size_t>(JNum("viterbi_skip_head", 96));
    const size_t tail = static_cast<size_t>(JNum("viterbi_skip_tail", 64));
    const size_t n = std::min(bits.size(), want.elements());
    size_t nerr = 0;
    for (size_t i = head; i + tail < n; ++i) nerr += bits[i] != want.as<uint8_t>()[i];
    Report(nerr == 0, "viterbi_decode", std::to_string(nerr) + " bit errors");
}

// recpluto のチャンネル表記（地デジ物理 ch / CATV C 付き）と --scan の範囲指定
void TestRecplutoChannels() {
    using namespace isdbt::mirakurun;
    System sys;
    int ch = 0;
    double freq = 0.0;
    bool ok = ParseChannel("27", &sys, &ch, &freq) && sys == System::kIsdbt && ch == 27 && std::fabs(freq - 557'142'857.0) < 1.0;
    ok = ok && ParseChannel("13ch", &sys, &ch, &freq) && ch == 13;
    ok = ok && ParseChannel("C56", &sys, &ch, &freq) && sys == System::kIsdbc && ch == 72 + (56 - 23) && std::fabs(freq - 423e6) < 1.0;
    ok = ok && ParseChannel("c13ch", &sys, &ch, &freq) && ch == 62 && std::fabs(freq - 111e6) < 1.0;
    ok = ok && ChannelLabel(System::kIsdbc, 72 + (56 - 23)) == "C56" && ChannelLabel(System::kIsdbt, 27) == "27";
    for (const char* bad : {"12", "63", "C12", "C64", "BS01_0", "", "2x", "C"}) ok = ok && !ParseChannel(bad, &sys, &ch, &freq);
    Report(ok, "recpluto_channel_spec");
    std::vector<std::string> t;
    ok = ScanTargets("gr", &t) && t.size() == 50 && t.front() == "13" && t.back() == "62";
    t.clear();
    ok = ok && ScanTargets("catv:C13-C22", &t) && t.size() == 10 && t.front() == "C13" && t.back() == "C22";
    t.clear();
    ok = ok && ScanTargets("all", &t) && t.size() == 50 + 51;
    t.clear();
    ok = ok && ScanTargets("gr:24-27", &t) && t.size() == 4 && t[0] == "24";
    t.clear();
    ok = ok && !ScanTargets("bs", &t) && !ScanTargets("gr:27-24", &t) && !ScanTargets("all:1-2", &t);
    Report(ok, "recpluto_scan_targets");
}

// 8bit 量子化ビタビ（ライブ経路、AVX2 / NEON）: 同じ軟値列を ViterbiDecodeSoft（量子化込み）と
// ViterbiDecodeQ（量子化済み）で復号して情報ビットに一致すること。どちらも無いビルドでは float 版に落ちる
void TestViterbiQ() {
    NpyArray soft = NpyRead(Path("viterbi_soft.npy"));
    NpyArray want = NpyRead(Path("viterbi_bits.npy"));
    CodeRate rate{3, 4};
    const size_t n = soft.elements();
    const float* sp = soft.as<float>();
    const size_t head = static_cast<size_t>(JNum("viterbi_skip_head", 96));
    const size_t tail = static_cast<size_t>(JNum("viterbi_skip_tail", 64));
    auto count_err = [&](const std::vector<uint8_t>& bits) {
        const size_t m = std::min(bits.size(), want.elements());
        size_t nerr = 0;
        for (size_t i = head; i + tail < m; ++i) nerr += bits[i] != want.as<uint8_t>()[i];
        return nerr;
    };
    const size_t e_soft = count_err(ViterbiDecodeSoft(sp, n, rate));
    Report(e_soft == 0, "viterbi_decode_soft_q", std::to_string(e_soft) + " bit errors");
    // 量子化済み版: ViterbiDecodeSoft と同じ尺度（平均振幅 12）で ±kViterbiSoftQ に丸める
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += std::fabs(sp[i]);
    const double scale = 12.0 * static_cast<double>(n) / acc;
    std::vector<int8_t> qs(n);
    for (size_t i = 0; i < n; ++i) {
        long r = std::lround(sp[i] * scale);
        r = std::max<long>(-kViterbiSoftQ, std::min<long>(kViterbiSoftQ, r));
        qs[i] = static_cast<int8_t>(r);
    }
    const size_t e_q = count_err(ViterbiDecodeQ(qs.data(), n, rate));
    Report(e_q == 0, "viterbi_decode_q", std::to_string(e_q) + " bit errors");
}

void TestByteDeinterleave() {
    NpyArray in = NpyRead(Path("byte_deint_in.npy"));
    NpyArray want = NpyRead(Path("byte_deint_out.npy"));
    std::vector<uint8_t> out(in.data.size());
    ByteDeinterleave(in.data.data(), in.rows(), out.data());
    Report(std::memcmp(out.data(), want.data.data(), out.size()) == 0, "byte_deinterleave");
}

void TestFft() {
    NpyArray in = NpyRead(Path("fft_in.npy"));
    NpyArray want = NpyRead(Path("fft_out.npy"));
    std::vector<cfloat> x(in.as<cfloat>(), in.as<cfloat>() + in.elements());
    Fft fft(static_cast<int>(x.size()));
    fft.Forward(x.data());
    double e = RelRmsError(x.data(), want.as<cfloat>(), x.size());
    Report(e < 1e-3, "fft_8192", "rel rms err = " + std::to_string(e));
}

void TestFracResample() {
    NpyArray in = NpyRead(Path("frac_in.npy"));
    NpyArray want = NpyRead(Path("frac_out.npy"));
    std::vector<cfloat> x(in.as<cfloat>(), in.as<cfloat>() + in.elements());
    std::vector<cfloat> y = ResampleFractional(x, JNum("frac_ratio"), static_cast<long long>(JNum("frac_m0", 0)),
                                               static_cast<long long>(JNum("frac_m1", 0)));
    bool ok = y.size() == want.elements();
    double e = ok ? RelRmsError(y.data(), want.as<cfloat>(), y.size()) : 1.0;
    Report(ok && e < 1e-4, "resample_fractional", "rel rms err = " + std::to_string(e));
}

void TestDemap() {
    NpyArray in = NpyRead(Path("demap64_in.npy"));
    NpyArray want = NpyRead(Path("demap64_out.npy"));
    std::vector<float> out(in.elements() * 6);
    for (size_t i = 0; i < in.elements(); ++i) DemapSoft(in.as<cfloat>()[i], Modulation::QAM64, &out[i * 6]);
    double e = RelRmsError(out.data(), want.as<float>(), out.size());
    Report(e < 1e-5, "demap_soft_64qam", "rel rms err = " + std::to_string(e));
}

void TestPilotPrbs() {
    NpyArray want = NpyRead(Path("pilot_w_mode3.npy"));
    std::vector<uint8_t> w = PilotPrbs(Mode::M3);
    Report(w.size() == want.elements() &&
           std::memcmp(w.data(), want.data.data(), w.size()) == 0, "pilot_prbs_mode3");
}

void TestTmccFrame() {
    NpyArray bits = NpyRead(Path("tmcc_frame.npy"));
    const uint8_t* b = bits.as<uint8_t>();
    bool ok = SyncWordOf(b) == 0;
    ok = ok && DsccCheck(b + 20);
    TmccInfo info = ParseTmccInfo(Mode::M3, b + 20);
    ok = ok && info.partial_reception == JBool("tmcc_partial");
    const TmccLayer& A = info.layers[0];
    const TmccLayer& B = info.layers[1];
    ok = ok && A.used && std::string(ModulationName(A.mod)) == JStr("tmcc_A_mod") &&
         A.rate.str() == JStr("tmcc_A_rate") && A.interleave == static_cast<int>(JNum("tmcc_A_I")) &&
         A.segments == static_cast<int>(JNum("tmcc_A_segments"));
    ok = ok && B.used && std::string(ModulationName(B.mod)) == JStr("tmcc_B_mod") &&
         B.rate.str() == JStr("tmcc_B_rate") && B.interleave == static_cast<int>(JNum("tmcc_B_I")) &&
         B.segments == static_cast<int>(JNum("tmcc_B_segments"));
    ok = ok && !info.layers[2].used;
    Report(ok, "tmcc_parse");
}

// ---------------------------------------------------------------------------
// パイプライン
// ---------------------------------------------------------------------------
bool CompareLayerTs(const char* name, const LayerResult& lr, int skip_tsp) {
    NpyArray want_ts = NpyRead(Path(std::string("pipe_ts_") + name + ".npy"));
    NpyArray want_err = NpyRead(Path(std::string("pipe_rserr_") + name + ".npy"));
    const size_t n_got = lr.ts.size() / kTsPacket;
    bool ok = n_got == want_ts.rows();
    std::string detail;
    if (!ok) {
        detail = "TSP count " + std::to_string(n_got) + " != " + std::to_string(want_ts.rows());
    } else {
        // 先頭 skip_tsp はバイトデインターリーブの履歴が無く不定なので比較しない
        const size_t off = static_cast<size_t>(skip_tsp) * kTsPacket;
        if (std::memcmp(lr.ts.data() + off, want_ts.data.data() + off, lr.ts.size() - off) != 0) {
            ok = false;
            size_t i = off;
            while (i < lr.ts.size() && lr.ts[i] == want_ts.data.data()[i]) ++i;
            detail = "TS bytes differ at offset " + std::to_string(i) + " (TSP " + std::to_string(i / kTsPacket) + ")";
        }
        const int64_t* we = want_err.as<int64_t>();
        for (size_t i = skip_tsp; i < lr.rs_err.size() && ok; ++i) {
            if ((lr.rs_err[i] < 0) != (we[i] < 0)) {
                ok = false;
                detail = "rs_err sign mismatch at TSP " + std::to_string(i);
            }
        }
    }
    Report(ok, std::string("pipeline_ts_") + name, detail);
    return ok;
}

void TestPipeline() {
    SigmfMeta meta;
    std::vector<cfloat> iq = ReadSigmf(Path("pipe_iq.sigmf-meta"), &meta);
    std::printf("     pipe: %zu samples at %.0f Hz\n", iq.size(), meta.sample_rate);

    // 同期 + フレーム同期のパラメータ比較
    CarrierSource src(iq, meta.sample_rate, Mode::M3, Gi{8});
    const SyncInfo& si = src.info();
    Report(std::abs(si.n_sym - JNum("pipe_n_sym")) <= 1, "sync_n_sym",
           std::to_string(si.n_sym) + " vs " + std::to_string((int)JNum("pipe_n_sym")));
    Report(std::abs(si.clock_ppm - JNum("pipe_clock_ppm")) < 0.5, "sync_clock_ppm",
           std::to_string(si.clock_ppm) + " vs " + std::to_string(JNum("pipe_clock_ppm")));
    Report(std::abs(si.cfo_frac_hz - JNum("pipe_cfo_frac_hz")) < 2.0, "sync_cfo_frac",
           std::to_string(si.cfo_frac_hz) + " vs " + std::to_string(JNum("pipe_cfo_frac_hz")));
    Report(si.cfo_int_bins == static_cast<int>(JNum("pipe_cfo_int_bins")), "sync_cfo_int",
           std::to_string(si.cfo_int_bins));
    const bool same_start = si.gi_start0 == JNum("pipe_gi_start0");
    if (!same_start) {
        std::printf("     note: gi_start0 %.1f != ref %.1f -> Y/H の直接比較はスキップ\n",
                    si.gi_start0, JNum("pipe_gi_start0"));
    }

    FrameSyncInfo fsi = FrameSync(&src, Mode::M3);
    Report(fsi.frame_start == static_cast<int>(JNum("pipe_frame_start")), "frame_start",
           std::to_string(fsi.frame_start) + " vs " + std::to_string((int)JNum("pipe_frame_start")));
    Report(fsi.tmcc_parity_ok == JBool("pipe_parity_ok"), "tmcc_parity_ok");
    Report(fsi.n_frames == static_cast<int>(JNum("pipe_n_frames")), "n_frames",
           std::to_string(fsi.n_frames));

    // 段ごとの比較（front_end 相当: 全長一括）
    if (same_start) {
        const int head = static_cast<int>(JNum("pipe_stage_head", 256));
        Mat<cfloat> Yh = src.Symbols(0, head);
        NpyArray want_y = NpyRead(Path("pipe_Y_head.npy"));
        double ey = RelRmsError(Yh.data(), want_y.as<cfloat>(), Yh.size());
        Report(ey < 2e-3, "stage_Y", "rel rms err = " + std::to_string(ey));

        Mat<cfloat> Yall = src.Symbols(0, src.n_sym());
        Mat<cfloat> H, Xt;
        Mat<float> Wt;
        EqualizeAndDeinterleave(Yall, Mode::M3, fsi.frame_start, fsi.layers, fsi.groups, &H, &Xt, &Wt);
        NpyArray want_h = NpyRead(Path("pipe_H_head.npy"));
        double eh = RelRmsError(H.data(), want_h.as<cfloat>(), static_cast<size_t>(head) * H.cols);
        Report(eh < 2e-3, "stage_H", "rel rms err = " + std::to_string(eh));

        const int r0 = static_cast<int>(JNum("pipe_xt_r0"));
        const int r1 = static_cast<int>(JNum("pipe_xt_r1"));
        NpyArray want_xt = NpyRead(Path("pipe_Xt_rows.npy"));
        NpyArray want_wt = NpyRead(Path("pipe_Wt_rows.npy"));
        double ext = RelRmsError(Xt[r0], want_xt.as<cfloat>(), static_cast<size_t>(r1 - r0) * Xt.cols);
        double ewt = RelRmsError(Wt[r0], want_wt.as<float>(), static_cast<size_t>(r1 - r0) * Wt.cols);
        Report(ext < 5e-3, "stage_Xt", "rel rms err = " + std::to_string(ext));
        Report(ewt < 5e-3, "stage_Wt", "rel rms err = " + std::to_string(ewt));
    }

    // 復調本体: TS がビット一致（先頭 11 TSP を除く）
    DemodResult res = Demodulate(std::move(iq), meta.sample_rate, Mode::M3, Gi{8});
    const int skip = static_cast<int>(JNum("pipe_skip_tsp", 11));
    for (const char* name : {"A", "B"}) {
        auto it = res.layers.find(name);
        if (it == res.layers.end()) {
            Report(false, std::string("pipeline_ts_") + name, "layer missing");
            continue;
        }
        Report(it->second.first_frame == static_cast<int>(JNum(std::string("pipe_first_frame_") + name)),
               std::string("first_frame_") + name, std::to_string(it->second.first_frame));
        CompareLayerTs(name, it->second, skip);
    }
}

bool ContainsBlock(const std::vector<uint8_t>& hay, const uint8_t* needle, size_t len) {
    if (hay.size() < len || len == 0) return false;
    for (size_t i = 0; i + len <= hay.size(); ++i) {
        if (std::memcmp(hay.data() + i, needle, len) == 0) return true;
    }
    return false;
}

// 追従: クロックドリフト（±15 ppm ランプ）+ CFO で TS が送信内容と一致する
void TestTrackedDrift() {
    SigmfMeta meta;
    std::vector<cfloat> iq = ReadSigmf(Path("track_iq.sigmf-meta"), &meta);
    TrackedResult res = DemodulateTracked(std::move(iq), meta.sample_rate, Mode::M3, Gi{8});
    int decoded = 0;
    for (const auto& s : res.segments) decoded += s.decoded;
    Report(decoded >= 1, "tracked_drift_segments", std::to_string(decoded) + " decoded");
    for (const char* name : {"A", "B"}) {
        NpyArray tx = NpyRead(Path(std::string("track_tx_") + name + ".npy"));
        const size_t n = tx.rows();
        const auto& ts = res.layers[name].ts;
        const bool found = ContainsBlock(ts, tx.data.data() + (n / 2) * kTsPacket,
                                         (n - n / 2) * kTsPacket);
        Report(found, std::string("tracked_drift_ts_") + name,
               "tx tail not found (" + std::to_string(ts.size() / kTsPacket) + " TSP out)");
    }
}

// 追従: 0.3 s の瞬断を挟んで前後の区間が再同期される
void TestTrackedDropout() {
    SigmfMeta meta;
    std::vector<cfloat> iq = ReadSigmf(Path("drop_iq.sigmf-meta"), &meta);
    TrackedResult res = DemodulateTracked(std::move(iq), meta.sample_rate, Mode::M3, Gi{8});
    int decoded = 0;
    for (const auto& s : res.segments) decoded += s.decoded;
    Report(decoded >= 2, "tracked_dropout_segments", std::to_string(decoded) + " decoded");
    NpyArray tx = NpyRead(Path("drop_tx_B.npy"));
    const auto& ts = res.layers["B"].ts;
    const size_t tpf = 2592 * kTsPacket;
    bool early = false, late = false;
    for (int k : {0, 1, 2}) early = early || ContainsBlock(ts, tx.data.data() + k * tpf, tpf);
    for (int k : {5, 6, 7}) late = late || ContainsBlock(ts, tx.data.data() + k * tpf, tpf);
    Report(early, "tracked_dropout_ts_pre", "pre-gap TS missing");
    Report(late, "tracked_dropout_ts_post", "post-gap TS missing (resync failed)");
}

// 7.0 MSPS 入力（リサンプル経路）: 期待 TS の後半が出力に連続して現れること
// RateMonitor（rt/demod/rate_monitor.h）: 時刻を明示的に渡せるので実時間なしで検証できる
void TestRateMonitor() {
    // 公称どおり: fs=1000 Hz、0.1 s ごとに 100 サンプル → 窓 1 s で 1000 Hz、不足なし
    {
        RateMonitor m(1000.0, 1.0, 0.99);
        bool closed = false;
        for (int i = 0; i <= 10; ++i) closed |= m.Feed(100, i * 0.1);
        Report(closed && std::abs(m.rate() - 1000.0) < 1e-6 && m.slow() == 0,
               "rate_monitor_nominal",
               "rate=" + std::to_string(m.rate()) + " slow=" + std::to_string(m.slow()));
    }
    // 2% 落ち: 0.1 s ごとに 98 サンプル → 980 Hz < 990 Hz で不足 1 回
    {
        RateMonitor m(1000.0, 1.0, 0.99);
        for (int i = 0; i <= 10; ++i) m.Feed(98, i * 0.1);
        Report(m.slow() == 1 && m.last_slow(), "rate_monitor_slow",
               "rate=" + std::to_string(m.rate()) + " slow=" + std::to_string(m.slow()));
    }
    // Reset 後は窓を仕切り直す: 長い無受信（再接続）をまたいでも不足と誤計上しない
    {
        RateMonitor m(1000.0, 1.0, 0.99);
        for (int i = 0; i <= 10; ++i) m.Feed(100, i * 0.1);     // 1 窓目: 正常
        m.Reset();
        // 100 s 後に受信再開。Reset していなければ (次の窓が) 大幅な不足になるところ
        for (int i = 0; i <= 10; ++i) m.Feed(100, 100.0 + i * 0.1);
        Report(m.slow() == 0 && std::abs(m.rate() - 1000.0) < 1e-6, "rate_monitor_reset",
               "rate=" + std::to_string(m.rate()) + " slow=" + std::to_string(m.slow()));
    }
}

void TestPipeline7() {
    SigmfMeta meta;
    std::vector<cfloat> iq = ReadSigmf(Path("pipe7_iq.sigmf-meta"), &meta);
    DemodResult res = Demodulate(std::move(iq), meta.sample_rate, Mode::M3, Gi{8});
    for (const char* name : {"A", "B"}) {
        NpyArray want = NpyRead(Path(std::string("pipe_tx_") + name + ".npy"));
        const size_t n = want.rows();
        const uint8_t* tail = want.data.data() + (n / 2) * kTsPacket;
        const size_t tail_len = (n - n / 2) * kTsPacket;
        const auto& ts = res.layers[name].ts;
        bool found = false;
        if (ts.size() >= tail_len) {
            for (size_t i = 0; i + tail_len <= ts.size(); i += 1) {
                if (std::memcmp(ts.data() + i, tail, tail_len) == 0) { found = true; break; }
            }
        }
        Report(found, std::string("pipeline7_ts_") + name,
               "tx tail not found in output (" + std::to_string(ts.size() / kTsPacket) + " TSP)");
    }
}

// ---------------------------------------------------------------------------
// ISDB-C（rt/isdbc）: ref/isdbc のベクタと照合
// ---------------------------------------------------------------------------
void TestIsdbcMapping() {
    NpyArray c = NpyRead(Path("isdbc_constellation.npy"));
    const cfloat* want = c.as<cfloat>();
    const cfloat* got = isdbc::Constellation();
    bool ok = c.elements() == 64;
    for (int i = 0; ok && i < 64; ++i) ok = std::abs(got[i] - want[i]) < 1e-6f;
    for (int i = 0; ok && i < 64; ++i) ok = isdbc::DemapHard(got[i]) == i;
    Report(ok, "isdbc_constellation");

    NpyArray words = NpyRead(Path("isdbc_words.npy"));
    NpyArray enc = NpyRead(Path("isdbc_diffenc.npy"));
    std::vector<uint8_t> e(words.elements());
    isdbc::DiffEncode(words.as<uint8_t>(), words.elements(), e.data());
    ok = std::memcmp(e.data(), enc.data.data(), e.size()) == 0;
    isdbc::DiffDecoder dec;
    for (size_t i = 0; ok && i < e.size(); ++i) ok = dec.Decode(e[i]) == words.as<uint8_t>()[i];
    Report(ok, "isdbc_diff_encode_decode");

    NpyArray bytes = NpyRead(Path("isdbc_bytes.npy"));
    NpyArray mt = NpyRead(Path("isdbc_mtuples.npy"));
    std::vector<uint8_t> w(mt.elements());
    isdbc::BytesToMtuples(bytes.as<uint8_t>(), bytes.elements(), w.data());
    Report(std::memcmp(w.data(), mt.data.data(), w.size()) == 0, "isdbc_bytes_to_mtuples");
}

void TestIsdbcFraming() {
    NpyArray stream = NpyRead(Path("isdbc_stream.npy"));
    NpyArray want = NpyRead(Path("isdbc_packets.npy"));
    int off = 0;
    const double ratio = isdbc::SyncScore(stream.as<uint8_t>(), stream.elements(), &off);
    const int junk = static_cast<int>(JNum("isdbc_stream_junk", 57));
    bool ok = off == junk && ratio > 0.8;
    isdbc::FrameDecoder fd;
    std::vector<uint8_t> got;
    const uint8_t* s = stream.as<uint8_t>() + off;
    const size_t n_tsp = (stream.elements() - off) / isdbc::kRsN;
    int bad = 0, n_out = 0;
    uint8_t pkts[isdbc::FrameDecoder::kBatch * 188];
    int nerr[isdbc::FrameDecoder::kBatch];
    auto take = [&](int n) {
        for (int k = 0; k < n; ++k) {
            got.insert(got.end(), pkts + k * 188, pkts + (k + 1) * 188);
            ++n_out;
            if (nerr[k] != 5) ++bad;    // 各 TSP に 5 バイト誤りを入れてある
        }
    };
    for (size_t i = 0; i < n_tsp; ++i) take(fd.Push(s + i * isdbc::kRsN, pkts, nerr));
    take(fd.Flush(pkts, nerr));
    // 出力 i は入力パケット i + 11 に対応（先頭 11 TSP は履歴なし）
    const size_t n_cmp = std::min<size_t>(n_out, want.rows() - 11);
    ok = ok && n_cmp > 100 && bad == 0 &&
         std::memcmp(got.data(), want.data.data() + 11 * 188, n_cmp * 188) == 0;
    Report(ok, "isdbc_frame_decoder", "sync off " + std::to_string(off) + " ratio " + std::to_string(ratio) +
                                      " out " + std::to_string(n_out) + " bad " + std::to_string(bad));
}

void TestIsdbcTsmf() {
    NpyArray h = NpyRead(Path("isdbc_tsmf_header.npy"));
    NpyArray slots = NpyRead(Path("isdbc_tsmf_slots.npy"));
    isdbc::TsmfHeader th;
    bool ok = isdbc::ParseTsmfHeader(h.as<uint8_t>(), &th);
    ok = ok && th.continuity_counter == 9 && th.sync_inverted && th.version == 5 && th.emergency == 1;
    ok = ok && th.available[0] && !th.available[1] && th.available[2];
    ok = ok && th.ts_id[0] == 16625 && th.onid[0] == 4 && th.ts_id[2] == 16592;
    for (int i = 0; ok && i < isdbc::kTsmfDataSlots; ++i) ok = th.slot_ts[i] == slots.as<int64_t>()[i];
    std::vector<uint8_t> bad(h.data);
    bad[100] ^= 1;                      // 1 ビット壊すと CRC で弾く
    ok = ok && !isdbc::ParseTsmfHeader(bad.data(), &th);
    Report(ok, "isdbc_tsmf_header");
    Report(isdbc::Crc32Mpeg(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0x0376E6E7u, "isdbc_crc32");
}

// 連続一致区間の長さ（want の 1/4 地点をアンカーにする）
size_t MatchedPackets(const std::vector<uint8_t>& got, const NpyArray& want) {
    const size_t n = want.rows();
    const uint8_t* anchor = want.data.data() + (n / 4) * 188;
    size_t pos = std::string::npos;
    for (size_t i = 0; i + 188 <= got.size(); i += 188) {
        if (std::memcmp(got.data() + i, anchor, 188) == 0) { pos = i / 188; break; }
    }
    if (pos == std::string::npos) return 0;
    const size_t m = std::min(got.size() / 188 - pos, n - n / 4);
    size_t matched = 0;
    for (size_t k = 0; k < m; ++k) {
        if (std::memcmp(got.data() + (pos + k) * 188, want.data.data() + (n / 4 + k) * 188, 188) != 0) break;
        ++matched;
    }
    return matched;
}

// ループバック SigMF（TSMF 2 本、CFO −12 kHz、+16 ppm）を小さなブロックで Feed し、
// 相対 TS 2 が元 TS と一致すること
void TestIsdbcDemod() {
    SigmfMeta meta;
    std::vector<cfloat> iq = ReadSigmf(Path("isdbc_iq"), &meta);
    NpyArray want = NpyRead(Path("isdbc_ts2.npy"));
    isdbc::IsdbcConfig cfg;
    cfg.fs = meta.sample_rate;
    cfg.select.rel = 2;
    std::vector<uint8_t> got;
    isdbc::IsdbcDemod demod(cfg, [&](const uint8_t* pk, size_t n) { got.insert(got.end(), pk, pk + n * 188); });
    const auto t0 = std::chrono::steady_clock::now();
    const size_t blk = 65536;
    for (size_t i = 0; i < iq.size(); i += blk) demod.Feed(iq.data() + i, std::min(blk, iq.size() - i));
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const isdbc::IsdbcStatus st = demod.status();
    const size_t matched = MatchedPackets(got, want);
    const size_t n = want.rows();
    const double cfo_want = JNum("isdbc_cfo_hz", 0.0), ppm_want = JNum("isdbc_clock_ppm", 0.0);
    // 変調器の出力は末尾で途切れる（RRC の裾 + 無信号）ので、最後の十数 TSP は訂正不能になり
    // バイト同期も最後に外れうる。先頭から 90% 以上の連続一致と、尻尾以外の RS 誤りなしを要求する
    const bool ok = st.rs_uncorrectable <= 16 && st.sync_losses <= 1 && st.reacquisitions == 0 &&
                    matched >= (n - n / 4) * 9 / 10 &&
                    std::fabs(st.cfo_hz - cfo_want) < 300.0 && std::fabs(st.clock_ppm - ppm_want) < 3.0 &&
                    st.tsmf_selected == 2 && st.tsmf_streams.size() == 2 && st.mer_db > 26.0;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "locked %d mer %.1f cfo %.0f ppm %.1f rs_bad %lld/%lld matched %zu/%zu sync_losses %lld "
                  "reacq %lld tsmf sel %d (%.2fs for %.2fs of I/Q)",
                  st.locked ? 1 : 0, st.mer_db, st.cfo_hz, st.clock_ppm, st.rs_uncorrectable, st.rs_packets,
                  matched, n - n / 4, st.sync_losses, st.reacquisitions, st.tsmf_selected, sec,
                  iq.size() / meta.sample_rate);
    Report(ok, "isdbc_demod_loopback", buf);
    if (ok) std::printf("     (%s)\n", buf);
}

// 実放送キャプチャ（testdata/captured/catv_56ch_g30、gitignore）があれば復調して RS 誤り 0 を確認
void TestIsdbcReal() {
    const std::string path = g_dir + "/../captured/catv_56ch_g30";
    SigmfMeta meta;
    std::vector<cfloat> iq;
    try {
        iq = ReadSigmf(path, &meta);
    } catch (const std::exception&) {
        std::printf("skip isdbc_real (testdata/captured/catv_56ch_g30 が無い)\n");
        return;
    }
    isdbc::IsdbcConfig cfg;
    cfg.fs = meta.sample_rate;
    long long n_out = 0;
    unsigned pat_tsid = 0;
    isdbc::IsdbcDemod demod(cfg, [&](const uint8_t* pk, size_t n) {
        n_out += static_cast<long long>(n);
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* p = pk + i * 188;
            if ((((p[1] & 0x1F) << 8) | p[2]) == 0 && (p[1] & 0x40) && pat_tsid == 0) {
                pat_tsid = static_cast<unsigned>((p[8] << 8) | p[9]);
            }
        }
    });
    const auto t0 = std::chrono::steady_clock::now();
    const size_t blk = 1 << 18;
    for (size_t i = 0; i < iq.size(); i += blk) demod.Feed(iq.data() + i, std::min(blk, iq.size() - i));
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const isdbc::IsdbcStatus st = demod.status();
    char buf[320];
    std::snprintf(buf, sizeof(buf), "locked %d mer %.1f cfo %.0f ppm %.1f rs_bad %lld/%lld out %lld pat_tsid %u "
                  "tsmf sel %d streams %zu (%.2fs for %.2fs)",
                  st.locked ? 1 : 0, st.mer_db, st.cfo_hz, st.clock_ppm, st.rs_uncorrectable, st.rs_packets,
                  n_out, pat_tsid, st.tsmf_selected, st.tsmf_streams.size(), sec, iq.size() / meta.sample_rate);
    const bool ok = st.locked && st.rs_uncorrectable == 0 && st.rs_packets > 5000 && pat_tsid == 16625 &&
                    st.mer_db > 28.0 && !st.inverted;
    Report(ok, "isdbc_real_catv56ch", buf);
    if (ok) std::printf("     (%s)\n", buf);
}

// --bench: FEC 段の速さ（ビルド・CPU ごとの比較用。AVX2 / NEON / スカラーの切り分けに使う）。
// 入力は乱数でよい: ビタビの計算量はデータに依らず、RS は「誤り無し（シンドロームだけ）」が実運用の大半
void Bench() {
    auto now = [] { return std::chrono::steady_clock::now(); };
    auto sec = [&](auto a) { return std::chrono::duration<double>(now() - a).count(); };
    const CodeRate rate{3, 4};
    // 64QAM 3/4 の 12 セグメント ≈ 1 フレーム（0.23 s）あたり 2.6 M 軟値。10 フレーム分
    const size_t n_soft = 26'000'000;
    std::vector<float> soft(n_soft);
    uint32_t x = 12345;
    for (float& v : soft) {
        x = x * 1664525u + 1013904223u;
        v = ((x >> 16) & 1 ? 1.0f : -1.0f) + (static_cast<float>(x & 0xFFFF) / 65536.0f - 0.5f) * 0.6f;
    }
    {
        std::vector<float> pairs;
        const size_t steps = Depuncture(soft.data(), n_soft, rate, &pairs);
        const auto t0 = now();
        std::vector<uint8_t> bits = ViterbiDecode(pairs.data(), steps);
        const double t = sec(t0);
        std::printf("viterbi float  : %7.3f s  %6.1f Msteps/s  (%zu steps)\n", t, steps / t / 1e6, steps);
    }
    {
        const auto t0 = now();
        std::vector<uint8_t> bits = ViterbiDecodeSoft(soft.data(), n_soft, rate);
        const double t = sec(t0);
        std::printf("viterbi u8     : %7.3f s  %6.1f Msteps/s  (%zu steps, 量子化込み)\n", t, bits.size() / t / 1e6, bits.size());
    }
    {
        // FFT 8192: Mode 3 は 992 回/秒
        Fft fft(8192);
        std::vector<std::complex<float>> buf(8192);
        for (size_t i = 0; i < buf.size(); ++i) buf[i] = {static_cast<float>(i % 7) - 3.0f, static_cast<float>(i % 5) - 2.0f};
        const int reps = 4000;
        const auto t0 = now();
        for (int r = 0; r < reps; ++r) fft.Forward(buf.data());
        const double t = sec(t0);
        std::printf("fft 8192       : %7.3f s  %6.1f k FFT/s  (%d 回)\n", t, reps / t / 1e3, reps);
    }
    {
        const size_t n_tsp = 200'000;      // 64QAM 3/4 の 12 セグメントで約 36 s 分
        std::vector<uint8_t> tsps(n_tsp * kRsN, 0);      // 全 0 は符号語（誤り無し = シンドロームだけで終わる）
        std::vector<int> nerr(n_tsp);
        const auto t0 = now();
        RsDecode(tsps.data(), n_tsp, nerr.data());
        const double t = sec(t0);
        std::printf("rs clean       : %7.3f s  %6.1f k TSP/s\n", t, n_tsp / t / 1e3);
        // 1 TSP に 4 バイト誤り（Berlekamp-Massey まで回る）
        for (size_t i = 0; i < n_tsp; ++i) {
            for (int k = 0; k < 4; ++k) {
                x = x * 1664525u + 1013904223u;
                tsps[i * kRsN + (x >> 8) % kRsN] ^= static_cast<uint8_t>((x >> 24) | 1);
            }
        }
        const auto t1 = now();
        RsDecode(tsps.data(), n_tsp, nerr.data());
        const double t2 = sec(t1);
        size_t fixed = 0;
        for (int e : nerr) fixed += (e >= 1 && e <= 4);
        std::printf("rs 4 errors    : %7.3f s  %6.1f k TSP/s  (%zu/%zu corrected)\n", t2, n_tsp / t2 / 1e3, fixed, n_tsp);
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rt_test <vectors_dir> | rt_test --bench\n");
        return 2;
    }
    if (std::string(argv[1]) == "--bench") {
        Bench();
        return 0;
    }
    g_dir = argv[1];
    try {
        std::vector<uint8_t> j = ReadFileBytes(Path("expected.json"));
        g_json.assign(j.begin(), j.end());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "cannot read expected.json: %s\n(tools/gen_rt_vectors.py を先に実行)\n", e.what());
        return 2;
    }
    try {
        TestRateMonitor();
        TestPrbs();
        TestRs();
        TestViterbi();
        TestViterbiQ();
        TestRecplutoChannels();
        TestByteDeinterleave();
        TestFft();
        TestFracResample();
        TestDemap();
        TestPilotPrbs();
        TestTmccFrame();
        TestPipeline();
        TestPipeline7();
        if (JBool("track_present")) {
            TestTrackedDrift();
            TestTrackedDropout();
        } else {
            std::printf("skip tracked tests (track ベクタ未生成: tools/gen_rt_vectors.py を再実行)\n");
        }
        if (JNum("isdbc_fs", 0.0) > 0.0) {
            TestIsdbcMapping();
            TestIsdbcFraming();
            TestIsdbcTsmf();
            TestIsdbcDemod();
            TestIsdbcReal();
        } else {
            std::printf("skip isdbc tests (isdbc ベクタ未生成: tools/gen_rt_vectors.py を再実行)\n");
        }
    } catch (const std::exception& e) {
        std::printf("FAIL exception: %s\n", e.what());
        ++g_fail;
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
