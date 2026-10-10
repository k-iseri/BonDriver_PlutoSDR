"""長時間受信・再追従のテスト: 時間変動チャンネル + TrackedCarrierSource + 区間再同期。

チャンネルモデルの単体テストは常時実行、ループバックは slow。
ドリフト量は実機（温度変化で ~0.1 ppm/s 程度）より桁違いに速くして短い信号で追従を試す。
"""
from __future__ import annotations

import numpy as np
import pytest
from isdbt import params as P
from isdbt import sync
from isdbt.channel import ChannelConfig, apply_channel, apply_clock_drift
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
TSP_A = 64        # QPSK 2/3 × 1 セグの TSP/フレーム
TSP_B = 2592      # 64QAM 3/4 × 12 セグの TSP/フレーム


def _contains(haystack: np.ndarray, needle: np.ndarray) -> bool:
    return needle.tobytes() in haystack.tobytes()


# -------------------------------------------------------------------------
# チャンネルモデル単体（常時実行）
# -------------------------------------------------------------------------
def test_clock_drift_constant_matches_fractional():
    """ドリフト無し（一定 ppm）なら resample_fractional と同じ結果になる。"""
    rng = np.random.default_rng(0)
    x = (rng.normal(size=50_000) + 1j * rng.normal(size=50_000)).astype(np.complex64)
    y1 = apply_clock_drift(x, 50.0, 50.0)
    y2 = sync.resample_fractional(x, 1 + 50e-6, 0, len(x))
    n = min(len(y1), len(y2)) - 100
    err = np.sqrt(np.mean(np.abs(y1[100:n] - y2[100:n]) ** 2))
    assert err < 1e-3


def test_clock_drift_ramp_shifts_timing():
    """0→100 ppm のランプで末尾の標本化位置が約 ∫e dm だけずれる。"""
    n = 200_000
    ramp = np.arange(n, dtype=np.float64)       # 位置がそのまま読めるように傾き 1 の信号
    y = apply_clock_drift(ramp.astype(np.complex64), 0.0, 100.0)
    # 末尾での累積ずれ = 平均 50 ppm × n = 10 サンプル
    assert abs((y[n - 100].real - (n - 100)) - 50e-6 * (n - 100)) < 0.2


def test_phase_noise_preserves_power_and_walks():
    rng = np.random.default_rng(1)
    x = np.ones(100_000, dtype=np.complex64)
    y = apply_channel(x, ChannelConfig(sample_rate_hz=FS, cn_db=None, phase_noise_linewidth_hz=100.0, seed=2))
    assert abs(np.mean(np.abs(y) ** 2) - 1.0) < 1e-3
    phi = np.unwrap(np.angle(y))
    steps = np.diff(phi)
    assert abs(np.var(steps) - 2 * np.pi * 100.0 / FS) < 0.2 * 2 * np.pi * 100.0 / FS
    assert rng is not None


def test_dropout_zeroes_region():
    x = np.ones(int(FS * 0.01), dtype=np.complex64)
    y = apply_channel(x, ChannelConfig(sample_rate_hz=FS, cn_db=None, dropouts_s=[(0.002, 0.001)]))
    a = int(0.002 * FS)
    b = a + int(0.001 * FS)       # apply_dropouts と同じ丸め
    assert np.all(y[a:b] == 0) and np.all(y[:a] != 0) and np.all(y[b:] != 0)


def test_impulse_adds_burst_power():
    x = np.ones(int(FS * 0.01), dtype=np.complex64)
    y = apply_channel(x, ChannelConfig(sample_rate_hz=FS, cn_db=None, impulses_s=[(0.004, 0.002, 10.0)]))
    a, b = int(0.004 * FS), int(0.006 * FS)
    burst = y[a:b] - 1.0
    p = np.mean(np.abs(burst) ** 2)
    assert 10 ** 0.8 < p < 10 ** 1.2            # +10 dB ± 2 dB


# -------------------------------------------------------------------------
# TrackedCarrierSource（slow）
# -------------------------------------------------------------------------
@pytest.mark.slow
def test_tracked_source_clean_single_segment():
    """劣化なしなら 1 区間・全チャンクロック・ppm ≈ 0。"""
    from isdbt.modulator import modulate
    iq = modulate({"A": make_test_ts(64, 1), "B": make_test_ts(TSP_B, 2)}, STD_CFG)
    src = sync.TrackedCarrierSource(iq, FS)
    info = src.info()
    assert len(src.segments()) == 1
    assert info["locked_chunks"] == info["total_chunks"]
    assert abs(info["ppm_min"]) < 1.0 and abs(info["ppm_max"]) < 1.0


