"""ISDB-C 復調器（参照実装）: 複素ベースバンド I/Q → 64QAM シンボル → TS パケット。

ブロック処理（numpy）で、速度より段ごとの見通しを優先する。段:
 1. 2 サンプル/シンボル（2 sps）へ有理数比リサンプル
 2. 粗 CFO: スペクトルの −3 dB 端（RRC では ±Rs/2）の中点 → 周波数補正
 3. RRC 整合フィルタ（α = 0.13）
 4. シンボルタイミング: ブロックごとに 4 sps へ補間して Oerder–Meyr 推定 → 直線フィット
    （クロック偏差）→ 多相 sinc 補間でシンボル抽出（1 sps）
 5. 微 CFO: 4 乗法（z^4 のスペクトル線）
 6. 搬送波位相: ブロックごとの 4 乗法推定 → 判定指向 LS 等化器（T 間隔 FFE）→ 判定指向位相
 7. 硬判定デマップ → 2 MSB 差動復号 → ビット境界探索（204 バイト周期の同期バイト）→
    framing.decode_stream（デインターリーブ・RS・逆ランダマイズ）→ tsmf.demultiplex
スペクトル反転（I/Q 入れ替わり）は同期が取れなければ共役で再試行して検出する。
差動符号化のおかげで π/2 の整数倍の位相あいまいさは解決不要。
"""
from __future__ import annotations

from fractions import Fraction

import numpy as np
from isdbt import sync as _tsync
from scipy.signal import fftconvolve, resample_poly

from . import framing, mapping, tsmf
from . import params as P
from .modulator import rrc_taps

SPS = 2


# ---------------------------------------------------------------------------
# 1. リサンプル
# ---------------------------------------------------------------------------
def resample_to_2sps(iq: np.ndarray, fs: float, symbol_rate: float = P.SYMBOL_RATE_HZ) -> tuple[np.ndarray, float]:
    """任意レート → 約 2 sps。戻り値 (x, 実際のレート fs2)。fs2 は 2Rs と ppm 級でずれうる（タイミング推定が吸収）。"""
    r = Fraction(SPS * symbol_rate / float(fs)).limit_denominator(4000)
    x = np.asarray(iq, dtype=np.complex64)
    if r != 1:
        x = resample_poly(x, r.numerator, r.denominator).astype(np.complex64)
    return x, float(fs) * r.numerator / r.denominator


# ---------------------------------------------------------------------------
# 2. 粗 CFO（帯域端の中点）
# ---------------------------------------------------------------------------
def power_spectrum(x: np.ndarray, nfft: int = 4096, max_segments: int = 256) -> tuple[np.ndarray, np.ndarray]:
    """平均ピリオドグラム。戻り値 (P, f[cycles/sample])、fftshift 済み。"""
    n_seg = len(x) // nfft
    seg = x[: n_seg * nfft].reshape(n_seg, nfft)
    if n_seg > max_segments:
        seg = seg[np.linspace(0, n_seg - 1, max_segments).astype(int)]
    w = np.hanning(nfft).astype(np.float32)
    Pxx = np.mean(np.abs(np.fft.fft(seg * w, axis=1)) ** 2, axis=0)
    return np.fft.fftshift(Pxx), np.fft.fftshift(np.fft.fftfreq(nfft))


