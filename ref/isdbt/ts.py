"""TS パケットの補助（ヌルパケット、フレーム単位のパディング、階層ごとの TSP 数）。"""
from __future__ import annotations

import numpy as np

from . import params as P

TS_PACKET = 188
TS_SYNC = 0x47
NULL_PID = 0x1FFF


def null_packet() -> np.ndarray:
    """PID 0x1FFF のヌルパケット（ペイロードは 0xFF）。"""
    pk = np.full(TS_PACKET, 0xFF, dtype=np.uint8)
    pk[0] = TS_SYNC
    pk[1] = 0x1F
    pk[2] = 0xFF
    pk[3] = 0x10
    return pk


def pad_packets(packets: np.ndarray, n_total: int) -> np.ndarray:
    """(n, 188) を n_total 個になるまでヌルパケットで埋める。"""
    pk = np.asarray(packets, dtype=np.uint8).reshape(-1, TS_PACKET)
    if pk.shape[0] >= n_total:
        return pk[:n_total]
    fill = np.tile(null_packet(), (n_total - pk.shape[0], 1))
    return np.concatenate([pk, fill], axis=0)


def frames_needed(n_packets: int, tsp_per_frame: int) -> int:
    return max(1, -(-n_packets // tsp_per_frame))


def is_null(packets: np.ndarray) -> np.ndarray:
    pk = np.asarray(packets, dtype=np.uint8)
    pid = ((pk[:, 1].astype(int) & 0x1F) << 8) | pk[:, 2]
    return pid == NULL_PID


def tsp_per_frame(mode: P.Mode, mod: P.Modulation, rate: str, n_segments: int) -> int:
    return P.tsp_per_frame(mode, mod, rate, n_segments)
