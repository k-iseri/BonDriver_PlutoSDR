"""params.py の整合性検査。

仕様値の転記ミスを機械的に捕まえるためのテスト群。
VERIFY/未転記（None）の項目は skip にし、転記後に skip を外す。
"""
from fractions import Fraction

import numpy as np
import pytest
from isdbt import params as P
from isdbt.tables import carrier_randomize as CR


def test_sample_rate():
    assert P.SAMPLE_RATE_HZ == Fraction(512, 63) * 1_000_000
    assert abs(float(P.SAMPLE_RATE_HZ) - 8_126_984.127) < 1.0


@pytest.mark.parametrize("mode", list(P.Mode))
def test_carrier_counts(mode):
    mp = P.MODE_PARAMS[mode]
    # 13 セグメント + 右端 CP 1 本
    assert mp.total_carriers == mp.carriers_per_segment * P.N_SEGMENTS + 1
    # 同期変調セグメント内の内訳
    assert (mp.data_carriers_per_segment + mp.sp_per_segment
            + mp.tmcc_per_segment + mp.ac1_per_segment) == mp.carriers_per_segment
    # SP は 12 本に 1 本
    assert mp.sp_per_segment == mp.carriers_per_segment // P.SP_SPACING
    # Mode 間の倍々関係
    assert mp.fft_size == 2048 * 2 ** (mode.value - 1)
    assert mp.carriers_per_segment == 108 * 2 ** (mode.value - 1)


def test_mode3_timing():
    mp = P.MODE_PARAMS[P.Mode.MODE3]
    assert abs(float(mp.carrier_spacing_hz) - 992.06) < 0.01
    assert abs(float(mp.useful_symbol_s) * 1e6 - 1008.0) < 0.01
    gi = P.GUARD_INTERVALS["1/8"]
    total = float(mp.useful_symbol_s * (1 + gi))
    assert abs(total * 1e6 - 1134.0) < 0.01
    frame = total * P.SYMBOLS_PER_FRAME
    assert abs(frame * 1e3 - 231.336) < 0.001
    assert P.guard_samples(P.Mode.MODE3, "1/8") == 1024
    assert P.symbol_samples(P.Mode.MODE3, "1/8") == 9216


def test_segment_order():
    assert sorted(P.SEGMENT_ORDER_IN_FREQ) == list(range(13))
    assert P.SEGMENT_ORDER_IN_FREQ[6] == 0  # ワンセグは中央
    assert P.SEGMENT_FREQ_INDEX[0] == 6 and P.SEGMENT_FREQ_INDEX[11] == 0


def test_carrier_to_fft_bin():
    # 中央キャリアが DC、左端は負周波数（N + bin）
    assert P.carrier_to_fft_bin(P.Mode.MODE3, 2808) == 0
    assert P.carrier_to_fft_bin(P.Mode.MODE3, 0) == 8192 - 2808
    assert P.carrier_to_fft_bin(P.Mode.MODE3, 5616) == 2808


def test_sp_pattern_covers_all_mod3_positions():
    covered = set()
    for n in range(P.SP_SYMBOL_PERIOD):
        for k in range(12):
            if P.is_sp_position(k, n):
                covered.add(k)
    assert covered == {0, 3, 6, 9}


def test_time_interleave_delay_formula():
    # 仕様例: キャリア 0 は遅延 0、I=2 で最大遅延は 2*95
    assert P.time_interleave_delay(0, 2) == 0
    assert max(P.time_interleave_delay(i, 2) for i in range(384)) == 2 * 95


@pytest.mark.parametrize("I,adj", [(0, 0), (1, 109), (2, 14), (4, 28), (8, 56), (16, 112)])
def test_time_interleave_adjust(I, adj):
    assert P.time_interleave_adjust(I) == adj
    assert (95 * I + adj) % P.SYMBOLS_PER_FRAME == 0


@pytest.mark.parametrize("rate", list(P.PUNCTURE_PATTERNS))
def test_puncture_pattern_rate(rate):
    x, y = P.PUNCTURE_PATTERNS[rate]
    assert len(x) == len(y)
    sent = x.count("1") + y.count("1")
    assert Fraction(len(x), sent) == Fraction(rate)


@pytest.mark.parametrize("mode", list(P.Mode))
@pytest.mark.parametrize("mod", [P.Modulation.QPSK, P.Modulation.QAM16, P.Modulation.QAM64])
@pytest.mark.parametrize("rate", list(P.CODE_RATES))
def test_tsp_per_frame_integer(mode, mod, rate):
    for nseg in (1, 12, 13):
        n = P.tsp_per_frame(mode, mod, rate, nseg)
        assert n > 0
    # 日本の標準運用の既知値
    assert P.tsp_per_frame(P.Mode.MODE3, P.Modulation.QPSK, "2/3", 1) == 64
    assert P.tsp_per_frame(P.Mode.MODE3, P.Modulation.QAM64, "3/4", 12) == 12 * 216


