// LiveDemod の実装。アルゴリズムの説明は live_demod.h 冒頭を参照。
#include "live_demod.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace isdbt {

namespace {
constexpr double kRetuneSkipSeconds = 0.65;    // SDR のブロックキュー（4 × 147 ms = 0.59 s）+ 余裕
constexpr int kFirstWindowNewFrames = 6;        // 選局直後の最初のウィンドウの新規フレーム数（TS まで約 5 秒、
                                                // 排出 5 フレーム ≈ 1.2 s で SDT の繰り返し周期 1 s を覆う）
constexpr int kFirstWindowCtxFrames = 2;        // 同・文脈フレーム数（定常は ctx_frames_ = 4）
constexpr int kFirstWindowNewFramesSym = 4;     // FPGA 等化モードの最初のウィンドウ（文脈 3 + 4 + 1 フレーム ≈ 1.9 s。TVTest のスキャンは選局 9 秒以内に PSI が要る）
}

LiveDemod::LiveDemod(SdrConfig cfg, Mode mode, Gi gi, int new_frames, int ctx_frames, int workers,
                     bool verbose, std::string dump_iq_path, SdrSource* external_sdr)
    : cfg_(std::move(cfg)), mode_(mode), gi_(gi),
      new_frames_(new_frames), ctx_frames_(ctx_frames), workers_(workers),
      in_per_nominal_(cfg_.fs / kSampleRateHz),     // 入力サンプル数 / 公称サンプル数
      frame_samples_(static_cast<double>(SymbolSamples(mode, gi)) * kSymbolsPerFrame *
                     (cfg_.fs / kSampleRateHz)),    // 1 フレームの入力サンプル数
      desc_("sdr:" + cfg_.uri), sdr_(external_sdr ? external_sdr : &own_sdr_),
      external_(external_sdr != nullptr), rate_mon_(cfg_.fs), verbose_(verbose) {
    if (!dump_iq_path.empty()) dump_ = std::fopen(dump_iq_path.c_str(), "wb");
}

void LiveDemod::Start() {
    if (!external_) {
        sdr_->Start(cfg_, [this](const cfloat* p, size_t n) { OnBlock(p, n); },
                    [this](bool up) { OnSdrEvent(up); },
                    [this](const EqSymbol& s) { OnSymbol(s); });
    }
    skip_samples_ = static_cast<long long>(kRetuneSkipSeconds * cfg_.fs);
    quick_level_pending_ = true;
    sched_ = std::thread(&LiveDemod::SchedLoop, this);
    pacer_ = std::thread(&LiveDemod::PaceLoop, this);
}

void LiveDemod::Stop() {
    stop_ = true;
    iq_cv_.notify_all();
    pace_cv_.notify_all();
    if (!external_) sdr_->Stop();
    if (sched_.joinable()) sched_.join();
    if (pacer_.joinable()) pacer_.join();
}

std::shared_ptr<ClientQueue> LiveDemod::AddClient() {
    auto q = std::make_shared<ClientQueue>();
    std::lock_guard<std::mutex> lock(mu_);
    clients_.push_back(q);
    return q;
}

void LiveDemod::AttachClient(const std::shared_ptr<ClientQueue>& q) {
    std::lock_guard<std::mutex> lock(mu_);
    clients_.push_back(q);
}

void LiveDemod::RemoveClient(const std::shared_ptr<ClientQueue>& q) {
    std::lock_guard<std::mutex> lock(mu_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), q), clients_.end());
}

void LiveDemod::Tune(bool has_ch, int ch, double freq) {
    {
        // 同一周波数への tune（クライアントの再接続・選局し直し）ではパイプラインを
        // 壊さない。リセットすると ~10 秒の再立ち上がりが毎回走ってしまう。
        std::lock_guard<std::mutex> lock(mu_);
        if (has_freq_ && freq == freq_hz_) return;
    }
    if (!external_) sdr_->Retune(freq);
    skip_samples_ = static_cast<long long>(kRetuneSkipSeconds * cfg_.fs);
    quick_level_pending_ = true;
    {
        std::lock_guard<std::mutex> pl(pace_mu_);
        pace_q_.clear();
        pace_reset_ = true;
    }
    std::lock_guard<std::mutex> iql(iq_mu_);
    std::lock_guard<std::mutex> lock(mu_);
    ++gen_;                 // 仕掛かり中のウィンドウ結果は捨てる
    iq_q_.clear();
    sym_q_.clear();
    sym_mode_ = false;
    has_channel_ = has_ch;
    channel_ = ch;
    freq_hz_ = freq;
    has_freq_ = true;
    locked_ = false;
    mer_db_ = 0.0;              // 前チャンネルの MER を信号レベルとして返し続けない
    layers_.clear();
    for (auto& q : clients_) {
        std::lock_guard<std::mutex> ql(q->mu);
        q->chunks.clear();
    }
}

