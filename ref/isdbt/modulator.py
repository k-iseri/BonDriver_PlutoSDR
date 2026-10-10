"""ISDB-T 変調器（試験信号生成専用、STD-B31 3 章の送信側処理）。

階層ごとの TS → RS(204,188) → エネルギー拡散 → バイトインターリーブ → 畳み込み符号 + パンクチャ
→ ビットインターリーブ → マッピング → 階層合成 → 時間インターリーブ → 周波数インターリーブ
→ OFDM フレーム構成（SP/CP/TMCC/AC）→ IFFT → GI 付加。

フレーム整合の規約（docs/isdbt-spec-notes.md 10 章）:
  各インターリーブ段の送信側遅延調整は「合計遅延が整数フレーム（バイト/時間）または
  2 OFDM シンボル（ビット）」になるよう仕様で定められている。整数フレームのずれは
  伝送上区別できないので、ここでは遅延ではなく「先読み」の形で実装し、
  OFDM フレームの先頭シンボルに各階層の TSP 境界が来るようにする。
  受信側は仕様どおりの補遺延（バイト: 11-b TSP、ビット: 120-d_i、時間: I*(95-m_i)）を
  入れれば TSP 境界がフレーム先頭に戻る。
出力は複素ベースバンド（complex64）、サンプルレート params.SAMPLE_RATE_HZ、平均電力 1。

復調器は本ファイルを逆順になぞるのではなく、仕様書（と gr-isdbt の受信側）を見て独立に書くこと。
"""
from __future__ import annotations

from dataclasses import dataclass, field

import numpy as np
from scipy import fft as sfft

from . import fec, pilots, tmcc, ts
from . import params as P


@dataclass
class LayerConfig:
    segments: int
    mod: P.Modulation
    rate: str
    I: int


@dataclass
class TxConfig:
    mode: P.Mode = P.JP_DEFAULT_MODE
    gi: str = P.JP_DEFAULT_GI
    layers: dict = field(default_factory=dict)      # "A"/"B"/"C" -> LayerConfig（この順でセグメント番号を割当）
    partial_reception: bool | None = None           # None: A が 1 セグメントで他階層があれば True
    extra_frames: int | None = None                 # None: 時間インターリーブ遅延分 + guard_frames を自動で付加
    guard_frames: int = 1                           # 末尾の余裕（チャンネル模擬で末尾が欠けても最終フレームを守る）

    def __post_init__(self):
        self.layers = {k: (v if isinstance(v, LayerConfig) else LayerConfig(**v))
                       for k, v in self.layers.items()}
        total = sum(lc.segments for lc in self.layers.values())
        if total != P.N_SEGMENTS:
            raise ValueError(f"segments must sum to {P.N_SEGMENTS}, got {total}")
        for name, lc in self.layers.items():
            if lc.I not in P.MODE_PARAMS[self.mode].time_interleave_choices:
                raise ValueError(f"layer {name}: I={lc.I} is not valid for {self.mode}")
            if lc.mod == P.Modulation.DQPSK:
                raise NotImplementedError("差動変調（DQPSK）セグメントは未対応")
        if self.partial_reception is None:
            a = self.layers.get("A")
            self.partial_reception = bool(a and a.segments == 1 and len(self.layers) > 1)

    def segment_numbers(self) -> dict:
        """階層名 → その階層が使うセグメント番号のリスト（A から順に 0, 1, ...）。"""
        out, nxt = {}, 0
        for name in tmcc.LAYER_NAMES:
            if name in self.layers:
                n = self.layers[name].segments
                out[name] = list(range(nxt, nxt + n))
                nxt += n
        return out

    def tmcc_info(self) -> tmcc.TmccInfo:
        layers = {name: tmcc.TmccLayer(lc.mod, lc.rate, lc.I, lc.segments)
                  for name, lc in self.layers.items()}
        return tmcc.TmccInfo(partial_reception=self.partial_reception, layers=layers)


def as_config(cfg) -> TxConfig:
    if isinstance(cfg, TxConfig):
        return cfg
    return TxConfig(**cfg)


