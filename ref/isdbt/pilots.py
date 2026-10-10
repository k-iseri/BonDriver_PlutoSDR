"""パイロット・制御キャリアの配置と値（STD-B31 3.12）。変調器と復調器の両方が使う。"""
from __future__ import annotations

import numpy as np

from . import params as P


def pilot_prbs(mode: P.Mode) -> np.ndarray:
    """キャリア絶対番号 k ごとの PRBS 出力 w_k（0/1）。STD-B31 図 3-31: g(x) = x^11 + x^9 + 1。

    reg の bit10 が D1（最新）、bit0 が D11（出力）。D11 xor D9（bit0 xor bit2）を D1 に入れて右シフト。
    全 1 で初期化し帯域左端（セグメント 11 のキャリア 0）から連続生成する。各セグメント先頭での
    レジスタ状態が表 3-16 と一致することをテストで確認済み。
    """
    n = P.MODE_PARAMS[mode].total_carriers
    reg = P.PILOT_PRBS_INIT
    w = np.zeros(n, dtype=np.uint8)
    for k in range(n):
        w[k] = reg & 1
        new = ((reg >> 2) ^ reg) & 1
        reg = (reg >> 1) | (new << 10)
    return w


def pilot_values(mode: P.Mode) -> np.ndarray:
    """キャリア k の BPSK パイロット値 (4/3)(1 - 2 w_k)。実数。"""
    w = pilot_prbs(mode).astype(np.float64)
    return float(P.PILOT_AMPLITUDE) * (1.0 - 2.0 * w)


def sp_positions(mode: P.Mode, n_symbol: int) -> np.ndarray:
    """シンボル番号 n における SP の絶対キャリア番号（右端 CP は含まない）。"""
    mp = P.MODE_PARAMS[mode]
    start = 3 * (n_symbol % P.SP_SYMBOL_PERIOD)
    return np.arange(start, mp.total_carriers - 1, P.SP_SPACING)


def cp_position(mode: P.Mode) -> int:
    return P.MODE_PARAMS[mode].total_carriers - 1


def data_carrier_positions(mode: P.Mode, n_symbol: int) -> np.ndarray:
    """シンボル番号 n のデータキャリア絶対番号を (13, data_per_segment) で返す。

    行は周波数順セグメント（0 = 最低周波数）、列はセグメント内データキャリア番号
    （SP/TMCC/AC/CP を除いて周波数の低い順に詰めた番号）。
    """
    mp = P.MODE_PARAMS[mode]
    n_active = mp.total_carriers - 1
    is_pilot = np.zeros(n_active, dtype=bool)
    is_pilot[sp_positions(mode, n_symbol)] = True
    is_pilot[list(P.TMCC_POSITIONS_ABS[mode])] = True
    is_pilot[list(P.AC1_POSITIONS_ABS[mode])] = True
    data = np.flatnonzero(~is_pilot)
    assert len(data) == mp.data_carriers_total, (len(data), mp.data_carriers_total)
    return data.reshape(P.N_SEGMENTS, mp.data_carriers_per_segment)


def data_carrier_positions_4(mode: P.Mode) -> np.ndarray:
    """n mod 4 = 0..3 の 4 通りをまとめて (4, 13, data_per_segment) で返す。"""
    return np.stack([data_carrier_positions(mode, n) for n in range(P.SP_SYMBOL_PERIOD)])
