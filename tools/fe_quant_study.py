#!/usr/bin/env python3
"""前段 FPGA 化（pluto-fw-isdbt design-v2 の 2b）の量子化スタディ。

FPGA で同期・FFT・等化を行い「等化済みキャリア（I/Q 各 8bit）+ CSI（8bit）」をホストに渡す案について、
各段の語長が MER と RS 誤りにどう効くかを実波録音で測る。ref/ の浮動小数点パイプラインの途中に
量子化を挟むだけなので、FPGA の実装（ビット一致モデル）ではなく語長を決めるための見積もり。

  python tools/fe_quant_study.py testdata/captured/ch24.sigmf-meta --sec 2.5

変種:
  float      : 参照（ref/ そのまま）
  fftN       : FFT 出力を N bit（シンボルごとのブロック浮動小数点: 最大値でスケール）に量子化
  fftN+d8    : さらに等化出力 D を 8bit（フルスケール ±2.0）、CSI を 8bit（0〜4.0）に量子化
"""
import argparse
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ref"))
from isdbt import demod, measure, params as P, sync  # noqa: E402
from isdbt.io import read_sigmf  # noqa: E402


def quant_block_float(z: np.ndarray, bits: int) -> np.ndarray:
    """シンボル（行）ごとに最大振幅でスケールして bits bit の整数格子に丸める（FFT のスケーリング近似）。"""
    full = 2 ** (bits - 1) - 1
    s = np.maximum(np.max(np.maximum(np.abs(z.real), np.abs(z.imag)), axis=1, keepdims=True), 1e-30)
    re = np.round(z.real / s * full)
    im = np.round(z.imag / s * full)
    return ((re + 1j * im) * (s / full)).astype(np.complex64)


def quant_fixed(x: np.ndarray, bits: int, full_scale: float, signed: bool = True) -> np.ndarray:
    full = 2 ** (bits - 1) - 1 if signed else 2 ** bits - 1
    lo = -full - 1 if signed else 0
    q = np.clip(np.round(x / full_scale * full), lo, full)
    return (q * (full_scale / full)).astype(x.dtype)


def quant_complex_fixed(z: np.ndarray, bits: int, full_scale: float) -> np.ndarray:
    return (quant_fixed(z.real.astype(np.float32), bits, full_scale)
            + 1j * quant_fixed(z.imag.astype(np.float32), bits, full_scale)).astype(np.complex64)


def run_variant(Y: np.ndarray, fsi: dict, mode: P.Mode, n_sym: int, fft_bits: int | None, d8: bool) -> dict:
    f0 = fsi["frame_start"]
    Yq = quant_block_float(Y, fft_bits) if fft_bits else Y
    H, bad = demod.channel_estimate(Yq, mode, f0, return_quality=True)
    D, csi = demod.equalize(Yq, H)
    if d8:
        D = quant_complex_fixed(D, 8, 2.0)
        csi = quant_fixed(csi, 8, 4.0, signed=False)
    csi[bad] = 0.0
    demod.null_interfered_carriers(D, csi, mode, f0, fsi["layers"])

    # MER（時間デインターリーブ前、全キャリアが埋まるフレーム以降）
    hist = max((-(-demod.DI.time_deinterleave_max_delay(lyr.I) // P.SYMBOLS_PER_FRAME)
                for lyr in fsi["layers"]), default=0)
    r0 = f0 + hist * P.SYMBOLS_PER_FRAME
    rows = np.arange(r0, n_sym)
    rows = rows[~bad[rows]] if len(rows) else rows
    Yl = demod.DI.data_to_logical(D, mode, f0)
    out = {}
    for lyr in fsi["layers"]:
        slots = Yl[rows][:, lyr.segments, :].reshape(-1)
        out[f"mer_{lyr.name}"] = measure.mer_db(slots, lyr.mod)

    # RS 誤り（軟判定 → ビタビ → RS）
    Wl = demod.DI.data_to_logical(csi, mode, f0)
    I_seg = np.zeros(P.N_SEGMENTS, dtype=np.int64)
    for lyr in fsi["layers"]:
        I_seg[lyr.segments] = lyr.I
    Xt = demod.DI.time_deinterleave(demod.DI.intersegment_deinterleave(Yl, fsi["groups"]), I_seg)
    Wt = demod.DI.time_deinterleave(demod.DI.intersegment_deinterleave(Wl, fsi["groups"]), I_seg)
    fe = {"frame_start": f0, "n_frames": (n_sym - f0) // P.SYMBOLS_PER_FRAME, "Xt": Xt, "Wt": Wt}
    for lyr in fsi["layers"]:
        r = demod.layer_soft_bits(fe, lyr)
        if r is None:
            continue
        soft, _fstart = r
        _ts, nerr, _bits = demod.decode_soft_bits(soft, lyr, mode)
        nerr = np.asarray(nerr)
        out[f"rs_{lyr.name}"] = (int(np.sum(nerr < 0)), int(len(nerr)), float(np.mean(np.maximum(nerr, 0))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sigmf")
    ap.add_argument("--sec", type=float, default=2.5)
    ap.add_argument("--fft-bits", default="16,12,10")
    args = ap.parse_args()
    iq, meta = read_sigmf(args.sigmf)
    fs = float(meta["global"]["core:sample_rate"])
    iq = iq[: int(args.sec * fs)]
    mode, gi = P.JP_DEFAULT_MODE, P.JP_DEFAULT_GI
    t0 = time.time()
    src = sync.CarrierSource(iq, fs, mode, gi)
    fsi = demod.frame_sync(src, mode)
    n_sym = src.n_sym
    Y = src.symbols(0, n_sym)
    print(f"sync {time.time() - t0:.1f}s: n_sym {n_sym}, frame_start {fsi['frame_start']}, "
          f"layers {[(l.name, l.mod.name, l.rate, l.I, len(l.segments)) for l in fsi['layers']]}")
    variants = [("float", None, False)]
    for b in (int(x) for x in args.fft_bits.split(",")):
        variants.append((f"fft{b}", b, False))
        variants.append((f"fft{b}+d8", b, True))
    print(f"{'variant':12} " + " ".join(f"{'MER_' + l.name:>8} {'RSbad_' + l.name:>10}" for l in fsi["layers"]))
    for name, bits, d8 in variants:
        t1 = time.time()
        r = run_variant(Y, fsi, mode, n_sym, bits, d8)
        cols = []
        for l in fsi["layers"]:
            rs = r.get(f"rs_{l.name}")
            cols.append(f"{r[f'mer_{l.name}']:8.2f} {('%d/%d' % (rs[0], rs[1])) if rs else '-':>10}")
        print(f"{name:12} " + " ".join(cols) + f"   ({time.time() - t1:.0f}s)")


if __name__ == "__main__":
    main()
