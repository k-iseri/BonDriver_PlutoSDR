"""同期（受信機側の実装。STD-B31 には規定がない部分）。

I/Q（任意のサンプルレート）→ 公称 512/63 MHz へリサンプル → GI 相関（チャンクごとに畳み込み）で
シンボル位置とサンプルクロック偏差を直線フィット、GI 相関位相で小数 CFO → 固定パイロット電力で整数 CFO。
その後は CarrierSource が、必要なシンボル範囲だけを分数リサンプル → 周波数補正 → FFT して
有効キャリア行列を返す（長い録音でもメモリを使い切らない）。

ref/ なので速度より分かりやすさを優先する。
"""
from __future__ import annotations

from fractions import Fraction

import numpy as np
from scipy import fft as sfft
from scipy.signal import resample_poly

from . import params as P

FS = float(P.SAMPLE_RATE_HZ)


# ---------------------------------------------------------------------------
# リサンプル
# ---------------------------------------------------------------------------
def resample_to_nominal(x: np.ndarray, fs: float) -> np.ndarray:
    """任意レート fs の I/Q を公称レートへ有理数比でリサンプルする（7.0 MSPS → 512/441 など）。"""
    r = Fraction(FS / float(fs)).limit_denominator(10_000)
    x = np.asarray(x, dtype=np.complex64)
    if r == 1:
        return x
    return resample_poly(x, r.numerator, r.denominator).astype(np.complex64)


_FRAC_Q = 256       # 小数位相の分解能（1/256 サンプル）
_FRAC_TAPS = 48


def _frac_filters(fc: float = 0.45, beta: float = 8.0) -> np.ndarray:
    """窓付き sinc の多相補間フィルタ (Q, taps)。位相 q は q/Q サンプルの遅延。"""
    c = _FRAC_TAPS // 2 - 1
    mu = np.arange(_FRAC_Q) / _FRAC_Q
    i = np.arange(_FRAC_TAPS)
    t = i[None, :] - c - mu[:, None]
    w = np.interp(t, i - c, np.kaiser(_FRAC_TAPS, beta))
    h = 2 * fc * np.sinc(2 * fc * t) * w
    h /= h.sum(axis=1, keepdims=True)
    return h.astype(np.float32)


_FRAC_H = _frac_filters()


def resample_at(x: np.ndarray, t: np.ndarray, chunk: int = 250_000) -> np.ndarray:
    """入力 x を任意の実数位置 t[k]（サンプル単位）で多相 sinc 補間する。

    範囲外の入力位置は端の値で埋める。分解能は 1/256 サンプル。
    """
    x = np.asarray(x, dtype=np.complex64)
    t = np.asarray(t, dtype=np.float64)
    c = _FRAC_TAPS // 2 - 1
    y = np.empty(len(t), dtype=np.complex64)
    offs = np.arange(_FRAC_TAPS) - c
    for a in range(0, len(t), chunk):
        tt = t[a:a + chunk]
        n = np.floor(tt).astype(np.int64)
        q = np.round((tt - n) * _FRAC_Q).astype(np.int64)
        n += q // _FRAC_Q
        q %= _FRAC_Q
        idx = np.clip(n[:, None] + offs[None, :], 0, len(x) - 1)
        y[a:a + len(tt)] = (x[idx] * _FRAC_H[q]).sum(axis=1)
    return y


def resample_fractional(x: np.ndarray, ratio: float, m0: int = 0, m1: int | None = None,
                        chunk: int = 250_000) -> np.ndarray:
    """出力サンプル m（m0 ≤ m < m1）を入力位置 m*ratio に置く分数リサンプル（ppm 級のクロック偏差補正用）。

    範囲外の入力位置は端の値で埋める。
    """
    x = np.asarray(x, dtype=np.complex64)
    if m1 is None:
        m1 = int((len(x) - _FRAC_TAPS) / ratio)
    y = np.empty(max(0, m1 - m0), dtype=np.complex64)
    for a in range(m0, m1, chunk):
        b = min(m1, a + chunk)
        y[a - m0:b - m0] = resample_at(x, np.arange(a, b) * ratio, chunk)
    return y


