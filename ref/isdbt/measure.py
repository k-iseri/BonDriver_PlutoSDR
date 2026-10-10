"""受信品質の測定（MER、キャリア別 MER、遅延プロファイル、Mode/GI のブラインド判定）。

実機の初回キャプチャで「映らない」ときの切り分け用。復調の本流（demod.py）からは独立。
"""
from __future__ import annotations

import numpy as np

from . import demod, pilots, sync
from . import params as P

# ---------------------------------------------------------------------------
# コンスタレーションのスライスと MER
# ---------------------------------------------------------------------------
slice_nearest = demod.DI.slice_nearest     # 実体は deinterleave.py（demod と共用）


def mer_db(z: np.ndarray, mod: P.Modulation) -> float:
    """MER [dB] = 10 log10(理想点の平均電力 / 誤差ベクトルの平均電力)。"""
    ref = slice_nearest(z, mod)
    err = np.mean(np.abs(z - ref) ** 2)
    sig = np.mean(np.abs(ref) ** 2)
    return float(10 * np.log10(sig / max(err, 1e-30)))


# ---------------------------------------------------------------------------
# 受信品質レポート（同期 → 等化 → 階層ごとの MER / キャリア別 MER / 遅延プロファイル）
# ---------------------------------------------------------------------------
def rx_quality(iq: np.ndarray, fs: float, mode: P.Mode = P.JP_DEFAULT_MODE, gi: str = P.JP_DEFAULT_GI,
               max_frames: int = 2) -> dict:
    """先頭 max_frames フレームから受信品質をまとめる。

    返り値:
      sync, frame_start, tmcc, layers,
      mer_db: 階層名 → MER [dB]
      carrier_mer_db: (total_carriers,) キャリア別 MER（データキャリアのみ、他は NaN）
      constellation: 階層名 → 等化後シンボルのサンプル (≤ 20000,)
      delay_profile_db: (fft_size,) 平均チャネルインパルス応答（dB、ピーク 0 dB 正規化）
      delay_axis_us: 対応する遅延 [µs]
    """
    src = sync.CarrierSource(iq, fs, mode, gi)
    fsi = demod.frame_sync(src, mode)
    f0 = fsi["frame_start"]
    # 時間デインターリーブは解かずにキャリアシンボルのまま測る（MER は並びに不変）。
    # ただし送信開始直後は時間インターリーブの押し出しでゼロのキャリアが混ざるので、
    # 全キャリアが埋まるフレーム以降だけを使う
    hist = max((-(-demod.DI.time_deinterleave_max_delay(lyr.I) // P.SYMBOLS_PER_FRAME)
                for lyr in fsi["layers"]), default=0)
    r0 = f0 + hist * P.SYMBOLS_PER_FRAME
    n_sym = min(src.n_sym, r0 + max_frames * P.SYMBOLS_PER_FRAME)
    if n_sym - r0 < P.SYMBOLS_PER_FRAME // 2:
        r0 = f0
        n_sym = min(src.n_sym, f0 + max_frames * P.SYMBOLS_PER_FRAME)
    Y = src.symbols(0, n_sym)
    H, bad = demod.channel_estimate(Y, mode, f0, return_quality=True)
    D, _csi = demod.equalize(Y, H)

    Yl = demod.DI.data_to_logical(D, mode, f0)
    rows = np.arange(r0, n_sym)
    rows = rows[~bad[rows]] if len(rows) else rows

    mp = P.MODE_PARAMS[mode]
    out_mer: dict = {}
    constel: dict = {}
    for lyr in fsi["layers"]:
        # セグメント間インターリーブ前の並びで十分（MER は並びに不変）
        slots = Yl[rows][:, lyr.segments, :].reshape(-1)
        out_mer[lyr.name] = mer_db(slots, lyr.mod)
        constel[lyr.name] = slots[:20000].copy()

    # キャリア別 MER: 伝送位置ごと（データキャリアのみ）
    dpos4 = pilots.data_carrier_positions_4(mode)
    carrier_err = np.full(mp.total_carriers, np.nan)
    mod_of_seg = {}
    for lyr in fsi["layers"]:
        for s in lyr.segments:
            mod_of_seg[s] = lyr.mod
    phases = (rows - f0) % P.SP_SYMBOL_PERIOD
    err_sum = np.zeros(mp.total_carriers)
    sig_sum = np.zeros(mp.total_carriers)
    cnt = np.zeros(mp.total_carriers)
    for p in range(P.SP_SYMBOL_PERIOD):
        sel = rows[phases == p]
        if len(sel) == 0:
            continue
        for f_idx in range(P.N_SEGMENTS):
            seg_no = P.SEGMENT_ORDER_IN_FREQ[f_idx]
            mod = mod_of_seg.get(seg_no)
            if mod is None:
                continue
            ks = dpos4[p][f_idx]
            z = D[np.ix_(sel, ks)]
            ref = slice_nearest(z, mod)
            err_sum[ks] += np.abs(z - ref).__pow__(2).sum(axis=0)
            sig_sum[ks] += np.abs(ref).__pow__(2).sum(axis=0)
            cnt[ks] += len(sel)
    ok = cnt > 0
    carrier_err[ok] = 10 * np.log10(np.maximum(sig_sum[ok], 1e-30) / np.maximum(err_sum[ok], 1e-30))

    # 遅延プロファイル: H の平均インパルス応答。FFT 窓を margin 分早く開くぶん主波が
    # 正の遅延に乗るので、最強パスを遅延 0 に回して「主波基準の相対遅延」で返す
    Hm = H[rows].mean(axis=0) if len(rows) else H.mean(axis=0)
    full = np.zeros(mp.fft_size, dtype=np.complex64)
    bins = P.carrier_to_fft_bin(mode, np.arange(mp.total_carriers))
    full[bins] = Hm
    cir = np.fft.ifft(full)
    prof = np.abs(cir) ** 2
    prof = np.roll(prof, -int(np.argmax(prof)))
    prof_db = 10 * np.log10(np.maximum(prof / prof.max(), 1e-12))
    delay_us = np.arange(mp.fft_size) / float(P.SAMPLE_RATE_HZ) * 1e6

    return {"sync": src.info(), "frame_start": f0, "tmcc_parity_ok": fsi["tmcc_parity_ok"],
            "tmcc": fsi["tmcc"], "layers": fsi["layers"], "mer_db": out_mer,
            "carrier_mer_db": carrier_err, "constellation": constel,
            "delay_profile_db": prof_db, "delay_axis_us": delay_us,
            "bad_symbols": int(bad.sum())}


# ---------------------------------------------------------------------------
# Mode / GI のブラインド判定
# ---------------------------------------------------------------------------
def detect_mode_gi(iq: np.ndarray, fs: float, use_seconds: float = 0.3) -> tuple[P.Mode, str, dict]:
    """GI 相関のロック指標が最大になる (Mode, GI) を探す。

    日本の実運用は Mode 3 / GI 1/8 固定だが、録音ミス（レート違い等）の切り分けに使う。
    返り値 (mode, gi, 全組み合わせの指標)。
    """
    x = sync.resample_to_nominal(iq[: int(use_seconds * fs)], fs)
    x = x - np.mean(x)
    scores: dict = {}
    best = (P.JP_DEFAULT_MODE, P.JP_DEFAULT_GI, -1.0)
    for mode in P.Mode:
        N = P.MODE_PARAMS[mode].fft_size
        for gi in P.GUARD_INTERVALS:
            Ng = P.guard_samples(mode, gi)
            T = N + Ng
            n_sym = (len(x) - N - Ng) // T
            if n_sym < 4:
                scores[(mode.value, gi)] = float("nan")
                continue
            chunk = min(16, n_sym)
            _idx, _pos, val, pwr = sync.gi_fold_chunks(x, N, Ng, chunk_syms=chunk, return_power=True)
            with np.errstate(divide="ignore", invalid="ignore"):
                m = np.abs(val) / np.maximum(chunk * Ng * pwr, 1e-30)
            score = float(np.median(m))
            scores[(mode.value, gi)] = score
            if score > best[2]:
                best = (mode, gi, score)
    return best[0], best[1], scores