std::string LiveDemod::StatusJson() {
    std::lock_guard<std::mutex> lock(mu_);
    std::string s = "{\"ok\": true";
    s += ", \"locked\": " + std::string(locked_ ? "true" : "false");
    s += ", \"signal_db\": " + FormatDouble(mer_db_);
    s += ", \"channel\": " + (has_channel_ ? std::to_string(channel_) : "null");
    s += ", \"freq_hz\": " + (has_freq_ ? FormatDouble(freq_hz_) : "null");
    s += ", \"mode\": " + std::to_string(static_cast<int>(mode_));
    s += ", \"gi\": \"" + gi_.str() + "\"";
    s += ", \"layers\": [";
    for (size_t i = 0; i < layers_.size(); ++i) {
        const LayerDesc& d = layers_[i];
        if (i) s += ", ";
        s += "{\"name\": \"" + d.name + "\", \"mod\": \"" + d.mod + "\", \"rate\": \"" + d.rate +
             "\", \"I\": " + std::to_string(d.I) + ", \"segments\": " + std::to_string(d.segments) + "}";
    }
    s += "]";
    s += ", \"ts_packets\": " + std::to_string(ts_packets_);
    s += ", \"rs_uncorrectable\": " + std::to_string(rs_uncorrectable_);
    s += ", \"dropped\": " + std::to_string(dropped_);
    s += ", \"underruns\": " + std::to_string(underruns_);
    s += ", \"frames_lost\": " + std::to_string(frames_lost_);
    s += ", \"sdr_blocks\": " + std::to_string(sdr_->blocks());
    s += ", \"fpga_eq\": " + std::string(sdr_->fpga_eq() ? "true" : "false");
    s += ", \"fpga_delta\": " + std::to_string(sdr_->fpga_delta());
    s += ", \"xo_ppm\": " + FormatDouble(sdr_->xo_ppm());
    s += ", \"fpga_symbols\": " + std::to_string(sdr_->fpga_symbols());
    s += ", \"fpga_symbol_gaps\": " + std::to_string(sym_gaps_.load());
    s += ", \"fpga_timeouts\": " + std::to_string(sdr_->fpga_timeouts());
    s += ", \"fpga_last_start\": " + std::to_string(sdr_->fpga_last_start());
    s += ", \"fpga_start_hi\": " + std::to_string(sdr_->fpga_last_start_hi());
    s += ", \"splice_resets\": " + std::to_string(splice_resets_);
    s += ", \"sdr_connected\": " + std::string(sdr_->connected() ? "true" : "false");
    s += ", \"sdr_reconnects\": " + std::to_string(sdr_->reconnects());
    s += ", \"hw_ovf\": " + std::to_string(sdr_->hw_ovf());
    if (sdr_->hw_counter()) {
        s += ", \"hw_dropped\": " + std::to_string(sdr_->hw_dropped_samples());
        s += ", \"hw_ovf_events\": " + std::to_string(sdr_->hw_ovf_events());
    }
    s += ", \"gain_db\": " + FormatDouble(sdr_->gain_db());
    s += ", \"rx_level_dbfs\": " + FormatDouble(sdr_->level_dbfs());
    s += ", \"rx_rate\": " + FormatDouble(rx_rate_.load());
    s += ", \"rx_slow\": " + std::to_string(rx_slow_.load());
    s += ", \"pace_starved\": " + std::to_string(pace_starved_.load());
    s += ", \"windows\": " + std::to_string(windows_done_);
    s += ", \"windows_failed\": " + std::to_string(windows_failed_);
    s += ", \"stream_clients\": " + std::to_string(clients_.size());
    s += ", \"bitrate\": " + FormatDouble(bitrate_);
    s += "}";
    return s;
}

bool LiveDemod::locked() {
    std::lock_guard<std::mutex> lock(mu_);
    return locked_;
}

double LiveDemod::signal_db() {
    std::lock_guard<std::mutex> lock(mu_);
    return mer_db_;
}

void LiveDemod::OnBlock(const cfloat* p, size_t n) {
    if (skip_samples_ > 0) {        // 選局前に SDR 側で取り込まれていた分は捨てる
        skip_samples_ -= static_cast<long long>(n);
        return;
    }
    // 実測レート監視（このコールバックは SDR 受信スレッド 1 本からだけ呼ばれる）
    const double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (rate_mon_.Feed(static_cast<long long>(n), t)) {
        rx_rate_ = rate_mon_.rate();
        rx_slow_ = rate_mon_.slow();
        if (rate_mon_.last_slow()) {
            std::fprintf(stderr, "sdr: rx rate %.0f Hz < %.0f Hz: USB 帯域不足かサンプル落ち\n",
                         rate_mon_.rate(), rate_mon_.fs());
        }
    }
    if (dump_ && dump_written_ < kDumpMaxSamples) {
        const size_t w = std::min(n, static_cast<size_t>(kDumpMaxSamples - dump_written_));
        std::fwrite(p, sizeof(cfloat), w, dump_);
        dump_written_ += static_cast<long long>(w);
        if (dump_written_ >= kDumpMaxSamples) {
            std::fclose(dump_);
            dump_ = nullptr;
            std::fprintf(stderr, "dump-iq: %lld samples written\n", dump_written_);
        }
    }
    std::lock_guard<std::mutex> lock(iq_mu_);
    if (iq_total_ > kIqBoundSamples) {      // 復調が追いつかない: 全部捨てて仕切り直し
        iq_q_.clear();
        iq_total_ = 0;
        overflowed_ = true;
    }
    iq_q_.emplace_back(p, p + n);
    iq_total_ += static_cast<long long>(n);
    iq_cv_.notify_one();
}