# ---------------------------------------------------------------------------
# GI 相関によるシンボル同期・クロック偏差・小数 CFO
# ---------------------------------------------------------------------------
def gi_correlation(x: np.ndarray, N: int, Ng: int) -> np.ndarray:
    """s[n] = Σ_{i<Ng} conj(x[n+i]) x[n+i+N]。|s| のピークが GI の先頭、位相が CFO に対応する。"""
    c = np.conj(x[:-N]) * x[N:]
    cs = np.cumsum(c, dtype=np.complex128)
    cs = np.concatenate([[0.0], cs])
    return (cs[Ng:] - cs[:-Ng]).astype(np.complex64)


def gi_fold_chunks(x: np.ndarray, N: int, Ng: int, chunk_syms: int = 64,
                   return_power: bool = False):
    """信号を chunk_syms シンボルずつ区切り、GI 相関をシンボル周期 T で複素のまま畳んでピークを探す。

    返り値 (chunk の中心シンボル番号, ピーク位置 r ∈ [0, T), ピークの複素値)。メモリは chunk 分だけ使う。
    return_power=True なら各チャンクの平均電力も返す（ロック判定用）。
    """
    T = N + Ng
    n_sym = (len(x) - N - Ng) // T          # s[n] を n < chunk*T まで作るには末尾に N + Ng サンプル要る
    chunk = min(chunk_syms, n_sym)
    if chunk < 1:
        raise ValueError("信号が短すぎる")
    idx, pos, val, pwr = [], [], [], []
    for j0 in range(0, n_sym - chunk + 1, chunk):
        seg = x[j0 * T: (j0 + chunk) * T + N + Ng]
        s = gi_correlation(seg, N, Ng)[: chunk * T]
        fold = s.reshape(chunk, T).sum(axis=0)
        r = int(np.argmax(np.abs(fold)))
        idx.append(j0 + (chunk - 1) / 2)
        pos.append(r)
        val.append(fold[r])
        if return_power:
            pwr.append(float(np.mean(np.abs(seg[: chunk * T]) ** 2)))
    out = (np.array(idx, dtype=np.float64), np.array(pos, dtype=np.float64), np.array(val))
    return out + (np.array(pwr),) if return_power else out


def fit_symbol_timing(idx: np.ndarray, pos: np.ndarray, T: int) -> tuple[float, float]:
    """チャンクごとのピーク位置を直線フィットする。シンボル i の GI 先頭 ≈ r0 + i*T*(1+delta)。"""
    pos = pos[0] + ((pos - pos[0] + T / 2) % T) - T / 2        # 周期境界をまたぐ場合のアンラップ
    if len(idx) >= 2:
        slope, r0 = np.polyfit(idx, pos, 1)
    else:
        slope, r0 = 0.0, pos[0]
    return float(r0), float(slope / T)


def cfo_fraction_from_peaks(val: np.ndarray, N: int) -> float:
    """畳んだ GI 相関ピークの位相から小数部の CFO [Hz]（|f| < キャリア間隔/2）を推定する。"""
    return float(np.angle(np.sum(val)) / (2 * np.pi) * FS / N)


def estimate_cfo_fraction(s: np.ndarray, gi_starts: np.ndarray, N: int) -> float:
    """（全長の GI 相関 s がある場合）ピーク位置の位相から小数 CFO を推定する。"""
    idx = np.round(gi_starts).astype(np.int64)
    idx = idx[(idx >= 0) & (idx < len(s))]
    return float(np.angle(np.sum(s[idx])) / (2 * np.pi) * FS / N)


def derotate(x: np.ndarray, f_hz: float, n0: int = 0, chunk: int = 1 << 20) -> np.ndarray:
    """x[k] * exp(-j 2π f (n0 + k) / FS)。n0 は絶対サンプル番号（ブロック処理で位相を連続にする）。"""
    if f_hz == 0.0:
        return x
    y = np.empty_like(x)
    for a in range(0, len(x), chunk):
        n = np.arange(n0 + a, n0 + min(len(x), a + chunk), dtype=np.float64)
        y[a:a + len(n)] = x[a:a + len(n)] * np.exp(-2j * np.pi * f_hz * n / FS).astype(np.complex64)
    return y


# ---------------------------------------------------------------------------
# FFT と整数 CFO
# ---------------------------------------------------------------------------
def fft_symbols(x: np.ndarray, starts: np.ndarray, N: int) -> np.ndarray:
    seg = x[starts[:, None] + np.arange(N)[None, :]]
    return sfft.fft(seg, axis=1, workers=-1)