def coarse_cfo(x: np.ndarray, fs2: float, symbol_rate: float = P.SYMBOL_RATE_HZ) -> tuple[float, float]:
    """スペクトルの平坦部から外へ向かって初めて −3 dB を切る周波数（RRC では ±Rs/2）の中点。

    隣接チャンネルは自チャンネルの端より外にあるので、内側から探せば影響しない。
    戻り値 (中心周波数 Hz, 推定帯域幅 Hz ≈ Rs)。
    """
    Pxx, f = power_spectrum(x)
    f_hz = f * fs2
    k = max(1, int(200e3 / (fs2 / len(f))))                   # 200 kHz 幅の移動平均
    Ps = np.convolve(Pxx, np.ones(k) / k, mode="same")
    PdB = 10 * np.log10(Ps + 1e-30)
    mid = np.abs(f_hz) < 0.25 * symbol_rate
    thr = float(np.median(PdB[mid])) - 3.0
    c = int(np.argmin(np.abs(f_hz)))

    def edge(step: int) -> float:
        i = c
        while 0 < i < len(f_hz) - 1 and PdB[i] > thr:
            i += step
        j = i - step
        if PdB[j] == PdB[i]:
            return float(f_hz[i])
        t = (PdB[j] - thr) / (PdB[j] - PdB[i])
        return float(f_hz[j] + t * (f_hz[i] - f_hz[j]))

    hi, lo = edge(+1), edge(-1)
    return 0.5 * (hi + lo), hi - lo


def derotate(x: np.ndarray, f_cycles_per_sample: float) -> np.ndarray:
    n = np.arange(len(x), dtype=np.float64)
    return (x * np.exp(-2j * np.pi * f_cycles_per_sample * n)).astype(np.complex64)


# ---------------------------------------------------------------------------
# 4. シンボルタイミング（Oerder–Meyr、ブロックごと → 直線フィット）
# ---------------------------------------------------------------------------
def timing_blocks(y: np.ndarray, block: int = 8192) -> tuple[np.ndarray, np.ndarray]:
    """2 sps の整合フィルタ出力からブロックごとのタイミング位相 τ_b [シンボル] を推定する。

    O&M: ε̂ = −(1/2π) arg Σ_n |y(nT/4)|² e^{−j2πn/4}。2 sps ではシンボルレート線がナイキストに重なり
    位相が取れないので、ブロックを 4 sps に補間してから求める。block は偶数（整数シンボル）であること。
    戻り値 (τ_b, ブロック中心 [2 sps サンプル])。
    """
    assert block % 2 == 0
    nb = len(y) // block
    tau = np.empty(nb)
    for b in range(nb):
        y4 = resample_poly(y[b * block:(b + 1) * block], 2, 1)
        n = np.arange(len(y4))
        tone = np.sum(np.abs(y4) ** 2 * np.exp(-2j * np.pi * n / 4))
        tau[b] = -np.angle(tone) / (2 * np.pi)
    centers = (np.arange(nb) + 0.5) * block
    return tau, centers


def fit_timing(tau: np.ndarray, centers: np.ndarray) -> tuple[float, float]:
    """τ_b を 1 シンボル周期で unwrap して直線 τ(n) = slope·n + tau0 にフィット（n は 2 sps サンプル）。"""
    tu = np.unwrap(tau, period=1.0)
    slope, tau0 = np.polyfit(centers, tu, 1)
    return float(slope), float(tau0)


def extract_symbols(y: np.ndarray, slope: float, tau0: float, guard: int = 32) -> tuple[np.ndarray, np.ndarray]:
    """シンボル k の最適標本点 n_k = 2(k + τ(n_k)) を多相 sinc 補間で取り出す。戻り値 (z, n_k)。"""
    n_sym = len(y) // SPS
    k = np.arange(n_sym, dtype=np.float64)
    n_nom = SPS * k
    t = n_nom + SPS * (slope * n_nom + tau0)
    t = t + SPS * slope * (t - n_nom)                         # τ は n_k で評価（1 回反復で十分）
    ok = (t >= guard) & (t <= len(y) - 1 - guard)
    t = t[ok]
    return _tsync.resample_at(y, t), t


# ---------------------------------------------------------------------------
# 5. 微 CFO（4 乗法）と 6. 位相・等化
# ---------------------------------------------------------------------------
def fine_cfo(z: np.ndarray, max_symbols: int = 1 << 20) -> float:
    """z^4 のスペクトル線の位置 / 4 [cycles/symbol]。放物線補間でビン以下まで詰める。"""
    n = min(len(z), max_symbols)
    z4 = (z[:n] ** 4) * np.hanning(n)
    Z = np.abs(np.fft.fft(z4))
    k = int(np.argmax(Z))
    km, kp = (k - 1) % n, (k + 1) % n
    a, b, c = Z[km], Z[k], Z[kp]
    delta = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    f4 = (k + delta) / n
    if f4 > 0.5:
        f4 -= 1.0
    return float(f4 / 4)


