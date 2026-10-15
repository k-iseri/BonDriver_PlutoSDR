// isdbtd: ISDB-T 復調デーモン（docs/daemon-protocol.md v1 の rt/ 実装）。
//
// tools/demod_server.py（偽デーモン）を置き換える。ソースは 2 つ:
//   --ts    録画済み TS を実速再生（PCR からレート推定、無ければ --bitrate）
//   --sigmf SigMF の I/Q を rt/ の復調コアで復調してから実速再生
// SDR 実機ソース（libiio）はボード到着後に追加する。プロトコルは demod_server.py と同一なので
// BonDriver 側は差し替えに気付かない。
//
// 使い方例:
//   isdbtd --ts C:/rec/ch27.ts --loop
//   isdbtd --sigmf testdata/synthetic/std_mode3_gi8_clean.sigmf-meta --loop
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
inline void CloseSocket(socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
inline void CloseSocket(socket_t s) { close(s); }
#endif

#ifdef _WIN32
#include <corecrt_io.h>     // _setmode（<io.h> は rt/isdbt/io.h と名前が衝突する）
#include <fcntl.h>
#endif

#include <algorithm>
#include <csignal>
#include <cstdarg>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../isdbc/demod.h"
#include "../isdbt/demod.h"
#include "../isdbt/io.h"
#include "../isdbt/params.h"
#include "live_demod.h"
#include "live_switch.h"
#include "sdr_source.h"

using namespace isdbt;

namespace {

constexpr int kProtocolVersion = 2;     // v2: tune/status に system（isdbt|isdbc）。docs/daemon-protocol.md
constexpr int kDefaultPort = 47188;
constexpr double kDefaultBitrate = 16'851'000.0;    // Mode 3 / GI 1/8 / 64QAM 3/4 × 13 セグの概算 TS レート
constexpr size_t kChunkPackets = 64;
// ChannelToHz / FormatDouble / kQueueMaxChunks / kTeiMask / ClientQueue / IFeed / LiveDemod は
// live_demod.h（BonDriver スタンドアロンモードと共用）
std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;     // 制御文字は落とす
        out += c;
    }
    return out;
}

// ---- 最小限の JSON 読み取り（要求は 1 階層・既知キーだけ見る） ----
const char* FindValue(const std::string& json, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return nullptr;
    pos = json.find(':', pos + pat.size());
    if (pos == std::string::npos) return nullptr;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    return json.c_str() + pos;
}

std::string JsonStringValue(const std::string& json, const char* key) {
    const char* v = FindValue(json, key);
    if (!v || *v != '"') return {};
    const char* e = std::strchr(v + 1, '"');
    return e ? std::string(v + 1, e) : std::string();
}

bool JsonNumber(const std::string& json, const char* key, double* out) {
    const char* v = FindValue(json, key);
    if (!v) return false;
    if (std::strncmp(v, "null", 4) == 0) return false;
    char* end = nullptr;
    double d = std::strtod(v, &end);
    if (end == v) return false;
    *out = d;
    return true;
}

// ---------------------------------------------------------------------------
// ソースの用意
// ---------------------------------------------------------------------------
struct Source {
    std::vector<uint8_t> packets;       // 188 バイト境界の連結
    double bitrate = kDefaultBitrate;
    std::string source_desc;
    bool locked = true;
    int mode = 3;
    std::string gi = "1/8";
    std::vector<LayerDesc> layers;
    long long rs_uncorrectable = 0;
    double mer_db = 0.0;                // 復調ソースのとき: 最悪階層の MER（0 = 無し → --signal-db を使う）
};

// PCR からビットレートを推定する（tools/demod_server.py と同じ方法: 最多 PCR PID の最初と最後）
double EstimateBitrateFromPcr(const std::vector<uint8_t>& ts, double fallback) {
    const size_t n = ts.size() / kTsPacket;
    struct Pcr { size_t idx; long long val; int pid; };
    std::vector<Pcr> pcrs;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &ts[i * kTsPacket];
        if (!(p[3] & 0x20) || p[4] < 7 || !(p[5] & 0x10)) continue;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        long long base = (static_cast<long long>(p[6]) << 25) | (p[7] << 17) | (p[8] << 9) |
                         (p[9] << 1) | (p[10] >> 7);
        long long ext = ((p[10] & 1) << 8) | p[11];
        pcrs.push_back({i, base * 300 + ext, pid});
    }
    if (pcrs.size() < 2) return fallback;
    // 最も多い PID
    std::vector<int> pids;
    for (const auto& p : pcrs) pids.push_back(p.pid);
    std::sort(pids.begin(), pids.end());
    int best_pid = pids[0], best_count = 0;
    for (size_t i = 0; i < pids.size();) {
        size_t j = i;
        while (j < pids.size() && pids[j] == pids[i]) ++j;
        if (static_cast<int>(j - i) > best_count) { best_count = static_cast<int>(j - i); best_pid = pids[i]; }
        i = j;
    }
    const Pcr* first = nullptr;
    const Pcr* last = nullptr;
    for (const auto& p : pcrs) {
        if (p.pid != best_pid) continue;
        if (!first) first = &p;
        last = &p;
    }
    if (!first || first == last) return fallback;
    const double dt = (last->val - first->val) / 27e6;
    if (dt <= 0) return fallback;
    return (last->idx - first->idx) * kTsPacket * 8 / dt;
}

