#!/usr/bin/env python3
"""Pluto 互換機から I/Q を録音して SigMF に保存する。

16bit(12bit) 経路は 2026-10-10 に実機（Pluto SDR nano, fw v0.39）で動作確認済み。
サンプル幅はファームの申告（scan-element の data_format）で自動判別する:
  純正ファーム        → int16（12bit）。既定 fs は公称の 7/8 = 7,111,111 Hz（USB 2.0 の帯域対策）
  pluto-fw-isdbt      → int8（8bit パッキング）。既定 fs は公称 8,126,984 Hz。ci8 で保存
使用例:
  python tools/iq_record.py --uri usb: --freq 557142857 --gain 40 --sec 30 --out testdata/captured/ch27
物理チャンネル中心周波数 = 473.142857 MHz + 6 MHz × (ch - 13)  （地デジ 13〜52ch、+1/7 MHz オフセット込み）

--bits 8t は tezuka_fw の complex 8bit モード用（hw/pluto/fw-8bit.md、未検証のまま残置）。
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


FS_NOMINAL = 8_126_984.0
FS_7_8 = 7_111_111.0
REG_PCORE_MAGIC = 0x80000000      # cf_axi_adc の debugfs_reg_access で AXI コアレジスタを指すマジック
REG_CNT_MAGIC = 0x3000            # pluto-fw-isdbt のサンプルカウンタ/情報ブロック（"ISDB"）
REG_CNT_FORMAT = 0x3018           # bit[7:0] = pack8 の右シフト量


def probe_sample_bits(ctx) -> int:
    """cf-ad9361-lpc の voltage0 の申告ビット幅（16 = 純正、8 = pluto-fw-isdbt）。"""
    dev = ctx.find_device("cf-ad9361-lpc")
    return int(dev.find_channel("voltage0", False).data_format.length)


def probe_pack_shift(ctx) -> int:
    """8bit 値のスケール: pluto-fw-isdbt の FORMAT レジスタ（無ければ素直な 12→8bit の 4）。"""
    dev = ctx.find_device("cf-ad9361-lpc")
    try:
        if dev.reg_read(REG_PCORE_MAGIC | REG_CNT_MAGIC) != 0x49534442:
            return 4
        return int(dev.reg_read(REG_PCORE_MAGIC | REG_CNT_FORMAT) & 0xFF)
    except OSError:
        return 4


def soft_gain_int8(sdr, shift: int, target_dbfs: float = -24.0, start_db: float = 30.0) -> float:
    """8bit ファーム用の 1 回限りのソフト AGC。AD936x の slow_attack は帯域内 −11 dBFS 程度に合わせるので、
    +6 dB 前置ゲイン付きの 8bit（FS = ADC −6 dBFS）では OFDM のピークが約 1% クリップする（実測）。
    BonDriver と同じく ADC 換算 −24 dBFS を目標に manual ゲインを決める。"""
    import iio
    sdr.gain_control_mode_chan0 = "manual"
    sdr.rx_hardwaregain_chan0 = start_db
    time.sleep(0.3)
    dev = sdr.ctx.find_device("cf-ad9361-lpc")
    mask = iio.ChannelsMask(dev)
    mask.channels = [dev.find_channel(name, False) for name in ("voltage0", "voltage1")]
    stream = iio.Stream(dev.get_buffer(0), mask, 1 << 18, nb_blocks=2)
    raw = None
    for i, block in enumerate(stream):
        raw = np.frombuffer(block.read(), dtype=np.int8).astype(np.float32)
        if i >= 1:        # 最初のブロックはゲイン変更前の信号を含みうるので 2 ブロック目を使う
            break
    del stream
    rms_adc = float(np.sqrt(np.mean(raw ** 2))) * (1 << shift) / 2048.0 * np.sqrt(2.0)   # I,Q 混在 → 複素振幅
    level = 20.0 * np.log10(rms_adc + 1e-9)
    gain = float(max(0.0, min(73.0, round(start_db + (target_dbfs - level)))))
    sdr.rx_hardwaregain_chan0 = gain
    time.sleep(0.3)
    print(f"soft AGC: level {level:.1f} dBFS at {start_db:.0f} dB -> gain {gain:.0f} dB (target {target_dbfs:.0f} dBFS)")
    return gain


def record_int8(sdr, fs: float, sec: float) -> np.ndarray:
    """pluto-fw-isdbt の 8bit パッキング（voltage0/1 とも int8、申告どおり）。libiio のバッファを直接読む。
    戻り値は int8 格子を /128 した complex64（ci8 保存で往復が無劣化）。ADC フルスケール換算は
    別途 pack shift（FORMAT レジスタ）で分かるが、復調には絶対スケールは不要。
    実機確認 2026-10-14（Pluto Nano、r3 ファーム）。"""
    import iio  # pylibiio 1.0（libiio v1 API: ChannelsMask + Stream）
    dev = sdr.ctx.find_device("cf-ad9361-lpc")
    mask = iio.ChannelsMask(dev)
    mask.channels = [dev.find_channel(name, False) for name in ("voltage0", "voltage1")]
    stream = iio.Stream(dev.get_buffer(0), mask, 1 << 20, nb_blocks=4)   # 1 ブロック = 複素 1M サンプル（2 MB）
    n_total = int(sec * fs)
    chunks = []
    got = 0
    scale = 1.0 / 128.0
    for block in stream:
        raw = np.frombuffer(block.read(), dtype=np.int8)
        x = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
        chunks.append((x * scale).astype(np.complex64))
        got += len(x)
        if got >= n_total:
            break
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
    ap.add_argument("--fs", type=float, default=0.0,
                    help="取り込みレート Hz。省略時はファームの申告で自動（16bit: 7111111 / 8bit: 8126984）")
    ap.add_argument("--bw", type=float, default=5_600_000.0)
    ap.add_argument("--gain", type=float, default=None, help="dB。省略で AGC slow_attack")
    ap.add_argument("--sec", type=float, default=10.0)
    ap.add_argument("--bits", choices=("auto", "12", "8t"), default="auto",
                    help="auto = 申告で判別（既定）。12 = 純正 16bit 経路を強制。8t = tezuka_fw の complex 8bit モード")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    freq = args.freq if args.freq else channel_to_hz(args.ch)

    # libiio1.dll は venv の Scripts/ に置いてある（docs/hw-setup.md 参照）。
    # pylibiio は PATH 検索で DLL を探すため、venv 非 activate 実行に備えて追加。
    os.environ["PATH"] = str(Path(sys.executable).parent) + os.pathsep + os.environ["PATH"]
    import adi  # pyadi-iio
    sdr = adi.Pluto(args.uri)
    sample_bits = 16 if args.bits == "12" else probe_sample_bits(sdr.ctx)
    if args.bits == "8t":
        mode = "8t"
    else:
        mode = "int8" if sample_bits == 8 else "int16"
    if args.fs <= 0:
        args.fs = FS_NOMINAL if mode == "int8" else FS_7_8
    print(f"firmware sample format: {mode} (voltage0 {sample_bits} bit) -> fs {args.fs:.0f} Hz")
    sdr.rx_lo = int(freq)
    sdr.sample_rate = int(args.fs)
    sdr.rx_rf_bandwidth = int(args.bw)
    pack_shift = probe_pack_shift(sdr.ctx) if mode == "int8" else 4
    if args.gain is None and mode == "int8":
        args.gain = soft_gain_int8(sdr, pack_shift)
    elif args.gain is None:
        sdr.gain_control_mode_chan0 = "slow_attack"
    else:
        sdr.gain_control_mode_chan0 = "manual"
        sdr.rx_hardwaregain_chan0 = args.gain

    t0 = time.time()
    if mode == "8t":
        x = record_8bit(sdr, args.fs, args.sec)
    elif mode == "int8":
        x = record_int8(sdr, args.fs, args.sec)
    else:
        x = record_16bit(sdr, args.fs, args.sec)
    print(f"captured {len(x)} samples in {time.time()-t0:.1f} s (wall) — サンプル落ちの有無は別途確認")

    # int8 経路は ±128 格子のまま ci8 で保存（往復無劣化）。ADC フルスケール換算の情報として
    # pack shift（4 = 素直な 12→8bit、3 = +6 dB 前置ゲイン）をメタに残す
    extra = {"isdbt:uri": args.uri, "isdbt:gain": args.gain, "isdbt:format": mode}
    if mode == "int8":
        extra["isdbt:pack_shift"] = pack_shift
    datatype = "ci8" if mode != "int16" else "ci16_le"
    write_sigmf(args.out, x, args.fs, freq, datatype=datatype, extra=extra)
    print(f"wrote {args.out}.sigmf-data / .sigmf-meta")


if __name__ == "__main__":
    main()
