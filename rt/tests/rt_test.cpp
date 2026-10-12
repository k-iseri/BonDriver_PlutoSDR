// rt/isdbt の検証: tools/gen_rt_vectors.py が ref/ から作ったベクタと段ごとに突き合わせる。
// 使い方: rt_test <vectors_dir>
// 整数段（RS、PRBS、バイトデインターリーブ、TMCC、Viterbi 判定）はビット一致、
// 浮動小数段（FFT、リサンプル、等化）は相対 RMS 誤差で判定する。
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../demod/rate_monitor.h"
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rt_test <vectors_dir>\n");
        return 2;
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
    } catch (const std::exception& e) {
        std::printf("FAIL exception: %s\n", e.what());
        ++g_fail;
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
