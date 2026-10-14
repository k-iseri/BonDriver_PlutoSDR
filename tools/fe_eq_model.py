#!/usr/bin/env python3
"""FPGA 等化器（pluto-fw-isdbt 2b-3b、モード 4）のアルゴリズムモデル。

  python tools/fe_eq_model.py <capture.npz> [--ts-ref ...]   # fe_carrier_demod.py --dump の npz（同期 FFT のビン）

FPGA に載せる処理を numpy で模擬し、出力（等化済み I/Q 8bit + CSI 8bit）を ref/ の後段（デインターリーブ →
ビタビ → RS）に通して MER と RS 誤りを、同じ入力での ref/ 浮動小数点等化（channel_estimate）と比べる。
FPGA 側の処理（シンボルごと、キャリア順に 1 パス + 履歴 BRAM）:
  1. 整数 CFO δ（ホストがレジスタで与える）: キャリア k = ビン (k − 2808 + δ) mod N
  2. 窓タイミングの小数部 Δ_n（追従の滑らかな時間軸と実際の窓開始の差）: ビン b を e^{−j2π b Δ_n / N} で回す
     （整数サンプルの窓のずれを位相ランプで消す。帯域端で 1 サンプル = 2.1 rad）
  3. AFC: SP は 4 シンボル周期で同値なので Σ_sp Y_n·conj(Y_{n−4}) の位相 / 4 = 残留 CFO による 1 シンボルの回転。
     これを積算してビン領域の共通位相として逆回転する（NCO に帰還すると整数 CFO δ の既知の位相ステップ
     δ·NG/N 回転/シンボルまで周波数として吸い込み、本物の小数 CFO を作ってしまうので NCO には入れない）
  4. SP 位相（n mod 4）: 4 候補の SP 位置集合のエネルギーが最大のもの（SP は電力 16/9 倍）
  5. H_sp = Y·(±1)·3/4 を履歴に入れ、3 キャリア格子の各点を直近 2 シンボル（同位相）で平均 → 線形補間（1/3, 2/3）
  6. D = Y / H（conj(H)/|H|²、逆数は正規化 + 表引き）→ I/Q 8bit（±2.0）、CSI = |H|² / 格子平均 → 8bit（0〜4）
"""
import argparse
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ref"))
from isdbt import deinterleave as DI, demod, measure, params as P, pilots  # noqa: E402

N, NG, T = 8192, 1024, 9216
TOTAL, CENTER = 5617, 2808
FS = 512e6 / 63


def recip_lut(x, bits=10, out_bits=16):
    """1/x を「仮数を [0.5,1) に正規化 → 上位 bits ビットで表引き（out_bits ビット）→ 指数を戻す」で近似。x ≤ 0 は 0。"""
    x = np.asarray(x, dtype=np.float64)
    out = np.zeros_like(x)
    pos = x > 0
    e = np.floor(np.log2(x[pos])) + 1                   # x = m · 2^e, m ∈ [0.5, 1)
    m = x[pos] / 2.0 ** e
    mq = np.floor(m * 2 ** bits) / 2 ** bits            # 表の入口（切り捨て）+ 中点
    r = np.round((1.0 / (mq + 0.5 / 2 ** bits)) * 2 ** (out_bits - 1)) / 2 ** (out_bits - 1)   # 1/m ∈ (1, 2]
    out[pos] = r / 2.0 ** e
    return out


def q8(x, lsb):
    """8bit 量子化（丸め、飽和）→ 実数値に戻す。"""
    return np.clip(np.round(x / lsb), -128, 127) * lsb


def q8u(x, lsb):
    return np.clip(np.round(x / lsb), 0, 255) * lsb


class EqSource:
    """ref/ の _decode_source が要求する CarrierSource 互換。symbols() は等化済み D を返し、直近の区間を覚える。"""

    def __init__(self, D, csi):
        self.D = D.astype(np.complex64)
        self.csi = csi.astype(np.float32)
        self.n_sym = D.shape[0]
        self.last = (0, self.n_sym)

    def symbols(self, a, b):
        self.last = (a, b)
        return self.D[a:b]

    def info(self):
        return {"source": "fpga-eq-model"}


