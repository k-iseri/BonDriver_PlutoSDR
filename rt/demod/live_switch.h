// 地デジ（ISDB-T、LiveDemod）とケーブル TV トラモジ（ISDB-C、LiveIsdbc）を 1 台の SDR で切り替える
// 入れ物。isdbtd --sdr と BonDriver_PlutoSDR_TC.dll が共用する。
// SDR（SdrSource）は LiveSwitch が 1 つだけ開いて持ち続け、受信ブロックをそのとき有効な復調
// パイプラインへ転送する。方式が変わる選局では復調パイプラインだけ作り直し、SDR は帯域幅と
// 周波数を書き換えるだけ（SDR を開き直すと USB の iiod の都合で 10 秒かかる — 2026-10-14 実測）。
// 作り直しは専用スレッドで行い、TuneSystem はすぐ返る（デーモンプロトコルの「tune は 2 秒以内に応答」）。
// 切り替え中は status が locked=false / signal 0 / switching=true を返す。クライアントのキューは引き継ぐ。
#pragma once
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../isdbc/tsmf.h"
#include "live_demod.h"
#include "live_isdbc.h"

namespace isdbt {

enum class System { kIsdbt, kIsdbc };

inline const char* SystemName(System s) { return s == System::kIsdbc ? "isdbc" : "isdbt"; }
inline bool ParseSystem(const std::string& s, System* out) {
    if (s.empty() || s == "isdbt") { *out = System::kIsdbt; return true; }
    if (s == "isdbc") { *out = System::kIsdbc; return true; }
    return false;
}

struct LiveParams {
    SdrConfig sdr;                      // bw は方式ごとの既定（下記）で上書きされる（bw_fixed なら固定）
    double bw_isdbt_hz = 5'600'000.0;   // 地デジ: 信号帯域ちょうど（docs/hw-setup.md）
    double bw_isdbc_hz = 6'000'000.0;   // ISDB-C: Rs(1+α) = 5.96 MHz
    bool bw_fixed = false;
    // ISDB-T
    Mode mode = Mode::M3;
    Gi gi{8};
    int win_frames = 8, ctx_frames = 4, workers = 3;
    std::string dump_iq;
    // ISDB-C
    isdbc::TsmfDemux::Select select;
    bool verbose = false;
};

class LiveSwitch : public IFeed {
public:
    explicit LiveSwitch(LiveParams p);
    ~LiveSwitch() override;

    // 初期方式でパイプラインを起動し SDR を開く（失敗は std::runtime_error）
    void Start(System initial);
    void Stop();

    std::shared_ptr<ClientQueue> AddClient() override;
    void AttachClient(const std::shared_ptr<ClientQueue>& q) override;
    void RemoveClient(const std::shared_ptr<ClientQueue>& q) override;
    void Tune(bool has_ch, int ch, double freq) override;          // 現在の方式で選局
    void TuneSystem(System sys, bool has_ch, int ch, double freq);  // 方式を指定して選局（必要なら差し替え。即時復帰）
    std::string StatusJson() override;
    const std::string& source_desc() const override;

    System system();
    bool switching();
    bool locked();
    double signal_db();
    // ISDB-C の TS 選択を変える（ISDB-T 中は次回の差し替え時に反映）
    void SetSelect(isdbc::TsmfDemux::Select select);

private:
    struct Request {
        System sys;
        bool has_ch;
        int ch;
        double freq;
    };
    double BwFor(System sys) const;
    void BuildFeed(System sys, double freq);     // 復調パイプラインを作って active に据える（ロック無しで呼ぶ）
    void TeardownFeed();                         // ロック無しで呼ぶ
    void Worker();
    IFeed* ActiveLocked() { return isdbc_ ? static_cast<IFeed*>(isdbc_.get()) : static_cast<IFeed*>(isdbt_.get()); }
    void ForwardBlock(const cfloat* p, size_t n);
    void ForwardEvent(bool up);

    LiveParams p_;
    SdrSource sdr_;
    System system_ = System::kIsdbt;
    double freq_hz_ = 0.0;              // SDR に設定済みの LO 周波数
    std::mutex mu_;                     // isdbt_ / isdbc_ / clients_ / pending_ を守る
    std::mutex fwd_mu_;                 // 転送先（SDR 受信スレッドと差し替えスレッドの間）
    std::condition_variable cv_;
    std::unique_ptr<LiveDemod> isdbt_;
    std::unique_ptr<LiveIsdbc> isdbc_;
    LiveDemod* fwd_t_ = nullptr;        // 転送先（fwd_mu_ 下）
    LiveIsdbc* fwd_c_ = nullptr;
    std::vector<std::shared_ptr<ClientQueue>> clients_;
    std::string desc_;
    bool switching_ = false;
    bool has_pending_ = false;
    Request pending_{};
    bool stop_ = false;
    bool started_ = false;
    std::thread worker_;
};

}  // namespace isdbt
