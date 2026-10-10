"""デインターリーブ（STD-B31 3.9〜3.11 の逆）と軟判定デマップ。

受信側の補遅延は仕様の値に合わせる（docs/isdbt-spec-notes.md 10 章）:
  時間: キャリア i に I·(95 − m_i) シンボル（送信 I·m_i と合わせて 95·I、調整込みで整数フレーム）
  ビット: b_i に 120 − d_i キャリアシンボル（合計 2 OFDM シンボル）
  バイト: パス b に 11 − b TSP（fec.byte_deinterleave）
配列は (n_sym, 13[セグメント番号], dc) を基本とし、複素データと実数の重み（CSI）の両方に使えるようにする。
"""
from __future__ import annotations

import numpy as np

from . import params as P
from . import pilots


# ---------------------------------------------------------------------------
# 周波数デインターリーブ（3.11.2 の逆）: データキャリア抽出 → セグメント番号順 → ランダマイズ・ローテーション解除
# ---------------------------------------------------------------------------
def data_to_logical(D: np.ndarray, mode: P.Mode, frame_start: int) -> np.ndarray:
    """等化後キャリア行列 (n_sym, total) → 論理キャリア (n_sym, 13[セグメント番号], dc)。

    シンボル n の SP 位相は (n − frame_start) mod 4。周波数順セグメント f はセグメント番号
    SEGMENT_ORDER_IN_FREQ[f]。ランダマイズ前の論理キャリア c はセグメント s の伝送位置
    perm[(c − s) mod dc] にある（図 3-27、表 3-13）。
    """
    mp = P.MODE_PARAMS[mode]
    dc = mp.data_carriers_per_segment
    n_sym = D.shape[0]
    dpos4 = pilots.data_carrier_positions_4(mode)                 # (4, 13[freq], dc)
    freq_of_seg = np.array([P.SEGMENT_FREQ_INDEX[s] for s in range(P.N_SEGMENTS)])
    phases = (np.arange(n_sym) - frame_start) % P.SP_SYMBOL_PERIOD
    Z = np.empty((n_sym, P.N_SEGMENTS, dc), dtype=D.dtype)
    for p in range(P.SP_SYMBOL_PERIOD):
        sel = np.flatnonzero(phases == p)
        if len(sel) == 0:
            continue
        Zf = D[sel][:, dpos4[p]]                                   # (nsel, 13[freq], dc)
        Z[sel] = Zf[:, freq_of_seg, :]
    perm = np.array(P.CARRIER_RANDOMIZE_TABLE[mode])
    c = np.arange(dc)
    Y = np.empty_like(Z)
    for s in range(P.N_SEGMENTS):
        Y[:, s, :] = Z[:, s, perm[(c - s) % dc]]
    return Y


# ---------------------------------------------------------------------------
# 時間デインターリーブ（3.11.1 の逆）
# ---------------------------------------------------------------------------
def time_deinterleave(Y: np.ndarray, I_per_segment) -> np.ndarray:
    """out[n, s, c] = Y[n − I_s·(95 − m_c), s, c]、m_c = (5c) mod 96。参照先が無い先頭部分は 0。"""
    n_sym, _, dc = Y.shape
    out = np.zeros_like(Y)
    c = np.arange(dc)
    n = np.arange(n_sym)
    for s in range(P.N_SEGMENTS):
        I = int(I_per_segment[s])
        delay = I * (P.TIME_INTERLEAVE_MOD - 1 - (P.TIME_INTERLEAVE_MULT * c) % P.TIME_INTERLEAVE_MOD)
        src = n[:, None] - delay[None, :]
        valid = src >= 0
        out[:, s, :] = np.where(valid, Y[np.clip(src, 0, None), s, c[None, :]], 0)
    return out


def time_deinterleave_max_delay(I: int) -> int:
    return I * (P.TIME_INTERLEAVE_MOD - 1)