def _zero_strongest_bins(X: np.ndarray, k: int = 5) -> np.ndarray:
    """最強 k ビンを除外する（整数 CFO 探索用）。

    LO リークの DC スパイクのような狭帯域妨害（1〜3 ビン）が argmax を乗っ取るのを防ぐ。
    閾値方式（中央値比）は録音先頭の低電力区間でパイロットまで除外してしまうので使わない。
    パイロットは Mode 3 で 165 本あり、たとえ上位 k 本がパイロットでも探索は壊れない。
    """
    out = X.copy()
    out[np.argsort(X)[-k:]] = 0.0
    return out


def integer_cfo(x: np.ndarray, starts: np.ndarray, mode: P.Mode, search: int = 80) -> int:
    """固定位置パイロット（TMCC/AC1/CP、振幅 4/3）の電力が最大になるビンずれを探す。"""
    mp = P.MODE_PARAMS[mode]
    N = mp.fft_size
    fixed = np.array(list(P.TMCC_POSITIONS_ABS[mode]) + list(P.AC1_POSITIONS_ABS[mode]) + [mp.total_carriers - 1])
    bins = P.carrier_to_fft_bin(mode, fixed)
    X = _zero_strongest_bins((np.abs(fft_symbols(x, starts, N)) ** 2).sum(axis=0))
    pw = np.array([X[(bins + sh) % N].sum() for sh in range(-search, search + 1)])
    return int(np.argmax(pw)) - search


def active_carriers(x: np.ndarray, starts: np.ndarray, mode: P.Mode, k_int: int, chunk: int = 64) -> np.ndarray:
    """シンボルごとに FFT し、整数 CFO 分ずらして有効キャリア (n_sym, total) を取り出す。

    整数 CFO k_int は FFT 内では純粋なビンずれだが、窓の開始位置に応じた位相 exp(j2π k_int start/N) が
    シンボルごとに付くので打ち消す（start は補正後タイムラインの絶対サンプル番号）。
    """
    mp = P.MODE_PARAMS[mode]
    N = mp.fft_size
    bins = (P.carrier_to_fft_bin(mode, np.arange(mp.total_carriers)) + k_int) % N
    Y = np.empty((len(starts), mp.total_carriers), dtype=np.complex64)
    for i0 in range(0, len(starts), chunk):
        st = starts[i0:i0 + chunk]
        X = fft_symbols(x, st, N)
        ph = np.exp(-2j * np.pi * k_int * st / N)
        Y[i0:i0 + len(st)] = X[:, bins] * ph[:, None]
    return Y


