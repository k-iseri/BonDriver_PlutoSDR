#!/usr/bin/env python3
"""Pluto 互換機から I/Q を録音して SigMF に保存する。

16bit(12bit) 経路は 2026-10-10 に実機（Pluto SDR nano, fw v0.39）で動作確認済み。
--bits 8 経路は未検証のまま（docs/hw-setup.md の「未検証」節を参照。pylibiio 1.0 で要改修）。
使用例:
  python tools/iq_record.py --uri usb: --freq 557142857 --fs 8126984 --gain 40 --sec 30 --out testdata/captured/ch27
物理チャンネル中心周波数 = 473.142857 MHz + 6 MHz × (ch - 13)  （地デジ 13〜52ch、+1/7 MHz オフセット込み）

--bits 8 は tezuka_fw の complex 8bit モード用（hw/pluto/fw-8bit.md）。純正ファームでは動かない。
USB 2.0 で 8.127 MSPS を通すにはこれが必要（16bit だと 32.5 MB/s で実用上限を超える）。
"""
import argparse
import os
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ref"))
sys.path.insert(0, str(ROOT / "hw" / "pluto"))
from isdbt.io import write_sigmf
from pack8 import unpack8


def channel_to_hz(ch: int) -> float:
    return 473_142_857.0 + 6_000_000.0 * (ch - 13)


def record_16bit(sdr, fs: float, sec: float) -> np.ndarray:
    """純正ファームの経路。pyadi の rx() で complex を受ける。"""
    sdr.rx_buffer_size = 1 << 20
    n_total = int(sec * fs)
    chunks = []
    got = 0
    while got < n_total:
        buf = sdr.rx()
        chunks.append(np.asarray(buf, dtype=np.complex64) / 2048.0)
        got += len(buf)
    return np.concatenate(chunks)[:n_total]


def record_8bit(sdr, fs: float, sec: float) -> np.ndarray:
    """tezuka_fw の complex 8bit モード。

    rx0_q（voltage1）を無効化すると FPGA が voltage0 の 16bit 枠に (i8, q8) を詰める。
    pyadi の rx() は I/Q 2 チャンネル前提なので、libiio のバッファを直接読む。
    未検証 / # VERIFY: モードの入り方（voltage1 無効化だけで切り替わるか）とバイト順は
    実機確認が要る。hw/pluto/pack8.py の VERIFY を参照。
    """
    import iio
    dev = sdr.ctx.find_device("cf-ad9361-lpc")
    v0 = dev.find_channel("voltage0", False)
    v1 = dev.find_channel("voltage1", False)
    v1.enabled = False
    v0.enabled = True
    buf = iio.Buffer(dev, 1 << 20, False)  # 1 エントリ = 16bit ワード = 複素 1 サンプル
    n_total = int(sec * fs)
    chunks = []
    got = 0
    while got < n_total:
        buf.refill()
        raw = np.frombuffer(buf.read(), dtype=np.int8)
        chunks.append(raw)
        got += len(raw) // 2
    return unpack8(np.concatenate(chunks))[:n_total]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", default="usb:")
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--freq", type=float)
    g.add_argument("--ch", type=int)
    ap.add_argument("--fs", type=float, default=8_126_984.0)
    ap.add_argument("--bw", type=float, default=5_600_000.0)
    ap.add_argument("--gain", type=float, default=None, help="dB。省略で AGC slow_attack")
    ap.add_argument("--sec", type=float, default=10.0)
    ap.add_argument("--bits", type=int, choices=(12, 8), default=12,
                    help="8 は tezuka_fw の complex 8bit モード（純正ファーム不可）")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    freq = args.freq if args.freq else channel_to_hz(args.ch)

    # libiio1.dll は venv の Scripts/ に置いてある（docs/hw-setup.md 参照）。
    # pylibiio は PATH 検索で DLL を探すため、venv 非 activate 実行に備えて追加。
    os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]
    import adi  # pyadi-iio
    sdr = adi.Pluto(args.uri)
    sdr.rx_lo = int(freq)
    sdr.sample_rate = int(args.fs)
    sdr.rx_rf_bandwidth = int(args.bw)
    if args.gain is None:
        sdr.gain_control_mode_chan0 = "slow_attack"
    else:
        sdr.gain_control_mode_chan0 = "manual"
        sdr.rx_hardwaregain_chan0 = args.gain

    t0 = time.time()
    if args.bits == 8:
        x = record_8bit(sdr, args.fs, args.sec)
    else:
        x = record_16bit(sdr, args.fs, args.sec)
    print(f"captured {len(x)} samples in {time.time()-t0:.1f} s (wall) — サンプル落ちの有無は別途確認")

    datatype = "ci8" if args.bits == 8 else "ci16_le"
    write_sigmf(args.out, x, args.fs, freq, datatype=datatype,
                extra={"isdbt:uri": args.uri, "isdbt:gain": args.gain, "isdbt:bits": args.bits})
    print(f"wrote {args.out}.sigmf-data / .sigmf-meta")


if __name__ == "__main__":
    main()
