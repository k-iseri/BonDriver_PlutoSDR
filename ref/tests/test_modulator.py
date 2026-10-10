"""modulator.py の構造テスト。

復調器が無くても検証できる性質を確認する:
- 出力長・電力・GI
- IFFT 前のキャリア行列が出力から復元できる
- パイロット（SP/CP/AC）と TMCC の位置・値
- 各段の「フレーム整合」規約: 受信側の仕様どおりの補遅延で TSP 境界がフレーム先頭に戻る
"""
import numpy as np
import pytest
from isdbt import fec, pilots, tmcc
from isdbt import params as P
from isdbt.modulator import TxConfig, modulate

FS = float(P.SAMPLE_RATE_HZ)
N = 8192
NG = 1024
SYM = N + NG


def make_ts(n, seed):
    rng = np.random.default_rng(seed)
    pk = rng.integers(0, 256, size=(n, 188), dtype=np.uint8)
    pk[:, 0] = 0x47
    pk[:, 1] = 0x01
    pk[:, 2] = 0x00
    pk[:, 3] = 0x10 | (np.arange(n) % 16)
    return pk


def small_cfg(I_a=0, I_b=0):
    """時間インターリーブ無し（I=0）で 1 フレームに収まる高速な構成。"""
    return {"mode": P.Mode.MODE3, "gi": "1/8", "guard_frames": 0,
            "layers": {"A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": I_a},
                       "B": {"segments": 12, "mod": P.Modulation.QAM64, "rate": "3/4", "I": I_b}}}


@pytest.fixture(scope="module")
def small():
    ts = {"A": make_ts(30, 1), "B": make_ts(300, 2)}
    iq, dbg = modulate(ts, small_cfg(), return_debug=True)
    return ts, iq, dbg


def _fft_carriers(iq, n):
    sym = iq[n * SYM + NG:(n + 1) * SYM]
    S = np.fft.fft(sym)
    return S[P.carrier_to_fft_bin(P.Mode.MODE3, np.arange(5617))]


def test_config_validation():
    with pytest.raises(ValueError):
        TxConfig(layers={"A": {"segments": 2, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 0}})
    with pytest.raises(ValueError):
        TxConfig(layers={"A": {"segments": 13, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 3}})
    cfg = TxConfig(**small_cfg())
    assert cfg.partial_reception is True
    assert cfg.segment_numbers() == {"A": [0], "B": list(range(1, 13))}


def test_output_length_power_gi(small):
    _, iq, dbg = small
    assert dbg["n_frames"] == 1
    assert iq.dtype == np.complex64
    assert len(iq) == 204 * SYM
    assert abs(np.mean(np.abs(iq) ** 2) - 1.0) < 1e-3
    for n in (0, 100):
        s = iq[n * SYM:(n + 1) * SYM]
        assert np.allclose(s[:NG], s[N:], atol=1e-6)


def test_fft_recovers_carriers(small):
    _, iq, dbg = small
    C = dbg["carriers"]
    assert C.shape == (204, 5617)
    for n in (0, 1, 2, 3, 77, 203):
        k = _fft_carriers(iq, n)
        f = np.vdot(C[n], k) / np.vdot(C[n], C[n])     # 共通スケール
        assert np.max(np.abs(k / f - C[n])) < 1e-4
    # 帯域外（有効キャリア以外のビン）は空
    S = np.fft.fft(iq[NG:SYM])
    mask = np.ones(N, bool)
    mask[P.carrier_to_fft_bin(P.Mode.MODE3, np.arange(5617))] = False
    assert np.max(np.abs(S[mask])) < 1e-3 * np.max(np.abs(S))


def test_pilots(small):
    _, _, dbg = small
    C = dbg["carriers"]
    pv = pilots.pilot_values(P.Mode.MODE3)
    assert set(np.round(np.abs(pv), 6)) == {round(4 / 3, 6)}
    for n in range(8):
        sp = pilots.sp_positions(P.Mode.MODE3, n)
        assert sp[0] == 3 * (n % 4) and len(sp) == 36 * 13
        assert np.allclose(C[n, sp], pv[sp])
    assert np.allclose(C[:, 5616], pv[5616])                   # CP
    assert pv[5616] > 0                                        # 表 3-19: Mode 3 の右端 CP は +4/3
    # AC1: スタッフィング 1 の DBPSK → 基準シンボルから毎シンボル反転
    ac = np.array(P.AC1_POSITIONS_ABS[P.Mode.MODE3])
    sign = np.where(np.arange(204) % 2 == 0, 1.0, -1.0)
    assert np.allclose(C[:, ac], pv[ac][None, :] * sign[:, None])
    # データキャリアの最大振幅は 64QAM の角 |7+7j|/√42 ≈ 1.528。実軸のみの ±4/3 は取らない
    dpos = pilots.data_carrier_positions(P.Mode.MODE3, 0)
    d = C[0, dpos]
    assert np.max(np.abs(d)) < 1.53
    assert np.all(np.abs(d) > 0)
    assert not np.any(np.isclose(d.imag, 0) & np.isclose(np.abs(d.real), 4 / 3))


