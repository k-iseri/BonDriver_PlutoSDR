"""hw/pluto/pack8.py（tezuka_fw 8bit パックのホスト側モデル）と io.py の ci8 の試験。

実機不要。pack8 は FPGA 側の動作を模したモデルなので、ここで確かめられるのは
「モデルとアンパックの整合」まで。実機でのバイト順確認は hw/pluto/fw-8bit.md の項目。
"""
import importlib.util
from pathlib import Path

import numpy as np
from isdbt.io import read_sigmf, write_sigmf

HW_PLUTO = Path(__file__).resolve().parents[2] / "hw" / "pluto"


def _pack8():
    spec = importlib.util.spec_from_file_location("pack8", HW_PLUTO / "pack8.py")
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def test_pack8_word_layout():
    """下位バイト = I、上位バイト = Q、12bit からの算術右シフト 4。"""
    m = _pack8()
    x = np.array([127 / 128 - 1j, 0.25 + 0.5j], dtype=np.complex64)
    w = m.pack8(x)
    # i12=2032→127(0x7F), q12=-2048→-128(0x80) / i12=512→32(0x20), q12=1024→64(0x40)
    assert w.view(np.uint8).tolist() == [0x7F, 0x80, 0x20, 0x40]
    y = m.unpack8(w)
    assert y[0] == np.complex64(127 / 128 - 1j)
    assert y[1] == np.complex64(0.25 + 0.5j)


def test_pack8_accepts_raw_bytes():
    m = _pack8()
    y = m.unpack8(bytes([0x7F, 0x80]))
    assert y[0] == np.complex64(127 / 128 - 1j)


def test_pack8_roundtrip_quantization():
    """切り捨て量子化なので誤差は片側に最大 1 LSB（1/128）+ 12bit 丸めの半 LSB。"""
    m = _pack8()
    rng = np.random.default_rng(1)
    x = (rng.uniform(-1, 1, 8192) + 1j * rng.uniform(-1, 1, 8192)).astype(np.complex64) * 0.95
    y = m.unpack8(m.pack8(x))
    err_r = x.real - y.real
    err_i = x.imag - y.imag
    for e in (err_r, err_i):
        assert np.all(e > -0.5 / 2048 - 1e-6)       # 切り捨てなので負側はほぼ出ない
        assert np.all(e < 16.5 / 2048 + 1e-6)       # 1 LSB8 + 半 LSB12


def test_sigmf_ci8_roundtrip(tmp_path):
    """int8 格子上の値は ci8 で往復不変（録音データを劣化なく保存できる）。"""
    i8 = np.arange(-128, 128, dtype=np.int8)
    x = (i8.astype(np.float32) + 1j * i8[::-1].astype(np.float32)) / 128.0
    x = x.astype(np.complex64)
    write_sigmf(tmp_path / "a", x, 8_126_984.0, 557_142_857.0, datatype="ci8")
    assert (tmp_path / "a.sigmf-data").stat().st_size == 2 * len(x)
    y, meta = read_sigmf(tmp_path / "a")
    assert meta["global"]["core:datatype"] == "ci8"
    assert np.array_equal(x, y)


def test_sigmf_ci8_matches_packed_bytes(tmp_path):
    """録音経路の不変条件: unpack8 → write_sigmf(ci8) のファイルはパック語のバイト列と一致。"""
    m = _pack8()
    rng = np.random.default_rng(2)
    x = (rng.uniform(-1, 1, 1024) + 1j * rng.uniform(-1, 1, 1024)).astype(np.complex64) * 0.9
    w = m.pack8(x)
    write_sigmf(tmp_path / "b", m.unpack8(w), 8_126_984.0, datatype="ci8")
    assert (tmp_path / "b.sigmf-data").read_bytes() == w.tobytes()
