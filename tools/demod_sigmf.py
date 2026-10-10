#!/usr/bin/env python3
"""SigMF の I/Q を復調して階層ごとの TS を書き出す。

例: python tools/demod_sigmf.py testdata/synthetic/std_mode3_gi8_clean.sigmf-meta --out out/
出力: <out>/<basename>_layerA.ts など。同期情報・TMCC・RS の訂正統計を表示する。
--track で追従復調（長時間・ドリフト・瞬断のある録音向け）、
--bts で 204 バイト放送 TS（多重フレーム + ISDB-T 情報 + RS(204,196)）も書き出す。
"""
import argparse
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ref"))

from isdbt import params as P
from isdbt.demod import demodulate
from isdbt.io import read_sigmf, write_ts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path", help=".sigmf-meta（または拡張子なし）")
    ap.add_argument("--out", default=None, help="出力ディレクトリ（既定: 入力と同じ場所）")
    ap.add_argument("--mode", type=int, default=3, choices=(1, 2, 3))
    ap.add_argument("--gi", default="1/8", choices=list(P.GUARD_INTERVALS))
    ap.add_argument("--max-seconds", type=float, default=None, help="先頭から指定秒だけ処理する")
    ap.add_argument("--block-frames", type=int, default=6, help="一度に処理するフレーム数（メモリとのトレードオフ）")
    ap.add_argument("--track", action="store_true", help="追従復調（クロックドリフト・瞬断に対応。区間ごとに再同期）")
    ap.add_argument("--bts", action="store_true", help="204 バイト放送 TS（_bts.ts）も書き出す")
    args = ap.parse_args()

    iq, meta = read_sigmf(args.path)
    fs = float(meta["global"]["core:sample_rate"])
    if args.max_seconds is not None:
        iq = iq[: int(args.max_seconds * fs)]
    print(f"input: {len(iq)} samples @ {fs:.0f} Hz ({len(iq) / fs:.2f} s), datatype {meta['global']['core:datatype']}")

    t0 = time.time()
    out, dbg = demodulate(iq, fs, P.Mode(args.mode), args.gi, block_frames=args.block_frames,
                          return_debug=True, track=args.track)
    print(f"demodulated in {time.time() - t0:.1f} s")
    if args.track:
        s = dbg["sync"]
        print(f"tracked: {s['locked_chunks']}/{s['total_chunks']} chunks locked, "
              f"clock {s['ppm_min']:+.2f}..{s['ppm_max']:+.2f} ppm, segments {s['segments']}")
        for d in dbg["segments"]:
            if "skipped" in d:
                print(f"  segment {d['symbols']}: skipped ({d['skipped']})")
            else:
                print(f"  segment {d['symbols']}: frame start {d['frame_start']}, {d['n_frames']} frames, "
                      f"TMCC parity {'ok' if d['tmcc_parity_ok'] else 'NG'}")
        decoded = [d for d in dbg["segments"] if "skipped" not in d]
        dbg_first = decoded[0] if decoded else None
    else:
        s = dbg["sync"]
        print(f"sync: clock {s['clock_ppm']:+.2f} ppm, CFO {s['cfo_hz']:+.1f} Hz (int {s['cfo_int_bins']} bins), "
              f"symbols {s['n_sym']}, frame start {dbg['frame_start']}, TMCC parity {'ok' if dbg['tmcc_parity_ok'] else 'NG'}")
        dbg_first = dbg
    if dbg_first is not None:
        info = dbg_first["tmcc"]
        print(f"TMCC: partial reception {info.partial_reception}, switching {info.switching_indicator:04b}, "
              f"emergency {info.emergency_alarm}")
        for lyr in dbg_first["layers"]:
            print(f"  layer {lyr.name}: {lyr.mod.value} {lyr.rate} I={lyr.I} segments {lyr.segments[0]}..{lyr.segments[-1]}")

    base = Path(args.path)
    base = base.with_suffix("") if base.suffix in (".sigmf-meta", ".sigmf-data") else base
    out_dir = Path(args.out) if args.out else base.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, pk in out.items():
        nerr = dbg["rs_errors"].get(name)
        path = out_dir / f"{base.name}_layer{name}.ts"
        write_ts(path, pk)
        if nerr is not None and len(nerr):
            bad = int((nerr < 0).sum())
            fixed = int(((nerr > 0)).sum())
            ff = "" if args.track else f", first frame {dbg['first_frame'].get(name)}"
            print(f"  layer {name}: {len(pk)} packets -> {path}  RS: {fixed} corrected, {bad} uncorrectable "
                  f"({100 * bad / len(nerr):.2f} %){ff}")
        else:
            print(f"  layer {name}: no output")
    sync_ok = all(bool(np.all(pk[:, 0] == 0x47)) for pk in out.values() if len(pk))
    print("all sync bytes 0x47 (uncorrectable TSPs included):", sync_ok)

    if args.bts:
        if args.track:
            print("--bts は --track と併用できない（区間をまたぐフレーム整列が未対応）")
            return
        from isdbt import bts as B
        layers = [B.SlotLayer(lyr.name, len(lyr.segments), lyr.mod, lyr.rate) for lyr in dbg["layers"]]
        tpf = {lyr.name: P.tsp_per_frame(P.Mode(args.mode), lyr.mod, lyr.rate, len(lyr.segments))
               for lyr in dbg["layers"]}
        f0 = max(dbg["first_frame"].values())
        n_common = min(dbg["first_frame"][n] + len(out[n]) // tpf[n] for n in out) - f0
        if n_common <= 0:
            print("BTS: 全階層が揃うフレームが無い")
            return
        layer_tsps = {n: out[n][(f0 - dbg["first_frame"][n]) * tpf[n]:][: n_common * tpf[n]] for n in out}
        stream = B.build_bts(layer_tsps, P.Mode(args.mode), args.gi, layers)
        path = out_dir / f"{base.name}_bts.ts"
        stream.ravel().tofile(path)
        print(f"  BTS: {stream.shape[0]} TSP ({n_common} multiplex frames) -> {path}")


if __name__ == "__main__":
    main()
