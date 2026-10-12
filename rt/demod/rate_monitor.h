// 受信サンプルレートの実測モニタ。USB 2.0 のサンプル落ちは libiio の API からは
// 直接見えない（DMA 溢れはホストに通知されない）ため、「一定時間窓で届いた
// サンプル数 ÷ 経過時間」を公称レートと比べて不足を検出する。
// 時刻は呼び出し側が渡す（実時間に依存しないので単体テストできる。rt_test 参照）。
#pragma once

namespace isdbt {

class RateMonitor {
public:
    // fs_hz: 期待サンプルレート。window_s: 集計窓。
    // tolerance: 実測がこの比率を下回った窓を「不足」と数える（0.99 = 1% 落ち）
    explicit RateMonitor(double fs_hz, double window_s = 10.0, double tolerance = 0.99)
        : fs_(fs_hz), window_(window_s), tol_(tolerance) {}

    // ブロック受信ごとに呼ぶ。t は単調増加の時刻 [s]、n はそのブロックのサンプル数。
    // 窓が満ちた（rate() / slow() / last_slow() が更新された）とき true を返す。
    // 最初の呼び出しは基準時刻の記録のみ（n は窓の開始前に届いた分なので数えない）。
    bool Feed(long long n, double t) {
        if (!started_) {
            t0_ = t;
            started_ = true;
            return false;
        }
        samples_ += n;
        if (t - t0_ < window_) return false;
        rate_ = static_cast<double>(samples_) / (t - t0_);
        last_slow_ = rate_ < fs_ * tol_;
        if (last_slow_) ++slow_;
        t0_ = t;
        samples_ = 0;
        return true;
    }

    // 窓を仕切り直す（SDR 再接続後など、無受信期間を不足と誤計上しないため）
    void Reset() {
        started_ = false;
        samples_ = 0;
    }

    double rate() const { return rate_; }        // 直近に閉じた窓の実測レート [Hz]（未計測は 0）
    long long slow() const { return slow_; }     // 不足と判定された窓の累計
    bool last_slow() const { return last_slow_; }
    double fs() const { return fs_; }

private:
    double fs_, window_, tol_;
    bool started_ = false;
    long long samples_ = 0;
    double t0_ = 0.0;
    double rate_ = 0.0;
    long long slow_ = 0;
    bool last_slow_ = false;
};

}  // namespace isdbt
