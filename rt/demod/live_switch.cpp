#include "live_switch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace isdbt {

LiveSwitch::LiveSwitch(LiveParams p) : p_(std::move(p)) {
    p_.bw_fixed = p_.sdr.bw > 0.0 && p_.bw_fixed;
    desc_ = "sdr:" + p_.sdr.uri;
}

LiveSwitch::~LiveSwitch() { Stop(); }

double LiveSwitch::BwFor(System sys) const {
    if (p_.bw_fixed) return p_.sdr.bw;
    return (sys == System::kIsdbc) ? p_.bw_isdbc_hz : p_.bw_isdbt_hz;
}

void LiveSwitch::ForwardBlock(const cfloat* p, size_t n) {
    std::lock_guard<std::mutex> lk(fwd_mu_);
    if (fwd_c_) fwd_c_->OnBlock(p, n);
    else if (fwd_t_) fwd_t_->OnBlock(p, n);
}

// FPGA 等化モードの等化済みシンボルは地デジ側だけが受ける（ISDB-C はファームの前段を使わない）
void LiveSwitch::ForwardSymbol(const EqSymbol& s) {
    std::lock_guard<std::mutex> lk(fwd_mu_);
    if (fwd_t_) fwd_t_->OnSymbol(s);
}

void LiveSwitch::ForwardEvent(bool up) {
    std::lock_guard<std::mutex> lk(fwd_mu_);
    if (fwd_c_) fwd_c_->OnSdrEvent(up);
    else if (fwd_t_) fwd_t_->OnSdrEvent(up);
}

// 復調パイプラインを作って起動し、クライアントを付け替え、SDR の転送先にする
void LiveSwitch::BuildFeed(System sys, double freq) {
    // FPGA 前段（同期・FFT・等化）は地デジ専用。ISDB-C は raw I/Q で復調するので前段を止める
    // （止めないと約 1 秒後にモード 3/4 へ移って raw が来なくなり、CATV が映らない。2026-10-15 TVTest で発覚）
    sdr_.EnableFpgaFrontEnd(sys != System::kIsdbc);
    SdrConfig sc = p_.sdr;
    sc.freq_hz = freq;
    sc.bw = BwFor(sys);
    std::unique_ptr<LiveDemod> t;
    std::unique_ptr<LiveIsdbc> c;
    if (sys == System::kIsdbc) {
        c = std::make_unique<LiveIsdbc>(sc, p_.select, p_.verbose, &sdr_);
        c->Start();
    } else {
        t = std::make_unique<LiveDemod>(sc, p_.mode, p_.gi, p_.win_frames, p_.ctx_frames, p_.workers,
                                        p_.verbose, p_.dump_iq, &sdr_);
        t->SetFirstBurst(p_.first_burst);
        t->Start();
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        isdbt_ = std::move(t);
        isdbc_ = std::move(c);
        system_ = sys;
        IFeed* a = ActiveLocked();
        for (auto& q : clients_) a->AttachClient(q);
        desc_ = a->source_desc();
    }
    std::lock_guard<std::mutex> lk(fwd_mu_);
    fwd_t_ = isdbt_.get();
    fwd_c_ = isdbc_.get();
}

void LiveSwitch::TeardownFeed() {
    {
        std::lock_guard<std::mutex> lk(fwd_mu_);
        fwd_t_ = nullptr;
        fwd_c_ = nullptr;
    }
    std::unique_ptr<LiveDemod> t;
    std::unique_ptr<LiveIsdbc> c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        t = std::move(isdbt_);
        c = std::move(isdbc_);
    }
    if (t) t->Stop();
    if (c) c->Stop();
}

void LiveSwitch::Start(System initial) {
    if (p_.sdr.fs <= 0.0) {
        // レート自動: ファームのサンプル幅で決める（16bit = 7/8 レート、8bit = 公称レート）。
        // 調べられなければ（接続失敗など）16bit の既定にして、本接続で改めてエラーにする
        const int bytes = SdrSource::ProbeSampleBytes(p_.sdr.uri);
        p_.sdr.fs = SdrSource::DefaultFsFor(bytes);
        std::fprintf(stderr, "sdr: sample format %s -> fs %.0f Hz\n",
                     bytes == 2 ? "int8 (8bit firmware)" : bytes == 4 ? "int16" : "unknown (assume int16)",
                     p_.sdr.fs);
    }
    BuildFeed(initial, p_.sdr.freq_hz);
    SdrConfig sc = p_.sdr;
    sc.bw = BwFor(initial);
    freq_hz_ = sc.freq_hz;
    try {
        sdr_.Start(sc, [this](const cfloat* p, size_t n) { ForwardBlock(p, n); },
                   [this](bool up) { ForwardEvent(up); },
                   [this](const EqSymbol& s) { ForwardSymbol(s); });
    } catch (...) {
        TeardownFeed();
        throw;
    }
    stop_ = false;
    started_ = true;
    worker_ = std::thread(&LiveSwitch::Worker, this);
}

