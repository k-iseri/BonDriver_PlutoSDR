"""fec.py（RS 符号化、エネルギー拡散、バイトインターリーブ、畳み込み符号）の単体テスト。"""
import numpy as np
import pytest
from isdbt import fec
from isdbt import params as P


@pytest.fixture
def rng():
    return np.random.default_rng(12345)


def test_rs_encode_syndromes_zero(rng):
    pk = rng.integers(0, 256, (40, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    c = fec.rs_encode(pk)
    assert c.shape == (40, 204)
    assert np.array_equal(c[:, :188], pk)
    assert (fec.rs_syndromes(c) == 0).all()


def test_rs_detects_errors(rng):
    pk = rng.integers(0, 256, (5, 188), dtype=np.uint8)
    c = fec.rs_encode(pk)
    c[2, 100] ^= 0x01
    s = fec.rs_syndromes(c)
    assert (s[[0, 1, 3, 4]] == 0).all()
    assert (s[2] != 0).any()


def test_rs_generator_poly():
    g = fec.rs_generator_poly()
    assert len(g) == 17 and g[0] == 1
    # 全ゼロの入力はパリティも全ゼロ
    assert (fec.rs_encode(np.zeros((1, 188), np.uint8))[0, 188:] == 0).all()


def test_energy_dispersal_prbs_known_sequence():
    # x^15+x^14+1、初期値 100101010000000 の先頭は DVB と同じ 03 F6 08 34 30 B8 A3 93
    assert list(fec.energy_dispersal_prbs(8)) == [0x03, 0xF6, 0x08, 0x34, 0x30, 0xB8, 0xA3, 0x93]


def test_energy_dispersal_frame_layout_and_inverse(rng):
    tsp = rng.integers(0, 256, (10, 204), dtype=np.uint8)
    tsp[:, 0] = 0x47
    d = fec.energy_dispersal_frame(tsp)
    # 同期バイトは末尾にそのまま
    assert (d[:, 203] == 0x47).all()
    # 先頭バイトは PRBS の最初のバイト 0x03 で XOR されている
    assert d[0, 0] == tsp[0, 1] ^ 0x03
    # 逆変換（PRBS は同期バイト分も進む）
    prbs = fec.energy_dispersal_prbs(10 * 204).reshape(10, 204)
    back = np.empty_like(tsp)
    back[:, 0] = d[:, 203]
    back[:, 1:] = d[:, :203] ^ prbs[:, :203]
    assert np.array_equal(back, tsp)


def test_energy_dispersal_resets_every_frame(rng):
    tsp = rng.integers(0, 256, (6, 204), dtype=np.uint8)
    d = fec.energy_dispersal_layer(tsp, tsp_per_frame=3)
    assert np.array_equal(d[:3], fec.energy_dispersal_frame(tsp[:3]))
    assert np.array_equal(d[3:], fec.energy_dispersal_frame(tsp[3:]))


def test_byte_interleave_structure_and_inverse(rng):
    s = rng.integers(0, 256, (60, 204), dtype=np.uint8)
    bi = fec.byte_interleave(s)
    # バイト j はブランチ j mod 12、先読み形: out[t][j] = in[t + 11 - (j mod 12)][j]
    for t in (0, 7, 20):
        for j in (0, 1, 11, 12, 13, 203):
            assert bi[t, j] == s[t + 11 - (j % 12), j]
    bd = fec.byte_deinterleave(bi)
    assert np.array_equal(bd[11:], s[11:])      # 先頭 11 TSP は不定、それ以降は恒等
    assert not np.array_equal(bi, s)


def test_conv_encode_against_reference(rng):
    """K=7 (171,133)。参照は Karn 流（レジスタ bit0 = 現在入力、多項式 0x4F/0x6D）で独立に計算。"""
    bits = rng.integers(0, 2, 500, dtype=np.uint8)
    x, y = fec.conv_encode(bits)
    sr = 0
    xr, yr = [], []
    for b in bits:
        sr = ((sr << 1) | int(b)) & 0x7F
        xr.append((sr & 0x4F).bit_count() & 1)
        yr.append((sr & 0x6D).bit_count() & 1)
    assert np.array_equal(x, xr) and np.array_equal(y, yr)


def test_conv_encode_impulse_response():
    # 単位インパルス → X = 1111001（171(8)）、Y = 1011011（133(8)）
    u = np.zeros(10, dtype=np.uint8)
    u[0] = 1
    x, y = fec.conv_encode(u)
    assert "".join(map(str, x[:7])) == "1111001"
    assert "".join(map(str, y[:7])) == "1011011"


@pytest.mark.parametrize("rate", list(P.PUNCTURE_PATTERNS))
def test_puncture_rate_and_order(rate):
    period = len(P.PUNCTURE_PATTERNS[rate][0])
    n = period * 4
    x = np.arange(1, n + 1)
    y = np.arange(101, n + 101)
    out = fec.puncture(x, y, rate)
    assert len(out) * P.CODE_RATES[rate] == n
    # 先頭は必ず X1 Y1
    assert out[0] == 1 and out[1] == 101
    if rate == "3/4":
        assert list(out[:4]) == [1, 101, 102, 3]          # X1 Y1 Y2 X3
    if rate == "2/3":
        assert list(out[:3]) == [1, 101, 102]             # X1 Y1 Y2
    if rate == "7/8":
        assert list(out[:8]) == [1, 101, 102, 103, 104, 5, 106, 7]


def test_bytes_to_bits_msb_first():
    assert list(fec.bytes_to_bits(np.array([0x80, 0x01], np.uint8))) == [1, 0, 0, 0, 0, 0, 0, 0,
                                                                          0, 0, 0, 0, 0, 0, 0, 1]


# -------------------------------------------------------------------------
# 復号側
# -------------------------------------------------------------------------
@pytest.mark.parametrize("rate", list(P.PUNCTURE_PATTERNS))
def test_depuncture_inverts_puncture(rate, rng):
    period = len(P.PUNCTURE_PATTERNS[rate][0])
    n = period * 20
    x = rng.integers(0, 2, n, dtype=np.uint8)
    y = rng.integers(0, 2, n, dtype=np.uint8)
    soft = 1.0 - 2.0 * fec.puncture(x, y, rate).astype(np.float32)
    pairs = fec.depuncture(soft, rate)
    xm, ym = P.PUNCTURE_PATTERNS[rate]
    for i in range(n):
        assert pairs[i, 0] == ((1 - 2 * int(x[i])) if xm[i % period] == "1" else 0)
        assert pairs[i, 1] == ((1 - 2 * int(y[i])) if ym[i % period] == "1" else 0)


@pytest.mark.parametrize("rate", ["1/2", "2/3", "3/4", "5/6", "7/8"])
def test_viterbi_decodes_noisy_stream(rate, rng):
    period = len(P.PUNCTURE_PATTERNS[rate][0])
    bits = rng.integers(0, 2, 30_000 - 30_000 % period, dtype=np.uint8)
    coded = fec.conv_encode_punctured(bits, rate)
    soft = 1.0 - 2.0 * coded.astype(np.float32) + rng.normal(0, 0.3, len(coded)).astype(np.float32)
    dec = fec.viterbi_decode(fec.depuncture(soft, rate), block_len=1024)
    assert np.array_equal(dec, bits)


def test_viterbi_handles_segment_boundaries(rng):
    bits = rng.integers(0, 2, 20_000, dtype=np.uint8)
    coded = fec.conv_encode_punctured(bits, "1/2")
    soft = 1.0 - 2.0 * coded.astype(np.float32)
    dec = fec.viterbi_decode(fec.depuncture(soft, "1/2"), block_len=512, segment_len=3000)
    assert np.array_equal(dec, bits)


def test_energy_descramble_inverts_dispersal(rng):
    tsp = rng.integers(0, 256, (12, 204), dtype=np.uint8)
    tsp[:, 0] = 0x47
    d = fec.energy_dispersal_layer(tsp, tsp_per_frame=4)
    assert np.array_equal(fec.energy_descramble_layer(d, tsp_per_frame=4), tsp)
