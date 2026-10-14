#!/usr/bin/env python3
"""FPGA 等化器（pluto-fw-isdbt モード 4 = 等化済みキャリア I/Q/CSI 各 8bit）の出力をホストの ref/ 後段で復号する。

  python tools/fe_eq_test.py --stim <dir>        # ループバックに合成信号を流して確認（TS 照合つき）
  python tools/fe_eq_test.py --rf --ch 24         # 実波

手順: モード 3（同期 FFT 8bit）を短く取り込んで整数 CFO δ を求め、EQ_CTRL に書いてモード 4 で取り込む。
FPGA からは窓（シンボル）ごとに 8 ワードのヘッダ + 4213 ワード（5617 × {I8, Q8, CSI8}）が届く。
ホストは等化済みキャリアをそのまま ref/ のデインターリーブ → ビタビ → RS に通す（tools/fe_eq_model.py の
EqSource / decode_equalized を流用）。MER は等化後のデータキャリアから測る。
"""
import argparse
import os
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ref"))
sys.path.insert(0, str(ROOT / "tools"))
from isdbt import deinterleave as DI, demod, measure, params as P  # noqa: E402
import fe_carrier_demod as FC  # noqa: E402
import fe_eq_model as EM  # noqa: E402

REG = 0x80000000
R_MAGIC, R_VERSION, R_MODE, R_DECIM, R_STATUS, R_FRAMES = 0x3000, 0x3004, 0x3020, 0x302C, 0x3030, 0x3034
R_SYNC_STATUS, R_SYNC_PEAK = 0x3038, 0x303C
R_EQ_CTRL, R_EQ_STATUS, R_EQ_OMEGA, R_TIM_EST, R_TIM_DELTA = 0x3040, 0x3044, 0x3048, 0x304C, 0x3050
HDR_MAGIC = 0x49534254
N, NG, T = 8192, 1024, 9216
FS = 512e6 / 63
TOTAL = 5617
EQ_WORDS = (TOTAL * 3 + 3) // 4          # 4213


