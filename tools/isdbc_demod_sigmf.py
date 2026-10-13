#!/usr/bin/env python3
"""ケーブル TV（ISDB-C、J.83 Annex C 64QAM トランスモジュレーション）の SigMF を復調して TS を書き出す。

例: python tools/isdbc_demod_sigmf.py testdata/captured/catv_56ch_g30.sigmf-meta --out out/
出力: <out>/<basename>.ts（選択した TS、TEI 付き）。同期・MER・RS 統計と TSMF の TS 一覧を表示する。
--ts で TSMF の相対 TS 番号（1..15）か TS_id（"tsid:16625"）を指定、省略時は最小の相対 TS 番号、
"all" でヘッダ含め全スロットをそのまま出す。
"""
import argparse
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ref"))

from isdbc import demod
from isdbt.io import read_sigmf, write_ts


def parse_select(s: str | None):
    if s is None or s == "auto":
        return "auto"
    if s == "all":
        return "all"
    if s.startswith("tsid:"):
        return (None, int(s[5:], 0))
    return int(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", help=".sigmf-meta（または拡張子なし）")
    ap.add_argument("--out", default=None, help="出力ディレクトリ（既定: 入力と同じ場所）")
    ap.add_argument("--max-seconds", type=float, default=None, help="先頭から指定秒だけ処理する")
    ap.add_argument("--ts", default=None, help="相対 TS 番号 / tsid:<n> / auto / all")
    ap.add_argument("--no-inversion", action="store_true", help="スペクトル反転の再試行をしない")
    args = ap.parse_args()

    iq, meta = read_sigmf(args.path)
    fs = float(meta["global"]["core:sample_rate"])
    if args.max_seconds is not None:
        iq = iq[: int(args.max_seconds * fs)]
    rms = float(np.sqrt(np.mean(np.abs(iq) ** 2)))
    print(f"input: {len(iq)} samples @ {fs:.0f} Hz ({len(iq) / fs:.2f} s), {meta['global']['core:datatype']}, "
          f"RMS {20 * np.log10(max(rms, 1e-9)):.1f} dBFS")

    t0 = time.time()
    out = demod.demodulate(iq, fs, select=parse_select(args.ts), allow_inversion=not args.no_inversion)
    print(f"demodulated in {time.time() - t0:.1f} s")
    print(f"sync: ratio {out['sync_ratio']:.3f}, bit offset {out['bit_offset']}, "
          f"spectrum {'inverted' if out['inverted'] else 'normal'}, CFO {out['cfo_hz']:+.0f} Hz, "
          f"clock {out['clock_ppm']:+.1f} ppm, MER {out['mer_db']:.1f} dB")
    n = len(out["rs_err"])
    print(f"RS: {n} TSP, uncorrectable {out['rs_uncorrectable']} ({100.0 * out['rs_uncorrectable'] / max(n, 1):.2f}%)")
    info = out["tsmf"]
    if info.frames:
        print(f"TSMF: {info.frames} frames ({info.bad_headers} bad headers), streams:")
        for rel, tsid, onid in info.streams:
            mark = " <- selected" if rel == info.selected else ""
            print(f"  relative TS {rel}: TS_id {tsid} (0x{tsid:04X}), original_network_id {onid}{mark}")
    else:
        print("TSMF: none (single TS)")
    pk = out["packets"]
    pids = ((pk[:, 1].astype(int) & 0x1F) << 8) | pk[:, 2]
    u, c = np.unique(pids, return_counts=True)
    print(f"output: {pk.shape[0]} packets, PIDs: " + ", ".join(f"0x{int(a):04X}:{int(b)}" for a, b in zip(u, c)))

    base = Path(args.path)
    if base.suffix == ".sigmf-meta":
        base = base.with_suffix("")
    out_dir = Path(args.out) if args.out else base.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"{base.name}.ts"
    write_ts(path, pk)
    print(f"wrote {path}")


if __name__ == "__main__":
    main()
