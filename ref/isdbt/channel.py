"""チャンネル劣化モデル。

安価な Pluto 互換機を想定した劣化を複素ベースバンド I/Q に加える:
- AWGN（C/N 指定）
- 搬送波周波数オフセット（±数十 kHz。クロック精度 25 ppm @ 500 MHz ≈ 12.5 kHz）
- サンプルクロック偏差（ppm 指定。リサンプルで模擬）
- マルチパス（固定遅延・減衰のタップ列）
- 初期位相・固定遅延（整数 + 小数サンプル）
時間変動する劣化（長時間受信・再追従の試験用）:
- クロックドリフト（ppm の線形ランプ。温度変化の模擬）
- 位相雑音（ランダムウォーク、線幅指定。LO の位相雑音の粗い模擬）
- 瞬断（指定区間の信号を 0 にする。アンテナ外れ・フェージングの底）
- インパルス雑音（指定区間だけ強い白色雑音を加える。家電ノイズの模擬）

ref/ なので速度は気にしない。
"""
from __future__ import annotations

from dataclasses import dataclass, field
from fractions import Fraction

import numpy as np
from scipy.signal import resample_poly

from . import sync as _sync


@dataclass
class MultipathTap:
    delay_s: float
    gain_db: float
    phase_rad: float = 0.0


@dataclass
class ChannelConfig:
    sample_rate_hz: float
    cn_db: float | None = 30.0           # None なら雑音なし
    freq_offset_hz: float = 0.0
    clock_ppm: float = 0.0
    taps: list[MultipathTap] = field(default_factory=list)
    integer_delay: int = 0
    phase_rad: float = 0.0
    seed: int | None = 0
    # --- 時間変動（長時間受信の試験用） ---
    clock_drift_ppm: tuple[float, float] | None = None   # (開始 ppm, 終了 ppm) の線形ランプ。clock_ppm とは排他
    phase_noise_linewidth_hz: float = 0.0                # ランダムウォーク位相雑音の線幅（0 = なし）
    dropouts_s: list[tuple[float, float]] = field(default_factory=list)   # [(開始秒, 長さ秒)] 信号を 0 に
    impulses_s: list[tuple[float, float, float]] = field(default_factory=list)  # [(開始秒, 長さ秒, 電力 dB 対信号)]
    # --- 受信フロントエンド（AD9363 系の模擬。雑音も含めた受信信号の最後に掛かる） ---
    dc_offset: complex = 0.0                 # LO リーク。信号 RMS に対する複素定数（0.1 = -20 dBc）
    iq_gain_imbalance_db: float = 0.0        # I/Q の振幅差 [dB]
    iq_phase_imbalance_deg: float = 0.0      # I/Q の直交位相誤差 [度]


def _signal_power(x: np.ndarray) -> float:
    return float(np.mean(np.abs(x) ** 2))


def apply_multipath(x: np.ndarray, taps: list[MultipathTap], fs: float) -> np.ndarray:
    if not taps:
        return x
    max_delay = max(round(t.delay_s * fs) for t in taps)
    y = np.zeros(len(x) + max_delay, dtype=np.complex64)
    y[: len(x)] += x  # 直接波（0 dB、遅延 0）
    for t in taps:
        d = round(t.delay_s * fs)
        g = 10 ** (t.gain_db / 20) * np.exp(1j * t.phase_rad)
        y[d : d + len(x)] += (g * x).astype(np.complex64)
    return y[: len(x)]


def apply_clock_offset(x: np.ndarray, ppm: float) -> np.ndarray:
    """受信側サンプルクロックが ppm だけ速い/遅い状況を模擬する。

    実際の比率 1 + ppm*1e-6 を分数で近似して resample_poly に渡す。
    """
    if ppm == 0.0:
        return x
    ratio = Fraction(1 + ppm * 1e-6).limit_denominator(1_000_000)
    y = resample_poly(x, ratio.numerator, ratio.denominator)
    return y.astype(np.complex64)


def apply_clock_drift(x: np.ndarray, ppm_start: float, ppm_end: float) -> np.ndarray:
    """受信側クロックの ppm が録音の間に線形に変わる状況（温度ドリフト）を模擬する。

    受信機の標本化位置は t(m) = m + ∫e dm（e = ppm·1e-6 を出力位置で線形補間）。
    """
    n_out = len(x)
    m = np.arange(n_out, dtype=np.float64)
    e = (ppm_start + (ppm_end - ppm_start) * m / max(1, n_out - 1)) * 1e-6
    t = m + np.cumsum(e) - e[0]           # ∫e dm の台形近似（先頭 0）
    return _sync.resample_at(x, t)


