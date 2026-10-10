"""SigMF I/Q ファイルと TS ファイルの読み書き。"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np

TS_PACKET = 188
TS_SYNC = 0x47


def read_sigmf(path: str | Path) -> tuple[np.ndarray, dict]:
    """`.sigmf-meta` のパスまたは拡張子なしのパスを受け取り、complex64 配列とメタを返す。"""
    p = Path(path)
    base = p.with_suffix("") if p.suffix in (".sigmf-meta", ".sigmf-data") else p
    meta = json.loads(base.with_suffix(".sigmf-meta").read_text())
    dtype = meta["global"]["core:datatype"]
    raw = np.fromfile(base.with_suffix(".sigmf-data"), dtype=_np_dtype(dtype))
    if dtype.startswith("cf32"):
        x = raw.astype(np.complex64)
    elif dtype.startswith("ci16"):
        x = (raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)) / 2048.0
        x = x.astype(np.complex64)
    elif dtype.startswith("ci8"):
        x = (raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)) / 128.0
        x = x.astype(np.complex64)
    else:
        raise ValueError(f"unsupported datatype {dtype}")
    return x, meta


def write_sigmf(base: str | Path, x: np.ndarray, sample_rate_hz: float, center_hz: float = 0.0,
                datatype: str = "cf32_le", extra: dict | None = None) -> None:
    base = Path(base)
    if datatype == "cf32_le":
        x.astype(np.complex64).tofile(base.with_suffix(".sigmf-data"))
    elif datatype == "ci16_le":
        # 複素のまま astype(int) すると虚部が落ちるので、実部虚部に分けてから量子化する
        re = np.clip(np.round(np.real(x) * 2047), -2048, 2047).astype(np.int16)
        im = np.clip(np.round(np.imag(x) * 2047), -2048, 2047).astype(np.int16)
        np.stack([re, im], axis=1).ravel().tofile(base.with_suffix(".sigmf-data"))
    elif datatype == "ci8":
        # ±128 スケール（+フルスケールのみクリップ）。読み戻しの /128 と対にして
        # int8 格子上の値（8bit 録音の生データ）が往復で不変になるようにする。
        re = np.clip(np.round(np.real(x) * 128), -128, 127).astype(np.int8)
        im = np.clip(np.round(np.imag(x) * 128), -128, 127).astype(np.int8)
        np.stack([re, im], axis=1).ravel().tofile(base.with_suffix(".sigmf-data"))
    else:
        raise ValueError(datatype)
    meta = {
        "global": {"core:datatype": datatype, "core:sample_rate": float(sample_rate_hz),
                   "core:version": "1.0.0"},
        "captures": [{"core:sample_start": 0, "core:frequency": float(center_hz)}],
        "annotations": [],
    }
    if extra:
        meta["global"].update(extra)
    base.with_suffix(".sigmf-meta").write_text(json.dumps(meta, indent=2))


def _np_dtype(datatype: str):
    if datatype.startswith("cf32"):
        return np.complex64
    if datatype.startswith("ci16"):
        return np.int16
    if datatype.startswith("ci8"):
        return np.int8
    raise ValueError(datatype)


def read_ts(path: str | Path) -> np.ndarray:
    data = np.fromfile(path, dtype=np.uint8)
    n = len(data) // TS_PACKET
    return data[: n * TS_PACKET].reshape(n, TS_PACKET)


def write_ts(path: str | Path, packets: np.ndarray) -> None:
    np.asarray(packets, dtype=np.uint8).ravel().tofile(path)


def ts_sync_ok(packets: np.ndarray) -> bool:
    return bool(np.all(packets[:, 0] == TS_SYNC))
