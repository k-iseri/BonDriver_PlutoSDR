"""誤り訂正符号・ランダマイズ・バイトインターリーブ（STD-B31 3.5〜3.8）。

送信側（符号化）の実装。復号側（RS 復号、Viterbi）は復調器実装時に追加する。
ref/ なので速度より可読性を優先し、仕様の記述に対応づけて書く。
"""
from __future__ import annotations

import numpy as np

from . import params as P

# ---------------------------------------------------------------------------
# GF(2^8) と RS(204,188)（STD-B31 3.5「外符号」）
# ---------------------------------------------------------------------------
_GF_EXP = np.zeros(512, dtype=np.int64)
_GF_LOG = np.zeros(256, dtype=np.int64)


def _build_gf_tables():
    x = 1
    for i in range(255):
        _GF_EXP[i] = x
        _GF_LOG[x] = i
        x <<= 1
        if x & 0x100:
            x ^= P.RS_PRIM_POLY
    for i in range(255, 512):
        _GF_EXP[i] = _GF_EXP[i - 255]


_build_gf_tables()


def gf_mul(a, b):
    """GF(2^8) の乗算（配列対応）。"""
    a = np.asarray(a, dtype=np.int64)
    b = np.asarray(b, dtype=np.int64)
    out = _GF_EXP[(_GF_LOG[a] + _GF_LOG[b]) % 255]
    return np.where((a == 0) | (b == 0), 0, out)


def rs_generator_poly() -> np.ndarray:
    """g(x) = Π_{i=0}^{15} (x - α^i)。係数は最高次から並べ、先頭は 1。"""
    g = np.array([1], dtype=np.int64)
    for i in range(P.RS_NROOTS):
        root = _GF_EXP[P.RS_FIRST_ROOT + i]
        # g(x) * (x + root)
        new = np.zeros(len(g) + 1, dtype=np.int64)
        new[:-1] ^= g
        new[1:] ^= gf_mul(g, root)
        g = new
    return g


_RS_GEN = rs_generator_poly()


def rs_encode(packets: np.ndarray) -> np.ndarray:
    """(n, 188) の TS パケット → (n, 204) の TSP（末尾 16 バイトがパリティ）。

    RS(255,239) を先頭 51 バイトのゼロで短縮した系統的符号化。同期バイトも符号化対象。
    """
    pk = np.asarray(packets, dtype=np.int64)
    assert pk.ndim == 2 and pk.shape[1] == P.RS_K, pk.shape
    n = pk.shape[0]
    rem = np.zeros((n, P.RS_NROOTS), dtype=np.int64)
    gtail = _RS_GEN[1:]  # 先頭係数 1 を除いた 16 個
    for j in range(P.RS_K):
        fb = rem[:, 0] ^ pk[:, j]
        shifted = np.concatenate([rem[:, 1:], np.zeros((n, 1), dtype=np.int64)], axis=1)
        rem = shifted ^ gf_mul(fb[:, None], gtail[None, :])
    return np.concatenate([pk, rem], axis=1).astype(np.uint8)


def rs_syndromes(tsps: np.ndarray) -> np.ndarray:
    """(n, 204) → (n, 16) のシンドローム S_i = c(α^i)。誤りなしなら全 0。"""
    c = np.asarray(tsps, dtype=np.int64)
    n_syms = c.shape[1]
    out = np.zeros((c.shape[0], P.RS_NROOTS), dtype=np.int64)
    for i in range(P.RS_NROOTS):
        root = _GF_EXP[P.RS_FIRST_ROOT + i]
        acc = np.zeros(c.shape[0], dtype=np.int64)
        for j in range(n_syms):
            acc = gf_mul(acc, root) ^ c[:, j]
        out[:, i] = acc
    return out