Source LoadTsFile(const std::string& path, double bitrate_opt) {
    Source s;
    s.packets = ReadFileBytes(path);
    s.packets.resize(s.packets.size() / kTsPacket * kTsPacket);
    if (s.packets.empty()) throw std::runtime_error(path + ": TS パケットが無い");
    for (size_t i = 0; i < s.packets.size(); i += kTsPacket) {
        if (s.packets[i] != kTsSync) {
            std::fprintf(stderr, "warning: %s: 同期バイトが 0x47 でない TSP がある\n", path.c_str());
            break;
        }
    }
    s.bitrate = bitrate_opt > 0 ? bitrate_opt : EstimateBitrateFromPcr(s.packets, kDefaultBitrate);
    s.source_desc = "replay:" + path;
    return s;
}

// demodulate の階層別出力を OFDM フレーム順（フレーム内は A, B, C の順）に 1 本の TS へ戻す。
// 訂正不能 TSP と先頭 11 TSP には transport_error_indicator を立てる（demod_server.py と同じ近似）
std::vector<uint8_t> MergeLayersByFrame(const DemodResult& res, Mode mode) {
    struct L {
        const RxLayer* lyr;
        std::vector<uint8_t> pk;
        const std::vector<int>* nerr;
        int tpf, start;
    };
    std::vector<L> ls;
    int f_first = 1 << 30, f_last = 0;
    for (const RxLayer& lyr : res.fsi.layers) {
        auto it = res.layers.find(lyr.name);
        if (it == res.layers.end() || it->second.ts.empty()) continue;
        L l;
        l.lyr = &lyr;
        l.pk = it->second.ts;
        l.nerr = &it->second.rs_err;
        l.tpf = TspPerFrame(mode, lyr.mod, lyr.rate, static_cast<int>(lyr.segments.size()));
        l.start = it->second.first_frame;
        for (size_t i = 0; i < l.nerr->size(); ++i) {
            if ((*l.nerr)[i] < 0) l.pk[i * kTsPacket + 1] |= kTeiMask;
        }
        for (size_t i = 0; i < 11 && i * kTsPacket < l.pk.size(); ++i) l.pk[i * kTsPacket + 1] |= kTeiMask;
        f_first = std::min(f_first, l.start);
        f_last = std::max(f_last, l.start + static_cast<int>(l.pk.size() / kTsPacket) / l.tpf);
        ls.push_back(std::move(l));
    }
    if (ls.empty()) throw std::runtime_error("復調結果に TS が無い");
    std::vector<uint8_t> out;
    for (int f = f_first; f < f_last; ++f) {
        for (const L& l : ls) {
            const int i = f - l.start;
            if (i < 0) continue;
            const size_t a = static_cast<size_t>(i) * l.tpf * kTsPacket;
            const size_t b = std::min(a + static_cast<size_t>(l.tpf) * kTsPacket, l.pk.size());
            if (a < b) out.insert(out.end(), l.pk.begin() + a, l.pk.begin() + b);
        }
    }
    return out;
}