def test_tmcc_from_carriers(small):
    _, _, dbg = small
    C = dbg["carriers"]
    cfg = dbg["config"]
    tm = np.array(P.TMCC_POSITIONS_ABS[P.Mode.MODE3])
    pv = pilots.pilot_values(P.Mode.MODE3)
    # 全 TMCC キャリアは同じ値（PRBS を除く）
    s = C[:, tm].real * pv[tm][None, :]
    assert np.allclose(s, s[:, :1])
    d = (s[:, 0] < 0).astype(np.uint8)                          # 差動符号化後の D_n
    b = np.zeros(204, np.uint8)
    b[1:] = d[1:] ^ d[:-1]
    assert d[0] == 0
    assert tmcc.sync_word_of(b) == 0
    assert list(b[17:20]) == [0, 0, 0]                           # 表 3-20: 同期変調 000
    assert tmcc.dscc_check(b[20:204])
    info = tmcc.parse_info(P.Mode.MODE3, b[20:122])
    assert info.partial_reception is True
    assert info.layers["A"] == tmcc.TmccLayer(P.Modulation.QPSK, "2/3", 0, 1)
    assert info.layers["B"] == tmcc.TmccLayer(P.Modulation.QAM64, "3/4", 0, 12)
    assert info.layers["C"] is None
    assert cfg.tmcc_info().layers["B"].segments == 12


def test_spectrum_occupancy(small):
    _, iq, _ = small
    n = 1 << 18
    spec = np.abs(np.fft.fftshift(np.fft.fft(iq[:n]))) ** 2
    f = np.fft.fftshift(np.fft.fftfreq(n, 1 / FS))
    half_bw = 5617 * float(P.MODE_PARAMS[P.Mode.MODE3].carrier_spacing_hz) / 2
    inband = spec[np.abs(f) < half_bw + 10e3].sum() / spec.sum()
    assert inband > 0.97


def test_byte_chain_frame_alignment(small):
    """バイトインターリーブ出力に受信側の補遅延（11-b TSP）をかけると TSP 境界がフレーム先頭に戻り、
    エネルギー逆拡散 → RS シンドローム 0 → 入力 TS に一致する。"""
    ts, _, dbg = small
    for name in ("A", "B"):
        L = dbg["layers"][name]
        tsp_pf = P.tsp_per_frame(P.Mode.MODE3, dbg["config"].layers[name].mod,
                                 dbg["config"].layers[name].rate, dbg["config"].layers[name].segments)
        de = fec.byte_deinterleave(L["byte_interleaved"])
        frame0 = de[:tsp_pf]
        # 先頭 11 TSP は補遅延で不定のため、それ以降を比較
        assert np.array_equal(frame0[11:], L["dispersed"][11:tsp_pf])
        prbs = fec.energy_dispersal_prbs(tsp_pf * 204).reshape(tsp_pf, 204)
        back = np.empty_like(frame0)
        back[:, 0] = frame0[:, 203]
        back[:, 1:] = frame0[:, :203] ^ prbs[:, :203]
        assert np.array_equal(back[11:], L["tsp"][11:tsp_pf])
        assert (fec.rs_syndromes(back[11:]) == 0).all()
        n_in = min(len(ts[name]), tsp_pf)
        assert np.array_equal(back[11:n_in, :188], ts[name][11:n_in])


def test_bit_chain_frame_alignment(small):
    """マッピング出力をデマップし、受信側の補遅延（120-d_i）をかけると符号化ビット列に戻る。"""
    _, _, dbg = small
    for name, mod in (("A", P.Modulation.QPSK), ("B", P.Modulation.QAM64)):
        L = dbg["layers"][name]
        sym = L["mapped"].ravel()
        table = P.constellation_table(mod)
        idx = np.argmin(np.abs(sym[:, None] - table[None, :]), axis=1)
        nb = P.BITS_PER_SYMBOL[mod]
        bits = (idx[:, None] >> (nb - 1 - np.arange(nb))[None, :]) & 1   # 列 i = b_i
        delays = np.array(P.BIT_INTERLEAVE_DELAYS[mod])
        n = len(sym)
        t = np.arange(120, n)
        rx = np.stack([bits[t - (120 - delays[i]), i] for i in range(nb)], axis=1)
        coded = L["coded_bits"][: n * nb].reshape(n, nb)
        assert np.array_equal(rx, coded[120:])


def _derand_derot(Z: np.ndarray) -> np.ndarray:
    """ランダマイズ・ローテーションだけを戻す（セグメント間はそのまま）。"""
    perm = np.array(P.CARRIER_RANDOMIZE_TABLE[P.Mode.MODE3])
    c = np.arange(Z.shape[2])
    W = np.empty_like(Z)
    for s in range(P.N_SEGMENTS):
        W[:, s, :] = Z[:, s, perm[(c - s) % Z.shape[2]]]
    return W


