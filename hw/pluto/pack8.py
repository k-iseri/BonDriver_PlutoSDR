"""tezuka_fw の complex 8bit モードのパック/アンパック（ホスト側）。

Pluto 互換機の USB 2.0 では 16bit I/Q の 8.127 MSPS（32.5 MB/s）が実用上通らないため、
tezuka_fw（https://github.com/F5OEO/tezuka_fw, GPL-3.0）の complex 8bit モードを使う
（経緯と導入手順は hw/pluto/fw-8bit.md、採用判断は docs/decisions.md）。

仕組み: IIO で rx0_q（cf-ad9361-lpc の voltage1）を無効化すると、FPGA が I チャンネルの
16bit 枠に (i8, q8) を詰めて送る。ホストは voltage0 のバッファを int8 の対として解釈する。
このモジュールは tezuka_fw のコードを複製したものではなく、プロトコル（データ配置）に
合わせて独立に書いたもの。

# VERIFY: 16bit ワード内のバイト順（下位バイト = I、上位バイト = Q）と、12bit→8bit が
# 上位 8bit の切り捨て（算術右シフト 4）であることは実機未確認。確認手順:
# 既知トーン（搬送波 +100 kHz など）を受信し、unpack8 のスペクトルのピークが正しい側に
# 出るか、I/Q 入れ替わり（スペクトル反転）が無いかを見る。誤りならここと
# tools/iq_record.py を直し、この VERIFY を外す。

未検証: ボード未着のため実機では試していない。pack8 は FPGA 側の動作を模した試験用
モデルで、round-trip テスト（ref/tests/test_pack8.py）にのみ使う。実際の送信には使わない。
"""
from __future__ import annotations

import numpy as np


def pack8(x: np.ndarray) -> np.ndarray:
    """複素サンプル（フルスケール ±1.0）→ FPGA が送る 16bit ワード列（試験用モデル）。

    AD9363 の 12bit ADC 値（±2048）を模してから上位 8bit を切り捨てる（算術右シフト 4）。
    丸めではなく切り捨てなのは、FPGA 側の最小実装（ビット選択のみ）に合わせるため。
    """
    i12 = np.clip(np.round(np.real(x) * 2048), -2048, 2047).astype(np.int16)
    q12 = np.clip(np.round(np.imag(x) * 2048), -2048, 2047).astype(np.int16)
    words = np.empty(len(i12), dtype="<i2")
    b = words.view(np.int8)
    b[0::2] = (i12 >> 4).astype(np.int8)  # 下位バイト = I
    b[1::2] = (q12 >> 4).astype(np.int8)  # 上位バイト = Q
    return words


def unpack8(raw: np.ndarray | bytes) -> np.ndarray:
    """voltage0 バッファ（16bit ワード列または生バイト列）→ complex64（±1.0 スケール）。"""
    a = np.frombuffer(raw, dtype=np.int8) if isinstance(raw, (bytes, bytearray)) \
        else np.ascontiguousarray(raw).view(np.int8)
    x = a[0::2].astype(np.float32) + 1j * a[1::2].astype(np.float32)
    return (x / 128.0).astype(np.complex64)
