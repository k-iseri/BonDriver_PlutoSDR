// recpluto: Mirakurun / EPGStation 向けのチューナーコマンド（recpt1 互換の引数）。
//   recpluto [options] <channel> <rectime> <destfile>
//     channel  … 地デジ UHF の物理チャンネル（13〜62）か、ケーブル TV トラモジ（ISDB-C）の物理チャンネル
//                （C13〜C63。BS/CS の再送信）。衛星の直接受信（BS01_0 など）は対象外
//     rectime  … 秒数。- で止めるまで（Mirakurun は - を渡し、チャンネル切替・停止で SIGTERM を送る）
//     destfile … ファイル名。- で標準出力
// Pluto SDR を libiio で開き、rt/demod の LiveSwitch（isdbtd --sdr / BonDriver と同じ復調パイプライン）で
// TS を出す。recpt1 の --b25 / --strip / --sid などは受け付けて無視する（スクランブル解除は Mirakurun の
// decoder: に任せる。本リポジトリの範囲外）。
// --scan で受信できるチャンネルを調べ、channels.yml の形で出す。
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../demod/live_switch.h"
#include "../isdbc/params.h"
#include "channel_spec.h"

using namespace isdbt;
using isdbt::mirakurun::ChannelLabel;
using isdbt::mirakurun::HintFromSystem;
using isdbt::mirakurun::HintFromType;
using isdbt::mirakurun::ParseChannel;
using isdbt::mirakurun::ScanTarget;
using isdbt::mirakurun::ScanTargets;
using isdbt::mirakurun::SystemHint;

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

void Usage() {
    std::fprintf(stderr,
        "usage: recpluto [options] <channel> <rectime> <destfile>\n"
        "       recpluto [options] --scan [gr|catv|all|gr:24-27|catv:C13-C63]\n"
        "  channel   UHF 物理チャンネル 13..62、またはケーブル TV の C 帯 C13..C63。数字は --type / --system が\n"
        "            無ければ地デジ、BS / CS / SKY か isdbc ならケーブルの UHF 帯トラモジ（BS/CS 再送信）\n"
        "  rectime   秒数、- で止めるまで（SIGTERM / SIGINT で止まる）\n"
        "  destfile  ファイル名、- で標準出力\n"
        "options:\n"
        "  --type GR|BS|CS|SKY Mirakurun の <type>（GR = 地デジ、他 = ケーブル）。--type=<type> と書ける\n"
        "  --system isdbt|isdbc 方式を直接指定（--type より優先）\n"
        "  --tsid N            ISDB-C: TSMF の TS を TS_id で選ぶ（--onid N も合わせられる）。--tsid=<tsid> と書け、\n"
        "                      値が空か 0 なら自動（--tsmf と同じ）\n"
        "  --device URI        libiio の URI (default usb:)。Pluto+ / LibreSDR の LAN なら ip:192.168.2.1\n"
        "  --fpga-eq auto|0|1  FPGA 前段（pluto-fw-isdbt v2）を使う (default auto)\n"
        "  --xo-ppm auto|0|F   XO 補正（auto = 同期で測ったクロック偏差を xo_correction に書く。default auto）\n"
        "  --gain F | hw       手動ゲイン dB / AD936x の slow_attack AGC。省略時はソフト AGC\n"
        "  --agc-target F      ソフト AGC の目標帯域内レベル dBFS (default -24)\n"
        "  --fs F              取り込みレート Hz（省略時はファームのサンプル幅で自動）\n"
        "  --bw F              RF 帯域幅 Hz を固定（省略時は方式ごとの既定）\n"
        "  --tsmf N | all      ISDB-C: TSMF の相対 TS 番号（既定は自動、all = 全スロット通過）\n"
        "  --win-frames N / --ctx-frames N / --workers N / --first-burst psi|all|none  isdbtd と同じ\n"
        "  --scan-wait F       --scan で 1 チャンネルを待つ秒数 (default 8)\n"
        "  --quiet             10 秒ごとの status（JSON）を stderr に出さない\n"
        "  recpt1 互換で受け付けて無視: --b25 --strip --EMM --round N --sid S --udp --addr H --port P --http P --lnb V\n");
}

// 選局してロックを待つ。戻り値: ロックしたか。signal_db に最後のレベル（MER / C/N）
bool WaitLock(LiveSwitch* live, double wait_s, double* signal_db) {
    const auto t_end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long long>(wait_s * 1000));
    *signal_db = 0.0;
    while (!g_stop && std::chrono::steady_clock::now() < t_end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (live->switching()) continue;
        *signal_db = live->signal_db();
        if (live->locked()) return true;
    }
    return false;
}

