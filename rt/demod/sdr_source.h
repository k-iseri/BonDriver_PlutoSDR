// Pluto SDR（AD936x + cf-ad9361-lpc）からの I/Q キャプチャ（libiio v1 API）。
// 受信スレッドを 1 本持ち、ブロックが埋まるたびにコールバックへ cfloat（/2048 正規化済み）を渡す。
//
// libiio が無い環境でもビルドできるよう、ISDBT_WITH_LIBIIO が無いときは Start() が例外を投げる
// スタブになる（rt/third_party/fetch_libiio.ps1 で取得すると CMake が自動で有効化する）。
//
// 実機検証: 2026-10-10 Pluto SDR nano (Z7010-AD9363A, fw v0.39) で動作確認。
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
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
    double gain_db = 0.0;           // agc=false のときの手動ゲイン
    bool agc = true;                // slow_attack AGC
    // 1 MiB ブロック（262144 サンプル）では USB 2.0 の実効スループットが足りず、
    // 初期バッファ枯渇後（~2.4 s）からブロック境界でサンプル落ちが続発した（実機で確認）。
    // Python 側 (tools/iq_record.py) と同じ 4 MiB ブロックにすると持続する。
    size_t block_samples = 1 << 20; // 1 ブロックの複素サンプル数（≈129 ms, 4 MiB）
    size_t nb_blocks = 4;           // デバイス側のブロック数（≈516 ms 分のバッファ）
};

class SdrSource {
public:
    using BlockFn = std::function<void(const std::complex<float>*, size_t)>;

    ~SdrSource();

    // 接続・設定して受信スレッドを開始する。失敗は std::runtime_error。
    void Start(const SdrConfig& cfg, BlockFn on_block);
    void Retune(double freq_hz);    // LO だけ変更（ストリームは止めない）
    void Stop();

    const std::string& desc() const { return desc_; }
    long long blocks() const { return blocks_; }

private:
    void Run();

    struct Impl;                    // libiio ハンドル類（.cpp 内定義）
    Impl* impl_ = nullptr;
    BlockFn on_block_;
    std::thread thread_;
    volatile bool stop_ = false;
    volatile long long blocks_ = 0;
    std::string desc_;
};

}  // namespace isdbt
