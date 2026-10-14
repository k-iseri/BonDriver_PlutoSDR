#include "sdr_source.h"

#include <cmath>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <vector>

#ifdef ISDBT_WITH_LIBIIO
#include <iio/iio.h>
#endif

namespace isdbt {

#ifndef ISDBT_WITH_LIBIIO

struct SdrSource::Impl {};
SdrSource::~SdrSource() {}
void SdrSource::Start(const SdrConfig&, BlockFn, EventFn) {
    throw std::runtime_error(
        "libiio 無しでビルドされている。rt\\third_party\\fetch_libiio.ps1 を実行してから make rt し直す");
}
void SdrSource::Retune(double) {}
void SdrSource::SetBandwidth(double) {}
void SdrSource::Stop() {}
void SdrSource::Run() {}
void SdrSource::Connect() {}
void SdrSource::Teardown() {}
int SdrSource::ProbeSampleBytes(const std::string&) { return 0; }

#else  // ISDBT_WITH_LIBIIO

namespace {

[[noreturn]] void Fail(const std::string& what, int err) {
    char buf[256] = {};
    iio_strerror(-err, buf, sizeof(buf));
    throw std::runtime_error(what + ": " + buf);
}

void CheckPtr(const void* p, const char* what) {
    const int err = iio_err(p);
    if (err) Fail(what, err);
    if (!p) throw std::runtime_error(std::string(what) + ": not found");
}

// 属性は型を気にせず文字列で書く（カーネル sysfs と同じ扱い）
void WriteAttr(const iio_channel* ch, const char* name, const std::string& value) {
    const iio_attr* attr = iio_channel_find_attr(ch, name);
    if (!attr) throw std::runtime_error(std::string("attr not found: ") + name);
    const ssize_t r = iio_attr_write_string(attr, value.c_str());
    if (r < 0) Fail(std::string("write ") + name + "=" + value, static_cast<int>(r));
}

std::string FormatLL(double v) { return std::to_string(static_cast<long long>(v)); }

// axi_ad9361 コア（cf-ad9361-lpc）の overflow 監視。
// cf_axi_adc の debugfs_reg_access はアドレスにこのマジックを立てると AXI コアレジスタを
// 直接読み書きする（純正ファームで動作確認 2026-10-12）。UI_STATUS の UI_OVF（RW1C）は
// DMA が受けきれずサンプルが落ちると立つ。取り込み停止中は常時立つのが正常なので、
// DMA が流れ始めてからクリアし、取り込み中に立ったものだけをドロップとして数える。
constexpr uint32_t kPcoreRegMagic = 0x80000000;
constexpr uint32_t kRegUiStatus = 0x0088;   // adi_regmap_adc: UI_STATUS
constexpr uint32_t kUiOvf = 1u << 2;        // bit2 = UI_OVF (RW1C)
constexpr long long kOvfCheckBlocks = 8;    // 約 1 秒周期（1 ブロック ≈ 129〜147 ms）

}  // namespace

struct SdrSource::Impl {
    iio_context* ctx = nullptr;
    iio_channels_mask* mask = nullptr;
    iio_stream* stream = nullptr;
    iio_channel* phy_rx = nullptr;      // ad9361-phy voltage0 (input): rate/bw/gain
    iio_channel* phy_lo = nullptr;      // ad9361-phy altvoltage0 (output): RX LO
    iio_device* rxdev = nullptr;        // 非 const（iio_device_reg_read/write が要求）
    size_t block_samples = 0;
};

SdrSource::~SdrSource() { Stop(); }

// cf-ad9361-lpc の voltage0/1 を有効にしたときのサンプルサイズ = scan-element 申告の合計。
// 純正ファームは le:s12/16 × 2 = 4、pluto-fw-isdbt の 8bit ファームは le:s8/8 × 2 = 2
//（カーネルがコアの CHAN_USR_CNTRL_1 申告を読んで型を決める。ホストは申告だけで判断し、
// ファーム識別子には依存しない）
int SdrSource::ProbeSampleBytes(const std::string& uri) {
    iio_context* ctx = iio_create_context(nullptr, uri.c_str());
    if (iio_err(ctx) || !ctx) return 0;
    int bytes = 0;
    iio_device* rxdev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    iio_channel* v0 = rxdev ? iio_device_find_channel(rxdev, "voltage0", false) : nullptr;
    iio_channel* v1 = rxdev ? iio_device_find_channel(rxdev, "voltage1", false) : nullptr;
    if (v0 && v1) {
        iio_channels_mask* mask = iio_create_channels_mask(iio_device_get_channels_count(rxdev));
        if (mask) {
            iio_channel_enable(v0, mask);
            iio_channel_enable(v1, mask);
            const ssize_t ss = iio_device_get_sample_size(rxdev, mask);
            if (ss > 0) bytes = static_cast<int>(ss);
            iio_channels_mask_destroy(mask);
        }
    }
    iio_context_destroy(ctx);
    return bytes;
}

void SdrSource::Start(const SdrConfig& cfg, BlockFn on_block, EventFn on_event) {
    impl_ = new Impl;
    on_block_ = std::move(on_block);
    on_event_ = std::move(on_event);
    cfg_ = cfg;
    Connect();          // 初回は同期（失敗はそのまま投げる）。以降のエラーは Run が再接続する
    desc_ = "sdr:" + cfg_.uri;
    stop_ = false;
    thread_ = std::thread(&SdrSource::Run, this);
}

// cfg_ と現在周波数で接続・設定する。ハンドルはローカルに組み立て、
// 全部成功してから mu_ 下で impl_ に移す（途中失敗の後始末を一箇所にするため）。
void SdrSource::Connect() {
    Impl im;
    try {
        iio_context* ctx = iio_create_context(nullptr, cfg_.uri.c_str());
        CheckPtr(ctx, "iio_create_context");
        im.ctx = ctx;

        iio_device* phy = iio_context_find_device(ctx, "ad9361-phy");
        iio_device* rxdev = iio_context_find_device(ctx, "cf-ad9361-lpc");
        if (!phy || !rxdev) throw std::runtime_error("ad9361-phy / cf-ad9361-lpc が見つからない（Pluto 系か確認）");
        im.rxdev = rxdev;

        im.phy_rx = iio_device_find_channel(phy, "voltage0", false);
        im.phy_lo = iio_device_find_channel(phy, "altvoltage0", true);
        if (!im.phy_rx || !im.phy_lo) throw std::runtime_error("ad9361-phy のチャンネルが見つからない");
        WriteAttr(im.phy_rx, "sampling_frequency", FormatLL(cfg_.fs));
        WriteAttr(im.phy_rx, "rf_bandwidth", FormatLL(cfg_.bw));
        if (cfg_.agc && cfg_.hw_agc) {
            WriteAttr(im.phy_rx, "gain_control_mode", "slow_attack");
        } else {
            // 手動 / ソフト AGC（再接続時は直前のゲインを引き継ぐ）
            if (gain_db_ == 0.0 && cfg_.gain_db != 0.0) gain_db_ = cfg_.gain_db;
            WriteAttr(im.phy_rx, "gain_control_mode", "manual");
            WriteAttr(im.phy_rx, "hardwaregain", std::to_string(static_cast<int>(std::lround(gain_db_))));
        }
        double freq;
        {
            std::lock_guard<std::mutex> lk(mu_);
            freq = cfg_.freq_hz;        // 切断中に Retune された周波数を拾う
        }
        WriteAttr(im.phy_lo, "frequency", FormatLL(freq));

        iio_channel* v0 = iio_device_find_channel(rxdev, "voltage0", false);
        iio_channel* v1 = iio_device_find_channel(rxdev, "voltage1", false);
        if (!v0 || !v1) throw std::runtime_error("cf-ad9361-lpc の voltage0/1 が見つからない");
        im.mask = iio_create_channels_mask(iio_device_get_channels_count(rxdev));
        if (!im.mask) throw std::runtime_error("iio_create_channels_mask failed");
        iio_channel_enable(v0, im.mask);
        iio_channel_enable(v1, im.mask);

        iio_buffer* buf = iio_device_get_buffer(rxdev, 0);
        if (!buf) throw std::runtime_error("cf-ad9361-lpc に buffer0 が無い");
        const ssize_t ss = iio_device_get_sample_size(rxdev, im.mask);
        if (ss != 4 && ss != 2) {
            throw std::runtime_error("sample size " + std::to_string(ss) +
                                     "（I/Q 2ch で 4 = int16 か 2 = int8 のどちらかを想定）");
        }
        sample_bytes_ = static_cast<int>(ss);
        im.block_samples = cfg_.block_samples;
        iio_stream* st = iio_buffer_create_stream(buf, cfg_.nb_blocks, cfg_.block_samples, im.mask);
        CheckPtr(st, "iio_buffer_create_stream");
        im.stream = st;
    } catch (...) {
        if (im.stream) iio_stream_destroy(im.stream);
        if (im.mask) iio_channels_mask_destroy(im.mask);
        if (im.ctx) iio_context_destroy(im.ctx);
        throw;
    }
    std::lock_guard<std::mutex> lk(mu_);
    *impl_ = im;
    connected_ = true;
}

// mu_ を取ってハンドルを破棄する（再接続時は Run から、終了時は Stop から呼ばれる）
void SdrSource::Teardown() {
    std::lock_guard<std::mutex> lk(mu_);
    connected_ = false;
    if (impl_->stream) iio_stream_destroy(impl_->stream);
    if (impl_->mask) iio_channels_mask_destroy(impl_->mask);
    if (impl_->ctx) iio_context_destroy(impl_->ctx);
    impl_->stream = nullptr;
    impl_->mask = nullptr;
    impl_->ctx = nullptr;
    impl_->phy_rx = impl_->phy_lo = nullptr;
    impl_->rxdev = nullptr;
}

void SdrSource::Retune(double freq_hz) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.freq_hz = freq_hz;
    if (!connected_ || !impl_->phy_lo) return;      // 再接続時に Connect() が適用する
    try {
        WriteAttr(impl_->phy_lo, "frequency", FormatLL(freq_hz));
    } catch (const std::exception& e) {
        // デバイスが消えた直後など。ストリーム側もすぐエラーになり再接続が走るので、
        // ここでは記録だけ（cfg_.freq_hz は更新済みなので再接続後に正しい周波数になる）
        std::fprintf(stderr, "sdr: retune failed (%s); will apply after reconnect\n", e.what());
    }
}