def decode_equalized(src: EqSource, mode):
    """等化済み入力で ref/ の復号を回す（equalize_and_deinterleave を差し替え）。"""
    orig = demod.equalize_and_deinterleave

    def eq_and_di(Y, mode_, frame_start, layers, groups):
        a, b = src.last
        D = Y
        csi = src.csi[a:b]
        Yl = DI.data_to_logical(D, mode_, frame_start)
        Wl = DI.data_to_logical(csi, mode_, frame_start)
        I_seg = np.zeros(P.N_SEGMENTS, dtype=np.int64)
        for lyr in layers:
            I_seg[lyr.segments] = lyr.I
        Xt = DI.time_deinterleave(DI.intersegment_deinterleave(Yl, groups), I_seg)
        Wt = DI.time_deinterleave(DI.intersegment_deinterleave(Wl, groups), I_seg)
        return None, Xt, Wt

    demod.equalize_and_deinterleave = eq_and_di
    try:
        out, dbg = demod._decode_source(src, mode, 6)
    finally:
        demod.equalize_and_deinterleave = orig
    return out, dbg


def report(tag, D, csi, mode, ts_ref=None):
    src = EqSource(D, csi)
    fsi = demod.frame_sync(src, mode)
    fs = fsi["frame_start"]
    Yl = DI.data_to_logical(D, mode, fs)
    hist = max((-(-DI.time_deinterleave_max_delay(l.I) // P.SYMBOLS_PER_FRAME) for l in fsi["layers"]), default=0)
    rows = np.arange(fs + hist * P.SYMBOLS_PER_FRAME, D.shape[0])
    rows = rows[rows >= getattr(args_global, 'skip', 0)]
    mers = {l.name: measure.mer_db(Yl[rows][:, l.segments, :].reshape(-1), l.mod) for l in fsi["layers"]}
    out, dbg = decode_equalized(src, mode)
    rs = {name: (int(np.sum(np.asarray(dbg["rs_errors"][name]) < 0)), len(dbg["rs_errors"][name]),
                 int(np.sum(np.asarray(dbg["rs_errors"][name])[len(dbg["rs_errors"][name]) // 2:] < 0))) for name in out}
    line = f"{tag}: frame_start {fs}, parity {fsi['tmcc_parity_ok']}, " + ", ".join(f"MER {k} {v:.2f} dB" for k, v in mers.items()) \
        + ", RS uncorrectable " + ", ".join(f"{k} {a}/{b} (2nd half {c})" for k, (a, b, c) in rs.items())
    if ts_ref is not None and "B" in out:
        ref_set = ts_ref
        hit = np.array([bytes(p) in ref_set for p in out["B"]])
        best = run = 0
        for h in hit:
            run = run + 1 if h else 0
            best = max(best, run)
        line += f", B in TS {hit.sum()}/{len(hit)} (longest run {best})"
    print(line)
    if getattr(args_global, "carrier_mer", False):
        # データキャリアの誤差（最近傍点との差）をキャリア番号ごとに集計（4 位相まとめて、A = セグメント 6 だけ表示）
        dpos4 = pilots.data_carrier_positions_4(mode)
        err = np.full(TOTAL, np.nan); cnt = np.zeros(TOTAL)
        e_acc = np.zeros(TOTAL)
        for pph in range(4):
            sel = rows[((rows - fs) % 4) == pph]
            kk_ = dpos4[pph][6]                       # 周波数順セグメント 6 = 中央（A 層）
            dd = D[np.ix_(sel, kk_)]
            ref_ = DI.slice_nearest(dd, fsi["layers"][0].mod)
            e_acc[kk_] += np.sum(np.abs(dd - ref_) ** 2, axis=0); cnt[kk_] += len(sel)
        ok = cnt > 0
        err[ok] = e_acc[ok] / cnt[ok]
        kk_a = np.flatnonzero(ok)
        m = -10 * np.log10(err[ok] / np.mean(np.abs(DI.slice_nearest(D[np.ix_(rows[:50], kk_a)], fsi["layers"][0].mod)) ** 2))
        order = np.argsort(m)
        print(f"    seg6 carrier MER: median {np.median(m):.2f}, worst 8: " + ", ".join(f"k{kk_a[i]}={m[i]:.1f}" for i in order[:8])
              + f", mean of worst 20 excluded {np.mean(np.sort(m)[20:]):.2f}")
    return mers


def block_timing(starts, sched=(8, 10, 12, 13), iir_shift=1):
    """FPGA の時間軸: T_est = (s_n − s_{n−2^L}) / 2^L（L を sched の順に伸ばす）、τ += T_est、Δ_n = s_n − τ。
    追従器の窓開始列だけから作れ、割り算はシフト。Δ は mod N でよい（整数ビンの位相ランプは N 周期）。
    最長の L に達した後はブロック推定を IIR（1/2^iir_shift）で平均して T_est の雑音をさらに下げる。
    追従器の出力は ±1 サンプルの遅い揺れ（数百シンボル周期）を持つが、τ は帰還なしの直線なので影響しない。"""
    s = np.asarray(starts, dtype=np.float64)
    tau, Test, li, last = s[0], float(T), 0, 0
    out = np.zeros(len(s))
    for i in range(len(s)):
        out[i] = s[i] - tau
        if i - last == (1 << sched[li]):
            est = (s[i] - s[last]) / (1 << sched[li])
            Test = est if li < len(sched) - 1 else Test + (est - Test) / (1 << iir_shift)
            last = i
            li = min(li + 1, len(sched) - 1)
        tau += Test
    return out


def fpga_equalizer(Yc, b_signed, dev, starts, delta, sp_phase0, args, Ybins=None):
    """FPGA 等化器のモデル。Yc: (n_sym, 5617) キャリア（δ 補正済み、ビン順に並べ替え済み）。

    2 パス構成（FPGA は 1 パス + 履歴 BRAM だが、--centered の実験のためにモデルでは分ける）:
      パス 1: 回転（Δ・δ・AFC）→ SP 位相検出 → H_sp を格子配列 Hs (n_sym, M) に置く
      パス 2: 格子平均（直近 avg シンボル、--centered なら前後）→ 線形補間 → 等化 → 量子化
    """
    n_sym = Yc.shape[0]
    pv = pilots.pilot_values(P.JP_DEFAULT_MODE)
    sign = np.sign(pv)
    M = (TOTAL - 1) // 3 + 1                         # 格子点 k = 3j
    kk = np.arange(TOTAL)
    if args.fpga_timing:
        dev = block_timing(starts)
    dq = np.round(dev * 256) / 256
    Y = Yc * np.exp(-2j * np.pi * np.outer(dq, b_signed) / N) * np.exp(-2j * np.pi * delta * (starts % N) / N)[:, None]
    sp_sets = [np.arange(3 * p, TOTAL - 1, 12) for p in range(4)]
    Hs = np.zeros((n_sym, M), dtype=np.complex128)
    Cs = np.zeros((n_sym, M), dtype=np.float64)
    omega, phi = 0.0, 0.0
    Ysp_hist = [None] * 4
    sp_ph_est = []
    for n in range(n_sym):
        y = Y[n] * np.exp(-1j * phi)
        e = [np.sum(np.abs(y[s]) ** 2) for s in sp_sets]
        p = int(np.argmax(e)) if not args.known_sp else (n - sp_phase0) % 4
        sp_ph_est.append(p)
        k = sp_sets[p]
        if Ysp_hist[p] is not None:
            meas = np.angle(np.vdot(Ysp_hist[p], y[k])) / 4.0
            omega += meas * args.afc_gain
        Ysp_hist[p] = y[k].copy()
        Y[n] = y
        Hs[n, k // 3] = y[k] * sign[k] * 0.75
        Cs[n, k // 3] = 1
        Hs[n, M - 1] = y[TOTAL - 1] * sign[TOTAL - 1] * 0.75
        Cs[n, M - 1] = 1
        phi += omega
    sp_ph_est = np.array(sp_ph_est)
    ok = np.mean(((sp_ph_est[1:] - sp_ph_est[:-1]) % 4) == 1)
    print(f"  model: SP phase detection consistent {100 * ok:.2f}%, final AFC omega {omega:+.4f} rad/symbol "
          f"({omega / (2 * np.pi) * N / T:+.4f} bins)")
    D = np.zeros_like(Y)
    csi = np.zeros((n_sym, TOTAL), dtype=np.float32)
    j = np.arange(M)
    cH = np.concatenate([np.zeros((1, M)), np.cumsum(Hs, axis=0)])
    cC = np.concatenate([np.zeros((1, M)), np.cumsum(Cs, axis=0)])
    for n in range(n_sym):
        if args.centered:
            a_, b_ = max(0, n - args.avg // 2), min(n_sym, n - args.avg // 2 + args.avg)
        else:
            a_, b_ = max(0, n - args.avg + 1), n + 1
        E, C = cH[b_] - cH[a_], cC[b_] - cC[a_]
        known = C > 0
        Hg = np.where(known, E / np.where(known, C, 1), 0)
        if not known.all() and known.sum() >= 2:
            Hg = np.interp(j, j[known], Hg[known].real) + 1j * np.interp(j, j[known], Hg[known].imag)
        H = np.interp(kk, 3 * j, Hg.real) + 1j * np.interp(kk, 3 * j, Hg.imag)
        y = Y[n]
        if args.cpe_own:
            k = sp_sets[sp_ph_est[n]]
            ph_ = np.angle(np.vdot(H[k], y[k] * sign[k]))
            if args.fixed:
                ph_ = np.round(ph_ / (2 * np.pi) * 4096) / 4096 * 2 * np.pi      # 位相 12bit → cos/sin 表
                cs = (np.round(np.cos(ph_) * 2048) + 1j * np.round(np.sin(ph_) * 2048)) / 2048
                y = y * np.conj(cs)
            else:
                y = y * np.exp(-1j * ph_)
        h2 = np.abs(H) ** 2
        mean_h2 = np.mean(np.abs(Hg) ** 2)
        if args.fixed:
            # 逆数: h2 を [0.5, 1) に正規化して上位 10bit で表引き（出力 16bit）
            inv = recip_lut(h2)
            inv_mean = recip_lut(np.array([mean_h2]))[0]
            d = y * np.conj(H) * inv
            c = h2 * inv_mean
        else:
            with np.errstate(divide="ignore", invalid="ignore"):
                d = np.where(h2 > 0, y * np.conj(H) / h2, 0)
            c = np.where(h2 > 0, h2 / max(mean_h2, 1e-30), 0)
        D[n] = q8(d.real, 1 / 64) + 1j * q8(d.imag, 1 / 64) if args.quant else d
        csi[n] = q8u(c, 1 / 64) if args.quant else c
    if args.vectors:
        vd = Path(args.vectors)
        vd.mkdir(parents=True, exist_ok=True)
        nv = min(args.vec_symbols, n_sym)
        ex = int(np.round(np.log2(np.max(np.abs(Yc)) / 2047 + 1e-30)))     # 取り込みは 8bit×16×2^exp なので共通指数
        with open(vd / "eq_in.txt", "w", newline="\n") as f:
            for n in range(nv):
                dq_n = int(np.round(block_timing(starts)[n] * 256)) & 0x1FFFFF if args.fpga_timing else int(np.round(dev[n] * 256)) & 0x1FFFFF
                f.write(f"H {int(starts[n]) & 0xFFFFFFFF:08x} {dq_n:08x} {ex & 0xFF:02x}\n")
                yb = np.round(Ybins[n] / 2.0 ** ex)
                for b in range(N):
                    f.write(f"{int(np.clip(yb[b].real, -2048, 2047)) & 0xFFFF:04x} {int(np.clip(yb[b].imag, -2048, 2047)) & 0xFFFF:04x}\n")
        with open(vd / "eq_ref.txt", "w", newline="\n") as f:
            for n in range(nv):
                for k in range(TOTAL):
                    f.write(f"{int(np.round(D[n, k].real * 64)) & 0xFF:02x} {int(np.round(D[n, k].imag * 64)) & 0xFF:02x} {int(np.round(csi[n, k] * 64)) & 0xFF:02x}\n")
        print(f"  wrote vectors for {nv} symbols to {vd} (exp {ex})")
    return D, csi


args_global = None


def main():
    global args_global
    ap = argparse.ArgumentParser()
    ap.add_argument("npz")
    ap.add_argument("--ts-ref", default=None)
    ap.add_argument("--avg", type=int, default=8, help="格子平均に使う直近シンボル数（8 = 各位相 2 シンボル）")
    ap.add_argument("--afc-gain", type=float, default=0.25)
    ap.add_argument("--no-quant", dest="quant", action="store_false")
    ap.add_argument("--known-sp", action="store_true", help="SP 位相をフレーム同期から与える（検出の切り分け）")
    ap.add_argument("--carrier-mer", action="store_true", help="中央セグメントのキャリア別 MER を表示")
    ap.add_argument("--centered", action="store_true", help="実験: 格子平均を前後対称にする（FPGA では不可）")
    ap.add_argument("--fpga-timing", action="store_true", help="Δ_n を直線フィットでなく FPGA のブロック平均時間軸から作る")
    ap.add_argument("--fixed", action="store_true", help="固定小数点要素（逆数表 10bit、cos/sin 12bit）を入れる")
    ap.add_argument("--skip", type=int, default=0, help="MER の集計から先頭 skip シンボルを除く（時間軸の立ち上がり）")
    ap.add_argument("--vectors", default=None, help="RTL 検証用ベクタ（eq_in.txt / eq_ref.txt）を書くディレクトリ")
    ap.add_argument("--vec-symbols", type=int, default=120)
    ap.add_argument("--cpe-own", action="store_true", help="実験: 自シンボルの SP で共通位相を補正（ref と同じ）")
    args = ap.parse_args()
    args_global = args
    z = np.load(args.npz)
    Yb = z["Y"].astype(np.complex128)
    starts = z["start"].astype(np.float64)
    mode = P.JP_DEFAULT_MODE
    # 1. δ
    ps = np.fft.fftshift(np.mean(np.abs(Yb) ** 2, axis=0))
    delta = max(range(-100, 101), key=lambda d: ps[N // 2 - CENTER + d: N // 2 - CENTER + d + TOTAL].sum())
    bins = (P.carrier_to_fft_bin(mode, np.arange(TOTAL)) + delta) % N
    b_signed = ((bins + N // 2) % N) - N // 2
    Yc = Yb[:, bins]
    n_idx = np.arange(len(starts), dtype=np.float64)
    rel = starts - starts[0]
    coef = np.polyfit(n_idx, rel, 1)
    dev = rel - np.polyval(coef, n_idx)
    print(f"delta {delta}, clock drift {(coef[0] - T) / T * 1e6:+.1f} ppm, jitter std {dev.std():.2f}")
    ts_ref = None
    if args.ts_ref:
        r = np.fromfile(args.ts_ref, dtype=np.uint8)
        ts_ref = {bytes(p) for p in r[: len(r) // 188 * 188].reshape(-1, 188)}

    # ---- 基準: ホスト側の処理（fe_carrier_demod.py と同じ補償 + ref/ の浮動小数点等化）----
    ph = np.exp(-2j * np.pi * delta * (starts % N) / N)
    Yh = Yc * ph[:, None] * np.exp(-2j * np.pi * np.outer(dev, b_signed) / N)
    fs0 = demod.frame_sync(EqSource(Yh, np.ones_like(Yh, dtype=np.float32)), mode)["frame_start"]
    rot = np.zeros(Yh.shape[0])
    for n in range(Yh.shape[0] - 4):
        k = pilots.sp_positions(mode, (n - fs0) % 4)
        rot[n] = np.angle(np.vdot(Yh[n, k], Yh[n + 4, k])) / 4.0
    rot[-4:] = rot[-5]
    rs = np.convolve(rot, np.ones(33) / 33, mode="same")
    Yh = Yh * np.exp(-1j * np.concatenate([[0.0], np.cumsum(rs[:-1])]))[:, None]
    H, _bad = demod.channel_estimate(Yh, mode, fs0, return_quality=True)
    Dref, cref = demod.equalize(Yh, H)
    report("ref float eq  ", Dref, cref, mode, ts_ref)
    report("ref eq + 8bit ", q8(Dref.real, 1 / 64) + 1j * q8(Dref.imag, 1 / 64), q8u(cref, 1 / 64), mode, ts_ref)

    # ---- FPGA モデル ----
    D, csi = fpga_equalizer(Yc, b_signed, dev, starts, delta, fs0 % 4, args, Ybins=Yb)
    report("fpga model    ", D, csi, mode, ts_ref)


if __name__ == "__main__":
    main()
