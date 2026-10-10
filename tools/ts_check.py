#!/usr/bin/env python3
"""TS ファイルの健全性検査: 同期バイト、PID 分布、継続カウンタ欠落、PAT 有無。"""
import argparse
import sys
from collections import Counter
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ref"))
from isdbt.io import read_ts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    args = ap.parse_args()
    pk = read_ts(args.path)
    print(f"packets: {len(pk)}")
    sync_bad = int(np.sum(pk[:, 0] != 0x47))
    print(f"sync errors: {sync_bad}")
    pid = ((pk[:, 1].astype(int) & 0x1F) << 8) | pk[:, 2]
    cc = pk[:, 3] & 0x0F
    tei = (pk[:, 1] & 0x80) != 0
    print(f"TEI set: {int(tei.sum())}")
    cnt = Counter(pid.tolist())
    print("top PIDs:", cnt.most_common(10))
    print("PAT present:", 0 in cnt)
    # 継続カウンタ欠落（PID ごと、ペイロードありのみ）
    has_payload = (pk[:, 3] & 0x10) != 0
    drops = 0
    last = {}
    for p, c, hp in zip(pid, cc, has_payload):
        if p == 0x1FFF or not hp:
            continue
        if p in last and (last[p] + 1) % 16 != c:
            drops += 1
        last[p] = c
    print(f"continuity errors: {drops}")


if __name__ == "__main__":
    main()
