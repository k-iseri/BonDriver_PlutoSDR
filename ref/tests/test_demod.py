"""復調器の単体・結合テスト（チャンネル劣化なし）。劣化ありの基準は test_loopback.py。"""
import numpy as np
import pytest
from isdbt import deinterleave as DI
from isdbt import demod, fec, sync, tmcc
from isdbt import params as P
from isdbt.modulator import TxConfig, modulate

FS = float(P.SAMPLE_RATE_HZ)


def make_ts(n, seed):
    rng = np.random.default_rng(seed)
    pk = rng.integers(0, 256, size=(n, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    pk[:, 1] = 0x01
    pk[:, 2] = 0x00
    pk[:, 3] = 0x10 | (np.arange(n) % 16)
    return pk


def small_cfg(I_a=0, I_b=0, guard=0):
    return {"mode": P.Mode.MODE3, "gi": "1/8", "guard_frames": guard,
            "layers": {"A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": I_a},
                       "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": I_b}}}


@pytest.fixture(scope="module")
def clean():
    ts = {"A": make_ts(30, 11), "B": make_ts(300, 12)}
    iq, mdbg = modulate(ts, small_cfg(), return_debug=True)
    return ts, iq, mdbg


# -------------------------------------------------------------------------
# デインターリーブは変調器の各段の逆になっている
# -------------------------------------------------------------------------
def test_frequency_deinterleave_inverts_modulator(clean):
    """data_to_logical はランダマイズ・回転の逆のみ。セグメント間の逆まで合わせると
    時間インターリーブ後の行列 Y に戻る（3.12 の逆）。"""
    _, _, mdbg = clean
    C, Y = mdbg["carriers"], mdbg["Y"]
    Yl = DI.data_to_logical(C, P.Mode.MODE3, frame_start=0)
    groups = demod.interleave_groups(True, [])
    assert np.allclose(DI.intersegment_deinterleave(Yl, groups), Y)


def test_time_and_intersegment_deinterleave_invert_modulator():
    ts = {"A": make_ts(100, 13), "B": make_ts(100, 14)}
    cfg = TxConfig(**small_cfg(I_a=4, I_b=2, guard=0))
    _, mdbg = modulate(ts, cfg, return_debug=True)
    I_seg = np.array([4] + [2] * 12)
    Xt = DI.time_deinterleave(mdbg["Y"], I_seg)
    n_sym = Xt.shape[0]
    for s, I in ((0, 4), (3, 2)):
        total = 95 * I + P.time_interleave_adjust(I)
        assert np.allclose(Xt[total:, s, :], mdbg["X"][:n_sym - total, s, :])
    # 階層合成はセグメント間インターリーブを含まないので、X から直接各階層が取り出せる
    assert np.allclose(DI.layer_stream(mdbg["X"], list(range(1, 13))), mdbg["layers"]["B"]["mapped"])
    assert np.allclose(DI.layer_stream(mdbg["X"], [0]), mdbg["layers"]["A"]["mapped"])


def test_demap_and_bit_deinterleave_invert_modulator(clean):
    _, _, mdbg = clean
    for name, mod, rate in (("A", P.Modulation.QPSK, "2/3"), ("B", P.Modulation.QAM64, "3/4")):
        L = mdbg["layers"][name]
        soft = DI.demap_soft(L["mapped"].reshape(-1), mod)
        hard = (soft < 0).astype(np.uint8)
        de = DI.bit_deinterleave(hard.astype(np.float32), mod)
        n = de.shape[0]
        coded = L["coded_bits"][: n * P.BITS_PER_SYMBOL[mod]].reshape(n, -1)
        assert np.array_equal(de[120:].astype(np.uint8), coded[120:])


def test_demap_soft_signs():
    t = P.constellation_table(P.Modulation.QAM64)
    nb = 6
    idx = np.arange(64)
    bits = (idx[:, None] >> (nb - 1 - np.arange(nb))[None, :]) & 1
    soft = DI.demap_soft(t, P.Modulation.QAM64)
    assert np.array_equal((soft < 0).astype(int), bits)
    assert np.all(np.abs(soft) >= 1 - 1e-5)                       # 判定境界から最低 1 離れている
    t16 = P.constellation_table(P.Modulation.QAM16)
    bits16 = (np.arange(16)[:, None] >> (3 - np.arange(4))[None, :]) & 1
    assert np.array_equal((DI.demap_soft(t16, P.Modulation.QAM16) < 0).astype(int), bits16)


# -------------------------------------------------------------------------
# 同期と TMCC
# -------------------------------------------------------------------------
def test_synchronize_clean(clean):
    _, iq, mdbg = clean
    Y, info = sync.synchronize(iq, FS)
    assert info["n_sym"] == 204
    assert abs(info["cfo_hz"]) < 1.0 and info["cfo_int_bins"] == 0
    assert abs(info["clock_ppm"]) < 0.5
    assert abs(info["gi_start0"]) < 2
    # 窓を margin だけ早く開くので、キャリア方向に一定の位相傾斜が乗るだけで振幅は一致する
    C = mdbg["carriers"]
    ratio = Y[7] / C[7]
    assert np.allclose(np.abs(ratio), np.abs(ratio).mean(), rtol=1e-3)
    slope = np.angle(ratio[1:] * np.conj(ratio[:-1]))
    assert np.allclose(slope, -2 * np.pi * info["margin"] / 8192, atol=1e-3)


