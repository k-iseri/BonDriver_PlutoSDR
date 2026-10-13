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


def isdbc_vectors(out: Path, expected: dict) -> None:
    """ISDB-C（rt/isdbc）の検証ベクタ: 配置表・差動符号・フレーム処理・TSMF ヘッダ・ループバック I/Q。"""
    from isdbc import demod as cdemod
    from isdbc import framing as cframing
    from isdbc import mapping as cmapping
    from isdbc import modulator as cmod
    from isdbc import tsmf as ctsmf

    rng = np.random.default_rng(123)
    np.save(out / "isdbc_constellation.npy", cmapping.CONSTELLATION.astype(np.complex64))
    words = rng.integers(0, 64, size=4096, dtype=np.uint8)
    np.save(out / "isdbc_words.npy", words)
    np.save(out / "isdbc_diffenc.npy", cmapping.diff_encode(words))
    data = rng.integers(0, 256, size=3 * 1000, dtype=np.uint8)
    np.save(out / "isdbc_bytes.npy", data)
    np.save(out / "isdbc_mtuples.npy", cmapping.bytes_to_mtuples(data))

    # フレーム処理: 伝送バイト列（先頭に 57 バイトのゴミ、各 TSP の同じ 5 バイト位置に誤り）→ TS
    pk = cframing.null_packets(160)
    pk[:, 1] = 0x01
    pk[:, 2] = 0x00
    pk[:, 3] = 0x10 | (np.arange(160) % 16)
    pk[:, 4:] = rng.integers(0, 256, size=(160, 184), dtype=np.uint8)
    stream = cframing.encode_ts(pk).reshape(-1, 204)
    cols = rng.choice(np.arange(1, 204), size=5, replace=False)
    stream[:, cols] ^= rng.integers(1, 256, size=(stream.shape[0], 5)).astype(np.uint8)
    junk = rng.integers(0, 256, size=57, dtype=np.uint8)
    np.save(out / "isdbc_stream.npy", np.concatenate([junk, stream.ravel()]))
    np.save(out / "isdbc_packets.npy", pk)
    expected["isdbc_stream_junk"] = 57

    # TSMF ヘッダ
    slot = np.array([1] * 30 + [3] * 20 + [0] * 2)
    hdr = ctsmf.build_header(slot, {1: (16625, 4), 3: (16592, 4)}, continuity_counter=9,
                             sync_inverted=True, version=5, emergency=1)
    np.save(out / "isdbc_tsmf_header.npy", hdr)
    np.save(out / "isdbc_tsmf_slots.npy", slot)
    expected["isdbc_tsmf"] = {"cc": 9, "inverted": True, "version": 5, "emergency": 1,
                              "streams": [[1, 16625, 4], [3, 16592, 4]]}

    # ループバック: 2 TS を TSMF 多重 → 変調（7.111111 MSPS）→ CFO/クロック/雑音 → SigMF
    s1 = cframing.null_packets(2400)
    s1[:, 1] = 0x01
    s1[:, 2] = 0x01
    s1[:, 3] = 0x10 | (np.arange(2400) % 16)
    s1[:, 4:] = rng.integers(0, 256, size=(2400, 184), dtype=np.uint8)
    s2 = cframing.null_packets(600)
    s2[:, 1] = 0x01
    s2[:, 2] = 0x02
    s2[:, 3] = 0x10 | (np.arange(600) % 16)
    s2[:, 4:] = rng.integers(0, 256, size=(600, 184), dtype=np.uint8)
    mux = ctsmf.multiplex({1: s1, 2: s2}, {1: (16625, 4), 2: (16592, 4)})
    fs_c = 7_111_111.0
    iq = cmod.modulate(mux, fs_c)
    ch = ChannelConfig(sample_rate_hz=fs_c, cn_db=30.0, freq_offset_hz=-12_000.0, clock_ppm=16.0,
                       phase_rad=1.1, seed=5)
    rx = apply_channel(iq, ch)
    write_sigmf(out / "isdbc_iq", rx, fs_c, datatype="cf32_le")
    np.save(out / "isdbc_ts1.npy", s1)
    np.save(out / "isdbc_ts2.npy", s2)
    expected["isdbc_fs"] = fs_c
    expected["isdbc_cfo_hz"] = -12_000.0
    expected["isdbc_clock_ppm"] = 16.0
    res = cdemod.demodulate(rx, fs_c, select=2)
    expected["isdbc_ref_rs_uncorrectable"] = int(res["rs_uncorrectable"])
    expected["isdbc_ref_mer_db"] = float(res["mer_db"])


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
    isdbc_vectors(out, expected)
    (out / "expected.json").write_text(json.dumps(expected, indent=2))
    print(f"wrote vectors to {out}")


if __name__ == "__main__":
    main()
