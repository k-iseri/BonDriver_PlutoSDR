#!/usr/bin/env python3
"""合成試験信号を testdata/synthetic/ に生成する。

日本の標準運用（Mode 3、GI 1/8、A: 1 セグ QPSK 2/3 I=4、B: 12 セグ 64QAM 3/4 I=2）で
ランダム TS を変調し、SigMF（cf32_le）と入力 TS、設定 JSON を書き出す。
--cn を指定すると AWGN を加えた版も出す。
"""
import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ref"))

from isdbt import params as P
from isdbt.channel import ChannelConfig, apply_channel
from isdbt.io import write_sigmf, write_ts
from isdbt.modulator import TxConfig, frames_for, modulate

FS = float(P.SAMPLE_RATE_HZ)


def make_test_ts(n_packets: int, seed: int, pid: int = 0x100) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pk = rng.integers(0, 256, size=(n_packets, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    pk[:, 1] = (pid >> 8) & 0x1F
    pk[:, 2] = pid & 0xFF
    pk[:, 3] = 0x10 | (np.arange(n_packets) % 16)
    return pk


def standard_config() -> TxConfig:
    return TxConfig(mode=P.Mode.MODE3, gi="1/8", layers={
        "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 4},
        "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 2},
    })


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="testdata/synthetic")
    ap.add_argument("--seconds", type=float, default=2.0)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--cn", type=float, default=None, help="AWGN 版も生成する C/N [dB]")
    ap.add_argument("--save-carriers", action="store_true", help="IFFT 前のキャリア行列を .npy で保存")
    args = ap.parse_args()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    cfg = standard_config()
    frame_s = P.SYMBOLS_PER_FRAME * P.symbol_samples(cfg.mode, cfg.gi) / FS
    _, extra = frames_for(cfg, {})
    data_frames = max(1, int(np.ceil(args.seconds / frame_s)) - extra)
    ts = {}
    for i, (name, lc) in enumerate(cfg.layers.items()):
        n = data_frames * P.tsp_per_frame(cfg.mode, lc.mod, lc.rate, lc.segments)
        ts[name] = make_test_ts(n, args.seed * 10 + i, pid=0x100 + i)

    t0 = time.time()
    iq, dbg = modulate(ts, cfg, return_debug=True)
    print(f"modulated {dbg['n_frames']} frames ({len(iq)} samples, {len(iq) / FS:.2f} s) in {time.time() - t0:.1f} s")

    base = out / "std_mode3_gi8_clean"
    write_sigmf(base, iq, FS, extra={"isdbt:config": json.dumps(cfg_to_json(cfg))})
    for name, pk in ts.items():
        write_ts(out / f"std_mode3_gi8_layer{name}.ts", pk)
    if args.save_carriers:
        np.save(out / "std_mode3_gi8_carriers.npy", dbg["carriers"])
    (out / "std_mode3_gi8_config.json").write_text(json.dumps(cfg_to_json(cfg), indent=2, ensure_ascii=False))
    print("wrote", base.with_suffix(".sigmf-data"))

    if args.cn is not None:
        noisy = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=args.cn, seed=args.seed))
        nb = out / f"std_mode3_gi8_awgn{int(args.cn)}dB"
        write_sigmf(nb, noisy, FS, extra={"isdbt:config": json.dumps(cfg_to_json(cfg)), "isdbt:cn_db": args.cn})
        print("wrote", nb.with_suffix(".sigmf-data"))


def cfg_to_json(cfg: TxConfig) -> dict:
    return {"mode": cfg.mode.value, "gi": cfg.gi, "partial_reception": cfg.partial_reception,
            "layers": {n: {"segments": lc.segments, "mod": lc.mod.value, "rate": lc.rate, "I": lc.I}
                       for n, lc in cfg.layers.items()}}


if __name__ == "__main__":
    main()