Source LoadSigmf(const std::string& path, Mode mode, Gi gi, double max_seconds, double bitrate_opt,
                 bool track, bool fast_fec) {
    SigmfMeta meta;
    std::vector<cfloat> iq = ReadSigmf(path, &meta);
    if (max_seconds > 0) {
        const size_t n = static_cast<size_t>(max_seconds * meta.sample_rate);
        if (iq.size() > n) iq.resize(n);
    }
    std::fprintf(stderr, "demodulating %s (%.1f s of I/Q%s) ...\n", path.c_str(),
                 iq.size() / meta.sample_rate, track ? ", tracked" : "");
    const auto t0 = std::chrono::steady_clock::now();
    DemodResult res;
    if (track) {
        // 追従モード: ロック区間ごとに復調し、区間ごとに再多重して連結する
        TrackedResult tr = DemodulateTracked(std::move(iq), meta.sample_rate, mode, gi);
        const double sec0 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "done in %.1f s (%zu segments)\n", sec0, tr.segments.size());
        Source s;
        bool have = false;
        for (const TrackedSegment& seg : tr.segments) {
            if (!seg.decoded) continue;
            DemodResult one;
            one.fsi = seg.fsi;
            one.layers = seg.layers;
            std::vector<uint8_t> pk = MergeLayersByFrame(one, mode);
            s.packets.insert(s.packets.end(), pk.begin(), pk.end());
            if (!have) {
                res.fsi = seg.fsi;      // 階層情報・レート計算は最初に復調できた区間のものを使う
                have = true;
            }
            for (const auto& [name, lr] : seg.layers) {
                for (int e : lr.rs_err) s.rs_uncorrectable += e < 0;
                if (lr.mer_db > 0.0) s.mer_db = s.mer_db == 0.0 ? lr.mer_db : std::min(s.mer_db, lr.mer_db);
            }
        }
        if (!have) throw std::runtime_error("復調できた区間が無い");
        s.locked = true;
        s.mode = static_cast<int>(mode);
        s.gi = gi.str();
        long long tsp_pf_total = 0;
        for (const RxLayer& lyr : res.fsi.layers) {
            LayerDesc d{lyr.name, ModulationName(lyr.mod), lyr.rate.str(), lyr.I,
                        static_cast<int>(lyr.segments.size())};
            s.layers.push_back(d);
            tsp_pf_total += TspPerFrame(mode, lyr.mod, lyr.rate, d.segments);
        }
        const double frame_s = SymbolSamples(mode, gi) * static_cast<double>(kSymbolsPerFrame) / kSampleRateHz;
        s.bitrate = bitrate_opt > 0 ? bitrate_opt : tsp_pf_total * kTsPacket * 8 / frame_s;
        s.source_desc = "sigmf:" + path;
        return s;
    }
    res = Demodulate(std::move(iq), meta.sample_rate, mode, gi, kDefaultBlockFrames, fast_fec);
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "done in %.1f s\n", sec);
    std::fprintf(stderr,
                 "sync: clock %.2f ppm, cfo %.1f Hz (int %d), frame_start %d, parity %d, n_frames %d\n",
                 res.sync.clock_ppm, res.sync.cfo_hz(), res.sync.cfo_int_bins,
                 res.fsi.frame_start, res.fsi.tmcc_parity_ok ? 1 : 0, res.fsi.n_frames);
    for (const auto& [name, lr] : res.layers) {
        long long bad = 0;
        for (int e : lr.rs_err) bad += e < 0;
        std::fprintf(stderr, "  layer %s: MER %.1f dB, uncorrectable %lld/%zu\n",
                     name.c_str(), lr.mer_db, bad, lr.rs_err.size());
    }

    Source s;
    s.packets = MergeLayersByFrame(res, mode);
    s.locked = res.fsi.tmcc_parity_ok;
    for (const auto& [name, lr] : res.layers) {
        if (lr.mer_db > 0.0) s.mer_db = s.mer_db == 0.0 ? lr.mer_db : std::min(s.mer_db, lr.mer_db);
    }
    s.mode = static_cast<int>(mode);
    s.gi = gi.str();
    long long tsp_pf_total = 0;
    for (const RxLayer& lyr : res.fsi.layers) {
        LayerDesc d;
        d.name = lyr.name;
        d.mod = ModulationName(lyr.mod);
        d.rate = lyr.rate.str();
        d.I = lyr.I;
        d.segments = static_cast<int>(lyr.segments.size());
        s.layers.push_back(d);
        tsp_pf_total += TspPerFrame(mode, lyr.mod, lyr.rate, d.segments);
        auto it = res.layers.find(lyr.name);
        if (it != res.layers.end()) {
            for (int e : it->second.rs_err) s.rs_uncorrectable += e < 0;
        }
    }
    const double frame_s = SymbolSamples(mode, gi) * static_cast<double>(kSymbolsPerFrame) / kSampleRateHz;
    s.bitrate = bitrate_opt > 0 ? bitrate_opt : tsp_pf_total * kTsPacket * 8 / frame_s;
    s.source_desc = "sigmf:" + path;
    return s;
}