# ---------------------------------------------------------------------------
# 階層ごとのビット列生成（3.5 〜 3.8）
# ---------------------------------------------------------------------------
def encode_layer(packets: np.ndarray, lc: LayerConfig, mode: P.Mode, n_frames: int, debug: dict | None = None):
    """TS (n,188) → パンクチャ後の直列ビット列。n_frames フレーム分（+ 先読み用に 1 フレーム）。"""
    tsp_pf = P.tsp_per_frame(mode, lc.mod, lc.rate, lc.segments)
    pk = ts.pad_packets(packets, (n_frames + 1) * tsp_pf)
    tsp = fec.rs_encode(pk)                                     # 3.5 外符号
    disp = fec.energy_dispersal_layer(tsp, tsp_pf)              # 3.6 エネルギー拡散（フレームごとに初期化）
    inter = fec.byte_interleave(disp)                           # 3.7 バイトインターリーブ（フレーム整合形）
    bits = fec.bytes_to_bits(inter)
    coded = fec.conv_encode_punctured(bits, lc.rate)            # 3.8 内符号
    if debug is not None:
        debug.update(packets=pk, tsp=tsp, dispersed=disp, byte_interleaved=inter, coded_bits=coded)
    return coded


def bit_interleave_and_map(coded: np.ndarray, lc: LayerConfig, mode: P.Mode, n_frames: int) -> np.ndarray:
    """直列ビット → ビットインターリーブ（先読み形）→ マッピング。戻り値は (n_sym, nseg*dc) の複素。"""
    mp = P.MODE_PARAMS[mode]
    bps = P.BITS_PER_SYMBOL[lc.mod]
    slots_per_sym = lc.segments * mp.data_carriers_per_segment
    n_sym = n_frames * P.SYMBOLS_PER_FRAME
    n_slots = n_sym * slots_per_sym
    delays = np.array(P.BIT_INTERLEAVE_DELAYS[lc.mod])
    need = n_slots + P.BIT_INTERLEAVE_MAX_DELAY
    streams = coded[: need * bps].reshape(need, bps)            # 直並列変換: 列 i がビット列 b_i
    # 送信側遅延 d_i + 調整（合計 2 シンボル）を、整数フレームを無視して先読み (120 - d_i) に置換
    t = np.arange(n_slots)[:, None] + (P.BIT_INTERLEAVE_MAX_DELAY - delays)[None, :]
    inter = streams[t, np.arange(bps)[None, :]]
    sym = P.map_bits(inter, lc.mod)                               # 3.9 マッピング
    return sym.reshape(n_sym, slots_per_sym)


# ---------------------------------------------------------------------------
# 階層合成・時間/周波数インターリーブ（3.10 〜 3.11）
# ---------------------------------------------------------------------------
def interleave_groups(cfg: TxConfig) -> list:
    """セグメント間インターリーブの対象グループ（STD-B31 3.11.2）。

    部分受信セグメント（0）は対象外。差動変調部と同期変調部はそれぞれ別グループで、
    階層が異なっても同じ変調部ならまとめて行う。現状は同期変調のみ対応。
    """
    partial = [0] if cfg.partial_reception else []
    coherent = [s for s in range(P.N_SEGMENTS) if s not in partial]
    return [g for g in (partial, coherent) if len(g) >= 2]


def hierarchical_combine(cfg: TxConfig, layer_symbols: dict, n_sym: int) -> np.ndarray:
    """各階層の (n_sym, nseg*dc) → 論理キャリア行列 X (n_sym, 13, dc)。添字 1 はセグメント番号。

    階層合成（図 3-21）: 各階層の直列シンボルをその階層のデータセグメントへ順に詰める
    （セグメント k のキャリア i = 階層内 k'*dc + i）。
    セグメント間インターリーブはここでは行わない。送信チェーンの順序は
    時間インターリーブ（3.11）→ 周波数インターリーブ（3.12: セグメント間 → 回転 → ランダマイズ）
    であり、セグメント間は frequency_interleave 側にある。
    2026-10-10 実放送（物理24ch、B 階層復号成功）で順序を検証済み。
    """
    mp = P.MODE_PARAMS[cfg.mode]
    dc = mp.data_carriers_per_segment
    X0 = np.zeros((n_sym, P.N_SEGMENTS, dc), dtype=np.complex64)
    for name, segs in cfg.segment_numbers().items():
        X0[:, segs, :] = layer_symbols[name].reshape(n_sym, len(segs), dc)
    return X0


