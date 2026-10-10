"""ループバックテスト: 変調器 → チャンネル → 復調器 → TS 比較。

このファイルは開発の「ゴール定義」である。各段が実装されるまでは skip される。
基準値は CLAUDE.md の「テスト要件」と一致させること。
"""
from __future__ import annotations

import numpy as np
import pytest
from isdbt import params as P
from isdbt.channel import ChannelConfig, MultipathTap, apply_channel

FS = float(P.SAMPLE_RATE_HZ)


# -------------------------------------------------------------------------
# 試験 TS の生成（決定論的）
# -------------------------------------------------------------------------
def make_test_ts(n_packets: int, seed: int = 0) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pk = rng.integers(0, 256, size=(n_packets, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    # PID 0x100、継続カウンタを埋めて ts_check で検証可能にする
    pk[:, 1] = 0x01
    pk[:, 2] = 0x00
    pk[:, 3] = 0x10 | (np.arange(n_packets) % 16)
    return pk


@pytest.fixture
def layer_config():
    """日本の標準運用に近い構成。TMCC で表現できる範囲に収める。"""
    return {
        "mode": P.Mode.MODE3,
        "gi": "1/8",
        "layers": {
            "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 4},
            "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 2},
        },
    }


def _modulate_or_skip(ts_a, ts_b, cfg):
    try:
        from isdbt.modulator import modulate
    except ImportError:
        pytest.skip("modulator 未実装")
    return modulate({"A": ts_a, "B": ts_b}, cfg)


def _demodulate_or_skip(iq, fs):
    try:
        from isdbt.demod import demodulate
    except ImportError:
        pytest.skip("demod 未実装")
    return demodulate(iq, fs)


def _assert_ts_equal(expected: np.ndarray, actual: np.ndarray, layer: str):
    """時間インターリーブと同期確立による先頭欠落を許容し、連続一致区間を要求する。"""
    assert actual is not None and len(actual) > 0, f"layer {layer}: TS が出力されない"
    # expected の後半（少なくとも 50%）が actual の中に連続して現れること
    n = len(expected)
    tail = expected[n // 2:]
    a = actual.tobytes()
    assert tail.tobytes() in a, f"layer {layer}: TS が一致しない"


# -------------------------------------------------------------------------
# ループバック本体
# -------------------------------------------------------------------------
@pytest.mark.slow
def test_loopback_clean(layer_config):
    ts_a = make_test_ts(200, seed=1)
    ts_b = make_test_ts(2000, seed=2)
    iq = _modulate_or_skip(ts_a, ts_b, layer_config)
    iq = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0))
    out = _demodulate_or_skip(iq, FS)
    _assert_ts_equal(ts_a, out.get("A"), "A")
    _assert_ts_equal(ts_b, out.get("B"), "B")


@pytest.mark.slow
@pytest.mark.parametrize("df_hz", [-50_000.0, 50_000.0])
@pytest.mark.parametrize("ppm", [-30.0, 30.0])
def test_loopback_offsets(layer_config, df_hz, ppm):
    ts_a = make_test_ts(200, seed=3)
    ts_b = make_test_ts(2000, seed=4)
    iq = _modulate_or_skip(ts_a, ts_b, layer_config)
    iq = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0,
                                         freq_offset_hz=df_hz, clock_ppm=ppm))
    out = _demodulate_or_skip(iq, FS)
    _assert_ts_equal(ts_a, out.get("A"), "A")
    _assert_ts_equal(ts_b, out.get("B"), "B")


@pytest.mark.slow
def test_loopback_multipath(layer_config):
    ts_a = make_test_ts(200, seed=5)
    ts_b = make_test_ts(2000, seed=6)
    iq = _modulate_or_skip(ts_a, ts_b, layer_config)
    taps = [MultipathTap(delay_s=20e-6, gain_db=-10.0, phase_rad=1.0)]
    iq = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, taps=taps))
    out = _demodulate_or_skip(iq, FS)
    _assert_ts_equal(ts_b, out.get("B"), "B")