# ---------------------------------------------------------------------------
# セグメント間デインターリーブ（図 3-26 の逆）
# ---------------------------------------------------------------------------
def intersegment_deinterleave(X: np.ndarray, groups: list) -> np.ndarray:
    """グループ内: 直列 S[q] = X[group[q mod n], q div n]、S をセグメント順に n 等分して戻す。"""
    n_sym, _, dc = X.shape
    out = X.copy()
    for group in groups:
        n = len(group)
        if n < 2:
            continue
        S = np.transpose(X[:, group, :], (0, 2, 1)).reshape(n_sym, n * dc)
        out[:, group, :] = S.reshape(n_sym, n, dc)
    return out


def layer_stream(Xs: np.ndarray, segments: list) -> np.ndarray:
    """セグメント間デインターリーブ後の行列から階層の直列シンボル (n_sym, nseg*dc) を取り出す。"""
    n_sym = Xs.shape[0]
    return Xs[:, segments, :].reshape(n_sym, -1)


# ---------------------------------------------------------------------------
# 硬判定スライス（最近傍コンスタレーション点）と軟判定デマップ（図 3-15/17/19 の逆）
# ---------------------------------------------------------------------------
def slice_nearest(z: np.ndarray, mod: P.Modulation) -> np.ndarray:
    """正規化済みシンボル → 最近傍の理想コンスタレーション点（MER・妨害検出用）。"""
    norm = P.CONSTELLATION_NORM[mod]
    zz = np.asarray(z) * norm
    if mod in (P.Modulation.QPSK, P.Modulation.DQPSK):
        levels = np.array([-1.0, 1.0])
    elif mod == P.Modulation.QAM16:
        levels = np.array([-3.0, -1.0, 1.0, 3.0])
    else:
        levels = np.array([-7.0, -5.0, -3.0, -1.0, 1.0, 3.0, 5.0, 7.0])
    i = levels[np.argmin(np.abs(zz.real[..., None] - levels), axis=-1)]
    q = levels[np.argmin(np.abs(zz.imag[..., None] - levels), axis=-1)]
    return ((i + 1j * q) / norm).astype(np.complex64)



def demap_soft(D: np.ndarray, mod: P.Modulation) -> np.ndarray:
    """正規化を戻した複素シンボル（レベル ±1, ±3, …）→ (..., bits_per_symbol) の軟判定値。

    偶数ビットが I 軸、奇数ビットが Q 軸。b0/b1 は符号、以降はグレイ符号の振幅判定:
      16QAM: b2 = |I| − 2（0 ↔ レベル 3）
      64QAM: b2 = |I| − 4（0 ↔ 7,5）、b4 = ||I| − 4| − 2（0 ↔ 7,1）
    """
    z = np.asarray(D) * P.CONSTELLATION_NORM[mod]
    i, q = z.real.astype(np.float32), z.imag.astype(np.float32)
    if mod in (P.Modulation.QPSK, P.Modulation.DQPSK):
        return np.stack([i, q], axis=-1)
    if mod == P.Modulation.QAM16:
        return np.stack([i, q, np.abs(i) - 2, np.abs(q) - 2], axis=-1)
    if mod == P.Modulation.QAM64:
        ai, aq = np.abs(i), np.abs(q)
        return np.stack([i, q, ai - 4, aq - 4, np.abs(ai - 4) - 2, np.abs(aq - 4) - 2], axis=-1)
    raise ValueError(mod)


# ---------------------------------------------------------------------------
# ビットデインターリーブ（3.9.3 の逆）
# ---------------------------------------------------------------------------
def bit_deinterleave(soft: np.ndarray, mod: P.Modulation) -> np.ndarray:
    """(n_slots, bps) → 同形。out[t, i] = in[t − (120 − d_i), i]。先頭の参照できない部分は 0。"""
    delays = np.array(P.BIT_INTERLEAVE_DELAYS[mod])
    rx_delay = P.BIT_INTERLEAVE_MAX_DELAY - delays
    n = soft.shape[0]
    out = np.zeros_like(soft)
    for i, d in enumerate(rx_delay):
        if d < n:
            out[d:, i] = soft[: n - d, i]
    return out