// FPGA 等化モードの 1 シンボル（SDR 受信スレッドから）
void LiveDemod::OnSymbol(const EqSymbol& s) {
    const double t = std::chrono::duration<double>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    // FPGA のシンボルは公称レートの 1 シンボル分の入力に相当する（レート監視は入力サンプル単位）
    if (rate_mon_.Feed(static_cast<long long>(std::llround(SymbolSamples(mode_, gi_) * in_per_nominal_)), t)) {
        rx_rate_ = rate_mon_.rate();
        rx_slow_ = rate_mon_.slow();
    }
    // ロック前の暫定 MER: 等化済みなので TMCC / AC1 キャリア（DBPSK、±4/3 の実軸。フレーム位置に依らない）の
    // 誤差から 64 シンボルごとに出す。TVTest のチャンネルスキャンは選局 5 秒後の信号レベルが 7 dB 未満だと
    // 「信号なし」にするが、GI 相関の暫定値（8〜13 dB）は際どく、フレーム同期（約 5 秒）を待つと間に合わない
    {
        bool need = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            need = !locked_;
        }
        if (need) {
            int n_t = 0, n_a = 0;
            const int16_t* pt = TmccPositionsAbs(mode_, &n_t);
            const int16_t* pa = Ac1PositionsAbs(mode_, &n_a);
            const uint8_t* raw = s.raw.data();
            auto acc = [&](const int16_t* pos, int n) {
                for (int i = 0; i < n; ++i) {
                    const int k = pos[i];
                    const uint8_t c = raw[k * 3 + 2];
                    if (c == 0) continue;                       // CSI 0 = 消失
                    const double re = static_cast<int8_t>(raw[k * 3]) / 64.0;
                    const double im = static_cast<int8_t>(raw[k * 3 + 1]) / 64.0;
                    const double ref = (re >= 0.0) ? kPilotAmplitude : -kPilotAmplitude;
                    qm_err_ += (re - ref) * (re - ref) + im * im;
                    qm_cnt_ += 1.0;
                }
            };
            acc(pt, n_t);
            acc(pa, n_a);
            if (++qm_syms_ >= 64) {
                if (qm_cnt_ > 0.0) {
                    const double mer = 10.0 * std::log10(kPilotAmplitude * kPilotAmplitude / (qm_err_ / qm_cnt_ + 1e-9));
                    std::lock_guard<std::mutex> lock(mu_);
                    if (!locked_) mer_db_ = std::max(0.0, std::min(40.0, mer));
                }
                qm_err_ = qm_cnt_ = 0.0;
                qm_syms_ = 0;
            }
        }
    }
    std::lock_guard<std::mutex> lock(iq_mu_);
    if (sym_q_.size() > kSymBound) {        // 復調が追いつかない: 全部捨てて仕切り直し
        sym_q_.clear();
        overflowed_ = true;
    }
    sym_q_.push_back(s);
    sym_mode_ = true;
    iq_cv_.notify_one();
}

// SDR の切断/再接続（sdr_source の自動再接続）。受信スレッドから呼ばれる。
// 切断をまたいだサンプルは不連続なので、オーバーフロー時と同じ経路で
// パイプラインを仕切り直す（SchedLoop が ResetSplice する。underruns にも数えられるが、
// 切断起因かどうかは status の sdr_reconnects で区別できる）。
void LiveDemod::OnSdrEvent(bool up) {
    if (up) {
        rate_mon_.Reset();      // 無受信期間をレート不足と誤計上しない
        return;
    }
    {
        std::lock_guard<std::mutex> lock(iq_mu_);
        iq_q_.clear();
        iq_total_ = 0;
        sym_q_.clear();
        overflowed_ = true;
    }
    iq_cv_.notify_all();
    std::lock_guard<std::mutex> lock(mu_);
    locked_ = false;
    mer_db_ = 0.0;              // GetSignalLevel が切断を 0 dB で示せるように
}

