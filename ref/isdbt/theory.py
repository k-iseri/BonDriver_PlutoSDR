"""理論曲線（テストの基準値用）。"""
from __future__ import annotations

import math

import numpy as np


def qam_ber(M: int, esn0_db: float) -> float:
    """グレイ符号化 正方 M-QAM の AWGN 非符号化 BER（Cho & Yoon 2002 の厳密式）。esn0 はシンボルあたり。"""
    gamma = 10 ** (esn0_db / 10)
    sqrt_m = round(math.sqrt(M))
    k_max = round(math.log2(sqrt_m))
    total = 0.0
    for k in range(1, k_max + 1):
        pk = 0.0
        for i in range(int((1 - 2 ** (-k)) * sqrt_m)):
            w = (-1) ** (i * 2 ** (k - 1) // sqrt_m) * (2 ** (k - 1) - (i * 2 ** (k - 1) // sqrt_m + 0.5).__floor__())
            pk += w * math.erfc((2 * i + 1) * math.sqrt(3 * gamma / (2 * (M - 1))))
        total += pk / sqrt_m
    return float(total / k_max)


def qam_ber_curve(M: int, esn0_db: np.ndarray) -> np.ndarray:
    return np.array([qam_ber(M, float(x)) for x in np.asarray(esn0_db)])