// ISDB-C（ケーブル TV トラモジ）の SigMF をオフラインで復調して再生ソースにする（--system isdbc）
Source LoadSigmfIsdbc(const std::string& path, double max_seconds, double bitrate_opt,
                      isdbc::TsmfDemux::Select sel) {
    SigmfMeta meta;
    std::vector<cfloat> iq = ReadSigmf(path, &meta);
    if (max_seconds > 0) {
        const size_t n = static_cast<size_t>(max_seconds * meta.sample_rate);
        if (iq.size() > n) iq.resize(n);
    }
    std::fprintf(stderr, "demodulating %s as ISDB-C (%.1f s of I/Q) ...\n", path.c_str(),
                 iq.size() / meta.sample_rate);
    const auto t0 = std::chrono::steady_clock::now();
    isdbc::IsdbcConfig cfg;
    cfg.fs = meta.sample_rate;
    cfg.select = sel;
    Source s;
    isdbc::IsdbcDemod demod(cfg, [&](const uint8_t* pk, size_t n) {
        s.packets.insert(s.packets.end(), pk, pk + n * kTsPacket);
    });
    const size_t blk = 1 << 18;
    for (size_t i = 0; i < iq.size(); i += blk) demod.Feed(iq.data() + i, std::min(blk, iq.size() - i));
    const isdbc::IsdbcStatus st = demod.status();
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::fprintf(stderr, "done in %.1f s: locked %d, MER %.1f dB, CFO %.0f Hz, clock %.1f ppm, "
                 "RS uncorrectable %lld/%lld, TSMF streams %zu (selected %d)\n",
                 sec, st.locked ? 1 : 0, st.mer_db, st.cfo_hz, st.clock_ppm, st.rs_uncorrectable,
                 st.rs_packets, st.tsmf_streams.size(), st.tsmf_selected);
    for (const auto& t : st.tsmf_streams) {
        std::fprintf(stderr, "  relative TS %d: TS_id %d, original_network_id %d\n", t.rel, t.tsid, t.onid);
    }
    if (s.packets.empty()) throw std::runtime_error("ISDB-C 復調で TS が得られない");
    s.locked = st.locked;
    s.mer_db = st.mer_db;
    s.rs_uncorrectable = st.rs_uncorrectable;
    s.mode = 0;
    s.gi = "-";
    s.bitrate = bitrate_opt > 0 ? bitrate_opt : EstimateBitrateFromPcr(s.packets, isdbc::kTsBitrateBps);
    s.source_desc = "sigmf-isdbc:" + path;
    return s;
}

// ---------------------------------------------------------------------------
// 再生器
// ---------------------------------------------------------------------------
class Replayer : public IFeed {
public:
    Replayer(Source src, bool loop, double signal_db)
        : src_(std::move(src)), loop_(loop),
          signal_db_(src_.mer_db > 0.0 ? src_.mer_db : signal_db) {}   // 復調ソースは MER を返す

    void Start() { thread_ = std::thread(&Replayer::Run, this); }
    void Stop() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    std::shared_ptr<ClientQueue> AddClient() override {
        auto q = std::make_shared<ClientQueue>();
        std::lock_guard<std::mutex> lock(mu_);
        clients_.push_back(q);
        return q;
    }
    void AttachClient(const std::shared_ptr<ClientQueue>& q) override {
        std::lock_guard<std::mutex> lock(mu_);
        clients_.push_back(q);
    }
    void RemoveClient(const std::shared_ptr<ClientQueue>& q) override {
        std::lock_guard<std::mutex> lock(mu_);
        clients_.erase(std::remove(clients_.begin(), clients_.end(), q), clients_.end());
    }

    void Tune(bool has_ch, int ch, double freq) override {
        std::lock_guard<std::mutex> lock(mu_);
        has_channel_ = has_ch;
        channel_ = ch;
        freq_hz_ = freq;
        has_freq_ = true;
        pos_ = 0;
        ended_ = false;
        for (auto& q : clients_) {
            std::lock_guard<std::mutex> ql(q->mu);
            q->chunks.clear();
        }
    }

    std::string StatusJson() override {
        std::lock_guard<std::mutex> lock(mu_);
        std::string s = "{\"ok\": true";
        s += ", \"locked\": " + std::string(src_.locked && !ended_ ? "true" : "false");
        s += ", \"signal_db\": " + FormatDouble(ended_ ? 0.0 : signal_db_);
        s += ", \"channel\": " + (has_channel_ ? std::to_string(channel_) : "null");
        s += ", \"freq_hz\": " + (has_freq_ ? FormatDouble(freq_hz_) : "null");
        s += ", \"mode\": " + std::to_string(src_.mode);
        s += ", \"gi\": \"" + src_.gi + "\"";
        s += ", \"layers\": [";
        for (size_t i = 0; i < src_.layers.size(); ++i) {
            const LayerDesc& d = src_.layers[i];
            if (i) s += ", ";
            s += "{\"name\": \"" + d.name + "\", \"mod\": \"" + d.mod + "\", \"rate\": \"" + d.rate +
                 "\", \"I\": " + std::to_string(d.I) + ", \"segments\": " + std::to_string(d.segments) + "}";
        }
        s += "]";
        s += ", \"ts_packets\": " + std::to_string(ts_packets_);
        s += ", \"rs_uncorrectable\": " + std::to_string(src_.rs_uncorrectable);
        s += ", \"dropped\": " + std::to_string(dropped_);
        s += ", \"stream_clients\": " + std::to_string(clients_.size());
        s += ", \"bitrate\": " + FormatDouble(src_.bitrate);
        s += "}";
        return s;
    }

    const std::string& source_desc() const override { return src_.source_desc; }
    bool Ended() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return ended_;
    }

