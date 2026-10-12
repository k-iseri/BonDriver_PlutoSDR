#include "sdr_source.h"

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
void SdrSource::Stop() {}
void SdrSource::Run() {}
void SdrSource::Connect() {}
void SdrSource::Teardown() {}

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

}  // namespace

struct SdrSource::Impl {
    iio_context* ctx = nullptr;
    iio_channels_mask* mask = nullptr;
    iio_stream* stream = nullptr;
    iio_channel* phy_rx = nullptr;      // ad9361-phy voltage0 (input): rate/bw/gain
    iio_channel* phy_lo = nullptr;      // ad9361-phy altvoltage0 (output): RX LO
    const iio_device* rxdev = nullptr;
    size_t block_samples = 0;
};

SdrSource::~SdrSource() { Stop(); }

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
        if (cfg_.agc) {
            WriteAttr(im.phy_rx, "gain_control_mode", "slow_attack");
        } else {
            WriteAttr(im.phy_rx, "gain_control_mode", "manual");
            WriteAttr(im.phy_rx, "hardwaregain", std::to_string(cfg_.gain_db));
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
        if (ss != 4) {
            throw std::runtime_error("sample size " + std::to_string(ss) +
                                     " != 4（I/Q 16bit 2ch 前提。8bit ファームは未対応）");
        }
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
    constexpr float kScale = 1.0f / 2048.0f;    // AD936x は 12bit（int16 の下位 12bit）
    int backoff_ms = 500;
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
                std::fprintf(stderr, "sdr: reconnected (#%lld)\n",
                             static_cast<long long>(reconnects_));
                if (on_event_) on_event_(true);
                break;
            }
            continue;
        }
        const int16_t* p = static_cast<const int16_t*>(iio_block_start(blk));
        const int16_t* e = static_cast<const int16_t*>(iio_block_end(blk));
        const size_t n = static_cast<size_t>(e - p) / 2;    // 複素サンプル数
        if (out.size() < n) out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = {p[2 * i] * kScale, p[2 * i + 1] * kScale};
        }
        ++blocks_;
        if (on_block_) on_block_(out.data(), n);
    }
}

#endif  // ISDBT_WITH_LIBIIO

}  // namespace isdbt
