"""受信品質測定（measure.py）のテスト。"""
from __future__ import annotations

import numpy as np
import pytest
from isdbt import measure
from isdbt import params as P
from isdbt.channel import ChannelConfig, MultipathTap, apply_channel

FS = float(P.SAMPLE_RATE_HZ)

STD_CFG = {
    "mode": P.Mode.MODE3,
    "gi": "1/8",
    "layers": {
        "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 4},
        "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 2},
    },
}


def make_test_ts(n_packets: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pk = rng.integers(0, 256, size=(n_packets, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    return pk


def test_mer_of_known_noise():
    """既知の S/N を持つ 64QAM シンボル列の MER が S/N と一致する。"""
    rng = np.random.default_rng(0)
    bits = rng.integers(0, 2, size=(50_000, 6))
    z = P.map_bits(bits, P.Modulation.QAM64)
    snr_db = 28.0
    noise = (rng.normal(size=len(z)) + 1j * rng.normal(size=len(z))) * np.sqrt(10 ** (-snr_db / 10) / 2)
    mer = measure.mer_db(z + noise.astype(np.complex64), P.Modulation.QAM64)
    assert abs(mer - snr_db) < 0.5


def test_slice_nearest_identity():
    bits = np.random.default_rng(1).integers(0, 2, size=(1000, 4))
    z = P.map_bits(bits, P.Modulation.QAM16)
    assert np.allclose(measure.slice_nearest(z, P.Modulation.QAM16), z, atol=1e-6)


@pytest.mark.slow
def test_detect_mode_gi():
    """Mode 3 / GI 1/8 と Mode 2 / GI 1/16 の信号を正しく判定する。"""
    from isdbt.modulator import modulate
    iq = modulate({"A": make_test_ts(64, 1), "B": make_test_ts(2592, 2)}, STD_CFG)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=25.0, freq_offset_hz=5000.0, seed=3))
    mode, gi, scores = measure.detect_mode_gi(rx, FS)
    assert mode == P.Mode.MODE3 and gi == "1/8", scores

    cfg2 = {"mode": P.Mode.MODE2, "gi": "1/16", "layers": {
        "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 8},
        "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 4}}}
    iq2 = modulate({"A": make_test_ts(10, 4), "B": make_test_ts(100, 5)}, cfg2)
    rx2 = apply_channel(iq2, ChannelConfig(sample_rate_hz=FS, cn_db=25.0, seed=6))
    mode2, gi2, scores2 = measure.detect_mode_gi(rx2, FS)
    assert mode2 == P.Mode.MODE2 and gi2 == "1/16", scores2


@pytest.mark.slow
def test_rx_quality_clean_and_multipath():
    """C/N 30 dB で MER ≈ 29±2 dB、マルチパスでキャリア別 MER にリップルが出る。"""
    from isdbt.modulator import modulate
    iq = modulate({"A": make_test_ts(64, 7), "B": make_test_ts(2592, 8)}, STD_CFG)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, seed=9))
    r = measure.rx_quality(rx, FS)
    assert r["tmcc_parity_ok"]
    assert 27.0 < r["mer_db"]["B"] < 32.0, r["mer_db"]
    assert 27.0 < r["mer_db"]["A"] < 32.0
    cm = r["carrier_mer_db"]
    # データが乗りうる位置 = 有効キャリア 5616（CP 除く）− TMCC 52 − AC1 104
    assert np.isfinite(cm).sum() == 5616 - 52 - 104
    # 遅延プロファイル: クリーンならピークは遅延 0 近傍のみ
    prof = r["delay_profile_db"]
    assert prof[0] > -1.0

    taps = [MultipathTap(delay_s=20e-6, gain_db=-10.0, phase_rad=1.0)]
    rx2 = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, taps=taps, seed=10))
    r2 = measure.rx_quality(rx2, FS)
    # 2 波 20 µs -10 dB: 遅延プロファイルに -10 dB 付近の第 2 パスが見える
    d_idx = round(20e-6 * FS)
    second = r2["delay_profile_db"][d_idx - 2: d_idx + 3].max()
    assert -14.0 < second < -6.0, second