def time_interleave(cfg: TxConfig, X: np.ndarray) -> np.ndarray:
    """論理キャリア行列の時間インターリーブ。キャリア c の遅延 = I*((5c) mod 96) + 調整。"""
    n_sym, _, dc = X.shape
    Y = np.zeros_like(X)
    c = np.arange(dc)
    n = np.arange(n_sym)
    for name, segs in cfg.segment_numbers().items():
        I = cfg.layers[name].I
        delay = P.time_interleave_delay(c, I) + P.time_interleave_adjust(I)
        src = n[:, None] - delay[None, :]
        valid = src >= 0
        for s in segs:
            Y[:, s, :] = np.where(valid, X[np.clip(src, 0, None), s, c[None, :]], 0)
    return Y


def frequency_interleave(cfg: TxConfig, Y: np.ndarray) -> np.ndarray:
    """周波数インターリーブ（3.12）: セグメント間 → セグメント内ローテーション → ランダマイズ。
    出力 Z[n, s, p] の p は伝送上のデータキャリア番号。

    セグメント間（図 3-26）: グループ内の直列番号 q（セグメント順に連結）は
    セグメント group[q mod n]、セグメント内キャリア q div n に置かれる。部分受信セグメントは対象外。
    ローテーション: キャリア c → (c - s) mod dc（s = セグメント番号）。
    ランダマイズ: 位置 r → perm[r]。
    # VERIFY: ローテーションの向きは gr-isdbt の受信側（実放送で検証済み）の逆写像に合わせた。
    セグメント間が時間インターリーブの後であることは 2026-10-10 実放送で検証済み。
    """
    n_sym, _, dc = Y.shape
    X = Y.copy()
    for group in interleave_groups(cfg):
        n = len(group)
        S = Y[:, group, :].reshape(n_sym, n * dc)              # S[q]、q = k*dc + i（セグメント順）
        X[:, group, :] = np.transpose(S.reshape(n_sym, dc, n), (0, 2, 1))   # seg k' carrier i' = S[i'*n + k']
    perm = np.array(P.CARRIER_RANDOMIZE_TABLE[cfg.mode])
    Z = np.zeros_like(Y)
    c = np.arange(dc)
    for s in range(P.N_SEGMENTS):
        pos = perm[(c - s) % dc]
        Z[:, s, pos] = X[:, s, :]
    return Z


# ---------------------------------------------------------------------------
# OFDM フレーム構成（3.12 〜 3.15）と IFFT
# ---------------------------------------------------------------------------
def build_carriers(cfg: TxConfig, Z: np.ndarray, tmcc_frames: np.ndarray) -> np.ndarray:
    """Z (n_sym, 13, dc) と各フレームの TMCC ビット (n_frames, 204) → キャリア行列 C (n_sym, total)。"""
    mode = cfg.mode
    mp = P.MODE_PARAMS[mode]
    n_sym = Z.shape[0]
    C = np.zeros((n_sym, mp.total_carriers), dtype=np.complex64)
    pv = pilots.pilot_values(mode).astype(np.complex64)
    dpos4 = pilots.data_carrier_positions_4(mode)                # (4, 13[freq], dc)
    order = np.array(P.SEGMENT_ORDER_IN_FREQ)
    n = np.arange(n_sym)
    for ph in range(P.SP_SYMBOL_PERIOD):
        sel = n[n % P.SP_SYMBOL_PERIOD == ph]
        data = Z[sel][:, order, :].reshape(len(sel), -1)         # 周波数順セグメントに並べ替え
        C[sel[:, None], dpos4[ph].reshape(1, -1)] = data
        sp = pilots.sp_positions(mode, ph)
        C[sel[:, None], sp[None, :]] = pv[sp][None, :]
    cp = pilots.cp_position(mode)
    C[:, cp] = pv[cp]
    # AC1: 付加情報なし → B0 基準、B1..B203 はスタッフィング 1（3.13.4）。DBPSK で毎シンボル反転する
    ac = np.array(P.AC1_POSITIONS_ABS[mode])
    ac_bits = np.ones(P.SYMBOLS_PER_FRAME, dtype=np.uint8)
    ac_bits[0] = 0
    d_ac = np.tile(tmcc.differential_encode(ac_bits), tmcc_frames.shape[0])[:n_sym]
    C[:, ac] = pv[ac][None, :] * (1.0 - 2.0 * d_ac[:, None]).astype(np.complex64)
    # TMCC: DBPSK、B'_0 = W_i、B'_k = B'_{k-1} xor B_k、0 → +4/3、1 → -4/3（3.13.3）
    tm = np.array(P.TMCC_POSITIONS_ABS[mode])
    d = np.concatenate([tmcc.differential_encode(b) for b in tmcc_frames])[:n_sym]
    C[:, tm] = pv[tm][None, :] * (1.0 - 2.0 * d[:, None]).astype(np.complex64)
    return C