void LiveDemod::SchedLoop() {
    // 選局直後の最初のウィンドウだけ新規フレーム数を小さくして初回ロックを早める
    // （16 フレームだと I/Q 待ち 4.8 s + 復調で TS まで 13 s。TVTest のチャンネルスキャンは
    // 1 チャンネル約 9 秒で次へ進むので間に合わなかった。6 フレームなら 6 秒前後）。
    // 定常のウィンドウは new_frames_ のまま（CPU 効率はそちらで決まる）。
    const int first_new = std::min(kFirstWindowNewFrames, new_frames_);
    const int first_ctx = std::min(kFirstWindowCtxFrames, ctx_frames_);
    auto win_samples_for = [&](int nf, int ctx) {
        const long long syms = static_cast<long long>(ctx + nf + 1) * kSymbolsPerFrame;
        return static_cast<long long>(std::llround(static_cast<double>(syms) * SymbolSamples(mode_, gi_) *
                                                   in_per_nominal_)) + (1 << 16);
    };
    auto adv_samples_for = [&](int nf) {
        return static_cast<long long>(std::llround(static_cast<double>(nf) * kSymbolsPerFrame *
                                                   SymbolSamples(mode_, gi_) * in_per_nominal_));
    };
    int cur_new = first_new;
    // 入力レートでの FFT 長・GI 長・シンボル長（暫定信号レベルの GI 相関用）
    const int N_in = static_cast<int>(std::lround(ModeParamsOf(mode_).fft_size * in_per_nominal_));
    const int Ng_in = static_cast<int>(std::lround(GuardSamples(mode_, gi_) * in_per_nominal_));
    const int T_in = N_in + Ng_in;
    std::vector<cfloat> acc;
    long long acc_abs0 = 0, abs_in = 0;
    long long cur_gen = 0;
    // XO 補正を書いた後、この入力サンプル番号より前に始まるウィンドウの clock_ppm は再提案に使わない
    long long xo_valid_from = 0;
    constexpr double kXoSettleSec = 0.5;    // 書き込みが取り込みに反映されるまでの余裕（USB バッファ 2〜4 ブロック分）
    std::deque<WindowJob> jobs;
    // FPGA 等化モード: シンボル単位のウィンドウ（連番が途切れたら組み直す）
    std::vector<EqSymbol> acc_sym;
    uint32_t last_seq = 0;
    bool have_seq = false;
    // FPGA 等化モードのウィンドウ: 文脈 kSymCtxFrames（時間デインターリーブ履歴 380 シンボル + 先頭助走 102 が
    // フレーム位置によらず収まる 3 フレーム）+ 新規 nf + 1 フレーム + 末尾助走。復号は新規 nf フレームだけ
    //（DemodulateEqualized の範囲復号）なので、重なり分を復号し直さない
    constexpr int kSymCtxFrames = 3;
    auto win_syms_for = [&](int nf, int /*ctx*/) {
        return static_cast<size_t>(kSymCtxFrames + nf + 1) * kSymbolsPerFrame + kFecTailSymbols + 8;
    };
    // 仕掛かり数は現世代のジョブだけ数える。選局前のジョブ（1 本 4〜5 秒）を待っていると
    // 次のチャンネルの最初のウィンドウが遅れ、信号のあるチャンネルが続くとスキャンが交互に落ちる
    auto active_jobs = [&]() {
        int n = 0;
        for (const WindowJob& j : jobs) n += (j.gen == cur_gen);
        return n;
    };

    while (!stop_) {
        // I/Q の取り込み（溢れフラグが立っていたらパイプラインを仕切り直す）
        {
            std::unique_lock<std::mutex> lock(iq_mu_);
            iq_cv_.wait_for(lock, std::chrono::milliseconds(200),
                            [&] { return !iq_q_.empty() || !sym_q_.empty() || stop_; });
            if (overflowed_) {
                overflowed_ = false;
                acc.clear();
                acc_sym.clear();
                have_seq = false;
                cur_new = first_new;
                lock.unlock();
                ResetSplice(&cur_gen);
                lock.lock();
            }
            while (!sym_q_.empty()) {
                EqSymbol& s = sym_q_.front();
                if (have_seq && s.seq != last_seq + 1) {
                    // 窓の欠け（FPGA の同期ミス・USB 取りこぼし）: 継ぎ目をまたぐウィンドウは作れないので
                    // 溜めた分を捨てて組み直す。継ぎ位置は絶対サンプル番号（start）で決まるので Emit の
                    // アンカーはそのまま（弱い局で窓が頻繁に欠けても underruns を膨らませない）
                    acc_sym.clear();
                    ++sym_gaps_;
                }
                last_seq = s.seq;
                have_seq = true;
                acc_sym.push_back(std::move(s));
                sym_q_.pop_front();
            }
            while (!iq_q_.empty()) {
                auto& blk = iq_q_.front();
                abs_in += static_cast<long long>(blk.size());
                iq_total_ -= static_cast<long long>(blk.size());
                // XO 補正直後の過渡（PLL の引き込み + USB で流れてくる補正前の残り）はウィンドウに入れない。
                // 入れると補正後の最初のウィンドウが sync failed になり、ロックが 1 ウィンドウ分遅れた（2026-10-16）
                if (abs_in > xo_valid_from) {
                    if (acc.empty()) acc_abs0 = abs_in - static_cast<long long>(blk.size());
                    acc.insert(acc.end(), blk.begin(), blk.end());
                }
                iq_q_.pop_front();
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (gen_ != cur_gen) {          // tune された
                cur_gen = gen_;
                acc.clear();
                acc_sym.clear();
                have_seq = false;
                cur_new = first_new;
                anchored_ = false;
                last_emitted_ = -1;
            }
        }
        // 暫定信号レベル（GI 相関）: 選局後 14 シンボル分（約 0.13 s）の I/Q が溜まったら一度だけ
        if (quick_level_pending_ && acc.size() >= static_cast<size_t>(14 * T_in)) {
            quick_level_pending_ = false;
            const double rho = QuickGiRho(acc.data(), acc.size(), N_in, Ng_in);
            double est = 0.0;
            if (rho > 0.25 && rho < 0.999) est = std::max(0.0, std::min(30.0, 10.0 * std::log10(rho / (1.0 - rho))));
            std::lock_guard<std::mutex> lock(mu_);
            if (!locked_) mer_db_ = est;
            if (verbose_) std::fprintf(stderr, "quick level: GI rho %.3f -> %.1f dB\n", rho, est);
        }
        if (static_cast<long long>(acc.size()) > kIqBoundSamples) {
            // 復調が恒常的に追いつかない: 組み立てバッファごと仕切り直す
            acc.clear();
            cur_new = first_new;
            ResetSplice(&cur_gen);
        }
        // FPGA 等化モードのウィンドウ投入（等化済みシンボル → デインターリーブ以降だけ）
        if (sym_mode_ && !acc_sym.empty()) {
            std::lock_guard<std::mutex> lock(mu_);
            if (!locked_ && mer_db_ == 0.0) mer_db_ = 10.0;     // 暫定レベル（FPGA が同期しているので信号はある）
        }
        auto sym_new = [&]() { return (cur_new == first_new) ? std::min(kFirstWindowNewFramesSym, cur_new) : cur_new; };
        while (sym_mode_ && acc_sym.size() >= win_syms_for(sym_new(), 0) && active_jobs() < workers_) {
            const int nf = sym_new();
            const size_t win_syms = win_syms_for(nf, 0);
            const int fec_frames = nf;
            const size_t adv_syms = static_cast<size_t>(nf) * kSymbolsPerFrame;
            cur_new = new_frames_;
            const int total = ModeParamsOf(mode_).total_carriers;
            // {I8, Q8, CSI8} を 32bit に詰めたまま後段へ（cfloat/float に広げない）
            auto P = std::make_shared<Mat<uint32_t>>(win_syms, static_cast<size_t>(total));
            for (size_t r = 0; r < win_syms; ++r) {
                const uint8_t* raw = acc_sym[r].raw.data();
                uint32_t* d = (*P)[r];
                for (int k = 0; k < total; ++k) {
                    d[k] = static_cast<uint32_t>(raw[k * 3]) | (static_cast<uint32_t>(raw[k * 3 + 1]) << 8) |
                           (static_cast<uint32_t>(raw[k * 3 + 2]) << 16);
                }
            }
            WindowJob job;
            job.gen = cur_gen;
            // FPGA のサンプルカウンタは公称レート。Emit は入力サンプル単位で継ぐので換算しておく
            job.abs0 = static_cast<long long>(std::llround(static_cast<double>(acc_sym[0].start) * in_per_nominal_));
            job.fut = std::async(std::launch::async, [this, P, fec_frames]() -> std::shared_ptr<DemodResult> {
                try {
                    // 受信機の DC（LO リーク）は中央キャリア − δ に乗る（キャリア順に並べ直した後）
                    const int dc_k = ModeParamsOf(mode_).total_carriers / 2 - sdr_->fpga_delta();
                    return std::make_shared<DemodResult>(DemodulateEqualizedPacked(P.get(), mode_, fec_frames, dc_k));
                } catch (const std::exception&) {
                    return nullptr;
                }
            });
            jobs.push_back(std::move(job));
            acc_sym.erase(acc_sym.begin(), acc_sym.begin() + static_cast<std::ptrdiff_t>(adv_syms));
        }
        // ウィンドウの投入（仕掛かりは workers_ まで）
        while (!sym_mode_ && static_cast<long long>(acc.size()) >=
                   win_samples_for(cur_new, cur_new == first_new ? first_ctx : ctx_frames_) &&
               active_jobs() < workers_) {
            const bool first = (cur_new == first_new);
            const long long win_samples = win_samples_for(cur_new, first ? first_ctx : ctx_frames_);
            // 最初のウィンドウは文脈が短いぶん進みを控えめにし、次のウィンドウ（文脈 ctx_frames_）が
            // 最初のウィンドウの排出フレームに続くようにする
            const long long adv_samples =
                adv_samples_for(first ? std::max(1, cur_new - (ctx_frames_ - first_ctx)) : cur_new);
            cur_new = new_frames_;
            WindowJob job;
            job.gen = cur_gen;
            job.abs0 = acc_abs0;
            std::vector<cfloat> win(acc.begin(), acc.begin() + win_samples);
            // 文脈フレームは前のウィンドウが排出済み（範囲復号は末尾フレームまで出す）なので、1 フレームの
            // 余裕を残して FEC を省く。前段と時間デインターリーブの履歴はその前から取られる
            const int first_fec = std::max(0, (first ? first_ctx : ctx_frames_) - 1);
            job.fut = std::async(std::launch::async, [this, w = std::move(win), first_fec]() mutable
                                 -> std::shared_ptr<DemodResult> {
                try {
                    auto res = std::make_shared<DemodResult>(
                        Demodulate(std::move(w), cfg_.fs, mode_, gi_, kDefaultBlockFrames, /*fast_fec=*/true, first_fec,
                                   /*null_dc=*/true));
                    return res;
                } catch (const std::exception&) {
                    return nullptr;         // 同期失敗（無信号など）
                }
            });
            jobs.push_back(std::move(job));
            acc.erase(acc.begin(), acc.begin() + adv_samples);
            acc_abs0 += adv_samples;
        }
        // 結果の回収（現世代は先頭から順に。未完ならブロックせず次のループへ。
        // 選局前の世代のジョブは終わったものから捨て、未完でも後ろの現世代を妨げない）
        for (auto it = jobs.begin(); it != jobs.end();) {
            const bool ready = it->fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
            if (it->gen != cur_gen) {
                if (ready) {
                    it->fut.get();
                    it = jobs.erase(it);
                } else {
                    ++it;
                }
                continue;
            }
            if (!ready) break;
            WindowJob job = std::move(*it);
            it = jobs.erase(it);
            std::shared_ptr<DemodResult> res = job.fut.get();
            ++windows_done_;
            if (!res) {
                ++windows_failed_;
                std::lock_guard<std::mutex> lock(mu_);
                locked_ = false;
                if (verbose_) std::fprintf(stderr, "window @%lld: sync failed\n", job.abs0);
                continue;
            }
            // raw 経路: 同期で測ったサンプルクロック偏差を SDR に伝える（XO 補正）。
            // 書き込みで LO とサンプルレートが変わるので、仕掛かり中のウィンドウと蓄積済みの I/Q（補正前）は捨てて
            // 選局直後と同じ形で組み直す。補正前のサンプルで測った同じ ppm を次のウィンドウがもう一度報告して
            // 重ね書き（−16 → −32 → −48 ppm）になっていたので（純正 16bit ファームの TVTest 起動直後にドロップ多発、
            // 2026-10-16）、書き込み後 kXoSettleSec 以降に始まるウィンドウだけが次の値を提案できる
            if (!sdr_->fpga_eq() && job.abs0 >= xo_valid_from && sdr_->SuggestClockPpm(res->sync.clock_ppm)) {
                if (verbose_) std::fprintf(stderr, "window @%lld: clock %+.2f ppm -> xo_correction applied; resyncing\n",
                                           job.abs0, res->sync.clock_ppm);
                Emit(*res, job.abs0);           // このウィンドウは補正前のサンプルで一貫しているので出す
                xo_valid_from = abs_in + static_cast<long long>(std::llround(cfg_.fs * kXoSettleSec));
                acc.clear();
                cur_new = first_new;
                ResetSplice(&cur_gen, /*count_underrun=*/false);
                break;                          // 旧世代の仕掛かりは次のループで捨てられる
            }
            Emit(*res, job.abs0);
        }
    }
}

void LiveDemod::ResetSplice(long long* cur_gen, bool count_underrun) {
    std::lock_guard<std::mutex> lock(mu_);
    if (count_underrun) ++underruns_;
    ++gen_;                 // 仕掛かり結果は無効化
    *cur_gen = gen_;
    anchored_ = false;
    last_emitted_ = -1;     // 継ぎ目はフレーム番号が飛ぶ（frames_lost に出る）
}

void LiveDemod::Emit(const DemodResult& res, long long abs0) {
    const long long T = SymbolSamples(mode_, gi_);
    const int f0 = res.fsi.frame_start;
    int reliable_from = 0;
    for (const RxLayer& lyr : res.fsi.layers) {
        auto it = res.layers.find(lyr.name);
        if (it == res.layers.end()) continue;
        reliable_from = std::max(reliable_from, it->second.first_frame + 1);
    }
    std::vector<std::vector<uint8_t>> frames_out;
    long long first_F = -1, last_F = -1;
    long long uncorrectable = 0;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // 最終フレームは排出しない: バイトデインターリーブの補遅延（11 TSP）の尻尾が
        // 未完成で、各呼び出しの末尾 11 TSP が必ず訂正不能になる。重複分（ctx_frames_）が
        // あるので次のウィンドウが完全な形で出し直す。
        int f_begin = reliable_from, f_end = res.fsi.n_frames - 1;
        if (res.emit_first >= 0) {          // 範囲復号: [emit_first, emit_end) は全層とも完全
            f_begin = res.emit_first;
            f_end = res.emit_end;
        }
        for (int f = f_begin; f < f_end; ++f) {
            // gi_start0・シンボル番号は公称タイムライン、abs0 は入力サンプル → 入力単位に換算
            const double s_f = abs0 + (res.sync.gi_start0 +
                               static_cast<double>(f0 + f * kSymbolsPerFrame) * T) * in_per_nominal_;
            long long F;
            if (!anchored_) {
                anchor_t_ = s_f;
                anchor_f_ = last_emitted_ + 1;
                anchored_ = true;
                F = anchor_f_;
            } else {
                F = anchor_f_ + llround((s_f - anchor_t_) / frame_samples_);
                // 安全弁: 継ぎ位置が大きく戻る／飛ぶ（サンプル番号の周回など）ときは、黙って捨て続けずに
                // 継ぎ目を仕切り直す（frames_lost に 1 つ分だけ出る）
                if (F < last_emitted_ - 100 || F > last_emitted_ + 100000) {
                    ++splice_resets_;
                    anchor_t_ = s_f;
                    anchor_f_ = last_emitted_ + 2;
                    F = anchor_f_;
                }
            }
            if (F <= last_emitted_) continue;
            if (first_F < 0) first_F = F;
            // 1 フレーム分を階層順（A, B, C）に連結
            std::vector<uint8_t> out;
            for (const RxLayer& lyr : res.fsi.layers) {
                auto it = res.layers.find(lyr.name);
                if (it == res.layers.end()) continue;
                const LayerResult& lr = it->second;
                const int tpf = TspPerFrame(mode_, lyr.mod, lyr.rate,
                                            static_cast<int>(lyr.segments.size()));
                const long long i = f - lr.first_frame;
                const size_t a = static_cast<size_t>(i) * tpf * kTsPacket;
                const size_t b = a + static_cast<size_t>(tpf) * kTsPacket;
                if (i < 0 || b > lr.ts.size()) continue;
                const size_t base = out.size();
                out.insert(out.end(), lr.ts.begin() + a, lr.ts.begin() + b);
                for (int t = 0; t < tpf; ++t) {
                    if (lr.rs_err[static_cast<size_t>(i) * tpf + t] < 0) {
                        out[base + static_cast<size_t>(t) * kTsPacket + 1] |= kTeiMask;
                        ++uncorrectable;
                    }
                }
            }
            if (out.empty()) continue;
            // アンカーを更新して丸め誤差の蓄積を防ぐ
            anchor_t_ = s_f;
            anchor_f_ = F;
            if (last_emitted_ >= 0 && F > last_emitted_ + 1) frames_lost_ += F - last_emitted_ - 1;
            last_emitted_ = F;
            last_F = F;
            ts_packets_ += out.size() / kTsPacket;
            frames_out.push_back(std::move(out));
        }
        rs_uncorrectable_ += uncorrectable;
        locked_ = res.fsi.tmcc_parity_ok;
        // 状態表示用の階層情報・MER・ビットレート
        layers_.clear();
        long long tsp_pf_total = 0;
        double mer = 0.0;
        for (const RxLayer& lyr : res.fsi.layers) {
            layers_.push_back({lyr.name, ModulationName(lyr.mod), lyr.rate.str(), lyr.I,
                               static_cast<int>(lyr.segments.size())});
            tsp_pf_total += TspPerFrame(mode_, lyr.mod, lyr.rate,
                                        static_cast<int>(lyr.segments.size()));
            auto it = res.layers.find(lyr.name);
            if (it != res.layers.end() && it->second.mer_db > 0.0) {
                mer = mer == 0.0 ? it->second.mer_db : std::min(mer, it->second.mer_db);
            }
        }
        mer_db_ = mer;
        bitrate_ = tsp_pf_total * kTsPacket * 8 / (frame_samples_ / cfg_.fs);
    }
    if (verbose_) {
        std::string per;
        for (const RxLayer& lyr : res.fsi.layers) {
            auto it = res.layers.find(lyr.name);
            if (it == res.layers.end()) continue;
            long long bad = 0;
            for (int e : it->second.rs_err) bad += e < 0;
            char buf[64];
            std::snprintf(buf, sizeof(buf), " %s:%.1fdB/%lld", lyr.name.c_str(),
                          it->second.mer_db, bad);
            per += buf;
        }
        std::fprintf(stderr, "window @%lld: frames %lld..%lld, uncorrectable %lld,%s\n",
                     abs0, first_F, last_F, uncorrectable, per.c_str());
    }
    // ペーサーへ渡す（約 3.7 秒ごとのバースト排出を、フレーム周期で平滑に送り出す）
    {
        std::lock_guard<std::mutex> lock(pace_mu_);
        for (auto& chunk : frames_out) pace_q_.push_back(std::move(chunk));
    }
    pace_cv_.notify_one();
}

