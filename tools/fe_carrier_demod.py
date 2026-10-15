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
from isdbt import demod, measure, params as P, pilots  # noqa: E402

REG = 0x80000000
R_MAGIC, R_MODE, R_DECIM, R_STATUS, R_FRAMES = 0x3000, 0x3020, 0x302C, 0x3030, 0x3034
R_SYNC_STATUS, R_SYNC_PEAK = 0x3038, 0x303C
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


def sync_state_str(sy: int, pk: int) -> str:
    """SYNC_STATUS / SYNC_PEAK（ファーム v2b3a-diag 以降。旧ファームは 0）を人が読める形に。"""
    st = ("ACQ", "TRACK", "?", "?")[sy >> 30]
    return (f"sync {st} search {(sy >> 29) & 1} locked {(sy >> 28) & 1} miss {(sy >> 24) & 0xF} "
            f"acq_fail {(sy >> 16) & 0xFF} lock_losses {sy & 0xFFFF} avg_peak_hi {pk >> 16} peak_hi {pk & 0xFFFF}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", default="usb:")
    ap.add_argument("--stim", default=None, help="ループバック用スティミュラスのディレクトリ（sync_stim.bin / .json）")
    ap.add_argument("--rf", action="store_true", help="RX 端子の実波を使う（--ch / --freq、ソフト AGC）")
    ap.add_argument("--ch", type=int, default=24)
    ap.add_argument("--freq", type=float, default=None)
    ap.add_argument("--gain", type=float, default=26.0, help="--rf の手動ゲイン dB（レベル依存の切り分け用）")
    ap.add_argument("--sec", type=float, default=2.0, help="取り込む秒数")
    ap.add_argument("--settle", type=float, default=2.0)
    ap.add_argument("--ts-ref", default=str(ROOT / "testdata" / "synthetic" / "std_mode3_gi8_layerB.ts"))
    ap.add_argument("--dump", default=None, help="取り込んだ窓（ヘッダ + ビン）を npz に保存")
    ap.add_argument("--ts-out", default=None, help="復号した階層 TS を npz（A, B: (n, 188) uint8）に保存")
    ap.add_argument("--load", default=None, help="取り込みの代わりに --dump した npz を読む（オフライン解析）")
    args = ap.parse_args()
    if args.load:
        z = np.load(args.load)
        Yd = z["Y"]                                   # NpzFile は添字ごとに読み直すので一度だけ取り出す
        hd = {k: z[k] for k in ("mode", "dropped", "blk_exp", "start", "seq", "cfo_inc", "status")}
        frames = [({k: int(hd[k][i]) for k in hd}, Yd[i]) for i in range(len(hd["seq"]))]
        analyze(frames, args)
        return

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
        time.sleep(1.0)      # 大きい TX ブロック（1 フレーム 7.5 MB）は走り出すまで時間がかかる
    else:
        freq = args.freq if args.freq else 473_142_857.0 + 6_000_000.0 * (args.ch - 13)
        phy.find_channel("altvoltage0", True).attrs["frequency"].value = str(int(freq))
        rxch.attrs["gain_control_mode"].value = "manual"
        rxch.attrs["hardwaregain"].value = str(int(round(args.gain)))
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
        print(f"fe frames/s ~ {2 * (f1 - f0)} (expect {FS / T:.0f})")
        print("  " + sync_state_str(rx.reg_read(REG | R_SYNC_STATUS), rx.reg_read(REG | R_SYNC_PEAK)))
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
    if args.dump:
        np.savez_compressed(args.dump, Y=np.array([y for _, y in frames], dtype=np.complex64),
                            **{k: np.array([h[k] for h, _ in frames], dtype=np.int64)
                               for k in ("mode", "dropped", "blk_exp", "start", "seq", "cfo_inc", "status")})
    analyze(frames, args)


