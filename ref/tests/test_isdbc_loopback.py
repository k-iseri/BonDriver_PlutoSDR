"""ISDB-C ループバック: 変調器 → チャンネル劣化 → 復調器 → TS 一致。

基準（CLAUDE.md のループバック基準に準じる）:
  - AWGN C/N 30 dB: TS 完全一致（RS 訂正不能 0）
  - 周波数オフセット ±50 kHz、サンプルクロック偏差 ±30 ppm: 完全一致
  - マルチパス（ケーブルの短い反射 2 波）: 完全一致
  - スペクトル反転: 自動検出して完全一致
  - TSMF 多重から相対 TS 番号 / TS_id で選択
"""
from __future__ import annotations

import numpy as np
import pytest
from isdbc import demod, framing, modulator, tsmf
from isdbt.channel import ChannelConfig, MultipathTap, apply_channel

FS = 7_032_000.0        # = 4/3 × 5.274 MHz（2 sps へのリサンプル比が 3:2 で速い）


def make_ts(n: int, seed: int, pid: int = 0x100) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pk = framing.null_packets(n)
    pk[:, 1] = (pid >> 8) & 0x1F
    pk[:, 2] = pid & 0xFF
    pk[:, 3] = 0x10 | (np.arange(n) % 16)
    pk[:, 4:] = rng.integers(0, 256, size=(n, 184), dtype=np.uint8)
    return pk


def assert_ts_match(expected: np.ndarray, got: np.ndarray, min_fraction: float = 0.9):
    """got の中に expected の後半が連続して現れ、重なる範囲が完全一致すること。"""
    assert got is not None and got.shape[0] > 0, "TS が出力されない"
    n = expected.shape[0]
    anchor = expected[n // 4].tobytes()
    pos = got.tobytes().find(anchor)
    assert pos >= 0 and pos % 188 == 0, "TS が一致しない（アンカーが見つからない）"
    i0 = pos // 188
    m = min(got.shape[0] - i0, n - n // 4)
    assert m >= min_fraction * (n - n // 4), f"一致区間が短い: {m}"
    assert np.array_equal(got[i0:i0 + m], expected[n // 4:n // 4 + m]), "TS の内容が一致しない"


@pytest.fixture(scope="module")
def tx():
    pk = make_ts(1600, seed=1)
    return pk, modulator.modulate(pk, FS)


def test_loopback_clean(tx):
    pk, iq = tx
    out = demod.demodulate(apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0)), FS)
    assert out["rs_uncorrectable"] == 0
    assert out["mer_db"] > 27.0
    assert not out["inverted"]
    assert_ts_match(pk, out["packets"])


@pytest.mark.parametrize("cfo_hz,ppm", [(50e3, 30.0), (-50e3, -30.0)])
def test_loopback_offsets(tx, cfo_hz, ppm):
    pk, iq = tx
    cfg = ChannelConfig(sample_rate_hz=FS, cn_db=30.0, freq_offset_hz=cfo_hz, clock_ppm=ppm, phase_rad=0.7)
    out = demod.demodulate(apply_channel(iq, cfg), FS)
    assert out["rs_uncorrectable"] == 0
    assert abs(out["cfo_hz"] - cfo_hz) < 500.0
    assert abs(out["clock_ppm"] - ppm) < 3.0
    assert_ts_match(pk, out["packets"])


def test_loopback_multipath(tx):
    pk, iq = tx
    taps = [MultipathTap(0.0, 0.0), MultipathTap(0.6e-6, -12.0, 1.0), MultipathTap(1.5e-6, -18.0, -2.0)]
    out = demod.demodulate(apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=32.0, taps=taps)), FS)
    assert out["rs_uncorrectable"] == 0
    assert out["mer_db"] > 26.0
    assert_ts_match(pk, out["packets"])


def test_loopback_spectral_inversion(tx):
    pk, iq = tx
    out = demod.demodulate(np.conj(apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0))), FS)
    assert out["inverted"]
    assert out["rs_uncorrectable"] == 0
    assert_ts_match(pk, out["packets"])


def test_loopback_other_sample_rate():
    pk = make_ts(800, seed=7)
    fs = 7_111_111.0            # Pluto の既定取り込みレート
    iq = modulator.modulate(pk, fs)
    out = demod.demodulate(apply_channel(iq, ChannelConfig(sample_rate_hz=fs, cn_db=30.0)), fs)
    assert out["rs_uncorrectable"] == 0
    assert_ts_match(pk, out["packets"])


def test_loopback_tsmf_select():
    s1, s2 = make_ts(1200, seed=11, pid=0x101), make_ts(300, seed=12, pid=0x102)
    ids = {1: (16625, 4), 2: (16592, 4)}
    mux = tsmf.multiplex({1: s1, 2: s2}, ids)
    iq = apply_channel(modulator.modulate(mux, FS), ChannelConfig(sample_rate_hz=FS, cn_db=30.0))
    out = demod.demodulate(iq, FS, select=2)
    assert out["tsmf"].frames > 10 and out["tsmf"].selected == 2
    assert out["tsmf"].streams == [(1, 16625, 4), (2, 16592, 4)]
    assert_ts_match(s2, out["packets"])
    out = demod.demodulate(iq, FS, select=(4, 16625))
    assert_ts_match(s1, out["packets"])


def test_cn_threshold():
    """RS のみ（内符号なし）なので 64QAM は C/N 18 dB で破綻、24 dB では訂正できる。

    C/N は標本化帯域（FS）基準なので、整合フィルタ後の Es/N0 はそれより 10log10(FS/Rs) ≈ 1.2 dB 高い。
    実放送（AGC 過大ゲインで MER 20 dB）では RS がほぼ全滅した（docs/isdbc-spec-notes.md 6 章）。"""
    pk = make_ts(800, seed=21)
    iq = modulator.modulate(pk, FS)
    bad = demod.demodulate(apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=18.0)), FS)
    assert bad["rs_uncorrectable"] > 0.5 * len(bad["rs_err"])
    ok = demod.demodulate(apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=24.0)), FS)
    assert ok["rs_uncorrectable"] == 0