// GI 相関の正規化ピーク ρ = max_n |Σ_i conj(x[n+i]) x[n+i+N]| / 平均電力（K シンボル分を足し合わせ）。
// OFDM があれば ρ ≈ C/(C+N)、無信号なら ≈ 0。スライド和で O(T) / シンボル
double LiveDemod::QuickGiRho(const cfloat* x, size_t n, int N, int Ng) {
    const int T = N + Ng;
    const long long K = (static_cast<long long>(n) - N - Ng) / T;
    if (K < 2) return 0.0;
    std::vector<double> mag(static_cast<size_t>(T), 0.0);
    for (long long k = 0; k < K; ++k) {
        const cfloat* b = x + k * T;
        std::complex<double> s = 0.0;
        double p = 0.0;
        for (int i = 0; i < Ng; ++i) {
            s += std::conj(std::complex<double>(b[i])) * std::complex<double>(b[i + N]);
            p += std::norm(b[i]) + std::norm(b[i + N]);
        }
        for (int m = 0; m < T; ++m) {
            mag[static_cast<size_t>(m)] += std::abs(s) / (0.5 * p + 1e-12);
            s -= std::conj(std::complex<double>(b[m])) * std::complex<double>(b[m + N]);
            s += std::conj(std::complex<double>(b[m + Ng])) * std::complex<double>(b[m + Ng + N]);
            p -= std::norm(b[m]) + std::norm(b[m + N]);
            p += std::norm(b[m + Ng]) + std::norm(b[m + Ng + N]);
        }
    }
    return *std::max_element(mag.begin(), mag.end()) / static_cast<double>(K);
}

