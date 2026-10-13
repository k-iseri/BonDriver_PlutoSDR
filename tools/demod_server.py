#!/usr/bin/env python3
"""復調デーモン（偽物）: BonDriver 開発用に TS を docs/daemon-protocol.md のプロトコルで配る。

実時間復調（rt/）が無い間の代役。2 つのソースを持つ:
  --ts    録画済み TS ファイルを実速で再生する（PCR からビットレートを推定、無ければ --bitrate）
  --sigmf SigMF の I/Q を ref/ で先に全部復調し、階層 A/B をフレーム順に戻してから実速で再生する
          （復調には実時間の 1/14 程度かかる。訂正不能 TSP には transport_error_indicator を立てる）

例:
  python tools/demod_server.py --ts C:/rec/ch27.ts --loop
  python tools/demod_server.py --sigmf testdata/synthetic/std_mode3_gi8_clean.sigmf-meta --loop
  python tools/demod_server.py --ts a.ts --port 47188 --signal-db 30

将来 rt/ の実時間デーモンが同じプロトコルを実装してこのスクリプトを置き換える。
"""
from __future__ import annotations

import argparse
import json
import queue
import socket
import socketserver
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "ref"))

from isdbt import params as P
from isdbt.io import TS_PACKET, TS_SYNC, read_sigmf, read_ts

PROTOCOL_VERSION = 2       # v2: tune/status に system（isdbt|isdbc）。未知キー無視なので v1 クライアントとも互換
DEFAULT_PORT = 47188
DEFAULT_BITRATE = 16_851_000.0      # Mode 3 / GI 1/8 / 64QAM 3/4 × 13 セグメントの TS レート（概算）
CHUNK_PACKETS = 64                   # 1 回の送信単位
STREAM_QUEUE_CHUNKS = 256            # クライアントごとの送信待ち上限（約 3 MB ≈ 1.5 秒 @16 Mbps）
TEI_MASK = 0x80                      # transport_error_indicator（TSP 2 バイト目）


def channel_to_hz(ch: int) -> float:
    """地デジ物理チャンネル → 中心周波数（+1/7 MHz オフセット込み）。tools/iq_record.py と同じ式。"""
    return 473_142_857.0 + 6_000_000.0 * (ch - 13)


# ---------------------------------------------------------------------------
# TS ソースの用意
# ---------------------------------------------------------------------------
def estimate_bitrate_from_pcr(packets: np.ndarray, fallback: float = DEFAULT_BITRATE) -> float:
    """同一 PID の最初と最後の PCR からビットレート [bit/s] を推定する。PCR が無ければ fallback。"""
    pk = packets
    if len(pk) < 2:
        return fallback
    has_af = (pk[:, 3] & 0x20) != 0
    af_len_ok = pk[:, 4] >= 7
    pcr_flag = (pk[:, 5] & 0x10) != 0
    idx = np.nonzero(has_af & af_len_ok & pcr_flag)[0]
    if len(idx) < 2:
        return fallback
    pid = ((pk[idx, 1].astype(np.int64) & 0x1F) << 8) | pk[idx, 2]
    # 最も多く PCR を持つ PID を使う
    pids, counts = np.unique(pid, return_counts=True)
    sel = idx[pid == pids[np.argmax(counts)]]
    if len(sel) < 2:
        return fallback

    def pcr(i: int) -> int:
        b = pk[i, 6:12].astype(np.int64)
        base = (b[0] << 25) | (b[1] << 17) | (b[2] << 9) | (b[3] << 1) | (b[4] >> 7)
        ext = ((b[4] & 1) << 8) | b[5]
        return base * 300 + ext

    first, last = int(sel[0]), int(sel[-1])
    dt = (pcr(last) - pcr(first)) / 27e6
    if dt <= 0:
        return fallback
    return (last - first) * TS_PACKET * 8 / dt


def load_ts_file(path: str | Path) -> tuple[np.ndarray, float, dict]:
    pk = read_ts(path)
    if len(pk) == 0:
        raise SystemExit(f"{path}: TS パケットが無い")
    if not np.all(pk[:, 0] == TS_SYNC):
        print(f"warning: {path}: 同期バイトが 0x47 でない TSP がある（188 バイト境界がずれている可能性）", file=sys.stderr)
    rate = estimate_bitrate_from_pcr(pk)
    return pk, rate, {"source": f"replay:{path}", "locked": True, "mode": 3, "gi": "1/8", "layers": []}


