"""tmcc.py の単体テスト（生成 → 解析の往復、パリティ、同期語）。"""
import numpy as np
from isdbt import params as P
from isdbt import tmcc


def _info():
    return tmcc.TmccInfo(
        partial_reception=True,
        layers={"A": tmcc.TmccLayer(P.Modulation.QPSK, "2/3", 4, 1),
                "B": tmcc.TmccLayer(P.Modulation.QAM64, "3/4", 2, 12)},
    )


def test_frame_bits_layout():
    b0 = tmcc.build_frame_bits(P.Mode.MODE3, _info(), 0)
    b1 = tmcc.build_frame_bits(P.Mode.MODE3, _info(), 1)
    assert len(b0) == 204
    assert tmcc.sync_word_of(b0) == 0 and tmcc.sync_word_of(b1) == 1
    assert "".join(map(str, b0[1:17])) == "0011010111101110"
    assert "".join(map(str, b0[17:20])) == "000"        # 同期変調（表 3-20: 差動 111 / 同期 000）
    assert np.array_equal(b0[20:], b1[20:])             # 情報部とパリティは同じ
    assert tmcc.dscc_check(b0[20:204])


def test_parity_detects_single_error():
    b = tmcc.build_frame_bits(P.Mode.MODE3, _info(), 0)
    for pos in (20, 77, 121, 122, 203):
        c = b.copy()
        c[pos] ^= 1
        assert not tmcc.dscc_check(c[20:204])


def test_info_roundtrip():
    info = _info()
    bits = tmcc.encode_info(P.Mode.MODE3, info)
    assert len(bits) == 102
    back = tmcc.parse_info(P.Mode.MODE3, bits)
    assert back.system_id == 0
    assert back.switching_indicator == 0b1111
    assert back.emergency_alarm == 0
    assert back.partial_reception is True
    assert back.layers["A"] == info.layers["A"]
    assert back.layers["B"] == info.layers["B"]
    assert back.layers["C"] is None
    assert back.next_layers["A"] == info.layers["A"]
    assert back.phase_correction == 0b111 and back.reserved == 0xFFF


def test_layer_field_codes():
    # 階層 A: QPSK(001) 2/3(001) I=4@Mode3(011) 1 セグメント(0001)
    bits = tmcc.encode_info(P.Mode.MODE3, _info())
    cur = bits[7:47]
    assert cur[0] == 1                                                   # 部分受信フラグ
    assert "".join(map(str, cur[1:14])) == "001" + "001" + "011" + "0001"
    assert "".join(map(str, cur[14:27])) == "011" + "010" + "010" + "1100"  # 64QAM 3/4 I=2 12 セグ
    assert "".join(map(str, cur[27:40])) == "1" * 13                        # 未使用階層
    # Mode 1 で I=4 は符号 001
    info1 = tmcc.TmccInfo(layers={"A": tmcc.TmccLayer(P.Modulation.QPSK, "1/2", 4, 13)})
    b1 = tmcc.encode_info(P.Mode.MODE1, info1)
    assert "".join(map(str, b1[8:21])) == "001" + "000" + "001" + "1101"


def test_differential_encode():
    b = np.array([0, 1, 1, 0, 1], dtype=np.uint8)
    d = tmcc.differential_encode(b)
    assert list(d) == [0, 1, 0, 0, 1]
    # 復号: B_n = D_n xor D_{n-1}
    assert list(d[1:] ^ d[:-1]) == list(b[1:])
