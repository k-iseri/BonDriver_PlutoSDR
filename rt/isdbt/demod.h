// ISDB-T 復調器（rt 実装）: 有効キャリア行列 → TMCC → 等化 → デインターリーブ → 復号 → TS。
// ref/isdbt/demod.py demodulate(block_frames=6) と同じブロック処理・同じフレーム整合規約。
#pragma once
#include <map>
#include <string>
#include <vector>

#include "mat.h"
#include "params.h"
#include "sync.h"
#include "tmcc.h"

namespace isdbt {

struct RxLayer {
    std::string name;               // "A" / "B" / "C"
    Modulation mod;
    CodeRate rate;
    int I;
    std::vector<int> segments;      // セグメント番号
};

struct FrameSyncInfo {
    int frame_start = -1;           // TMCC B0 のシンボル番号
    bool tmcc_parity_ok = false;
    TmccInfo tmcc;
    std::vector<RxLayer> layers;
    std::vector<std::vector<int>> groups;   // セグメント間インターリーブのグループ
    int n_frames = 0;
};

// 先頭数フレームから TMCC を復号してフレーム先頭と階層構成を決める
FrameSyncInfo FrameSync(ISymbolSource* src, Mode mode);

struct LayerResult {
    std::vector<uint8_t> ts;        // 188 バイト × n 連結
    std::vector<int> rs_err;        // TSP ごとの訂正バイト数（訂正不能は -1）
    int first_frame = 0;            // 出力先頭の受信フレーム番号
    double mer_db = 0.0;            // 等化後コンスタレーションの MER [dB]
};

struct DemodResult {
    SyncInfo sync;
    FrameSyncInfo fsi;
    std::map<std::string, LayerResult> layers;
};

// I/Q（サンプルレート fs）→ 階層ごとの TS。block_frames フレームずつ処理する。
// ブロックの前段（FFT〜デインターリーブ）と復号（Viterbi〜RS）はスレッドでパイプライン化される
DemodResult Demodulate(std::vector<cfloat> iq, double fs, Mode mode, Gi gi, int block_frames = 6);

// 追従型（長時間・クロックドリフト・瞬断向け）: ロック区間ごとにフレーム同期からやり直す
struct TrackedSegment {
    int i0 = 0, i1 = 0;             // シンボル範囲（録音全体）
    bool decoded = false;
    std::string skip_reason;
    FrameSyncInfo fsi;
    std::map<std::string, LayerResult> layers;      // この区間の分
};
struct TrackedResult {
    std::vector<TrackedSegment> segments;
    std::map<std::string, LayerResult> layers;      // 区間をまたいで連結した TS
};
TrackedResult DemodulateTracked(std::vector<cfloat> iq, double fs, Mode mode, Gi gi,
                                int block_frames = 6);

// --- 個別の段（テストとデーモンが使う） ---

// 等化 → 論理キャリア → 時間 → セグメント間デインターリーブ。H は不要なら nullptr
void EqualizeAndDeinterleave(const Mat<cfloat>& Y, Mode mode, int frame_start,
                             const std::vector<RxLayer>& layers,
                             const std::vector<std::vector<int>>& groups,
                             Mat<cfloat>* H_out, Mat<cfloat>* Xt, Mat<float>* Wt);

// 等化済みキャリア（D = Y/H と CSI）からデインターリーブ以降。FPGA 等化器（pluto-fw-isdbt モード 4）の
// 出力をホスト後段に渡す入口。EqualizeAndDeinterleave の等化部分を飛ばしたもの
void DeinterleaveEqualized(const Mat<cfloat>& D, const Mat<float>& csi, Mode mode, int frame_start,
                           const std::vector<RxLayer>& layers,
                           const std::vector<std::vector<int>>& groups,
                           Mat<cfloat>* Xt, Mat<float>* Wt);

// 行列をそのまま返す ISymbolSource（等化済みキャリア行列のフレーム同期・復号用）
class MatSource : public ISymbolSource {
public:
    explicit MatSource(const Mat<cfloat>* m) : m_(m) {}
    int n_sym() const override { return static_cast<int>(m_->rows); }
    Mat<cfloat> Symbols(int i0, int i1) override;
private:
    const Mat<cfloat>* m_;
};

// 等化済みキャリア行列 D (n_sym, total) と CSI → 階層ごとの TS（Demodulate の後段だけ）。
// 同期情報は gi_start0 = 0（行 i はウィンドウ先頭 + i シンボル）として返す
DemodResult DemodulateEqualized(const Mat<cfloat>& D, const Mat<float>& csi, Mode mode,
                                int block_frames = 6);

// 時間デインターリーブの参照先が揃う最初の受信フレーム番号
int LayerFirstFrame(const RxLayer& lyr, int frame_start);

// ビットデインターリーブ後の軟判定 (n_slots, bps) → TS パケット列・RS 統計
void DecodeSoftBits(const Mat<float>& soft, const RxLayer& lyr, Mode mode,
                    std::vector<uint8_t>* ts, std::vector<int>* nerr);

}  // namespace isdbt
