"""tools/ts_add_pat.py: ワンセグ TS への PAT 注入。"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from ts_add_pat import ONESEG_PMT_PID, TS_PKT, add_pat, build_pat_packet, crc32_mpeg


def make_pmt_packet(program_number: int) -> bytes:
    sec = bytearray([0x02])                 # table_id = PMT
    body = bytearray()
    body += program_number.to_bytes(2, "big")
    body += bytes([0xC1, 0x00, 0x00])
    body += bytes([0xE1, 0x81])             # PCR PID（ダミー）
    body += bytes([0xF0, 0x00])             # program_info_length 0
    section_length = len(body) + 4
    sec += bytes([0xB0 | (section_length >> 8), section_length & 0xFF])
    sec += body
    sec += crc32_mpeg(bytes(sec)).to_bytes(4, "big")
    pkt = bytearray([0x47, 0x40 | (ONESEG_PMT_PID >> 8), ONESEG_PMT_PID & 0xFF, 0x10, 0x00])
    pkt += sec
    pkt += b"\xFF" * (TS_PKT - len(pkt))
    return bytes(pkt)


def null_packet(i: int) -> bytes:
    return bytes([0x47, 0x1F, 0xFF, 0x10]) + bytes([i & 0xFF]) * (TS_PKT - 4)


def test_crc32_mpeg_known_value():
    # PAT の既知例: CRC を含めた全セクションの CRC32-MPEG を再計算すると 0 になる性質で検査
    sec = bytes.fromhex("00b00d0001c100000001e1f0")
    full = sec + crc32_mpeg(sec).to_bytes(4, "big")
    assert crc32_mpeg(full) == 0


def test_add_pat_roundtrip():
    pn = 0x5678
    src = make_pmt_packet(pn) + b"".join(null_packet(i) for i in range(120))
    out = add_pat(src, interval=50)
    pkts = [out[i:i + TS_PKT] for i in range(0, len(out), TS_PKT)]
    pats = [p for p in pkts if ((p[1] & 0x1F) << 8 | p[2]) == 0]
    assert len(pats) == 3                                   # 121 パケット、50 間隔で 3 枚
    # 元のパケット列は順序そのまま保存される
    rest = b"".join(p for p in pkts if ((p[1] & 0x1F) << 8 | p[2]) != 0)
    assert rest == src
    # PAT の中身: CRC が通り、program_number → 0x1FC8 を指す
    sec = pats[0][5:5 + 16]
    assert sec[0] == 0x00
    length = ((sec[1] & 0x0F) << 8) | sec[2]
    full = sec[: 3 + length]
    assert crc32_mpeg(full) == 0
    got_pn = (full[8] << 8) | full[9]
    got_pid = ((full[10] & 0x1F) << 8) | full[11]
    assert got_pn == pn and got_pid == ONESEG_PMT_PID
    # 連続性カウンタは 0,1,2
    assert [p[3] & 0x0F for p in pats] == [0, 1, 2]


def test_build_pat_packet_size():
    pkt = build_pat_packet(1, 0)
    assert len(pkt) == TS_PKT and pkt[0] == 0x47
