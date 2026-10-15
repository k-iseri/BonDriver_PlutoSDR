#include "sdr_source.h"

#include <cmath>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#ifdef ISDBT_WITH_LIBIIO
#include <iio/iio.h>
#endif

namespace isdbt {

#ifndef ISDBT_WITH_LIBIIO

struct SdrSource::Impl {};
SdrSource::~SdrSource() {}
void SdrSource::Start(const SdrConfig&, BlockFn, EventFn, SymbolFn) {
    throw std::runtime_error(
        "libiio 無しでビルドされている。rt\\third_party\\fetch_libiio.ps1 を実行してから make rt し直す");
}
void SdrSource::Retune(double) {}
void SdrSource::SetBandwidth(double) {}
bool SdrSource::SuggestClockPpm(double) { return false; }
bool SdrSource::ApplyXoPpmLocked(::iio_device*, double) { return false; }
void SdrSource::EnableFpgaFrontEnd(bool) {}
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

// デバイス属性（ad9361-phy の xo_correction など）
void WriteDevAttr(const iio_device* dev, const char* name, const std::string& value) {
    const iio_attr* attr = iio_device_find_attr(dev, name);
    if (!attr) throw std::runtime_error(std::string("device attr not found: ") + name);
    const ssize_t r = iio_attr_write_string(attr, value.c_str());
    if (r < 0) Fail(std::string("write ") + name + "=" + value, static_cast<int>(r));
}
std::string ReadDevAttr(const iio_device* dev, const char* name) {
    const iio_attr* attr = iio_device_find_attr(dev, name);
    if (!attr) throw std::runtime_error(std::string("device attr not found: ") + name);
    char buf[64] = {};
    const ssize_t r = iio_attr_read_raw(attr, buf, sizeof(buf) - 1);
    if (r < 0) Fail(std::string("read ") + name, static_cast<int>(r));
    return std::string(buf);
}

// axi_ad9361 コア（cf-ad9361-lpc）の overflow 監視。
// cf_axi_adc の debugfs_reg_access はアドレスにこのマジックを立てると AXI コアレジスタを
// 直接読み書きする（純正ファームで動作確認 2026-10-12）。UI_STATUS の UI_OVF（RW1C）は
// DMA が受けきれずサンプルが落ちると立つ。取り込み停止中は常時立つのが正常なので、
// DMA が流れ始めてからクリアし、取り込み中に立ったものだけをドロップとして数える。
constexpr uint32_t kPcoreRegMagic = 0x80000000;
constexpr uint32_t kRegUiStatus = 0x0088;   // adi_regmap_adc: UI_STATUS
constexpr uint32_t kUiOvf = 1u << 2;        // bit2 = UI_OVF (RW1C)
constexpr long long kOvfCheckBlocks = 8;    // 約 1 秒周期（1 ブロック ≈ 129〜147 ms）

// pluto-fw-isdbt ファームのサンプルカウンタブロック（axi_ad9361 コアのバイト 0x3000。
// hdl フォーク library/axi_ad9361/isdbt_sample_counter.v）。純正コアはこの領域に応答しない
//（0xDEADDEAD を返す）ので MAGIC で判別する。すべて読み出し専用、32bit はラップする
constexpr uint32_t kRegCntMagic = 0x3000;   // 0x49534442 "ISDB"
constexpr uint32_t kRegCntOvfCnt = 0x3010;  // adc_dovf 立ち上がり回数（overflow イベント）
constexpr uint32_t kRegCntDropCnt = 0x3014; // adc_dovf が立っていたクロック数 = 捨てられた DMA ビート数
constexpr uint32_t kRegCntFormat = 0x3018;  // bit[7:0] = pack8 の右シフト量（8bit 値 × 2^shift = 12bit 値）
constexpr uint32_t kCntMagic = 0x49534442;
constexpr long long kSamplesPerBeat = 2;    // 1R1T では 16bit/8bit どちらも 1 ビート = 複素 2 サンプル

// pluto-fw-isdbt v2 の前段レジスタ（rtl/isdbt_regs.v）と DMA フレーム（rtl/isdbt_fe_out.v）
constexpr uint32_t kRegCaps = 0x301C;       // bit0 8bit、bit1 カウンタ、bit2 FFT 出力、bit3 等化済みキャリア出力
constexpr uint32_t kRegMode = 0x3020;       // 0 raw I/Q、3 同期 FFT 8bit ビン、4 等化済みキャリア
constexpr uint32_t kRegDecim = 0x302C;
constexpr uint32_t kRegSyncStatus = 0x3038;
constexpr uint32_t kRegEqCtrl = 0x3040;     // bit[7:0] δ、bit8 AFC、bit9 CPE
constexpr uint32_t kRegEqStatus = 0x3044;
constexpr uint32_t kRegAdcLevel = 0x3054;   // pack8 後の I²+Q² を 65536 サンプル積算（CAPS bit4、VERSION 4）
constexpr uint32_t kCapsEq = 1u << 3;
constexpr uint32_t kCapsLevel = 1u << 4;
constexpr uint32_t kHdrMagic = 0x49534254;  // "ISBT"
constexpr int kFeN = 8192;
constexpr int kFeTotal = 5617;
constexpr int kFeCenter = 2808;
constexpr int kFeEqWords = (kFeTotal * 3 + 3) / 4;   // 4213
constexpr int kFeSettleBlocks = 8;          // raw でソフト AGC を走らせる時間（≈ 1.0 秒。AGC は 4 ブロックごとに 1 段動く。5 だと選局後の同期失敗が増えた）
constexpr int kFeCfoFrames = 128;           // 整数 CFO と窓開始の傾き（クロック偏差）の推定に使う同期 FFT の窓数（約 0.15 s）
constexpr double kFeSymbolSamples = 9216.0; // 前段が前提とするシンボル長（Mode 3、GI 1/8）
constexpr double kXoNominalHz = 40e6;       // Pluto の XO 公称
constexpr int kFeCfoTimeoutBlocks = 40;     // モード 3 で窓が来ないときの諦め（≈ 5 秒 → raw へ戻してやり直す）
constexpr int kFeWarmupSymbols = 1024;       // モード 4 に入ってから捨てるシンボル数（時間軸の立ち上がり 256 シンボルで B 層が崩れる）

// DMA ワード列から前段フレーム（ヘッダ 8 ワード + 本体）を切り出す。ブロック境界をまたぐので持ち越す
class FeParser {
public:
    void Reset() { buf_.clear(); }
    template <typename Fn>
    void Feed(const uint8_t* p, size_t n, Fn&& on_frame) {
        buf_.insert(buf_.end(), p, p + n);
        size_t pos = 0;
        while (buf_.size() - pos >= 32) {
            const uint32_t* w = reinterpret_cast<const uint32_t*>(buf_.data() + pos);
            if (w[0] != kHdrMagic) { pos += 4; continue; }
            const int fmt = static_cast<int>((w[1] >> 24) & 3);
            const size_t nw = (fmt == 2) ? kFeEqWords : (fmt == 1) ? kFeN / 2 : kFeN;
            if (buf_.size() - pos < (8 + nw) * 4) break;
            on_frame(w, fmt, nw);
            if (buf_.empty()) return;       // コールバックが Reset() した
            pos += (8 + nw) * 4;
        }
        if (pos) buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos));
    }
