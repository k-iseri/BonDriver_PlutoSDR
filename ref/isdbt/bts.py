"""BTS（放送 TS: 204 バイト多重フレーム）の生成と解析（ARIB STD-B31 3.2 / 添付 5.5）。

多重フレーム（STD-B31 3.2.1）:
  - 1 多重フレーム = n 個の伝送 TSP（204 バイト）。n は表 3-5（モードと GI で決まり、
    FFT サンプルクロックの 4 倍のクロックで送ると OFDM フレーム長と一致する）。
    n = 204 シンボル × (N + Ng) クロック / (204 バイト × 8 / 4) = symbol_samples × 204 / 408
  - TSP の並び（どのスロットがどの階層か）はモデル受信機（3.2.2、図 3-4）の動作で定まる:
      * 階層分割への入力はセグメント番号順・キャリア番号順（3.2.2.1、図 3-5）。
        1 キャリアシンボル = 1 クロックでシンボル先頭から詰められ、残りはヌル。
      * 階層 X の k 番目の入力で階層バッファに積まれるマザー符号ビット数は
        B_X,k = 2(⌊k·Sx·Rx⌋ − ⌊(k−1)·Sx·Rx⌋)（3.2.2.2。Sx は表 3-6: QPSK 2 / 16QAM 4 / 64QAM 6）。
        408 バイト（= 1 TSP 分のマザー符号）貯まるたびに S1 が TS バッファへ転送する。
      * TS 再生部は TSP 周期（408 クロック）ごとに TS バッファを確認し、
        1 TSP 分あれば出力、無ければヌル TSP を出力する。
      * TS 再生部は 2 系統あり、S3（入力側）はモード 1: OFDM フレームごと、モード 2:
        1/2 フレーム（102 シンボル）、モード 3: 1/4 フレーム（51 シンボル）で交互に切替。
        S4（出力側）は S3 の 3 TSP 周期後に同じ側へ切り替わる。
  - 1 フレームあたりの各階層の TSP 数はちょうど tsp_per_frame になり、パターンは
    フレームごとに同一（モード 1 は S3 がフレーム単位なので 2 フレーム周期）。

ISDB-T 情報（添付 5.5.2、表 5-6 / 5-7）:
  - 伝送 TSP は [188 バイト TS | 8 バイト ISDB-T 情報 | 8 バイト パリティ（オプション）]。
  - パリティは短縮 RS(204,196)。原始多項式は本編 3.3 と同じ x^8+x^4+x^3+x^2+1、
    生成多項式の根は λ^0..λ^7（λ = 02hex）。
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

from . import fec
from . import params as P

TSP_BYTES = 204
TS_BYTES = 188

# 表 3-6: 変調方式ごとの Sx（キャリアシンボルあたりのビット数と同じ）
_SX = {P.Modulation.DQPSK: 2, P.Modulation.QPSK: 2, P.Modulation.QAM16: 4, P.Modulation.QAM64: 6}
# S3 の切替間隔（シンボル）: モード 1 はフレームごと、2 は 1/2、3 は 1/4（3.2.2.2）
_S3_INTERVAL = {P.Mode.MODE1: 204, P.Mode.MODE2: 102, P.Mode.MODE3: 51}

LAYER_INDICATOR = {"null": 0b0000, "A": 0b0001, "B": 0b0010, "C": 0b0011}
_IND_TO_LAYER = {v: k for k, v in LAYER_INDICATOR.items()}


def mux_tsps_per_frame(mode: P.Mode, gi: str) -> int:
    """1 多重フレームの伝送 TSP 数（表 3-5）。= symbol_samples × 204 / 408。"""
    n = P.symbol_samples(mode, gi) * P.SYMBOLS_PER_FRAME
    assert n % 408 == 0
    return n // 408


@dataclass(frozen=True)
class SlotLayer:
    name: str               # "A"/"B"/"C"
    segments: int
    mod: P.Modulation
    rate: str


def slot_pattern(mode: P.Mode, gi: str, layers: list[SlotLayer]) -> list[tuple[str | None, int]]:
    """定常状態の 1 多重フレーム分のスロット割当 [(階層名 or None, 階層内 TSP 番号), ...]。

    モデル受信機（3.2.2）を 3 フレームぶんシミュレートし、2 フレーム目と 3 フレーム目の
    パターンが一致する（定常）ことを確認して返す。モード 1 は S3 がフレーム単位で
    切り替わるため 2 フレーム周期になりうる。その場合は偶数フレームのパターンを返す。
    """
    mp = P.MODE_PARAMS[mode]
    T = P.symbol_samples(mode, gi)                   # 1 シンボルのクロック数
    dc = mp.data_carriers_per_segment
    n_slots = mux_tsps_per_frame(mode, gi)
    s3_T = _S3_INTERVAL[mode] * T                    # S3 切替間隔（クロック）
    s4_delay = 3 * 408                               # S4 は S3 の 3 TSP 周期後（3.2.2.2）

    # 各階層の 1 シンボル内でのクロックオフセット（セグメント番号順に詰める。3.2.2.1 図 3-5）
    offs = {}
    pos = 0
    for lyr in layers:
        offs[lyr.name] = pos
        pos += lyr.segments * dc

    def section_of_clock(t: int) -> int:
        return (t // s3_T) % 2

    def section_of_slot(j: int) -> int:
        t = j * 408
        if t < s3_T + s4_delay:
            return 0
        return ((t - s4_delay) // s3_T) % 2

    n_frames_sim = 3
    fifos: list[list[tuple[int, str, int]]] = [[], []]   # (完成クロック, 階層, 通し番号)
    done = {lyr.name: 0 for lyr in layers}               # 階層ごとの完成 TSP 通し番号
    patterns = []
    for f in range(n_frames_sim):
        base = f * P.SYMBOLS_PER_FRAME * T
        # 1 フレーム分の TSP 完成イベントを作る（k は多重フレーム内で階層ごとにリセット）
        events: list[tuple[int, str]] = []
        for lyr in layers:
            sx_rx = _SX[lyr.mod] * P.CODE_RATES[lyr.rate]
            k_per_sym = lyr.segments * dc
            tsps = 0
            for n_sym in range(P.SYMBOLS_PER_FRAME):
                k0 = n_sym * k_per_sym
                # このシンボル中に完成する TSP の k を二分で求めるまでもなく逐次で数える
                bits_end = int((k0 + k_per_sym) * sx_rx)          # ⌊k·Sx·Rx⌋（マザー符号の半分）
                while (tsps + 1) * 1632 <= bits_end:
                    # (tsps+1)·1632 ビット目に到達する k を求める
                    need = (tsps + 1) * 1632
                    # k·sx_rx ≥ need となる最小の k
                    k = -(-need * sx_rx.denominator // sx_rx.numerator)
                    t = base + n_sym * T + offs[lyr.name] + (k - 1 - k0)
                    events.append((t, lyr.name))
                    tsps += 1
            assert tsps == P.tsp_per_frame(mode, lyr.mod, lyr.rate, lyr.segments)
        events.sort()
        for t, name in events:
            fifos[section_of_clock(t)].append((t, name, done[name]))
            done[name] += 1
        # 出力スロット
        pat: list[tuple[str | None, int]] = []
        for j in range(n_slots):
            t_j = (f * n_slots + j) * 408
            sec = section_of_slot(f * n_slots + j)
            q = fifos[sec]
            if q and q[0][0] <= t_j:
                _, name, _ = q.pop(0)
                pat.append((name, -1))
            else:
                pat.append((None, -1))
        patterns.append(pat)

    if patterns[1] != patterns[2]:
        raise NotImplementedError("多重フレームパターンが 1 フレーム周期でない（モード 1 の 2 フレーム周期は未対応）")
    pat = patterns[1]
    # 階層内の通し番号を振り直す
    counters = {lyr.name: 0 for lyr in layers}
    out: list[tuple[str | None, int]] = []
    for name, _ in pat:
        if name is None:
            out.append((None, -1))
        else:
            out.append((name, counters[name]))
            counters[name] += 1
    for lyr in layers:
        assert counters[lyr.name] == P.tsp_per_frame(mode, lyr.mod, lyr.rate, lyr.segments)
    return out


# ---------------------------------------------------------------------------
# ISDB-T 情報（表 5-6 / 5-7）
# ---------------------------------------------------------------------------
def isdbt_info_bytes(layer: str | None, tsp_counter: int, frame_head: bool, frame_odd: bool,
                     countdown: int = 0b1111) -> np.ndarray:
    """8 バイトの ISDB-T 情報。AC データなし（stuffing 全 1）。"""
    b = np.empty(8, dtype=np.uint8)
    # byte0: TMCC_identifier '10'（地上デジタル）、reserved '1'、各フラグ 0、フレーム頭、偶奇
    b[0] = 0b10100000 | (int(frame_head) << 1) | int(frame_odd)
    ind = LAYER_INDICATOR["null" if layer is None else layer]
    b[1] = (ind << 4) | (countdown & 0xF)
    # byte2: AC_data_invalid '1'、AC_data_effective_bytes '11'、TSP カウンタ上位 5 ビット
    b[2] = 0b11100000 | ((tsp_counter >> 8) & 0x1F)
    b[3] = tsp_counter & 0xFF
    b[4:8] = 0xFF
    return b


def parse_isdbt_info(b: np.ndarray) -> dict:
    layer_ind = (int(b[1]) >> 4) & 0xF
    return {
        "tmcc_identifier": (int(b[0]) >> 6) & 0b11,
        "frame_head": bool(int(b[0]) & 0b10),
        "frame_odd": bool(int(b[0]) & 0b1),
        "layer": _IND_TO_LAYER.get(layer_ind, f"0b{layer_ind:04b}"),
        "countdown": int(b[1]) & 0xF,
        "ac_invalid": bool(int(b[2]) & 0x80),
        "tsp_counter": ((int(b[2]) & 0x1F) << 8) | int(b[3]),
    }


# ---------------------------------------------------------------------------
# RS(204,196) パリティ（5.5.2 (1)。λ = 02、根 λ^0..λ^7）
# ---------------------------------------------------------------------------
def _rs196_generator() -> np.ndarray:
    """g(x) = Π_{i=0}^{7} (x - λ^i)。GF は本編 3.3 と同一なので fec のテーブルを使う。"""
    g = np.array([1], dtype=np.int64)
    for i in range(8):
        root = fec._GF_EXP[i]
        new = np.zeros(len(g) + 1, dtype=np.int64)
        new[:-1] ^= g
        new[1:] ^= fec.gf_mul(g, root)
        g = new
    return g


_RS196_GEN = _rs196_generator()


def rs204_196_parity(blocks: np.ndarray) -> np.ndarray:
    """(n, 196) [TS188 + ISDB-T 情報 8] → (n, 8) のパリティ。"""
    pk = np.asarray(blocks, dtype=np.int64)
    assert pk.ndim == 2 and pk.shape[1] == 196
    n = pk.shape[0]
    rem = np.zeros((n, 8), dtype=np.int64)
    gtail = _RS196_GEN[1:]
    for j in range(196):
        fb = rem[:, 0] ^ pk[:, j]
        shifted = np.concatenate([rem[:, 1:], np.zeros((n, 1), dtype=np.int64)], axis=1)
        rem = shifted ^ fec.gf_mul(fb[:, None], gtail[None, :])
    return rem.astype(np.uint8)


def rs204_196_check(tsps: np.ndarray) -> np.ndarray:
    """(n, 204) の各行が RS(204,196) 符号語か（シンドローム全 0 か）。"""
    c = np.asarray(tsps, dtype=np.int64)
    ok = np.ones(c.shape[0], dtype=bool)
    for i in range(8):
        root = fec._GF_EXP[i]
        acc = np.zeros(c.shape[0], dtype=np.int64)
        for j in range(204):
            acc = fec.gf_mul(acc, root) ^ c[:, j]
        ok &= acc == 0
    return ok


# ---------------------------------------------------------------------------
# 組み立てと分解
# ---------------------------------------------------------------------------
def null_ts() -> np.ndarray:
    pk = np.full(TS_BYTES, 0xFF, dtype=np.uint8)
    pk[0] = 0x47
    pk[1] = 0x1F
    pk[2] = 0xFF
    pk[3] = 0x10
    return pk


def build_bts(layer_tsps: dict[str, np.ndarray], mode: P.Mode, gi: str, layers: list[SlotLayer],
              first_frame_odd: bool = False, parity: bool = True) -> np.ndarray:
    """階層ごとの TS (n_X, 188) → BTS (n_frames × n_slots, 204)。

    n_X は全階層で同じフレーム数（tsp_per_frame の倍数）であること。
    """
    n_slots = mux_tsps_per_frame(mode, gi)
    pat = slot_pattern(mode, gi, layers)
    tpf = {lyr.name: P.tsp_per_frame(mode, lyr.mod, lyr.rate, lyr.segments) for lyr in layers}
    n_frames = None
    for lyr in layers:
        pk = layer_tsps[lyr.name]
        assert pk.shape[0] % tpf[lyr.name] == 0, (lyr.name, pk.shape)
        nf = pk.shape[0] // tpf[lyr.name]
        assert n_frames is None or nf == n_frames, "階層間でフレーム数が揃っていない"
        n_frames = nf
    assert n_frames is not None

    out = np.zeros((n_frames * n_slots, TSP_BYTES), dtype=np.uint8)
    nul = null_ts()
    for f in range(n_frames):
        odd = (f % 2 == 1) ^ first_frame_odd
        for j, (name, idx) in enumerate(pat):
            row = f * n_slots + j
            if name is None:
                out[row, :TS_BYTES] = nul
            else:
                out[row, :TS_BYTES] = layer_tsps[name][f * tpf[name] + idx]
            out[row, TS_BYTES:196] = isdbt_info_bytes(name, j, frame_head=(j == 0), frame_odd=odd)
        if parity:
            out[f * n_slots:(f + 1) * n_slots, 196:] = rs204_196_parity(
                out[f * n_slots:(f + 1) * n_slots, :196])
    return out


def split_bts(bts: np.ndarray) -> dict:
    """BTS (n, 204) → {"A"/"B"/"C": TS (m, 188), "info": 解析結果の要約}。

    ヌル TSP（layer_indicator 0000）は捨てる。
    """
    bts = np.asarray(bts, dtype=np.uint8).reshape(-1, TSP_BYTES)
    ind = (bts[:, TS_BYTES + 1].astype(int) >> 4) & 0xF
    out: dict = {"n_tsp": int(bts.shape[0])}
    for name, code in LAYER_INDICATOR.items():
        if name == "null":
            out["nulls"] = int((ind == code).sum())
            continue
        sel = ind == code
        if sel.any():
            out[name] = bts[sel, :TS_BYTES].copy()
    heads = np.flatnonzero((bts[:, TS_BYTES] & 0b10) != 0)
    out["frame_heads"] = heads
    counters = ((bts[:, TS_BYTES + 2].astype(int) & 0x1F) << 8) | bts[:, TS_BYTES + 3].astype(int)
    out["tsp_counters"] = counters
    return out