# ---------------------------------------------------------------------------
# 同期済み信号からシンボル範囲の有効キャリアをオンデマンドで作る
# ---------------------------------------------------------------------------
class CarrierSource:
    """同期（タイミング・クロック偏差・CFO）を一度だけ推定し、symbols(i0, i1) で有効キャリア行列を返す。

    補正後タイムライン（クロック偏差を除いたサンプル番号 m）上でシンボル i の FFT 窓は
    starts[i] = round(r0') + i*T + Ng - margin。残留ドリフト（フィット雑音 ~0.1 ppm）は追従せず
    等化器の位相傾斜推定に吸収させる（窓を丸めて追従すると 1 サンプル飛びで等化が崩れるため）。
    """

    def __init__(self, iq: np.ndarray, fs: float, mode: P.Mode = P.JP_DEFAULT_MODE, gi: str = P.JP_DEFAULT_GI,
                 margin: int | None = None, cfo_search: int = 80):
        mp = P.MODE_PARAMS[mode]
        self.mode, self.gi = mode, gi
        self.N, self.Ng = mp.fft_size, P.guard_samples(mode, gi)
        self.T = self.N + self.Ng
        self.margin = self.Ng // 8 if margin is None else margin
        self.x = resample_to_nominal(iq, fs)
        # GI 相関（タイミング・CFO 推定）には DC を除いたコピーを使う（強い LO リークは相関を
        # 全ラグに持ち上げる）。復調本体の x から平均は引かない: DC ビン（帯域中央のキャリア）は
        # 4 シンボルごとに SP が乗るため信号自体がコヒーレントな DC を持ち、引くと等化が壊れる。
        # LO リーク自体は中央付近の数キャリアを汚すだけで、FEC（Viterbi + RS）が吸収する
        x_sync = self.x - np.mean(self.x)
        idx, pos, val = gi_fold_chunks(x_sync, self.N, self.Ng)
        del x_sync
        r0, delta = fit_symbol_timing(idx, pos, self.T)
        self.delta = delta
        self.ratio = 1.0 + delta if abs(delta) > 2e-7 else 1.0     # 補正後 m ↔ 入力位置 m*ratio
        self.f_frac = cfo_fraction_from_peaks(val, self.N)
        # 補正後タイムラインでのシンボル 0 の GI 先頭（FFT 窓が収まる最初のシンボルに正規化）
        r0c = r0 / self.ratio
        r0c = ((r0c + self.Ng - self.margin) % self.T) - (self.Ng - self.margin)
        n_corrected = int((len(self.x) - _FRAC_TAPS) / self.ratio) if self.ratio != 1.0 else len(self.x)
        self.n_sym = max(0, int((n_corrected - round(r0c) - self.Ng + self.margin - self.N) // self.T) + 1)
        self.starts = round(r0c) + np.arange(self.n_sym) * self.T + self.Ng - self.margin
        self.abs_symbol0 = int(np.round((self.starts[0] - (self.Ng - self.margin)) / self.T)) if self.n_sym else 0
        self.k_int = 0
        if self.n_sym:
            n_use = min(8, self.n_sym)
            xs, base = self._samples(self.starts[0], self.starts[n_use - 1] + self.N)
            self.k_int = integer_cfo(xs, self.starts[:n_use] - base, mode, cfo_search)

    def _samples(self, m0: int, m1: int) -> tuple[np.ndarray, int]:
        """補正後タイムラインの [m0, m1) を分数リサンプル + 周波数補正して返す。"""
        if self.ratio != 1.0:
            xs = resample_fractional(self.x, self.ratio, m0, m1)
        else:
            xs = self.x[m0:m1]
        return derotate(xs, self.f_frac, n0=m0), m0

    def symbols(self, i0: int, i1: int) -> np.ndarray:
        """シンボル i0..i1-1 の有効キャリア行列 (i1-i0, total_carriers)。"""
        i0, i1 = max(0, i0), min(i1, self.n_sym)
        if i1 <= i0:
            return np.zeros((0, P.MODE_PARAMS[self.mode].total_carriers), dtype=np.complex64)
        xs, base = self._samples(self.starts[i0], self.starts[i1 - 1] + self.N)
        Y = active_carriers(xs, self.starts[i0:i1] - base, self.mode, self.k_int)
        if self.k_int:
            # active_carriers は窓位置 (starts - base) で位相を補正しているので、base 分を追加で補正
            Y *= np.exp(-2j * np.pi * self.k_int * base / self.N).astype(np.complex64)
        return Y

    def info(self) -> dict:
        return {"clock_ppm": self.delta * 1e6, "clock_residual_ppm": 0.0 if self.ratio != 1.0 else self.delta * 1e6,
                "gi_start0": float(self.starts[0] - (self.Ng - self.margin)) if self.n_sym else 0.0,
                "abs_symbol0": self.abs_symbol0, "cfo_hz": self.f_frac + self.k_int * FS / self.N,
                "cfo_frac_hz": self.f_frac, "cfo_int_bins": self.k_int, "n_sym": self.n_sym,
                "margin": self.margin, "starts": self.starts}


def synchronize(iq: np.ndarray, fs: float, mode: P.Mode = P.JP_DEFAULT_MODE, gi: str = P.JP_DEFAULT_GI,
                margin: int | None = None, cfo_search: int = 80) -> tuple[np.ndarray, dict]:
    """I/Q → 全シンボルの有効キャリア行列 Y (n_sym, total_carriers)（等化前）と同期情報。短い信号向け。"""
    src = CarrierSource(iq, fs, mode, gi, margin, cfo_search)
    return src.symbols(0, src.n_sym), src.info()


# ---------------------------------------------------------------------------
# 追従型（長時間録音・クロックドリフト・位相雑音・瞬断向け）
# ---------------------------------------------------------------------------
class TrackedCarrierSource:
    """タイミングと CFO を区分線形で追従する CarrierSource。

    CarrierSource が全長を 1 本の直線（固定クロック比・固定 CFO）で表すのに対し、こちらは
    gi_fold_chunks のチャンク（既定 64 シンボル）ごとのピークから
      - タイミング: ピーク位置をアンラップ → 前後 smooth チャンクで移動平均 →
        シンボル i の GI 先頭の入力位置 u(i) = i·T + p(i)（チャンク中心で区分線形補間）
      - 小数 CFO: ピーク位相 → 同じく移動平均 + 区分線形補間
      - ロック判定: |ピーク| / (チャンク電力 × Ng) が閾値未満の区間は unlocked
    を作る。FFT 窓はシンボルごとに任意実数位置から局所クロック比で多相 sinc 補間するので、
    ppm が時間変化しても窓内の標本化レート誤差が残らない。
    周波数補正は f(i) + k_int·FS/N をサンプル時刻（公称レート）基準で回す。シンボル間の
    位相の継ぎ目は等化器の共通位相補正（SP）が吸収する。

    symbols(i0, i1) の添字は録音全体のシンボル番号。segments() がロック区間 [i0, i1) を返し、
    復調側は区間ごとにフレーム同期からやり直す（瞬断からの再同期）。
    """

    def __init__(self, iq: np.ndarray, fs: float, mode: P.Mode = P.JP_DEFAULT_MODE, gi: str = P.JP_DEFAULT_GI,
                 margin: int | None = None, cfo_search: int = 80, chunk_syms: int = 64,
                 smooth_chunks: int = 5, lock_threshold: float = 0.5):
        # lock_threshold: クリーンで ~1.0、無信号で ~0。瞬断を部分的に含むチャンク（~0.7）を
        # 区間に入れないよう 0.5 にしている（入れると区間端の死んだシンボルが復号を壊す）
        mp = P.MODE_PARAMS[mode]
        self.mode, self.gi = mode, gi
        self.N, self.Ng = mp.fft_size, P.guard_samples(mode, gi)
        self.T = self.N + self.Ng
        self.margin = self.Ng // 8 if margin is None else margin
        self.cfo_search = cfo_search
        self.x = resample_to_nominal(iq, fs)
        # GI 相関にだけ DC 除去したコピーを使う（理由は CarrierSource と同じ）
        x_sync = self.x - np.mean(self.x)
        idx, pos, val, pwr = gi_fold_chunks(x_sync, self.N, self.Ng, chunk_syms, return_power=True)
        del x_sync
        self.chunk_idx = idx
        # ロック判定: 完全相関なら |fold| ≈ chunk·Ng·P（チャンク自身の電力で正規化）。
        # 信号レベルの変動（送信先頭の時間インターリーブ押し出し、AGC）に不変で、
        # 無信号（雑音のみ）ではピークが相関しないので ~0.02 に落ちる。
        # 瞬断を部分的に含むチャンクは比が保たれて「ロック」になるが、死んだシンボルは
        # 等化器のバースト検出（消失扱い）が吸収し、整数 CFO はロック指標が最大の
        # チャンクで推定するので問題にならない。
        chunk = min(chunk_syms, int((len(self.x) - self.N - self.Ng) // self.T))
        with np.errstate(divide="ignore", invalid="ignore"):
            self.lock_metric = np.abs(val) / np.maximum(chunk * self.Ng * pwr, 1e-30)
        self.locked = self.lock_metric > lock_threshold

        # 全録音でのシンボル総数（末尾は窓が収まる範囲まで）
        self.n_sym = max(0, int((len(self.x) - self.N - self.T) // self.T))
        self.chunk_syms = chunk

        # タイミングのアンラップと CFO（チャンクごとの生値）
        pos_u = pos.copy()
        for k in range(1, len(pos_u)):
            d = (pos_u[k] - pos_u[k - 1] + self.T / 2) % self.T - self.T / 2
            pos_u[k] = pos_u[k - 1] + d
        f = np.angle(val) / (2 * np.pi) * FS / self.N

        # アンカーの平滑化・補間は「ロック区間内で完結」させる。
        # 瞬断をまたぐと unwrap の整数 T ずれ（区間ごとに任意）が混ざるので、区間をまたぐ
        # 平滑化・橋渡し補間は一切しない。区間端は端のアンカー値でクランプする。
        runs = self._locked_runs()
        self._segments = []
        xs: list[float] = []
        ps: list[float] = []
        fs_: list[float] = []
        sx: list[float] = []
        sv: list[float] = []
        for k0, k1 in runs:                      # チャンク番号 [k0, k1)
            i0 = k0 * self.chunk_syms
            i1 = min(k1 * self.chunk_syms, self.n_sym)
            if i1 - i0 >= 2:
                self._segments.append((i0, i1))
            pv = _box_smooth(pos_u[k0:k1], smooth_chunks)
            fv = _box_smooth(f[k0:k1], smooth_chunks)
            cx = idx[k0:k1]
            xs += [float(i0), *cx.tolist(), float(i1)]
            ps += [float(pv[0]), *pv.tolist(), float(pv[-1])]
            fs_ += [float(fv[0]), *fv.tolist(), float(fv[-1])]
            # 局所クロック比（アンカー差分）。区間端は端の傾きでクランプ
            if k1 - k0 >= 2:
                d = np.diff(pv) / (np.diff(cx) * self.T)
                mid = (cx[1:] + cx[:-1]) / 2
                sx += [float(i0), *mid.tolist(), float(i1)]
                sv += [float(d[0]), *d.tolist(), float(d[-1])]
            else:
                sx += [float(i0), float(i1)]
                sv += [0.0, 0.0]
        self._interp_x = np.array(xs)
        self._interp_p = np.array(ps)
        self._interp_f = np.array(fs_)
        self._slope_x = np.array(sx)
        self._slope_v = np.array(sv)
        # 時変 CFO の累積位相 [rad]（アンカー時刻でのサンプル時刻による台形積分）。
        # exp(-2πj·f(i)·t) のように「時変 f × 絶対時刻」を使うと位相誤差の微分が f'·t で
        # 録音後半ほど増大し、見かけの CFO ずれ → ICI で等化が崩壊する（実放送 10 秒で確認）。
        # 正しくは φ(t) = 2π∫f dt。シンボル内は f(i) 一定の線形位相、窓頭の基準位相はこの積分値。
        tk = self._interp_x * self.T + self._interp_p
        df = (self._interp_f[1:] + self._interp_f[:-1]) / 2 * np.diff(tk)
        self._interp_phi = np.concatenate([[0.0], np.cumsum(df)]) * (2 * np.pi / FS)
        # 整数 CFO は区間ごとに推定してキャッシュ
        self._k_int: dict[int, int] = {}

    # --- 内部 ---
    def _locked_runs(self) -> list[tuple[int, int]]:
        """ロックしたチャンクの連続区間 [k0, k1)。"""
        runs = []
        k0 = None
        for k in range(len(self.locked)):
            if self.locked[k] and k0 is None:
                k0 = k
            if not self.locked[k] and k0 is not None:
                runs.append((k0, k))
                k0 = None
        if k0 is not None:
            runs.append((k0, len(self.locked)))
        return runs

    def _p_of(self, i: np.ndarray) -> np.ndarray:
        """シンボル番号 → GI 先頭オフセット p(i)（入力サンプル、i·T に足す）。区間内補間・端クランプ。"""
        return np.interp(i, self._interp_x, self._interp_p)

    def _f_of(self, i: np.ndarray) -> np.ndarray:
        return np.interp(i, self._interp_x, self._interp_f)

    def _delta_of(self, i: np.ndarray) -> np.ndarray:
        """局所クロック比 - 1（= dp/di / T）。区間内のアンカー差分から区分線形で引く。"""
        if len(self._slope_x) == 0:
            return np.zeros_like(np.asarray(i, dtype=np.float64))
        return np.interp(i, self._slope_x, self._slope_v)

    def segments(self) -> list[tuple[int, int]]:
        return list(self._segments)

    def k_int_for(self, i0: int) -> int:
        """区間先頭 i0 の整数 CFO（キャッシュ付き）。

        区間の端は瞬断の残り（死んだシンボル）を含みうるが、ロック区間の中央は
        構成上必ず生きているので、区間中央の 8 シンボルから推定する。
        """
        if i0 not in self._k_int:
            seg = next(((a, b) for a, b in self._segments if a <= i0 < b), (i0, self.n_sym))
            start = max(i0, (seg[0] + seg[1]) // 2 - 4)
            start = min(start, max(0, self.n_sym - 8))
            n_use = min(8, self.n_sym - start)
            Y, _ = self._fft_window(start, start + n_use, k_int=0, f_extra=0.0, raw_bins=True)
            mp = P.MODE_PARAMS[self.mode]
            fixed = np.array(list(P.TMCC_POSITIONS_ABS[self.mode]) + list(P.AC1_POSITIONS_ABS[self.mode])
                             + [mp.total_carriers - 1])
            bins = P.carrier_to_fft_bin(self.mode, fixed)
            Xp = _zero_strongest_bins((np.abs(Y) ** 2).sum(axis=0))   # 狭帯域妨害ビンを除外（integer_cfo と同じ）
            pw = np.array([Xp[(bins + sh) % self.N].sum() for sh in range(-self.cfo_search, self.cfo_search + 1)])
            self._k_int[i0] = int(np.argmax(pw)) - self.cfo_search
        return self._k_int[i0]

    def _fft_window(self, i0: int, i1: int, k_int: int, f_extra: float, raw_bins: bool = False):
        """シンボル i0..i1-1 を任意位置リサンプル + 周波数補正 + FFT する。"""
        mp = P.MODE_PARAMS[self.mode]
        n = i1 - i0
        i = np.arange(i0, i1, dtype=np.float64)
        w0 = i * self.T + self._p_of(i) + (self.Ng - self.margin)   # FFT 窓の開始（入力位置）
        r = 1.0 + self._delta_of(i)                                  # 局所クロック比
        k = np.arange(self.N, dtype=np.float64)
        t = w0[:, None] + k[None, :] * r[:, None]                    # (n, N) 入力位置
        xs = resample_at(self.x, t.reshape(-1)).reshape(n, self.N)
        # 周波数補正。整数 CFO + f_extra は定数なので f·t でよいが、時変の小数 CFO は
        # 累積位相 ∫f dt を基準に窓内だけ f(i) の線形位相を掛ける（__init__ の _interp_phi 参照）。
        f_const = k_int * FS / self.N + f_extra
        f_var = self._f_of(i)
        phi0 = np.interp(i, self._interp_x, self._interp_phi)
        ph = np.exp(-1j * (2 * np.pi / FS) * (f_const * t + f_var[:, None] * (t - w0[:, None]))
                    - 1j * phi0[:, None]).astype(np.complex64)
        X = sfft.fft(xs * ph, axis=1, workers=-1)
        if raw_bins:
            return X, None
        bins = P.carrier_to_fft_bin(self.mode, np.arange(mp.total_carriers))
        return X[:, bins].astype(np.complex64), None

    def symbols(self, i0: int, i1: int) -> np.ndarray:
        """シンボル i0..i1-1 の有効キャリア行列。i0 を含むロック区間の整数 CFO を使う。"""
        i0c, i1c = max(0, i0), min(i1, self.n_sym)
        if i1c <= i0c:
            return np.zeros((0, P.MODE_PARAMS[self.mode].total_carriers), dtype=np.complex64)
        seg0 = next((a for a, b in self._segments if a <= i0c < b), i0c)
        Y, _ = self._fft_window(i0c, i1c, self.k_int_for(seg0), 0.0)
        return Y

    def info(self) -> dict:
        li = self.chunk_idx[self.locked] if self.locked.any() else self.chunk_idx
        ppm = self._delta_of(li) * 1e6
        return {"n_sym": self.n_sym, "segments": self._segments,
                "locked_chunks": int(self.locked.sum()), "total_chunks": len(self.locked),
                "ppm_min": float(ppm.min()) if len(ppm) else 0.0,
                "ppm_max": float(ppm.max()) if len(ppm) else 0.0,
                "cfo_anchor_hz": self._f_of(li), "lock_metric": self.lock_metric,
                "margin": self.margin}


def _box_smooth(v: np.ndarray, width: int) -> np.ndarray:
    """移動平均（中央寄せ、幅 width、端は窓が縮む）。"""
    if len(v) == 0:
        return v
    kernel = np.ones(min(width, len(v)))
    num = np.convolve(v, kernel, mode="same")
    den = np.convolve(np.ones(len(v)), kernel, mode="same")
    return num / den
