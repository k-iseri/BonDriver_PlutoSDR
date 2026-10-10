// LiveDemod の実装。アルゴリズムの説明は live_demod.h 冒頭を参照。
#include "live_demod.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace isdbt {

LiveDemod::LiveDemod(SdrConfig cfg, Mode mode, Gi gi, int new_frames, int ctx_frames, int workers,
                     bool verbose, std::string dump_iq_path)
    : cfg_(std::move(cfg)), mode_(mode), gi_(gi),
      new_frames_(new_frames), ctx_frames_(ctx_frames), workers_(workers),
      in_per_nominal_(cfg_.fs / kSampleRateHz),     // 入力サンプル数 / 公称サンプル数
      frame_samples_(static_cast<double>(SymbolSamples(mode, gi)) * kSymbolsPerFrame *
                     (cfg_.fs / kSampleRateHz)),    // 1 フレームの入力サンプル数
      desc_("sdr:" + cfg_.uri), verbose_(verbose) {
    if (!dump_iq_path.empty()) dump_ = std::fopen(dump_iq_path.c_str(), "wb");
}

void LiveDemod::Start() {
    sdr_.Start(cfg_, [this](const cfloat* p, size_t n) { OnBlock(p, n); });
    sched_ = std::thread(&LiveDemod::SchedLoop, this);
    pacer_ = std::thread(&LiveDemod::PaceLoop, this);
}

void LiveDemod::Stop() {
    stop_ = true;
    iq_cv_.notify_all();
    pace_cv_.notify_all();
    sdr_.Stop();
    if (sched_.joinable()) sched_.join();
    if (pacer_.joinable()) pacer_.join();
}

std::shared_ptr<ClientQueue> LiveDemod::AddClient() {
    auto q = std::make_shared<ClientQueue>();
    std::lock_guard<std::mutex> lock(mu_);
    clients_.push_back(q);
    return q;
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
    sdr_.Retune(freq);
    {
        std::lock_guard<std::mutex> pl(pace_mu_);
        pace_q_.clear();
    }
    std::lock_guard<std::mutex> iql(iq_mu_);
    std::lock_guard<std::mutex> lock(mu_);
    ++gen_;                 // 仕掛かり中のウィンドウ結果は捨てる
    iq_q_.clear();
    has_channel_ = has_ch;
    channel_ = ch;
    freq_hz_ = freq;
    has_freq_ = true;
    locked_ = false;
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
    s += ", \"sdr_blocks\": " + std::to_string(sdr_.blocks());
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

void LiveDemod::SchedLoop() {
    const long long win_syms = static_cast<long long>(ctx_frames_ + new_frames_ + 1) * kSymbolsPerFrame;
    const long long win_samples = static_cast<long long>(
        std::llround(static_cast<double>(win_syms) * SymbolSamples(mode_, gi_) * in_per_nominal_)) +
        (1 << 16);
    const long long adv_samples = static_cast<long long>(std::llround(
        static_cast<double>(new_frames_) * kSymbolsPerFrame * SymbolSamples(mode_, gi_) *
        in_per_nominal_));
    std::vector<cfloat> acc;
    long long acc_abs0 = 0, abs_in = 0;
    long long cur_gen = 0;
    std::deque<WindowJob> jobs;

    while (!stop_) {
        // I/Q の取り込み（溢れフラグが立っていたらパイプラインを仕切り直す）
        {
            std::unique_lock<std::mutex> lock(iq_mu_);
            iq_cv_.wait_for(lock, std::chrono::milliseconds(200),
                            [&] { return !iq_q_.empty() || stop_; });
            if (overflowed_) {
                overflowed_ = false;
                acc.clear();
                lock.unlock();
                ResetSplice(&cur_gen);
                lock.lock();
            }
            while (!iq_q_.empty()) {
                auto& blk = iq_q_.front();
                abs_in += static_cast<long long>(blk.size());
                if (acc.empty()) acc_abs0 = abs_in - static_cast<long long>(blk.size());
                acc.insert(acc.end(), blk.begin(), blk.end());
                iq_total_ -= static_cast<long long>(blk.size());
                iq_q_.pop_front();
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (gen_ != cur_gen) {          // tune された
                cur_gen = gen_;
                acc.clear();
                anchored_ = false;
                last_emitted_ = -1;
            }
        }
        if (static_cast<long long>(acc.size()) > kIqBoundSamples) {
            // 復調が恒常的に追いつかない: 組み立てバッファごと仕切り直す
            acc.clear();
            ResetSplice(&cur_gen);
        }
        // ウィンドウの投入（仕掛かりは workers_ まで）
        while (static_cast<long long>(acc.size()) >= win_samples &&
               static_cast<int>(jobs.size()) < workers_) {
            WindowJob job;
            job.gen = cur_gen;
            job.abs0 = acc_abs0;
            std::vector<cfloat> win(acc.begin(), acc.begin() + win_samples);
            job.fut = std::async(std::launch::async, [this, w = std::move(win)]() mutable
                                 -> std::shared_ptr<DemodResult> {
                try {
                    auto res = std::make_shared<DemodResult>(
                        Demodulate(std::move(w), cfg_.fs, mode_, gi_));
                    return res;
                } catch (const std::exception&) {
                    return nullptr;         // 同期失敗（無信号など）
                }
            });
            jobs.push_back(std::move(job));
            acc.erase(acc.begin(), acc.begin() + adv_samples);
            acc_abs0 += adv_samples;
        }
        // 結果の回収（先頭から順に。未完ならブロックせず次のループへ）
        while (!jobs.empty() &&
               jobs.front().fut.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            WindowJob job = std::move(jobs.front());
            jobs.pop_front();
            std::shared_ptr<DemodResult> res = job.fut.get();
            if (job.gen != cur_gen) continue;
            ++windows_done_;
            if (!res) {
                ++windows_failed_;
                std::lock_guard<std::mutex> lock(mu_);
                locked_ = false;
                if (verbose_) std::fprintf(stderr, "window @%lld: sync failed\n", job.abs0);
                continue;
            }
            Emit(*res, job.abs0);
        }
    }
}

void LiveDemod::ResetSplice(long long* cur_gen) {
    std::lock_guard<std::mutex> lock(mu_);
    ++underruns_;
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
        for (int f = reliable_from; f < res.fsi.n_frames - 1; ++f) {
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

// ペーサー: 1 フレーム（≈231 ms）周期でクライアントへ送出する。
// 復調は平均で実時間なのでキューは「復調レイテンシ + 1 ウィンドウ」程度で振動する。
// 大きく溜まったら（瞬断後の再同期など）追いつくまで間隔なしで流す。
void LiveDemod::PaceLoop() {
    const auto frame_dt = std::chrono::microseconds(
        static_cast<long long>(frame_samples_ / cfg_.fs * 1e6));
    auto t_next = std::chrono::steady_clock::now();
    while (!stop_) {
        std::vector<uint8_t> chunk;
        size_t backlog = 0;
        {
            std::unique_lock<std::mutex> lock(pace_mu_);
            if (!pace_cv_.wait_for(lock, std::chrono::milliseconds(200),
                                   [&] { return !pace_q_.empty() || stop_; })) {
                t_next = std::chrono::steady_clock::now();      // 空走中は基準を進めない
                continue;
            }
            if (stop_) break;
            chunk = std::move(pace_q_.front());
            pace_q_.pop_front();
            backlog = pace_q_.size();
        }
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