def merge_layers_by_frame(out: dict, dbg: dict, mode: P.Mode) -> np.ndarray:
    """demodulate() の階層別出力を OFDM フレーム順（フレーム内は A, B, C の順）に 1 本の TS へ戻す。

    本来は多重フレーム内の TSP スロット順（BTS の ISDB-T 情報）で戻すべきだが、
    ここでは「同じフレームの TSP を階層順に並べる」近似で済ませる。TVTest の表示には十分。
    訂正不能 TSP（rs_errors < 0）には transport_error_indicator を立てる。
    """
    layers = dbg["layers"]
    per_frame = {}
    start = {}
    for lyr in layers:
        pk = out.get(lyr.name)
        if pk is None or len(pk) == 0:
            continue
        pk = pk.copy()
        nerr = dbg["rs_errors"].get(lyr.name)
        if nerr is not None and len(nerr) == len(pk):
            pk[nerr < 0, 1] |= TEI_MASK
        pk[:11, 1] |= TEI_MASK  # バイトデインターリーブの履歴が無い先頭
        per_frame[lyr.name] = (pk, P.tsp_per_frame(mode, lyr.mod, lyr.rate, len(lyr.segments)))
        start[lyr.name] = dbg["first_frame"].get(lyr.name, 0)
    if not per_frame:
        raise SystemExit("復調結果に TS が無い")
    f_first = min(start.values())
    f_last = max(start[n] + len(pk) // tpf for n, (pk, tpf) in per_frame.items())
    chunks = []
    for f in range(f_first, f_last):
        for lyr in layers:
            if lyr.name not in per_frame:
                continue
            pk, tpf = per_frame[lyr.name]
            i = f - start[lyr.name]
            if i < 0:
                continue
            chunks.append(pk[i * tpf:(i + 1) * tpf])
    return np.concatenate(chunks)


def load_sigmf(path: str | Path, mode: P.Mode, gi: str, max_seconds: float | None) -> tuple[np.ndarray, float, dict]:
    from isdbt.demod import demodulate

    iq, meta = read_sigmf(path)
    fs = float(meta["global"]["core:sample_rate"])
    if max_seconds is not None:
        iq = iq[: int(max_seconds * fs)]
    print(f"demodulating {path} ({len(iq) / fs:.1f} s of I/Q) ... ", end="", flush=True)
    t0 = time.time()
    out, dbg = demodulate(iq, fs, mode, gi, return_debug=True)
    print(f"done in {time.time() - t0:.1f} s")
    pk = merge_layers_by_frame(out, dbg, mode)
    mp = P.MODE_PARAMS[mode]
    frame_s = float(Fraction_frame_seconds(mp.fft_size, gi))
    tsp_per_frame = sum(P.tsp_per_frame(mode, lyr.mod, lyr.rate, len(lyr.segments)) for lyr in dbg["layers"])
    rate = tsp_per_frame * TS_PACKET * 8 / frame_s
    bad = int(sum(int((v < 0).sum()) for v in dbg["rs_errors"].values()))
    info = {
        "source": f"sigmf:{path}",
        "locked": bool(dbg["tmcc_parity_ok"]),
        "mode": int(mode.value) if hasattr(mode, "value") else int(mode),
        "gi": gi,
        "layers": [{"name": lyr.name, "mod": lyr.mod.value, "rate": lyr.rate, "I": lyr.I,
                    "segments": len(lyr.segments)} for lyr in dbg["layers"]],
        "rs_uncorrectable_total": bad,
    }
    return pk, rate, info


def Fraction_frame_seconds(fft_size: int, gi: str):
    return (fft_size * (1 + P.GUARD_INTERVALS[gi])) * P.SYMBOLS_PER_FRAME / P.SAMPLE_RATE_HZ


# ---------------------------------------------------------------------------
# 再生器とサーバ
# ---------------------------------------------------------------------------
@dataclass
class Replayer:
    """TS パケット列をビットレート通りの速さでチャンクに切って配る。"""
    packets: np.ndarray
    bitrate: float
    loop: bool = True
    signal_db: float = 30.0
    info: dict = field(default_factory=dict)
    channel: int | None = None
    freq_hz: float | None = None
    system: str = "isdbt"
    ts_packets: int = 0
    dropped: int = 0
    _pos: int = 0
    _lock: threading.Lock = field(default_factory=threading.Lock)
    _clients: list = field(default_factory=list)
    _stop: threading.Event = field(default_factory=threading.Event)
    _thread: threading.Thread | None = None
    _ended: bool = False

    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="replayer", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=2)

    def add_client(self) -> queue.Queue:
        q: queue.Queue = queue.Queue(maxsize=STREAM_QUEUE_CHUNKS)
        with self._lock:
            self._clients.append(q)
        return q

    def remove_client(self, q: queue.Queue) -> None:
        with self._lock:
            if q in self._clients:
                self._clients.remove(q)

    def tune(self, channel: int | None, freq_hz: float | None, system: str = "isdbt") -> dict:
        with self._lock:
            self.channel = channel
            self.freq_hz = freq_hz
            self.system = system
            self._pos = 0
            self._ended = False
            for q in self._clients:          # 送信待ちを捨てる（選局後のデータだけ流す）
                while True:
                    try:
                        q.get_nowait()
                    except queue.Empty:
                        break
        return {"channel": channel, "freq_hz": freq_hz, "system": system}

    def status(self) -> dict:
        with self._lock:
            n_clients = len(self._clients)
        d = {
            "locked": bool(self.info.get("locked", True)) and not self._ended,
            "signal_db": float(self.signal_db) if not self._ended else 0.0,
            "system": self.system,
            "channel": self.channel,
            "freq_hz": self.freq_hz,
            "mode": self.info.get("mode", 3),
            "gi": self.info.get("gi", "1/8"),
            "layers": self.info.get("layers", []),
            "ts_packets": self.ts_packets,
            "rs_uncorrectable": int(self.info.get("rs_uncorrectable_total", 0)),
            "dropped": self.dropped,
            "stream_clients": n_clients,
            "bitrate": self.bitrate,
        }
        return d

    def _run(self) -> None:
        n = len(self.packets)
        chunk_s = CHUNK_PACKETS * TS_PACKET * 8 / self.bitrate
        t_next = time.perf_counter()
        while not self._stop.is_set():
            chunk = None
            clients = []
            with self._lock:
                if self._pos >= n:
                    if not self.loop:
                        self._ended = True
                    else:
                        self._pos = 0
                if not self._ended:
                    chunk = self.packets[self._pos:self._pos + CHUNK_PACKETS]
                    self._pos += len(chunk)
                    clients = list(self._clients)
            if chunk is not None and len(chunk):
                data = chunk.tobytes()
                self.ts_packets += len(chunk)
                for q in clients:
                    try:
                        q.put_nowait(data)
                    except queue.Full:
                        self.dropped += len(chunk)
            t_next += chunk_s
            delay = t_next - time.perf_counter()
            if delay > 0:
                self._stop.wait(delay)
            elif delay < -1.0:          # 大きく遅れたら追いかけずに基準を引き直す
                t_next = time.perf_counter()


