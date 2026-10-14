#!/usr/bin/env python3
"""FPGA 前段（pluto-fw-isdbt モード 3 = 同期済み FFT、8bit ビン）の出力をホストの ref/ 後段で復号する。

  python tools/fe_carrier_demod.py --stim <dir>   # ループバックに合成信号（make_sync_stimulus --seamless --tx-bin）を流して確認
  python tools/fe_carrier_demod.py --rf --ch 24    # 実波（RX 端子）で確認

FPGA からは窓（= OFDM シンボル、GI の中で開いた FFT 窓）ごとに 8192 ビン（I/Q 8bit、BFP 指数つき）が
連番とサンプルカウンタ付きで届く。ホストは
  1. 連番が連続する区間をシンボル行列 Y_bins (n_sym, 8192) にする（指数で共通スケールに）
  2. 整数 CFO（ビンのずれ）を帯域エネルギーから推定し、有効キャリア 5617 本を切り出す
  3. ref/isdbt の frame_sync → 等化 → デインターリーブ → ビタビ → RS（既存の浮動小数点パイプライン）
  4. MER・RS 誤りを出し、--stim なら TS を合成信号の元 TS と照合する
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ref"))
from isdbt import demod, measure, params as P  # noqa: E402

REG = 0x80000000
R_MAGIC, R_MODE, R_DECIM, R_STATUS, R_FRAMES = 0x3000, 0x3020, 0x302C, 0x3030, 0x3034
HDR_MAGIC = 0x49534254
N, NG, T = 8192, 1024, 9216
FS = 512e6 / 63
TOTAL = 5617
CENTER = 2808


def parse_frames(buf: bytes):
    """バイト列 → [(hdr, bins complex (8192,) float)]。8bit/16bit 両対応。"""
    w = np.frombuffer(buf, dtype="<u4")
    out = []
    i = 0
    while i + 8 <= len(w):
        if w[i] != HDR_MAGIC:
            i += 1
            continue
        fmt8 = (int(w[i + 1]) >> 24) & 1
        nw = N // 2 if fmt8 else N
        if i + 8 + nw > len(w):
            break
        hdr = {"mode": int(w[i + 1] & 0xF), "dropped": int((w[i + 1] >> 8) & 1), "blk_exp": int((w[i + 1] >> 16) & 0xFF),
               "start": int(w[i + 2]) | (int(w[i + 3]) << 32), "seq": int(w[i + 4]),
               "cfo_inc": int(np.int32(w[i + 6])), "status": int(w[i + 7])}
        if fmt8:
            d = w[i + 8:i + 8 + nw].view(np.int8).reshape(N, 2).astype(np.float64) * 16.0   # 8bit は 12bit の /16
        else:
            d = w[i + 8:i + 8 + nw].view("<i2").reshape(N, 2).astype(np.float64)
        out.append((hdr, (d[:, 0] + 1j * d[:, 1]) * (2.0 ** hdr["blk_exp"])))
        i += 8 + nw
    return out


class CarrierMatrixSource:
    """ref/isdbt.demod が要求する CarrierSource 互換の最小実装（symbols(a, b) と n_sym）。"""

    def __init__(self, Y: np.ndarray, info: dict):
        self.Y = Y.astype(np.complex64)
        self.n_sym = Y.shape[0]
        self._info = info

    def symbols(self, a: int, b: int) -> np.ndarray:
        return self.Y[a:b]

    def info(self) -> dict:
        return self._info


def integer_cfo_from_bins(Yb: np.ndarray, search: int = 100) -> int:
    """fftshift 後のビン列で、5617 本の帯域がどこにあるか（中心からのずれ δ）を平均電力の箱型相関で求める。"""
    p = np.mean(np.abs(Yb) ** 2, axis=0)             # (8192,)
    ps = np.fft.fftshift(p)                           # 中心 = DC
    best = None
    for d in range(-search, search + 1):
        lo = N // 2 - CENTER + d
        e = ps[lo:lo + TOTAL].sum()
        if best is None or e > best[1]:
            best = (d, e)
    return best[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", default="usb:")
    ap.add_argument("--stim", default=None, help="ループバック用スティミュラスのディレクトリ（sync_stim.bin / .json）")
    ap.add_argument("--rf", action="store_true", help="RX 端子の実波を使う（--ch / --freq、ソフト AGC）")
    ap.add_argument("--ch", type=int, default=24)
    ap.add_argument("--freq", type=float, default=None)
    ap.add_argument("--sec", type=float, default=2.0, help="取り込む秒数")
    ap.add_argument("--settle", type=float, default=2.0)
    ap.add_argument("--ts-ref", default=str(ROOT / "testdata" / "synthetic" / "std_mode3_gi8_layerB.ts"))
    args = ap.parse_args()

    os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]
    import iio
    ctx = iio.Context(args.uri)
    phy = ctx.find_device("ad9361-phy")
    rx = ctx.find_device("cf-ad9361-lpc")
    assert rx.reg_read(REG | R_MAGIC) == 0x49534442
    rxch = phy.find_channel("voltage0", False)
    rxch.attrs["sampling_frequency"].value = str(int(round(FS)))
    rxch.attrs["rf_bandwidth"].value = "5600000"
    tx_bs = None
    if args.stim:
        sd = Path(args.stim)
        tx = np.fromfile(sd / "sync_stim.bin", dtype="<i2")
        txd = ctx.find_device("cf-ad9361-dds-core-lpc")
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
    else:
        freq = args.freq if args.freq else 473_142_857.0 + 6_000_000.0 * (args.ch - 13)
        phy.find_channel("altvoltage0", True).attrs["frequency"].value = str(int(freq))
        rxch.attrs["gain_control_mode"].value = "manual"
        rxch.attrs["hardwaregain"].value = "26"
        phy.debug_attrs["loopback"].value = "0"
    time.sleep(0.3)

    rx_mask = iio.ChannelsMask(rx)
    rx_mask.channels = [rx.find_channel("voltage0", False), rx.find_channel("voltage1", False)]
    try:
        rx.reg_write(REG | R_MODE, 0)
        rx.reg_write(REG | R_DECIM, 1)
        rx.reg_write(REG | R_MODE, 3)
        time.sleep(args.settle)
        f0 = rx.reg_read(REG | R_FRAMES)
        time.sleep(0.5)
        f1 = rx.reg_read(REG | R_FRAMES)
        print(f"fe frames/s ≈ {2 * (f1 - f0)} (expect {FS / T:.0f})")
        if f1 == f0:
            sys.exit("no windows (sync not locked)")
        st = iio.Stream(rx.get_buffer(0), rx_mask, 1 << 19, nb_blocks=4)    # 1 MB ブロック
        need = int(args.sec * FS / T) * (8 + N // 2) * 4
        chunks, got = [], 0
        t0 = time.time()
        for b in st:
            data = bytes(b.read())
            chunks.append(data)
            got += len(data)
            if got >= need:
                break
        dt = time.time() - t0
        del st
        status = rx.reg_read(REG | R_STATUS)
        print(f"captured {got / 1e6:.1f} MB in {dt:.1f} s ({got / dt / 1e6:.1f} MB/s), overrun {status & 0xFFFF}, fifo_ovf {status >> 16}")
    finally:
        rx.reg_write(REG | R_MODE, 0)
        phy.debug_attrs["loopback"].value = "0"

    frames = parse_frames(b"".join(chunks))
    print(f"frames {len(frames)}")
    if len(frames) < 300:
        sys.exit("too few frames")
    seqs = np.array([h["seq"] for h, _ in frames])
    starts = np.array([h["start"] for h, _ in frames])
    locks = np.array([(h["status"] >> 16) & 1 for h, _ in frames])
    gaps = np.where(np.diff(seqs) != 1)[0]
    print(f"seq gaps: {len(gaps)} (USB 取りこぼし), start diffs: {np.unique(np.diff(starts))[:6].tolist()}, locked {locks.mean():.2f}")
    # 最長の連続区間
    bounds = np.concatenate([[0], gaps + 1, [len(frames)]])
    runs = [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]
    a, b = max(runs, key=lambda r: r[1] - r[0])
    frames = frames[a:b]
    print(f"using {b - a} contiguous symbols")
    Yb = np.array([y for _, y in frames])                     # (n_sym, 8192) ビン順（DC = 0）
    delta = integer_cfo_from_bins(Yb)
    print(f"integer CFO (bin shift) {delta}")
    Ys = np.fft.fftshift(Yb, axes=1)
    lo = N // 2 - CENTER + delta
    Y = Ys[:, lo:lo + TOTAL]                                  # (n_sym, 5617) キャリア k=0..5616
    mode = P.JP_DEFAULT_MODE
    src = CarrierMatrixSource(Y, {"source": "fpga-mode3", "int_cfo": delta})
    fsi = demod.frame_sync(src, mode)
    print(f"frame_start {fsi['frame_start']}, tmcc parity ok {fsi['tmcc_parity_ok']}, "
          f"layers {[(l.name, l.mod.name, l.rate, l.I, len(l.segments)) for l in fsi['layers']]}")
    # MER（measure.rx_quality と同じ要領）
    H, bad = demod.channel_estimate(Y, mode, fsi["frame_start"], return_quality=True)
    D, _csi = demod.equalize(Y, H)
    Yl = demod.DI.data_to_logical(D, mode, fsi["frame_start"])
    hist = max((-(-demod.DI.time_deinterleave_max_delay(l.I) // P.SYMBOLS_PER_FRAME) for l in fsi["layers"]), default=0)
    r0 = fsi["frame_start"] + hist * P.SYMBOLS_PER_FRAME
    rows = np.arange(r0, Y.shape[0])
    for l in fsi["layers"]:
        slots = Yl[rows][:, l.segments, :].reshape(-1)
        print(f"  MER {l.name} ({l.mod.name}): {measure.mer_db(slots, l.mod):.2f} dB")
    # 復号
    out, dbg = demod._decode_source(src, mode, 6)
    for name, pk in out.items():
        nerr = np.asarray(dbg["rs_errors"].get(name, []))
        print(f"  layer {name}: {len(pk)} TS packets, RS uncorrectable {int(np.sum(nerr < 0))} / {len(nerr)}")
    if args.stim and "B" in out and Path(args.ts_ref).exists():
        ref_ts = np.fromfile(args.ts_ref, dtype=np.uint8)
        ref_ts = ref_ts[: len(ref_ts) // 188 * 188].reshape(-1, 188)
        got = out["B"]
        ref_set = {bytes(p) for p in ref_ts}
        hit = sum(1 for p in got if bytes(p) in ref_set)
        print(f"  layer B packets found in the modulator's TS: {hit} / {len(got)} ({100.0 * hit / max(len(got), 1):.1f}%)")
        print("PASS" if hit >= 0.98 * len(got) and len(got) > 100 else "FAIL")


if __name__ == "__main__":
    main()