void LiveSwitch::Stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!started_) return;
        started_ = false;
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    sdr_.Stop();
    TeardownFeed();
}

std::shared_ptr<ClientQueue> LiveSwitch::AddClient() {
    auto q = std::make_shared<ClientQueue>();
    AttachClient(q);
    return q;
}

void LiveSwitch::AttachClient(const std::shared_ptr<ClientQueue>& q) {
    std::lock_guard<std::mutex> lk(mu_);
    clients_.push_back(q);
    if (IFeed* a = ActiveLocked()) a->AttachClient(q);
}

void LiveSwitch::RemoveClient(const std::shared_ptr<ClientQueue>& q) {
    std::lock_guard<std::mutex> lk(mu_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), q), clients_.end());
    if (IFeed* a = ActiveLocked()) a->RemoveClient(q);
}

void LiveSwitch::Tune(bool has_ch, int ch, double freq) { TuneSystem(system(), has_ch, ch, freq); }

void LiveSwitch::TuneSystem(System sys, bool has_ch, int ch, double freq) {
    IFeed* a = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!switching_ && !has_pending_ && sys == system_ && ActiveLocked()) {
            a = ActiveLocked();             // 同じ方式: その場で選局（速い）
        } else {
            pending_ = Request{sys, has_ch, ch, freq};
            has_pending_ = true;
        }
    }
    if (a) {
        if (freq != freq_hz_) {
            sdr_.Retune(freq);
            freq_hz_ = freq;
        }
        a->Tune(has_ch, ch, freq);
        return;
    }
    cv_.notify_all();
}

// 差し替えスレッド: 要求を取り出し、旧パイプラインを止めて SDR の帯域・周波数を変え、新しい方を作る
void LiveSwitch::Worker() {
    for (;;) {
        Request req{};
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || has_pending_; });
            if (stop_) return;
            req = pending_;
            has_pending_ = false;
            switching_ = true;
        }
        const auto t0 = std::chrono::steady_clock::now();
        TeardownFeed();
        sdr_.SetBandwidth(BwFor(req.sys));
        if (req.freq != freq_hz_) {
            sdr_.Retune(req.freq);
            freq_hz_ = req.freq;
        }
        bool ok = true;
        try {
            BuildFeed(req.sys, req.freq);
        } catch (const std::exception& e) {
            ok = false;
            std::fprintf(stderr, "live: switch to %s failed: %s\n", SystemName(req.sys), e.what());
        }
        if (ok) {
            if (p_.verbose) {
                std::fprintf(stderr, "live: switched to %s (%.3f MHz) in %.2f s\n", SystemName(req.sys),
                             req.freq / 1e6,
                             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            }
            IFeed* a = nullptr;
            {
                std::lock_guard<std::mutex> lk(mu_);
                a = ActiveLocked();
            }
            if (a) a->Tune(req.has_ch, req.ch, req.freq);
        }
        std::lock_guard<std::mutex> lk(mu_);
        switching_ = false;
    }
}

std::string LiveSwitch::StatusJson() {
    IFeed* a = nullptr;
    System sys;
    bool sw;
    {
        std::lock_guard<std::mutex> lk(mu_);
        a = ActiveLocked();
        sys = system_;
        sw = switching_ || has_pending_;
    }
    if (!a || sw) {
        return std::string("{\"ok\": true, \"system\": \"") + SystemName(sys) +
               "\", \"switching\": true, \"locked\": false, \"signal_db\": 0}";
    }
    std::string s = a->StatusJson();
    if (sys == System::kIsdbt && s.rfind("{\"ok\": true", 0) == 0) {
        s.insert(std::string("{\"ok\": true").size(), ", \"system\": \"isdbt\"");
    }
    return s;
}

const std::string& LiveSwitch::source_desc() const { return desc_; }

System LiveSwitch::system() {
    std::lock_guard<std::mutex> lk(mu_);
    return system_;
}

bool LiveSwitch::switching() {
    std::lock_guard<std::mutex> lk(mu_);
    return switching_ || has_pending_;
}

bool LiveSwitch::locked() {
    std::lock_guard<std::mutex> lk(mu_);
    if (switching_) return false;
    if (isdbc_) return isdbc_->locked();
    if (isdbt_) return isdbt_->locked();
    return false;
}

double LiveSwitch::signal_db() {
    std::lock_guard<std::mutex> lk(mu_);
    if (switching_) return 0.0;
    if (isdbc_) return isdbc_->signal_db();
    if (isdbt_) return isdbt_->signal_db();
    return 0.0;
}

void LiveSwitch::SetSelect(isdbc::TsmfDemux::Select select) {
    std::lock_guard<std::mutex> lk(mu_);
    p_.select = select;
    if (isdbc_) isdbc_->SetSelect(select);
}

}  // namespace isdbt
