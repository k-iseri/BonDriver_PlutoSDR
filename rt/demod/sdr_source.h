// Pluto SDR（AD936x + cf-ad9361-lpc）からの I/Q キャプチャ（libiio v1 API）。
// 受信スレッドを 1 本持ち、ブロックが埋まるたびにコールバックへ cfloat（/2048 正規化済み）を渡す。
// ストリームエラー（USB 瞬断・デバイスリセット等）時は既定で自動再接続する
// （バックオフ 0.5→5 s。cfg.reconnect=false で従来どおり停止）。
//
// libiio が無い環境でもビルドできるよう、ISDBT_WITH_LIBIIO が無いときは Start() が例外を投げる
// スタブになる（rt/third_party/fetch_libiio.ps1 で取得すると CMake が自動で有効化する）。
//
// 実機検証: 2026-10-10 Pluto SDR nano (Z7010-AD9363A, fw v0.39) で動作確認。
// 自動再接続は 2026-10-12 に TVTest 視聴中の USB 抜き差しで復帰を実機確認済み。
#pragma once
#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace isdbt {

struct SdrConfig {
    std::string uri = "usb:";
    double freq_hz = 0.0;
    // 既定レートは公称 512/63 MHz の 7/8 = 7,111,111 Hz。
    // 16bit × 公称レート（32.5 MB/s）は USB 2.0 の実効スループットに約 3〜4% 足りず、
    // 初期バッファ枯渇後（~15 s）にサンプル落ちが続発する（実機で確認）。
    // 7/8 レート（28.4 MB/s）なら持続し、復調コアが内部で 8/7 リサンプルして吸収する。
    // GbE 機（Pluto+ / LibreSDR）では --fs 8126984 に戻してよい。
    double fs = 7'111'111.0;
    // 5.6 MHz（信号帯域ちょうど）。7.111 MSPS ではナイキスト余裕が薄く、6 MHz だと
    // フィルタ裾のエイリアシングで帯域端の MER が落ちる（tools/iq_record.py の既定と同じ値）
    double bw = 5'600'000.0;
    // ゲイン。既定はソフトウェア AGC: 帯域内レベルを agc_target_dbfs に保つよう hardwaregain を
    // 約 0.5 秒ごとに調整する（manual モード）。AD9363 の slow_attack AGC（約 −11 dBFS）は
    // ケーブル TV 端子のように全帯域が強い入力でフロントエンドが圧縮され MER が 20 dB に張り付く
    // （2026-10-13 実測。docs/isdbc-spec-notes.md 6 章。地デジも AGC 19.8 → 手動 30 dB で 27 dB）。
    bool agc = true;                // false なら gain_db 固定
    bool hw_agc = false;            // agc=true のとき true なら AD936x の slow_attack を使う（旧動作）
    double gain_db = 30.0;          // 手動ゲイン / ソフト AGC の初期値 [dB]
    double agc_target_dbfs = -24.0; // ソフト AGC の目標帯域内レベル（圧縮の崖 −21 dBFS に 3 dB 余裕）
    // 1 MiB ブロック（262144 サンプル）では USB 2.0 の実効スループットが足りず、
    // 初期バッファ枯渇後（~2.4 s）からブロック境界でサンプル落ちが続発した（実機で確認）。
    // Python 側 (tools/iq_record.py) と同じ 4 MiB ブロックにすると持続する。
    size_t block_samples = 1 << 20; // 1 ブロックの複素サンプル数（≈129 ms, 4 MiB）
    size_t nb_blocks = 4;           // デバイス側のブロック数（≈516 ms 分のバッファ）
    bool reconnect = true;          // ストリームエラー時に自動再接続する（USB 瞬断対策）
};

class SdrSource {
public:
    using BlockFn = std::function<void(const std::complex<float>*, size_t)>;
    // up=false: ストリームが切れた（以降のサンプルは不連続。呼び出し側は継ぎ目を仕切り直す）。
    // up=true: 再接続して受信を再開した。どちらも受信スレッドから呼ばれる（on_block と同じスレッド）。
    using EventFn = std::function<void(bool up)>;

    ~SdrSource();

    // 接続・設定して受信スレッドを開始する。初回接続の失敗は std::runtime_error
    //（設定ミスを呼び出し元へ即座に伝える。再接続するのはいったん開けた後のエラーだけ）。
    void Start(const SdrConfig& cfg, BlockFn on_block, EventFn on_event = {});
    // LO だけ変更（ストリームは止めない）。例外を投げない:
    // 切断中は周波数を記録するだけで、再接続時に適用される。
    void Retune(double freq_hz);
    // RF 帯域幅だけ変更（ストリームは止めない。方式切替用。例外を投げない）
    void SetBandwidth(double bw_hz);
    void Stop();

    const std::string& desc() const { return desc_; }
    long long blocks() const { return blocks_; }
    bool connected() const { return connected_; }
    long long reconnects() const { return reconnects_; }
    // FPGA が報告したハード確定ドロップ（axi_ad9361 UI_STATUS の UI_OVF を約 1 秒周期で
    // 監視した検出回数）。純正ファームで動く。実機検証 2026-10-12（健全時 0、USB 飽和で検出）。
    long long hw_ovf() const { return hw_ovf_; }
    double gain_db() const { return gain_db_; }             // 現在の hardwaregain（AGC モードでも）
    double level_dbfs() const { return level_dbfs_; }       // 直近の帯域内平均電力

private:
    void Run();
    void Connect();                 // cfg_（freq は現在値）で接続・設定。失敗は throw
    void Teardown();                // ハンドル破棄（mu_ を取って呼ぶ）

    struct Impl;                    // libiio ハンドル類（.cpp 内定義）
    Impl* impl_ = nullptr;
    SdrConfig cfg_;                 // freq_hz は Retune で更新される現在値
    BlockFn on_block_;
    EventFn on_event_;
    std::thread thread_;
    std::mutex mu_;                 // impl_ のハンドルと cfg_.freq_hz を守る（Retune ↔ 再接続）
    std::condition_variable cv_;    // 再接続バックオフ待ちを Stop() で即中断するため
    std::atomic<bool> stop_{false};
    std::atomic<bool> connected_{false};
    std::atomic<long long> reconnects_{0};
    std::atomic<long long> hw_ovf_{0};
    std::atomic<double> gain_db_{0.0};
    std::atomic<double> level_dbfs_{-100.0};
    volatile long long blocks_ = 0;
    std::string desc_;
};

}  // namespace isdbt