// 最初のバースト用: PSI/SI（PID < 0x30）と、PAT に載っている PMT だけを残す。
// 映像・音声の PES が無ければ TVTest は再生を始めないので、連続分が揃うまで映像が止まって見えない。
std::vector<uint8_t> LiveDemod::FilterPsi(const std::vector<uint8_t>& chunk) {
    std::vector<uint8_t> out;
    out.reserve(chunk.size() / 8);
    for (size_t i = 0; i + kTsPacket <= chunk.size(); i += kTsPacket) {
        const uint8_t* p = chunk.data() + i;
        if (p[0] != 0x47) continue;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (pid == 0 && (p[1] & 0x40) && (p[3] & 0x10)) {
            // PAT: program_number ≠ 0 のエントリの PMT PID を集める（セクションは 1 パケットに収まる前提）
            size_t off = 4;
            if (p[3] & 0x20) off += 1 + p[4];           // adaptation field
            if (off < kTsPacket) {
                off += 1 + p[off];                      // pointer_field
                if (off + 8 <= kTsPacket && p[off] == 0x00) {
                    const size_t sec_len = ((p[off + 1] & 0x0F) << 8) | p[off + 2];
                    const size_t end = std::min<size_t>(off + 3 + sec_len - 4, kTsPacket);
                    for (size_t e = off + 8; e + 4 <= end; e += 4) {
                        const int prog = (p[e] << 8) | p[e + 1];
                        const int pmt = ((p[e + 2] & 0x1F) << 8) | p[e + 3];
                        if (prog != 0 && std::find(pmt_pids_.begin(), pmt_pids_.end(), pmt) == pmt_pids_.end()) {
                            pmt_pids_.push_back(pmt);
                        }
                    }
                }
            }
        }
        const bool keep = pid < 0x30 || std::find(pmt_pids_.begin(), pmt_pids_.end(), pid) != pmt_pids_.end();
        if (keep) out.insert(out.end(), p, p + kTsPacket);
    }
    // 連続分が揃うまでの間、同じ PSI/SI を繰り返し送るために取っておく（上限 4000 パケット ≈ 0.75 MB）
    if (psi_cache_.size() < 4000u * kTsPacket) psi_cache_.insert(psi_cache_.end(), out.begin(), out.end());
    return out;
}

