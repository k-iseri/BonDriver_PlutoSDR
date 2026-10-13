"""実放送キャプチャ（ケーブル TV の BS トランスモジュレーション）での ISDB-C 復調試験。

testdata/captured/catv_56ch_g30（UHF 56ch = 731 MHz、NHK BS、TS_id 16625、Pluto 手動ゲイン 30 dB、
7.111 MSPS、0.5 秒）が無ければ skip。録音手順は testdata/captured/README.md。
"""
from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest
from isdbc import demod
from isdbt.io import read_sigmf

CAPTURE = Path(__file__).resolve().parents[2] / "testdata" / "captured" / "catv_56ch_g30"


@pytest.mark.slow
def test_real_catv_56ch():
    if not CAPTURE.with_suffix(".sigmf-meta").exists():
        pytest.skip("実波キャプチャ catv_56ch_g30 が無い")
    iq, meta = read_sigmf(CAPTURE)
    fs = float(meta["global"]["core:sample_rate"])
    out = demod.demodulate(iq[: int(0.5 * fs)], fs)
    assert not out["inverted"]
    assert out["sync_ratio"] > 0.99
    assert out["mer_db"] > 28.0
    assert out["rs_uncorrectable"] == 0
    assert out["tsmf"].frames > 100
    assert (1, 16625, 4) in out["tsmf"].streams
    pk = out["packets"]
    pids = ((pk[:, 1].astype(int) & 0x1F) << 8) | pk[:, 2]
    pat = pk[pids == 0][0]
    assert ((int(pat[8]) << 8) | int(pat[9])) == 16625        # PAT の transport_stream_id
    assert not np.any(pk[:, 1] & 0x80)
