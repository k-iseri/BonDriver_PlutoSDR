"""rt/（C++ 実装）の検証。ビルド済みの rt_test.exe / isdbtd.exe が無ければ skip する。

ビルド: .\\rt\\build.ps1（または make rt）
ベクタ: tools/gen_rt_vectors.py（無ければこのテストが生成する）
"""
from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
VECTORS = ROOT / "testdata" / "rt_vectors"


def _exe(name: str) -> Path | None:
    for config in ("Release", "Debug"):
        p = ROOT / "rt" / "build" / "rt-x64" / config / f"{name}.exe"
        if p.exists():
            return p
    return None


def _require_exe(name: str) -> Path:
    p = _exe(name)
    if p is None:
        pytest.skip(f"{name}.exe が未ビルド（.\\rt\\build.ps1 を実行）")
    return p


@pytest.mark.slow
def test_rt_matches_ref():
    """rt_test.exe が全段で ref/ のベクタと一致すること。"""
    exe = _require_exe("rt_test")
    if not (VECTORS / "expected.json").exists():
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_rt_vectors.py"),
                            "--out", str(VECTORS)], capture_output=True, text=True, check=False)
        assert r.returncode == 0, f"ベクタ生成に失敗:\n{r.stdout}\n{r.stderr}"
    r = subprocess.run([str(exe), str(VECTORS)], capture_output=True, text=True, timeout=600, check=False)
    assert r.returncode == 0, f"rt_test failed:\n{r.stdout}\n{r.stderr}"


# ---------------------------------------------------------------------------
# isdbtd.exe のプロトコル試験（--ts 再生。docs/daemon-protocol.md v2）
# ---------------------------------------------------------------------------
def _free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _packets(n: int) -> np.ndarray:
    pk = np.full((n, 188), 0xFF, dtype=np.uint8)
    pk[:, 0] = 0x47
    pk[:, 1] = 0x1F
    pk[:, 2] = 0xFF
    pk[:, 3] = 0x10
    idx = np.arange(n)
    pk[:, 4] = idx >> 8
    pk[:, 5] = idx & 0xFF
    return pk


@pytest.fixture
def daemon(tmp_path):
    exe = _require_exe("isdbtd")
    ts = tmp_path / "test.ts"
    _packets(5000).tofile(ts)
    port = _free_port()
    proc = subprocess.Popen([str(exe), "--ts", str(ts), "--loop", "--bitrate", "50e6",
                             "--signal-db", "27.5", "--port", str(port), "--quiet"],
                            stderr=subprocess.PIPE)
    # 起動待ち（接続できるまで）
    deadline = time.time() + 10
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            if proc.poll() is not None:
                pytest.fail(f"isdbtd が起動しない: {proc.stderr.read().decode(errors='replace')}")
            time.sleep(0.1)
    yield port
    proc.kill()
    proc.wait(timeout=5)


def _connect(port: int):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    return s, s.makefile("rwb")


def _cmd(f, obj: dict) -> dict:
    f.write((json.dumps(obj) + "\n").encode())
    f.flush()
    line = f.readline()
    assert line.endswith(b"\n")
    return json.loads(line)


def test_isdbtd_control(daemon):
    port = daemon
    s, f = _connect(port)
    r = _cmd(f, {"cmd": "hello"})
    assert r["ok"] and r["protocol"] == 2 and r["source"].startswith("replay:")

    r = _cmd(f, {"cmd": "tune", "channel": 27})
    assert r["ok"] and r["channel"] == 27 and r["freq_hz"] == pytest.approx(557_142_857.0)
    assert r["system"] == "isdbt"
    # v2: ケーブル TV（ISDB-C）の通し番号 55 = UHF 56ch = 731 MHz
    r = _cmd(f, {"cmd": "tune", "system": "isdbc", "channel": 55})
    assert r["ok"] and r["system"] == "isdbc" and r["freq_hz"] == pytest.approx(731e6)
    assert not _cmd(f, {"cmd": "tune", "system": "isdbc", "channel": 113})["ok"]
    assert not _cmd(f, {"cmd": "tune", "system": "foo", "channel": 1})["ok"]
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


def test_isdbtd_stream(daemon):
    port = daemon
    sc, fc = _connect(port)
    ss, fs = _connect(port)
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
    data = np.frombuffer(bytes(buf[: n * 188]), dtype=np.uint8).reshape(-1, 188)
    assert np.all(data[:, 0] == 0x47)
    idx = (data[:, 4].astype(int) << 8) | data[:, 5]
    assert np.all(np.diff(idx) % 5000 == 1), "TSP の順序が崩れている"
    assert 0 in idx            # 選局後は先頭から流れ直す

    r = _cmd(fc, {"cmd": "status"})
    assert r["stream_clients"] == 1 and r["ts_packets"] > 0
    for x in (fs, ss, fc, sc):
        x.close()


def test_isdbtd_quit(daemon):
    port = daemon
    s, f = _connect(port)
    assert _cmd(f, {"cmd": "quit"})["ok"]
    s.close()
    # quit 後は新規接続できなくなる
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            time.sleep(0.1)
        except OSError:
            return
    pytest.fail("quit してもデーモンが止まらない")
