"""ISDB-T 復調器（参照実装）: 有効キャリア行列 → TMCC → 等化 → デインターリーブ → 復号 → TS。

同期（sync.py）の後:
  1. TMCC キャリアを DBPSK 復調し、同期語（表 3-21）とパリティ（3.15.6.9）でフレーム先頭を決める
  2. TMCC 情報（表 3-22〜3-31）から階層構成を得る
  3. SP（図 3-30）で伝達関数を推定し等化。CSI（|H|²）を軟判定の重みにする
  4. 周波数・時間・セグメント間・ビットの各デインターリーブ（deinterleave.py）
  5. デパンクチャ → Viterbi → バイトデインターリーブ → エネルギー逆拡散 → RS 復号（fec.py）
フレーム整合の規約は docs/isdbt-spec-notes.md 10 章。
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import deinterleave as DI
from . import fec, pilots, sync, tmcc
from . import params as P


@dataclass
class RxLayer:
    name: str
    mod: P.Modulation
    rate: str
    I: int
    segments: list


# ---------------------------------------------------------------------------
# TMCC: フレーム同期と階層構成
# ---------------------------------------------------------------------------
def tmcc_bits_from_carriers(Y: np.ndarray, mode: P.Mode) -> np.ndarray:
    """DBPSK 復調。b[n] = B_n（n ≥ 1）、b[0] は未定義（0）。全 TMCC キャリアを同相加算する。"""
    tm = np.array(P.TMCC_POSITIONS_ABS[mode])
    prod = (Y[1:, tm] * np.conj(Y[:-1, tm])).sum(axis=1).real
    b = np.zeros(Y.shape[0], dtype=np.uint8)
    b[1:] = (prod < 0).astype(np.uint8)
    return b


def find_frame_start(b: np.ndarray) -> tuple[int, int]:
    """同期語（w0/w1）が一致し、かつパリティが通る最初の位置 n0（B0 のシンボル）を返す。

    返り値 (n0, parity_ok)。パリティが通る候補が無ければ同期語一致のみで決める（parity_ok = 0）。
    """
    n = len(b)
    first_sync = -1
    for n0 in range(n - P.TMCC_BITS_PER_FRAME + 1):
        if tmcc.sync_word_of(b[n0:n0 + 17]) is None:
            continue
        if first_sync < 0:
            first_sync = n0
        if tmcc.dscc_check(b[n0 + 20:n0 + 204]):
            return n0, 1
    if first_sync < 0:
        raise RuntimeError("TMCC 同期語が見つからない")
    return first_sync, 0


def layers_from_tmcc(info: tmcc.TmccInfo) -> list:
    """TMCC 情報 → 階層リスト（A から順にセグメント番号を割当、3.11.2）。"""
    layers, nxt = [], 0
    for name in tmcc.LAYER_NAMES:
        lyr = info.layers.get(name)
        if lyr is None or not lyr.segments:
            continue
        layers.append(RxLayer(name, lyr.mod, lyr.rate, lyr.interleave, list(range(nxt, nxt + lyr.segments))))
        nxt += lyr.segments
    if nxt != P.N_SEGMENTS:
        raise RuntimeError(f"TMCC の階層構成が不正: 合計 {nxt} セグメント")
    return layers


def interleave_groups(partial_reception: bool, layers: list) -> list:
    """セグメント間インターリーブのグループ（3.11.2）。差動変調は未対応なので同期変調部のみ。"""
    partial = [0] if partial_reception else []
    coherent = [s for s in range(P.N_SEGMENTS) if s not in partial]
    return [g for g in (partial, coherent) if len(g) >= 2]


# ---------------------------------------------------------------------------
# 伝達関数推定と等化
# ---------------------------------------------------------------------------
def channel_estimate(Y: np.ndarray, mode: P.Mode, frame_start: int, avg_symbols: int = 8,
                     return_quality: bool = False):
    """SP（図 3-30、値は表 3-17）から各シンボルの伝達関数 H (n_sym, total) を推定する。

    1. SP 位置で H = Y / P を取り、直近 avg_symbols シンボルで平均（4 位相で 3 キャリアおきの格子になる）
    1b. 自シンボルの SP が窓平均から大きく外れるシンボル（インパルス雑音などのバースト）を検出し、
       平均から除いて取り直す。検出したシンボルは CSI を 0（消失）として扱わせる
    2. 格子の位相傾斜（FFT 窓を早く開く分 + タイミング誤差 = バルク遅延）を推定して取り除き、
       残り（マルチパスによる緩やかな変動）をキャリア方向に線形補間してから傾斜を戻す。
       CIR 領域で矩形窓をかける方法は非整数ビンの遅延で帯域端にリンギングが出るので使わない
    3. 各シンボル自身の SP で共通位相誤差を補正

    return_quality=True なら (H, bad) を返す。bad[n] はバースト検出されたシンボル。
    """
    mp = P.MODE_PARAMS[mode]
    K = mp.total_carriers
    n_sym = Y.shape[0]
    pv = pilots.pilot_values(mode).astype(np.float32)
    M = (K - 1) // 3 + 1                                   # 格子 k = 3j、j = 0..M-1（右端 CP を含む）
    assert 3 * (M - 1) == K - 1
    E = np.zeros((n_sym, M), dtype=np.complex64)
    C = np.zeros((n_sym, M), dtype=np.float32)
    phases = (np.arange(n_sym) - frame_start) % P.SP_SYMBOL_PERIOD
    for p in range(P.SP_SYMBOL_PERIOD):
        sel = np.flatnonzero(phases == p)
        k = pilots.sp_positions(mode, p)
        E[np.ix_(sel, k // 3)] = Y[np.ix_(sel, k)] / pv[k]
        C[np.ix_(sel, k // 3)] = 1.0
    E[:, M - 1] = Y[:, K - 1] / pv[K - 1]
    C[:, M - 1] = 1.0
    n = np.arange(n_sym)
    avg = min(avg_symbols, n_sym)
    a = np.clip(n - avg // 2, 0, n_sym - avg)

    def windowed(Ein, Cin):
        cs = np.concatenate([np.zeros((1, M), np.complex64), np.cumsum(Ein, axis=0)])
        cc = np.concatenate([np.zeros((1, M), np.float32), np.cumsum(Cin, axis=0)])
        # 平均窓は中央寄せ（前後 avg_symbols シンボル）。avg_symbols ≥ 4 なら全行で 4 位相が揃う
        Es, Cs = cs[a + avg] - cs[a], cc[a + avg] - cc[a]
        known = Cs > 0
        Hg = np.where(known, Es / np.where(known, Cs, 1), 0).astype(np.complex64)
        return Hg, known

    Hg, known = windowed(E, C)
    # バースト検出: 自シンボルの SP（瞬時推定 E）が窓平均 Hg からどれだけ外れるか。
    # 位相雑音による共通位相のずれはバーストではないので、共通位相を合わせてから比べる
    own = C > 0
    num = (E * np.conj(Hg) * own).sum(axis=1)
    theta = np.exp(-1j * np.angle(num)).astype(np.complex64)
    diff = (E * theta[:, None] - Hg) * own
    sig = np.maximum((np.abs(Hg) ** 2 * own).sum(axis=1), 1e-30)
    err = (np.abs(diff) ** 2).sum(axis=1) / sig
    bad = err > max(10.0 * float(np.median(err)), 0.05)
    if bad.any():
        E2, C2 = E.copy(), C.copy()
        E2[bad] = 0
        C2[bad] = 0
        Hg, known = windowed(E2, C2)
    j = np.arange(M)
    # バルク遅延（格子 1 点 = 3 キャリアあたりの位相回転）を既知の隣接点対から推定して除く
    pair = (known[:, 1:] & known[:, :-1]).astype(np.float32)
    dphi = np.angle((Hg[:, 1:] * np.conj(Hg[:, :-1]) * pair).sum(axis=1)).astype(np.float32)
    flat = Hg * np.exp(-1j * dphi[:, None] * j[None, :]).astype(np.complex64)
    for r in np.flatnonzero(~known.all(axis=1)):
        ok = known[r]
        if ok.sum() < 2:
            continue
        flat[r] = np.interp(j, j[ok], flat[r, ok].real) + 1j * np.interp(j, j[ok], flat[r, ok].imag)
    kk = np.arange(K, dtype=np.float32)
    H = np.empty((n_sym, K), dtype=np.complex64)
    for r in range(n_sym):
        H[r] = np.interp(kk, 3 * j, flat[r].real) + 1j * np.interp(kk, 3 * j, flat[r].imag)
    H *= np.exp(1j * dphi[:, None] * kk[None, :] / 3).astype(np.complex64)
    # 共通位相誤差（各シンボル自身の SP）
    for p in range(P.SP_SYMBOL_PERIOD):
        sel = np.flatnonzero(phases == p)
        k = pilots.sp_positions(mode, p)
        num = (Y[np.ix_(sel, k)] * np.conj(H[np.ix_(sel, k)]) * pv[k][None, :]).sum(axis=1)
        H[sel] *= np.exp(1j * np.angle(num))[:, None].astype(np.complex64)
    return (H, bad) if return_quality else H


def equalize(Y: np.ndarray, H: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """ゼロフォーシング等化 D = Y/H と、軟判定の重み CSI = |H|²（シンボルごとに平均 1 に正規化）。

    H が 0 になる位置（瞬断などで推定不能）は D も CSI も 0 にする。NaN を残すと
    CSI = 0 を掛けても NaN のままで Viterbi のパスメトリック全体を汚染する。
    """
    with np.errstate(divide="ignore", invalid="ignore"):
        D = (Y / H).astype(np.complex64)
    ok = np.isfinite(D)
    D = np.where(ok, D, 0).astype(np.complex64)
    csi = np.abs(H) ** 2
    with np.errstate(invalid="ignore"):
        csi = (csi / np.maximum(csi.mean(axis=1, keepdims=True), 1e-30)).astype(np.float32)
    csi = np.where(ok, csi, 0.0).astype(np.float32)
    return D, csi


# ---------------------------------------------------------------------------
# 前段（同期 → TMCC → 等化 → 周波数/時間/セグメント間デインターリーブ）
# ---------------------------------------------------------------------------
EQ_CONTEXT_SYMBOLS = 4      # 等化の平均窓（前後 8 シンボル）のためにブロックの前後に足すシンボル数


def frame_sync(src: sync.CarrierSource, mode: P.Mode) -> dict:
    """先頭数フレームから TMCC を復号してフレーム先頭と階層構成を決める。"""
    Yh = src.symbols(0, min(src.n_sym, 3 * P.SYMBOLS_PER_FRAME))
    b = tmcc_bits_from_carriers(Yh, mode)
    f0, parity_ok = find_frame_start(b)
    info = tmcc.parse_info(mode, b[f0 + 20:f0 + 122])
    layers = layers_from_tmcc(info)
    return {"frame_start": f0, "tmcc_parity_ok": parity_ok, "tmcc": info, "layers": layers,
            "groups": interleave_groups(info.partial_reception, layers),
            "n_frames": (src.n_sym - f0) // P.SYMBOLS_PER_FRAME}


def null_interfered_carriers(D: np.ndarray, csi: np.ndarray, mode: P.Mode, frame_start: int,
                             layers: list) -> int:
    """狭帯域妨害（LO リークの残り等）が常時乗っているキャリアの CSI を 0 にする。

    等化後の誤差（最近傍コンスタレーション点との差）が「キャリア固定で」大きい位置は
    AWGN ではなく持続的な妨害とみなして消失扱いにする。妨害は H 推定（SP 格子）も
    汚して周辺キャリアに波及するので、CSI 重みだけでは守れない。
    行方向（時間方向）の統計は中央値を使う: 録音先頭の時間インターリーブ無音区間
    （本物の送信ゼロ。行の半分未満）を誤検出しないため。キャリア方向の閾値は
    同位相グループ内の中央値の 10 倍（低 C/N では中央値ごと上がるので誤検出しない）。
    返り値は消失化したキャリア数（4 位相の延べ数）。
    """
    mp = P.MODE_PARAMS[mode]
    dpos4 = pilots.data_carrier_positions_4(mode)
    mod_of_freqseg: list = [None] * P.N_SEGMENTS
    for lyr in layers:
        for s in lyr.segments:
            mod_of_freqseg[P.SEGMENT_FREQ_INDEX[s]] = lyr.mod
    phases = (np.arange(D.shape[0]) - frame_start) % P.SP_SYMBOL_PERIOD
    n_nulled = 0
    for p in range(P.SP_SYMBOL_PERIOD):
        sel = np.flatnonzero(phases == p)
        if len(sel) < 8:
            continue
        err_k = np.full(mp.total_carriers, np.nan)
        for f in range(P.N_SEGMENTS):
            mod = mod_of_freqseg[f]
            if mod is None:
                continue
            ks = dpos4[p][f]
            z = D[np.ix_(sel, ks)]
            ref = DI.slice_nearest(z, mod)
            err_k[ks] = np.median(np.abs(z - ref) ** 2, axis=0)
        med = np.nanmedian(err_k)
        bad_k = np.flatnonzero(err_k > 10.0 * med)
        if len(bad_k):
            csi[np.ix_(sel, bad_k)] = 0.0
            n_nulled += len(bad_k)
    return n_nulled


def equalize_and_deinterleave(Y: np.ndarray, mode: P.Mode, frame_start: int, layers: list, groups: list):
    """等化 → 論理キャリア → 時間 → セグメント間デインターリーブ。(H, Xt, Wt) を返す。"""
    H, bad = channel_estimate(Y, mode, frame_start, return_quality=True)
    D, csi = equalize(Y, H)
    csi[bad] = 0.0      # バースト検出されたシンボルは消失扱い（Viterbi で情報なし）
    null_interfered_carriers(D, csi, mode, frame_start, layers)
    Yl = DI.data_to_logical(D, mode, frame_start)
    Wl = DI.data_to_logical(csi, mode, frame_start)
    I_seg = np.zeros(P.N_SEGMENTS, dtype=np.int64)
    for lyr in layers:
        I_seg[lyr.segments] = lyr.I
    # 逆変換の順序は送信チェーン（時間 3.11 → 周波数 3.12: セグメント間 → 回転 → ランダマイズ）の逆:
    # ランダマイズ・回転解除（data_to_logical）→ セグメント間 → 時間。
    # 2026-10-10 実放送（物理24ch）で検証: セグメント間を時間の後に置くと B 階層が復号不能になる。
    Xt = DI.time_deinterleave(DI.intersegment_deinterleave(Yl, groups), I_seg)
    Wt = DI.time_deinterleave(DI.intersegment_deinterleave(Wl, groups), I_seg)
    return H, Xt, Wt


def front_end(iq: np.ndarray, fs: float, mode: P.Mode, gi: str) -> dict:
    """録音全体を一度に処理する前段（短い信号・テスト・BER 測定用）。"""
    src = sync.CarrierSource(iq, fs, mode, gi)
    fs_info = frame_sync(src, mode)
    Y = src.symbols(0, src.n_sym)
    H, Xt, Wt = equalize_and_deinterleave(Y, mode, fs_info["frame_start"], fs_info["layers"], fs_info["groups"])
    out = {"sync": src.info(), "Y": Y, "H": H, "Xt": Xt, "Wt": Wt}
    out.update(fs_info)
    return out


def layer_first_frame(lyr: RxLayer, frame_start: int) -> int:
    """時間デインターリーブの参照先が揃う最初の受信フレーム番号。"""
    need = max(0, DI.time_deinterleave_max_delay(lyr.I) - frame_start)
    return -(-need // P.SYMBOLS_PER_FRAME)


def soft_bits_from_rows(Xt: np.ndarray, Wt: np.ndarray, rows: np.ndarray, lyr: RxLayer) -> np.ndarray:
    """指定シンボル行の階層データ → 軟判定ビット列（ビットデインターリーブ後、(n_slots, bps)）。"""
    stream = DI.layer_stream(Xt[rows], lyr.segments).reshape(-1)
    weight = DI.layer_stream(Wt[rows], lyr.segments).reshape(-1)
    soft = DI.demap_soft(stream, lyr.mod) * weight[:, None]
    return DI.bit_deinterleave(soft, lyr.mod)


def layer_soft_bits(fe: dict, lyr: RxLayer) -> tuple[np.ndarray, int] | None:
    """階層の軟判定ビット列と、その先頭の受信フレーム番号。1 フレームも無ければ None。"""
    f0, n_frames = fe["frame_start"], fe["n_frames"]
    fstart = layer_first_frame(lyr, f0)
    if fstart >= n_frames:
        return None
    rows = f0 + np.arange(fstart * P.SYMBOLS_PER_FRAME, n_frames * P.SYMBOLS_PER_FRAME)
    return soft_bits_from_rows(fe["Xt"], fe["Wt"], rows, lyr), fstart


def tx_frame_offset(lyr: RxLayer) -> int:
    """受信フレーム f の階層データが送信データフレームの何フレーム前に当たるか（送受合計遅延）。"""
    return (95 * lyr.I + P.time_interleave_adjust(lyr.I)) // P.SYMBOLS_PER_FRAME


# ---------------------------------------------------------------------------
# 階層ごとの復号
# ---------------------------------------------------------------------------
def decode_soft_bits(soft: np.ndarray, layer: RxLayer, mode: P.Mode) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """ビットデインターリーブ後の軟判定 (n_slots, bps) → (TS パケット (n, 188), RS 結果, Viterbi 出力ビット)。"""
    pairs = fec.depuncture(soft.reshape(-1), layer.rate)
    bits = fec.viterbi_decode(pairs)
    n_bytes = len(bits) // 8
    onair = np.packbits(bits[: n_bytes * 8])
    tsp_pf = P.tsp_per_frame(mode, layer.mod, layer.rate, len(layer.segments))
    T = (n_bytes // P.RS_N) // tsp_pf * tsp_pf
    onair = onair[: T * P.RS_N].reshape(T, P.RS_N)
    deint = fec.byte_deinterleave(onair)
    tsps = fec.energy_descramble_layer(deint, tsp_pf)
    corrected, nerr = fec.rs_decode(tsps)
    return corrected[:, :P.RS_K], nerr, bits


# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------
class _SegmentView:
    """TrackedCarrierSource のロック区間 [i0, i1) を、先頭を 0 とする CarrierSource 互換に見せる。"""

    def __init__(self, src, i0: int, i1: int):
        self._src, self._i0 = src, i0
        self.n_sym = i1 - i0

    def symbols(self, a: int, b: int) -> np.ndarray:
        a, b = max(0, a), min(b, self.n_sym)
        return self._src.symbols(self._i0 + a, self._i0 + b)

    def info(self) -> dict:
        info = dict(self._src.info())
        info["abs_symbol0"] = self._i0      # 区間先頭の（録音全体での）シンボル番号
        return info


def demodulate(iq: np.ndarray, fs: float, mode: P.Mode = P.JP_DEFAULT_MODE, gi: str = P.JP_DEFAULT_GI,
               *, block_frames: int = 6, return_debug: bool = False, track: bool = False):
    """I/Q（サンプルレート fs）→ 階層名 → TS パケット (n, 188)。return_debug で中間情報も返す。

    block_frames フレームずつ処理する。各ブロックには時間デインターリーブの履歴（最大 I の分）と
    等化の前後文脈を付け、ビット/バイトデインターリーブと Viterbi の立ち上がりのために
    直前 1 フレームを重ねて復号し、その分の出力は捨てる。

    track=True で TrackedCarrierSource を使う（長時間・クロックドリフト・瞬断向け）。
    ロック区間ごとにフレーム同期からやり直し、各階層の TS は区間をまたいで連結する
    （瞬断中のデータは欠落する。区間の内訳は debug["segments"]）。
    """
    if track:
        return _demodulate_tracked(iq, fs, mode, gi, block_frames=block_frames, return_debug=return_debug)
    src = sync.CarrierSource(iq, fs, mode, gi)
    out, dbg = _decode_source(src, mode, block_frames)
    if return_debug:
        dbg["sync"] = src.info()
        return out, dbg
    return out


def _decode_source(src, mode: P.Mode, block_frames: int) -> tuple[dict, dict]:
    """CarrierSource（互換オブジェクト）1 本をフレーム同期からブロック復号まで処理する。"""
    fsi = frame_sync(src, mode)
    f0, layers, groups, n_frames = fsi["frame_start"], fsi["layers"], fsi["groups"], fsi["n_frames"]
    hist = max((-(-DI.time_deinterleave_max_delay(lyr.I) // P.SYMBOLS_PER_FRAME) for lyr in layers), default=0)
    fstart = {lyr.name: layer_first_frame(lyr, f0) for lyr in layers}
    packets = {lyr.name: [] for lyr in layers}
    nerrs = {lyr.name: [] for lyr in layers}
    first_frame = {}
    n_blocks = 0
    step = max(1, block_frames)
    for b0 in range(0, n_frames, step):
        b1 = min(n_frames, b0 + step)
        fa = max(0, b0 - 1)
        s0 = max(0, f0 + (fa - hist) * P.SYMBOLS_PER_FRAME - EQ_CONTEXT_SYMBOLS)
        s1 = min(src.n_sym, f0 + b1 * P.SYMBOLS_PER_FRAME + EQ_CONTEXT_SYMBOLS)
        Y = src.symbols(s0, s1)
        _, Xt, Wt = equalize_and_deinterleave(Y, mode, f0 - s0, layers, groups)
        n_blocks += 1
        for lyr in layers:
            fa_l = max(fa, fstart[lyr.name])
            if fa_l >= b1:
                continue
            rows = (f0 - s0) + np.arange(fa_l * P.SYMBOLS_PER_FRAME, b1 * P.SYMBOLS_PER_FRAME)
            soft = soft_bits_from_rows(Xt, Wt, rows, lyr)
            pk, nerr, _ = decode_soft_bits(soft, lyr, mode)
            tsp_pf = P.tsp_per_frame(mode, lyr.mod, lyr.rate, len(lyr.segments))
            skip = (max(b0, fa_l) - fa_l) * tsp_pf
            packets[lyr.name].append(pk[skip:])
            nerrs[lyr.name].append(nerr[skip:])
            first_frame.setdefault(lyr.name, max(b0, fa_l))
    out = {name: (np.concatenate(v) if v else np.zeros((0, P.RS_K), dtype=np.uint8)) for name, v in packets.items()}
    dbg = {"frame_start": f0, "tmcc_parity_ok": fsi["tmcc_parity_ok"], "tmcc": fsi["tmcc"],
           "layers": layers, "n_frames": n_frames, "n_blocks": n_blocks, "first_frame": first_frame,
           "rs_errors": {name: (np.concatenate(v) if v else np.zeros(0, dtype=np.int64))
                         for name, v in nerrs.items()}}
    return out, dbg


def _demodulate_tracked(iq: np.ndarray, fs: float, mode: P.Mode, gi: str,
                        *, block_frames: int, return_debug: bool):
    """追従型: ロック区間ごとにフレーム同期からやり直し、階層 TS を区間をまたいで連結する。"""
    src = sync.TrackedCarrierSource(iq, fs, mode, gi)
    packets: dict[str, list] = {}
    nerrs: dict[str, list] = {}
    seg_dbg = []
    for (i0, i1) in src.segments():
        if i1 - i0 < 3 * P.SYMBOLS_PER_FRAME:       # フレーム同期 + 1 フレームに足りない
            seg_dbg.append({"symbols": (i0, i1), "skipped": "too short"})
            continue
        view = _SegmentView(src, i0, i1)
        try:
            out, dbg = _decode_source(view, mode, block_frames)
        except RuntimeError as e:
            seg_dbg.append({"symbols": (i0, i1), "skipped": str(e)})
            continue
        for name, pk in out.items():
            packets.setdefault(name, []).append(pk)
            nerrs.setdefault(name, []).append(dbg["rs_errors"][name])
        dbg["symbols"] = (i0, i1)
        seg_dbg.append(dbg)
    out = {name: (np.concatenate(v) if v else np.zeros((0, P.RS_K), dtype=np.uint8))
           for name, v in packets.items()}
    if return_debug:
        debug = {"sync": src.info(), "segments": seg_dbg,
                 "rs_errors": {name: (np.concatenate(v) if v else np.zeros(0, dtype=np.int64))
                               for name, v in nerrs.items()}}
        return out, debug
    return out


def demodulate_to_ber(iq: np.ndarray, fs: float, tx_debug: dict, mode: P.Mode = P.JP_DEFAULT_MODE,
                      gi: str = P.JP_DEFAULT_GI, skip_bits: int = 2048) -> dict:
    """変調器の debug（modulate(..., return_debug=True)）と突き合わせて階層ごとの BER を測る。

    raw_ber: ビットデインターリーブ後の硬判定 vs 送信側の符号化ビット（Viterbi 前）
    viterbi_ber: Viterbi 出力 vs 送信側のバイトインターリーブ後ビット列
    先頭 skip_bits はデインターリーブ・Viterbi の立ち上がりを除くため数えない。RS の統計も先頭 11 TSP を除く。
    """
    fe = front_end(iq, fs, mode, gi)
    res = {}
    for lyr in fe["layers"]:
        got = layer_soft_bits(fe, lyr)
        if got is None:
            continue
        soft, fstart = got
        tdbg = tx_debug["layers"][lyr.name]
        # 受信フレーム fstart の電波上のフレーム番号（信号先頭 = 送信フレーム 0）から送信データフレームへ
        onair_frame0 = int(np.round((fe["sync"]["abs_symbol0"] + fe["frame_start"]) / P.SYMBOLS_PER_FRAME))
        tx_frame = onair_frame0 + fstart - tx_frame_offset(lyr)
        if tx_frame < 0:
            continue
        bps = P.BITS_PER_SYMBOL[lyr.mod]
        slots_pf = len(lyr.segments) * P.MODE_PARAMS[mode].data_carriers_per_segment * P.SYMBOLS_PER_FRAME
        rx_raw = (soft.reshape(-1) < 0).astype(np.uint8)
        tx_raw = tdbg["coded_bits"][tx_frame * slots_pf * bps:]
        n = min(len(rx_raw), len(tx_raw))
        raw_err = int(np.count_nonzero(rx_raw[skip_bits:n] != tx_raw[skip_bits:n]))
        _, nerr, bits = decode_soft_bits(soft, lyr, mode)
        tsp_pf = P.tsp_per_frame(mode, lyr.mod, lyr.rate, len(lyr.segments))
        tx_info = fec.bytes_to_bits(tdbg["byte_interleaved"][tx_frame * tsp_pf:])
        m = min(len(bits), len(tx_info))
        vit_err = int(np.count_nonzero(bits[skip_bits:m] != tx_info[skip_bits:m]))
        res[lyr.name] = {"raw_ber": raw_err / max(1, n - skip_bits), "raw_bits": n - skip_bits,
                         "viterbi_ber": vit_err / max(1, m - skip_bits), "viterbi_bits": m - skip_bits,
                         # 先頭 11 TSP はバイトデインターリーブの履歴が無く常に訂正不能なので除く
                         "rs_uncorrectable": int((nerr[P.BYTE_INTERLEAVE_BRANCHES - 1:] < 0).sum()),
                         "tsp": max(0, len(nerr) - (P.BYTE_INTERLEAVE_BRANCHES - 1))}
    return res
