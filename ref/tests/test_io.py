"""io.py の SigMF 読み書きの試験。"""
import numpy as np
from isdbt.io import read_sigmf, write_sigmf


def test_sigmf_ci16_roundtrip(tmp_path):
    """回帰: 旧実装は complex を astype(int16) していて虚部が全部 0 で書かれていた。"""
    rng = np.random.default_rng(0)
    x = (rng.uniform(-1, 1, 2048) + 1j * rng.uniform(-1, 1, 2048)).astype(np.complex64) * 0.9
    write_sigmf(tmp_path / "a", x, 8_126_984.0, 557_142_857.0, datatype="ci16_le")
    y, meta = read_sigmf(tmp_path / "a")
    assert meta["global"]["core:datatype"] == "ci16_le"
    assert np.any(np.abs(y.imag) > 0.1)
    # 書き 2047 / 読み 2048 のスケール差 + 丸めで誤差は 1 LSB 強に収まる
    assert np.max(np.abs(y - x)) < 2.0 / 2048


def test_sigmf_cf32_roundtrip(tmp_path):
    x = (np.arange(8) - 4 + 1j * np.arange(8)).astype(np.complex64) / 8
    write_sigmf(tmp_path / "b", x, 7_000_000.0, datatype="cf32_le")
    y, meta = read_sigmf(tmp_path / "b")
    assert np.array_equal(x, y)
    assert meta["global"]["core:sample_rate"] == 7_000_000.0