private:
    void Run() {
        const size_t total = src_.packets.size();
        const double chunk_s = kChunkPackets * kTsPacket * 8 / src_.bitrate;
        auto t_next = std::chrono::steady_clock::now();
        while (!stop_) {
            {
                // 切り出しから各クライアントへの積み込みまで mu_ を持ったまま行う。離すと Tune（pos_ = 0 と
                // キューのクリア）が割り込み、選局前のチャンクが選局後の先頭の後ろに積まれて順序が崩れる
                // （macOS の CI で test_isdbtd_stream が落ちた。2026-10-16）
                std::lock_guard<std::mutex> lock(mu_);
                if (pos_ >= total) {
                    if (!loop_) ended_ = true;
                    else pos_ = 0;
                }
                if (!ended_) {
                    const size_t n = std::min(kChunkPackets * kTsPacket, total - pos_);
                    std::vector<uint8_t> chunk(src_.packets.begin() + pos_, src_.packets.begin() + pos_ + n);
                    pos_ += n;
                    ts_packets_ += n / kTsPacket;
                    for (auto& q : clients_) {
                        std::lock_guard<std::mutex> ql(q->mu);
                        if (q->chunks.size() >= kQueueMaxChunks) {
                            dropped_ += n / kTsPacket;
                        } else {
                            q->chunks.push_back(chunk);
                            q->cv.notify_one();
                        }
                    }
                }
            }
            t_next += std::chrono::microseconds(static_cast<long long>(chunk_s * 1e6));
            auto now = std::chrono::steady_clock::now();
            if (t_next > now) {
                std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
                    t_next - now, std::chrono::milliseconds(200)));
            } else if (now - t_next > std::chrono::seconds(1)) {
                t_next = now;       // 大きく遅れたら基準を引き直す
            }
        }
    }

    Source src_;
    bool loop_;
    double signal_db_;
    mutable std::mutex mu_;                 // Ended() は const
    std::vector<std::shared_ptr<ClientQueue>> clients_;
    size_t pos_ = 0;
    bool ended_ = false;
    bool has_channel_ = false, has_freq_ = false;
    int channel_ = 0;
    double freq_hz_ = 0.0;
    std::atomic<long long> ts_packets_{0};
    std::atomic<long long> dropped_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// サーバ
// ---------------------------------------------------------------------------
class Server {
public:
    // live が非 null なら tune の system でパイプラインを切り替える（SDR ソースのとき）
    Server(IFeed* rp, LiveSwitch* live, const std::string& host, int port, bool allow_quit, bool verbose)
        : rp_(rp), live_(live), allow_quit_(allow_quit), verbose_(verbose) {
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
        int yes = 1;
        setsockopt(listen_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
        if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listen_, 8) != 0) {
            throw std::runtime_error("bind/listen failed on " + host + ":" + std::to_string(port));
        }
    }

    bool stopping() const { return stopping_; }

    void Serve() {
        while (!stopping_) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(listen_, &rd);
            timeval tv{0, 200'000};
            const int r = select(static_cast<int>(listen_) + 1, &rd, nullptr, nullptr, &tv);    // 第 1 引数は Windows では無視される
            if (r <= 0) continue;
            socket_t c = accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::thread(&Server::Handle, this, c).detach();
        }
        CloseSocket(listen_);
    }

