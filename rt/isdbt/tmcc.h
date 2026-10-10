// TMCC（伝送制御情報）の解析（STD-B31 3.15）。ref/isdbt/tmcc.py の復調側。
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "params.h"

namespace isdbt {

struct TmccLayer {
    bool used = false;
    Modulation mod = Modulation::QPSK;
    CodeRate rate = {1, 2};
    int interleave = 0;
    int segments = 0;
};

struct TmccInfo {
    int system_id = 0;
    int switching_indicator = 0b1111;
    int emergency_alarm = 0;
    bool partial_reception = false;
    TmccLayer layers[3];            // A, B, C
};

// B20..B121 の 102 ビット（1 バイトに 1 ビット）を解析する
TmccInfo ParseTmccInfo(Mode mode, const uint8_t* info_bits102);

// 184 ビット（情報 102 + パリティ 82）が (184,102) 短縮差集合巡回符号の符号語か
bool DsccCheck(const uint8_t* code_bits184);

// B1..B16 がどちらの同期語か（0: w0、1: w1、-1: 不一致）。bits は B0 起点で 17 要素以上
int SyncWordOf(const uint8_t* bits);

// 受信キャリア行列から TMCC ビット列を DBPSK 復調する（全 TMCC キャリアを同相加算）。
// Y は (n_sym, total_carriers) 行優先。b[0] は未定義（0）。
std::vector<uint8_t> TmccBitsFromCarriers(const cfloat* Y, size_t n_sym, Mode mode);

// 同期語が一致しパリティが通る最初の位置 n0 を探す。パリティが通らなければ同期語一致のみで
// 決め *parity_ok = false。見つからなければ -1。
int FindFrameStart(const std::vector<uint8_t>& b, bool* parity_ok);

}  // namespace isdbt
