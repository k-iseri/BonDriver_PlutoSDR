"""BTS（204 バイト放送 TS）のテスト。根拠は STD-B31 3.2 / 添付 5.5（ref/isdbt/bts.py 冒頭）。"""
from __future__ import annotations

import numpy as np
import pytest
from isdbt import bts
from isdbt import params as P

STD_LAYERS = [
    bts.SlotLayer("A", 1, P.Modulation.QPSK, "2/3"),
    bts.SlotLayer("B", 12, P.Modulation.QAM64, "3/4"),
]


def make_test_ts(n_packets: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pk = rng.integers(0, 256, size=(n_packets, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    pk[:, 1] = 0x01
    pk[:, 2] = 0x00
    pk[:, 3] = 0x10 | (np.arange(n_packets) % 16)
    return pk


def test_mux_tsps_per_frame_matches_table_3_5():
    """表 3-5 の値と一致する。"""
    expect = {
        (P.Mode.MODE1, "1/4"): 1280, (P.Mode.MODE1, "1/8"): 1152,
        (P.Mode.MODE1, "1/16"): 1088, (P.Mode.MODE1, "1/32"): 1056,
        (P.Mode.MODE2, "1/4"): 2560, (P.Mode.MODE2, "1/8"): 2304,
        (P.Mode.MODE2, "1/16"): 2176, (P.Mode.MODE2, "1/32"): 2112,
        (P.Mode.MODE3, "1/4"): 5120, (P.Mode.MODE3, "1/8"): 4608,
        (P.Mode.MODE3, "1/16"): 4352, (P.Mode.MODE3, "1/32"): 4224,
    }
    for (mode, gi), n in expect.items():
        assert bts.mux_tsps_per_frame(mode, gi) == n, (mode, gi)


def test_slot_pattern_counts_and_interleaving():
    pat = bts.slot_pattern(P.Mode.MODE3, "1/8", STD_LAYERS)
    assert len(pat) == 4608
    counts = {"A": 0, "B": 0, None: 0}
    for name, idx in pat:
        counts[name] += 1
    assert counts["A"] == 64 and counts["B"] == 2592
    assert counts[None] == 4608 - 64 - 2592
    # 階層内の通し番号は昇順に現れる（モデル受信機は FIFO）
    for layer in ("A", "B"):
        seq = [idx for name, idx in pat if name == layer]
        assert seq == sorted(seq)
    # A と B はフレーム内で交互に現れる（かたまりで分かれていない）。
    # B の最初と最後の間に A のスロットが挟まっていることを確認する
    first_b = next(i for i, (n, _) in enumerate(pat) if n == "B")
    last_b = max(i for i, (n, _) in enumerate(pat) if n == "B")
    a_between = any(n == "A" for n, _ in pat[first_b:last_b])
    assert a_between, "A と B のスロットがインターリーブされていない"


def test_isdbt_info_roundtrip():
    b = bts.isdbt_info_bytes("B", 1234, frame_head=False, frame_odd=True)
    info = bts.parse_isdbt_info(b)
    assert info["layer"] == "B" and info["tsp_counter"] == 1234
    assert info["frame_odd"] and not info["frame_head"]
    assert info["tmcc_identifier"] == 0b10 and info["ac_invalid"]
    b2 = bts.isdbt_info_bytes(None, 0, frame_head=True, frame_odd=False)
    info2 = bts.parse_isdbt_info(b2)
    assert info2["layer"] == "null" and info2["frame_head"] and not info2["frame_odd"]


def test_rs204_196_parity_is_codeword():
    rng = np.random.default_rng(0)
    blk = rng.integers(0, 256, size=(16, 196), dtype=np.uint8)
    par = bts.rs204_196_parity(blk)
    tsp = np.concatenate([blk, par], axis=1)
    assert bts.rs204_196_check(tsp).all()
    tsp[3, 50] ^= 0xA5
    assert not bts.rs204_196_check(tsp)[3]
    assert bts.rs204_196_check(np.delete(tsp, 3, axis=0)).all()


def test_build_and_split_roundtrip():
    ts_a = make_test_ts(2 * 64, 1)
    ts_b = make_test_ts(2 * 2592, 2)
    out = bts.build_bts({"A": ts_a, "B": ts_b}, P.Mode.MODE3, "1/8", STD_LAYERS)
    assert out.shape == (2 * 4608, 204)
    assert bts.rs204_196_check(out).all()
    # フレーム頭フラグと TSP カウンタ
    sp = bts.split_bts(out)
    assert list(sp["frame_heads"]) == [0, 4608]
    assert sp["tsp_counters"][0] == 0 and sp["tsp_counters"][4607] == 4607
    assert np.array_equal(sp["A"], ts_a)
    assert np.array_equal(sp["B"], ts_b)
    assert sp["nulls"] == 2 * (4608 - 64 - 2592)
    # 偶奇フレーム表示
    infos0 = bts.parse_isdbt_info(out[0, 188:196])
    infos1 = bts.parse_isdbt_info(out[4608, 188:196])
    assert not infos0["frame_odd"] and infos1["frame_odd"]


@pytest.mark.slow
def test_bts_from_loopback_demod():
    """ループバック復調結果 → BTS → 分解で元の TS に戻る（復調器との整合）。"""
    from isdbt.channel import ChannelConfig, apply_channel
    from isdbt.demod import demodulate
    from isdbt.modulator import modulate
    cfg = {"mode": P.Mode.MODE3, "gi": "1/8", "layers": {
        "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 4},
        "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 2}}}
    ts_a = make_test_ts(2 * 64, 3)
    ts_b = make_test_ts(2 * 2592, 4)
    iq = modulate({"A": ts_a, "B": ts_b}, cfg)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=float(P.SAMPLE_RATE_HZ), cn_db=30.0, seed=5))
    out, dbg = demodulate(rx, float(P.SAMPLE_RATE_HZ), return_debug=True)
    # 全階層が揃う共通フレーム範囲に切り出して BTS 化
    tpf = {"A": 64, "B": 2592}
    f0 = max(dbg["first_frame"].values())
    n_common = min(dbg["first_frame"][n] + len(out[n]) // tpf[n] for n in ("A", "B")) - f0
    layer_tsps = {n: out[n][(f0 - dbg["first_frame"][n]) * tpf[n]:][: n_common * tpf[n]] for n in ("A", "B")}
    stream = bts.build_bts(layer_tsps, P.Mode.MODE3, "1/8", STD_LAYERS)
    sp = bts.split_bts(stream)
    # 送信 TS の後半（立ち上がり分を除く）が BTS 経由でも残っていること
    assert ts_a[64:].tobytes() in sp["A"].tobytes()
    assert ts_b[2592:].tobytes() in sp["B"].tobytes()