private:
    void Log(const char* fmt, ...) {
        if (!verbose_) return;
        va_list ap;
        va_start(ap, fmt);
        std::vfprintf(stderr, fmt, ap);
        std::fprintf(stderr, "\n");
        va_end(ap);
    }

    static bool SendAll(socket_t s, const char* data, size_t n) {
        while (n > 0) {
            const int r = static_cast<int>(send(s, data, static_cast<int>(std::min<size_t>(n, 1 << 20)), 0));
            if (r <= 0) return false;
            data += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }

    static bool SendLine(socket_t s, const std::string& json) {
        std::string line = json + "\n";
        return SendAll(s, line.data(), line.size());
    }

    void Handle(socket_t c) {
        std::string buf;
        char tmp[4096];
        while (!stopping_) {
            size_t nl = buf.find('\n');
            if (nl == std::string::npos) {
                const int r = static_cast<int>(recv(c, tmp, sizeof(tmp), 0));
                if (r <= 0) break;
                buf.append(tmp, static_cast<size_t>(r));
                continue;
            }
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            std::string cmd = JsonStringValue(line, "cmd");
            if (cmd.empty() && line.find("\"cmd\"") == std::string::npos) {
                SendLine(c, "{\"ok\": false, \"error\": \"invalid json\"}");
                continue;
            }
            if (cmd == "hello") {
                SendLine(c, std::string("{\"ok\": true, \"name\": \"isdbtd\", \"protocol\": ") +
                             std::to_string(kProtocolVersion) + ", \"source\": \"" +
                             JsonEscape(rp_->source_desc()) + "\"}");
            } else if (cmd == "tune") {
                double freq = 0.0, chd = 0.0;
                const bool has_freq = JsonNumber(line, "freq_hz", &freq);
                const bool has_ch = JsonNumber(line, "channel", &chd);
                System sys;
                if (!ParseSystem(JsonStringValue(line, "system"), &sys)) {
                    SendLine(c, "{\"ok\": false, \"error\": \"system must be isdbt or isdbc\"}");
                    continue;
                }
                if (!has_freq && !has_ch) {
                    SendLine(c, "{\"ok\": false, \"error\": \"channel or freq_hz required\"}");
                    continue;
                }
                int ch = static_cast<int>(chd);
                if (!has_freq) {
                    if (sys == System::kIsdbc) {
                        // ケーブル TV のチャンネル番号（BonDriver_BDA 互換の通し番号 0..112）
                        if (!isdbc::CatvChannel(ch, &freq, nullptr)) {
                            SendLine(c, "{\"ok\": false, \"error\": \"channel out of range 0..112\"}");
                            continue;
                        }
                    } else {
                        if (ch < 13 || ch > 62) {
                            SendLine(c, "{\"ok\": false, \"error\": \"channel out of range 13..62\"}");
                            continue;
                        }
                        freq = ChannelToHz(ch);
                    }
                }
                double ts_sel = 0.0;
                if (live_ && JsonNumber(line, "ts", &ts_sel)) {
                    isdbc::TsmfDemux::Select s;
                    s.rel = static_cast<int>(ts_sel);
                    live_->SetSelect(s);
                }
                if (live_) live_->TuneSystem(sys, has_ch, ch, freq);
                else rp_->Tune(has_ch, ch, freq);
                Log("tune system=%s channel=%d freq=%.0f", SystemName(sys), has_ch ? ch : -1, freq);
                SendLine(c, std::string("{\"ok\": true, \"system\": \"") + SystemName(sys) +
                             "\", \"channel\": " + (has_ch ? std::to_string(ch) : "null") +
                             ", \"freq_hz\": " + FormatDouble(freq) + "}");
            } else if (cmd == "status") {
                SendLine(c, rp_->StatusJson());
            } else if (cmd == "stream") {
                SendLine(c, "{\"ok\": true}");
                Stream(c);
                break;
            } else if (cmd == "quit") {
                if (!allow_quit_) {
                    SendLine(c, "{\"ok\": false, \"error\": \"quit disabled\"}");
                    continue;
                }
                SendLine(c, "{\"ok\": true}");
                Log("quit requested");
                stopping_ = true;
                break;
            } else {
                SendLine(c, "{\"ok\": false, \"error\": \"unknown cmd '" + cmd + "'\"}");
            }
        }
        CloseSocket(c);
    }

    void Stream(socket_t c) {
        auto q = rp_->AddClient();
        Log("stream client connected");
        int yes = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
        while (!stopping_) {
            std::vector<uint8_t> chunk;
            {
                std::unique_lock<std::mutex> lock(q->mu);
                if (!q->cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return !q->chunks.empty(); })) {
                    continue;
                }
                chunk = std::move(q->chunks.front());
                q->chunks.pop_front();
            }
            if (!SendAll(c, reinterpret_cast<const char*>(chunk.data()), chunk.size())) break;
        }
        rp_->RemoveClient(q);
        Log("stream client disconnected");
    }

    IFeed* rp_;
    LiveSwitch* live_;
    bool allow_quit_;
    bool verbose_;
    socket_t listen_;
    std::atomic<bool> stopping_{false};
};

// --stdout: TCP サーバを立てず、TS を標準出力に流す（Mirakurun / recpt1 相当のチューナーコマンド用）。
// SIGTERM / SIGINT（Mirakurun はチャンネル切替・停止でプロセスを止める）か、stdout が閉じたら終わる
std::atomic<bool> g_stop{false};
void OnSignal(int) { g_stop = true; }

int StreamToStdout(IFeed* feed, bool verbose) {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    auto q = feed->AddClient();
    auto last_status = std::chrono::steady_clock::now();
    long long bytes = 0;
    while (!g_stop) {
        const auto now = std::chrono::steady_clock::now();
        if (verbose && now - last_status >= std::chrono::seconds(10)) {
            std::fprintf(stderr, "%s\n", feed->StatusJson().c_str());
            last_status = now;
        }
        std::vector<uint8_t> chunk;
        {
            std::unique_lock<std::mutex> lock(q->mu);
            if (!q->cv.wait_for(lock, std::chrono::milliseconds(500), [&] { return !q->chunks.empty(); })) {
                if (feed->Ended()) break;       // ファイル再生（--loop 無し）の末尾: 出し切ったら終わる
                continue;
            }
            chunk = std::move(q->chunks.front());
            q->chunks.pop_front();
        }
        if (std::fwrite(chunk.data(), 1, chunk.size(), stdout) != chunk.size() || std::fflush(stdout) != 0) {
            if (verbose) std::fprintf(stderr, "stdout closed after %lld bytes\n", bytes);
            break;
        }
        bytes += static_cast<long long>(chunk.size());
    }
    feed->RemoveClient(q);
    if (verbose) std::fprintf(stderr, "stdout stream end (%lld bytes%s)\n", bytes, g_stop ? ", signal" : "");
    return 0;
}