bool LiveDemod::IsPsiPid(int pid) const {
    return pid < 0x30 || std::find(pmt_pids_.begin(), pmt_pids_.end(), pid) != pmt_pids_.end();
}

// PSI/SI の PID の continuity_counter を出力側の連番に振り直す。最初のバースト → 繰り返し → 本流と
// 出どころが変わっても CC がつながり、TVTest にドロップとして数えられない（映像・音声は触らない）
void LiveDemod::RemapPsiCc(std::vector<uint8_t>* chunk) {
    if (psi_cc_.size() != 8192) {
        psi_cc_.assign(8192, 0);
        psi_cc_tracked_.assign(8192, 0);
    }
    for (size_t i = 0; i + kTsPacket <= chunk->size(); i += kTsPacket) {
        uint8_t* p = chunk->data() + i;
        const int pid = ((p[1] & 0x1F) << 8) | p[2];
        if (!IsPsiPid(pid) || !(p[3] & 0x10)) continue;      // ペイロードがあるときだけ CC が進む
        const size_t k = static_cast<size_t>(pid);
        if (!psi_cc_tracked_[k]) {
            psi_cc_tracked_[k] = 1;
            psi_cc_[k] = static_cast<uint8_t>(p[3] & 0x0F);  // 初出はそのまま
            continue;
        }
        psi_cc_[k] = static_cast<uint8_t>((psi_cc_[k] + 1) & 0x0F);
        p[3] = static_cast<uint8_t>((p[3] & 0xF0) | psi_cc_[k]);
    }
}

// psi_cache_ のコピーを、CC を振り直して返す
std::vector<uint8_t> LiveDemod::RepeatPsi() {
    std::vector<uint8_t> out(psi_cache_);
    RemapPsiCc(&out);
    return out;
}

