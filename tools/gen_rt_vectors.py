#!/usr/bin/env python3
"""rt/（C++ 実装）の検証用テストベクタを ref/ から生成する。

rt/build/.../rt_test.exe が読み込み、段ごとに ref/ と突き合わせる:
  - 単体ベクタ: PRBS、RS 復号、Viterbi、バイトデインターリーブ、FFT、分数リサンプル、
    デマップ、パイロット PRBS、TMCC フレーム
  - パイプラインベクタ: ループバック信号（SigMF）→ ref demodulate の TS・中間データ
生成物は testdata/rt_vectors/（gitignore 済み、約 100 MB）。

使い方: python tools/gen_rt_vectors.py [--out testdata/rt_vectors]
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ref"))

from isdbt import deinterleave as DI
from isdbt import fec, pilots, sync, tmcc
from isdbt import params as P
from isdbt.channel import ChannelConfig, apply_channel
from isdbt.demod import demodulate, front_end
from isdbt.io import write_sigmf
from isdbt.modulator import modulate

FS = float(P.SAMPLE_RATE_HZ)


def make_test_ts(n_packets: int, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    pk = rng.integers(0, 256, size=(n_packets, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    pk[:, 1] = 0x01
    pk[:, 2] = 0x00
    pk[:, 3] = 0x10 | (np.arange(n_packets) % 16)
    return pk


def unit_vectors(out: Path, expected: dict) -> None:
    rng = np.random.default_rng(42)

    # エネルギー拡散 PRBS
    np.save(out / "prbs.npy", fec.energy_dispersal_prbs(1024))

    # RS 復号: 行 i に i % 11 バイトの誤りを入れる（9, 10 誤りは訂正不能のはず）
    pk = rng.integers(0, 256, size=(64, 188), dtype=np.uint8)
    code = fec.rs_encode(pk)
    corrupted = code.copy()
    for i in range(len(corrupted)):
        n_err = i % 11
        pos = rng.choice(204, size=n_err, replace=False)
        corrupted[i, pos] ^= rng.integers(1, 256, size=n_err).astype(np.uint8)
    fixed, nerr = fec.rs_decode(corrupted)
    np.save(out / "rs_in.npy", corrupted)
    np.save(out / "rs_expected.npy", fixed)
    np.save(out / "rs_nerr.npy", nerr)

    # Viterbi: 既知の情報ビット → 3/4 パンクチャ → 軟判定（雑音あり）。復号結果は情報ビットに一致する
    info = rng.integers(0, 2, size=96_000, dtype=np.uint8)
    coded = fec.conv_encode_punctured(info, "3/4")
    soft = (1.0 - 2.0 * coded.astype(np.float32)) + rng.normal(0, 0.3, len(coded)).astype(np.float32)
    np.save(out / "viterbi_soft.npy", soft)
    np.save(out / "viterbi_bits.npy", info)
    expected["viterbi_rate"] = "3/4"
    expected["viterbi_skip_head"] = 96
    expected["viterbi_skip_tail"] = 64

    # バイトデインターリーブ
    bd_in = rng.integers(0, 256, size=(40, 204), dtype=np.uint8)
    np.save(out / "byte_deint_in.npy", bd_in)
    np.save(out / "byte_deint_out.npy", fec.byte_deinterleave(bd_in))

    # FFT（8192 点）
    x = (rng.normal(size=8192) + 1j * rng.normal(size=8192)).astype(np.complex64)
    from scipy import fft as sfft
    np.save(out / "fft_in.npy", x)
    np.save(out / "fft_out.npy", sfft.fft(x).astype(np.complex64))

    # 分数リサンプル
    fr_in = (rng.normal(size=10_000) + 1j * rng.normal(size=10_000)).astype(np.complex64)
    ratio = 1.0000123
    fr_out = sync.resample_fractional(fr_in, ratio, 0, 9_000)
    np.save(out / "frac_in.npy", fr_in)
    np.save(out / "frac_out.npy", fr_out)
    expected["frac_ratio"] = ratio
    expected["frac_m0"] = 0
    expected["frac_m1"] = 9_000

    # 64QAM デマップ
    dm_in = ((rng.normal(size=1000) + 1j * rng.normal(size=1000)) * 1.2).astype(np.complex64)
    np.save(out / "demap64_in.npy", dm_in)
    np.save(out / "demap64_out.npy", DI.demap_soft(dm_in, P.Modulation.QAM64))

    # パイロット PRBS（w_k）
    np.save(out / "pilot_w_mode3.npy", pilots.pilot_prbs(P.Mode.MODE3))

    # TMCC フレームビット（同期語 w0、パリティ付き）
    info_t = tmcc.TmccInfo(partial_reception=True, layers={
        "A": tmcc.TmccLayer(P.Modulation.QPSK, "2/3", 4, 1),
        "B": tmcc.TmccLayer(P.Modulation.QAM64, "3/4", 2, 12),
    })
    bits = tmcc.build_frame_bits(P.Mode.MODE3, info_t, frame_index=0)
    np.save(out / "tmcc_frame.npy", bits)
    # キーは C++ 側の最小 JSON パーサ（キー検索）で読むため、フラットで一意にする
    expected["tmcc_partial"] = True
    expected["tmcc_A_mod"] = "QPSK"
    expected["tmcc_A_rate"] = "2/3"
    expected["tmcc_A_I"] = 4
    expected["tmcc_A_segments"] = 1
    expected["tmcc_B_mod"] = "64QAM"
    expected["tmcc_B_rate"] = "3/4"
    expected["tmcc_B_I"] = 2
    expected["tmcc_B_segments"] = 12


def pipeline_vectors(out: Path, expected: dict) -> None:
    cfg = {
        "mode": P.Mode.MODE3,
        "gi": "1/8",
        "layers": {
            "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 4},
            "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 2},
        },
    }
    ts_a = make_test_ts(64, seed=11)       # A は 64 TSP/フレーム → データ 1 フレームに収める
    ts_b = make_test_ts(2592, seed=12)
    np.save(out / "pipe_tx_A.npy", ts_a)
    np.save(out / "pipe_tx_B.npy", ts_b)
    iq = modulate({"A": ts_a, "B": ts_b}, cfg)
    # クロック偏差 0（分数リサンプラは単体ベクタで検証し、パイプラインは決定的に保つ）
    ch = ChannelConfig(sample_rate_hz=FS, cn_db=32.0, freq_offset_hz=1200.0, integer_delay=1234, seed=7)
    rx = apply_channel(iq, ch)
    write_sigmf(out / "pipe_iq", rx, FS, datatype="cf32_le")

    print("ref demodulate (full) ...", flush=True)
    t0 = time.time()
    res, dbg = demodulate(rx, FS, return_debug=True)
    print(f"  done in {time.time() - t0:.1f} s", flush=True)
    for name in ("A", "B"):
        np.save(out / f"pipe_ts_{name}.npy", res[name])
        np.save(out / f"pipe_rserr_{name}.npy", dbg["rs_errors"][name])
    expected["pipe_frame_start"] = int(dbg["frame_start"])
    expected["pipe_parity_ok"] = bool(dbg["tmcc_parity_ok"])
    expected["pipe_n_frames"] = int(dbg["n_frames"])
    expected["pipe_n_sym"] = int(dbg["sync"]["n_sym"])
    expected["pipe_clock_ppm"] = float(dbg["sync"]["clock_ppm"])
    expected["pipe_cfo_frac_hz"] = float(dbg["sync"]["cfo_frac_hz"])
    expected["pipe_cfo_int_bins"] = int(dbg["sync"]["cfo_int_bins"])
    expected["pipe_gi_start0"] = float(dbg["sync"]["gi_start0"])
    expected["pipe_abs_symbol0"] = int(dbg["sync"]["abs_symbol0"])
    for k, v in dbg["first_frame"].items():
        expected[f"pipe_first_frame_{k}"] = int(v)
    expected["pipe_skip_tsp"] = 11

    # 段ごとの中間データ（front_end は全長一括処理）
    print("ref front_end (stage dumps) ...", flush=True)
    fe = front_end(rx, FS, P.Mode.MODE3, "1/8")
    head = 256
    np.save(out / "pipe_Y_head.npy", fe["Y"][:head])
    np.save(out / "pipe_H_head.npy", fe["H"][:head])
    r0, r1 = 420, 548       # 時間デインターリーブの参照が揃った範囲（I=4: 380 シンボル + frame_start 以降）
    np.save(out / "pipe_Xt_rows.npy", fe["Xt"][r0:r1].reshape(r1 - r0, -1))
    np.save(out / "pipe_Wt_rows.npy", fe["Wt"][r0:r1].reshape(r1 - r0, -1))
    expected["pipe_stage_head"] = head
    expected["pipe_xt_r0"] = r0
    expected["pipe_xt_r1"] = r1

    # 7.0 MSPS で録音された前提の入力（リサンプル経路の end-to-end 検証）
    from fractions import Fraction

    from scipy.signal import resample_poly
    fs_rec = 7_000_000.0
    r = Fraction(fs_rec / FS).limit_denominator(10_000)
    iq7 = resample_poly(iq, r.numerator, r.denominator).astype(np.complex64)
    rx7 = apply_channel(iq7, ChannelConfig(sample_rate_hz=fs_rec, cn_db=32.0, freq_offset_hz=-900.0, seed=8))
    write_sigmf(out / "pipe7_iq", rx7, fs_rec, datatype="cf32_le")
    expected["pipe7_fs"] = fs_rec


def tracked_vectors(out: Path, expected: dict) -> None:
    """追従（TrackedCarrierSource / DemodulateTracked）用。

    ref との突き合わせではなく送信 TS との自己完結照合にする（部品の等価性は他のベクタで
    担保済み。チャンク境界の argmax など float 依存の微差で揺れる比較を避ける）。
    """
    cfg = {
        "mode": P.Mode.MODE3,
        "gi": "1/8",
        "layers": {
            "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 4},
            "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 2},
        },
    }
    # クロックドリフト（±15 ppm 線形ランプ）+ CFO
    ts_a = make_test_ts(3 * 64, seed=21)
    ts_b = make_test_ts(3 * 2592, seed=22)
    np.save(out / "track_tx_A.npy", ts_a)
    np.save(out / "track_tx_B.npy", ts_b)
    iq = modulate({"A": ts_a, "B": ts_b}, cfg)
    rx = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, freq_offset_hz=1000.0,
                                         clock_drift_ppm=(-15.0, 15.0), seed=25))
    write_sigmf(out / "track_iq", rx, FS, datatype="cf32_le")

    # 瞬断（0.3 s）を挟んだ再同期
    ts_b2 = make_test_ts(8 * 2592, seed=24)
    np.save(out / "drop_tx_B.npy", ts_b2)
    iq2 = modulate({"A": make_test_ts(8 * 64, seed=23), "B": ts_b2}, cfg)
    dur = len(iq2) / FS
    rx2 = apply_channel(iq2, ChannelConfig(sample_rate_hz=FS, cn_db=30.0, freq_offset_hz=-800.0,
                                           dropouts_s=[(dur * 0.35, 0.3)], seed=26))
    write_sigmf(out / "drop_iq", rx2, FS, datatype="cf32_le")
    expected["track_present"] = True


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="testdata/rt_vectors")
    args = ap.parse_args()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    expected: dict = {}
    unit_vectors(out, expected)
    pipeline_vectors(out, expected)
    tracked_vectors(out, expected)
    (out / "expected.json").write_text(json.dumps(expected, indent=2))
    print(f"wrote vectors to {out}")


if __name__ == "__main__":
    main()