# ---------------------------------------------------------------------------
# エネルギー拡散（STD-B31 3.6）
# ---------------------------------------------------------------------------
def energy_dispersal_prbs(n_bytes: int) -> np.ndarray:
    """初期化直後から n_bytes バイト分の PRBS（x^15+x^14+1、初期値 100101010000000）。

    各バイトは出力ビットを MSB から詰める。先頭は 0x03, 0xF6, 0x08, 0x34, ...（DVB と同じ系列）。
    """
    reg = list(map(int, f"{P.ENERGY_DISPERSAL_INIT:015b}"))  # reg[0] が段 1
    out = np.zeros(n_bytes, dtype=np.uint8)
    for i in range(n_bytes):
        v = 0
        for _ in range(8):
            fb = reg[13] ^ reg[14]          # 段 14 と段 15 の XOR
            v = (v << 1) | fb
            reg = [fb] + reg[:-1]
        out[i] = v
    return out


def energy_dispersal_frame(tsps: np.ndarray) -> np.ndarray:
    """1 フレーム分（1 階層）の TSP (n, 204) [sync, data187, parity16] → 伝送順バイト列 (n, 204)。

    PRBS はフレーム先頭で初期化。伝送順は [同期バイト直後の 203 バイト ^ PRBS, 同期バイト]。
    同期バイトは XOR しないが PRBS は 8 クロック進む。
    # VERIFY: STD-B31 3.6 の図との照合（gr-isdbt tx/rx の規約に合わせている）
    """
    tsps = np.asarray(tsps, dtype=np.uint8)
    n = tsps.shape[0]
    prbs = energy_dispersal_prbs(n * P.RS_N).reshape(n, P.RS_N)
    out = np.empty_like(tsps)
    out[:, :P.RS_N - 1] = tsps[:, 1:] ^ prbs[:, :P.RS_N - 1]
    out[:, P.RS_N - 1] = tsps[:, 0]
    return out


def energy_dispersal_layer(tsps: np.ndarray, tsp_per_frame: int) -> np.ndarray:
    """複数フレーム分をまとめて処理する。n は tsp_per_frame の倍数であること。"""
    tsps = np.asarray(tsps, dtype=np.uint8)
    assert tsps.shape[0] % tsp_per_frame == 0
    n_frames = tsps.shape[0] // tsp_per_frame
    return np.concatenate([energy_dispersal_frame(tsps[f * tsp_per_frame:(f + 1) * tsp_per_frame])
                           for f in range(n_frames)], axis=0)


# ---------------------------------------------------------------------------
# バイトインターリーブ（STD-B31 3.7）
# ---------------------------------------------------------------------------
def byte_interleave(stream: np.ndarray) -> np.ndarray:
    """伝送順 TSP 列 (T, 204) → バイトインターリーブ後 (T, 204)。

    畳み込み型 12 ブランチ × 17 バイト。バイト位置 j（フレーム先頭から数えて）はブランチ
    j mod 12 に入り、ブランチ b は b*17*12 = b TSP 分遅延する。送信側は更に遅延調整を加えて
    合計遅延を 1 フレームにするが、整数フレームのずれは伝送上区別できないため、ここでは
    「フレーム整合済み」の形 out[t][j] = in[t + 11 - (j mod 12)][j] で実装する（11 TSP 先読み）。
    末尾で先読み先が無い部分は 0 で埋める。受信側（遅延 11 - b）を通すと恒等になる。
    """
    s = np.asarray(stream, dtype=np.uint8)
    T = s.shape[0]
    pad = np.concatenate([s, np.zeros((P.BYTE_INTERLEAVE_BRANCHES - 1, P.RS_N), dtype=np.uint8)], axis=0)
    j = np.arange(P.RS_N)
    branch = j % P.BYTE_INTERLEAVE_BRANCHES
    t = np.arange(T)[:, None] + (P.BYTE_INTERLEAVE_BRANCHES - 1) - branch[None, :]
    return pad[t, j[None, :]]


def byte_deinterleave(stream: np.ndarray) -> np.ndarray:
    """byte_interleave の逆（受信側: ブランチ b を 11 - b TSP 遅延）。先頭 11 TSP は不定。"""
    s = np.asarray(stream, dtype=np.uint8)
    T = s.shape[0]
    pad = np.concatenate([np.zeros((P.BYTE_INTERLEAVE_BRANCHES - 1, P.RS_N), dtype=np.uint8), s], axis=0)
    j = np.arange(P.RS_N)
    branch = j % P.BYTE_INTERLEAVE_BRANCHES
    # pad の添字 = t + 11 - (11 - b) = t + b
    t = np.arange(T)[:, None] + branch[None, :]
    return pad[t, j[None, :]]


