"""TMCC（伝送制御情報）の生成と解析（STD-B31 3.15）。

1 フレーム 204 ビット B0..B203:
  B0        : 差動変調の基準（送出値は PRBS w_k による）
  B1-B16    : 同期語 w0 = 0011010111101110、次フレームは反転（以後交互）
  B17-B19   : セグメント形式識別（差動変調 111 / 同期変調 000、STD-B31 表 3-20）
  B20-B121  : TMCC 情報 102 ビット
  B122-B203 : パリティ 82 ビット（(184,102) 短縮差集合巡回符号）
TMCC 情報の中身は params.TMCC_FIELDS / TMCC_LAYER_FIELDS を参照。
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from . import params as P

LAYER_NAMES = ("A", "B", "C")


@dataclass
class TmccLayer:
    mod: P.Modulation | None   # None = 未使用
    rate: str | None
    interleave: int | None
    segments: int | None


@dataclass
class TmccInfo:
    system_id: int = 0
    switching_indicator: int = 0b1111
    emergency_alarm: int = 0
    partial_reception: bool = False
    layers: dict = field(default_factory=dict)          # name -> TmccLayer（current）
    next_partial_reception: bool | None = None          # None なら current と同じ
    next_layers: dict | None = None
    phase_correction: int = 0b111
    reserved: int = 0b111111111111


def _put(bits: np.ndarray, pos: int, width: int, value: int) -> None:
    for i in range(width):
        bits[pos + i] = (value >> (width - 1 - i)) & 1


def _get(bits: np.ndarray, pos: int, width: int) -> int:
    v = 0
    for i in range(width):
        v = (v << 1) | int(bits[pos + i])
    return v


def _layer_bits(mode: P.Mode, layer: TmccLayer | None) -> int:
    """階層情報 13 ビット（変調 3、符号化率 3、I 3、セグメント数 4）を整数で返す。"""
    if layer is None or layer.segments in (None, 0):
        return (P.TMCC_UNUSED_3 << 10) | (P.TMCC_UNUSED_3 << 7) | (P.TMCC_UNUSED_3 << 4) | P.TMCC_UNUSED_4
    mod = P.TMCC_MOD_CODE[layer.mod]
    rate = P.TMCC_RATE_CODE[layer.rate]
    il = P.tmcc_interleave_code(mode, layer.interleave)
    segs = layer.segments
    assert 1 <= segs <= 13
    return (mod << 10) | (rate << 7) | (il << 4) | segs


def _params_bits(mode: P.Mode, partial: bool, layers: dict) -> int:
    v = int(partial)
    for name in LAYER_NAMES:
        v = (v << 13) | _layer_bits(mode, layers.get(name))
    return v


def encode_info(mode: P.Mode, info: TmccInfo) -> np.ndarray:
    """TMCC 情報 102 ビット（B20..B121 相当）を返す。"""
    bits = np.zeros(102, dtype=np.uint8)
    f = P.TMCC_FIELDS
    _put(bits, *f["system_id"], info.system_id)
    _put(bits, *f["switching_indicator"], info.switching_indicator)
    _put(bits, *f["emergency_alarm"], info.emergency_alarm)
    _put(bits, *f["current"], _params_bits(mode, info.partial_reception, info.layers))
    nxt_partial = info.partial_reception if info.next_partial_reception is None else info.next_partial_reception
    nxt_layers = info.layers if info.next_layers is None else info.next_layers
    _put(bits, *f["next"], _params_bits(mode, nxt_partial, nxt_layers))
    _put(bits, *f["reserved"], (info.phase_correction << 12) | info.reserved)
    return bits


def dscc_parity(info_bits: np.ndarray) -> np.ndarray:
    """102 ビット → 82 ビットのパリティ（系統的符号化、m(x)·x^82 mod g(x)）。先頭が最高次。"""
    g = np.zeros(83, dtype=np.uint8)
    for e in P.TMCC_DSCC_GENERATOR_EXPONENTS:
        g[82 - e] = 1          # g[0] が x^82 の係数
    r = np.concatenate([np.asarray(info_bits, dtype=np.uint8), np.zeros(82, dtype=np.uint8)])
    for i in range(102):
        if r[i]:
            r[i:i + 83] ^= g
    return r[102:].copy()


def dscc_check(code_bits: np.ndarray) -> bool:
    """184 ビット（情報 102 + パリティ 82）が符号語か。"""
    cb = np.asarray(code_bits, dtype=np.uint8)
    return bool(np.array_equal(dscc_parity(cb[:102]), cb[102:]))


def build_frame_bits(mode: P.Mode, info: TmccInfo, frame_index: int,
                     coherent: bool = True) -> np.ndarray:
    """フレーム番号 frame_index（0 起点）の TMCC ビット列 B0..B203。B0 は 0 として置く。"""
    bits = np.zeros(P.TMCC_BITS_PER_FRAME, dtype=np.uint8)
    sync = P.TMCC_SYNC_WORD_W0 if frame_index % 2 == 0 else P.TMCC_SYNC_WORD_W1
    _put(bits, 1, 16, sync)
    _put(bits, 17, 3, 0b000 if coherent else 0b111)   # 表 3-20: 差動 111、同期 000
    info_bits = encode_info(mode, info)
    bits[20:122] = info_bits
    bits[122:204] = dscc_parity(info_bits)
    return bits


def differential_encode(bits: np.ndarray) -> np.ndarray:
    """B0..B203 → 差動符号化後 D_n（D_0 = 0、D_n = D_{n-1} xor B_n）。送出値は (1-2 D_n)·(4/3)(1-2 w_k)。"""
    b = np.asarray(bits, dtype=np.uint8)
    d = np.zeros_like(b)
    for n in range(1, len(b)):
        d[n] = d[n - 1] ^ b[n]
    return d


# ---------------------------------------------------------------------------
# 解析（復調器用。誤り訂正は未実装、パリティ検査のみ）
# ---------------------------------------------------------------------------
def parse_layer(mode: P.Mode, v: int) -> TmccLayer | None:
    mod_c, rate_c, il_c, segs = (v >> 10) & 7, (v >> 7) & 7, (v >> 4) & 7, v & 15
    if segs == P.TMCC_UNUSED_4 or mod_c == P.TMCC_UNUSED_3:
        return None
    mod = {c: m for m, c in P.TMCC_MOD_CODE.items()}.get(mod_c)
    rate = {c: r for r, c in P.TMCC_RATE_CODE.items()}.get(rate_c)
    il = P.tmcc_interleave_from_code(mode, il_c)
    return TmccLayer(mod, rate, il, segs)


def parse_info(mode: P.Mode, info_bits: np.ndarray) -> TmccInfo:
    f = P.TMCC_FIELDS
    b = np.asarray(info_bits, dtype=np.uint8)
    cur = _get(b, *f["current"])
    nxt = _get(b, *f["next"])

    def split(v):
        partial = bool((v >> 39) & 1)
        layers = {}
        for i, name in enumerate(LAYER_NAMES):
            lv = (v >> (26 - 13 * i)) & 0x1FFF
            layers[name] = parse_layer(mode, lv)
        return partial, layers

    cp, cl = split(cur)
    npart, nl = split(nxt)
    res = _get(b, *f["reserved"])
    return TmccInfo(system_id=_get(b, *f["system_id"]),
                    switching_indicator=_get(b, *f["switching_indicator"]),
                    emergency_alarm=_get(b, *f["emergency_alarm"]),
                    partial_reception=cp, layers=cl,
                    next_partial_reception=npart, next_layers=nl,
                    phase_correction=res >> 12, reserved=res & 0xFFF)


def sync_word_of(bits: np.ndarray) -> int | None:
    """B1..B16 を見てどちらの同期語か（0: w0, 1: w1, None: 不一致）。"""
    v = _get(np.asarray(bits, dtype=np.uint8), 1, 16)
    if v == P.TMCC_SYNC_WORD_W0:
        return 0
    if v == P.TMCC_SYNC_WORD_W1:
        return 1
    return None