def parse_eq_frames(buf: bytes):
    """バイト列 → [(hdr, D complex (5617,), csi (5617,))]。fmt = 2 のフレームだけ。"""
    w = np.frombuffer(buf, dtype="<u4")
    out = []
    i = 0
    while i + 8 <= len(w):
        if w[i] != HDR_MAGIC:
            i += 1
            continue
        fmt = (int(w[i + 1]) >> 24) & 3
        nw = EQ_WORDS if fmt == 2 else (N // 2 if fmt == 1 else N)
        if i + 8 + nw > len(w):
            break
        if fmt == 2:
            hdr = {"mode": int(w[i + 1] & 0xF), "dropped": int((w[i + 1] >> 8) & 1), "blk_exp": int((w[i + 1] >> 16) & 0xFF),
                   "start": int(w[i + 2]) | (int(w[i + 3]) << 32), "seq": int(w[i + 4]), "delta": int(np.int32(w[i + 5])),
                   "cfo_inc": int(np.int32(w[i + 6])), "status": int(w[i + 7])}
            b = w[i + 8:i + 8 + nw].view(np.uint8)[: TOTAL * 3].reshape(TOTAL, 3)
            D = (b[:, 0].astype(np.int8).astype(np.float64) + 1j * b[:, 1].astype(np.int8).astype(np.float64)) / 64.0
            csi = b[:, 2].astype(np.float64) / 64.0
            out.append((hdr, D, csi))
        i += 8 + nw
    return out


def eq_state_str(st: int, om: int, te: int, td: int) -> str:
    states = ["IDLE", "A", "B1", "B1T", "AFC", "CPEi", "CPEw", "DIV", "B2", "B2T", "FLUSH"]
    s = st >> 28
    return (f"eq state {states[s] if s < len(states) else s} sp_phase {(st >> 26) & 3} overrun {(st >> 25) & 1} "
            f"cpe {((st >> 12) & 0xFFF) / 4096 * 360:.1f}deg sym_count {st & 0xFFF}, omega {int(np.int32(np.uint32(om))) / 2 ** 32 * N / T:+.4f} bins, "
            f"T_est {te / 65536:.4f}, delta {int(np.int32(np.uint32(td))) / 256:+.2f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", default="usb:")
    ap.add_argument("--stim", default=None)
    ap.add_argument("--rf", action="store_true")
    ap.add_argument("--ch", type=int, default=24)
    ap.add_argument("--freq", type=float, default=None)
    ap.add_argument("--sec", type=float, default=2.5)
    ap.add_argument("--settle", type=float, default=3.0, help="モード 4 にしてから取り込むまでの秒数（時間軸の立ち上がり ≥ 256 シンボル）")
    ap.add_argument("--delta", type=int, default=None, help="整数 CFO δ を手で与える（省略時はモード 3 で推定）")
    ap.add_argument("--flags", type=lambda v: int(v, 0), default=0x3, help="EQ_CTRL bit8.. (bit0 AFC, bit1 CPE, bit2 SP 固定, bit[5:4] 位相)")
    ap.add_argument("--ts-ref", default=str(ROOT / "testdata" / "synthetic" / "std_mode3_gi8_layerB.ts"))
    ap.add_argument("--dump", default=None)
    ap.add_argument("--load", default=None)
    args = ap.parse_args()
    if args.load:
        z = np.load(args.load)
        frames = [({"start": int(z["start"][i]), "seq": int(z["seq"][i]), "delta": int(z["delta"][i])}, z["D"][i], z["csi"][i]) for i in range(len(z["seq"]))]
        analyze(frames, args)
        return

    os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]
    import iio
    ctx = iio.Context(args.uri)
    phy = ctx.find_device("ad9361-phy")
    rx = ctx.find_device("cf-ad9361-lpc")
    assert rx.reg_read(REG | R_MAGIC) == 0x49534442
    ver = rx.reg_read(REG | R_VERSION)
    if ver < 3 and rx.reg_read(REG | R_EQ_CTRL) == 0:
        sys.exit(f"firmware VERSION {ver} has no equalizer (need >= 3)")
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
        time.sleep(1.0)
    else:
        freq = args.freq if args.freq else 473_142_857.0 + 6_000_000.0 * (args.ch - 13)
        phy.find_channel("altvoltage0", True).attrs["frequency"].value = str(int(freq))
        rxch.attrs["gain_control_mode"].value = "manual"
        rxch.attrs["hardwaregain"].value = "26"
        phy.debug_attrs["loopback"].value = "0"
    time.sleep(0.3)

    rx_mask = iio.ChannelsMask(rx)
    rx_mask.channels = [rx.find_channel("voltage0", False), rx.find_channel("voltage1", False)]

    def capture(nbytes, blk_size=1 << 19):
        st = iio.Stream(rx.get_buffer(0), rx_mask, blk_size, nb_blocks=4)
        chunks, got = [], 0
        t0 = time.time()
        for b in st:
            data = bytes(b.read())
            chunks.append(data)
            got += len(data)
            if got >= nbytes:
                break
        dt = time.time() - t0
        del st
        return b"".join(chunks), dt

    try:
        rx.reg_write(REG | R_MODE, 0)
        rx.reg_write(REG | R_DECIM, 1)
        delta = args.delta
        if delta is None:
            # モード 3 で整数 CFO
            rx.reg_write(REG | R_MODE, 3)
            time.sleep(2.0)
            buf, _ = capture(40 * (8 + N // 2) * 4)
            fr = FC.parse_frames(buf)
            if len(fr) < 8:
                sys.exit("mode 3: too few frames (sync not locked?)")
            Yb = np.array([y for _, y in fr])
            delta = FC.integer_cfo_from_bins(Yb)
            rx.reg_write(REG | R_MODE, 0)
            time.sleep(0.2)
        print(f"integer CFO delta {delta}")
        rx.reg_write(REG | R_EQ_CTRL, (delta & 0xFF) | ((args.flags & 0xFF) << 8))
        rx.reg_write(REG | R_MODE, 4)
        time.sleep(args.settle)
        f0 = rx.reg_read(REG | R_FRAMES)
        time.sleep(0.5)
        f1 = rx.reg_read(REG | R_FRAMES)
        print(f"fe frames/s ~ {2 * (f1 - f0)} (expect {FS / T:.0f})")
        print("  " + FC.sync_state_str(rx.reg_read(REG | R_SYNC_STATUS), rx.reg_read(REG | R_SYNC_PEAK)))
        print("  " + eq_state_str(rx.reg_read(REG | R_EQ_STATUS), rx.reg_read(REG | R_EQ_OMEGA), rx.reg_read(REG | R_TIM_EST), rx.reg_read(REG | R_TIM_DELTA)))
        if f1 == f0:
            sys.exit("no frames")
        need = int(args.sec * FS / T) * (8 + EQ_WORDS) * 4
        buf, dt = capture(need)
        status = rx.reg_read(REG | R_STATUS)
        print(f"captured {len(buf) / 1e6:.1f} MB in {dt:.1f} s ({len(buf) / dt / 1e6:.1f} MB/s), overrun {status & 0xFFFF}, fifo_ovf {status >> 16}")
        print("  " + eq_state_str(rx.reg_read(REG | R_EQ_STATUS), rx.reg_read(REG | R_EQ_OMEGA), rx.reg_read(REG | R_TIM_EST), rx.reg_read(REG | R_TIM_DELTA)))
    finally:
        rx.reg_write(REG | R_MODE, 0)
        phy.debug_attrs["loopback"].value = "0"

    frames = parse_eq_frames(buf)
    print(f"frames {len(frames)}")
    if args.dump:
        np.savez_compressed(args.dump, D=np.array([d for _, d, _ in frames], dtype=np.complex64),
                            csi=np.array([c for _, _, c in frames], dtype=np.float32),
                            **{k: np.array([h[k] for h, _, _ in frames], dtype=np.int64) for k in ("start", "seq", "delta")})
    analyze(frames, args)


def analyze(frames, args):
    if len(frames) < 300:
        sys.exit("too few frames")
    seqs = np.array([h["seq"] for h, _, _ in frames])
    starts = np.array([h["start"] for h, _, _ in frames])
    deltas = np.array([h["delta"] for h, _, _ in frames]) / 256.0
    gaps = np.where(np.diff(seqs) != 1)[0]
    print(f"seq gaps: {len(gaps)}, start diffs: {np.unique(np.diff(starts))[:6].tolist()}, delta: mean {deltas.mean():+.2f} std {deltas.std():.2f} samples")
    bounds = np.concatenate([[0], gaps + 1, [len(frames)]])
    runs = [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]
    a, b = max(runs, key=lambda r: r[1] - r[0])
    frames = frames[a:b]
    print(f"using {b - a} contiguous symbols")
    D = np.array([d for _, d, _ in frames])
    csi = np.array([c for _, _, c in frames])
    print(f"|D| median {np.median(np.abs(D)):.3f}, csi median {np.median(csi):.3f}, csi zero fraction {np.mean(csi == 0):.4f}")
    mode = P.JP_DEFAULT_MODE
    class _A:
        skip = 0
        carrier_mer = False
    EM.args_global = _A()
    ts_ref = None
    if args.stim and Path(args.ts_ref).exists():
        r = np.fromfile(args.ts_ref, dtype=np.uint8)
        ts_ref = {bytes(p) for p in r[: len(r) // 188 * 188].reshape(-1, 188)}
    EM.report("fpga eq       ", D, csi, mode, ts_ref)


if __name__ == "__main__":
    main()