int Scan(LiveSwitch* live, const std::vector<ScanTarget>& targets, double wait_s, bool verbose) {
    std::printf("# recpluto --scan: 受信できたチャンネル（channels.yml の形式。name は局名に書き換える）\n");
    int found = 0;
    for (const ScanTarget& t : targets) {
        if (g_stop) break;
        System sys = System::kIsdbt;
        int ch = 0;
        double freq = 0.0;             // 初期化は MSVC の C4701（ParseChannel が失敗時に触らない）対策
        if (!ParseChannel(t.channel, t.hint, &sys, &ch, &freq)) continue;
        live->TuneSystem(sys, true, ch, freq);
        double sig = 0.0;
        const bool ok = WaitLock(live, wait_s, &sig);
        const std::string label = ChannelLabel(sys, ch);
        if (verbose) std::fprintf(stderr, "%-5s %-5s %7.3f MHz  %s  %.1f dB\n", SystemName(sys), label.c_str(), freq / 1e6, ok ? "locked" : "-", sig);
        if (!ok) continue;
        ++found;
        if (sys == System::kIsdbt) {
            std::printf("- name: GR%s\n  type: GR\n  channel: '%s'          # MER %.1f dB\n", label.c_str(), label.c_str(), sig);
        } else {
            // BS/CS の区別と serviceId は TS を見て決める（Mirakurun はサービス走査で埋める）
            std::printf("- name: %s\n  type: BS\n  channel: '%s'          # ISDB-C MER %.1f dB。CS なら type: CS、TS は commandVars の tsid で選ぶ\n", label.c_str(), label.c_str(), sig);
        }
        std::fflush(stdout);
    }
    std::fprintf(stderr, "%d channel(s) found\n", found);
    return 0;
}

