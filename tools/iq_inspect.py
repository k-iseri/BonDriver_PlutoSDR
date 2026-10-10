#!/usr/bin/env python3
"""SigMF I/Q の簡易解析: 平均電力、占有帯域、スペクトル画像、GI 相関によるシンボル長推定。"""
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ref"))
from isdbt.io import read_sigmf


def occupied_bw(x, fs, frac=0.99):
    spec = np.abs(np.fft.fftshift(np.fft.fft(x[: 1 << 16]))) ** 2
    c = np.cumsum(spec) / spec.sum()
    lo = np.searchsorted(c, (1 - frac) / 2)
    hi = np.searchsorted(c, 1 - (1 - frac) / 2)
    return (hi - lo) * fs / len(spec)


def gi_correlation_peak(x, fs, fft_size=8192, max_gi=1 << 11):
    """r[n] = sum x[n] x*[n+N] のピーク間隔からシンボル長を推定する（Mode 3 想定）。"""
    n = min(len(x) - fft_size - 1, 200_000)
    seg = x[: n + fft_size]
    corr = seg[:n] * np.conj(seg[fft_size : fft_size + n])
    mag = np.abs(np.convolve(corr, np.ones(max_gi // 4), mode="valid"))
    pk = np.argmax(mag[: fft_size * 2])
    return pk


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--png", help="スペクトル画像の出力先")
    args = ap.parse_args()
    x, meta = read_sigmf(args.path)
    fs = meta["global"]["core:sample_rate"]
    print(f"samples: {len(x)}  fs: {fs/1e6:.4f} MSPS  duration: {len(x)/fs:.2f} s")
    print(f"mean power: {10*np.log10(np.mean(np.abs(x)**2)):.1f} dBFS-ish")
    print(f"peak |x|: {np.max(np.abs(x)):.3f}")
    print(f"occupied bw (99%): {occupied_bw(x, fs)/1e6:.3f} MHz")
    if args.png:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        spec = 20 * np.log10(np.abs(np.fft.fftshift(np.fft.fft(x[: 1 << 16]))) + 1e-9)
        f = np.fft.fftshift(np.fft.fftfreq(1 << 16, 1 / fs)) / 1e6
        plt.plot(f, spec); plt.xlabel("MHz"); plt.ylabel("dB"); plt.savefig(args.png)
        print(f"wrote {args.png}")


if __name__ == "__main__":
    main()
