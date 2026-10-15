#include "live_isdbc.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace isdbt {

namespace {
constexpr double kRetuneSkipSeconds = 0.65;    // SDR のブロックキュー（4 × 147 ms = 0.59 s）+ 余裕
constexpr size_t kEmitPackets = 64;     // クライアントへ渡すチャンクの単位（64 TSP ≈ 12 KB、約 3.3 ms）
}

LiveIsdbc::LiveIsdbc(SdrConfig cfg, isdbc::TsmfDemux::Select select, bool verbose, SdrSource* external_sdr)
    : cfg_(std::move(cfg)), select_(select), verbose_(verbose), desc_("sdr:" + cfg_.uri + " (isdbc)"),
      sdr_(external_sdr ? external_sdr : &own_sdr_), external_(external_sdr != nullptr), rate_mon_(cfg_.fs) {
    isdbc::IsdbcConfig dc;
    dc.fs = cfg_.fs;
    dc.select = select_;
    demod_ = std::make_unique<isdbc::IsdbcDemod>(dc, [this](const uint8_t* pk, size_t n) { OnPackets(pk, n); });
}

LiveIsdbc::~LiveIsdbc() { Stop(); }

void LiveIsdbc::Start() {
    if (!external_) {
        sdr_->Start(cfg_, [this](const cfloat* p, size_t n) { OnBlock(p, n); },
                    [this](bool up) { OnSdrEvent(up); });
    }
    skip_samples_ = static_cast<long long>(kRetuneSkipSeconds * cfg_.fs);
    worker_ = std::thread(&LiveIsdbc::WorkLoop, this);
}

void LiveIsdbc::Stop() {
    if (stop_.exchange(true)) return;
    iq_cv_.notify_all();
    if (!external_) sdr_->Stop();
    if (worker_.joinable()) worker_.join();
}

std::shared_ptr<ClientQueue> LiveIsdbc::AddClient() {
    auto q = std::make_shared<ClientQueue>();
    std::lock_guard<std::mutex> lock(mu_);
    clients_.push_back(q);
    return q;
}

void LiveIsdbc::AttachClient(const std::shared_ptr<ClientQueue>& q) {
    std::lock_guard<std::mutex> lock(mu_);
    clients_.push_back(q);
}

void LiveIsdbc::RemoveClient(const std::shared_ptr<ClientQueue>& q) {
    std::lock_guard<std::mutex> lock(mu_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), q), clients_.end());
}

void LiveIsdbc::Tune(bool has_ch, int ch, double freq) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (has_freq_ && freq == freq_hz_) {
            has_channel_ = has_ch;
            channel_ = ch;
            return;                     // 同一周波数: 復調を壊さない
        }
        has_channel_ = has_ch;
        channel_ = ch;
        freq_hz_ = freq;
        has_freq_ = true;
        for (auto& q : clients_) {
            std::lock_guard<std::mutex> ql(q->mu);
            q->chunks.clear();
        }
    }
    if (!external_) sdr_->Retune(freq);
    skip_samples_ = static_cast<long long>(kRetuneSkipSeconds * cfg_.fs);
    {
        std::lock_guard<std::mutex> lock(iq_mu_);
        iq_q_.clear();
        iq_total_ = 0;
    }
    reset_pending_ = true;
    iq_cv_.notify_all();
}

void LiveIsdbc::SetSelect(isdbc::TsmfDemux::Select select) {
    std::lock_guard<std::mutex> lock(mu_);
    select_ = select;
    select_pending_ = true;
}

std::string LiveIsdbc::StatusJson() {
    const isdbc::IsdbcStatus st = demod_->status();
    std::lock_guard<std::mutex> lock(mu_);
    std::string name;
    if (has_channel_) isdbc::CatvChannel(channel_, nullptr, &name);
    std::string s = "{\"ok\": true, \"system\": \"isdbc\"";
    s += ", \"locked\": " + std::string(st.locked ? "true" : "false");
    s += ", \"signal_db\": " + FormatDouble(st.locked ? st.mer_db : 0.0);
    s += ", \"mer_db\": " + FormatDouble(st.mer_db);
    s += ", \"channel\": " + (has_channel_ ? std::to_string(channel_) : "null");
    s += ", \"channel_name\": " + (has_channel_ ? "\"" + name + "\"" : "null");
    s += ", \"freq_hz\": " + (has_freq_ ? FormatDouble(freq_hz_) : "null");
    s += ", \"acquired\": " + std::string(st.acquired ? "true" : "false");
    s += ", \"byte_sync\": " + std::string(st.byte_sync ? "true" : "false");
    s += ", \"inverted\": " + std::string(st.inverted ? "true" : "false");
    s += ", \"cfo_hz\": " + FormatDouble(st.cfo_hz);
    s += ", \"clock_ppm\": " + FormatDouble(st.clock_ppm);
    s += ", \"level_dbfs\": " + FormatDouble(st.level_dbfs);
    s += ", \"ts_packets\": " + std::to_string(st.ts_packets);
    s += ", \"rs_packets\": " + std::to_string(st.rs_packets);
    s += ", \"rs_uncorrectable\": " + std::to_string(st.rs_uncorrectable);
    s += ", \"sync_losses\": " + std::to_string(st.sync_losses);
    s += ", \"reacquisitions\": " + std::to_string(st.reacquisitions);
    s += ", \"tsmf\": {\"frames\": " + std::to_string(st.tsmf_frames) +
         ", \"bad_headers\": " + std::to_string(st.tsmf_bad_headers) +
         ", \"selected\": " + std::to_string(st.tsmf_selected) + ", \"streams\": [";
    for (size_t i = 0; i < st.tsmf_streams.size(); ++i) {
        if (i) s += ", ";
        s += "{\"rel\": " + std::to_string(st.tsmf_streams[i].rel) +
             ", \"tsid\": " + std::to_string(st.tsmf_streams[i].tsid) +
             ", \"onid\": " + std::to_string(st.tsmf_streams[i].onid) + "}";
    }
    s += "]}";
    s += ", \"dropped\": " + std::to_string(dropped_);
    s += ", \"underruns\": " + std::to_string(underruns_);
    s += ", \"sdr_blocks\": " + std::to_string(sdr_->blocks());
    s += ", \"sdr_connected\": " + std::string(sdr_->connected() ? "true" : "false");
    s += ", \"sdr_reconnects\": " + std::to_string(sdr_->reconnects());
    s += ", \"hw_ovf\": " + std::to_string(sdr_->hw_ovf());
    if (sdr_->hw_counter()) {
        s += ", \"hw_dropped\": " + std::to_string(sdr_->hw_dropped_samples());
        s += ", \"hw_ovf_events\": " + std::to_string(sdr_->hw_ovf_events());
    }
    s += ", \"gain_db\": " + FormatDouble(sdr_->gain_db());
    s += ", \"rx_level_dbfs\": " + FormatDouble(sdr_->level_dbfs());
    s += ", \"rx_rate\": " + FormatDouble(rx_rate_.load());
    s += ", \"rx_slow\": " + std::to_string(rx_slow_.load());
    s += ", \"stream_clients\": " + std::to_string(clients_.size());
    s += ", \"bitrate\": " + FormatDouble(isdbc::kTsBitrateBps);
    s += "}";
    return s;
}