void Usage() {
    std::fprintf(stderr,
        "usage: isdbtd (--ts <file> | --sigmf <file> | --sdr [uri]) [options]\n"
        "  --system isdbt|isdbc  方式 (default isdbt)。isdbc = ケーブル TV の 64QAM トラモジ\n"
        "  --mode 1|2|3        (default 3)\n"
        "  --gi 1/4|1/8|1/16|1/32  (default 1/8)\n"
        "  --max-seconds F     --sigmf の先頭から処理する秒数\n"
        "  --bitrate F         再生ビットレート [bit/s]\n"
        "  --loop              末尾まで来たら先頭に戻る\n"
        "  --track             追従復調（長時間・ドリフト・瞬断のある録音向け）\n"
        "  --fast-fec          --sigmf でもライブ受信と同じ 8bit ビタビを使う（float 版との比較用）\n"
        "  --signal-db F       status.signal_db に返す固定値 (default 30)\n"
        "  --tsmf N | all      ISDB-C: TSMF の相対 TS 番号（既定は自動、all = 全スロット通過）\n"
        "  --host H --port N   (default 127.0.0.1:47188)\n"
        "  --no-quit           quit コマンドを受け付けない\n"
        "  --stdout            TCP サーバを立てず TS を標準出力へ（Mirakurun 等のチューナーコマンド用。\n"
        "                      --sdr では --ch か --freq が必須。SIGTERM / SIGINT か stdout が閉じると終了）\n"
        "  --quiet\n"
        "SDR ライブ受信（--sdr。uri 省略時 usb:）:\n"
        "  --ch N | --freq F   初期チャンネル/周波数（省略時は tune 待ち。isdbc の N は 0..112 の通し番号）\n"
        "  --fs F              取り込みレート Hz (省略時はファームのサンプル幅で自動: 16bit = 7111111（公称の 7/8、\n"
        "                      USB 2.0 の帯域対策）/ 8bit パッキングファーム = 8126984（公称）)\n"
        "  --gain F | hw       手動ゲイン dB / AD936x の slow_attack AGC。省略時はソフト AGC\n"
        "  --agc-target F      ソフト AGC の目標帯域内レベル dBFS (default -24)\n"
        "  --fpga-eq auto|0|1  FPGA 前段（pluto-fw-isdbt v2 の同期・FFT・等化）を使う (default auto)\n"
        "  --bw F              RF 帯域幅 Hz を固定（省略時は方式ごとの既定: 地デジ 5.6 MHz / ISDB-C 6 MHz）\n"
        "  --no-reconnect      ストリームエラー時に自動再接続しない（既定は再接続）\n"
        "  --win-frames N      ウィンドウの新規フレーム数 (default 8。選局後の遅延 vs CPU 効率)\n"
        "  --ctx-frames N      ウィンドウの文脈フレーム数 (default 4)\n"
        "  --workers N         並列復調ワーカー数 (default 3)\n"
        "  --first-burst psi|all|none  選局直後の最初のバースト（default psi: PSI/SI だけ先に出す）\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::string ts_path, sigmf_path, host = "127.0.0.1", gi_str = "1/8";
    int mode_i = 3, port = kDefaultPort;
    double max_seconds = 0, bitrate = 0, signal_db = 30.0;
    bool loop = false, no_quit = false, quiet = false, track = false, to_stdout = false, fast_fec = false;
    bool sdr = false;
    SdrConfig sdr_cfg;
    std::string dump_iq, system_str;
    int init_ch = 0;
    int win_frames = 8, ctx_frames = 4, workers = 3;
    std::string first_burst_str;
    bool bw_fixed = false;
    isdbc::TsmfDemux::Select ts_sel;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--ts") ts_path = next();
        else if (a == "--sigmf") sigmf_path = next();
        else if (a == "--sdr") {
            sdr = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') sdr_cfg.uri = next();
        }
        else if (a == "--system") system_str = next();
        else if (a == "--ch") init_ch = std::atoi(next().c_str());
        else if (a == "--freq") sdr_cfg.freq_hz = std::atof(next().c_str());
        else if (a == "--fs") sdr_cfg.fs = std::atof(next().c_str());
        else if (a == "--bw") { sdr_cfg.bw = std::atof(next().c_str()); bw_fixed = true; }
        else if (a == "--gain") {
            const std::string g = next();
            if (g == "hw") sdr_cfg.hw_agc = true;
            else { sdr_cfg.agc = false; sdr_cfg.gain_db = std::atof(g.c_str()); }
        }
        else if (a == "--agc-target") sdr_cfg.agc_target_dbfs = std::atof(next().c_str());
        else if (a == "--fpga-eq") { const std::string v = next(); sdr_cfg.fpga_eq = (v == "0") ? 0 : (v == "1") ? 1 : -1; }
        else if (a == "--no-reconnect") sdr_cfg.reconnect = false;
        else if (a == "--dump-iq") dump_iq = next();
        else if (a == "--win-frames") win_frames = std::atoi(next().c_str());
        else if (a == "--ctx-frames") ctx_frames = std::atoi(next().c_str());
        else if (a == "--workers") workers = std::atoi(next().c_str());
        else if (a == "--first-burst") first_burst_str = next();
        else if (a == "--mode") mode_i = std::atoi(next().c_str());
        else if (a == "--gi") gi_str = next();
        else if (a == "--max-seconds") max_seconds = std::atof(next().c_str());
        else if (a == "--bitrate") bitrate = std::atof(next().c_str());
        else if (a == "--loop") loop = true;
        else if (a == "--track") track = true;
        else if (a == "--fast-fec") fast_fec = true;
        else if (a == "--signal-db") signal_db = std::atof(next().c_str());
        else if (a == "--tsmf" || a == "--rel-ts") {
            const std::string v = next();
            if (v == "all") ts_sel.pass_all = true;
            else ts_sel.rel = std::atoi(v.c_str());
        }
        else if (a == "--host") host = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--no-quit") no_quit = true;
        else if (a == "--quiet") quiet = true;
        else if (a == "--stdout") to_stdout = true;
        else { Usage(); return 2; }
    }
    const int n_sources = (!ts_path.empty()) + (!sigmf_path.empty()) + (sdr ? 1 : 0);
    if (n_sources != 1) {
        Usage();
        return 2;
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);    // クライアントが切れたときの send() は戻り値で扱う
#endif
    try {
        Gi gi{8};
        if (!ParseGi(gi_str, &gi)) throw std::runtime_error("bad --gi: " + gi_str);
        System system;
        if (!ParseSystem(system_str, &system)) throw std::runtime_error("bad --system: " + system_str);
        if (sdr) {
            double init_freq = 0.0;
            if (init_ch) {
                if (system == System::kIsdbc) {
                    if (!isdbc::CatvChannel(init_ch, &init_freq, nullptr)) throw std::runtime_error("bad --ch for isdbc");
                } else {
                    init_freq = ChannelToHz(init_ch);
                }
                sdr_cfg.freq_hz = init_freq;
            }
            if (sdr_cfg.freq_hz <= 0) sdr_cfg.freq_hz = ChannelToHz(27);   // tune 待ちの仮周波数
            LiveParams lp;
            lp.sdr = sdr_cfg;
            lp.bw_fixed = bw_fixed;
            lp.mode = static_cast<Mode>(mode_i);
            lp.gi = gi;
            lp.win_frames = win_frames;
            lp.ctx_frames = ctx_frames;
            lp.workers = workers;
            lp.dump_iq = dump_iq;
            lp.select = ts_sel;
            lp.verbose = !quiet;
            if (!ParseFirstBurst(first_burst_str, &lp.first_burst)) throw std::runtime_error("bad --first-burst: " + first_burst_str);
            if (to_stdout && !init_ch && sdr_cfg.freq_hz <= 0) throw std::runtime_error("--stdout with --sdr needs --ch or --freq");
            LiveSwitch live(lp);
            live.Start(system);
            if (init_ch) live.TuneSystem(system, true, init_ch, init_freq);
            if (to_stdout) {
                std::fprintf(stderr, "live demod (%s, %s, win=%d+%d frames, %d workers) -> stdout\n",
                             sdr_cfg.uri.c_str(), SystemName(system), ctx_frames, win_frames, workers);
                StreamToStdout(&live, !quiet);
                live.Stop();
            } else {
                std::fprintf(stderr, "live demod (%s, %s, win=%d+%d frames, %d workers) on %s:%d\n",
                             sdr_cfg.uri.c_str(), SystemName(system), ctx_frames, win_frames, workers,
                             host.c_str(), port);
                Server server(&live, &live, host, port, !no_quit, !quiet);
                server.Serve();
                live.Stop();
            }
        } else {
            Source src = !ts_path.empty() ? LoadTsFile(ts_path, bitrate)
                         : (system == System::kIsdbc
                                ? LoadSigmfIsdbc(sigmf_path, max_seconds, bitrate, ts_sel)
                                : LoadSigmf(sigmf_path, static_cast<Mode>(mode_i), gi, max_seconds, bitrate, track, fast_fec));
            const size_t n_tsp = src.packets.size() / kTsPacket;
            std::fprintf(stderr, "serving %zu TSP (%.1f s at %.3f Mbps, loop=%d) on %s:%d  source=%s\n",
                         n_tsp, n_tsp * kTsPacket * 8 / src.bitrate, src.bitrate / 1e6, loop ? 1 : 0,
                         host.c_str(), port, src.source_desc.c_str());
            Replayer rp(std::move(src), loop, signal_db);
            if (to_stdout) {
                rp.Start();
                StreamToStdout(&rp, !quiet);
                rp.Stop();
            } else {
                Server server(&rp, nullptr, host, port, !no_quit, !quiet);
                rp.Start();
                server.Serve();
                rp.Stop();
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
