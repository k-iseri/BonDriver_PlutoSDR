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

using namespace isdbt;

namespace {

std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

void Usage() {
    std::fprintf(stderr,
        "usage: recpluto [options] <channel> <rectime> <destfile>\n"
        "       recpluto [options] --scan [gr|catv|all|gr:24-27|catv:C13-C63]\n"
        "  channel   地デジ UHF 物理チャンネル 13..62、またはケーブル TV トラモジ（BS/CS 再送信）C13..C63\n"
        "  rectime   秒数、- で止めるまで（SIGTERM / SIGINT で止まる）\n"
        "  destfile  ファイル名、- で標準出力\n"
        "options:\n"
        "  --device URI        libiio の URI (default usb:)。Pluto+ / LibreSDR の LAN なら ip:192.168.2.1\n"
        "  --fpga-eq auto|0|1  FPGA 前段（pluto-fw-isdbt v2）を使う (default auto)\n"
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

// "27" / "27ch" → 地デジ物理 ch、"C56" / "C56ch" → CATV 通し番号（isdbc::CatvChannel の index）
bool ParseChannel(std::string s, System* sys, int* ch, double* freq) {
    if (s.size() > 2 && (s.compare(s.size() - 2, 2, "ch") == 0 || s.compare(s.size() - 2, 2, "CH") == 0)) s.resize(s.size() - 2);
    if (s.empty()) return false;
    if (s[0] == 'C' || s[0] == 'c') {
        const int n = std::atoi(s.c_str() + 1);
        int index;
        if (n >= 13 && n <= 22) index = 62 + (n - 13);
        else if (n >= 23 && n <= 63) index = 72 + (n - 23);
        else return false;
        *sys = System::kIsdbc;
        *ch = index;
        return isdbc::CatvChannel(index, freq, nullptr);
    }
    for (char c : s) if (c < '0' || c > '9') return false;
    const int n = std::atoi(s.c_str());
    if (n < 13 || n > 62) return false;
    *sys = System::kIsdbt;
    *ch = n;
    *freq = ChannelToHz(n);
    return true;
}

std::string ChannelLabel(System sys, int ch) {
    if (sys == System::kIsdbt) return std::to_string(ch);
    std::string name;
    isdbc::CatvChannel(ch, nullptr, &name);        // "C56ch"
    if (name.size() > 2) name.resize(name.size() - 2);
    return name;
}

// --scan の対象を channel 文字列の列にする。"gr" = 13..62、"catv" = C13..C63、"gr:24-27" / "catv:C13-C22" で範囲
bool ScanTargets(const std::string& spec, std::vector<std::string>* out) {
    std::string kind = spec, range;
    const size_t colon = spec.find(':');
    if (colon != std::string::npos) { kind = spec.substr(0, colon); range = spec.substr(colon + 1); }
    auto add = [&](bool catv) {
        int lo = catv ? 13 : 13, hi = catv ? 63 : 62;
        if (!range.empty()) {
            const size_t dash = range.find('-');
            if (dash == std::string::npos) return false;
            std::string a = range.substr(0, dash), b = range.substr(dash + 1);
            if (!a.empty() && (a[0] == 'C' || a[0] == 'c')) a.erase(0, 1);
            if (!b.empty() && (b[0] == 'C' || b[0] == 'c')) b.erase(0, 1);
            lo = std::atoi(a.c_str());
            hi = std::atoi(b.c_str());
            if (lo <= 0 || hi < lo) return false;
        }
        for (int n = lo; n <= hi; ++n) out->push_back((catv ? "C" : "") + std::to_string(n));
        return true;
    };
    if (kind == "gr") return add(false);
    if (kind == "catv") return add(true);
    if (kind == "all" || kind.empty()) return add(false) && (range.clear(), add(true));
    return false;
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

int Scan(LiveSwitch* live, const std::vector<std::string>& targets, double wait_s, bool verbose) {
    std::printf("# recpluto --scan: 受信できたチャンネル（channels.yml の形式。name は局名に書き換える）\n");
    int found = 0;
    for (const std::string& t : targets) {
        if (g_stop) break;
        System sys;
        int ch;
        double freq;
        if (!ParseChannel(t, &sys, &ch, &freq)) continue;
        live->TuneSystem(sys, true, ch, freq);
        double sig = 0.0;
        const bool ok = WaitLock(live, wait_s, &sig);
        const std::string label = ChannelLabel(sys, ch);
        if (verbose) std::fprintf(stderr, "%-5s %7.3f MHz  %s  %.1f dB\n", label.c_str(), freq / 1e6, ok ? "locked" : "-", sig);
        if (!ok) continue;
        ++found;
        if (sys == System::kIsdbt) {
            std::printf("- name: GR%s\n  type: GR\n  channel: '%s'          # MER %.1f dB\n", label.c_str(), label.c_str(), sig);
        } else {
            // BS/CS の区別と serviceId は TS を見て決める（Mirakurun はサービス走査で埋める）
            std::printf("- name: %s\n  type: BS\n  channel: '%s'          # ISDB-C MER %.1f dB。CS なら type: CS\n", label.c_str(), label.c_str(), sig);
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
    auto next = [&](int& i) -> std::string {
        if (i + 1 >= argc) { Usage(); std::exit(2); }
        return argv[++i];
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--device") sdr_cfg.uri = next(i);
        else if (a == "--fs") sdr_cfg.fs = std::atof(next(i).c_str());
        else if (a == "--bw") { sdr_cfg.bw = std::atof(next(i).c_str()); bw_fixed = true; }
        else if (a == "--gain") {
            const std::string g = next(i);
            if (g == "hw") sdr_cfg.hw_agc = true;
            else { sdr_cfg.agc = false; sdr_cfg.gain_db = std::atof(g.c_str()); }
        }
        else if (a == "--agc-target") sdr_cfg.agc_target_dbfs = std::atof(next(i).c_str());
        else if (a == "--fpga-eq") { const std::string v = next(i); sdr_cfg.fpga_eq = (v == "0") ? 0 : (v == "1") ? 1 : -1; }
        else if (a == "--no-reconnect") sdr_cfg.reconnect = false;
        else if (a == "--win-frames") win_frames = std::atoi(next(i).c_str());
        else if (a == "--ctx-frames") ctx_frames = std::atoi(next(i).c_str());
        else if (a == "--workers") workers = std::atoi(next(i).c_str());
        else if (a == "--first-burst") first_burst_str = next(i);
        else if (a == "--tsmf" || a == "--rel-ts") {
            const std::string v = next(i);
            if (v == "all") ts_sel.pass_all = true;
            else ts_sel.rel = std::atoi(v.c_str());
        }
        else if (a == "--scan") {
            scan = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') scan_spec = next(i);
        }
        else if (a == "--scan-wait") scan_wait = std::atof(next(i).c_str());
        else if (a == "--quiet") quiet = true;
        else if (a == "--help" || a == "-h") { Usage(); return 0; }
        // recpt1 互換（無視）
        else if (a == "--b25" || a == "--strip" || a == "--EMM" || a == "--udp" || a == "--http") {}
        else if (a == "--round" || a == "--sid" || a == "--addr" || a == "--port" || a == "--lnb") next(i);
        else if (a.size() > 1 && a[0] == '-' && a != "-") { Usage(); return 2; }
        else pos.push_back(a);
    }

    System sys = System::kIsdbt;
    int ch = 0;
    double freq = 0.0, rectime = 0.0;
    std::string dest = "-";
    std::vector<std::string> scan_targets;
    if (scan) {
        if (!ScanTargets(scan_spec, &scan_targets) || scan_targets.empty()) {
            std::fprintf(stderr, "bad --scan: %s\n", scan_spec.c_str());
            return 2;
        }
        if (!ParseChannel(scan_targets[0], &sys, &ch, &freq)) return 2;
    } else {
        if (pos.size() != 3) { Usage(); return 2; }
        if (!ParseChannel(pos[0], &sys, &ch, &freq)) {
            std::fprintf(stderr, "bad channel: %s（地デジ 13..62 か CATV C13..C63）\n", pos[0].c_str());
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
