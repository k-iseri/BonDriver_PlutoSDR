#!/usr/bin/env python3
"""pluto-fw-isdbt v2b-2（FPGA 同期 + FFT）の実機検証。

make_sync_stimulus.py --seamless --tx-bin で作った合成 ISDB-T（CFO 付き、長さ = シンボル長の整数倍）を
AD9361 のデジタルループバックで巡回送信し、モード 2（同期）のヘッダから
  - 窓開始位置が毎シンボル 9216 ずつ進む（追従）、ロック、CFO 推定（小数部）
  - FFT 窓の中身（逆 FFT）が送信パターンの該当区間と一致（逆回転込み）
を確認する。

  python tools/fe_sync_test.py <stim_dir> [--uri usb:] [--windows 16]
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

REG = 0x80000000
R_MAGIC, R_CAPS = 0x3000, 0x301C
R_MODE, R_STRIDE, R_OFFSET, R_DECIM, R_STATUS, R_FRAMES = 0x3020, 0x3024, 0x3028, 0x302C, 0x3030, 0x3034
HDR_MAGIC = 0x49534254
N, NG, T = 8192, 1024, 9216
FS = 512e6 / 63


def parse_windows(buf: bytes):
    w = np.frombuffer(buf, dtype="<u4")
    out = []
    i = 0
    while i + 8 + N <= len(w):
        if w[i] != HDR_MAGIC:
            i += 1
            continue
        hdr = {"mode": int(w[i + 1] & 0xF), "dropped": int((w[i + 1] >> 8) & 1), "blk_exp": int((w[i + 1] >> 16) & 0xFF),
               "start": int(w[i + 2]) | (int(w[i + 3]) << 32), "seq": int(w[i + 4]), "n": int(w[i + 5]),
               "cfo_inc": int(np.int32(w[i + 6])), "status": int(w[i + 7])}
        d = w[i + 8:i + 8 + N].view("<i2").reshape(N, 2)
        out.append((hdr, d[:, 0].astype(np.float64) + 1j * d[:, 1].astype(np.float64)))
        i += 8 + N
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stim_dir")
    ap.add_argument("--uri", default="usb:")
    ap.add_argument("--decim", type=int, default=4)
    ap.add_argument("--windows", type=int, default=16)
    ap.add_argument("--settle", type=float, default=2.0, help="モード 2 にしてから取り込むまでの秒数（捕捉・平均化）")
    args = ap.parse_args()
    sd = Path(args.stim_dir)
    meta = json.loads((sd / "sync_stim.json").read_text())
    tx = np.fromfile(sd / "sync_stim.bin", dtype="<i2")
    pat = (tx[0::2].astype(np.int64) >> 4) + 1j * (tx[1::2].astype(np.int64) >> 4)
    L = len(pat)
    assert L % T == 0, L

    os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]
    import iio
    ctx = iio.Context(args.uri)
    phy = ctx.find_device("ad9361-phy")
    rx = ctx.find_device("cf-ad9361-lpc")
    txd = ctx.find_device("cf-ad9361-dds-core-lpc")
    assert rx.reg_read(REG | R_MAGIC) == 0x49534442
    caps = rx.reg_read(REG | R_CAPS)
    print(f"caps 0x{caps:x}")
    rxch = phy.find_channel("voltage0", False)
    rxch.attrs["sampling_frequency"].value = str(int(round(FS)))
    rxch.attrs["gain_control_mode"].value = "manual"
    rxch.attrs["hardwaregain"].value = "0"
    phy.debug_attrs["loopback"].value = "1"

    tx_mask = iio.ChannelsMask(txd)
    tx_mask.channels = [txd.find_channel("voltage0", True), txd.find_channel("voltage1", True)]
    tx_bs = iio.BufferStream(txd.get_buffer(0), tx_mask)
    blk = tx_bs.create_block(tx.nbytes)
    blk.write(bytearray(tx.tobytes()))
    blk.enqueue(None, True)
    tx_bs.started = True
    time.sleep(0.3)

    rx_mask = iio.ChannelsMask(rx)
    rx_mask.channels = [rx.find_channel("voltage0", False), rx.find_channel("voltage1", False)]
    ok = True
    try:
        rx.reg_write(REG | R_MODE, 0)
        rx.reg_write(REG | R_DECIM, args.decim)
        rx.reg_write(REG | R_MODE, 2)
        time.sleep(args.settle)
        st = iio.Stream(rx.get_buffer(0), rx_mask, 1 << 18, nb_blocks=4)
        need = (args.windows + 2) * (8 + N) * 4
        chunks, got = [], 0
        for b in st:
            data = bytes(b.read())
            chunks.append(data)
            got += len(data)
            if got >= need:
                break
        del st
        status, frames = rx.reg_read(REG | R_STATUS), rx.reg_read(REG | R_FRAMES)
        print(f"fe: frames {frames}, overrun {status & 0xFFFF}, fifo_ovf {status >> 16}")
        wins = parse_windows(b"".join(chunks))
        print(f"parsed {len(wins)} windows")
        if len(wins) < 4:
            sys.exit("too few windows")
        starts = np.array([h["start"] for h, _ in wins])
        seqs = np.array([h["seq"] for h, _ in wins])
        locks = np.array([(h["status"] >> 16) & 1 for h, _ in wins])
        incs = np.array([h["cfo_inc"] for h, _ in wins])
        d = np.diff(starts)
        print(f"start diffs: {d[:8].tolist()} (expected {T * args.decim})")
        print(f"locked: {locks.tolist()}")
        frac = meta["cfo_bins"] - round(meta["cfo_bins"])
        est = -incs[-1] * N / 2 ** 32
        print(f"cfo estimate {est:.4f} bins (true fractional {frac:.4f})")
        ok = ok and np.all(locks[2:] == 1) and np.all(np.abs(d - T * args.decim) <= 2 * args.decim) and abs(est - frac) < 0.02
        # 窓の中身: 逆 FFT → 送信パターンのどこか（巡回オフセット θ）と、逆回転 e^{-j2π ε n/N} 込みで一致するか
        h0, y0 = wins[-1]
        xt = np.fft.ifft(y0 * (2.0 ** h0["blk_exp"]))
        # 逆回転の位相は NCO の絶対位相（不明）を含むので、振幅と「微分位相」で突き合わせる: |xt| と |pat| の相関で θ を探す
        a = np.abs(xt)
        best = None
        for th in range(0, L, 1):
            seg = np.abs(pat[(th + np.arange(N)) % L])
            err = np.sum((a - seg) ** 2)
            if best is None or err < best[1]:
                best = (th, err)
            if err < 0.05 * np.sum(seg ** 2):
                break
        th, err = best
        seg = pat[(th + np.arange(N)) % L]
        # 位相: xt ≈ seg · e^{j(φ0 − 2π ε n/N)}（ε = 推定 CFO）→ 残差を見る
        n = np.arange(N)
        rot = seg * np.exp(-2j * np.pi * est * n / N)
        g = np.vdot(rot, xt) / np.vdot(rot, rot)
        resid = np.sqrt(np.mean(np.abs(xt - g * rot) ** 2)) / np.sqrt(np.mean(np.abs(xt) ** 2))
        # 窓位置の妥当性: θ がシンボル境界（有効部分開始 − margin）に近いか
        pos_in_sym = (th + 128) % T
        print(f"window theta {th}: offset in symbol {pos_in_sym} (useful part starts at {NG}; want ~{NG} within GI), "
              f"|gain| {abs(g):.3f}, relative residual {resid:.3e}")
        ok = ok and resid < 0.05 and (NG - 300 <= pos_in_sym <= NG + 40)
        print("PASS" if ok else "FAIL")
    finally:
        rx.reg_write(REG | R_MODE, 0)
        phy.debug_attrs["loopback"].value = "0"
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