def phase_blocks_4th(z: np.ndarray, block: int = 1024) -> tuple[np.ndarray, np.ndarray]:
    """ブロックごとの搬送波位相（4 乗法）。64QAM は E[z^4] < 0 なので −z^4 の角度 / 4。π/2 周期で unwrap。"""
    nb = len(z) // block
    zz = z[: nb * block].reshape(nb, block)
    th = np.angle(np.sum(-(zz ** 4), axis=1)) / 4
    th = np.unwrap(th, period=np.pi / 2)
    return th, (np.arange(nb) + 0.5) * block


def phase_blocks_dd(z: np.ndarray, block: int = 256) -> tuple[np.ndarray, np.ndarray]:
    """判定指向のブロック位相: arg Σ z·conj(d)。"""
    nb = len(z) // block
    zz = z[: nb * block].reshape(nb, block)
    d = mapping.CONSTELLATION[mapping.demap_hard(zz)]
    th = np.angle(np.sum(zz * np.conj(d), axis=1))
    th = np.unwrap(th)
    return th, (np.arange(nb) + 0.5) * block


def apply_phase(z: np.ndarray, th: np.ndarray, centers: np.ndarray) -> np.ndarray:
    theta = np.interp(np.arange(len(z)), centers, th)
    return (z * np.exp(-1j * theta)).astype(np.complex64)


def agc(z: np.ndarray) -> np.ndarray:
    return (z * np.sqrt(P.QAM64_RMS ** 2 / np.mean(np.abs(z) ** 2))).astype(np.complex64)


def dd_equalize(z: np.ndarray, n_taps: int = 25, train_symbols: int = 1 << 16,
                iters: int = 3) -> tuple[np.ndarray, np.ndarray]:
    """判定指向の最小二乗 FFE（T 間隔、中央タップ基準）。

    先頭 train_symbols シンボルの硬判定を教師にタップを解き、判定し直して iters 回繰り返す。
    ケーブル伝送路（短い反射）向けの簡易等化。戻り値 (等化後シンボル, タップ)。
    """
    L = n_taps // 2
    zp = np.concatenate([np.zeros(L, np.complex64), z, np.zeros(L, np.complex64)])
    n_train = min(train_symbols, len(z))
    X = np.lib.stride_tricks.sliding_window_view(zp[: n_train + 2 * L], n_taps)
    w = np.zeros(n_taps, dtype=np.complex128)
    w[L] = 1.0
    for _ in range(iters):
        y = X @ w
        d = mapping.CONSTELLATION[mapping.demap_hard(y)].astype(np.complex128)
        w, *_ = np.linalg.lstsq(X, d, rcond=None)
    y_all = np.lib.stride_tricks.sliding_window_view(zp, n_taps) @ w
    return y_all.astype(np.complex64), w


# ---------------------------------------------------------------------------
# 7. シンボル → バイト列（ビット境界探索）
# ---------------------------------------------------------------------------
def symbols_to_bytes(y: np.ndarray, probe_packets: int = 2000) -> dict:
    """硬判定 → 差動復号 → 6 ビット語をビット列にし、24 通りのビット境界のうち 204 バイト周期の
    同期バイトが最も揃うものを選ぶ。戻り値 {"bytes", "bit_offset", "sync_offset", "sync_ratio"}。"""
    enc = mapping.demap_hard(y)
    words = mapping.diff_decode(enc)
    bits = ((words[:, None] >> np.arange(mapping.M_BITS - 1, -1, -1)) & 1).astype(np.uint8).ravel()
    best = None
    probe = P.RS_N * probe_packets
    for b in range(4 * mapping.M_BITS):
        n_bytes = (len(bits) - b) // 8
        by = np.packbits(bits[b: b + n_bytes * 8])
        off, ratio = framing.sync_score(by[:probe])
        if best is None or ratio > best["sync_ratio"]:
            best = {"bytes": by, "bit_offset": b, "sync_offset": off, "sync_ratio": ratio}
    return best


