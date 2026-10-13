"""ISDB-C のフレーム構成（J.83 C.5）: 同期反転・ランダマイズ・RS・バイトインターリーブと受信側の試験。"""
from __future__ import annotations

import numpy as np
from isdbc import framing
from isdbc import params as P


def _packets(n: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pk = framing.null_packets(n)
    pk[:, 1] = 0x01
    pk[:, 2] = 0x00
    pk[:, 3] = 0x10 | (np.arange(n) % 16)
    pk[:, 4:] = rng.integers(0, 256, size=(n, 184), dtype=np.uint8)
    return pk


def test_prbs_and_scramble_roundtrip():
    # PRBS 先頭は DVB / ISDB-T と同じ 0x03 0xF6 0x08 0x34（初期値 100101010000000）
    assert list(framing._PRBS[:4]) == [0x03, 0xF6, 0x08, 0x34]
    assert len(framing._PRBS) == 1503
    pk = _packets(32, 0)
    s = framing.scramble(pk)
    assert np.all(s[::8, 0] == P.SYNC_BYTE_INVERTED) and np.all(s[1::8, 0] == P.SYNC_BYTE)
    assert not np.array_equal(s[:, 1:], pk[:, 1:])
    assert np.array_equal(framing.descramble(s, 0), pk)
    # 途中から始まる場合はグループ位相を与える
    assert np.array_equal(framing.descramble(s[3:], 3), pk[3:])


def test_encode_decode_roundtrip():
    pk = _packets(96, 1)
    stream = framing.encode_ts(pk)
    assert stream.shape == (96 * P.RS_N,)
    junk = np.random.default_rng(2).integers(0, 256, size=123, dtype=np.uint8)
    res = framing.decode_stream(np.concatenate([junk, stream]))
    assert res["sync_offset"] == 123
    assert res["sync_ratio"] > 0.8
    got = res["packets"]
    n = got.shape[0]
    assert n >= 96 - 12
    assert np.array_equal(got, pk[P.INTERLEAVE_I - 1: P.INTERLEAVE_I - 1 + n])
    assert np.all(res["rs_err"] == 0)


def test_rs_corrects_up_to_8_bytes():
    """伝送 TSP の同じバイト位置を全 TSP で壊すと、デインターリーブ後も各 TSP の誤りはその位置数のまま。
    8 バイトまでは RS が直し、9 バイトでは訂正不能になる。"""
    pk = _packets(64, 3)
    rng = np.random.default_rng(4)
    for n_err, ok in ((8, True), (9, False)):
        stream = framing.encode_ts(pk).reshape(-1, P.RS_N)
        cols = rng.choice(np.arange(1, P.RS_N), size=n_err, replace=False)      # 同期バイトは避ける
        stream[:, cols] ^= rng.integers(1, 256, size=(stream.shape[0], n_err)).astype(np.uint8)
        res = framing.decode_stream(stream.ravel())
        got = res["packets"]
        n = got.shape[0]
        if ok:
            assert np.all(res["rs_err"] == n_err)
            assert np.array_equal(got, pk[P.INTERLEAVE_I - 1: P.INTERLEAVE_I - 1 + n])
        else:
            assert np.all(res["rs_err"] == -1)
            assert np.all(got[:, 1] & framing.TEI_MASK)


def test_uncorrectable_sets_tei():
    pk = _packets(64, 5)
    stream = framing.encode_ts(pk).copy()
    # 連続 40 バイトのバースト（12 ブランチに散るが 1 TSP あたり 3〜4 バイト）→ 直る
    stream[2000:2040] ^= 0xFF
    assert np.all(framing.decode_stream(stream)["rs_err"] >= 0)
    # 連続 200 バイト → 1 TSP あたり 16 バイト超 → 訂正不能
    stream[5000:5200] ^= 0xFF
    res = framing.decode_stream(stream)
    bad = res["rs_err"] < 0
    assert bad.any()
    assert np.all(res["packets"][bad, 1] & framing.TEI_MASK)
    assert not np.any(res["packets"][~bad, 1] & framing.TEI_MASK)


def test_sync_score_on_noise():
    rng = np.random.default_rng(6)
    _, ratio = framing.sync_score(rng.integers(0, 256, size=204 * 200, dtype=np.uint8))
    assert ratio < 0.1
