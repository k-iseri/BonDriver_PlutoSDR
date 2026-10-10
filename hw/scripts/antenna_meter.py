"""アンテナ調整用のリアルタイム信号品質メーター（Pluto SDR 用）。

GI 相関（Mode 3 / GI 1/8、lag=8192 の自己相関）から C/N を推定して表示し続ける。
アンテナを動かしながら数値が最大になる位置・向きを探すためのツール。

原理: 正規化 GI 相関 ρ の期待値は (GI/(Nu+GI))・s/(1+s) = (1/9)・s/(1+s)、
s = C/N（真値）。よって s = ρ / (1/9 − ρ) で逆算できる。
周波数選択性フェージングでは過小評価気味になるが、相対比較には十分。

全帯域に加えて、帯域中央のセグメント 0（ワンセグ、±214 kHz）だけを
周波数領域で切り出した C/N も表示する。ワンセグ目標: セグメント 0 の C/N ≈ 8 dB 以上。

使い方:
    .venv\\Scripts\\python hw\\scripts\\antenna_meter.py --ch 25
    Ctrl+C で終了。--iterations N で回数を区切れる（動作確認用）。

実機検証: 2026-10-10 Pluto SDR nano (Z7010-AD9363A, fw v0.39) で動作確認。
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

# libiio1.dll は venv の Scripts/ に置いてある（docs/hw-setup.md 参照）
os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]

import numpy as np

NU = 8192          # Mode 3 有効シンボル長（fs = 512/63 MHz のとき）
GI_FRAC = 1 / 9    # GI 1/8 → GI/(Nu+GI)
SEG0_HALF_HZ = 214_286  # セグメント 0 の片側幅 ≈ (5.572 MHz / 13) / 2


def channel_freq_hz(ch: int) -> int:
    return 473_142_857 + 6_000_000 * (ch - 13)


def gi_corr(x: np.ndarray) -> float:
    a, b = x[:-NU], x[NU:]
    num = np.abs(np.vdot(a, b))
    den = np.sqrt(np.vdot(a, a).real * np.vdot(b, b).real) + 1e-30
    return float(num / den)


def corr_to_cn_db(rho: float) -> float:
    s = rho / max(GI_FRAC - rho, 1e-6)
    return 10 * np.log10(max(s, 1e-6))


def seg0_filter(x: np.ndarray, fs: float) -> np.ndarray:
    """帯域中央 ±SEG0_HALF_HZ だけ残す（周波数領域マスク）。"""
    X = np.fft.fft(x)
    f = np.fft.fftfreq(len(x), 1 / fs)
    X[np.abs(f) > SEG0_HALF_HZ] = 0
    return np.fft.ifft(X)


def bar(cn_db: float, lo: float = -3.0, hi: float = 20.0, width: int = 30) -> str:
    frac = min(max((cn_db - lo) / (hi - lo), 0.0), 1.0)
    n = int(frac * width)
    return "#" * n + "-" * (width - n)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--uri", default="usb:")
    ap.add_argument("--ch", type=int, default=25, help="物理チャンネル")
    ap.add_argument("--gain", type=float, default=None, help="手動ゲイン dB（省略で AGC）")
    ap.add_argument("--nsamp", type=int, default=262144, help="1 回あたりサンプル数")
    ap.add_argument("--iterations", type=int, default=0, help="0 = Ctrl+C まで無限")
    args = ap.parse_args()

    import adi

    sdr = adi.Pluto(args.uri)
    sdr.sample_rate = 8_126_984
    sdr.rx_rf_bandwidth = 6_000_000
    sdr.rx_lo = channel_freq_hz(args.ch)
    if args.gain is None:
        sdr.gain_control_mode_chan0 = "slow_attack"
    else:
        sdr.gain_control_mode_chan0 = "manual"
        sdr.rx_hardwaregain_chan0 = args.gain
    sdr.rx_buffer_size = args.nsamp

    fs = float(sdr.sample_rate)
    for _ in range(2):  # AGC 整定とパイプライン内の古いサンプルを捨てる
        sdr.rx()
    print(f"ch {args.ch} ({channel_freq_hz(args.ch)/1e6:.3f} MHz)  "
          f"ワンセグ目標: seg0 C/N 8 dB 以上（64QAM フルセグは 19 dB 以上）")
    print("アンテナを動かして seg0 C/N を最大化。Ctrl+C で終了。")
    print("注意: 体がアンテナの近くにあると数 dB 変わる。読み取りは一歩離れて行うこと。")

    best = -99.0
    i = 0
    try:
        while args.iterations == 0 or i < args.iterations:
            x = np.asarray(sdr.rx(), dtype=np.complex64)
            cn_full = corr_to_cn_db(gi_corr(x))
            cn_seg0 = corr_to_cn_db(gi_corr(seg0_filter(x, fs)))
            best = max(best, cn_seg0)
            peak = int(np.max(np.abs(x.real)))
            sat = " [飽和]" if peak > 1800 else ""
            line = (f"full {cn_full:5.1f} dB | seg0 {cn_seg0:5.1f} dB "
                    f"[{bar(cn_seg0)}] best {best:5.1f}{sat}")
            print("\r" + line + " " * 4, end="", flush=True)
            i += 1
    except KeyboardInterrupt:
        pass
    print()


if __name__ == "__main__":
    main()
