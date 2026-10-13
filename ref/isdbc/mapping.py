"""バイト → m タプル変換、2 MSB の差動符号化、64QAM コンスタレーション（J.83 C.6.1〜C.6.3）。

J.83 Annex C の本文は Annex A（= EN 300 429 第 8・9 節）を引く。要点:
- C.6.1 バイト → m タプル: シンボル Z の MSB はバイト V の MSB から順に取る（MSB ファースト詰め）。
  64QAM は 3 バイト → 4 シンボル。Z が Z+1 より先に送られる。
- C.6.2 各シンボルの 2 MSB (A_k, B_k) を差動符号化して (I_k, Q_k) にし、π/2 回転不変にする。
- C.6.3 / EN 300 429 表 1: 第 1 象限（I_k Q_k = 00）の 4 LSB の配置を、第 2 象限（10）は +π/2、
  第 3 象限（11）は +π、第 4 象限（01）は +3π/2 回転して得る。
  第 1 象限の配置は図 C.7（EN 300 429 図 7）から転記: docs/isdbc-spec-notes.md 3 章。
"""
from __future__ import annotations

import itertools

import numpy as np

from . import params as P

M_BITS = P.QAM64_BITS
_BIT_WEIGHTS = np.array([32, 16, 8, 4, 2, 1], dtype=np.uint8)


# ---------------------------------------------------------------------------
# C.6.1 バイト ↔ m タプル
# ---------------------------------------------------------------------------
def bytes_to_mtuples(data: np.ndarray) -> np.ndarray:
    """(3k,) バイト列 → (4k,) の 6 ビット語（値 0..63、上位ビットが A_k B_k b3 b2 b1 b0 の順）。"""
    data = np.asarray(data, dtype=np.uint8).ravel()
    assert len(data) % 3 == 0, len(data)
    bits = np.unpackbits(data).reshape(-1, M_BITS)
    return (bits * _BIT_WEIGHTS).sum(axis=1).astype(np.uint8)


def mtuples_to_bytes(words: np.ndarray) -> np.ndarray:
    """bytes_to_mtuples の逆。長さは 4 の倍数であること。"""
    words = np.asarray(words, dtype=np.uint8).ravel()
    assert len(words) % 4 == 0, len(words)
    bits = ((words[:, None] >> np.arange(M_BITS - 1, -1, -1)) & 1).astype(np.uint8)
    return np.packbits(bits.ravel())


# ---------------------------------------------------------------------------
# C.6.2 2 MSB の差動符号化
#   I_k = ¬(A_k⊕B_k)·(A_k⊕I_{k-1}) + (A_k⊕B_k)·(A_k⊕Q_{k-1})
#   Q_k = ¬(A_k⊕B_k)·(B_k⊕Q_{k-1}) + (A_k⊕B_k)·(B_k⊕I_{k-1})
# この式は象限ラベル 00 → 10 → 11 → 01 → 00 の巡回（= 図 C.7 の反時計回り順）で回転不変になる。
# 右辺の否定の位置は原本 PDF の抽出テキストでは判別できず、US6226333 の引用と「回転不変で
# あること（下の自己検査）」で確定した。docs/isdbc-spec-notes.md 3 章参照。
# ---------------------------------------------------------------------------
def diff_encode_step(a: int, b: int, i_prev: int, q_prev: int) -> tuple[int, int]:
    """仕様の論理式そのもの（1 シンボル分）。"""
    x = a ^ b
    nx = 1 - x
    i = (nx & (a ^ i_prev)) | (x & (a ^ q_prev))
    q = (nx & (b ^ q_prev)) | (x & (b ^ i_prev))
    return i, q


def _build_phase_tables():
    """差動符号を「象限位相の加算」として表にする。

    回転不変な差動符号は、象限ラベルを巡回順に p = 0..3 と番号付けると p_k = p_{k-1} + d(A,B) mod 4
    になる。巡回順は論理式から総当たりで求め（回転不変性の自己検査を兼ねる）、
    IQ ラベル ↔ 位相と、(A,B) ↔ 位相差の表を返す。
    """
    labels = [(0, 0), (0, 1), (1, 0), (1, 1)]
    dec = {}
    for a, b, ip, qp in itertools.product((0, 1), repeat=4):
        dec[(ip, qp, *diff_encode_step(a, b, ip, qp))] = (a, b)
    assert len(dec) == 16, "差動符号が全単射でない"
    cycle = None
    for perm in itertools.permutations(labels):
        sigma = dict(zip(labels, perm))
        x, n = labels[0], 0
        while True:
            x = sigma[x]
            n += 1
            if x == labels[0]:
                break
        if n != 4:
            continue
        invariant = all(dec[(*sigma[(ip, qp)], *sigma[(i, q)])] == dec[(ip, qp, i, q)]
                        for (ip, qp, i, q) in dec)
        # 図 C.7 の反時計回り順 00 → 10 → 11 → 01 と向きを合わせる（逆向きでも不変性は同じ）
        if invariant and sigma[(0, 0)] == (1, 0):
            cycle = sigma
            break
    assert cycle is not None, "差動符号が π/2 回転不変でない（式の転記ミス）"
    phase_of_iq = np.zeros(4, dtype=np.int64)          # 添字 = I*2 + Q
    iq_of_phase = np.zeros(4, dtype=np.int64)
    x = (0, 0)
    for p in range(4):
        phase_of_iq[x[0] * 2 + x[1]] = p
        iq_of_phase[p] = x[0] * 2 + x[1]
        x = cycle[x]
    delta_of_ab = np.zeros(4, dtype=np.int64)          # 添字 = A*2 + B
    for a, b in itertools.product((0, 1), repeat=2):
        i, q = diff_encode_step(a, b, 0, 0)
        delta_of_ab[a * 2 + b] = (phase_of_iq[i * 2 + q] - phase_of_iq[0]) % 4
    ab_of_delta = np.zeros(4, dtype=np.int64)
    ab_of_delta[delta_of_ab] = np.arange(4)
    return phase_of_iq, iq_of_phase, delta_of_ab, ab_of_delta


