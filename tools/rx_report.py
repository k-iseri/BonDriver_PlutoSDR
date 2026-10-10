#!/usr/bin/env python3
"""受信品質レポート: SigMF を読み、同期 → TMCC → MER → 遅延プロファイルを一発で表示する。

実機で「映らない」ときの切り分け手順:
  1. Mode/GI 判定が Mode 3 / 1/8 以外 → 録音レートか中心周波数を疑う
  2. ロックするが MER が低い（< 22 dB）→ ゲイン・マルチパス・妨害を疑う（--plot で可視化）
  3. 階層 B だけ悪い → 64QAM に必要な C/N が足りない（ワンセグ A は粘る）

例: python tools/rx_report.py capture.sigmf-meta --plot out/report
"""
import argparse
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ref"))

from isdbt import measure
from isdbt import params as P
from isdbt.io import read_sigmf


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", help=".sigmf-meta（または拡張子なし）")
    ap.add_argument("--mode", type=int, default=None, choices=(1, 2, 3), help="既定: ブラインド判定")
    ap.add_argument("--gi", default=None, choices=list(P.GUARD_INTERVALS))
    ap.add_argument("--max-seconds", type=float, default=2.0, help="先頭から使う秒数")
    ap.add_argument("--frames", type=int, default=2, help="品質測定に使うフレーム数")
    ap.add_argument("--plot", default=None, help="PNG の出力先プレフィックス（スペクトル/コンスタ/キャリア MER/遅延）")
    args = ap.parse_args()

    iq, meta = read_sigmf(args.path)
    fs = float(meta["global"]["core:sample_rate"])
    iq = iq[: int(args.max_seconds * fs)]
    print(f"input: {len(iq)} samples @ {fs / 1e6:.4f} MSPS ({len(iq) / fs:.2f} s), "
          f"datatype {meta['global']['core:datatype']}")
    print(f"power: {10 * np.log10(np.mean(np.abs(iq) ** 2) + 1e-30):.1f} dBFS, peak |x| {np.max(np.abs(iq)):.3f}")

    # Mode / GI
    if args.mode is None or args.gi is None:
        mode, gi, scores = measure.detect_mode_gi(iq, fs)
        top = sorted(scores.items(), key=lambda kv: -(kv[1] if kv[1] == kv[1] else -1))[:3]
        print(f"mode/GI blind detection: Mode {mode.value} / GI {gi} "
              f"(top: {', '.join(f'M{m}/{g}={v:.2f}' for (m, g), v in top)})")
        if args.mode is not None:
            mode = P.Mode(args.mode)
        if args.gi is not None:
            gi = args.gi
    else:
        mode, gi = P.Mode(args.mode), args.gi

    r = measure.rx_quality(iq, fs, mode, gi, max_frames=args.frames)
    s = r["sync"]
    print(f"sync: clock {s['clock_ppm']:+.2f} ppm, CFO {s['cfo_hz']:+.1f} Hz (int {s['cfo_int_bins']} bins), "
          f"{s['n_sym']} symbols, frame start {r['frame_start']}, "
          f"TMCC parity {'ok' if r['tmcc_parity_ok'] else 'NG'}, burst symbols {r['bad_symbols']}")
    info = r["tmcc"]
    print(f"TMCC: partial reception {info.partial_reception}, switching {info.switching_indicator:04b}, "
          f"emergency {info.emergency_alarm}")
    for lyr in r["layers"]:
        need = {"QPSK": 6.2, "16QAM": 11.5, "64QAM": 16.5}.get(lyr.mod.value, 0) + \
               {"1/2": 0.0, "2/3": 1.6, "3/4": 2.6, "5/6": 3.6, "7/8": 4.3}.get(lyr.rate, 0)
        mer = r["mer_db"][lyr.name]
        verdict = "ok" if mer > need + 3 else ("marginal" if mer > need else "BAD")
        print(f"  layer {lyr.name}: {lyr.mod.value} {lyr.rate} I={lyr.I} x{len(lyr.segments)}seg  "
              f"MER {mer:5.1f} dB (必要目安 ~{need:.0f} dB) [{verdict}]")
    cm = r["carrier_mer_db"]
    ok = np.isfinite(cm)
    print(f"carrier MER: min {np.nanmin(cm):.1f} / median {np.nanmedian(cm):.1f} / max {np.nanmax(cm):.1f} dB "
          f"({int(ok.sum())} data carriers)")
    prof = r["delay_profile_db"]
    sig = np.flatnonzero(prof > -30)
    if len(sig) > 1:
        spread = r["delay_axis_us"][sig[-1]] if sig[-1] < len(prof) // 2 else 0.0
        print(f"delay profile: paths above -30 dB within {spread:.2f} us")

    if args.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        pre = Path(args.plot)
        pre.parent.mkdir(parents=True, exist_ok=True)

        n = 1 << 16
        spec = 20 * np.log10(np.abs(np.fft.fftshift(np.fft.fft(iq[:n]))) + 1e-9)
        f = np.fft.fftshift(np.fft.fftfreq(n, 1 / fs)) / 1e6
        plt.figure(figsize=(8, 4))
        plt.plot(f, spec, lw=0.4)
        plt.xlabel("MHz"), plt.ylabel("dB"), plt.title("spectrum"), plt.grid(True)
        plt.savefig(f"{pre}_spectrum.png", dpi=120)
        plt.close()

        n_l = len(r["constellation"])
        plt.figure(figsize=(4 * n_l, 4))
        for i, (name, z) in enumerate(r["constellation"].items()):
            ax = plt.subplot(1, n_l, i + 1)
            ax.plot(z.real, z.imag, ".", ms=1, alpha=0.3)
            ax.set_title(f"layer {name}  MER {r['mer_db'][name]:.1f} dB")
            ax.set_aspect("equal")
            ax.grid(True)
        plt.tight_layout()
        plt.savefig(f"{pre}_constellation.png", dpi=120)
        plt.close()

        plt.figure(figsize=(8, 4))
        plt.plot(cm, lw=0.5)
        plt.xlabel("carrier"), plt.ylabel("MER [dB]"), plt.title("per-carrier MER"), plt.grid(True)
        plt.savefig(f"{pre}_carrier_mer.png", dpi=120)
        plt.close()

        plt.figure(figsize=(8, 4))
        half = len(prof) // 2
        plt.plot(r["delay_axis_us"][:half], prof[:half], lw=0.6)
        plt.xlim(0, 126)        # GI 1/8 = 126 µs
        plt.ylim(-60, 3)
        plt.xlabel("delay [us]"), plt.ylabel("dB"), plt.title("channel delay profile"), plt.grid(True)
        plt.savefig(f"{pre}_delay.png", dpi=120)
        plt.close()
        print(f"wrote {pre}_{{spectrum,constellation,carrier_mer,delay}}.png")


if __name__ == "__main__":
    main()
