"""受信フロントエンド劣化（AD9363 模擬）のテスト: DC オフセット、IQ インバランス、8bit 量子化。

実機（Pluto 互換機）の初回キャプチャで必ず踏む劣化を、実機なしで先回りして検証する。
"""
from __future__ import annotations

import numpy as np
import pytest
from isdbt import io
from isdbt import params as P
from isdbt.channel import ChannelConfig, apply_channel, apply_iq_imbalance
from isdbt.demod import demodulate

FS = float(P.SAMPLE_RATE_HZ)


def make_test_ts(n_packets: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pk = rng.integers(0, 256, size=(n_packets, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    pk[:, 1] = 0x01
    pk[:, 2] = 0x00
    pk[:, 3] = 0x10 | (np.arange(n_packets) % 16)
    return pk


STD_CFG = {
    "mode": P.Mode.MODE3,
    "gi": "1/8",
    "layers": {
        "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 4},
        "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 2},
    },
}


def _contains(haystack: np.ndarray, needle: np.ndarray) -> bool:
    return needle.tobytes() in haystack.tobytes()


# -------------------------------------------------------------------------
# 劣化モデル単体（常時実行）
# -------------------------------------------------------------------------
def test_iq_imbalance_creates_image():
    """+f のトーンに掛けると -f に鏡像が立ち、そのレベルが |β/α|² になる。"""
    n = 65536
    t = np.arange(n) / FS
    f0 = 500_000.0
    tone = np.exp(2j * np.pi * f0 * t).astype(np.complex64)
    y = apply_iq_imbalance(tone, 0.5, 2.0)
    spec = np.abs(np.fft.fft(y)) ** 2
    freqs = np.fft.fftfreq(n, 1 / FS)
    main = spec[np.argmin(np.abs(freqs - f0))]
    image = spec[np.argmin(np.abs(freqs + f0))]
    g = 10 ** (0.5 / 20)
    phi = np.deg2rad(2.0)
    expect = np.abs((g - np.exp(-1j * phi)) / (g + np.exp(1j * phi))) ** 2
    assert abs(10 * np.log10(image / main) - 10 * np.log10(expect)) < 0.5


def test_dc_offset_adds_dc():
    x = (np.random.default_rng(0).normal(size=(10000, 2)).view(np.complex128)[:, 0]).astype(np.complex64)
    x /= np.sqrt(np.mean(np.abs(x) ** 2))
    y = apply_channel(x, ChannelConfig(sample_rate_hz=FS, cn_db=None, dc_offset=0.3 + 0.1j))
    assert abs(np.mean(y) - (0.3 + 0.1j)) < 0.05


# -------------------------------------------------------------------------
# ループバック（slow）
# -------------------------------------------------------------------------
@pytest.mark.slow
def test_dc_offset_loopback():
    """-6 dBc の強い LO リークがあっても受信側の DC 除去で TS が一致する。"""
    from isdbt.modulator import modulate
    ts_a = make_test_ts(64, 31)
    ts_b = make_test_ts(2592, 32)
    iq = modulate({"A": ts_a, "B": ts_b}, STD_CFG)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, freq_offset_hz=1000.0,
                                         dc_offset=0.45 + 0.2j, seed=33))
    out = demodulate(rx, FS)
    assert _contains(out["A"], ts_a[len(ts_a) // 2:])
    assert _contains(out["B"], ts_b[len(ts_b) // 2:])


@pytest.mark.slow
def test_iq_imbalance_loopback():
    """軽い IQ インバランス（0.2 dB / 1°、鏡像 ≈ -43 dB）は補正なしで許容できる。

    これより大きい場合は受信側の推定・補正が要る（実機で問題になったら実装する）。
    """
    from isdbt.modulator import modulate
    ts_a = make_test_ts(64, 34)
    ts_b = make_test_ts(2592, 35)
    iq = modulate({"A": ts_a, "B": ts_b}, STD_CFG)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, freq_offset_hz=-1500.0,
                                         iq_gain_imbalance_db=0.2, iq_phase_imbalance_deg=1.0, seed=36))
    out = demodulate(rx, FS)
    assert _contains(out["B"], ts_b[len(ts_b) // 2:])


@pytest.mark.slow
def test_ci8_roundtrip_loopback(tmp_path):
    """8bit 録音（ci8、USB 2.0 の実運用モード）の量子化を通しても TS が一致する。"""
    from isdbt.modulator import modulate
    ts_a = make_test_ts(64, 37)
    ts_b = make_test_ts(2592, 38)
    iq = modulate({"A": ts_a, "B": ts_b}, STD_CFG)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, freq_offset_hz=800.0, seed=39))
    # RMS 0.2 フルスケール（ピーク ~4σ がちょうど収まる控えめなゲイン設定を模擬）
    rx = (rx / np.sqrt(np.mean(np.abs(rx) ** 2)) * 0.2).astype(np.complex64)
    base = tmp_path / "ci8_test"
    io.write_sigmf(base, rx, FS, datatype="ci8")
    back, meta = io.read_sigmf(base)
    assert meta["global"]["core:datatype"] == "ci8"
    out = demodulate(back, float(meta["global"]["core:sample_rate"]))
    assert _contains(out["A"], ts_a[len(ts_a) // 2:])
    assert _contains(out["B"], ts_b[len(ts_b) // 2:])
