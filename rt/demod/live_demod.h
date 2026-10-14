// 実機ライブ復調（SDR 取り込み → ウィンドウ並列復調 → ペーサー送出）。
// isdbtd.exe（--sdr）と BonDriver_ISDBTSDR.dll のスタンドアロンモードが共用する。
//
// 復調コアはこのマシンで実時間比 ~0.9 倍（10 s を 11 s）なので、1 本のパイプラインでは
// ライブに追いつけない。そこで I/Q を「文脈 ctx フレーム + 新規 new フレーム」の重複
// ウィンドウに切り、ウィンドウ単位で独立に Demodulate() を並列実行する（既定 2 ワーカー）。
// 各ウィンドウは毎回フレーム同期からやり直すが、ウィンドウ間の重複（ctx ≥ 時間デ
// インターリーブ履歴 2 + バイト/ビット立ち上がり 1）があるため、重複部を捨てて
// 絶対フレーム番号で継ぎ合わせれば定常的に連続な TS になる。
// フレーム番号は入力サンプルの絶対位置から求める（アンカーを毎ウィンドウ更新するので
// クロック偏差 ±20ppm 程度では丸めを誤らない）。
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../isdbt/demod.h"
#include "../isdbt/params.h"
#include "rate_monitor.h"
#include "sdr_source.h"