def test_frequency_interleave_inverse(small):
    """Z = ランダマイズ(ローテーション(セグメント間(Y)))。図 3-26 の直列 S_q は
    セグメント group[q mod n]、キャリア q div n に置かれる（2026-10-10 実放送で順序検証済み）。"""
    _, _, dbg = small
    Y, Z = dbg["Y"], dbg["Z"]
    n_sym, _, dc = Y.shape
    W = _derand_derot(Z)                     # = セグメント間インターリーブ後の行列
    group = list(range(1, 13))               # small は部分受信構成
    n = len(group)
    S = Y[:, group, :].reshape(n_sym, n * dc)
    for q in (0, 1, 11, 12, 13, n * dc - 1):
        assert np.array_equal(W[:, group[q % n], q // n], S[:, q])
    # セグメント 0（部分受信）はセグメント間の対象外かつローテーションなし: 位置 = perm[c]
    perm = np.array(P.CARRIER_RANDOMIZE_TABLE[P.Mode.MODE3])
    assert np.array_equal(Z[:, 0, perm], Y[:, 0, :])


def test_hierarchical_combine_plain(small):
    """階層合成（図 3-21）は各階層の直列シンボルを担当セグメントへ順に詰めるだけ
    （セグメント間インターリーブは周波数インターリーブ側 3.12 にある）。"""
    _, _, dbg = small
    X = dbg["X"]
    mapped = dbg["layers"]["B"]["mapped"]
    dc = X.shape[2]
    for k in (0, 1, 11):
        assert np.array_equal(X[:, 1 + k, :], mapped[:, k * dc:(k + 1) * dc])
    assert np.array_equal(X[:, 0, :], dbg["layers"]["A"]["mapped"])


def test_intersegment_interleave_spans_layers():
    """同期変調部は階層が違ってもまとめてインターリーブする（3.11.2）: B=8, C=4 の構成。"""
    from isdbt.modulator import frequency_interleave, interleave_groups
    cfg = TxConfig(mode=P.Mode.MODE3, gi="1/8", layers={
        "A": {"segments": 1, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 0},
        "B": {"segments": 8, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 0},
        "C": {"segments": 4, "mod": P.Modulation.QAM16, "rate": "1/2", "I": 0}})
    assert interleave_groups(cfg) == [list(range(1, 13))]
    n_sym, dc = 2, 384
    rng = np.random.default_rng(0)
    Y = (rng.normal(size=(n_sym, P.N_SEGMENTS, dc)) + 0j).astype(np.complex64)
    W = _derand_derot(frequency_interleave(cfg, Y))
    group = list(range(1, 13))               # B と C をまたいだ 1 グループ
    S = Y[:, group, :].reshape(n_sym, 12 * dc)
    for q in (0, 1, 11, 12, 8 * dc - 1, 8 * dc, 12 * dc - 1):
        assert np.array_equal(W[:, group[q % 12], q // 12], S[:, q])
    assert np.array_equal(W[:, 0, :], Y[:, 0, :])
    # 部分受信なし（A が 2 セグメント）なら 13 セグメント全部が 1 グループ
    cfg2 = TxConfig(mode=P.Mode.MODE3, gi="1/8", layers={
        "A": {"segments": 2, "mod": P.Modulation.QPSK, "rate": "2/3", "I": 0},
        "B": {"segments": 11, "mod": P.Modulation.QAM64, "rate": "3/4", "I": 0}})
    assert cfg2.partial_reception is False
    assert interleave_groups(cfg2) == [list(range(13))]


@pytest.mark.slow
def test_time_interleave_standard_config():
    """日本の標準構成（I=4/2）。遅延分のフレームが付加され、受信側補遅延 I*(95-m) で整数フレームに戻る。"""
    ts = {"A": make_ts(100, 3), "B": make_ts(100, 4)}
    cfg = small_cfg(I_a=4, I_b=2)
    cfg["guard_frames"] = 1
    iq, dbg = modulate(ts, cfg, return_debug=True)
    assert dbg["data_frames"] == 2                     # A: 100 TSP / 64 per frame
    assert dbg["n_frames"] == 2 + 2 + 1                # I=4: 95*4+28 = 408 シンボル = 2 フレーム追加 + 余裕 1
    X, Y = dbg["X"], dbg["Y"]
    n_sym = X.shape[0]
    c = np.arange(384)
    for s, I in ((0, 4), (5, 2)):
        rx_delay = I * (95 - (5 * c) % 96)
        total = 95 * I + P.time_interleave_adjust(I)
        assert total % 204 == 0
        for cc in (0, 1, 50, 383):
            src = np.arange(n_sym) - rx_delay[cc]
            ok = src >= total          # 送受合計遅延分より後は元に戻る（整数フレームずれ）
            got = Y[src[ok] , s, cc]
            assert np.array_equal(got, X[np.arange(n_sym)[ok] - total, s, cc])
    assert len(iq) == 5 * 204 * SYM
