// ISDB-C 逐次復調器: 任意レートの複素ベースバンド I/Q を少しずつ与えると、64QAM の復調・RS・TSMF 分離
// を経た 188 バイト TS パケットをコールバックで返す。ref/isdbc/demod.py のブロック推定アルゴリズムを
// 逐次化したもの（docs/isdbc-spec-notes.md 4 章、設計判断は docs/decisions.md 2026-10-13）。
//
// 流れ（1 スレッドで実時間比 1 未満。5.27 Mbaud × 約 110 複素積和/シンボル）:
//   入力 → NCO（CFO 補正）→ [取得] PSD の帯域端で粗 CFO、Oerder–Meyr の直線フィットでタイミング、
//   z^4 の FFT で微 CFO → [追従] 入力レートの多相 RRC 補間でシンボル直接抽出（整合フィルタ兼用、
//   O&M でブロックごとにタイミング更新）→ AGC → T 間隔 FFE（判定指向 LS、チャンクごとに再解）
//   → 256 シンボルごとの判定指向位相 → 硬判定 → 差動復号 → ビット境界探索 → 204 バイト TSP →
//   FrameDecoder（デインターリーブ・RS・逆ランダマイズ）→ TsmfDemux → 出力。
// スペクトル反転は、ビット同期が取れないときにシンボルの共役で再試行して検出する。
#pragma once
#include <complex>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "framing.h"
#include "mapping.h"
#include "params.h"
#include "tsmf.h"

namespace isdbt { class Fft; }

namespace isdbc {

struct IsdbcConfig {
    double fs = 7'111'111.0;            // 入力サンプルレート
    double symbol_rate = kSymbolRateHz;
    int eq_taps = 25;                   // 奇数。T 間隔 FFE のタップ数
    TsmfDemux::Select select;           // TSMF の TS 選択
    bool allow_inversion = true;        // スペクトル反転の自動検出
};

struct IsdbcStatus {
    bool acquired = false;              // タイミング・CFO 取得済み（追従中）
    bool byte_sync = false;             // 204 バイト同期あり
    bool locked = false;                // byte_sync かつ MER > 15 dB
    bool inverted = false;
    double mer_db = 0.0;
    double cfo_hz = 0.0;                // NCO の補正量（受信信号の中心周波数 − LO）
    double clock_ppm = 0.0;             // 受信シンボルレートの公称からの偏差
    double level_dbfs = 0.0;            // 入力の平均電力
    long long ts_packets = 0;           // 出力したパケット数
    long long rs_packets = 0;           // RS 復号した TSP 数（TSMF 選別前）
    long long rs_uncorrectable = 0;
    long long sync_losses = 0;          // バイト同期を失った回数
    long long reacquisitions = 0;
    long long tsmf_frames = 0, tsmf_bad_headers = 0;
    int tsmf_selected = 0;
    struct Stream { int rel, tsid, onid; };
    std::vector<Stream> tsmf_streams;
};

class IsdbcDemod {
public:
    using PacketFn = std::function<void(const uint8_t* packets, size_t n_packets)>;

    IsdbcDemod(const IsdbcConfig& cfg, PacketFn on_packets);
    ~IsdbcDemod();
    IsdbcDemod(const IsdbcDemod&) = delete;
    IsdbcDemod& operator=(const IsdbcDemod&) = delete;

    // 入力サンプルを追加して処理できるところまで進める（任意長。呼び出しスレッドで処理する）
    void Feed(const cfloat* x, size_t n);
    // 選局などで信号が変わったとき: 取得からやり直す。Feed と同じスレッドから呼ぶ
    void Reset();
    IsdbcStatus status() const;

private:
    enum class State { kAcquire, kTrack };

