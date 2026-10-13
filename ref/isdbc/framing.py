"""フレーム構成と誤り訂正（J.83 C.4 / C.5 = Annex A）: 同期バイト反転・ランダマイズ・RS(204,188)・
畳み込みバイトインターリーブ、およびその受信側（同期検出・デインターリーブ・RS 復号・逆ランダマイズ）。

RS 符号・PRBS 系列・バイトインターリーブは ISDB-T（STD-B31 3.5〜3.7）と同一なので isdbt.fec を使う。
違いは PRBS の初期化単位（ISDB-T: フレームごと / ISDB-C: 8 パケットごと、先頭同期バイトを 0xB8 に反転）。
"""
from __future__ import annotations

import numpy as np
from isdbt import fec as _fec

from . import params as P

TEI_MASK = 0x80         # transport_error_indicator（2 バイト目）


def null_packets(n: int) -> np.ndarray:
    pk = np.full((n, P.TS_PACKET), 0xFF, dtype=np.uint8)
    pk[:, 0] = P.SYNC_BYTE
    pk[:, 1] = 0x1F
    pk[:, 2] = 0xFF
    pk[:, 3] = 0x10
    return pk


# ---------------------------------------------------------------------------
# C.5.1 同期バイト反転とランダマイズ
# PRBS（1 + x^14 + x^15、初期値 100101010000000）は 8 パケットごとに初期化し、最初の出力ビットを
# 反転同期バイト 0xB8 の直後のバイトの MSB に掛ける。以降 7 パケットの同期バイトの間も PRBS は
# 進むが出力しない（よって周期 1503 バイト）。
# ---------------------------------------------------------------------------
_PRBS = _fec.energy_dispersal_prbs(P.PRBS_PERIOD_BYTES)
# グループ内パケット i（0..7）のペイロード 187 バイトに掛かる PRBS: 先頭パケットは PRBS[0:187]、
# 以降は同期バイト分 1 バイトずつ飛ぶので PRBS[188 i : 188 i + 187]
_PRBS_PAYLOAD = np.stack([_PRBS[P.TS_PACKET * i: P.TS_PACKET * i + P.TS_PACKET - 1]
                          for i in range(P.SCRAMBLE_GROUP)])


def scramble(packets: np.ndarray) -> np.ndarray:
    """(8k, 188) の TS パケット → 同期バイト反転 + ランダマイズ後 (8k, 188)。"""
    pk = np.array(packets, dtype=np.uint8)
    assert pk.ndim == 2 and pk.shape[1] == P.TS_PACKET and pk.shape[0] % P.SCRAMBLE_GROUP == 0, pk.shape
    n_groups = pk.shape[0] // P.SCRAMBLE_GROUP
    pk[::P.SCRAMBLE_GROUP, 0] = P.SYNC_BYTE_INVERTED
    pk[:, 1:] ^= np.tile(_PRBS_PAYLOAD, (n_groups, 1))
    return pk


def descramble(packets: np.ndarray, group_phase: int) -> np.ndarray:
    """逆ランダマイズ。group_phase はパケット 0 のグループ内番号（0 なら先頭が 0xB8 のパケット）。

    同期バイトは 0x47 に戻す。"""
    pk = np.array(packets, dtype=np.uint8)
    n = pk.shape[0]
    idx = (np.arange(n) + group_phase) % P.SCRAMBLE_GROUP
    pk[:, 1:] ^= _PRBS_PAYLOAD[idx]
    pk[:, 0] = P.SYNC_BYTE
    return pk


# ---------------------------------------------------------------------------
# 送信側: TS → 伝送バイト列
# ---------------------------------------------------------------------------
def encode_ts(packets: np.ndarray) -> np.ndarray:
    """(8k, 188) → ランダマイズ → RS(204,188) → バイトインターリーブ → 1 次元バイト列 (8k*204,)。

    バイトインターリーブは isdbt.fec.byte_interleave（フレーム整合形: 整数パケット分の
    一定遅延を除いて Forney 型 I=12, M=17 と同一。同期バイトはブランチ 0）。
    """
    tsps = _fec.rs_encode(scramble(packets))
    return _fec.byte_interleave(tsps).ravel()


# ---------------------------------------------------------------------------
# 受信側
# ---------------------------------------------------------------------------
def sync_score(stream: np.ndarray) -> tuple[int, float]:
    """204 バイト周期の同期バイト（0x47 / 0xB8）が最も揃うオフセットと、その一致率を返す。"""
    s = np.asarray(stream, dtype=np.uint8).ravel()
    n = len(s) // P.RS_N - 1
    if n < 2:
        return 0, 0.0
    best, best_hits = 0, -1
    for off in range(P.RS_N):
        col = s[off: off + n * P.RS_N: P.RS_N]
        hits = int(np.count_nonzero((col == P.SYNC_BYTE) | (col == P.SYNC_BYTE_INVERTED)))
        if hits > best_hits:
            best, best_hits = off, hits
    return best, best_hits / n


def decode_stream(stream: np.ndarray, sync_offset: int | None = None) -> dict:
    """伝送バイト列 → TS パケット。

    戻り値: {"packets": (n,188) 同期バイトは 0x47、訂正不能は TEI を立てる,
            "rs_err": (n,) 訂正バイト数（-1 = 訂正不能）, "sync_offset", "sync_ratio", "group_phase"}
    先頭 11 パケット（デインターリーブの履歴なし）は捨てる。
    """
    s = np.asarray(stream, dtype=np.uint8).ravel()
    if sync_offset is None:
        sync_offset, ratio = sync_score(s)
    else:
        _, ratio = sync_score(s[sync_offset:])
        ratio = float(ratio)
    s = s[sync_offset:]
    n = len(s) // P.RS_N
    tsps = s[: n * P.RS_N].reshape(n, P.RS_N)
    deint = _fec.byte_deinterleave(tsps)[P.INTERLEAVE_I - 1:]      # 先頭 11 TSP は不定
    fixed, nerr = _fec.rs_decode(deint)
    pk = fixed[:, : P.TS_PACKET]
    # 8 パケットグループの位相: 訂正後の同期バイトが 0xB8 の行番号 mod 8 の最頻値
    inv_rows = np.flatnonzero((pk[:, 0] == P.SYNC_BYTE_INVERTED) & (nerr >= 0))
    if len(inv_rows) == 0:
        group_phase = 0
    else:
        counts = np.bincount(inv_rows % P.SCRAMBLE_GROUP, minlength=P.SCRAMBLE_GROUP)
        group_phase = int((-np.argmax(counts)) % P.SCRAMBLE_GROUP)
    out = descramble(pk, group_phase)
    out[nerr < 0, 1] |= TEI_MASK
    return {"packets": out, "rs_err": nerr, "sync_offset": int(sync_offset), "sync_ratio": float(ratio),
            "group_phase": group_phase}
