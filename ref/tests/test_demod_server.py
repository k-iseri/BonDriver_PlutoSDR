"""tools/demod_server.py（偽復調デーモン）のプロトコル試験。docs/daemon-protocol.md が仕様。"""
import importlib.util
import json
import socket
import sys
import time
from pathlib import Path

import numpy as np
import pytest
from isdbt import params as P
from isdbt.demod import RxLayer

TOOLS = Path(__file__).resolve().parents[2] / "tools"


def _load_module():
    spec = importlib.util.spec_from_file_location("demod_server", TOOLS / "demod_server.py")
    m = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = m   # dataclass が型注釈を解決するときにモジュールを参照する
    spec.loader.exec_module(m)
    return m


def _packets(n: int) -> np.ndarray:
    """ヌルパケット。ペイロード先頭 2 バイトに通し番号を入れる。"""
    pk = np.full((n, 188), 0xFF, dtype=np.uint8)
    pk[:, 0] = 0x47
    pk[:, 1] = 0x1F
    pk[:, 2] = 0xFF
    pk[:, 3] = 0x10
    idx = np.arange(n)
    pk[:, 4] = idx >> 8
    pk[:, 5] = idx & 0xFF
    return pk


def _index(data: bytes) -> np.ndarray:
    a = np.frombuffer(data, dtype=np.uint8).reshape(-1, 188)
    return (a[:, 4].astype(int) << 8) | a[:, 5]


@pytest.fixture
def server():
    m = _load_module()
    rp = m.Replayer(_packets(5000), bitrate=50e6, loop=True, signal_db=27.5, info={"source": "test"})
    srv = m.DemodServer(rp, "127.0.0.1", 0, verbose=False)
    srv.serve_in_thread()
    yield m, srv
    srv.shutdown()
    srv.server_close()


def _connect(port: int):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    return s, s.makefile("rwb")


def _cmd(f, obj: dict) -> dict:
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    line = f.readline()
    assert line.endswith(b"\n")
    return json.loads(line)


def test_control_commands(server):
    m, srv = server
    s, f = _connect(srv.port)
    r = _cmd(f, {"cmd": "hello"})
    assert r["ok"] and r["protocol"] == m.PROTOCOL_VERSION and r["source"] == "test"

    r = _cmd(f, {"cmd": "tune", "channel": 27})
    assert r["ok"] and r["channel"] == 27 and r["freq_hz"] == pytest.approx(557_142_857.0)
    r = _cmd(f, {"cmd": "tune", "freq_hz": 500e6})
    assert r["ok"] and r["freq_hz"] == 500e6
    assert not _cmd(f, {"cmd": "tune", "channel": 99})["ok"]
    assert not _cmd(f, {"cmd": "tune"})["ok"]

    r = _cmd(f, {"cmd": "status"})
    assert r["ok"] and r["signal_db"] == 27.5 and r["locked"] and r["stream_clients"] == 0
    assert r["freq_hz"] == 500e6

    assert not _cmd(f, {"cmd": "nosuch"})["ok"]
    f.write(b"this is not json\n")
    f.flush()
    assert not json.loads(f.readline())["ok"]
    s.close()


def test_stream_is_aligned_and_in_order(server):
    _, srv = server
    sc, fc = _connect(srv.port)
    ss, fs = _connect(srv.port)
    assert _cmd(fs, {"cmd": "stream"})["ok"]
    assert _cmd(fc, {"cmd": "tune", "channel": 20})["ok"]

    want = 188 * 600
    buf = bytearray()
    deadline = time.time() + 5
    while len(buf) < want and time.time() < deadline:
        chunk = ss.recv(65536)
        if not chunk:
            break
        buf += chunk
    assert len(buf) >= want
    n = len(buf) // 188
    data = bytes(buf[: n * 188])
    assert all(data[i] == 0x47 for i in range(0, len(data), 188))
    idx = _index(data)
    assert np.all(np.diff(idx) % 5000 == 1), "TSP の順序が崩れている"
    # 選局後は先頭から流れ直す
    assert 0 in idx

    r = _cmd(fc, {"cmd": "status"})
    assert r["stream_clients"] == 1 and r["ts_packets"] > 0
    fs.close()   # makefile() の参照が残っていると実際には閉じない
    ss.close()
    fc.close()
    sc.close()
    s2, f2 = _connect(srv.port)
    deadline = time.time() + 3
    while time.time() < deadline and _cmd(f2, {"cmd": "status"})["stream_clients"] != 0:
        time.sleep(0.05)
    assert _cmd(f2, {"cmd": "status"})["stream_clients"] == 0
    f2.close()
    s2.close()


def test_quit_stops_server(server):
    _, srv = server
    s, f = _connect(srv.port)
    assert _cmd(f, {"cmd": "quit"})["ok"]
    s.close()
    assert srv.stopping.wait(3)


