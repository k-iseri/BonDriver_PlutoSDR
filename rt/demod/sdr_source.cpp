#include "sdr_source.h"

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
void SdrSource::Start(const SdrConfig&, BlockFn) {
    throw std::runtime_error(
        "libiio 無しでビルドされている。rt\\third_party\\fetch_libiio.ps1 を実行してから make rt し直す");
}
void SdrSource::Retune(double) {}
void SdrSource::Stop() {}
void SdrSource::Run() {}

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

void SdrSource::Start(const SdrConfig& cfg, BlockFn on_block) {
    impl_ = new Impl;
    on_block_ = std::move(on_block);
    Impl& im = *impl_;

    iio_context* ctx = iio_create_context(nullptr, cfg.uri.c_str());
    CheckPtr(ctx, "iio_create_context");
    im.ctx = ctx;

    iio_device* phy = iio_context_find_device(ctx, "ad9361-phy");
    iio_device* rxdev = iio_context_find_device(ctx, "cf-ad9361-lpc");
    if (!phy || !rxdev) throw std::runtime_error("ad9361-phy / cf-ad9361-lpc が見つからない（Pluto 系か確認）");
    im.rxdev = rxdev;

    im.phy_rx = iio_device_find_channel(phy, "voltage0", false);
    im.phy_lo = iio_device_find_channel(phy, "altvoltage0", true);
    if (!im.phy_rx || !im.phy_lo) throw std::runtime_error("ad9361-phy のチャンネルが見つからない");
    WriteAttr(im.phy_rx, "sampling_frequency", FormatLL(cfg.fs));
    WriteAttr(im.phy_rx, "rf_bandwidth", FormatLL(cfg.bw));
    if (cfg.agc) {
        WriteAttr(im.phy_rx, "gain_control_mode", "slow_attack");
    } else {
        WriteAttr(im.phy_rx, "gain_control_mode", "manual");
        WriteAttr(im.phy_rx, "hardwaregain", std::to_string(cfg.gain_db));
    }
    WriteAttr(im.phy_lo, "frequency", FormatLL(cfg.freq_hz));

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
    im.block_samples = cfg.block_samples;
    iio_stream* st = iio_buffer_create_stream(buf, cfg.nb_blocks, cfg.block_samples, im.mask);
    CheckPtr(st, "iio_buffer_create_stream");
    im.stream = st;

    desc_ = "sdr:" + cfg.uri;
    stop_ = false;
    thread_ = std::thread(&SdrSource::Run, this);
}

void SdrSource::Retune(double freq_hz) {
    if (impl_ && impl_->phy_lo) WriteAttr(impl_->phy_lo, "frequency", FormatLL(freq_hz));
}

void SdrSource::Stop() {
    if (!impl_) return;
    stop_ = true;
    if (impl_->stream) iio_stream_cancel(impl_->stream);
    if (thread_.joinable()) thread_.join();
    if (impl_->stream) iio_stream_destroy(impl_->stream);
    if (impl_->mask) iio_channels_mask_destroy(impl_->mask);
    if (impl_->ctx) iio_context_destroy(impl_->ctx);
    delete impl_;
    impl_ = nullptr;
}

void SdrSource::Run() {
    std::vector<std::complex<float>> out(impl_->block_samples);
    constexpr float kScale = 1.0f / 2048.0f;    // AD936x は 12bit（int16 の下位 12bit）
    while (!stop_) {
        const iio_block* blk = iio_stream_get_next_block(impl_->stream);
        if (iio_err(blk)) {
            if (!stop_) std::fprintf(stderr, "sdr: stream error %d\n", iio_err(blk));
            break;
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