namespace isdbt {

constexpr size_t kQueueMaxChunks = 256;     // クライアントごとの送信待ち上限（約 1.5 秒 @16 Mbps）
constexpr uint8_t kTeiMask = 0x80;          // transport_error_indicator（TSP 2 バイト目）

// 地デジ物理チャンネル → 中心周波数（473 + 1/7 MHz + 6 MHz × (ch − 13)。tools/iq_record.py と同じ式）
inline double ChannelToHz(int ch) { return 473'142'857.0 + 6'000'000.0 * (ch - 13); }

inline std::string FormatDouble(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

struct LayerDesc {
    std::string name, mod, rate;
    int I = 0, segments = 0;
};

struct ClientQueue {
    std::deque<std::vector<uint8_t>> chunks;
    std::mutex mu;
    std::condition_variable cv;
};

// 配信元の共通インターフェース（録画再生 Replayer / 実機ライブ LiveDemod）
class IFeed {
public:
    virtual ~IFeed() = default;
    virtual std::shared_ptr<ClientQueue> AddClient() = 0;
    // 既存のキューを登録する（LiveSwitch がパイプラインを差し替えるときにクライアントを引き継ぐ）
    virtual void AttachClient(const std::shared_ptr<ClientQueue>& q) = 0;
    virtual void RemoveClient(const std::shared_ptr<ClientQueue>& q) = 0;
    virtual void Tune(bool has_ch, int ch, double freq) = 0;
    virtual std::string StatusJson() = 0;
    virtual const std::string& source_desc() const = 0;
};

// 選局直後の最初の（短い）ウィンドウの出し方。連続再生できる量が溜まる前に出すものを決める:
//   kPsi  … PSI/SI（PID < 0x30 と PAT に載る PMT）だけ出す。TVTest のチャンネルスキャンは検出でき、
//           映像・音声は連続分が揃ってから始まるので切替直後にかくつかない（既定）
//   kAll  … 全パケットを出す（一瞬映ってから数秒止まる）
//   kNone … 何も出さない（連続分が揃うまで無信号。スキャンは待ち時間を延ばす必要がある）
enum class FirstBurst { kPsi, kAll, kNone };

class LiveDemod : public IFeed {
public:
    // external_sdr が非 null なら SDR を自分で開かず、所有者（LiveSwitch）が OnBlock / OnSdrEvent を呼ぶ
    LiveDemod(SdrConfig cfg, Mode mode, Gi gi, int new_frames, int ctx_frames, int workers,
              bool verbose, std::string dump_iq_path = {}, SdrSource* external_sdr = nullptr);

    // SDR を開いて復調スレッドを開始する。失敗は std::runtime_error（libiio 無しビルドを含む）
    void Start();
    void Stop();

    std::shared_ptr<ClientQueue> AddClient() override;
    void AttachClient(const std::shared_ptr<ClientQueue>& q) override;
    void RemoveClient(const std::shared_ptr<ClientQueue>& q) override;
    void Tune(bool has_ch, int ch, double freq) override;
    std::string StatusJson() override;
    const std::string& source_desc() const override { return desc_; }

    // BonDriver スタンドアロンモード向けの軽量アクセサ（StatusJson の JSON を解さずに済む）
    bool locked();
    double signal_db();         // 最悪階層の MER [dB]（未ロック時 0）
    // 外部 SDR 所有者から呼ぶ入力（SDR 受信スレッドから）
    void OnBlock(const cfloat* p, size_t n);
    void OnSdrEvent(bool up);
    // Start() の前に呼ぶ
    void SetFirstBurst(FirstBurst mode) { first_burst_ = mode; }

private:
    struct WindowJob {
        long long gen = 0;
        long long abs0 = 0;             // ウィンドウ先頭の絶対サンプル番号
        std::future<std::shared_ptr<DemodResult>> fut;   // 失敗は nullptr
    };

    void SchedLoop();
    void ResetSplice(long long* cur_gen);
    void Emit(const DemodResult& res, long long abs0);
    void PaceLoop();

    static constexpr long long kIqBoundSamples = 40LL * 8'126'984;   // 取り込み待ち上限 ~40 s
    static constexpr long long kDumpMaxSamples = 10LL * 8'126'984;   // --dump-iq は先頭 10 秒
    static constexpr size_t kPaceCatchupFrames = 24;                 // ペーサーの追いつき閾値 ~5.5 s

    SdrConfig cfg_;
    Mode mode_;
    Gi gi_;
    int new_frames_, ctx_frames_, workers_;
    double in_per_nominal_;
    double frame_samples_;          // 入力サンプル単位
    std::string desc_;
    SdrSource own_sdr_;
    SdrSource* sdr_;                // 自前（own_sdr_）か LiveSwitch の共有 SDR
    bool external_;
    std::thread sched_, pacer_;
    std::atomic<bool> stop_{false};

    // 受信レートの実測（USB 帯域不足・サンプル落ちの可視化）。rate_mon_ は
    // SDR 受信スレッドだけが触り、窓が閉じるたびに atomic へ反映して status が読む
    RateMonitor rate_mon_;
    std::atomic<double> rx_rate_{0.0};
    std::atomic<long long> rx_slow_{0};

    std::mutex pace_mu_;
    std::condition_variable pace_cv_;
    std::deque<std::vector<uint8_t>> pace_q_;
    std::atomic<bool> pace_reset_{false};   // 選局でペーサーの再開待ち状態を初期化する
    std::atomic<long long> pace_starved_{0};   // 定常中に送出キューが空になった回数（復調の遅れの指標）
    FirstBurst first_burst_ = FirstBurst::kPsi;
    std::vector<int> pmt_pids_;             // 最初のバーストの PAT から拾った PMT の PID（ペーサースレッド専用）
    std::vector<uint8_t> psi_cache_;        // 最初のバーストで出した PSI/SI（連続分が揃うまで繰り返し送る）
    std::vector<uint8_t> psi_cc_;           // PSI/SI の PID ごとに出力側で振り直している continuity_counter
    std::vector<uint8_t> psi_cc_tracked_;   // その PID を一度でも出したか（0/1）
    std::vector<uint8_t> FilterPsi(const std::vector<uint8_t>& chunk);
    std::vector<uint8_t> RepeatPsi();       // psi_cache_ のコピーを CC を進めて返す
    void RemapPsiCc(std::vector<uint8_t>* chunk);   // PSI/SI の CC を出力側の連番に振り直す（繰り返し分と本流をつなぐ）
    bool IsPsiPid(int pid) const;
    void SendToClients(const std::vector<uint8_t>& chunk);

    std::mutex iq_mu_;
    std::condition_variable iq_cv_;
    std::deque<std::vector<cfloat>> iq_q_;
    long long iq_total_ = 0;
    bool overflowed_ = false;
    // 選局直後の暫定信号レベル: 最初のウィンドウが復調されるまで（約 6 秒）MER が無いので、GI 相関から
    // C/N を見積もって signal_db に入れる。TVTest のチャンネルスキャンは待ち時間（既定 5 秒）経過時点で
    // 信号レベル ≥ 7 dB か SDT 受信済みでなければ即座に次へ進むため（2026-10-14、ChannelScan.cpp より）
    std::atomic<bool> quick_level_pending_{false};
    static double QuickGiRho(const cfloat* x, size_t n, int N, int Ng);
    // 選局直後に捨てる入力サンプル数。SDR 側に積まれている約 0.6 秒分（nb_blocks × block）は
    // LO 変更前の信号なので、混ぜると最初のウィンドウが必ず同期失敗して TS が 1 ウィンドウ分遅れる
    std::atomic<long long> skip_samples_{0};

    std::mutex mu_;
    std::vector<std::shared_ptr<ClientQueue>> clients_;
    long long gen_ = 0;
    bool locked_ = false;
    bool anchored_ = false;
    double anchor_t_ = 0.0;
    long long anchor_f_ = 0;
    long long last_emitted_ = -1;
    std::vector<LayerDesc> layers_;
    double mer_db_ = 0.0, bitrate_ = 0.0;
    bool has_channel_ = false, has_freq_ = false;
    int channel_ = 0;
    double freq_hz_ = 0.0;
    long long ts_packets_ = 0, rs_uncorrectable_ = 0, frames_lost_ = 0;
    long long dropped_ = 0, underruns_ = 0;
    long long windows_done_ = 0, windows_failed_ = 0;
    bool verbose_ = false;
    FILE* dump_ = nullptr;
    long long dump_written_ = 0;
};

}  // namespace isdbt