# ---------------------------------------------------------------------------
# 全体
# ---------------------------------------------------------------------------
def demodulate_symbols(iq: np.ndarray, fs: float, *, symbol_rate: float = P.SYMBOL_RATE_HZ,
                       n_taps: int = 25) -> dict:
    """I/Q → 等化済みシンボル列（レベル ±1..±7 スケール）と同期情報。"""
    x, fs2 = resample_to_2sps(iq, fs, symbol_rate)
    cfo_hz, bw_hz = coarse_cfo(x, fs2, symbol_rate)
    x = derotate(x, cfo_hz / fs2)
    y = fftconvolve(x, rrc_taps(SPS), mode="same").astype(np.complex64)
    tau, centers = timing_blocks(y)
    slope, tau0 = fit_timing(tau, centers)
    z, _ = extract_symbols(y, slope, tau0)
    z = agc(z)
    f_fine = fine_cfo(z)
    z = derotate(z, f_fine)
    th4, c4 = phase_blocks_4th(z)
    z = apply_phase(z, th4, c4)
    y_eq, w = dd_equalize(z, n_taps=n_taps)
    thd, cd = phase_blocks_dd(y_eq)
    y_eq = apply_phase(y_eq, thd, cd)
    y_eq = agc(y_eq)
    # 直線フィットの傾きから受信シンボルレートの偏差（2 sps 公称レート基準）
    # 符号は channel.ChannelConfig.clock_ppm と同じ向き
    clock_ppm = 2.0 * slope * 1e6
    return {
        "symbols": y_eq,
        "cfo_hz": cfo_hz + f_fine * fs2 / SPS,
        "coarse_cfo_hz": cfo_hz,
        "bandwidth_hz": bw_hz,
        "clock_ppm": clock_ppm,
        "timing_tau": tau,
        "eq_taps": w,
        "mer_db": mapping.mer_db(y_eq[len(y_eq) // 8:]),      # 立ち上がりを除いた区間
        "fs2": fs2,
    }


def demodulate(iq: np.ndarray, fs: float, *, select="auto", symbol_rate: float = P.SYMBOL_RATE_HZ,
               allow_inversion: bool = True, n_taps: int = 25) -> dict:
    """I/Q → TS パケット。

    戻り値: {"packets": (n,188) 選択した TS（TEI 付き）, "tsmf": TsmfInfo, "rs_err", "rs_uncorrectable",
            "mer_db", "cfo_hz", "clock_ppm", "sync_ratio", "inverted", "all_packets"}
    """
    sym = demodulate_symbols(iq, fs, symbol_rate=symbol_rate, n_taps=n_taps)
    s2b = symbols_to_bytes(sym["symbols"])
    inverted = False
    if s2b["sync_ratio"] < 0.5 and allow_inversion:
        sym_i = demodulate_symbols(np.conj(np.asarray(iq)), fs, symbol_rate=symbol_rate, n_taps=n_taps)
        s2b_i = symbols_to_bytes(sym_i["symbols"])
        if s2b_i["sync_ratio"] > s2b["sync_ratio"]:
            sym, s2b, inverted = sym_i, s2b_i, True
    frame = framing.decode_stream(s2b["bytes"], s2b["sync_offset"])
    packets, info = tsmf.demultiplex(frame["packets"], select)
    return {
        "packets": packets,
        "all_packets": frame["packets"],
        "tsmf": info,
        "rs_err": frame["rs_err"],
        "rs_uncorrectable": int(np.count_nonzero(frame["rs_err"] < 0)),
        "mer_db": sym["mer_db"],
        "cfo_hz": sym["cfo_hz"],
        "clock_ppm": sym["clock_ppm"],
        "sync_ratio": s2b["sync_ratio"],
        "bit_offset": s2b["bit_offset"],
        "inverted": inverted,
        "eq_taps": sym["eq_taps"],
    }
