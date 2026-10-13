// TSMF（ITU-T J.183 Appendix I、53 スロット × 188 バイト、最大 15 TS）の受信側。ref/isdbc/tsmf.py の逐次版。
// ヘッダの配置は ref/isdbc/tsmf.py 冒頭と docs/isdbc-spec-notes.md 5 章。
#pragma once
#include <cstddef>
#include <cstdint>

#include "params.h"

namespace isdbc {

uint32_t Crc32Mpeg(const uint8_t* data, size_t n);

struct TsmfHeader {
    int continuity_counter = 0;
    bool sync_inverted = false;
    int version = 0;
    int frame_type = 0;
    bool available[kTsmfMaxTs] = {};
    uint16_t ts_id[kTsmfMaxTs] = {};
    uint16_t onid[kTsmfMaxTs] = {};
    uint8_t receive_status[kTsmfMaxTs] = {};
    int emergency = 0;
    uint8_t slot_ts[kTsmfDataSlots] = {};      // スロット 1..52 の相対 TS 番号（0 = 空き）
};

// PID 0x002F のパケットか（ヘッダ候補）
bool IsTsmfHeaderCandidate(const uint8_t* pkt);
// 同期語・固定ビット・CRC まで確認してヘッダを解析する
bool ParseTsmfHeader(const uint8_t* pkt, TsmfHeader* h);

// 1 本の TS を選び出す。TSMF ヘッダを一度も見ていなければ全パケットを通す（単一 TS）。
class TsmfDemux {
public:
    struct Select {
        int rel = 0;            // 相対 TS 番号 1..15。0 = 自動（有効 TS のうち最小番号）
        int tsid = -1;          // ≥ 0 なら TS_id で選ぶ（rel より優先）
        int onid = -1;          // tsid 指定時、≥ 0 なら original_network_id も一致を要求
        bool pass_all = false;  // ヘッダ含め全スロットを通す
    };
    void SetSelect(const Select& s) { sel_ = s; }
    void Reset();
    // パケットを 1 個入れ、出力すべきなら true
    bool Push(const uint8_t* pkt);

    bool has_header() const { return have_; }
    const TsmfHeader& header() const { return hdr_; }
    long long frames() const { return frames_; }
    long long bad_headers() const { return bad_; }
    int selected() const { return selected_; }      // 0 = 未選択

private:
    int Resolve() const;

    Select sel_;
    TsmfHeader hdr_;
    bool have_ = false;
    int slot_ = -1;             // 直近ヘッダからのスロット番号（1..52）
    long long frames_ = 0, bad_ = 0;
    int selected_ = 0;
};

}  // namespace isdbc