# ---------------------------------------------------------------------------
# 畳み込み符号とパンクチャ（STD-B31 3.8「内符号」）
# ---------------------------------------------------------------------------
def bytes_to_bits(data: np.ndarray) -> np.ndarray:
    """uint8 配列 → ビット列（各バイト MSB から）。"""
    return np.unpackbits(np.asarray(data, dtype=np.uint8).ravel())


def conv_encode(bits: np.ndarray, state: int = 0) -> tuple[np.ndarray, np.ndarray]:
    """K=7、G1=171(8)→X、G2=133(8)→Y の畳み込み符号化。初期状態は 0。

    X[n] = Σ_k g1[k] * u[n-k] (mod 2)。g1 の最上位ビットが現在入力 u[n] に対応する。
    """
    u = np.asarray(bits, dtype=np.uint8)
    n = len(u)
    # 直前 6 ビットの履歴を state から復元（state のビット 0 が直前の入力）
    hist = np.array([(state >> i) & 1 for i in range(P.CONV_K - 1)], dtype=np.uint8)[::-1]
    ext = np.concatenate([hist, u])
    x = np.zeros(n, dtype=np.uint8)
    y = np.zeros(n, dtype=np.uint8)
    for k in range(P.CONV_K):
        tap_bit = P.CONV_K - 1 - k   # k=0 が現在入力 = 多項式の最上位ビット
        seg = ext[P.CONV_K - 1 - k: P.CONV_K - 1 - k + n]
        if (P.CONV_G1_OCT >> tap_bit) & 1:
            x ^= seg
        if (P.CONV_G2_OCT >> tap_bit) & 1:
            y ^= seg
    return x, y


def puncture(x: np.ndarray, y: np.ndarray, rate: str) -> np.ndarray:
    """X, Y 列をパンクチャして直列ビット列にする。位置ごとに X, Y の順で送出。

    入力長はパンクチャ周期の倍数であること。
    """
    xm, ym = P.PUNCTURE_PATTERNS[rate]
    period = len(xm)
    n = len(x)
    assert n == len(y) and n % period == 0, (n, period)
    xm = np.array([c == "1" for c in xm])
    ym = np.array([c == "1" for c in ym])
    pair = np.stack([x.reshape(-1, period), y.reshape(-1, period)], axis=2)  # (blocks, period, 2)
    mask = np.stack([xm, ym], axis=1)                                       # (period, 2)
    return pair[:, mask].ravel().astype(np.uint8)


def conv_encode_punctured(bits: np.ndarray, rate: str) -> np.ndarray:
    x, y = conv_encode(bits)
    return puncture(x, y, rate)


# ---------------------------------------------------------------------------
# 復号側: デパンクチャと軟判定 Viterbi（STD-B31 3.8 の逆、図 3-4 のモデル受信機に相当）
# ---------------------------------------------------------------------------
def depuncture(soft: np.ndarray, rate: str) -> np.ndarray:
    """直列軟判定ビット列（正 = 0 らしい）→ (n_steps, 2) の (X, Y) 軟判定値。

    パンクチャで送られなかった位置は 0（情報なし）。入力は周期あたり送出ビット数の倍数に切り詰める。
    """
    xm, ym = P.PUNCTURE_PATTERNS[rate]
    period = len(xm)
    mask = np.array([[c == "1" for c in xm], [c == "1" for c in ym]]).T   # (period, 2)
    sent = int(mask.sum())
    n = len(soft) // sent
    out = np.zeros((n, period, 2), dtype=np.float32)
    out[:, mask] = np.asarray(soft[: n * sent], dtype=np.float32).reshape(n, sent)
    return out.reshape(n * period, 2)


