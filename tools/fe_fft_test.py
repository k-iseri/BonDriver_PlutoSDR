#!/usr/bin/env python3
"""pluto-fw-isdbt v2b-1（FPGA 前段の FFT テストモード）の実機検証。

AD9361 のデジタルループバック（TX データポート → RX データポート、RF を通らない）で既知の
12bit I/Q パターンを TX から巡回送信し、FPGA の FFT 出力（モード 1: 窓ごとにヘッダ + 8192 ビン）を
ホストの numpy FFT と比べる。パターン長を窓の間隔（stride）と同じにすると、どの窓も同じサンプル列
（未知だが一定の巡回オフセット θ）を見るので、最初の窓で θ を探索し、以降の窓は同じ θ で一致するか見る。

  python tools/fe_fft_test.py --uri usb: --windows 8
  python tools/fe_fft_test.py --raw          # モード 0（pack8 の 8bit raw 経路）をループバックで確認

レジスタ（axi_ad9361 コア 0x3000〜、docs/design-v2.md / rtl/isdbt_regs.v）:
  0x1C CAPS, 0x20 MODE, 0x24 WIN_STRIDE, 0x28 WIN_OFFSET, 0x2C WIN_DECIM, 0x30 FE_STATUS, 0x34 FE_FRAMES
"""
import argparse
import os
import sys
import time
from pathlib import Path

import numpy as np

REG = 0x80000000
R_MAGIC, R_VERSION, R_FORMAT, R_CAPS = 0x3000, 0x3004, 0x3018, 0x301C
R_MODE, R_STRIDE, R_OFFSET, R_DECIM, R_STATUS, R_FRAMES = 0x3020, 0x3024, 0x3028, 0x302C, 0x3030, 0x3034
HDR_MAGIC = 0x49534254
N = 8192


def make_pattern(length: int, seed: int = 1) -> np.ndarray:
    """12bit 一様乱数の複素パターン（|re|,|im| ≤ 1000）。"""
    rng = np.random.default_rng(seed)
    re = rng.integers(-1000, 1001, length)
    im = rng.integers(-1000, 1001, length)
    return re + 1j * im