def ofdm_modulate(C: np.ndarray, mode: P.Mode, gi: str, chunk: int = 64) -> np.ndarray:
    """キャリア行列 → IFFT → GI 付加 → 直列化（complex64、平均電力 1 に正規化）。"""
    mp = P.MODE_PARAMS[mode]
    N = mp.fft_size
    ng = P.guard_samples(mode, gi)
    bins = P.carrier_to_fft_bin(mode, np.arange(mp.total_carriers))
    n_sym = C.shape[0]
    out = np.empty((n_sym, N + ng), dtype=np.complex64)
    for i in range(0, n_sym, chunk):
        spec = np.zeros((min(chunk, n_sym - i), N), dtype=np.complex64)
        spec[:, bins] = C[i:i + len(spec)]
        x = sfft.ifft(spec, axis=1, workers=-1).astype(np.complex64)
        out[i:i + len(spec), ng:] = x
        out[i:i + len(spec), :ng] = x[:, N - ng:]
    out = out.ravel()
    rms = np.sqrt(np.mean(np.abs(out) ** 2))
    return (out / rms).astype(np.complex64)


# ---------------------------------------------------------------------------
# 本体
# ---------------------------------------------------------------------------
def frames_for(cfg: TxConfig, ts_by_layer: dict) -> tuple[int, int]:
    """(データフレーム数, 付加フレーム数)。付加分は時間インターリーブ遅延で押し出される分。"""
    data_frames = 1
    extra = 0
    for name, lc in cfg.layers.items():
        tsp_pf = P.tsp_per_frame(cfg.mode, lc.mod, lc.rate, lc.segments)
        n_pk = len(ts_by_layer.get(name, ())) if ts_by_layer.get(name) is not None else 0
        data_frames = max(data_frames, ts.frames_needed(n_pk, tsp_pf))
        total_delay = 95 * lc.I + P.time_interleave_adjust(lc.I)
        extra = max(extra, -(-total_delay // P.SYMBOLS_PER_FRAME))
    extra += cfg.guard_frames
    if cfg.extra_frames is not None:
        extra = cfg.extra_frames
    return data_frames, extra


def modulate(ts_by_layer: dict, cfg, *, return_debug: bool = False):
    """階層名 → TS パケット配列 (n,188) を受け取り、複素ベースバンド I/Q を返す。

    return_debug=True なら (iq, debug) を返す。debug には各段の中間データが入る。
    """
    cfg = as_config(cfg)
    data_frames, extra = frames_for(cfg, ts_by_layer)
    n_frames = data_frames + extra
    n_sym = n_frames * P.SYMBOLS_PER_FRAME
    debug: dict = {"config": cfg, "n_frames": n_frames, "data_frames": data_frames, "layers": {}}

    layer_symbols = {}
    for name, lc in cfg.layers.items():
        pk = ts_by_layer.get(name)
        pk = np.zeros((0, ts.TS_PACKET), dtype=np.uint8) if pk is None else np.asarray(pk, dtype=np.uint8)
        ldbg: dict = {}
        coded = encode_layer(pk, lc, cfg.mode, n_frames, ldbg if return_debug else None)
        layer_symbols[name] = bit_interleave_and_map(coded, lc, cfg.mode, n_frames)
        if return_debug:
            ldbg["mapped"] = layer_symbols[name]
            debug["layers"][name] = ldbg

    X = hierarchical_combine(cfg, layer_symbols, n_sym)
    Y = time_interleave(cfg, X)
    Z = frequency_interleave(cfg, Y)
    info = cfg.tmcc_info()
    tmcc_frames = np.stack([tmcc.build_frame_bits(cfg.mode, info, f) for f in range(n_frames)])
    C = build_carriers(cfg, Z, tmcc_frames)
    iq = ofdm_modulate(C, cfg.mode, cfg.gi)
    if return_debug:
        debug.update(X=X, Y=Y, Z=Z, carriers=C, tmcc_bits=tmcc_frames, tmcc_info=info)
        return iq, debug
    return iq