def test_quit_can_be_disabled():
    m = _load_module()
    rp = m.Replayer(_packets(100), bitrate=50e6, info={"source": "t"})
    srv = m.DemodServer(rp, "127.0.0.1", 0, allow_quit=False, verbose=False)
    srv.serve_in_thread()
    try:
        s, f = _connect(srv.port)
        assert not _cmd(f, {"cmd": "quit"})["ok"]
        assert _cmd(f, {"cmd": "hello"})["ok"]
        s.close()
    finally:
        srv.shutdown()
        srv.server_close()


def test_non_loop_ends_and_reports_unlocked():
    m = _load_module()
    rp = m.Replayer(_packets(200), bitrate=50e6, loop=False, info={"source": "t"})
    srv = m.DemodServer(rp, "127.0.0.1", 0, verbose=False)
    srv.serve_in_thread()
    try:
        deadline = time.time() + 5
        while time.time() < deadline and not rp._ended:
            time.sleep(0.05)
        s, f = _connect(srv.port)
        r = _cmd(f, {"cmd": "status"})
        assert not r["locked"] and r["signal_db"] == 0.0
        assert _cmd(f, {"cmd": "tune", "channel": 13})["ok"]   # 選局で先頭から再生し直す
        assert _cmd(f, {"cmd": "status"})["locked"]
        s.close()
    finally:
        srv.shutdown()
        srv.server_close()


def _pcr_packet(pid: int, ticks27: int) -> np.ndarray:
    pk = np.full(188, 0xFF, dtype=np.uint8)
    pk[0] = 0x47
    pk[1] = (pid >> 8) & 0x1F
    pk[2] = pid & 0xFF
    pk[3] = 0x30          # adaptation + payload
    pk[4] = 7             # adaptation length
    pk[5] = 0x10          # PCR flag
    base, ext = divmod(ticks27, 300)
    pk[6] = (base >> 25) & 0xFF
    pk[7] = (base >> 17) & 0xFF
    pk[8] = (base >> 9) & 0xFF
    pk[9] = (base >> 1) & 0xFF
    pk[10] = ((base & 1) << 7) | 0x7E | ((ext >> 8) & 1)
    pk[11] = ext & 0xFF
    return pk


def test_bitrate_from_pcr():
    m = _load_module()
    pk = _packets(2001)
    bits = 2000 * 188 * 8
    dt = bits / 10e6                         # 10 Mbps なら 2000 TSP で dt 秒
    pk[0] = _pcr_packet(0x100, 1_000_000)
    pk[2000] = _pcr_packet(0x100, 1_000_000 + round(dt * 27e6))
    assert m.estimate_bitrate_from_pcr(pk) == pytest.approx(10e6, rel=1e-6)
    assert m.estimate_bitrate_from_pcr(_packets(10), fallback=123.0) == 123.0


def test_merge_layers_by_frame_order_and_tei():
    m = _load_module()
    mode = P.Mode.MODE3
    la = RxLayer("A", P.Modulation.QPSK, "2/3", 4, [0])
    lb = RxLayer("B", P.Modulation.QAM64, "3/4", 2, list(range(1, 13)))
    na = P.tsp_per_frame(mode, la.mod, la.rate, 1)
    nb = P.tsp_per_frame(mode, lb.mod, lb.rate, 12)

    def mk(tag, tpf, frames, f0):
        pk = np.zeros((tpf * frames, 188), np.uint8)
        pk[:, 0] = 0x47
        pk[:, 4] = tag
        pk[:, 5] = np.repeat(np.arange(f0, f0 + frames), tpf)
        return pk

    out = {"A": mk(1, na, 3, 2), "B": mk(2, nb, 4, 1)}
    nerr_a = np.zeros(len(out["A"]), np.int64)
    nerr_a[20] = -1
    dbg = {"layers": [la, lb], "first_frame": {"A": 2, "B": 1},
           "rs_errors": {"A": nerr_a, "B": np.zeros(len(out["B"]), np.int64)}}
    ts = m.merge_layers_by_frame(out, dbg, mode)
    assert len(ts) == len(out["A"]) + len(out["B"])
    # フレーム 1 は B だけ、フレーム 2 からは A → B の順
    assert np.all(ts[:nb, 4] == 2) and np.all(ts[:nb, 5] == 1)
    assert np.all(ts[nb:nb + na, 4] == 1) and np.all(ts[nb:nb + na, 5] == 2)
    assert np.all(ts[nb + na:nb + na + nb, 4] == 2) and np.all(ts[nb + na:nb + na + nb, 5] == 2)
    # TEI: A の先頭 11 TSP と訂正不能だった 1 TSP、B の先頭 11 TSP
    a_rows = ts[ts[:, 4] == 1]
    tei = (a_rows[:, 1] & 0x80) != 0
    assert tei[:11].all() and tei[20] and tei.sum() == 12
    b_rows = ts[ts[:, 4] == 2]
    assert ((b_rows[:, 1] & 0x80) != 0).sum() == 11