bool LiveIsdbc::locked() { return demod_->status().locked; }

double LiveIsdbc::signal_db() {
    const isdbc::IsdbcStatus st = demod_->status();
    return st.locked ? st.mer_db : 0.0;
}

void LiveIsdbc::OnBlock(const cfloat* p, size_t n) {
    if (skip_samples_ > 0) {
        skip_samples_ -= static_cast<long long>(n);
        rate_mon_.Reset();          // 選局直後の読み飛ばしをレート不足と誤計上しない（毎回「rx rate 7.5 MHz」が出ていた）
        return;
    }
    const double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (rate_mon_.Feed(static_cast<long long>(n), t)) {
        rx_rate_ = rate_mon_.rate();
        rx_slow_ = rate_mon_.slow();
        if (rate_mon_.last_slow()) {
            std::fprintf(stderr, "sdr: rx rate %.0f Hz < %.0f Hz: USB 帯域不足かサンプル落ち\n",
                         rate_mon_.rate(), rate_mon_.fs());
        }
    }
    std::lock_guard<std::mutex> lock(iq_mu_);
    if (iq_total_ > kIqBoundSamples) {      // 復調が追いつかない: 捨てて仕切り直し
        iq_q_.clear();
        iq_total_ = 0;
        reset_pending_ = true;
        std::lock_guard<std::mutex> l2(mu_);
        ++underruns_;
    }
    iq_q_.emplace_back(p, p + n);
    iq_total_ += static_cast<long long>(n);
    iq_cv_.notify_one();
}

void LiveIsdbc::OnSdrEvent(bool up) {
    if (up) {
        rate_mon_.Reset();
        return;
    }
    std::lock_guard<std::mutex> lock(iq_mu_);
    iq_q_.clear();
    iq_total_ = 0;
    reset_pending_ = true;
}

void LiveIsdbc::WorkLoop() {
    while (!stop_) {
        std::vector<cfloat> blk;
        bool have_blk = false;
        {
            std::unique_lock<std::mutex> lock(iq_mu_);
            iq_cv_.wait_for(lock, std::chrono::milliseconds(200), [&] { return !iq_q_.empty() || stop_; });
            if (stop_) break;
            if (!iq_q_.empty()) {
                blk = std::move(iq_q_.front());
                iq_q_.pop_front();
                iq_total_ -= static_cast<long long>(blk.size());
                have_blk = true;
            }
        }
        // 選局後のリセットは I/Q が来なくても行う（LO 範囲外で SDR が I/Q を止めているとき、前の局の
        // locked / MER が status に残って TVTest のスキャンが信号ありと見る）
        if (reset_pending_.exchange(false)) {
            demod_->Reset();
            pending_.clear();
        }
        if (!have_blk) continue;
        if (select_pending_.exchange(false)) {
            // TS 選択の変更は復調器を作り直さず Reset（TSMF の再同期は 53 パケットで済む）
            isdbc::IsdbcConfig dc;
            dc.fs = cfg_.fs;
            {
                std::lock_guard<std::mutex> lock(mu_);
                dc.select = select_;
            }
            demod_ = std::make_unique<isdbc::IsdbcDemod>(dc, [this](const uint8_t* pk, size_t n) { OnPackets(pk, n); });
        }
        demod_->Feed(blk.data(), blk.size());
    }
}

void LiveIsdbc::OnPackets(const uint8_t* pk, size_t n) {
    pending_.insert(pending_.end(), pk, pk + n * kTsPacket);
    if (pending_.size() < kEmitPackets * kTsPacket) return;
    std::vector<std::shared_ptr<ClientQueue>> clients;
    {
        std::lock_guard<std::mutex> lock(mu_);
        clients = clients_;
    }
    std::vector<uint8_t> chunk;
    chunk.swap(pending_);
    for (auto& q : clients) {
        std::lock_guard<std::mutex> ql(q->mu);
        if (q->chunks.size() >= kQueueMaxChunks) {
            std::lock_guard<std::mutex> l2(mu_);
            dropped_ += chunk.size() / kTsPacket;
        } else {
            q->chunks.push_back(chunk);
            q->cv.notify_one();
        }
    }
}

}  // namespace isdbt