def test_synchronize_with_offsets(clean):
    """周波数オフセット（整数 + 小数キャリア）とクロック偏差を同期段だけで推定できる。"""
    from isdbt.channel import ChannelConfig, apply_channel
    _, iq, _ = clean
    df, ppm = 37_123.0, 25.0
    y = apply_channel(iq, ChannelConfig(sample_rate_hz=FS, cn_db=None, freq_offset_hz=df, clock_ppm=ppm))
    Y, info = sync.synchronize(y, FS)
    assert abs(info["cfo_hz"] - df) < 20
    assert abs(info["clock_ppm"] - ppm) < 1.0        # +ppm で受信サンプル周期が伸びる
    assert info["n_sym"] >= 203
    b = demod.tmcc_bits_from_carriers(Y, P.Mode.MODE3)
    f0, ok = demod.find_frame_start(b)
    assert ok == 1 and f0 == 0


def test_tmcc_frame_sync_and_parse(clean):
    _, iq, mdbg = clean
    Y, _ = sync.synchronize(iq, FS)
    b = demod.tmcc_bits_from_carriers(Y, P.Mode.MODE3)
    f0, ok = demod.find_frame_start(b)
    assert (f0, ok) == (0, 1)
    assert np.array_equal(b[1:204], mdbg["tmcc_bits"][0][1:204])
    info = tmcc.parse_info(P.Mode.MODE3, b[20:122])
    layers = demod.layers_from_tmcc(info)
    assert [(lyr.name, lyr.mod, lyr.rate, lyr.I, lyr.segments) for lyr in layers] == [
        ("A", P.Modulation.QPSK, "2/3", 0, [0]),
        ("B", P.Modulation.QAM64, "3/4", 0, list(range(1, 13)))]
    assert info.partial_reception is True


def test_channel_estimate_flat(clean):
    _, iq, _ = clean
    Y, _ = sync.synchronize(iq, FS)
    H = demod.channel_estimate(Y, P.Mode.MODE3, frame_start=0)
    D, csi = demod.equalize(Y, H)
    assert np.allclose(csi, 1.0, atol=0.05)
    # 等化後のデータキャリアは正規化コンスタレーションに乗る
    from isdbt import pilots
    dpos = pilots.data_carrier_positions(P.Mode.MODE3, 8)
    d = D[8, dpos[6]]                                   # 中央セグメント（QPSK）
    assert np.allclose(np.abs(d), 1.0, atol=0.02)
    assert np.allclose(np.abs(np.angle(d)) % (np.pi / 2), np.pi / 4, atol=0.02)


# -------------------------------------------------------------------------
# 全段（劣化なし）
# -------------------------------------------------------------------------
def test_demodulate_clean_end_to_end(clean):
    ts, iq, _ = clean
    out, dbg = demod.demodulate(iq, FS, return_debug=True)
    assert dbg["frame_start"] == 0 and dbg["tmcc_parity_ok"] == 1
    for name in ("A", "B"):
        pk = out[name]
        n = len(ts[name])
        # 先頭 11 TSP はバイトデインターリーブの履歴が無く不定、それ以降は完全一致
        assert np.array_equal(pk[11:n], ts[name][11:n])
        # TSP 11 はビットデインターリーブ先頭の不定ビットがバイトデインターリーブで混じるため RS 訂正が入りうる
        assert (dbg["rs_errors"][name][11:n] >= 0).all()
        assert (dbg["rs_errors"][name][12:n] == 0).all()
    assert len(out["B"]) == P.tsp_per_frame(P.Mode.MODE3, P.Modulation.QAM64, "3/4", 12)


def test_rs_decode_corrects_up_to_8():
    rng = np.random.default_rng(5)
    pk = rng.integers(0, 256, (40, 188), dtype=np.uint8)
    c = fec.rs_encode(pk)
    cc = c.copy()
    for i in range(40):
        for p in rng.choice(204, i % 10, replace=False):
            cc[i, p] ^= rng.integers(1, 256)
    fixed, nerr = fec.rs_decode(cc)
    for i in range(40):
        if i % 10 <= 8:
            assert nerr[i] == i % 10 and np.array_equal(fixed[i], c[i])
        else:
            assert nerr[i] == -1


@pytest.mark.slow
def test_block_processing_matches_whole():
    """ブロック処理（履歴 2 フレーム + 重ね 1 フレーム）と一括処理で出力 TS が一致する。"""
    ts = {"A": make_ts(200, 21), "B": make_ts(2000, 22)}
    iq = modulate(ts, small_cfg(I_a=4, I_b=2, guard=1))
    out_small, dbg_small = demod.demodulate(iq, FS, block_frames=2, return_debug=True)
    out_whole, dbg_whole = demod.demodulate(iq, FS, block_frames=1000, return_debug=True)
    assert dbg_small["n_blocks"] > 1 and dbg_whole["n_blocks"] == 1
    for name in ("A", "B"):
        assert np.array_equal(out_small[name], out_whole[name])
        assert dbg_small["first_frame"][name] == dbg_whole["first_frame"][name]
        n = len(ts[name])
        tail = ts[name][n // 2:].tobytes()
        assert tail in out_small[name].tobytes()
        assert (dbg_small["rs_errors"][name][11:] >= 0).all()