class _Handler(socketserver.StreamRequestHandler):
    server: DemodServer

    def handle(self) -> None:
        rp: Replayer = self.server.replayer
        while True:
            line = self.rfile.readline()
            if not line:
                return
            try:
                req = json.loads(line.decode("utf-8"))
                cmd = req.get("cmd")
            except (ValueError, AttributeError):
                self._send({"ok": False, "error": "invalid json"})
                continue
            if cmd == "hello":
                self._send({"ok": True, "name": "isdbt-sdr demod_server", "protocol": PROTOCOL_VERSION,
                            "source": rp.info.get("source", "")})
            elif cmd == "tune":
                ch = req.get("channel")
                freq = req.get("freq_hz")
                if freq is None and ch is None:
                    self._send({"ok": False, "error": "channel or freq_hz required"})
                    continue
                system = str(req.get("system", "isdbt"))
                if system not in ("isdbt", "isdbc"):
                    self._send({"ok": False, "error": "system must be isdbt or isdbc"})
                    continue
                if freq is None:
                    try:
                        ch = int(ch)
                    except (TypeError, ValueError):
                        self._send({"ok": False, "error": "channel must be int"})
                        continue
                    if system == "isdbc":
                        # ケーブル TV のチャンネル番号（BonDriver_BDA 互換 0..112）
                        from isdbc import params as CP
                        if not 0 <= ch < len(CP.CATV_CHANNELS):
                            self._send({"ok": False, "error": "channel out of range 0..112"})
                            continue
                        freq = CP.catv_channel(ch).freq_hz
                    else:
                        if not 13 <= ch <= 62:
                            self._send({"ok": False, "error": "channel out of range 13..62"})
                            continue
                        freq = channel_to_hz(ch)
                r = rp.tune(ch, float(freq), system)
                self.server.log(f"tune system={system} channel={ch} freq={freq}")
                self._send({"ok": True, **r})
            elif cmd == "status":
                self._send({"ok": True, **rp.status()})
            elif cmd == "stream":
                self._send({"ok": True})
                self._stream()
                return
            elif cmd == "quit":
                if not self.server.allow_quit:
                    self._send({"ok": False, "error": "quit disabled"})
                    continue
                self._send({"ok": True})
                self.server.log("quit requested")
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            else:
                self._send({"ok": False, "error": f"unknown cmd {cmd!r}"})

    def _send(self, obj: dict) -> None:
        self.wfile.write((json.dumps(obj) + "\n").encode("utf-8"))
        self.wfile.flush()

    def _stream(self) -> None:
        rp: Replayer = self.server.replayer
        q = rp.add_client()
        self.server.log(f"stream client {self.client_address} connected")
        try:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            while not self.server.stopping.is_set():
                try:
                    data = q.get(timeout=0.5)
                except queue.Empty:
                    continue
                self.connection.sendall(data)
        except (ConnectionError, OSError):
            pass
        finally:
            rp.remove_client(q)
            self.server.log(f"stream client {self.client_address} disconnected")


class DemodServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, replayer: Replayer, host: str = "127.0.0.1", port: int = DEFAULT_PORT,
                 allow_quit: bool = True, verbose: bool = True):
        self.replayer = replayer
        self.allow_quit = allow_quit
        self.verbose = verbose
        self.stopping = threading.Event()
        super().__init__((host, port), _Handler)

    @property
    def port(self) -> int:
        return self.server_address[1]

    def log(self, msg: str) -> None:
        if self.verbose:
            print(time.strftime("%H:%M:%S"), msg, flush=True)

    def serve_in_thread(self) -> threading.Thread:
        self.replayer.start()
        t = threading.Thread(target=self.serve_forever, name="demod-server", daemon=True)
        t.start()
        return t

    def shutdown(self) -> None:
        self.stopping.set()
        self.replayer.stop()
        super().shutdown()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--ts", help="再生する TS ファイル（188 バイト TSP の連結）")
    src.add_argument("--sigmf", help="復調してから再生する SigMF（.sigmf-meta）")
    ap.add_argument("--mode", type=int, default=3, choices=(1, 2, 3))
    ap.add_argument("--gi", default="1/8", choices=list(P.GUARD_INTERVALS))
    ap.add_argument("--max-seconds", type=float, default=None, help="--sigmf の先頭から処理する秒数")
    ap.add_argument("--bitrate", type=float, default=None, help="再生ビットレート [bit/s]（既定: PCR から推定 / 復調結果から算出）")
    ap.add_argument("--loop", action="store_true", help="末尾まで来たら先頭に戻る")
    ap.add_argument("--signal-db", type=float, default=30.0, help="status.signal_db に返す固定値")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--no-quit", action="store_true", help="quit コマンドを受け付けない")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if args.ts:
        pk, rate, info = load_ts_file(args.ts)
    else:
        pk, rate, info = load_sigmf(args.sigmf, P.Mode(args.mode), args.gi, args.max_seconds)
    if args.bitrate:
        rate = args.bitrate
    rp = Replayer(pk, rate, loop=args.loop, signal_db=args.signal_db, info=info)
    srv = DemodServer(rp, args.host, args.port, allow_quit=not args.no_quit, verbose=not args.quiet)
    print(f"serving {len(pk)} TSP ({len(pk) * TS_PACKET * 8 / rate:.1f} s at {rate / 1e6:.3f} Mbps, "
          f"loop={args.loop}) on {args.host}:{srv.port}  source={info.get('source')}", flush=True)
    srv.serve_in_thread()
    try:
        while not srv.stopping.wait(1.0):
            pass
    except KeyboardInterrupt:
        srv.shutdown()
    srv.server_close()


if __name__ == "__main__":
    main()