PHASE_OF_IQ, IQ_OF_PHASE, DELTA_OF_AB, AB_OF_DELTA = _build_phase_tables()


def diff_encode(words: np.ndarray, state: tuple[int, int] = (0, 0)) -> np.ndarray:
    """6 ビット語列 (A B b3..b0) → 差動符号化後の語列 (I Q b3..b0)。state は (I_{-1}, Q_{-1})。

    位相の累積和で書いている（diff_encode_step の逐次適用と同じ結果。test_isdbc_mapping で照合）。
    """
    words = np.asarray(words, dtype=np.uint8)
    ab = (words >> 4) & 3
    p0 = PHASE_OF_IQ[state[0] * 2 + state[1]]
    phase = (p0 + np.cumsum(DELTA_OF_AB[ab])) % 4
    iq = IQ_OF_PHASE[phase].astype(np.uint8)
    return ((iq << 4) | (words & 0x0F)).astype(np.uint8)


def diff_decode(enc_words: np.ndarray, state: tuple[int, int] = (0, 0)) -> np.ndarray:
    """diff_encode の逆。先頭シンボルは state（既定 (0,0)）との差分として復号する。"""
    enc_words = np.asarray(enc_words, dtype=np.uint8)
    iq = (enc_words >> 4) & 3
    phase = PHASE_OF_IQ[iq]
    prev = np.concatenate([[PHASE_OF_IQ[state[0] * 2 + state[1]]], phase[:-1]])
    ab = AB_OF_DELTA[(phase - prev) % 4].astype(np.uint8)
    return ((ab << 4) | (enc_words & 0x0F)).astype(np.uint8)


# ---------------------------------------------------------------------------
# C.6.3 コンスタレーション
# 第 1 象限（I_k Q_k = 00）の 4 LSB (b3 b2 b1 b0): I 軸のレベル 1,3,5,7 は (b2 b0) の Gray 符号
# 00,01,11,10、Q 軸のレベル 1,3,5,7 は (b3 b1) の Gray 符号 00,01,11,10（図 C.7 より転記）。
# ---------------------------------------------------------------------------
_GRAY_TO_LEVEL_INDEX = {0b00: 0, 0b01: 1, 0b11: 2, 0b10: 3}
_LEVEL_INDEX_TO_GRAY = np.array([0b00, 0b01, 0b11, 0b10], dtype=np.int64)
# 表 1: 象限 2 (10) = +π/2、象限 3 (11) = +π、象限 4 (01) = +3π/2。添字 = I*2 + Q
QUADRANT_ROTATION = np.array([1, -1j, 1j, -1], dtype=np.complex64)


def _quadrant1_point(lsb: int) -> complex:
    b3, b2, b1, b0 = (lsb >> 3) & 1, (lsb >> 2) & 1, (lsb >> 1) & 1, lsb & 1
    i = 2 * _GRAY_TO_LEVEL_INDEX[(b2 << 1) | b0] + 1
    q = 2 * _GRAY_TO_LEVEL_INDEX[(b3 << 1) | b1] + 1
    return complex(i, q)


# 差動符号化後の語 (I Q b3 b2 b1 b0) → 複素点（レベル ±1..±7、正規化なし）
CONSTELLATION = np.array(
    [QUADRANT_ROTATION[w >> 4] * _quadrant1_point(w & 0x0F) for w in range(64)], dtype=np.complex64)


def map_symbols(enc_words: np.ndarray) -> np.ndarray:
    return CONSTELLATION[np.asarray(enc_words, dtype=np.int64)]


def demap_hard(z: np.ndarray) -> np.ndarray:
    """複素シンボル（レベル ±1..±7 スケール）→ 最近傍点の語 (I Q b3 b2 b1 b0)。硬判定。"""
    z = np.asarray(z)
    i_bit = (z.real < 0).astype(np.int64)          # 象限ラベル: 左半面で I=1
    q_bit = (z.imag < 0).astype(np.int64)          # 下半面で Q=1（00 右上, 10 左上, 11 左下, 01 右下）
    iq = i_bit * 2 + q_bit
    z1 = z * np.conj(QUADRANT_ROTATION[iq])        # 第 1 象限へ戻す
    li = np.clip(np.round((np.abs(z1.real) - 1) / 2), 0, 3).astype(np.int64)
    lq = np.clip(np.round((np.abs(z1.imag) - 1) / 2), 0, 3).astype(np.int64)
    gi = _LEVEL_INDEX_TO_GRAY[li]
    gq = _LEVEL_INDEX_TO_GRAY[lq]
    b2, b0 = gi >> 1, gi & 1
    b3, b1 = gq >> 1, gq & 1
    return ((iq << 4) | (b3 << 3) | (b2 << 2) | (b1 << 1) | b0).astype(np.uint8)


def mer_db(z: np.ndarray) -> float:
    """MER [dB]（最近傍点基準）。z はレベル ±1..±7 スケール。"""
    ref = CONSTELLATION[demap_hard(z)]
    err = float(np.mean(np.abs(z - ref) ** 2))
    return float(10 * np.log10(np.mean(np.abs(ref) ** 2) / max(err, 1e-30)))