@pytest.mark.slow
def test_loopback_resampled_input(layer_config):
    """8.127 MSPS 以外（7.0 MSPS）で録音された前提の入力が処理できること。"""
    from scipy.signal import resample_poly
    ts_a = make_test_ts(200, seed=7)
    ts_b = make_test_ts(2000, seed=8)
    iq = _modulate_or_skip(ts_a, ts_b, layer_config)
    fs_rec = 7_000_000.0
    # 8126984.127 → 7000000 を分数近似してリサンプル
    from fractions import Fraction
    r = Fraction(fs_rec / FS).limit_denominator(10_000)
    iq_rec = resample_poly(iq, r.numerator, r.denominator).astype(np.complex64)
    iq_rec = apply_channel(iq_rec, ChannelConfig(sample_rate_hz=fs_rec, cn_db=30.0))
    out = _demodulate_or_skip(iq_rec, fs_rec)
    _assert_ts_equal(ts_b, out.get("B"), "B")


@pytest.mark.slow
def test_ber_curve_64qam_34(layer_config):
    """C/N を振り、Viterbi 前の硬判定 BER が 64QAM の理論値（実装損 3 dB 以内）に沿い、
    Viterbi 後の BER が高 C/N でほぼ 0、低 C/N で有意になること。"""
    from isdbt.demod import demodulate_to_ber
    from isdbt.modulator import modulate
    from isdbt.theory import qam_ber
    ts_a = make_test_ts(64, 9)
    ts_b = make_test_ts(2592, 10)              # 1 データフレーム分
    iq, tdbg = modulate({"A": ts_a, "B": ts_b}, layer_config, return_debug=True)
    # 全キャリア平均電力に対するデータキャリアの電力比（パイロット 625 本が (4/3)^2）
    n_pilot = 468 + 52 + 104 + 1
    data_frac_db = 10 * np.log10(5617 / (5617 - n_pilot + n_pilot * 16 / 9))
    results = {}
    for cn in (16.0, 19.0, 22.0, 25.0):
        y = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=cn, seed=int(cn)))
        r = demodulate_to_ber(y, FS, tdbg)["B"]
        results[cn] = r
        esn0 = cn + data_frac_db
        assert r["raw_bits"] > 1_000_000
        assert r["raw_ber"] <= qam_ber(64, esn0 - 3.0), (cn, r)        # 実装損 3 dB 以内
        assert r["raw_ber"] >= qam_ber(64, esn0 + 3.0) * 0.5, (cn, r)  # 理論より良すぎない（計測の健全性）
    assert results[25.0]["viterbi_ber"] < 1e-6
    assert results[16.0]["viterbi_ber"] > 1e-3
    assert results[25.0]["rs_uncorrectable"] == 0


# -------------------------------------------------------------------------
# チャンネルモデル自体の単体テスト（実装済み、常時実行）
# -------------------------------------------------------------------------
def test_channel_awgn_cn():
    rng = np.random.default_rng(0)
    x = (rng.normal(size=100_000) + 1j * rng.normal(size=100_000)).astype(np.complex64)
    x /= np.sqrt(np.mean(np.abs(x) ** 2))
    y = apply_channel(x, ChannelConfig(sample_rate_hz=FS, cn_db=20.0, seed=1))
    noise = y - x
    cn = 10 * np.log10(np.mean(np.abs(x) ** 2) / np.mean(np.abs(noise) ** 2))
    assert abs(cn - 20.0) < 0.2


def test_channel_freq_offset_shifts_tone():
    n = 65536
    t = np.arange(n) / FS
    tone = np.exp(1j * 2 * np.pi * 100_000 * t).astype(np.complex64)
    y = apply_channel(tone, ChannelConfig(sample_rate_hz=FS, cn_db=None, freq_offset_hz=10_000))
    spec = np.abs(np.fft.fft(y))
    peak_hz = np.fft.fftfreq(n, 1 / FS)[np.argmax(spec)]
    assert abs(peak_hz - 110_000) < FS / n * 2


def test_channel_clock_offset_changes_length():
    x = np.ones(100_000, dtype=np.complex64)
    y = apply_channel(x, ChannelConfig(sample_rate_hz=FS, cn_db=None, clock_ppm=100.0))
    assert abs(len(y) - 100_010) <= 2
