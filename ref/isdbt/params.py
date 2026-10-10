"""ISDB-T 物理層パラメータ（ARIB STD-B31 準拠）

方針:
- 仕様から確実に導ける値はここに定数として置く。
- 表引きが必要で転記ミスのリスクがある値は `# VERIFY:` を付け、
  docs/isdbt-spec-notes.md に根拠を書く。未転記のものは None のままにし、
  使用側のテストは skip にする（推測で埋めないこと）。
- 大きな表は ref/isdbt/tables/ に置き、ここでは Mode ごとの辞書に束ねる。
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from fractions import Fraction

import numpy as np

from .tables import carrier_randomize as _CR
from .tables import pilot_positions as _PP

# ---------------------------------------------------------------------------
# 基本諸元（STD-B31 3.3 / 3.4）
# ---------------------------------------------------------------------------
CHANNEL_BW_HZ = 6_000_000
N_SEGMENTS = 13
SAMPLE_RATE_HZ = Fraction(512, 63) * 1_000_000      # 8.126984... MSPS（6 MHz 帯域系）
SEGMENT_BW_HZ = Fraction(CHANNEL_BW_HZ, 14)         # 428.571... kHz
SYMBOLS_PER_FRAME = 204

# 周波数軸上のセグメント並び（低い周波数から）。中央がセグメント 0（ワンセグ）。
SEGMENT_ORDER_IN_FREQ = [11, 9, 7, 5, 3, 1, 0, 2, 4, 6, 8, 10, 12]
# 逆引き: セグメント番号 → 周波数順位置
SEGMENT_FREQ_INDEX = {s: f for f, s in enumerate(SEGMENT_ORDER_IN_FREQ)}


class Mode(Enum):
    MODE1 = 1
    MODE2 = 2
    MODE3 = 3


@dataclass(frozen=True)
class ModeParams:
    mode: Mode
    fft_size: int
    carriers_per_segment: int
    total_carriers: int           # 13 セグメント + 右端の CP 1 本
    data_carriers_per_segment: int
    sp_per_segment: int           # 同期変調部（コヒーレント）の SP 本数（1 シンボルあたり）
    tmcc_per_segment: int
    ac1_per_segment: int
    time_interleave_choices: tuple  # 時間インターリーブ長 I の選択肢

    @property
    def carrier_spacing_hz(self) -> Fraction:
        return SAMPLE_RATE_HZ / self.fft_size

    @property
    def useful_symbol_s(self) -> Fraction:
        return Fraction(self.fft_size) / SAMPLE_RATE_HZ

    @property
    def data_carriers_total(self) -> int:
        return self.data_carriers_per_segment * N_SEGMENTS


MODE_PARAMS = {
    Mode.MODE1: ModeParams(Mode.MODE1, 2048, 108, 1405, 96, 9, 1, 2, (0, 4, 8, 16)),
    Mode.MODE2: ModeParams(Mode.MODE2, 4096, 216, 2809, 192, 18, 2, 4, (0, 2, 4, 8)),
    Mode.MODE3: ModeParams(Mode.MODE3, 8192, 432, 5617, 384, 36, 4, 8, (0, 1, 2, 4)),
}

GUARD_INTERVALS = {
    "1/4": Fraction(1, 4),
    "1/8": Fraction(1, 8),
    "1/16": Fraction(1, 16),
    "1/32": Fraction(1, 32),
}

# 日本の実運用
JP_DEFAULT_MODE = Mode.MODE3
JP_DEFAULT_GI = "1/8"


def guard_samples(mode: Mode, gi: str) -> int:
    return int(MODE_PARAMS[mode].fft_size * GUARD_INTERVALS[gi])


def symbol_samples(mode: Mode, gi: str) -> int:
    return MODE_PARAMS[mode].fft_size + guard_samples(mode, gi)


def carrier_to_fft_bin(mode: Mode, k) -> np.ndarray:
    """有効キャリア絶対番号 k（0..total-1）→ FFT ビン番号（0 = DC、負周波数は N + bin）。

    中央キャリア (total-1)/2 が DC に乗る（STD-B31 3.12 図: 帯域中心 = 搬送波周波数）。
    """
    mp = MODE_PARAMS[mode]
    center = (mp.total_carriers - 1) // 2
    return (np.asarray(k) - center) % mp.fft_size


# ---------------------------------------------------------------------------
# 変調・符号化（STD-B31 3.5 〜 3.8）
# ---------------------------------------------------------------------------
class Modulation(Enum):
    DQPSK = "DQPSK"
    QPSK = "QPSK"
    QAM16 = "16QAM"
    QAM64 = "64QAM"


BITS_PER_SYMBOL = {
    Modulation.DQPSK: 2,
    Modulation.QPSK: 2,
    Modulation.QAM16: 4,
    Modulation.QAM64: 6,
}

CODE_RATES = {
    "1/2": Fraction(1, 2),
    "2/3": Fraction(2, 3),
    "3/4": Fraction(3, 4),
    "5/6": Fraction(5, 6),
    "7/8": Fraction(7, 8),
}

# 畳み込み符号: 拘束長 7、生成多項式（8 進）。X 出力が G1、Y 出力が G2（DVB-T と同じ）
CONV_K = 7
CONV_G1_OCT = 0o171
CONV_G2_OCT = 0o133

# パンクチャパターン（STD-B31 3.8 表 3-8、原本照合済み。DVB-T EN 300 744 Table 3 と同一）
# 形式: (X 出力の送出マスク, Y 出力の送出マスク)。送出順は位置ごとに X, Y の順:
#   2/3: X1 Y1 Y2 / 3/4: X1 Y1 Y2 X3 / 5/6: X1 Y1 Y2 X3 Y4 X5 / 7/8: X1 Y1 Y2 Y3 Y4 X5 Y6 X7
PUNCTURE_PATTERNS = {
    "1/2": ("1", "1"),
    "2/3": ("10", "11"),
    "3/4": ("101", "110"),
    "5/6": ("10101", "11010"),
    "7/8": ("1000101", "1111010"),
}

# 外符号 RS(204,188)、GF(2^8)、原始多項式 x^8+x^4+x^3+x^2+1、生成多項式の根 α^0..α^15
RS_N = 204
RS_K = 188
RS_PRIM_POLY = 0x11D
RS_FIRST_ROOT = 0
RS_NROOTS = 16

# バイトインターリーブ（畳み込み型）: 12 ブランチ、各段 17 バイト
BYTE_INTERLEAVE_BRANCHES = 12
BYTE_INTERLEAVE_DEPTH = 17

# エネルギー拡散 PRBS: x^15 + x^14 + 1、初期値 100101010000000
# 初期化は各 OFDM フレーム先頭（階層ごと）。同期バイトは XOR しないが PRBS は 8 クロック進める。
# 伝送路上の TSP バイト順は [同期バイトの次の 203 バイト, 同期バイト]（STD-B31 3.4 / 3.5、原本照合済み）
ENERGY_DISPERSAL_POLY = (15, 14)
ENERGY_DISPERSAL_INIT = 0b100101010000000

# ビットインターリーブ遅延（ビット列ごと、単位: キャリアシンボル）。b0 が遅延 0。
BIT_INTERLEAVE_DELAYS = {
    Modulation.DQPSK: (0, 120),
    Modulation.QPSK: (0, 120),
    Modulation.QAM16: (0, 40, 80, 120),
    Modulation.QAM64: (0, 24, 48, 72, 96, 120),
}
BIT_INTERLEAVE_MAX_DELAY = 120

# 時間インターリーブ: キャリア i の遅延 = I * ((5*i) mod 96) シンボル
TIME_INTERLEAVE_MOD = 96
TIME_INTERLEAVE_MULT = 5


def time_interleave_delay(i: int, I: int) -> int:
    return I * ((TIME_INTERLEAVE_MULT * i) % TIME_INTERLEAVE_MOD)


def time_interleave_adjust(I: int) -> int:
    """時間インターリーブの送信側遅延調整量（シンボル）。

    最大遅延 95*I にこれを足すと 204 の倍数（整数フレーム）になる。
    STD-B31 表 3-12（原本照合済み）: Mode3 I=1:109, I=2:14, I=4:28 / Mode2 I=8:56 /
    Mode1 I=16:112、I=0 は 0 と一致する。
    """
    return (-95 * I) % SYMBOLS_PER_FRAME


def tsp_per_frame(mode: Mode, mod: Modulation, rate: str, n_segments: int) -> int:
    """1 階層・1 OFDM フレームあたりの TSP 数（整数になる）。"""
    mp = MODE_PARAMS[mode]
    bits = Fraction(n_segments * mp.data_carriers_per_segment * SYMBOLS_PER_FRAME
                    * BITS_PER_SYMBOL[mod]) * CODE_RATES[rate]
    tsp = bits / (RS_N * 8)
    assert tsp.denominator == 1, (mode, mod, rate, n_segments, tsp)
    return int(tsp)


def byte_interleave_adjust_tsp(mode: Mode, mod: Modulation, rate: str, n_segments: int) -> int:
    """バイトインターリーブ前の送信側遅延調整量（TSP）。合計遅延が 1 フレームになる（STD-B31 表 3-7）。"""
    return tsp_per_frame(mode, mod, rate, n_segments) - (BYTE_INTERLEAVE_BRANCHES - 1)


def bit_interleave_adjust_bits(mode: Mode, mod: Modulation, n_segments: int) -> int:
    """ビットインターリーブ前の送信側遅延調整量（ビット）。

    インターリーブ遅延 120 キャリアシンボルと合わせて 2 OFDM シンボルになる。
    STD-B31 表 3-9（原本照合済み）: Mode3 QPSK 1536N-240, 16QAM 3072N-480,
    64QAM 4608N-720（N = セグメント数）。
    """
    mp = MODE_PARAMS[mode]
    b = BITS_PER_SYMBOL[mod]
    return (2 * n_segments * mp.data_carriers_per_segment - BIT_INTERLEAVE_MAX_DELAY) * b


# ---------------------------------------------------------------------------
# マッピング（STD-B31 3.9）
# ---------------------------------------------------------------------------
# ビット列 (b0, b1, ...) のうち偶数番 (b0, b2, b4) が I 軸、奇数番 (b1, b3, b5) が Q 軸。
# b0 / b1 が符号（0 → 正）、残りがグレイ符号で振幅: 16QAM (b2)=0→3,1→1
# 64QAM (b2,b4)=00→7, 01→5, 11→3, 10→1。正規化係数は QPSK √2、16QAM √10、64QAM √42。
CONSTELLATION_NORM = {
    Modulation.DQPSK: np.sqrt(2.0),
    Modulation.QPSK: np.sqrt(2.0),
    Modulation.QAM16: np.sqrt(10.0),
    Modulation.QAM64: np.sqrt(42.0),
}


def _axis_level(bits: np.ndarray) -> np.ndarray:
    """1 軸分のビット列 (…, nb) → 符号付きレベル。bits[...,0] が符号。"""
    nb = bits.shape[-1]
    sign = 1 - 2 * bits[..., 0].astype(np.int64)
    if nb == 1:
        mag = np.ones_like(sign)
    elif nb == 2:
        mag = np.where(bits[..., 1] == 0, 3, 1)
    elif nb == 3:
        b2, b4 = bits[..., 1], bits[..., 2]
        mag = np.select([(b2 == 0) & (b4 == 0), (b2 == 0) & (b4 == 1), (b2 == 1) & (b4 == 1)],
                        [7, 5, 3], default=1)
    else:
        raise ValueError(nb)
    return sign * mag


def map_bits(bits: np.ndarray, mod: Modulation) -> np.ndarray:
    """(n, bits_per_symbol) の 0/1 配列 → 正規化済み複素シンボル (n,)。"""
    bits = np.asarray(bits)
    assert bits.shape[-1] == BITS_PER_SYMBOL[mod]
    i = _axis_level(bits[..., 0::2])
    q = _axis_level(bits[..., 1::2])
    return ((i + 1j * q) / CONSTELLATION_NORM[mod]).astype(np.complex64)


def constellation_table(mod: Modulation) -> np.ndarray:
    """添字 = ビット列を b0 を MSB として詰めた整数、値 = 複素シンボル。"""
    nb = BITS_PER_SYMBOL[mod]
    idx = np.arange(1 << nb)
    bits = (idx[:, None] >> (nb - 1 - np.arange(nb))[None, :]) & 1
    return map_bits(bits, mod)


# ---------------------------------------------------------------------------
# パイロット（STD-B31 3.12）
# ---------------------------------------------------------------------------
# SP（スキャッタードパイロット）配置: 有効キャリア絶対番号 k、シンボル番号 n で
#   k mod 12 == 3 * (n mod 4)
# セグメント内キャリア数は 12 の倍数なので、セグメント相対番号でも同じ式になる。
SP_SPACING = 12
SP_SYMBOL_PERIOD = 4


def is_sp_position(k_in_segment: int, n_symbol: int) -> bool:
    return (k_in_segment % SP_SPACING) == 3 * (n_symbol % SP_SYMBOL_PERIOD)


# パイロット振幅と PRBS: 図 3-31 g(x) = x^11 + x^9 + 1（D11 と D9 の XOR を D1 へ、出力 D11）、
# 初期値すべて 1（表 3-16 の各セグメント初期値と一致を確認済み）。値は 4/3 * (1 - 2*w_k)（表 3-17）
# w_k はキャリア絶対番号 k ごとに決まる（シンボルによらない）。SP/CP/TMCC/AC すべてに共通。
PILOT_PRBS_POLY = (11, 9)
PILOT_PRBS_INIT = 0b11111111111
PILOT_AMPLITUDE = Fraction(4, 3)

# TMCC / AC1 の絶対キャリア位置（同期変調部）。出典は tables/pilot_positions.py を参照。
TMCC_POSITIONS_ABS = {
    Mode.MODE1: tuple(_PP.TMCC_ABS_MODE1),
    Mode.MODE2: tuple(_PP.TMCC_ABS_MODE2),
    Mode.MODE3: tuple(_PP.TMCC_ABS_MODE3),
}
AC1_POSITIONS_ABS = {
    Mode.MODE1: tuple(_PP.AC1_ABS_MODE1),
    Mode.MODE2: tuple(_PP.AC1_ABS_MODE2),
    Mode.MODE3: tuple(_PP.AC1_ABS_MODE3),
}


def _abs_to_per_segment(abs_positions, mode: Mode):
    cps = MODE_PARAMS[mode].carriers_per_segment
    out = [[] for _ in range(N_SEGMENTS)]
    for p in abs_positions:
        out[p // cps].append(p % cps)
    return [tuple(sorted(s)) for s in out]


# セグメント相対番号。配列の添字は周波数順（SEGMENT_ORDER_IN_FREQ の順、低い側から）。
# 仕様書の表はセグメント番号で引くので、変換には SEGMENT_FREQ_INDEX を使う。
TMCC_POSITIONS_COHERENT = {m: _abs_to_per_segment(TMCC_POSITIONS_ABS[m], m) for m in Mode}
AC1_POSITIONS_COHERENT = {m: _abs_to_per_segment(AC1_POSITIONS_ABS[m], m) for m in Mode}

# 差動変調（DQPSK）セグメント用（STD-B31 表 3-14）。日本の実運用では未使用だが転記済み。
# 添字は周波数順セグメント。AC1 は同期変調部と同じ位置。CP はセグメント内キャリア 0。
TMCC_POSITIONS_DIFFERENTIAL = {
    Mode.MODE1: [tuple(t) for t in _PP.TMCC_DIFF_MODE1],
    Mode.MODE2: [tuple(t) for t in _PP.TMCC_DIFF_MODE2],
    Mode.MODE3: [tuple(t) for t in _PP.TMCC_DIFF_MODE3],
}
AC2_POSITIONS_DIFFERENTIAL = {
    Mode.MODE1: [tuple(t) for t in _PP.AC2_DIFF_MODE1],
    Mode.MODE2: [tuple(t) for t in _PP.AC2_DIFF_MODE2],
    Mode.MODE3: [tuple(t) for t in _PP.AC2_DIFF_MODE3],
}
CP_POSITIONS_DIFFERENTIAL = {m: _PP.CP_DIFF for m in Mode}

# キャリアランダマイズ（周波数インターリーブ、セグメント内）表。意味は tables/carrier_randomize.py
CARRIER_RANDOMIZE_TABLE = {
    Mode.MODE1: tuple(_CR.MODE1),
    Mode.MODE2: tuple(_CR.MODE2),
    Mode.MODE3: tuple(_CR.MODE3),
}


# ---------------------------------------------------------------------------
# TMCC（STD-B31 3.15）
# ---------------------------------------------------------------------------
TMCC_BITS_PER_FRAME = 204
TMCC_SYNC_WORD_W0 = 0b0011010111101110   # B1-B16（最初のフレーム）。B1 が MSB
TMCC_SYNC_WORD_W1 = 0b1100101000010001   # 次のフレームは反転。以後交互
TMCC_SEGMENT_TYPE_BITS = (17, 19)        # B17-B19: 000 = 同期変調、111 = 差動変調（表 3-20）
TMCC_INFO_BITS = (20, 121)               # B20..B121 の 102 ビットが情報部
TMCC_PARITY_BITS = (122, 203)            # 82 ビット、(184,102) 短縮差集合巡回符号

# 差集合巡回符号 (273,191) の生成多項式（次数 82）。STD-B31 3.15.6.9 と一致（原本照合済み）
TMCC_DSCC_GENERATOR_EXPONENTS = (82, 77, 76, 71, 67, 66, 56, 52, 48, 40,
                                 36, 34, 24, 22, 18, 10, 4, 0)

# TMCC 情報部のフィールド（B20 起点の相対ビット位置と幅）
TMCC_FIELDS = {
    "system_id": (0, 2),             # B20-21: 00 = 地上デジタルテレビ
    "switching_indicator": (2, 4),   # B22-25: 伝送パラメータ切替指標（通常 1111）
    "emergency_alarm": (6, 1),       # B26: 緊急警報放送用起動フラグ
    "current": (7, 40),              # B27-66: 部分受信 1 + 階層 A/B/C 各 13（変調3 + 符号化率3 + I 3 + セグ数4）
    "next": (47, 40),                # B67-106
    "reserved": (87, 15),            # B107-109 位相補正（111）、B110-121 予備（全 1）
}
# 階層情報 13 ビット内の並び（先頭からの相対位置と幅）
TMCC_LAYER_FIELDS = {"mod": (0, 3), "rate": (3, 3), "interleave": (6, 3), "segments": (9, 4)}

TMCC_MOD_CODE = {Modulation.DQPSK: 0b000, Modulation.QPSK: 0b001,
                 Modulation.QAM16: 0b010, Modulation.QAM64: 0b011}
TMCC_RATE_CODE = {"1/2": 0b000, "2/3": 0b001, "3/4": 0b010, "5/6": 0b011, "7/8": 0b100}
TMCC_UNUSED_3 = 0b111
TMCC_UNUSED_4 = 0b1111


def tmcc_interleave_code(mode: Mode, I: int) -> int:
    """時間インターリーブ長 I → 3 ビット符号。Mode により同じ符号が異なる I を表す。"""
    choices = MODE_PARAMS[mode].time_interleave_choices
    return choices.index(I)


def tmcc_interleave_from_code(mode: Mode, code: int) -> int | None:
    choices = MODE_PARAMS[mode].time_interleave_choices
    return choices[code] if code < len(choices) else None


def carrier_index_range(mode: Mode, segment_in_freq: int) -> range:
    """周波数順 segment_in_freq（0..12）番目のセグメントの絶対キャリア範囲。"""
    mp = MODE_PARAMS[mode]
    start = segment_in_freq * mp.carriers_per_segment
    return range(start, start + mp.carriers_per_segment)