int Record(LiveSwitch* live, System sys, int ch, double freq, double rectime, const std::string& dest, bool verbose) {
    live->TuneSystem(sys, true, ch, freq);
    FILE* out = stdout;
    if (dest != "-") {
        out = std::fopen(dest.c_str(), "wb");
        if (!out) {
            std::fprintf(stderr, "cannot open %s\n", dest.c_str());
            return 1;
        }
    }
    auto q = live->AddClient();
    const auto t0 = std::chrono::steady_clock::now();
    const bool has_limit = rectime > 0;
    const auto t_end = t0 + std::chrono::milliseconds(static_cast<long long>(rectime * 1000));
    auto last_status = t0;
    long long bytes = 0;
    while (!g_stop) {
        const auto now = std::chrono::steady_clock::now();
        if (has_limit && now >= t_end) break;
        if (verbose && now - last_status >= std::chrono::seconds(10)) {
            std::fprintf(stderr, "%s\n", live->StatusJson().c_str());
            last_status = now;
        }
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(q->mu);
            if (!q->cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return !q->chunks.empty(); })) continue;
            chunk = std::move(q->chunks.front());
            q->chunks.pop_front();
        }
        if (std::fwrite(chunk.data(), 1, chunk.size(), out) != chunk.size() || std::fflush(out) != 0) {
            if (verbose) std::fprintf(stderr, "output closed after %lld bytes\n", bytes);
            break;
        }
        bytes += static_cast<long long>(chunk.size());
    }
    live->RemoveClient(q);
    if (out != stdout) std::fclose(out);
    if (verbose) std::fprintf(stderr, "recorded %lld bytes (%.1f s)%s\n", bytes,
                              std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                              g_stop ? ", signal" : "");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    SdrConfig sdr_cfg;
    bool bw_fixed = false, quiet = false, scan = false;
    std::string scan_spec = "all", first_burst_str;
    double scan_wait = 8.0;
    int win_frames = 8, ctx_frames = 4, workers = 3;
    isdbc::TsmfDemux::Select ts_sel;
    std::vector<std::string> pos;
    SystemHint hint = SystemHint::kAuto;
    bool hint_from_system = false;
    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) { Usage(); std::exit(2); }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        // Mirakurun の command は未定義のプレースホルダが空になるので、--tsid=<tsid> のような "=" 形式を受ける
        std::string eq_value;
        bool has_eq = false;
        if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
            const size_t eq = a.find('=');
            if (eq != std::string::npos) { eq_value = a.substr(eq + 1); a.resize(eq); has_eq = true; }
        }
        auto value = [&]() -> std::string { return has_eq ? eq_value : next(i); };
        if (a.empty()) continue;
        if (a == "--device") sdr_cfg.uri = value();
        else if (a == "--type") {
            const std::string t = value();
            SystemHint h = hint;          // 初期化は MSVC の C4701 対策（HintFromType が失敗時に触らない）
            if (!t.empty() && !HintFromType(t, &h)) { std::fprintf(stderr, "bad --type: %s\n", t.c_str()); return 2; }
            if (!t.empty() && !hint_from_system) hint = h;
        }
        else if (a == "--system") {
            const std::string v = value();
            if (!HintFromSystem(v, &hint)) { std::fprintf(stderr, "bad --system: %s\n", v.c_str()); return 2; }
            hint_from_system = hint != SystemHint::kAuto;
        }
        else if (a == "--tsid") {
            const std::string v = value();
            const long n = v.empty() ? 0 : std::atol(v.c_str());
            ts_sel.tsid = n > 0 ? static_cast<int>(n) : -1;
        }
        else if (a == "--onid") {
            const std::string v = value();
            const long n = v.empty() ? 0 : std::atol(v.c_str());
            ts_sel.onid = n > 0 ? static_cast<int>(n) : -1;
        }
        else if (a == "--fs") sdr_cfg.fs = std::atof(value().c_str());
        else if (a == "--bw") { sdr_cfg.bw = std::atof(value().c_str()); bw_fixed = true; }
        else if (a == "--gain") {
            const std::string g = value();
            if (g == "hw") sdr_cfg.hw_agc = true;
            else { sdr_cfg.agc = false; sdr_cfg.gain_db = std::atof(g.c_str()); }
        }
        else if (a == "--agc-target") sdr_cfg.agc_target_dbfs = std::atof(value().c_str());
        else if (a == "--fpga-eq") { const std::string v = value(); sdr_cfg.fpga_eq = (v == "0") ? 0 : (v == "1") ? 1 : -1; }
        else if (a == "--xo-ppm") {
            const std::string v = value();
            if (v == "auto") sdr_cfg.xo_mode = -1;
            else if (v == "0" || v == "off") sdr_cfg.xo_mode = 0;
            else { sdr_cfg.xo_mode = 1; sdr_cfg.xo_ppm = std::atof(v.c_str()); }
        }
        else if (a == "--no-reconnect") sdr_cfg.reconnect = false;
        else if (a == "--win-frames") win_frames = std::atoi(value().c_str());
        else if (a == "--ctx-frames") ctx_frames = std::atoi(value().c_str());
        else if (a == "--workers") workers = std::atoi(value().c_str());
        else if (a == "--first-burst") first_burst_str = value();
        else if (a == "--tsmf" || a == "--rel-ts") {
            const std::string v = value();
            if (v == "all") ts_sel.pass_all = true;
            else ts_sel.rel = std::atoi(v.c_str());
        }
        else if (a == "--scan") {
            scan = true;
            if (has_eq) scan_spec = eq_value;
            else if (i + 1 < argc && argv[i + 1][0] != '-') scan_spec = next(i);
        }
        else if (a == "--scan-wait") scan_wait = std::atof(value().c_str());
        else if (a == "--quiet") quiet = true;
        else if (a == "--help" || a == "-h") { Usage(); return 0; }
        // recpt1 互換（無視）
        else if (a == "--b25" || a == "--strip" || a == "--EMM" || a == "--udp" || a == "--http") {}
        else if (a == "--round" || a == "--sid" || a == "--addr" || a == "--port" || a == "--lnb") { if (!has_eq) next(i); }
        else if (a.size() > 1 && a[0] == '-' && a != "-") { Usage(); return 2; }
        else pos.push_back(a);
    }

    System sys = System::kIsdbt;
    int ch = 0;
    double freq = 0.0, rectime = 0.0;
    std::string dest = "-";
    std::vector<ScanTarget> scan_targets;
    if (scan) {
        if (!ScanTargets(scan_spec, &scan_targets) || scan_targets.empty()) {
            std::fprintf(stderr, "bad --scan: %s\n", scan_spec.c_str());
            return 2;
        }
        if (!ParseChannel(scan_targets[0].channel, scan_targets[0].hint, &sys, &ch, &freq)) return 2;
    } else {
        if (pos.size() != 3) { Usage(); return 2; }
        if (!ParseChannel(pos[0], hint, &sys, &ch, &freq)) {
            std::fprintf(stderr, "bad channel: %s（UHF 13..62 か CATV C13..C63。ケーブルの UHF 帯は --type BS/CS か --system isdbc）\n", pos[0].c_str());
            return 2;
        }
        rectime = pos[1] == "-" ? 0.0 : std::atof(pos[1].c_str());
        dest = pos[2];
    }

    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        LiveParams lp;
        sdr_cfg.freq_hz = freq;
        lp.sdr = sdr_cfg;
        lp.bw_fixed = bw_fixed;
        lp.win_frames = win_frames;
        lp.ctx_frames = ctx_frames;
        lp.workers = workers;
        lp.select = ts_sel;
        lp.verbose = !quiet;
        if (!ParseFirstBurst(first_burst_str, &lp.first_burst)) throw std::runtime_error("bad --first-burst: " + first_burst_str);
        LiveSwitch live(lp);
        live.Start(sys);
        int rc;
        if (scan) {
            rc = Scan(&live, scan_targets, scan_wait, !quiet);
        } else {
            if (!quiet) std::fprintf(stderr, "recpluto: %s %s (%.3f MHz) -> %s\n", SystemName(sys), ChannelLabel(sys, ch).c_str(), freq / 1e6, dest.c_str());
            rc = Record(&live, sys, ch, freq, rectime, dest, !quiet);
        }
        live.Stop();
        return rc;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "recpluto: %s\n", e.what());
        return 1;
    }
}
