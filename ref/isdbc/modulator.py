"""ISDB-C 変調器（試験信号生成専用）: TS パケット列 → J.83 Annex C の 64QAM ベースバンド I/Q。

処理順（J.83 図 C.1）: 同期反転 + ランダマイズ → RS(204,188) → 畳み込みインターリーブ →
バイト → m タプル → 2 MSB 差動符号化 → 64QAM マッピング → ルートロールオフ（α = 0.13）整形。
複数 TS を載せるときは tsmf.multiplex で TSMF フレーム列にしてから渡す。
"""
from __future__ import annotations

from fractions import Fraction

import numpy as np
from scipy.signal import resample_poly, upfirdn

from . import framing, mapping
from . import params as P


def rrc_taps(sps: int, span_symbols: int = 16, rolloff: float = P.ROLLOFF) -> np.ndarray:
    """ルートレイズドコサインのインパルス応答（J.83 C.6.4 の H(f) の平方根特性）。単位エネルギーに正規化。"""
    n = span_symbols * sps
    t = np.arange(-n, n + 1) / sps          # シンボル単位
    a = rolloff
    h = np.empty_like(t)
    for k, tk in enumerate(t):
        if abs(tk) < 1e-9:
            h[k] = 1 - a + 4 * a / np.pi
        elif a > 0 and abs(abs(tk) - 1 / (4 * a)) < 1e-9:
            h[k] = (a / np.sqrt(2)) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * a))
                                       + (1 - 2 / np.pi) * np.cos(np.pi / (4 * a)))
        else:
            h[k] = (np.sin(np.pi * tk * (1 - a)) + 4 * a * tk * np.cos(np.pi * tk * (1 + a))) / \
                   (np.pi * tk * (1 - (4 * a * tk) ** 2))
    h /= np.sqrt(np.sum(h ** 2))
    return h.astype(np.float32)


def pad_packets(packets: np.ndarray) -> np.ndarray:
    """ランダマイズ単位（8 パケット）の倍数になるよう NULL パケットで埋める。"""
    pk = np.asarray(packets, dtype=np.uint8)
    rem = (-pk.shape[0]) % P.SCRAMBLE_GROUP
    if rem:
        pk = np.concatenate([pk, framing.null_packets(rem)], axis=0)
    return pk


def modulate_symbols(packets: np.ndarray) -> np.ndarray:
    """TS パケット (n,188) → 差動符号化・マッピング済みの 64QAM シンボル列（平均電力 1）。"""
    stream = framing.encode_ts(pad_packets(packets))
    words = mapping.bytes_to_mtuples(stream)
    enc = mapping.diff_encode(words)
    return (mapping.map_symbols(enc) / P.QAM64_RMS).astype(np.complex64)


def modulate(packets: np.ndarray, fs: float, *, symbol_rate: float = P.SYMBOL_RATE_HZ,
             rolloff: float = P.ROLLOFF, span_symbols: int = 16, spectral_inversion: bool = False) -> np.ndarray:
    """TS パケット (n,188) → サンプルレート fs の複素ベースバンド I/Q（RMS ≈ 1、complex64）。

    2 サンプル/シンボルで RRC 整形してから fs へ有理数比リサンプルする。
    """
    sym = modulate_symbols(packets)
    h = rrc_taps(2, span_symbols, rolloff)
    x2 = upfirdn(h, sym, up=2)                                  # 2 sps
    r = Fraction(float(fs) / (2.0 * symbol_rate)).limit_denominator(4000)
    y = resample_poly(x2, r.numerator, r.denominator) if r != 1 else x2
    y = y / np.sqrt(np.mean(np.abs(y) ** 2))
    if spectral_inversion:
        y = np.conj(y)
    return y.astype(np.complex64)