void LiveDemod::SendToClients(const std::vector<uint8_t>& chunk) {
    std::vector<std::shared_ptr<ClientQueue>> clients;
    {
        std::lock_guard<std::mutex> lock(mu_);
        clients = clients_;
    }
    for (auto& q : clients) {
        std::lock_guard<std::mutex> ql(q->mu);
        if (q->chunks.size() >= kQueueMaxChunks) {
            dropped_ += chunk.size() / kTsPacket;
        } else {
            q->chunks.push_back(chunk);
            q->cv.notify_one();
        }
    }
}

// ペーサー: 1 フレーム（≈231 ms）周期でクライアントへ送出する。
// 復調は平均で実時間なのでキューは「復調レイテンシ + 1 ウィンドウ」程度で振動する。
// 大きく溜まったら（瞬断後の再同期など）追いつくまで間隔なしで流す。
void LiveDemod::PaceLoop() {
    const auto frame_dt = std::chrono::microseconds(
        static_cast<long long>(frame_samples_ / cfg_.fs * 1e6));
    auto t_next = std::chrono::steady_clock::now();
    // 再開の余裕: 選局直後は最初の短いウィンドウの数フレームを出した後、次のウィンドウまで数秒空く。
    // 空になるたびに次の 1 本だけで再開すると、以降も毎回ウィンドウの到着ぎりぎりで追いかけて
    // かくつくので、一度出した後に空になったら kResumeBacklog フレーム以上溜まる（か上限時間が
    // 過ぎる）まで待ってから再開する（最初の 1 本は TVTest のスキャン検出のため即座に出す）。
    // 選局後最初の空き（最初の短いウィンドウの直後）は 2 ウィンドウ分、それ以降の空き（復調が遅れた）は
    // 数フレーム分だけ溜め直す
    const size_t kResumeBacklogFirst = static_cast<size_t>(new_frames_ + 4);
    const size_t kResumeBacklogLater = 4;
    constexpr auto kResumeMaxWait = std::chrono::seconds(6);
    bool emitted = false, starved = false, holding = false, first_hold_done = false;
    std::chrono::steady_clock::time_point hold_since{}, psi_last{};
    constexpr auto kPsiRepeatInterval = std::chrono::milliseconds(250);
    while (!stop_) {
        std::vector<uint8_t> chunk;
        size_t backlog = 0;
        {
            std::unique_lock<std::mutex> lock(pace_mu_);
            if (pace_reset_.exchange(false)) {
                emitted = starved = holding = first_hold_done = false;
                pmt_pids_.clear();
                psi_cache_.clear();
                psi_cc_.clear();            // 次の RemapPsiCc で初期化し直す
            }
            if (!pace_cv_.wait_for(lock, std::chrono::milliseconds(200),
                                   [&] { return !pace_q_.empty() || stop_; })) {
                t_next = std::chrono::steady_clock::now();      // 空走中は基準を進めない
                if (emitted && !starved) {
                    starved = true;
                    if (first_hold_done) ++pace_starved_;        // 定常中に空になった回数（status の pace_starved）
                }
                // キューが空の間も PSI/SI の繰り返し送出は続ける（下の待ち分岐と同じ条件）
                if (starved && !first_hold_done && first_burst_ == FirstBurst::kPsi && !psi_cache_.empty() &&
                    t_next - psi_last >= kPsiRepeatInterval) {
                    psi_last = t_next;
                    lock.unlock();
                    SendToClients(RepeatPsi());
                }
                continue;
            }
            if (stop_) break;
            if (!emitted && first_burst_ == FirstBurst::kNone) {
                emitted = starved = true;       // 最初から「連続分が揃うまで待つ」扱い
            }
            if (!first_hold_done && !starved && pace_q_.size() >= kResumeBacklogFirst) {
                first_hold_done = true;         // 空く前に十分溜まった（最初のバースト扱いを終える）
            }
            if (starved) {
                const auto now = std::chrono::steady_clock::now();
                if (!holding) {
                    holding = true;
                    hold_since = now;
                }
                const size_t need = first_hold_done ? kResumeBacklogLater : kResumeBacklogFirst;
                if (pace_q_.size() < need && now - hold_since < kResumeMaxWait) {
                    lock.unlock();
                    // 連続分が揃うまでの待ちの間、最初のバーストの PSI/SI を繰り返し送る。TVTest の
                    // チャンネルスキャンは待ち時間経過時点の受信状態を見るので、途切れるとその時点で
                    // 「信号なし」と判定されて検出されない（2026-10-14 の実測）。
                    if (!first_hold_done && first_burst_ == FirstBurst::kPsi && !psi_cache_.empty() &&
                        now - psi_last >= kPsiRepeatInterval) {
                        psi_last = now;
                        SendToClients(RepeatPsi());
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                starved = holding = false;
                first_hold_done = true;
                t_next = now;
            }
            chunk = std::move(pace_q_.front());
            pace_q_.pop_front();
            backlog = pace_q_.size();
            emitted = true;
            // 最初のバースト（連続分が揃う前）は既定で PSI/SI だけ通す。PSI/SI の CC は出力側で振り直す
            if (first_burst_ == FirstBurst::kPsi) {
                if (!first_hold_done) chunk = FilterPsi(chunk);
                RemapPsiCc(&chunk);
            }
        }
        if (chunk.empty()) continue;
        SendToClients(chunk);
        if (backlog > kPaceCatchupFrames) continue;     // 溜まりすぎ: 間隔なしで追いつく
        t_next += frame_dt;
        const auto now = std::chrono::steady_clock::now();
        if (t_next > now) {
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
                t_next - now, std::chrono::milliseconds(300)));
        } else if (now - t_next > std::chrono::seconds(2)) {
            t_next = now;
        }
    }
}

}  // namespace isdbt