def apply_phase_noise(x: np.ndarray, fs: float, linewidth_hz: float, rng: np.random.Generator) -> np.ndarray:
    """ランダムウォーク位相雑音（Wiener 過程）。ステップ分散 = 2π·線幅/fs。"""
    if linewidth_hz <= 0.0:
        return x
    sigma = np.sqrt(2 * np.pi * linewidth_hz / fs)
    phi = np.cumsum(rng.normal(0.0, sigma, len(x)))
    return (x * np.exp(1j * phi)).astype(np.complex64)


def apply_dropouts(x: np.ndarray, fs: float, dropouts_s: list[tuple[float, float]]) -> np.ndarray:
    y = x.copy()
    for start_s, dur_s in dropouts_s:
        a = int(start_s * fs)
        b = min(len(y), a + int(dur_s * fs))
        y[a:b] = 0
    return y


def apply_impulses(x: np.ndarray, fs: float, impulses_s: list[tuple[float, float, float]],
                   rng: np.random.Generator) -> np.ndarray:
    if not impulses_s:
        return x
    y = x.copy()
    p = _signal_power(x)
    for start_s, dur_s, power_db in impulses_s:
        a = int(start_s * fs)
        b = min(len(y), a + int(dur_s * fs))
        n_power = p * 10 ** (power_db / 10)
        burst = rng.normal(0, np.sqrt(n_power / 2), (b - a, 2)).view(np.complex128)[:, 0]
        y[a:b] += burst.astype(np.complex64)
    return y


def apply_iq_imbalance(x: np.ndarray, gain_db: float, phase_deg: float) -> np.ndarray:
    """受信機の I/Q インバランス。y = α·x + β·conj(x)（β が鏡像妨害を作る）。

    I 枝の振幅を g = 10^(gain_db/20) 倍、直交位相誤差 φ: y_I = g·Re(x)、
    y_Q = Im(x)·cos φ + Re(x)·sin φ と置くと α = (g + e^{jφ})/2、β = (g − e^{-jφ})/2。
    """
    if gain_db == 0.0 and phase_deg == 0.0:
        return x
    g = 10 ** (gain_db / 20)
    phi = np.deg2rad(phase_deg)
    alpha = (g + np.exp(1j * phi)) / 2
    beta = (g - np.exp(-1j * phi)) / 2
    return (alpha * x + beta * np.conj(x)).astype(np.complex64)


def apply_dc_offset(x: np.ndarray, dc: complex) -> np.ndarray:
    """LO リーク由来の DC オフセット。dc は信号 RMS に対する複素係数。"""
    if dc == 0:
        return x
    rms = np.sqrt(_signal_power(x))
    return (x + np.complex64(dc * rms)).astype(np.complex64)


def apply_freq_offset(x: np.ndarray, fs: float, df: float, phase0: float = 0.0) -> np.ndarray:
    if df == 0.0 and phase0 == 0.0:
        return x
    n = np.arange(len(x))
    return (x * np.exp(1j * (2 * np.pi * df * n / fs + phase0))).astype(np.complex64)


def apply_awgn(x: np.ndarray, cn_db: float | None, rng: np.random.Generator) -> np.ndarray:
    if cn_db is None:
        return x
    p = _signal_power(x)
    n_power = p / (10 ** (cn_db / 10))
    noise = rng.normal(0, np.sqrt(n_power / 2), (len(x), 2)).view(np.complex128)[:, 0]
    return (x + noise).astype(np.complex64)


def apply_channel(x: np.ndarray, cfg: ChannelConfig) -> np.ndarray:
    """変調器出力 x（complex）に cfg の劣化を順に加える。"""
    rng = np.random.default_rng(cfg.seed)
    y = x.astype(np.complex64)
    y = apply_multipath(y, cfg.taps, cfg.sample_rate_hz)
    if cfg.clock_drift_ppm is not None:
        assert cfg.clock_ppm == 0.0, "clock_ppm と clock_drift_ppm は排他"
        y = apply_clock_drift(y, *cfg.clock_drift_ppm)
    else:
        y = apply_clock_offset(y, cfg.clock_ppm)
    y = apply_phase_noise(y, cfg.sample_rate_hz, cfg.phase_noise_linewidth_hz, rng)
    y = apply_freq_offset(y, cfg.sample_rate_hz, cfg.freq_offset_hz, cfg.phase_rad)
    if cfg.integer_delay:
        y = np.concatenate([np.zeros(cfg.integer_delay, dtype=np.complex64), y])
    y = apply_dropouts(y, cfg.sample_rate_hz, cfg.dropouts_s)
    y = apply_impulses(y, cfg.sample_rate_hz, cfg.impulses_s, rng)
    y = apply_awgn(y, cfg.cn_db, rng)
    # 受信フロントエンドの劣化は（雑音も含めた）最後に掛ける
    y = apply_iq_imbalance(y, cfg.iq_gain_imbalance_db, cfg.iq_phase_imbalance_deg)
    y = apply_dc_offset(y, cfg.dc_offset)
    return y