@pytest.mark.slow
def test_tracking_clock_drift_loopback():
    """±15 ppm の線形ドリフト（実機の数百倍速い）でも TS が一致する。"""
    from isdbt.modulator import modulate
    ts_a = make_test_ts(3 * TSP_A, 3)
    ts_b = make_test_ts(3 * TSP_B, 4)
    iq = modulate({"A": ts_a, "B": ts_b}, STD_CFG)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, freq_offset_hz=1000.0,
                                         clock_drift_ppm=(-15.0, 15.0), seed=5))
    out = demodulate(rx, FS, track=True)
    assert _contains(out["A"], ts_a[len(ts_a) // 2:])
    assert _contains(out["B"], ts_b[len(ts_b) // 2:])


@pytest.mark.slow
def test_tracking_phase_noise_loopback():
    """ランダムウォーク位相雑音（線幅 0.5 Hz）で TS が一致する（CPE 補正の検証）。"""
    from isdbt.modulator import modulate
    ts_a = make_test_ts(2 * TSP_A, 6)
    ts_b = make_test_ts(2 * TSP_B, 7)
    iq = modulate({"A": ts_a, "B": ts_b}, STD_CFG)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0,
                                         phase_noise_linewidth_hz=0.5, seed=8))
    out = demodulate(rx, FS, track=True)
    assert _contains(out["B"], ts_b[len(ts_b) // 2:])


@pytest.mark.slow
def test_tracking_dropout_resync():
    """0.3 秒の瞬断を挟んでも、前後の区間がそれぞれ復調される。

    瞬断は全体の 35% の位置。後半の区間は時間デインターリーブの立ち上がり
    （約 2 フレーム）を捨てた後もデータフレームが残るように前寄りにする。
    """
    from isdbt.modulator import modulate
    ts_b = make_test_ts(8 * TSP_B, 9)
    iq = modulate({"A": make_test_ts(8 * TSP_A, 10), "B": ts_b}, STD_CFG)
    dur_s = len(iq) / FS
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, freq_offset_hz=-800.0,
                                         dropouts_s=[(dur_s * 0.35, 0.3)], seed=11))
    out, dbg = demodulate(rx, FS, track=True, return_debug=True)
    decoded = [d for d in dbg["segments"] if "skipped" not in d]
    assert len(decoded) >= 2, f"区間が再同期されていない: {dbg['segments']}"
    b = out["B"]
    assert any(_contains(b, ts_b[k * TSP_B:(k + 1) * TSP_B]) for k in (0, 1, 2)), \
        "瞬断前の TS が出ていない"
    assert any(_contains(b, ts_b[k * TSP_B:(k + 1) * TSP_B]) for k in (5, 6, 7)), \
        "瞬断後の TS が出ていない（再同期失敗）"


@pytest.mark.slow
def test_tracking_impulse_noise_loopback():
    """短いインパルス雑音（0.5 ms × 3、+10 dB）で受信が崩壊しないこと。

    バーストは SP の等化平均窓（前後 8 シンボル）にも混入するため周辺シンボルの等化が
    劣化し、完全一致は保証できない。ここでは「訂正不能 TSP が少数にとどまり、
    無傷の区間はビット一致する」ことを確認する。
    """
    from isdbt.modulator import modulate
    ts_b = make_test_ts(3 * TSP_B, 12)
    iq = modulate({"A": make_test_ts(3 * TSP_A, 13), "B": ts_b}, STD_CFG)
    dur_s = len(iq) / FS
    imp = [(dur_s * r, 0.0005, 10.0) for r in (0.3, 0.5, 0.7)]
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, impulses_s=imp, seed=14))
    out, dbg = demodulate(rx, FS, track=True, return_debug=True)
    nerr = dbg["rs_errors"]["B"]
    bad = int((nerr[11:] < 0).sum())            # 先頭 11 TSP（履歴なし）は除く
    assert bad < 0.05 * len(nerr), f"訂正不能 TSP が多すぎる: {bad}/{len(nerr)}"
    # 少なくとも 1 フレーム分の TS は無傷で一致する
    assert any(_contains(out["B"], ts_b[k * TSP_B:(k + 1) * TSP_B]) for k in range(3)), \
        "無傷のフレームが 1 つも無い"
