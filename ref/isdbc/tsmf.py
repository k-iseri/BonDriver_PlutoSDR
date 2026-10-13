"""TSMF（Transport Streams Multiplexing Frame、ITU-T J.183）: 1 本の 64QAM キャリアに最大 15 本の
TS を 53 スロット（ヘッダ 1 + TS パケット 52）の固定フレームで時分割多重する。

ヘッダパケット（PID 0x002F、188 バイト）の配置（J.183 Appendix I 表 I.1 の運用値 N=53, M=15）:
  0       sync_byte 0x47
  1-2     000 + TSMF_header_PID (13 bit)
  3       0001 + continuity_counter (4 bit)
  4-5     reserved(3) + TSMF_sync(13) = 0x1A86（または反転 0x0579。VERIFY: 交互反転の運用）
  6       version_number(3) + slot_allocation_type(1: 0 = 相対 TS 番号方式) + frame_type(4: 1)
  7-8     availability_for_relative_TS_number[1..15]（MSB から）+ reserved(1)
  9-68    (TS_id(16), original_network_id(16)) × 15
  69-72   receive_status[1..15] (2 bit × 15) + reserved(1) + emergency_indicator(1)   VERIFY: 末尾 2 ビットの順
  73-98   relative_TS_number[slot 1..52]（4 bit × 52、上位ニブルが先）
  99-183  private_data（85 バイト）
  184-187 CRC_32（MPEG-2 と同じ。4..187 バイトを通すと余り 0）
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np

from . import params as P


# ---------------------------------------------------------------------------
# CRC-32/MPEG-2（生成多項式 0x04C11DB7、初期値 0xFFFFFFFF、反転なし）
# ---------------------------------------------------------------------------
def _crc_table() -> np.ndarray:
    tbl = np.zeros(256, dtype=np.uint32)
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04C11DB7) if (c & 0x80000000) else (c << 1)
            c &= 0xFFFFFFFF
        tbl[i] = c
    return tbl


_CRC_TABLE = _crc_table()


def crc32_mpeg(data: np.ndarray) -> int:
    crc = 0xFFFFFFFF
    for b in np.asarray(data, dtype=np.uint8).ravel():
        crc = ((crc << 8) & 0xFFFFFFFF) ^ int(_CRC_TABLE[((crc >> 24) ^ int(b)) & 0xFF])
    return crc


# ---------------------------------------------------------------------------
# ヘッダ
# ---------------------------------------------------------------------------
@dataclass
class TsmfHeader:
    continuity_counter: int
    sync_inverted: bool
    version: int
    frame_type: int
    available: list[bool]                 # 相対 TS 番号 1..15（添字 0 = TS 1）
    ts_id: list[int]
    onid: list[int]
    receive_status: list[int]
    emergency: int
    slot_ts: np.ndarray = field(repr=False)    # (52,) スロット 1..52 の相対 TS 番号（0 = 空き）

    def active_ts(self) -> list[tuple[int, int, int]]:
        """[(相対 TS 番号, TS_id, original_network_id)]（availability が立っているもの）。"""
        return [(i + 1, self.ts_id[i], self.onid[i]) for i in range(P.TSMF_MAX_TS) if self.available[i]]


def is_header_candidate(pkt: np.ndarray) -> bool:
    return bool(pkt[0] == P.SYNC_BYTE and (((int(pkt[1]) & 0x1F) << 8) | int(pkt[2])) == P.TSMF_HEADER_PID
                and (int(pkt[3]) & 0xF0) == 0x10)


def parse_header(pkt: np.ndarray) -> TsmfHeader | None:
    """188 バイト → TsmfHeader。PID・固定ビット・同期語・CRC のいずれかが合わなければ None。"""
    pkt = np.asarray(pkt, dtype=np.uint8)
    if len(pkt) != P.TS_PACKET or not is_header_candidate(pkt):
        return None
    sync = ((int(pkt[4]) << 8) | int(pkt[5])) & P.TSMF_SYNC_MASK
    if sync == P.TSMF_SYNC:
        inverted = False
    elif sync == P.TSMF_SYNC_INVERTED:
        inverted = True
    else:
        return None
    if crc32_mpeg(pkt[4:]) != 0:
        return None
    slot_alloc = (int(pkt[6]) >> 4) & 1
    if slot_alloc != 0:                 # 相対 TS 番号方式以外は未対応
        return None
    avail_bits = (int(pkt[7]) << 8) | int(pkt[8])
    available = [bool((avail_bits >> (15 - i)) & 1) for i in range(P.TSMF_MAX_TS)]
    ts_id = [(int(pkt[9 + 4 * i]) << 8) | int(pkt[10 + 4 * i]) for i in range(P.TSMF_MAX_TS)]
    onid = [(int(pkt[11 + 4 * i]) << 8) | int(pkt[12 + 4 * i]) for i in range(P.TSMF_MAX_TS)]
    rs_bits = int.from_bytes(bytes(pkt[69:73]), "big")
    receive_status = [(rs_bits >> (30 - 2 * i)) & 3 for i in range(P.TSMF_MAX_TS)]
    emergency = int(pkt[72]) & 1
    nib = pkt[73:99].astype(np.int64)
    slot_ts = np.empty(P.TSMF_DATA_SLOTS, dtype=np.int64)
    slot_ts[0::2] = nib >> 4
    slot_ts[1::2] = nib & 0x0F
    return TsmfHeader(int(pkt[3]) & 0x0F, inverted, (int(pkt[6]) >> 5) & 7, int(pkt[6]) & 0x0F,
                      available, ts_id, onid, receive_status, emergency, slot_ts)


def build_header(slot_ts: np.ndarray, ts_ids: dict[int, tuple[int, int]], *, continuity_counter: int = 0,
                 sync_inverted: bool = False, version: int = 0, emergency: int = 0) -> np.ndarray:
    """ヘッダパケットを組む（試験信号用）。ts_ids は {相対 TS 番号: (TS_id, original_network_id)}。"""
    slot_ts = np.asarray(slot_ts, dtype=np.int64)
    assert slot_ts.shape == (P.TSMF_DATA_SLOTS,)
    h = np.full(P.TS_PACKET, 0xFF, dtype=np.uint8)
    h[0] = P.SYNC_BYTE
    h[1] = (P.TSMF_HEADER_PID >> 8) & 0x1F
    h[2] = P.TSMF_HEADER_PID & 0xFF
    h[3] = 0x10 | (continuity_counter & 0x0F)
    sync = (0b111 << 13) | (P.TSMF_SYNC_INVERTED if sync_inverted else P.TSMF_SYNC)
    h[4], h[5] = sync >> 8, sync & 0xFF
    h[6] = ((version & 7) << 5) | (0 << 4) | (P.TSMF_FRAME_TYPE & 0x0F)
    avail = 0
    for rel in ts_ids:
        assert 1 <= rel <= P.TSMF_MAX_TS
        avail |= 1 << (15 - (rel - 1))
    avail |= 1                          # reserved = 1
    h[7], h[8] = avail >> 8, avail & 0xFF
    for i in range(P.TSMF_MAX_TS):
        tsid, onid = ts_ids.get(i + 1, (0xFFFF, 0xFFFF))
        h[9 + 4 * i], h[10 + 4 * i] = tsid >> 8, tsid & 0xFF
        h[11 + 4 * i], h[12 + 4 * i] = onid >> 8, onid & 0xFF
    h[69:72] = 0                        # receive_status = 00（受信良好）
    h[72] = 0b10 | (emergency & 1)      # reserved(1) + emergency_indicator
    h[73:99] = ((slot_ts[0::2] << 4) | slot_ts[1::2]).astype(np.uint8)
    crc = crc32_mpeg(h[4:184])
    h[184:188] = [(crc >> 24) & 0xFF, (crc >> 16) & 0xFF, (crc >> 8) & 0xFF, crc & 0xFF]
    assert crc32_mpeg(h[4:]) == 0
    return h


# ---------------------------------------------------------------------------
# 多重（送信側・試験用）と分離（受信側）
# ---------------------------------------------------------------------------
def multiplex(streams: dict[int, np.ndarray], ts_ids: dict[int, tuple[int, int]],
              n_frames: int | None = None) -> np.ndarray:
    """{相対 TS 番号: (n_i, 188)} を TSMF フレーム列 (53 F, 188) に多重する。

    各フレームの 52 スロットはパケット数に比例して配分し（最低 1）、尽きた TS の分は NULL で埋める。
    スロット割当は毎フレーム同じ。n_frames 省略時は全パケットが入る最小フレーム数。
    """
    from .framing import null_packets

    rels = sorted(streams)
    counts = {r: int(streams[r].shape[0]) for r in rels}
    total = sum(counts.values())
    share = {r: max(1, round(P.TSMF_DATA_SLOTS * counts[r] / total)) for r in rels}
    while sum(share.values()) > P.TSMF_DATA_SLOTS:
        share[max(share, key=share.get)] -= 1
    slot_ts = np.zeros(P.TSMF_DATA_SLOTS, dtype=np.int64)
    pos = 0
    for r in rels:                                  # TS ごとに連続スロット
        slot_ts[pos: pos + share[r]] = r
        pos += share[r]
    if n_frames is None:
        n_frames = max(-(-counts[r] // share[r]) for r in rels)
    out = np.empty((n_frames * P.TSMF_SLOTS, P.TS_PACKET), dtype=np.uint8)
    cursor = {r: 0 for r in rels}
    null = null_packets(1)[0]
    for f in range(n_frames):
        base = f * P.TSMF_SLOTS
        out[base] = build_header(slot_ts, ts_ids, continuity_counter=f % 16, sync_inverted=bool(f % 2))
        for s in range(P.TSMF_DATA_SLOTS):
            r = int(slot_ts[s])
            if r and cursor[r] < counts[r]:
                out[base + 1 + s] = streams[r][cursor[r]]
                cursor[r] += 1
            else:
                out[base + 1 + s] = null
    return out


@dataclass
class TsmfInfo:
    frames: int = 0                     # 認識したヘッダ数
    bad_headers: int = 0                # ヘッダ候補（PID 一致）だが同期語/CRC 不一致
    streams: list[tuple[int, int, int]] = field(default_factory=list)   # 最後のヘッダの (相対番号, TS_id, ONID)
    selected: int | None = None         # 出力した相対 TS 番号（None = TSMF なし / 全通過）


def demultiplex(packets: np.ndarray, select=None) -> tuple[np.ndarray, TsmfInfo]:
    """TSMF フレーム列から 1 本の TS を取り出す。

    select: None/"auto" = 有効 TS のうち最小の相対 TS 番号（1 本ならそれ）。
            int = 相対 TS 番号（1..15）。(onid, tsid) = 識別子で選ぶ（onid が None なら tsid のみ）。
            "all" = ヘッダ含め全スロットをそのまま通す（TSMF 非対応受信機の挙動）。
    TSMF ヘッダが 1 つも無ければ入力をそのまま返す（単一 TS）。
    """
    pk = np.asarray(packets, dtype=np.uint8)
    info = TsmfInfo()
    if select == "all":
        return pk, info
    n = pk.shape[0]
    keep = np.zeros(n, dtype=bool)
    header: TsmfHeader | None = None
    frame_start = -1
    for i in range(n):
        p = pk[i]
        if is_header_candidate(p):
            h = parse_header(p)
            if h is None:
                info.bad_headers += 1
                continue
            header = h
            frame_start = i
            info.frames += 1
            info.streams = h.active_ts()
            continue
        if header is None or not (0 < i - frame_start <= P.TSMF_DATA_SLOTS):
            continue
        want = _resolve(header, select)
        if want is None:
            continue
        info.selected = want
        if header.slot_ts[i - frame_start - 1] == want:
            keep[i] = True
    if info.frames == 0:
        return pk, info
    return pk[keep], info


def _resolve(h: TsmfHeader, select) -> int | None:
    active = h.active_ts()
    if not active:
        return None
    if select is None or select == "auto":
        return active[0][0]
    if isinstance(select, int):
        return select if h.available[select - 1] else None
    onid, tsid = select
    for rel, t, o in active:
        if t == tsid and (onid is None or o == onid):
            return rel
    return None