def analyze(frames, args):
    if len(frames) < 300:
        sys.exit("too few frames")
    seqs = np.array([h["seq"] for h, _ in frames])
    starts = np.array([h["start"] for h, _ in frames])
    locks = np.array([(h["status"] >> 16) & 1 for h, _ in frames])
    gaps = np.where(np.diff(seqs) != 1)[0]
    print(f"seq gaps: {len(gaps)} (USB 取りこぼし), start diffs: {np.unique(np.diff(starts))[:6].tolist()}, locked {locks.mean():.2f}")
    be = np.array([h["blk_exp"] for h, _ in frames])
    yb = np.array([y[N // 2 - 2808:N // 2 + 2809] for _, y in frames[:200]]) / (2.0 ** be[:200, None])
    print(f"blk_exp {be.min()}..{be.max()}, FFT 出力ビン（12bit 単位、有効キャリア）: rms {np.sqrt(np.mean(np.abs(yb) ** 2) / 2):.1f} LSB/成分, "
          f"max |re| {np.abs(yb.real).max():.0f}, |re| < 8 の割合 {np.mean(np.abs(yb.real) < 8):.3f}")
    # 最長の連続区間
    bounds = np.concatenate([[0], gaps + 1, [len(frames)]])
    runs = [(bounds[i], bounds[i + 1]) for i in range(len(bounds) - 1)]
    a, b = max(runs, key=lambda r: r[1] - r[0])
    frames = frames[a:b]
    print(f"using {b - a} contiguous symbols")
    Yb = np.array([y for _, y in frames])                     # (n_sym, 8192) ビン順（DC = 0）
    delta = integer_cfo_from_bins(Yb)
    print(f"integer CFO (bin shift) {delta}")
    mode = P.JP_DEFAULT_MODE
    # ref/isdbt/sync.active_carriers と同じ: キャリア k はビン (carrier_to_fft_bin(k) + δ) mod N。
    # 整数 CFO は時間領域では e^{j2π δ n/N} の回転なので、窓の開始位置 s に応じた位相 e^{-j2π δ s/N} で打ち消す
    bins = (P.carrier_to_fft_bin(mode, np.arange(TOTAL)) + delta) % N
    starts_w = np.array([h["start"] for h, _ in frames], dtype=np.float64)
    ph = np.exp(-2j * np.pi * delta * (starts_w % N) / N)
    # 窓開始の揺れの補償: FPGA の追従は窓開始を整数サンプルで動かす（雑音で ±数サンプル/シンボル）。窓を Δ だけ
    # 遅らせるとビン b（符号付き）の位相は +2π b Δ / N 回り、帯域端（|b| ≈ 2800）では 1 サンプルで 2.1 rad なので
    # チャネル推定の時間補間が壊れる（A 層 17 dB、B 層 −16 dB になった）。ref/isdbt/sync.py と同じ方針で、
    # 直線フィット（クロック偏差で窓が信号に追従して流れる分）はそのまま残す: 追従した窓は信号に対して一定位置
    # （GI 内 142 サンプル前）なのでチャネルは静的で、位相傾斜はチャネル推定が吸収する。公称 s_0 + nT からの
    # 偏差を全部消すと、逆に「流れる窓」を作ってしまい帯域端が崩れる（実波で B 層 19 → 15 dB になった）。
    # 消すのはフィットからの偏差（追従の揺れ）だけ
    n_idx = np.arange(len(starts_w), dtype=np.float64)
    rel = starts_w - starts_w[0]
    coef = np.polyfit(n_idx, rel, 1)
    dev = rel - np.polyval(coef, n_idx)
    print(f"window start: clock drift {coef[0] - T:+.4f} samples/symbol ({(coef[0] - T) / T * 1e6:+.1f} ppm), "
          f"jitter vs linear fit std {dev.std():.2f}, max |dev| {np.abs(dev).max():.1f} samples")
    b_signed = ((bins + N // 2) % N) - N // 2
    jit = np.exp(-2j * np.pi * np.outer(dev, b_signed) / N)
    Y = Yb[:, bins] * ph[:, None] * jit                       # (n_sym, 5617) キャリア k=0..5616
    # 残留 CFO の補償: FPGA の NCO は GI 相関位相の IIR から小数 CFO を決めるが 0.01〜0.02 ビンの誤差が残る
    # （実波では XO ドリフトも）。これはシンボルごとの共通位相回転（0.016 ビンで 0.11 rad/シンボル）になり、
    # 8 シンボル平均のチャネル推定格子（隣の格子点は別シンボル由来）がジグザグになってデータキャリアの補間だけが
    # 崩れる（パイロット位置は自シンボルで補正されるので MER 29 dB、データは 17 dB になった）。SP は 4 シンボル周期で
    # 同じ値なので Σ_k Y[n+4,k]·conj(Y[n,k]) の位相 / 4 が 1 シンボルあたりの回転。平滑化して積算し、逆回転する
    fs0 = demod.frame_sync(CarrierMatrixSource(Y, {}), mode)["frame_start"]
    n_sym = Y.shape[0]
    rot = np.zeros(n_sym)
    for n in range(n_sym - 4):
        k = pilots.sp_positions(mode, (n - fs0) % P.SP_SYMBOL_PERIOD)
        rot[n] = np.angle(np.vdot(Y[n, k], Y[n + 4, k])) / 4.0
    rot[n_sym - 4:] = rot[n_sym - 5]
    win = 33
    rot_s = np.convolve(rot, np.ones(win) / win, mode="same")
    rot_s[:win // 2] = rot_s[win // 2]
    rot_s[-(win // 2):] = rot_s[-(win // 2) - 1]
    cpe = np.concatenate([[0.0], np.cumsum(rot_s[:-1])])
    Y = Y * np.exp(-1j * cpe)[:, None]
    print(f"residual CFO from SP rotation: mean {rot.mean() / (2 * np.pi) * N / T:+.4f} bins "
          f"({rot.mean() / (2 * np.pi) * N / T * FS / N:+.1f} Hz), per-symbol rotation std {rot.std():.3f} rad")
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
    if args.ts_out:
        np.savez_compressed(args.ts_out, **{k: np.asarray(v) for k, v in out.items()})
    if args.stim and "B" in out and Path(args.ts_ref).exists():
        ref_ts = np.fromfile(args.ts_ref, dtype=np.uint8)
        ref_ts = ref_ts[: len(ref_ts) // 188 * 188].reshape(-1, 188)
        ts_a = Path(args.ts_ref).with_name(Path(args.ts_ref).name.replace("layerB", "layerA"))
        if ts_a.exists() and ts_a != Path(args.ts_ref):
            a = np.fromfile(ts_a, dtype=np.uint8)
            ref_ts = np.concatenate([ref_ts, a[: len(a) // 188 * 188].reshape(-1, 188)])
        # 刺激は k フレームの巡回ループなので、時間インターリーブ（B 層 I=2 で最大 190 シンボル遅延）が
        # ループの継ぎ目をまたぐフレーム（各周期の先頭 1 フレーム）は変調器の TS と一致しない。
        # 合格は「一致パケットが 1 フレーム分（tsp_per_frame）以上連続する」ことで判定する
        ref_set = {bytes(p) for p in ref_ts}
        for lname in ("B", "A"):
            if lname not in out:
                continue
            lyr = next(l for l in fsi["layers"] if l.name == lname)
            got = out[lname]
            hitmask = np.array([bytes(p) in ref_set for p in got], dtype=bool)
            # 最長の連続一致
            best = run = 0
            for h in hitmask:
                run = run + 1 if h else 0
                best = max(best, run)
            tsp_pf = P.tsp_per_frame(mode, lyr.mod, lyr.rate, len(lyr.segments))
            print(f"  layer {lname}: packets in the modulator's TS {int(hitmask.sum())} / {len(got)} "
                  f"({100.0 * hitmask.mean():.1f}%), longest matching run {best} (1 frame = {tsp_pf})")
            if lname == "B":
                print("PASS" if best >= tsp_pf else "FAIL")


if __name__ == "__main__":
    main()
