"""ISDB-C（J.83 Annex C + TSMF）の固定パラメータとケーブル TV チャンネル表。

根拠の章番号は ITU-T J.83 (12/2007) Annex C と ITU-T J.183 (03/2001)。
数値の出所は docs/isdbc-spec-notes.md に書く。
"""
from __future__ import annotations

from dataclasses import dataclass

from isdbt import params as _tp

# ---------------------------------------------------------------------------
# 変調（J.83 C.6）
# ---------------------------------------------------------------------------
# シンボルレート。J.83 Annex C 本文は「6 MHz チャンネルに最適化」とだけ書き、数値は
# JCTEA STD-002 側にある。日本の 64QAM / 256QAM トラモジは 5.274 Mbaud
# （BonDriver_BDA の J.83C-64QAM 設定 ModulationType0SymbolRate=5274 と一致）。
# VERIFY: JCTEA STD-002 原本の表番号
SYMBOL_RATE_HZ = 5_274_000.0
ROLLOFF = 0.13                      # J.83 C.6.4
QAM64_BITS = 6                      # m = 6（C.6.1: 3 バイト → 4 シンボル）
QAM64_RMS = 42.0 ** 0.5             # レベル ±1,±3,±5,±7 の平均電力 42 の平方根

# ---------------------------------------------------------------------------
# フレーム構成・誤り訂正（J.83 C.4 / C.5 = Annex A と同一）
# ---------------------------------------------------------------------------
TS_PACKET = 188
RS_N = _tp.RS_N                     # 204（C.5.2: RS(204,188) t=8、ISDB-T と同じ符号）
RS_K = _tp.RS_K                     # 188
SYNC_BYTE = 0x47
SYNC_BYTE_INVERTED = 0xB8           # C.5.1: 8 パケットごとの先頭パケットの同期バイトを反転
SCRAMBLE_GROUP = 8                  # C.5.1: PRBS は 8 パケットごとに初期化
PRBS_PERIOD_BYTES = 8 * TS_PACKET - 1   # 1503（C.5.1 の記述どおり。反転同期バイト直後から数える）
INTERLEAVE_I = _tp.BYTE_INTERLEAVE_BRANCHES   # 12（C.5.3。同期バイトはブランチ 0 = 遅延なし）
INTERLEAVE_M = 17

# 有効 TS ビットレート（同期バイト含む 188 バイト換算）: 5.274 M × 6 bit × 188/204 ≈ 29.16 Mbps
TS_BITRATE_BPS = SYMBOL_RATE_HZ * QAM64_BITS * RS_K / RS_N

# ---------------------------------------------------------------------------
# TSMF（J.183 Appendix I の運用値 = JCTEA STD-002）
# ---------------------------------------------------------------------------
TSMF_SLOTS = 53                     # ヘッダ 1 + TS パケット 52
TSMF_DATA_SLOTS = TSMF_SLOTS - 1
TSMF_MAX_TS = 15                    # 相対 TS 番号 1..15（m = 4 ビット）
TSMF_HEADER_PID = 0x002F            # VERIFY: J.183 は「他と重ならない値」とだけ規定。JCTEA/受信機実装の値
TSMF_SYNC = 0x1A86                  # 13 ビット（上位 3 ビットは reserved）
TSMF_SYNC_MASK = 0x1FFF
TSMF_SYNC_INVERTED = (~TSMF_SYNC) & TSMF_SYNC_MASK   # 0x0579。VERIFY: 反転パターンの運用（フレームごと交互）
TSMF_FRAME_TYPE = 0x1               # VERIFY: frame_type の値（受信機実装は 1 を要求）

# ---------------------------------------------------------------------------
# ケーブル TV のチャンネル表（BonDriver_BDA の ChannelSettingsAuto="TransModulation" と同じ
# 通し番号・周波数。TVTest の .ch2 を流用できるようにする）
#   0..2   VHF 1..3ch   93 MHz + 6 MHz × k
#   3..11  VHF 4..12ch  171 MHz + 6 MHz × k
#   12..61 UHF 13..62ch 473 MHz + 6 MHz × k   （地デジと違い +1/7 MHz オフセットは無い）
#   62..71 CATV C13..C22 111 MHz + 6 MHz × k
#   72..112 CATV C23..C63 225 MHz + 6 MHz × k
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class CatvChannel:
    index: int          # BonDriver のチャンネル番号（チューニング空間内）
    name: str           # "56ch" / "C47ch"
    freq_hz: float      # 中心周波数


def _band(start_index: int, first_ch: int, count: int, base_hz: float, fmt: str) -> list[CatvChannel]:
    return [CatvChannel(start_index + k, fmt % (first_ch + k), base_hz + 6e6 * k) for k in range(count)]


CATV_CHANNELS: list[CatvChannel] = (
    _band(0, 1, 3, 93e6, "%dch")
    + _band(3, 4, 9, 171e6, "%dch")
    + _band(12, 13, 50, 473e6, "%dch")
    + _band(62, 13, 10, 111e6, "C%dch")
    + _band(72, 23, 41, 225e6, "C%dch")
)
assert len(CATV_CHANNELS) == 113 and all(c.index == i for i, c in enumerate(CATV_CHANNELS))


def catv_channel(index: int) -> CatvChannel:
    return CATV_CHANNELS[index]


def catv_index_from_name(name: str) -> int:
    """"56ch" / "C47ch" / "56" → チャンネル番号。無ければ ValueError。"""
    key = name.strip().lower()
    if not key.endswith("ch"):
        key += "ch"
    for c in CATV_CHANNELS:
        if c.name.lower() == key:
            return c.index
    raise ValueError(f"unknown CATV channel: {name}")
