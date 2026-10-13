"""ISDB-C（J.83 Annex C）のバイト→シンボル変換・差動符号化・64QAM 配置の試験。

配置の正解は docs/isdbc-spec-notes.md 3 章（J.83 図 C.7 / EN 300 429 図 7 から転記）。
実放送（ケーブル 56ch、TS_id 16625）で RS 誤り 0 で復号できることが最終確認（test_isdbc_real.py）。
"""
from __future__ import annotations

import numpy as np
import pytest
from isdbc import mapping


def test_mtuple_roundtrip():
    rng = np.random.default_rng(0)
    data = rng.integers(0, 256, size=3 * 777, dtype=np.uint8)
    words = mapping.bytes_to_mtuples(data)
    assert words.shape == (4 * 777,) and words.max() < 64
    # 先頭バイト 0b10110011 の上位 6 ビットが最初の語（MSB ファースト、EN 300 429 図 5）
    assert mapping.bytes_to_mtuples(np.array([0b10110011, 0, 0], np.uint8))[0] == 0b101100
    assert np.array_equal(mapping.mtuples_to_bytes(words), data)


def test_diff_encode_matches_boolean_formula():
    rng = np.random.default_rng(1)
    words = rng.integers(0, 64, size=5000, dtype=np.uint8)
    enc = mapping.diff_encode(words)
    i_prev, q_prev = 0, 0
    want = []
    for w in words:
        a, b = (w >> 5) & 1, (w >> 4) & 1
        i_prev, q_prev = mapping.diff_encode_step(a, b, i_prev, q_prev)
        want.append((i_prev << 5) | (q_prev << 4) | (w & 0x0F))
    assert np.array_equal(enc, np.array(want, dtype=np.uint8))
    assert np.array_equal(mapping.diff_decode(enc), words)


@pytest.mark.parametrize("k", [1, 2, 3])
def test_rotation_invariance(k):
    """受信コンスタレーションが π/2 の k 倍回っていても差動復号結果は変わらない（先頭 1 シンボルを除く）。"""
    rng = np.random.default_rng(2)
    words = rng.integers(0, 64, size=2000, dtype=np.uint8)
    enc = mapping.diff_encode(words)
    rotated = mapping.map_symbols(enc) * (1j ** k)
    got = mapping.diff_decode(mapping.demap_hard(rotated))
    assert np.array_equal(got[1:], words[1:])


def test_constellation_layout():
    c = mapping.CONSTELLATION
    assert len(set(c.tolist())) == 64
    assert np.all(np.abs(c.real) % 2 == 1) and np.all(np.abs(c.imag) % 2 == 1)
    assert np.array_equal(mapping.demap_hard(c), np.arange(64))
    # 第 1 象限（I_k Q_k = 00）の 4 LSB: 図 C.7 の行（Q = 1,3,5,7）× 列（I = 1,3,5,7）
    rows = {1: [0b0000, 0b0001, 0b0101, 0b0100],
            3: [0b0010, 0b0011, 0b0111, 0b0110],
            5: [0b1010, 0b1011, 0b1111, 0b1110],
            7: [0b1000, 0b1001, 0b1101, 0b1100]}
    for q, labels in rows.items():
        for i, lsb in zip((1, 3, 5, 7), labels):
            assert c[lsb] == complex(i, q), (q, i, lsb)
    # 他象限は回転（表 1）: 10 = +π/2、11 = +π、01 = +3π/2。図 C.7 の読み取り例で確認
    assert c[0b101000] == complex(-7, 1)
    assert c[0b110100] == complex(-7, -1)
    assert c[0b010000] == complex(1, -1)
    # 象限内で隣り合う点は 1 ビットしか違わない（Gray）
    for q in (1, 3, 5, 7):
        for i in (1, 3, 5):
            a = mapping.demap_hard(np.array([complex(i, q)]))[0]
            b = mapping.demap_hard(np.array([complex(i + 2, q)]))[0]
            assert int(a ^ b).bit_count() == 1


def test_mer_db():
    rng = np.random.default_rng(3)
    z = mapping.CONSTELLATION[rng.integers(0, 64, size=20000)]
    assert mapping.mer_db(z) > 100
    noise = (rng.normal(size=z.shape) + 1j * rng.normal(size=z.shape)) * np.sqrt(42 / 2 / 10 ** (30 / 10))
    assert abs(mapping.mer_db(z + noise) - 30.0) < 0.5