def _viterbi_tables():
    """状態 = 直近 6 入力（最新が bit5）。新状態 s_new と前状態の最古ビット b から遷移を引く。"""
    ns = 1 << (P.CONV_K - 1)
    pred = np.zeros((ns, 2), dtype=np.int64)
    sx = np.zeros((ns, 2), dtype=np.float32)
    sy = np.zeros((ns, 2), dtype=np.float32)
    for s_new in range(ns):
        u = s_new >> 5
        for b in range(2):
            s_old = ((s_new & 0x1F) << 1) | b
            r = (u << 6) | s_old            # bit6 = 現在入力 … bit0 = 6 個前（conv_encode と同じ対応）
            pred[s_new, b] = s_old
            sx[s_new, b] = 1 - 2 * ((r & P.CONV_G1_OCT).bit_count() & 1)
            sy[s_new, b] = 1 - 2 * ((r & P.CONV_G2_OCT).bit_count() & 1)
    return pred, sx, sy


_VIT_PRED, _VIT_SX, _VIT_SY = _viterbi_tables()


def _viterbi_blocks(pairs: np.ndarray, block_len: int, warmup: int, tail: int) -> np.ndarray:
    """ブロック並列 Viterbi。各ブロックは前後に warmup / tail だけ余分に走らせて独立に復号する。"""
    T = len(pairs)
    nblk = -(-T // block_len)
    ext = warmup + block_len + tail
    padded = np.concatenate([np.zeros((warmup, 2), np.float32), pairs,
                             np.zeros((nblk * block_len - T + tail, 2), np.float32)])
    base = np.arange(nblk) * block_len
    pred, sx, sy = _VIT_PRED, _VIT_SX, _VIT_SY
    pm = np.zeros((nblk, 64), dtype=np.float32)
    dec = np.empty((ext, nblk, 8), dtype=np.uint8)
    for t in range(ext):
        s = padded[base + t]
        cand = pm[:, pred] + s[:, 0, None, None] * sx + s[:, 1, None, None] * sy
        choice = cand[:, :, 1] > cand[:, :, 0]
        pm = np.where(choice, cand[:, :, 1], cand[:, :, 0])
        pm -= pm.max(axis=1, keepdims=True)
        dec[t] = np.packbits(choice, axis=1)
    state = np.argmax(pm, axis=1)
    bi = np.arange(nblk)
    bits = np.empty((nblk, ext), dtype=np.uint8)
    for t in range(ext - 1, -1, -1):
        bits[:, t] = state >> 5
        byte = dec[t, bi, state >> 3]
        b = (byte >> (7 - (state & 7))) & 1
        state = pred[state, b]
    return bits[:, warmup:warmup + block_len].reshape(-1)[:T]


def viterbi_decode(pairs: np.ndarray, block_len: int = 8192, warmup: int = 96, tail: int = 64,
                   segment_len: int = 2_000_000) -> np.ndarray:
    """軟判定 Viterbi 復号（K=7、171/133）。pairs は (n, 2) の (X, Y) 軟判定値（正 = 0 らしい、0 = 消失）。

    返り値は復号ビット (n,)。符号器は状態 0 から始まるが、ブロック並列化のため先頭数ビットは
    ウォームアップ区間で決まる（先頭ブロックのみ不確か）。
    """
    pairs = np.asarray(pairs, dtype=np.float32)
    T = len(pairs)
    out = np.empty(T, dtype=np.uint8)
    for s0 in range(0, T, segment_len):
        s1 = min(T, s0 + segment_len)
        a, b = max(0, s0 - warmup), min(T, s1 + tail)
        bits = _viterbi_blocks(pairs[a:b], block_len, warmup, tail)
        out[s0:s1] = bits[s0 - a: s0 - a + (s1 - s0)]
    return out


# ---------------------------------------------------------------------------
# 復号側: RS(204,188) 復号（Berlekamp-Massey + Chien + Forney）
# ---------------------------------------------------------------------------
def _gmul(a: int, b: int) -> int:
    if a == 0 or b == 0:
        return 0
    return int(_GF_EXP[(_GF_LOG[a] + _GF_LOG[b]) % 255])


def _ginv(a: int) -> int:
    return int(_GF_EXP[(255 - _GF_LOG[a]) % 255])


def _gpow(a: int, k: int) -> int:
    if k == 0:
        return 1
    return int(_GF_EXP[(_GF_LOG[a] * k) % 255])


def _peval(poly: list, x: int) -> int:
    """poly[i] は x^i の係数。"""
    acc = 0
    for coef in reversed(poly):
        acc = _gmul(acc, x) ^ coef
    return acc


def _rs_decode_one(c: np.ndarray, S: list) -> int:
    """1 TSP を訂正（in place）。訂正バイト数を返す。訂正不能なら -1。"""
    t2 = P.RS_NROOTS
    # Berlekamp-Massey で誤り位置多項式 Λ(x) を求める
    C, B, L, m, b = [1], [1], 0, 1, 1
    for n in range(t2):
        d = S[n]
        for i in range(1, L + 1):
            d ^= _gmul(C[i], S[n - i])
        if d == 0:
            m += 1
            continue
        T = C[:]
        coef = _gmul(d, _ginv(b))
        need = len(B) + m
        if len(C) < need:
            C += [0] * (need - len(C))
        for i, bv in enumerate(B):
            C[i + m] ^= _gmul(coef, bv)
        if 2 * L <= n:
            L, B, b, m = n + 1 - L, T, d, 1
        else:
            m += 1
    if L > t2 // 2:
        return -1
    C = (C + [0] * (L + 1))[:L + 1]
    # Chien 探索: バイト位置 j の根は X_j = α^(203-j)、Λ(X_j^-1) = 0
    n_sym = len(c)
    pos = [j for j in range(n_sym) if _peval(C, _gpow(2, (255 - (n_sym - 1 - j)) % 255)) == 0]
    if len(pos) != L:
        return -1
    # Ω(x) = S(x) Λ(x) mod x^2t、Forney: e_j = X_j Ω(X_j^-1) / Λ'(X_j^-1)（fcr = 0）
    Om = [0] * t2
    for i, si in enumerate(S):
        for k, ck in enumerate(C):
            if i + k < t2:
                Om[i + k] ^= _gmul(si, ck)
    for j in pos:
        X = _gpow(2, n_sym - 1 - j)
        Xinv = _ginv(X)
        num = _gmul(X, _peval(Om, Xinv))
        den = 0
        for i in range(1, len(C), 2):
            den ^= _gmul(C[i], _gpow(Xinv, i - 1))
        if den == 0:
            return -1
        c[j] ^= _gmul(num, _ginv(den))
    return L


def rs_decode(tsps: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """(n, 204) → (訂正後 (n, 204), 各 TSP の訂正バイト数（訂正不能は -1）)。

    最大 8 バイト誤りを訂正する。シンドロームが全 0 の TSP は何もしない。
    """
    c = np.array(tsps, dtype=np.uint8)
    S = rs_syndromes(c)
    nerr = np.zeros(c.shape[0], dtype=np.int64)
    bad = np.flatnonzero(S.any(axis=1))
    for i in bad:
        row = c[i].astype(np.int64)
        nerr[i] = _rs_decode_one(row, [int(v) for v in S[i]])
        if nerr[i] >= 0:
            c[i] = row.astype(np.uint8)
    if len(bad):
        still = np.flatnonzero(rs_syndromes(c[bad]).any(axis=1))
        nerr[bad[still]] = -1
    return c, nerr


# ---------------------------------------------------------------------------
# 復号側: エネルギー逆拡散（3.5 の逆）
# ---------------------------------------------------------------------------
def energy_descramble_frame(onair: np.ndarray) -> np.ndarray:
    """伝送順 (n, 204) [情報 187, パリティ 16, 同期] → TSP (n, 204) [同期, 情報, パリティ]。フレーム先頭で初期化。"""
    onair = np.asarray(onair, dtype=np.uint8)
    n = onair.shape[0]
    prbs = energy_dispersal_prbs(n * P.RS_N).reshape(n, P.RS_N)
    out = np.empty_like(onair)
    out[:, 0] = onair[:, P.RS_N - 1]
    out[:, 1:] = onair[:, :P.RS_N - 1] ^ prbs[:, :P.RS_N - 1]
    return out


def energy_descramble_layer(onair: np.ndarray, tsp_per_frame: int) -> np.ndarray:
    onair = np.asarray(onair, dtype=np.uint8)
    assert onair.shape[0] % tsp_per_frame == 0
    n_frames = onair.shape[0] // tsp_per_frame
    return np.concatenate([energy_descramble_frame(onair[f * tsp_per_frame:(f + 1) * tsp_per_frame])
                           for f in range(n_frames)], axis=0)
