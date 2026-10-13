// 実機ライブ ISDB-C 受信（SDR 取り込み → IsdbcDemod → クライアント送出）。
// isdbtd.exe（--sdr、system=isdbc）と BonDriver_PlutoSDR_TC.dll の CATV 空間が共用する。
// ISDB-T の LiveDemod と違い復調は逐次（1 ワーカースレッド、実時間比 < 1）なので、ウィンドウ分割も
// ペーサーも要らない。SDR 受信スレッドはブロックをキューに積むだけで、ワーカーが IsdbcDemod::Feed する。
#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../isdbc/demod.h"
#include "live_demod.h"
#include "rate_monitor.h"
#include "sdr_source.h"

namespace isdbt {

class LiveIsdbc : public IFeed {
public:
    // select: TSMF の TS 選択（相対 TS 番号 0 = 自動）
    // external_sdr が非 null なら SDR を自分で開かず、所有者（LiveSwitch）が OnBlock / OnSdrEvent を呼ぶ
    LiveIsdbc(SdrConfig cfg, isdbc::TsmfDemux::Select select, bool verbose, SdrSource* external_sdr = nullptr);
    ~LiveIsdbc() override;

    void Start();
    void Stop();

    std::shared_ptr<ClientQueue> AddClient() override;
    void AttachClient(const std::shared_ptr<ClientQueue>& q) override;
    void RemoveClient(const std::shared_ptr<ClientQueue>& q) override;
    // ch は CATV チャンネル番号（BonDriver_BDA 互換の通し番号。docs/isdbc-spec-notes.md 1 章）
    void Tune(bool has_ch, int ch, double freq) override;
    std::string StatusJson() override;
    const std::string& source_desc() const override { return desc_; }

    bool locked();
    double signal_db();         // MER [dB]（未ロック時 0）
    // 選局後に TS 選択だけ変える（同一周波数の別 TS）
    void SetSelect(isdbc::TsmfDemux::Select select);
    // 外部 SDR 所有者から呼ぶ入力（SDR 受信スレッドから）
    void OnBlock(const cfloat* p, size_t n);
    void OnSdrEvent(bool up);

private:
    void WorkLoop();
    void OnPackets(const uint8_t* pk, size_t n);

    static constexpr long long kIqBoundSamples = 4LL * 7'111'111;   // 取り込み待ち上限 ~4 s

    SdrConfig cfg_;
    isdbc::TsmfDemux::Select select_;
    bool verbose_;
    std::string desc_;
    SdrSource own_sdr_;
    SdrSource* sdr_;
    bool external_;
    std::unique_ptr<isdbc::IsdbcDemod> demod_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> reset_pending_{false};
    std::atomic<bool> select_pending_{false};
    std::atomic<long long> skip_samples_{0};   // 選局直後に捨てる入力（LO 変更前に取り込まれた分）

    RateMonitor rate_mon_;
    std::atomic<double> rx_rate_{0.0};
    std::atomic<long long> rx_slow_{0};

    std::mutex iq_mu_;
    std::condition_variable iq_cv_;
    std::deque<std::vector<cfloat>> iq_q_;
    long long iq_total_ = 0;

    std::mutex mu_;
    std::vector<std::shared_ptr<ClientQueue>> clients_;
    bool has_channel_ = false, has_freq_ = false;
    int channel_ = 0;
    double freq_hz_ = 0.0;
    long long dropped_ = 0, underruns_ = 0;
    std::vector<uint8_t> pending_;      // クライアントへ渡す前の小分けバッファ
};

}  // namespace isdbt