def parse_windows(buf: bytes, n: int = N):
    """バイト列から (header dict, bins complex int16) を取り出す。"""
    w = np.frombuffer(buf, dtype="<u4")
    out = []
    i = 0
    while i + 8 + n <= len(w):
        if w[i] != HDR_MAGIC:
            i += 1
            continue
        hdr = {"mode": int(w[i + 1] & 0xF), "dropped": int((w[i + 1] >> 8) & 1), "blk_exp": int((w[i + 1] >> 16) & 0xFF),
               "start": int(w[i + 2]) | (int(w[i + 3]) << 32), "seq": int(w[i + 4]), "n": int(w[i + 5])}
        d = w[i + 8:i + 8 + n].view("<i2").reshape(n, 2)
        out.append((hdr, d[:, 0].astype(np.float64) + 1j * d[:, 1].astype(np.float64)))
        i += 8 + n
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", default="usb:")
    ap.add_argument("--fs", type=float, default=8_126_984)
    ap.add_argument("--stride", type=int, default=9216)
    ap.add_argument("--decim", type=int, default=4)
    ap.add_argument("--windows", type=int, default=8)
    ap.add_argument("--raw", action="store_true", help="モード 0（raw 8bit）をループバックで確認する")
    ap.add_argument("--keep", action="store_true", help="終了時にモード/ループバックを戻さない")
    ap.add_argument("--dump", default=None, help="窓とパターンを .npz に保存（解析用）")
    args = ap.parse_args()

    os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]
    import iio
    ctx = iio.Context(args.uri)
    phy = ctx.find_device("ad9361-phy")
    rx = ctx.find_device("cf-ad9361-lpc")
    tx = ctx.find_device("cf-ad9361-dds-core-lpc")
    assert rx.reg_read(REG | R_MAGIC) == 0x49534442, "isdbt レジスタブロックが無い（純正ファーム？）"
    ver, caps, fmt = rx.reg_read(REG | R_VERSION), rx.reg_read(REG | R_CAPS), rx.reg_read(REG | R_FORMAT)
    print(f"isdbt regs: version {ver} caps 0x{caps:x} pack_shift {fmt & 0xFF}")

    # チップ設定（ループバックは RF を通らないので LO・ゲインは何でもよい）
    rxch = phy.find_channel("voltage0", False)
    rxch.attrs["sampling_frequency"].value = str(int(args.fs))
    rxch.attrs["gain_control_mode"].value = "manual"
    rxch.attrs["hardwaregain"].value = "0"
    phy.debug_attrs["loopback"].value = "1"

    # TX: パターンを巡回送信（int16 I,Q、12bit を [15:4] に）
    L = args.stride
    pat = make_pattern(L)
    tx_mask = iio.ChannelsMask(tx)
    tx_mask.channels = [tx.find_channel("voltage0", True), tx.find_channel("voltage1", True)]
    tx_buf = tx.get_buffer(0)
    tx_bs = iio.BufferStream(tx_buf, tx_mask)
    tx_bytes = np.empty(2 * L, dtype="<i2")
    tx_bytes[0::2] = (pat.real.astype(np.int64) << 4).astype(np.int16)
    tx_bytes[1::2] = (pat.imag.astype(np.int64) << 4).astype(np.int16)
    blk = tx_bs.create_block(tx_bytes.nbytes)
    blk.write(bytearray(tx_bytes.tobytes()))
    blk.enqueue(None, True)
    tx_bs.started = True
    time.sleep(0.3)

    rx_mask = iio.ChannelsMask(rx)
    rx_mask.channels = [rx.find_channel("voltage0", False), rx.find_channel("voltage1", False)]
    ok = True
    try:
        if args.raw:
            rx.reg_write(REG | R_MODE, 0)
            st = iio.Stream(rx.get_buffer(0), rx_mask, 1 << 18, nb_blocks=4)
            raw = None
            for i, b in enumerate(st):
                raw = np.frombuffer(b.read(), dtype=np.int8)
                if i >= 2:
                    break
            del st
            x = raw[0::2].astype(np.int64) + 1j * raw[1::2].astype(np.int64)
            shift = fmt & 0xFF
            # 期待値: pack8(pat)（round half away from zero、飽和）を巡回オフセット θ で
            def pack8(v):
                a = np.abs(v)
                r = (a + (1 << (shift - 1))) >> shift
                r = np.where(v < 0, -r, r)
                return np.clip(r, -128, 127)
            exp_i, exp_q = pack8(pat.real.astype(np.int64)), pack8(pat.imag.astype(np.int64))
            exp = exp_i + 1j * exp_q
            # θ 探索（相関）
            seg = x[:L]
            best = None
            for th in range(L):
                e = np.roll(exp, -th)
                err = np.count_nonzero(seg != e)
                if best is None or err < best[1]:
                    best = (th, err)
                if err == 0:
                    break
            th, err = best
            mism = np.count_nonzero(x[: (len(x) // L) * L] != np.tile(np.roll(exp, -th), len(x) // L))
            print(f"raw mode: theta {th}, mismatches {mism} / {(len(x) // L) * L} samples ({'PASS' if mism == 0 else 'FAIL'})")
            ok = mism == 0
        else:
            rx.reg_write(REG | R_MODE, 0)
            rx.reg_write(REG | R_STRIDE, args.stride)
            rx.reg_write(REG | R_OFFSET, 0)
            rx.reg_write(REG | R_DECIM, args.decim)
            rx.reg_write(REG | R_MODE, 1)
            time.sleep(0.2)
            st = iio.Stream(rx.get_buffer(0), rx_mask, 1 << 18, nb_blocks=4)   # 512 KB ブロック
            need = args.windows * (8 + N) * 4 + 2 * (8 + N) * 4
            chunks = []
            got = 0
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
            if not wins:
                sys.exit("no windows found")
            # θ 探索: 最初の窓
            h0, y0 = wins[0]
            print(f"first header: {h0}")
            best = None
            for th in range(L):
                xw = np.roll(pat, -th)[:N]
                ref = np.fft.fft(xw) / (2.0 ** h0["blk_exp"])
                err = np.sqrt(np.mean(np.abs(y0 - ref) ** 2))
                if best is None or err < best[1]:
                    best = (th, err)
            th, err0 = best
            if args.dump:
                np.savez(args.dump, pat=pat, theta=th, exps=np.array([h["blk_exp"] for h, _ in wins]),
                         starts=np.array([h["start"] for h, _ in wins]), seqs=np.array([h["seq"] for h, _ in wins]),
                         bins=np.array([y for _, y in wins]))
            print(f"theta {th}: rms error {err0:.3f} LSB (blk_exp {h0['blk_exp']})")
            # 以降の窓: 同じ θ（stride = L なので窓内容は同一）。
            # 判定は (1) 周波数領域の相対 rms 誤差 < 1e-3（BFP 16bit の丸めは −70 dB 台）、
            # (2) 逆 FFT で時間領域に戻したときの最大誤差 < 2 LSB（入力サンプルの一致 = 窓切り出しの正しさ）、
            # (3) 連番とサンプルカウンタが stride*decim ずつ進む（取りこぼし無し）
            xw = np.roll(pat, -th)[:N]
            worst_rel, worst_t = 0.0, 0.0
            prev = None
            for h, y in wins:
                ref = np.fft.fft(xw) / (2.0 ** h["blk_exp"])
                rel = np.sqrt(np.mean(np.abs(y - ref) ** 2)) / np.sqrt(np.mean(np.abs(ref) ** 2))
                worst_rel = max(worst_rel, rel)
                xt = np.fft.ifft(y * (2.0 ** h["blk_exp"]))
                worst_t = max(worst_t, float(np.max(np.abs(xt - xw))))
                if prev is not None:
                    if h["seq"] != prev["seq"] + 1 or h["start"] != prev["start"] + args.stride * args.decim:
                        print(f"  sequence/start mismatch: {prev} -> {h}")
                        ok = False
                if h["dropped"]:
                    print(f"  window seq {h['seq']} flagged dropped")
                    ok = False
                prev = h
            print(f"{len(wins)} windows: worst relative rms error {worst_rel:.2e} ({20 * np.log10(max(worst_rel, 1e-12)):.1f} dB), "
                  f"worst time-domain max error {worst_t:.2f} LSB")
            ok = ok and worst_rel < 1e-3 and worst_t < 2.0
            print("PASS" if ok else "FAIL")
    finally:
        if not args.keep:
            rx.reg_write(REG | R_MODE, 0)
            phy.debug_attrs["loopback"].value = "0"
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