private:
    std::vector<uint8_t> buf_;
};

}  // namespace

struct SdrSource::Impl {
    iio_context* ctx = nullptr;
    iio_channels_mask* mask = nullptr;
    iio_stream* stream = nullptr;
    iio_channel* phy_rx = nullptr;      // ad9361-phy voltage0 (input): rate/bw/gain
    iio_channel* phy_lo = nullptr;      // ad9361-phy altvoltage0 (output): RX LO
    iio_device* rxdev = nullptr;        // 非 const（iio_device_reg_read/write が要求）
    iio_device* phy = nullptr;          // ad9361-phy（xo_correction）
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

void SdrSource::Start(const SdrConfig& cfg, BlockFn on_block, EventFn on_event, SymbolFn on_symbol) {
    impl_ = new Impl;
    on_block_ = std::move(on_block);
    on_event_ = std::move(on_event);
    on_symbol_ = std::move(on_symbol);
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
        // ブロック待ちの上限。モード 3/4 で同期が取れず窓が来ないときに早く raw へ戻すため短めに
        (void)iio_context_set_timeout(ctx, 1500);

        iio_device* phy = iio_context_find_device(ctx, "ad9361-phy");
        iio_device* rxdev = iio_context_find_device(ctx, "cf-ad9361-lpc");
        if (!phy || !rxdev) throw std::runtime_error("ad9361-phy / cf-ad9361-lpc が見つからない（Pluto 系か確認）");
        im.rxdev = rxdev;
        im.phy = phy;

        im.phy_rx = iio_device_find_channel(phy, "voltage0", false);
        im.phy_lo = iio_device_find_channel(phy, "altvoltage0", true);
        if (!im.phy_rx || !im.phy_lo) throw std::runtime_error("ad9361-phy のチャンネルが見つからない");
        // XO 補正（固定値指定）。自動は同期で測ってから SuggestClockPpm / 前段のモード 3 で書く
        xo_applied_ = false;
        xo_ppm_ = 0.0;
        if (cfg_.xo_mode == 1) {
            std::lock_guard<std::mutex> lk(mu_);
            ApplyXoPpmLocked(phy, cfg_.xo_ppm);
        }
        WriteAttr(im.phy_rx, "sampling_frequency", FormatLL(cfg_.fs));
        WriteAttr(im.phy_rx, "rf_bandwidth", FormatLL(cfg_.bw));
        double freq;
        {
            std::lock_guard<std::mutex> lk(mu_);
            freq = cfg_.freq_hz;        // 切断中に Retune された周波数を拾う
        }
        try {
            WriteAttr(im.phy_lo, "frequency", FormatLL(freq));
            muted_ = false;
        } catch (const std::exception& e) {
            // LO の範囲外（AD9363 は 325 MHz〜。CATV 空間の VHF 1〜12ch などは設定できない）。接続は続け、
            // 有効な周波数が来るまで I/Q を止める（止めないと前の周波数の TS が出続け、スキャンで同じ局が並ぶ）
            muted_ = true;
            std::fprintf(stderr, "sdr: frequency %.3f MHz not accepted (%s); no I/Q until a valid frequency is set\n",
                         freq / 1e6, e.what());
        }

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
        // ゲインモードはサンプル幅が分かってから決める: 8bit ファーム（+6 dB 前置ゲイン、FS = ADC −6 dBFS）
        // では AD936x の slow_attack（帯域内 −11 dBFS 程度）だと OFDM のピークが約 1% クリップする（実測）ので、
        // Gain=hw が指定されていてもソフト AGC に切り替える
        bool hw_agc = cfg_.agc && cfg_.hw_agc;
        if (hw_agc && ss == 2) {
            std::fprintf(stderr, "sdr: 8bit firmware: hardware slow_attack AGC would clip, using software AGC\n");
            hw_agc = false;
            cfg_.hw_agc = false;    // Run() のソフト AGC ループも有効にする
        }
        if (hw_agc) {
            WriteAttr(im.phy_rx, "gain_control_mode", "slow_attack");
        } else {
            // 手動 / ソフト AGC（再接続時は直前のゲインを引き継ぐ）
            if (gain_db_ == 0.0 && cfg_.gain_db != 0.0) gain_db_ = cfg_.gain_db;
            WriteAttr(im.phy_rx, "gain_control_mode", "manual");
            WriteAttr(im.phy_rx, "hardwaregain", std::to_string(static_cast<int>(std::lround(gain_db_))));
        }
        im.block_samples = cfg_.block_samples;
        iio_stream* st = iio_buffer_create_stream(buf, cfg_.nb_blocks, cfg_.block_samples, im.mask);
        CheckPtr(st, "iio_buffer_create_stream");
        im.stream = st;
        {
            uint32_t magic = 0;
            hw_counter_ = (iio_device_reg_read(rxdev, kPcoreRegMagic | kRegCntMagic, &magic) == 0 &&
                           magic == kCntMagic);
            // 8bit 値のフルスケール換算: 情報ブロックがあればその申告（前置ゲイン分）、無ければ素直な 12→8bit
            uint32_t shift = 4;
            if (hw_counter_ && iio_device_reg_read(rxdev, kPcoreRegMagic | kRegCntFormat, &shift) == 0) {
                shift &= 0xFF;
                if (shift > 11) shift = 4;
            } else {
                shift = 4;
            }
            pack_shift_ = static_cast<int>(shift);
            // FPGA 前段（等化器）の有無。接続のたびに raw（モード 0）から始める
            uint32_t caps = 0;
            fpga_capable_ = hw_counter_ && iio_device_reg_read(rxdev, kPcoreRegMagic | kRegCaps, &caps) == 0 &&
                            (caps & kCapsEq) != 0;
            if (fpga_capable_) (void)iio_device_reg_write(rxdev, kPcoreRegMagic | kRegMode, 0);
            fpga_level_ = fpga_capable_ && (caps & kCapsLevel) != 0;
            if (cfg_.fpga_eq == 1 && !fpga_capable_) throw std::runtime_error("fpga_eq=1 だがファームに等化器が無い（CAPS bit3）");
            fpga_eq_ = false;
        }
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

bool SdrSource::ApplyXoPpmLocked(::iio_device* phy, double ppm) {
    if (!phy) return false;
    try {
        const double cur = std::atof(ReadDevAttr(phy, "xo_correction").c_str());
        const double base = (cur > 1e6) ? cur : kXoNominalHz;
        const long long next = std::llround(base * (1.0 + ppm * 1e-6));
        WriteDevAttr(phy, "xo_correction", std::to_string(next));
        xo_ppm_ = (static_cast<double>(next) / kXoNominalHz - 1.0) * 1e6;
        xo_applied_ = true;
        std::fprintf(stderr, "sdr: xo_correction %lld -> %lld Hz (%+.2f ppm; measured %+.2f ppm)\n",
                     static_cast<long long>(cur), next, xo_ppm_.load(), ppm);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "sdr: xo_correction failed: %s\n", e.what());
        xo_applied_ = true;             // 二度と試さない（純正でない phy など）
        return false;
    }
}

bool SdrSource::SuggestClockPpm(double ppm) {
    if (cfg_.xo_mode != -1 || xo_applied_) return false;
    if (!(std::fabs(ppm) >= 1.0 && std::fabs(ppm) <= 100.0)) return false;    // 測定不良や NaN は無視
    std::lock_guard<std::mutex> lk(mu_);
    if (!connected_ || !impl_) return false;
    if (!ApplyXoPpmLocked(impl_->phy, ppm)) return false;
    if (fpga_capable_) fe_restart_ = true;      // LO とレートが変わったので前段は raw からやり直す
    return true;
}

void SdrSource::Retune(double freq_hz) {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.freq_hz = freq_hz;
    if (!connected_ || !impl_->phy_lo) return;      // 再接続時に Connect() が適用する
    try {
        WriteAttr(impl_->phy_lo, "frequency", FormatLL(freq_hz));
        muted_ = false;
        if (fpga_capable_) fe_restart_ = true;      // raw からやり直す（AGC → δ → モード 4）
    } catch (const std::exception& e) {
        // LO の範囲外（AD9363 は 325 MHz〜。CATV 空間の VHF 1〜12ch は 93〜222 MHz で設定できない）か、
        // デバイスが消えた直後。どちらも有効な周波数が設定できるまで I/Q を止める。止めないと前の周波数の
        // TS が出続け、TVTest のチャンネルスキャンで同じ局が何度も見つかる（2026-10-15 CATV で発覚）。
        // デバイス消失ならストリーム側もエラーになり再接続が走り、Connect() が cfg_.freq_hz を適用し直す
        muted_ = true;
        std::fprintf(stderr, "sdr: retune to %.3f MHz failed (%s); no I/Q until a valid frequency is set\n",
                     freq_hz / 1e6, e.what());
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

void SdrSource::EnableFpgaFrontEnd(bool on) {
    fe_enabled_ = on;
    if (!on) fe_restart_ = true;        // 受信スレッドが raw（モード 0）へ戻す
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
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (fpga_capable_ && connected_ && impl_->rxdev) (void)iio_device_reg_write(impl_->rxdev, kPcoreRegMagic | kRegMode, 0);
    }
    Teardown();
    delete impl_;
    impl_ = nullptr;
}

void SdrSource::Run() {
    std::vector<std::complex<float>> out(impl_->block_samples);
    // 純正ファーム: int16 の下位 12bit（AD936x は 12bit）。8bit ファーム: 12bit を pack_shift_ 右シフト（丸め）
    // した int8 なので 2^shift/2048（shift 4 で 1/128、前置ゲイン +6 dB の shift 3 で 1/256）。
    // どちらも ADC フルスケール = ±1.0 に揃うので、ソフト AGC の dBFS 目標はファームに依らず同じ意味
    constexpr float kScale16 = 1.0f / 2048.0f;
    const float kScale8 = static_cast<float>(1 << pack_shift_) / 2048.0f;
    int backoff_ms = 500;
    bool ovf_usable = true;         // reg アクセス不能なコア（将来の別ボード等）では監視を止める
    bool ovf_armed = false;         // 最初のブロック到着（= DMA 稼働）後にクリアしてから監視を始める
    long long ovf_countdown = kOvfCheckBlocks;
    // サンプルカウンタ（対応ファームのみ）: ストリーム開始前は DMA が止まっていて全ビートが overflow に
    // 数えられるので、UI_OVF と同じタイミングで基準値を取り、以降の増分だけを積む
    uint32_t cnt_drop_last = 0, cnt_ovf_last = 0;
    auto read_counters = [&](bool baseline) {
        uint32_t drop = 0, ovf = 0;
        if (iio_device_reg_read(impl_->rxdev, kPcoreRegMagic | kRegCntDropCnt, &drop) < 0 ||
            iio_device_reg_read(impl_->rxdev, kPcoreRegMagic | kRegCntOvfCnt, &ovf) < 0) {
            return;
        }
        if (!baseline) {
            hw_dropped_samples_ += kSamplesPerBeat * static_cast<long long>(drop - cnt_drop_last);
            hw_ovf_events_ += static_cast<long long>(ovf - cnt_ovf_last);
        }
        cnt_drop_last = drop;
        cnt_ovf_last = ovf;
    };
    constexpr int kAgcBlocks = 4;   // 約 0.5 秒
    double agc_acc = 0.0, agc_cnt = 0.0;
    int agc_blocks = 0;
    // 帯域内レベル [dBFS] から hardwaregain を目標へ寄せる（raw の測定と、モード 3/4 中のファームのレベル計で共用）
    auto agc_apply = [&](double level) {
        level_dbfs_ = level;
        if (!(cfg_.agc && !cfg_.hw_agc)) return;
        const double diff = cfg_.agc_target_dbfs - level;
        if (std::fabs(diff) < 1.0) return;
        const double step = std::max(-12.0, std::min(12.0, diff));
        const double g = std::max(0.0, std::min(73.0, std::round(gain_db_ + step)));
        if (g == gain_db_) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (connected_ && impl_->phy_rx) {
            try {
                WriteAttr(impl_->phy_rx, "hardwaregain", std::to_string(static_cast<int>(g)));
                gain_db_ = g;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "sdr: agc gain write failed: %s\n", e.what());
            }
        }
    };
    // FPGA 前段の状態: raw（AGC）→ cfo（モード 3 で整数 CFO）→ eq（モード 4）
    enum class Fe { kRaw, kCfo, kEqWait, kEq };
    Fe fe = Fe::kRaw;
    int fe_blocks = 0;
    FeParser parser;
    std::vector<double> cfo_pow(static_cast<size_t>(kFeN), 0.0);
    int cfo_frames = 0;
    std::vector<double> cfo_seq, cfo_start;     // モード 3 の窓ヘッダ（連番、窓開始）: 傾きからクロック偏差
    uint32_t cfo_start_prev = 0;
    uint64_t cfo_start_hi = 0;
    const bool fe_wanted = fpga_capable_ && cfg_.fpga_eq != 0 && on_symbol_;
    FILE* fe_dump = nullptr;            // 開発用: ISDBT_FE_DUMP=path で前段フレーム列をそのまま書く
    if (const char* dp = std::getenv("ISDBT_FE_DUMP")) fe_dump = std::fopen(dp, "wb");
    auto fe_write = [&](uint32_t reg, uint32_t v) {
        std::lock_guard<std::mutex> lk(mu_);
        if (connected_ && impl_->rxdev) (void)iio_device_reg_write(impl_->rxdev, kPcoreRegMagic | reg, v);
    };
    int fe_warm = 0;                    // モード 4 に入ってから受けたシンボル数（kFeWarmupSymbols まで捨てる）
    auto fe_to_raw = [&]() {
        fe_write(kRegMode, 0);
        fe = Fe::kRaw;
        fe_blocks = 0;
        fe_warm = 0;
        fpga_eq_ = false;
        parser.Reset();
        fe_start_init_ = false;
        fe_start_hi_ = 0;
    };
    auto fe_car0 = [&]() { return (kFeCenter - fpga_delta_ + kFeTotal) % kFeTotal; };
    while (!stop_) {
        const iio_block* blk = iio_stream_get_next_block(impl_->stream);
        if (iio_err(blk)) {
            if (stop_) break;
            if (iio_err(blk) == -110 && fe != Fe::kRaw) {
                // モード 3/4 では同期の取れた窓しか DMA に流れない。選局直後などで同期が取れないと
                // ブロックが埋まらず libiio の待ちがタイムアウトする（−110）。ストリーム断ではないので
                // raw（モード 0）に戻して AGC からやり直す（再接続すると 8 秒止まる。2026-10-15 スキャンで発覚）
                std::fprintf(stderr, "sdr: no front-end frames (sync not locked?); back to raw\n");
                fe_to_raw();
                ++fe_timeouts_;
                continue;
            }
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
                fe = Fe::kRaw;          // Connect() がモード 0 にしている
                fe_blocks = 0;
                fpga_eq_ = false;
                parser.Reset();
                std::fprintf(stderr, "sdr: reconnected (#%lld)\n",
                             static_cast<long long>(reconnects_));
                if (on_event_) on_event_(true);
                break;
            }
            continue;
        }
        if (fe_restart_.exchange(false) && fe != Fe::kRaw) fe_to_raw();
        if (muted_) {
            // 周波数が設定できていない: 何も渡さない（復調側は無信号として扱う）。前段も止めておく
            if (fe != Fe::kRaw) fe_to_raw();
            fe_blocks = 0;
            ++blocks_;
            continue;
        }
        if (fe != Fe::kRaw) {
            // ---- FPGA 前段のフレーム列（32bit ワード）----
            const uint8_t* p = static_cast<const uint8_t*>(iio_block_start(blk));
            const uint8_t* e = static_cast<const uint8_t*>(iio_block_end(blk));
            ++blocks_;
            ++fe_blocks;
            if (fe_dump) std::fwrite(p, 1, static_cast<size_t>(e - p), fe_dump);
            parser.Feed(p, static_cast<size_t>(e - p), [&](const uint32_t* w, int fmt, size_t /*nw*/) {
                if (fe == Fe::kCfo && fmt == 1) {
                    // 同期 FFT 8bit ビン: 電力を積算して帯域（5617 本）の位置から整数 CFO を求める
                    const int8_t* b = reinterpret_cast<const int8_t*>(w + 8);
                    for (int i = 0; i < kFeN; ++i) {
                        const double bi = b[2 * i], bq = b[2 * i + 1];
                        cfo_pow[static_cast<size_t>(i)] += bi * bi + bq * bq;
                    }
                    {
                        const uint32_t lo = w[2];
                        if (!cfo_seq.empty() && lo < cfo_start_prev && (cfo_start_prev - lo) > 0x80000000u) cfo_start_hi += 1;
                        cfo_start_prev = lo;
                        cfo_seq.push_back(static_cast<double>(w[4]));
                        cfo_start.push_back(static_cast<double>((cfo_start_hi << 32) | lo));
                    }
                    if (++cfo_frames >= kFeCfoFrames) {
                        // 窓開始の連番に対する傾き = 実測シンボル長 → サンプルクロック偏差。|ppm| ≥ 1 なら XO を補正して
                        // raw からやり直す（接続ごとに 1 回。SFO が残ると帯域端の ICI で B 層が頭打ちになる）
                        if (cfg_.xo_mode == -1 && !xo_applied_ && cfo_seq.size() >= 32) {
                            const size_t n = cfo_seq.size();
                            double mx = 0.0, my = 0.0;
                            for (size_t i = 0; i < n; ++i) { mx += cfo_seq[i]; my += cfo_start[i] - cfo_start[0]; }
                            mx /= static_cast<double>(n); my /= static_cast<double>(n);
                            double sxx = 0.0, sxy = 0.0;
                            for (size_t i = 0; i < n; ++i) {
                                const double dx = cfo_seq[i] - mx;
                                sxx += dx * dx;
                                sxy += dx * ((cfo_start[i] - cfo_start[0]) - my);
                            }
                            if (sxx > 0.0) {
                                const double ppm = (sxy / sxx / kFeSymbolSamples - 1.0) * 1e6;
                                if (SuggestClockPpm(ppm)) {
                                    fe_restart_ = false;
                                    fe_to_raw();
                                    return;
                                }
                            }
                        }
                        int best_d = 0;
                        double best_e = -1.0;
                        for (int d = -100; d <= 100; ++d) {
                            double e_sum = 0.0;
                            for (int k = 0; k < kFeTotal; ++k) {
                                const int bin = ((k - kFeCenter + d) % kFeN + kFeN) % kFeN;   // キャリア k のビン
                                e_sum += cfo_pow[static_cast<size_t>(bin)];
                            }
                            if (e_sum > best_e) { best_e = e_sum; best_d = d; }
                        }
                        if (std::abs(best_d) >= 60) {
                            // 帯域が見えていない（無信号・取り込み不良）。raw に戻して AGC からやり直す
                            std::fprintf(stderr, "sdr: fpga front-end: integer CFO estimate %d unreliable; retrying\n", best_d);
                            fe_to_raw();
                            return;
                        }
                        fpga_delta_ = best_d;
                        // いったんモード 0 に戻してから等化へ（tools/fe_eq_test.py と同じ手順。モード 3 から直接 4 に
                        // すると時間軸のずれ Δ が −10 サンプル以上に育ち MER が 10 dB 台に落ちた、2026-10-15）
                        fe_write(kRegMode, 0);
                        fe = Fe::kEqWait;
                        fe_blocks = 0;
                        parser.Reset();
                        std::fprintf(stderr, "sdr: fpga front-end: integer CFO %d bins\n", best_d);
                    }
                } else if (fe == Fe::kEq && fmt == 2) {
                    if (fe_warm < kFeWarmupSymbols) { ++fe_warm; return; }    // 立ち上がりは捨てる（先頭ウィンドウの TEI を防ぐ）
                    EqSymbol sym;
                    // 窓開始サンプル番号: ヘッダは 64bit だが v2.0.0 までのファームは下位 32bit しか入れていない
                    // （8.127 MSPS では 528 秒で一周し、継ぎ合わせのフレーム番号が戻って TS が止まった、2026-10-15）。
                    // 下位 32bit だけを見て周回をホストで数える（上位ワードが正しいファームでも同じ結果になる）
                    {
                        const uint32_t lo = w[2];
                        if (fe_start_init_ && lo < fe_start_lo_ && (fe_start_lo_ - lo) > 0x80000000u) fe_start_hi_ += 1;
                        fe_start_lo_ = lo;
                        fe_start_init_ = true;
                        sym.start = (fe_start_hi_ << 32) | lo;
                        fpga_last_start_ = sym.start;
                        fpga_last_start_hi_ = w[3];
                    }
                    sym.seq = w[4];
                    sym.delta_q = static_cast<int32_t>(w[5]);
                    sym.status = w[7];
                    sym.raw.resize(static_cast<size_t>(kFeTotal) * 3);
                    // FPGA はビン順（キャリア car0 から 5616、続いて 0 から）で出す → キャリア順へ
                    const uint8_t* b = reinterpret_cast<const uint8_t*>(w + 8);
                    const int car0 = fe_car0();
                    for (int k = 0; k < kFeTotal; ++k) {
                        const int i = (k - car0 + kFeTotal) % kFeTotal;
                        sym.raw[static_cast<size_t>(k) * 3 + 0] = b[i * 3 + 0];
                        sym.raw[static_cast<size_t>(k) * 3 + 1] = b[i * 3 + 1];
                        sym.raw[static_cast<size_t>(k) * 3 + 2] = b[i * 3 + 2];
                    }
                    ++fpga_symbols_;
                    on_symbol_(sym);
                }
            });
            if (fe == Fe::kEqWait && fe_blocks >= 2) {       // ≈ 260 ms モード 0 で待ってから等化へ
                fe_write(kRegEqCtrl, static_cast<uint32_t>(fpga_delta_.load() & 0xFF) | (0x3u << 8));
                fe_write(kRegMode, 4);
                fe = Fe::kEq;
                fe_blocks = 0;
                fe_warm = 0;
                fpga_eq_ = true;
                std::fprintf(stderr, "sdr: fpga front-end: equalized carriers (mode 4)\n");
            }
            if (fe == Fe::kCfo && fe_blocks > kFeCfoTimeoutBlocks) {
                std::fprintf(stderr, "sdr: fpga front-end: no synchronized windows (not locked?); retrying\n");
                fe_to_raw();
            }
            if (ovf_usable && --ovf_countdown <= 0) {
                ovf_countdown = kOvfCheckBlocks;
                bool have_level = false;
                uint32_t lvl = 0;
                {
                    uint32_t v = 0;
                    std::lock_guard<std::mutex> lk(mu_);
                    if (connected_ && impl_->rxdev) {
                        if (iio_device_reg_read(impl_->rxdev, kPcoreRegMagic | kRegSyncStatus, &v) == 0) fe_sync_status_ = v;
                        if (iio_device_reg_read(impl_->rxdev, kPcoreRegMagic | kRegEqStatus, &v) == 0) fe_eq_status_ = v;
                        if (fpga_level_ && iio_device_reg_read(impl_->rxdev, kPcoreRegMagic | kRegAdcLevel, &lvl) == 0) have_level = true;
                    }
                }
                if (have_level) {
                    // ファームのレベル計（pack8 後の 8bit I/Q の I²+Q² × 65536 サンプル）→ raw と同じ dBFS 定義
                    const double s8 = static_cast<double>(1 << pack_shift_) / 2048.0;
                    const double level = 10.0 * std::log10(static_cast<double>(lvl) / 65536.0 * s8 * s8 + 1e-30);
                    agc_apply(level);
                }
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
        // raw で AGC が落ち着いたら FPGA 前段へ（モード 3 で整数 CFO を求めてからモード 4）
        if (fe_wanted && fe_enabled_ && ++fe_blocks >= kFeSettleBlocks) {
            parser.Reset();
            std::fill(cfo_pow.begin(), cfo_pow.end(), 0.0);
            cfo_frames = 0;
            cfo_seq.clear();
            cfo_start.clear();
            cfo_start_hi = 0;
            fe_write(kRegDecim, 1);
            fe_write(kRegMode, 3);
            fe = Fe::kCfo;
            fe_blocks = 0;
        }
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
                agc_acc = 0.0;
                agc_cnt = 0.0;
                agc_blocks = 0;
                agc_apply(level);
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
                if (hw_counter_) read_counters(true);
            } else if (--ovf_countdown <= 0) {
                ovf_countdown = kOvfCheckBlocks;
                if (hw_counter_) {
                    const long long before = hw_dropped_samples_;
                    read_counters(false);
                    if (hw_dropped_samples_ != before) {
                        std::fprintf(stderr, "sdr: hardware drop counter: +%lld samples (total %lld)\n",
                                     static_cast<long long>(hw_dropped_samples_ - before),
                                     static_cast<long long>(hw_dropped_samples_));
                    }
                }
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
    if (fe_dump) std::fclose(fe_dump);
}

#endif  // ISDBT_WITH_LIBIIO

double SdrSource::DefaultFsFor(int sample_bytes) {
    return (sample_bytes == 2) ? 8'126'984.0 : 7'111'111.0;
}

}  // namespace isdbt
