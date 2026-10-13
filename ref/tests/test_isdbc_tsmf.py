"""TSMF（ITU-T J.183）ヘッダの組み立て・解析と多重・分離の試験。"""
from __future__ import annotations

import numpy as np
from isdbc import framing, tsmf
from isdbc import params as P


def test_crc32_mpeg_known_vector():
    assert tsmf.crc32_mpeg(np.frombuffer(b"123456789", dtype=np.uint8)) == 0x0376E6E7


def _ts(n: int, tag: int) -> np.ndarray:
    pk = framing.null_packets(n)
    pk[:, 1] = 0x01
    pk[:, 2] = tag
    pk[:, 4] = np.arange(n) & 0xFF
    return pk


def test_header_roundtrip():
    slot = np.array([1] * 30 + [3] * 20 + [0] * 2)
    ids = {1: (16625, 4), 3: (16592, 4)}
    h = tsmf.build_header(slot, ids, continuity_counter=9, sync_inverted=True, version=5, emergency=1)
    assert h.shape == (188,) and h[0] == 0x47
    assert (((h[1] & 0x1F) << 8) | h[2]) == P.TSMF_HEADER_PID
    p = tsmf.parse_header(h)
    assert p is not None
    assert p.continuity_counter == 9 and p.sync_inverted and p.version == 5 and p.emergency == 1
    assert p.frame_type == P.TSMF_FRAME_TYPE
    assert p.active_ts() == [(1, 16625, 4), (3, 16592, 4)]
    assert np.array_equal(p.slot_ts, slot)
    # 非反転同期語
    assert tsmf.parse_header(tsmf.build_header(slot, ids)).sync_inverted is False


def test_header_rejects_corruption():
    h = tsmf.build_header(np.ones(52, dtype=int), {1: (1, 1)})
    bad = h.copy()
    bad[100] ^= 1                       # private_data の 1 ビット → CRC 不一致
    assert tsmf.parse_header(bad) is None
    bad = h.copy()
    bad[5] ^= 0x01                      # 同期語
    assert tsmf.parse_header(bad) is None
    assert tsmf.parse_header(framing.null_packets(1)[0]) is None


def test_multiplex_and_demultiplex():
    s1, s2 = _ts(300, 1), _ts(60, 2)
    ids = {1: (16625, 4), 2: (16592, 4)}
    mux = tsmf.multiplex({1: s1, 2: s2}, ids)
    assert mux.shape[0] % P.TSMF_SLOTS == 0
    assert all(tsmf.parse_header(mux[i]) is not None for i in range(0, mux.shape[0], P.TSMF_SLOTS))

    out, info = tsmf.demultiplex(mux, 2)
    assert np.array_equal(out[:60], s2) and info.selected == 2 and info.bad_headers == 0
    assert info.streams == [(1, 16625, 4), (2, 16592, 4)]
    out, info = tsmf.demultiplex(mux, (4, 16625))
    assert np.array_equal(out[:300], s1) and info.selected == 1
    out, info = tsmf.demultiplex(mux, (None, 16592))
    assert np.array_equal(out[:60], s2)
    out, info = tsmf.demultiplex(mux, "auto")
    assert info.selected == 1 and np.array_equal(out[:300], s1)
    out, info = tsmf.demultiplex(mux, "all")
    assert out.shape == mux.shape
    # 存在しない相対 TS 番号 → 空
    out, info = tsmf.demultiplex(mux, 7)
    assert out.shape[0] == 0
    # TSMF でない単一 TS はそのまま
    out, info = tsmf.demultiplex(s1, "auto")
    assert np.array_equal(out, s1) and info.frames == 0


def test_demultiplex_resyncs_after_lost_header():
    s1 = _ts(500, 1)
    mux = tsmf.multiplex({1: s1}, {1: (100, 4)})
    broken = mux.copy()
    broken[P.TSMF_SLOTS * 2, 5] ^= 0xFF          # 3 フレーム目のヘッダを壊す
    out, info = tsmf.demultiplex(broken, 1)
    assert info.bad_headers == 1
    # 壊れたフレームのスロットは前ヘッダの割当で読まれるが、次のヘッダから復帰する
    assert out.shape[0] >= 500 - 52