void SdrSource::SetBandwidth(double bw_hz) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.bw = bw_hz;
    if (!connected_ || !impl_->phy_rx) return;
    try {
        WriteAttr(impl_->phy_rx, "rf_bandwidth", FormatLL(bw_hz));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sdr: set bandwidth failed (%s); will apply after reconnect", e.what());
        std::fprintf(stderr, "%c", 10);
    }
}

void SdrSource::Stop() {
    if (!impl_) return;
    {
        // stop_ は mu_ の中で立てる（バックオフ待ちの述語チェックとの通知取りこぼし防止）
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
        if (impl_->stream) iio_stream_cancel(impl_->stream);
    }
    cv_.notify_all();           // 再接続バックオフ待ちを起こす
    if (thread_.joinable()) thread_.join();
    Teardown();
    delete impl_;
    impl_ = nullptr;
}

void SdrSource::Run() {
    std::vector<std::complex<float>> out(impl_->block_samples);
    // 純正ファーム: int16 の下位 12bit（AD936x は 12bit）。8bit ファーム: 12bit を 4bit 右シフト（丸め）した
    // int8 なので 1/128。どちらも同じ振幅（フルスケール ±1.0）になる
    constexpr float kScale16 = 1.0f / 2048.0f;
    constexpr float kScale8 = 1.0f / 128.0f;
    int backoff_ms = 500;
    bool ovf_usable = true;         // reg アクセス不能なコア（将来の別ボード等）では監視を止める
    bool ovf_armed = false;         // 最初のブロック到着（= DMA 稼働）後にクリアしてから監視を始める
    long long ovf_countdown = kOvfCheckBlocks;
    constexpr int kAgcBlocks = 4;   // 約 0.5 秒
    double agc_acc = 0.0, agc_cnt = 0.0;
    int agc_blocks = 0;
    while (!stop_) {
        const iio_block* blk = iio_stream_get_next_block(impl_->stream);
        if (iio_err(blk)) {
            if (stop_) break;
            std::fprintf(stderr, "sdr: stream error %d%s\n", iio_err(blk),
                         cfg_.reconnect ? "; reconnecting" : "");
            if (!cfg_.reconnect) break;
            if (on_event_) on_event_(false);
            Teardown();
            // バックオフ付き再接続（Stop() の cv_ 通知で即中断できる）
            while (!stop_) {
                {
                    std::unique_lock<std::mutex> lk(mu_);
                    cv_.wait_for(lk, std::chrono::milliseconds(backoff_ms),
                                 [&] { return stop_.load(); });
                }
                if (stop_) break;
                try {
                    Connect();
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "sdr: reconnect failed: %s (retry in %d ms)\n",
                                 e.what(), backoff_ms);
                    backoff_ms = std::min(backoff_ms * 2, 5000);
                    continue;
                }
                ++reconnects_;
                backoff_ms = 500;
                ovf_usable = true;
                ovf_armed = false;
                std::fprintf(stderr, "sdr: reconnected (#%lld)\n",
                             static_cast<long long>(reconnects_));
                if (on_event_) on_event_(true);
                break;
            }
            continue;
        }
        size_t n;
        if (sample_bytes_ == 2) {
            const int8_t* p = static_cast<const int8_t*>(iio_block_start(blk));
            const int8_t* e = static_cast<const int8_t*>(iio_block_end(blk));
            n = static_cast<size_t>(e - p) / 2;     // 複素サンプル数
            if (out.size() < n) out.resize(n);
            for (size_t i = 0; i < n; ++i) {
                out[i] = {p[2 * i] * kScale8, p[2 * i + 1] * kScale8};
            }
        } else {
            const int16_t* p = static_cast<const int16_t*>(iio_block_start(blk));
            const int16_t* e = static_cast<const int16_t*>(iio_block_end(blk));
            n = static_cast<size_t>(e - p) / 2;
            if (out.size() < n) out.resize(n);
            for (size_t i = 0; i < n; ++i) {
                out[i] = {p[2 * i] * kScale16, p[2 * i + 1] * kScale16};
            }
        }
        ++blocks_;
        if (on_block_) on_block_(out.data(), n);
        // ソフトウェア AGC: 帯域内平均電力（間引き）を測り、約 0.5 秒ごとに hardwaregain を目標へ寄せる
        {
            double acc = 0.0;
            size_t cnt = 0;
            for (size_t i = 0; i < n; i += 64) {
                acc += std::norm(out[i]);
                ++cnt;
            }
            agc_acc += acc;
            agc_cnt += static_cast<double>(cnt);
            if (++agc_blocks >= kAgcBlocks) {
                const double level = 10.0 * std::log10(agc_acc / std::max(agc_cnt, 1.0) + 1e-30);
                level_dbfs_ = level;
                agc_acc = 0.0;
                agc_cnt = 0.0;
                agc_blocks = 0;
                if (cfg_.agc && !cfg_.hw_agc) {
                    const double diff = cfg_.agc_target_dbfs - level;
                    if (std::fabs(diff) >= 1.0) {
                        const double step = std::max(-12.0, std::min(12.0, diff));
                        const double g = std::max(0.0, std::min(73.0, std::round(gain_db_ + step)));
                        if (g != gain_db_) {
                            std::lock_guard<std::mutex> lk(mu_);
                            if (connected_ && impl_->phy_rx) {
                                try {
                                    WriteAttr(impl_->phy_rx, "hardwaregain",
                                              std::to_string(static_cast<int>(g)));
                                    gain_db_ = g;
                                } catch (const std::exception& e) {
                                    std::fprintf(stderr, "sdr: agc gain write failed: %s\n", e.what());
                                }
                            }
                        }
                    }
                }
            }
        }
        // UI_OVF 監視（約 1 秒周期。reg アクセスは制御転送 1 回で、ストリームへの影響は無視できる）
        if (ovf_usable) {
            if (!ovf_armed) {
                // 取り込み停止中は常時立つのが正常なので、DMA が流れ始めてからクリアする
                ovf_armed = true;
                ovf_countdown = kOvfCheckBlocks;
                if (iio_device_reg_write(impl_->rxdev, kPcoreRegMagic | kRegUiStatus, kUiOvf) < 0) {
                    ovf_usable = false;
                }
            } else if (--ovf_countdown <= 0) {
                ovf_countdown = kOvfCheckBlocks;
                uint32_t v = 0;
                if (iio_device_reg_read(impl_->rxdev, kPcoreRegMagic | kRegUiStatus, &v) < 0) {
                    ovf_usable = false;
                } else if (v & kUiOvf) {
                    ++hw_ovf_;
                    (void)iio_device_reg_write(impl_->rxdev, kPcoreRegMagic | kRegUiStatus, kUiOvf);
                    std::fprintf(stderr, "sdr: hardware overflow (sample drop) #%lld\n",
                                 static_cast<long long>(hw_ovf_));
                }
            }
        }
    }
}

#endif  // ISDBT_WITH_LIBIIO

double SdrSource::DefaultFsFor(int sample_bytes) {
    return (sample_bytes == 2) ? 8'126'984.0 : 7'111'111.0;
}

}  // namespace isdbt