def test_bit_interleave_adjust_known_values():
    # STD-B31 表（N=1）: Mode3 QPSK 1296, 16QAM 2592, 64QAM 3888 / Mode1 QPSK 144
    assert P.bit_interleave_adjust_bits(P.Mode.MODE3, P.Modulation.QPSK, 1) == 1296
    assert P.bit_interleave_adjust_bits(P.Mode.MODE3, P.Modulation.QAM16, 1) == 2592
    assert P.bit_interleave_adjust_bits(P.Mode.MODE3, P.Modulation.QAM64, 1) == 3888
    assert P.bit_interleave_adjust_bits(P.Mode.MODE1, P.Modulation.QPSK, 1) == 144
    # 遅延調整 + 最大遅延 120 = 2 シンボル
    for nseg in (1, 12):
        adj = P.bit_interleave_adjust_bits(P.Mode.MODE3, P.Modulation.QAM64, nseg)
        assert adj // 6 + 120 == 2 * nseg * 384


@pytest.mark.parametrize("mode", list(P.Mode))
def test_tmcc_positions_coherent(mode):
    tbl = P.TMCC_POSITIONS_COHERENT[mode]
    if tbl is None:
        pytest.skip(f"{mode}: TMCC 位置表が未転記（VERIFY）")
    mp = P.MODE_PARAMS[mode]
    assert len(tbl) == P.N_SEGMENTS
    for seg in tbl:
        assert len(seg) == mp.tmcc_per_segment
        for k in seg:
            assert 0 <= k < mp.carriers_per_segment
            # SP が 4 シンボル周期で k ≡ 0 (mod 3) を全て占めるため、TMCC は mod 3 != 0 でなければならない
            assert k % 3 != 0, f"TMCC position {k} collides with SP pattern"
    assert len(set(P.TMCC_POSITIONS_ABS[mode])) == mp.tmcc_per_segment * P.N_SEGMENTS


@pytest.mark.parametrize("mode", list(P.Mode))
def test_ac1_positions_coherent(mode):
    tbl = P.AC1_POSITIONS_COHERENT[mode]
    if tbl is None:
        pytest.skip(f"{mode}: AC1 位置表が未転記（VERIFY）")
    mp = P.MODE_PARAMS[mode]
    tm = P.TMCC_POSITIONS_COHERENT[mode]
    assert len(tbl) == P.N_SEGMENTS
    for s, seg in enumerate(tbl):
        assert len(seg) == mp.ac1_per_segment
        for k in seg:
            assert k % 3 != 0
            if tm is not None:
                assert k not in tm[s]


@pytest.mark.parametrize("mode,n_tmcc,n_ac2", [(P.Mode.MODE1, 5, 4), (P.Mode.MODE2, 10, 9), (P.Mode.MODE3, 20, 19)])
def test_differential_positions(mode, n_tmcc, n_ac2):
    """表 3-14: 差動変調部の CP/AC1/AC2/TMCC。重複せず、データキャリア数が同期変調部と同じになる。"""
    mp = P.MODE_PARAMS[mode]
    tm, ac2, ac1 = P.TMCC_POSITIONS_DIFFERENTIAL[mode], P.AC2_POSITIONS_DIFFERENTIAL[mode], P.AC1_POSITIONS_COHERENT[mode]
    assert P.CP_POSITIONS_DIFFERENTIAL[mode] == 0
    for f in range(P.N_SEGMENTS):
        assert len(tm[f]) == n_tmcc and len(ac2[f]) == n_ac2
        used = {0} | set(tm[f]) | set(ac2[f]) | set(ac1[f])
        assert len(used) == 1 + n_tmcc + n_ac2 + mp.ac1_per_segment
        assert max(used) < mp.carriers_per_segment
        assert mp.carriers_per_segment - len(used) == mp.data_carriers_per_segment


# 表 3-16: Mode 3 の各セグメント先頭での PRBS レジスタ初期値（D1..D11、周波数順 11,9,...,12）
_TABLE_3_16_MODE3 = {
    11: "11111111111",
    9: "11011100101",
    7: "10010100000",
    5: "01110001001",
    3: "00100011001",
    1: "11100110110",
    0: "00100001011",
    2: "11100111101",
    4: "01101010011",
    6: "10111010010",
    8: "01100010010",
    10: "11110100101",
    12: "00010011100",
}