    void Rotate(const cfloat* x, size_t n);          // NCO を掛けて x_ に追加
    void Process();
    bool Acquire();
    bool TrackStep();                                  // 1 シンボル分進める。入力不足なら false
    void TimingUpdate();                               // O&M でタイミングを更新（ブロック先頭で）
    double OerderMeyr(double t_base, int n_symbols) const;
    cfloat Interp(double t) const;                     // 多相 RRC 補間（整合フィルタ出力）
    static cfloat DotInterleaved(const float* x, const float* h, int n);
    void OnSymbol(cfloat z);                           // AGC → 等化 → 位相 → 判定 → ビット
    void FinishPhaseBlock();
    void SolveEqualizer();
    void UpdateTapArrays();
    void OnBits(uint8_t word);
    void OnTsp(const uint8_t* tsp);
    void FlushOutput();
    void StartTrack();
    void SetInverted(bool inv);
    double CoarseCfoHz(const std::vector<cfloat>& buf) const;
    double FineCfoCycles(const std::vector<cfloat>& z) const;

    IsdbcConfig cfg_;
    PacketFn on_packets_;
    mutable std::mutex mu_;
    IsdbcStatus st_;

    // 入力（NCO 適用済み）。x_[i] の絶対サンプル番号は x_abs0_ + i
    std::vector<cfloat> x_;
    long long x_abs0_ = 0;
    long long in_total_ = 0;
    double nco_freq_hz_ = 0.0;          // 補正周波数（信号を −f だけ回す）
    double nco_phase_ = 0.0;
    double level_acc_ = 0.0;
    long long level_n_ = 0;

    // 多相 RRC カーネル（入力レートで設計）
    static constexpr int kTaps = 40;
    static constexpr int kPhases = 256;
    std::vector<float> kernel_;         // [kPhases][kTaps]

    State state_ = State::kAcquire;
    double sym_period_ = 1.0;           // 入力サンプル / シンボル
    double next_pos_ = 0.0;             // 次のシンボルの位置（絶対サンプル番号、double）
    long long sym_count_ = 0;           // 追従開始からのシンボル数
    double agc_pow_ = 42.0;

    // 等化器（T 間隔 FFE）と判定指向 LS
    std::vector<float> zbuf_;           // 直近 eq_taps シンボルの 2 重長履歴（re, im 交互。OnSymbol 参照）
    int zpos_ = 0;
    int eq_len_ = 0;                    // 係数列の長さ（2·eq_taps を 8 の倍数に切り上げ）
    std::vector<float> c1_, c2_;        // 実部用・虚部用の係数列（UpdateTapArrays）
    std::vector<std::complex<double>> w_;
    std::vector<std::complex<double>> R_;   // eq_taps × eq_taps
    std::vector<std::complex<double>> p_;
    long long ls_count_ = 0;
    int ls_decim_ = 0;

    // 位相追従（256 シンボルブロック）
    static constexpr int kPhaseBlock = 256;
    std::vector<cfloat> ublk_;          // 位相補正前の等化出力
    std::vector<cfloat> zblk_;          // 等化器入力ベクトル（LS 用、デシメーション保存）
    std::vector<int> zblk_idx_;         // 各ベクトルに対応する ublk_ 内の位置
    int solves_ = 0;
    int blk_n_ = 0;
    int blocks_done_ = 0;
    double theta_ = 0.0, theta_rate_ = 0.0;
    double mer_err_acc_ = 0.0, mer_ref_acc_ = 0.0;
    long long mer_n_ = 0;
    double mer_db_ = 0.0;
    bool inverted_ = false;
    long long track_symbols_ = 0;       // 現在の反転設定で追従したシンボル数
    bool inversion_tried_ = false;

    // ビット → バイト同期
    DiffDecoder diff_;
    std::vector<uint8_t> bitbuf_;       // 未整列ビット（探索用）
    bool aligned_ = false;
    uint32_t bitacc_ = 0;
    int bitacc_n_ = 0;
    std::vector<uint8_t> tspbuf_;       // 組み立て中の 204 バイト
    int bad_sync_run_ = 0;

    FrameDecoder frame_;
    TsmfDemux tsmf_;
    std::vector<uint8_t> out_;          // 出力待ちパケット（188 の倍数）

    // 取得用
    std::vector<cfloat> acq_;
    isdbt::Fft* fft4096_ = nullptr;
    isdbt::Fft* fft64k_ = nullptr;
    long long low_mer_blocks_ = 0;
};

// RRC（α、sps、片側 span シンボル）のタップ列。ref/isdbc/modulator.py rrc_taps と同じ式
std::vector<float> RrcTaps(int sps, int span_symbols, double rolloff);

}  // namespace isdbc
