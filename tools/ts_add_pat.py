#!/usr/bin/env python3
"""ワンセグ（部分受信階層）TS に PAT を注入して一般プレイヤーで再生可能にする。

部分受信階層には PAT が多重されないことがある（ワンセグ受信機は固定 PMT PID 0x1FC8
を直接参照する運用）。VLC 等は PAT が無いとサービスを認識しないため、
TS 中の PMT（PID 0x1FC8）から program_number を読み取り、それを指す PAT を
一定間隔で挿入する。既存パケットは一切変更しない（純粋な多重追加）。

使い方:
    python tools/ts_add_pat.py in_layerA.ts out_with_pat.ts [--interval 50]
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

TS_PKT = 188
SYNC = 0x47
ONESEG_PMT_PID = 0x1FC8


def crc32_mpeg(data: bytes) -> int:
    """MPEG-2 PSI の CRC32（多項式 0x04C11DB7、初期値 0xFFFFFFFF、反転なし）。"""
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= b << 24
        for _ in range(8):
            crc = ((crc << 1) ^ 0x04C11DB7) if crc & 0x80000000 else (crc << 1)
            crc &= 0xFFFFFFFF
    return crc


def find_program_number(ts: memoryview) -> int:
    """PMT（PID 0x1FC8、table_id 2）のセクションヘッダから program_number を読む。"""
    for off in range(0, len(ts) - TS_PKT + 1, TS_PKT):
        pkt = ts[off:off + TS_PKT]
        if pkt[0] != SYNC:
            continue
        pid = ((pkt[1] & 0x1F) << 8) | pkt[2]
        if pid != ONESEG_PMT_PID or not (pkt[1] & 0x40):   # payload_unit_start のみ
            continue
        if pkt[1] & 0x80:                                   # TEI は信用しない
            continue
        p = 4
        if pkt[3] & 0x20:                                   # adaptation field
            p += 1 + pkt[4]
        p += 1 + pkt[p]                                     # pointer_field
        if p + 5 > TS_PKT or pkt[p] != 0x02:                # table_id = PMT
            continue
        return (pkt[p + 3] << 8) | pkt[p + 4]
    raise SystemExit(f"PMT (PID {ONESEG_PMT_PID:#06x}) が見つからない（録音が短いか別構成）")


def build_pat_packet(program_number: int, cc: int) -> bytes:
    """program_number → PMT 0x1FC8 の 1 エントリ PAT を 1 TS パケットで作る。"""
    sec = bytearray()
    sec += b"\x00"                                          # table_id = PAT
    body = bytearray()
    body += (1).to_bytes(2, "big")                          # transport_stream_id（任意）
    body += bytes([0xC1, 0x00, 0x00])                       # version 0, current, section 0/0
    body += program_number.to_bytes(2, "big")
    body += bytes([0xE0 | (ONESEG_PMT_PID >> 8), ONESEG_PMT_PID & 0xFF])
    section_length = len(body) + 4                          # + CRC32
    sec += bytes([0xB0 | (section_length >> 8), section_length & 0xFF])
    sec += body
    sec += crc32_mpeg(bytes(sec)).to_bytes(4, "big")
    pkt = bytearray([SYNC, 0x40, 0x00, 0x10 | (cc & 0x0F), 0x00])   # PID 0, PUSI, pointer 0
    pkt += sec
    pkt += b"\xFF" * (TS_PKT - len(pkt))
    return bytes(pkt)


def add_pat(ts: bytes, interval: int = 50) -> bytes:
    pn = find_program_number(memoryview(ts))
    out = bytearray()
    cc = 0
    for i in range(0, len(ts) - TS_PKT + 1, TS_PKT):
        if (i // TS_PKT) % interval == 0:
            out += build_pat_packet(pn, cc)
            cc += 1
        out += ts[i:i + TS_PKT]
    return bytes(out)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--interval", type=int, default=50, help="PAT 挿入間隔 [パケット]")
    args = ap.parse_args()
    ts = Path(args.src).read_bytes()
    if len(ts) < TS_PKT or len(ts) % TS_PKT:
        print(f"warning: サイズが 188 の倍数でない ({len(ts)})", file=sys.stderr)
    out = add_pat(ts, args.interval)
    Path(args.dst).write_bytes(out)
    print(f"wrote {args.dst}: {len(out)//TS_PKT} packets "
          f"(PAT {len(out)//TS_PKT - len(ts)//TS_PKT} 枚挿入, program_number は PMT から取得)")


if __name__ == "__main__":
    main()