def test_pilot_prbs_matches_table_3_16():
    from isdbt import pilots
    mp = P.MODE_PARAMS[P.Mode.MODE3]
    reg = P.PILOT_PRBS_INIT
    for k in range(mp.total_carriers - 1):          # 右端 CP は除く
        if k % mp.carriers_per_segment == 0:
            seg = P.SEGMENT_ORDER_IN_FREQ[k // mp.carriers_per_segment]
            d1_to_d11 = "".join(str((reg >> (10 - j)) & 1) for j in range(11))
            assert d1_to_d11 == _TABLE_3_16_MODE3[seg], seg
        new = ((reg >> 2) ^ reg) & 1
        reg = (reg >> 1) | (new << 10)
    # 表 3-19: 右端 CP は Mode 1 で -4/3、Mode 2/3 で +4/3
    assert pilots.pilot_values(P.Mode.MODE1)[-1] < 0
    assert pilots.pilot_values(P.Mode.MODE2)[-1] > 0
    assert pilots.pilot_values(P.Mode.MODE3)[-1] > 0
    assert np.array_equal(pilots.pilot_prbs(P.Mode.MODE3)[:11], [1] * 11)


def test_center_segment_known_positions_mode3():
    """ワンセグ受信機で広く知られる Mode 3 セグメント 0 の TMCC / AC1 位置。"""
    f = P.SEGMENT_FREQ_INDEX[0]
    assert P.TMCC_POSITIONS_COHERENT[P.Mode.MODE3][f] == (101, 131, 286, 349)
    assert P.AC1_POSITIONS_COHERENT[P.Mode.MODE3][f] == (7, 89, 206, 209, 226, 244, 377, 407)


@pytest.mark.parametrize("mode,tbl,n", [
    (P.Mode.MODE1, CR.MODE1, 96),
    (P.Mode.MODE2, CR.MODE2, 192),
    (P.Mode.MODE3, CR.MODE3, 384),
])
def test_carrier_randomize_is_permutation(mode, tbl, n):
    if tbl is None:
        pytest.skip(f"{mode}: キャリアランダマイズ表が未転記")
    assert sorted(tbl) == list(range(n))
    assert P.CARRIER_RANDOMIZE_TABLE[mode] == tuple(tbl)
    assert P.MODE_PARAMS[mode].data_carriers_per_segment == n


def test_constellation_mapping():
    q = P.constellation_table(P.Modulation.QPSK) * np.sqrt(2)
    assert np.allclose(q, [1 + 1j, 1 - 1j, -1 + 1j, -1 - 1j])
    c64 = P.constellation_table(P.Modulation.QAM64) * np.sqrt(42)
    # b0 b1 b2 b3 b4 b5 = 000000 → (7,7)、000001 → (7,5)、001000 → (1,7)、001100 → (1,1)
    assert np.allclose(c64[[0, 1, 8, 12]], [7 + 7j, 7 + 5j, 1 + 7j, 1 + 1j])
    c16 = P.constellation_table(P.Modulation.QAM16) * np.sqrt(10)
    # b0 b1 b2 b3 = 0000 → (3,3)、0001 → (3,1)、0010 → (1,3)、0100 → (3,-3)、1111 → (-1,-1)
    assert np.allclose(c16[[0, 1, 2, 4, 15]], [3 + 3j, 3 + 1j, 1 + 3j, 3 - 3j, -1 - 1j])
    for mod in (P.Modulation.QPSK, P.Modulation.QAM16, P.Modulation.QAM64):
        t = P.constellation_table(mod)
        assert abs(np.mean(np.abs(t) ** 2) - 1.0) < 1e-6   # 正規化で平均電力 1
        assert len(set(np.round(t, 6))) == len(t)           # 全点が異なる


def test_tmcc_layout():
    assert P.TMCC_SYNC_WORD_W1 == (~P.TMCC_SYNC_WORD_W0) & 0xFFFF
    info_lo, info_hi = P.TMCC_INFO_BITS
    par_lo, par_hi = P.TMCC_PARITY_BITS
    assert info_hi - info_lo + 1 == 102
    assert par_hi - par_lo + 1 == 82
    assert par_hi == P.TMCC_BITS_PER_FRAME - 1
    assert max(P.TMCC_DSCC_GENERATOR_EXPONENTS) == 82
    total = sum(w for _, w in P.TMCC_FIELDS.values())
    assert total == 102
    assert sum(w for _, w in P.TMCC_LAYER_FIELDS.values()) == 13
    assert P.tmcc_interleave_code(P.Mode.MODE3, 4) == 3
    assert P.tmcc_interleave_code(P.Mode.MODE1, 16) == 3
    assert P.tmcc_interleave_from_code(P.Mode.MODE2, 2) == 4


def test_rs_params():
    assert P.RS_N - P.RS_K == P.RS_NROOTS
    assert P.RS_PRIM_POLY == 0b1_0001_1101
