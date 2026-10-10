"""UHF 帯 ISDB-T チャンネルスキャナ（Pluto SDR 用）。

各物理チャンネル（13〜52ch）に同調して短時間キャプチャし、
  - 受信電力（固定ゲインなのでチャンネル間で比較可能）
  - GI 相関（Mode 3: FFT 8192、GI 1/8 の巡回プレフィックスによる
    lag=8192 の自己相関。ISDB-T 信号なら理論上 ~1/9 に近づく）
を表示する。GI 相関が明確に立っていれば ISDB-T 信号と判断できる。

使い方:
    .venv\\Scripts\\python hw\\scripts\\scan_channels.py [--uri usb:] [--gain 55]
        [--channels 13-52]

実機検証: 2026-10-10 Pluto SDR nano (Z7010-AD9363A, fw v0.39) で動作確認。
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

# libiio1.dll は venv の Scripts/ に置いてある（docs/hw-setup.md 参照）。
# pylibiio は ctypes.util.find_library（PATH 検索）で DLL を探すため、
# venv を activate せずに実行された場合に備えて PATH に追加する。
os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]

import numpy as np

# 物理チャンネル -> 中心周波数 [Hz]。
# 日本の UHF: 13ch = 473 + 1/7 MHz、以降 6 MHz 間隔（+1/7 MHz は全チャンネル共通オフセット）
def channel_freq_hz(ch: int) -> int:
    return 473_142_857 + 6_000_000 * (ch - 13)


# Mode 3 の有効シンボル長（サンプル数）。サンプルレートが 512/63 MHz のとき 8192。
NU = 8192
GI = NU // 8  # GI 1/8


def gi_correlation(x: np.ndarray) -> float:
    """lag=NU の正規化自己相関。ISDB-T (Mode 3, GI 1/8) なら GI 部分が相関し
    クリーンな信号で ~GI/(NU+GI) = 1/9 ≈ 0.111 に近づく。雑音のみなら ~0。"""
    a = x[:-NU]
    b = x[NU:]
    num = np.abs(np.vdot(a, b))
    den = np.sqrt(np.vdot(a, a).real * np.vdot(b, b).real) + 1e-30
    return float(num / den)


def scan(uri: str, channels: range, gain_db: float, nsamp: int) -> None:
    import adi

    sdr = adi.Pluto(uri)
    sdr.sample_rate = 8_126_984  # 512/63 MHz に最も近い整数 Hz
    sdr.rx_rf_bandwidth = 6_000_000
    sdr.gain_control_mode_chan0 = "manual"
    sdr.rx_hardwaregain_chan0 = gain_db
    sdr.rx_buffer_size = nsamp

    print(f"uri={uri} fs={sdr.sample_rate} Hz gain={gain_db} dB nsamp={nsamp}")
    print(f"{'ch':>3} {'freq [MHz]':>10} {'power [dB]':>10} {'GI corr':>8} {'max|x|':>7}  verdict")

    results = []
    for ch in channels:
        sdr.rx_lo = channel_freq_hz(ch)
        sdr.rx_destroy_buffer()  # 同調前の古いサンプルを捨てる
        x = sdr.rx()
        x = np.asarray(x, dtype=np.complex64)
        power_db = 10 * np.log10(np.mean(np.abs(x) ** 2) + 1e-12)
        corr = gi_correlation(x)
        peak = int(np.max(np.abs(x.real)) if len(x) else 0)
        verdict = "ISDB-T?" if corr > 0.05 else ""
        if peak > 1800:
            verdict += " [飽和気味: --gain を下げる]"
        print(
            f"{ch:>3} {channel_freq_hz(ch)/1e6:>10.3f} {power_db:>10.1f} "
            f"{corr:>8.3f} {peak:>7}  {verdict}"
        )
        results.append((ch, power_db, corr))

    hits = [r for r in results if r[2] > 0.05]
    print()
    if hits:
        best = max(hits, key=lambda r: r[2])
        print(f"ISDB-T らしき信号: {len(hits)} チャンネル。最良: 物理 {best[0]}ch "
              f"(GI corr {best[2]:.3f}, {best[1]:.1f} dB)")
    else:
        print("GI 相関の立つチャンネルなし（アンテナ・場所・ゲインを確認）")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--uri", default="usb:", help="libiio URI (usb: / ip:192.168.2.1)")
    ap.add_argument("--gain", type=float, default=55.0, help="RX 手動ゲイン [dB]")
    ap.add_argument("--channels", default="13-52", help="物理チャンネル範囲 (例: 13-52)")
    ap.add_argument("--nsamp", type=int, default=131072, help="チャンネルあたりサンプル数")
    args = ap.parse_args()

    lo, hi = (int(s) for s in args.channels.split("-"))
    scan(args.uri, range(lo, hi + 1), args.gain, args.nsamp)


if __name__ == "__main__":
    main()
