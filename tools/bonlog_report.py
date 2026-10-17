#!/usr/bin/env python3
"""BonDriver_PlutoSDR_T.log（[Debug] Log=1 の 10 秒ごとの復調統計）と bontest の出力から
長時間視聴試験の結果を集計する。

    python tools/bonlog_report.py rt/build/bondriver-x64/Release/BonDriver_PlutoSDR_T.log
    python tools/bonlog_report.py <log> --bontest longrun.txt --every 6

既定では最後の「loaded;」（DLL ロード）以降の 1 セッションだけを見る。--all で全セッション。
出力: 区間ごとの表（累積カウンタは前行からの増分）と、ロック率・MER の分布・増えたカウンタの一覧。
bontest の出力（--interval N で出る "drop N (+k) error M (+k)" 行）を --bontest で渡すと、
TS 側のドロップ（継続カウンタ欠落、TVTest の D）と TEI（TVTest の E）も合わせて集計する。
"""
from __future__ import annotations

import argparse
import json
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path

# 累積カウンタ（前行との差を出す）。順番は表の列順。
CUMULATIVE = (
    "rs_uncorrectable", "dropped", "underruns", "frames_lost", "hw_dropped", "hw_ovf_events",
    "sdr_reconnects", "splice_resets", "fpga_symbol_gaps", "fpga_timeouts", "windows_failed",
    # ISDB-C（live_isdbc）のみ。地デジの status には無いので None → 増分 0 扱い
    "sync_losses", "reacquisitions", "tsmf_bad_headers",
)
# そのまま出す値（cfo_hz / clock_ppm は ISDB-C のみ）
GAUGES = ("signal_db", "gain_db", "rx_level_dbfs", "bitrate", "cfo_hz", "clock_ppm")

_LINE = re.compile(r"^(\d{2}):(\d{2}):(\d{2})\.(\d{3}) \[BonDriver_PlutoSDR\] (.*)$")
_BONTEST = re.compile(
    r"^(\d{2}):(\d{2}):(\d{2})\s+([\d.]+) s\s+([\d.]+) Mbps\s+level\s+([-\d.]+)\s+pkts (\d+)\s+bad (\d+)"
    r"\s+drop (\d+) \(\+(\d+)\)\s+error (\d+) \(\+(\d+)\)"
)


@dataclass
class Event:
    t: float            # セッション先頭からの秒（深夜 0 時またぎは +86400 で補正）
    clock: str          # HH:MM:SS
    kind: str           # status / tune / loaded / closed / other
    text: str = ""
    status: dict = field(default_factory=dict)


@dataclass
class Session:
    events: list[Event] = field(default_factory=list)

    @property
    def statuses(self) -> list[Event]:
        return [e for e in self.events if e.kind == "status"]


def _clock_seconds(h: str, m: str, s: str, ms: str) -> float:
    return int(h) * 3600 + int(m) * 60 + int(s) + int(ms) / 1000.0


def parse_log(lines) -> list[Session]:
    """ログ行の列を「loaded;」区切りのセッション列にする。時刻は日付が無いので 0 時またぎだけ補正する。"""
    sessions: list[Session] = []
    cur: Session | None = None
    prev_abs = None
    day_offset = 0.0
    origin = None
    for raw in lines:
        m = _LINE.match(raw.rstrip("\r\n"))
        if not m:
            continue
        h, mi, s, ms, body = m.groups()
        abs_t = _clock_seconds(h, mi, s, ms)
        if prev_abs is not None and abs_t + 3600 < prev_abs:       # 1 時間以上「戻った」= 日付が変わった
            day_offset += 86400
        prev_abs = abs_t
        t_abs = abs_t + day_offset
        if body.startswith("loaded;") or cur is None:
            cur = Session()
            sessions.append(cur)
            origin = t_abs
        t = t_abs - (origin if origin is not None else t_abs)
        clock = f"{h}:{mi}:{s}"
        if body.startswith("live: "):
            try:
                st = json.loads(body[len("live: "):])
            except json.JSONDecodeError:
                cur.events.append(Event(t, clock, "other", body))
                continue
            cur.events.append(Event(t, clock, "status", body, st))
        elif body.startswith("tune "):
            cur.events.append(Event(t, clock, "tune", body))
        elif body.startswith("loaded;"):
            cur.events.append(Event(t, clock, "loaded", body))
        elif body.startswith("closed"):
            cur.events.append(Event(t, clock, "closed", body))
        else:
            cur.events.append(Event(t, clock, "other", body))
    return sessions


def deltas(statuses: list[Event]) -> list[dict]:
    """各 status 行について、累積カウンタの前行からの増分を付けた辞書を返す。
    選局（channel が変わる）や再接続でカウンタがリセットされ負になった場合は、その行の増分を 0 扱いにする。"""
    rows = []
    prev = None
    for e in statuses:
        st = dict(e.status)
        tsmf = st.get("tsmf")
        if isinstance(tsmf, dict) and "bad_headers" in tsmf:
            st["tsmf_bad_headers"] = tsmf["bad_headers"]
        row = {"t": e.t, "clock": e.clock, "locked": bool(st.get("locked")), "channel": st.get("channel"),
               "system": st.get("system", "isdbt")}
        for g in GAUGES:
            row[g] = st.get(g)
        for c in CUMULATIVE:
            v = st.get(c)
            if prev is None or v is None or prev.get(c) is None:
                d = 0
            else:
                d = max(v - prev[c], 0)
            row["d_" + c] = d
            row[c] = v
        rows.append(row)
        prev = st
    return rows


def summarize(session: Session) -> dict:
    rows = deltas(session.statuses)
    out: dict = {"intervals": len(rows)}
    if not rows:
        return out
    out["duration_s"] = rows[-1]["t"] - rows[0]["t"]
    out["channels"] = sorted({r["channel"] for r in rows if r["channel"] is not None})
    out["tunes"] = sum(1 for e in session.events if e.kind == "tune")
    locked = [r for r in rows if r["locked"]]
    out["locked_ratio"] = len(locked) / len(rows)
    mers = [r["signal_db"] for r in locked if isinstance(r.get("signal_db"), (int, float))]
    if mers:
        out["mer_min"] = min(mers)
        out["mer_median"] = statistics.median(mers)
        out["mer_max"] = max(mers)
    gains = [r["gain_db"] for r in rows if isinstance(r.get("gain_db"), (int, float))]
    if gains:
        out["gain_min"] = min(gains)
        out["gain_max"] = max(gains)
    out["systems"] = sorted({r["system"] for r in rows})
    for g in ("cfo_hz", "clock_ppm"):            # ISDB-C: 1 時間でのドリフトの幅
        vals = [r[g] for r in locked if isinstance(r.get(g), (int, float))]
        if vals:
            out[g + "_min"] = min(vals)
            out[g + "_max"] = max(vals)
    totals = {}
    bad_intervals = {}
    for c in CUMULATIVE:
        totals[c] = sum(r["d_" + c] for r in rows)
        bad_intervals[c] = sum(1 for r in rows if r["d_" + c] > 0)
    out["totals"] = totals
    out["bad_intervals"] = bad_intervals
    out["unlocked_intervals"] = [r["clock"] for r in rows if not r["locked"]]
    out["events"] = [
        (e.clock, e.text) for e in session.events
        if e.kind == "other" and not e.text.startswith("libiio loaded")
    ]
    return out


def parse_bontest(lines) -> dict:
    """bontest の統計行（--interval 付き）を集計する。"""
    rows = []
    for raw in lines:
        m = _BONTEST.match(raw.strip())
        if not m:
            continue
        h, mi, s, elapsed, mbps, level, pkts, bad, drop, ddrop, err, derr = m.groups()
        rows.append({
            "clock": f"{h}:{mi}:{s}", "elapsed": float(elapsed), "mbps": float(mbps), "level": float(level),
            "pkts": int(pkts), "bad": int(bad), "drop": int(drop), "d_drop": int(ddrop),
            "error": int(err), "d_error": int(derr),
        })
    out: dict = {"intervals": len(rows), "rows": rows}
    if rows:
        out["elapsed_s"] = rows[-1]["elapsed"]
        out["packets"] = rows[-1]["pkts"]
        out["bad_sync"] = rows[-1]["bad"]
        out["drop"] = sum(r["d_drop"] for r in rows)
        out["error"] = sum(r["d_error"] for r in rows)
        out["drop_intervals"] = [r["clock"] for r in rows if r["d_drop"] > 0]
        out["error_intervals"] = [r["clock"] for r in rows if r["d_error"] > 0]
        rates = [r["mbps"] for r in rows if r["mbps"] > 0]
        out["mbps_min"] = min(rates) if rates else 0.0
        out["mbps_median"] = statistics.median(rates) if rates else 0.0
    return out


def format_table(rows: list[dict], every: int = 1) -> str:
    cols = [
        ("clock", "time", "{}"), ("channel", "ch", "{}"), ("locked", "lock", "{}"),
        ("signal_db", "MER", "{:.1f}"), ("gain_db", "gain", "{}"), ("rx_level_dbfs", "dBFS", "{:.1f}"),
        ("bitrate", "Mbps", "{:.2f}"),
        ("d_rs_uncorrectable", "+rs", "{}"), ("d_hw_dropped", "+hwdrop", "{}"), ("d_frames_lost", "+lost", "{}"),
        ("d_underruns", "+undr", "{}"), ("d_sdr_reconnects", "+recon", "{}"), ("d_windows_failed", "+wfail", "{}"),
        ("d_splice_resets", "+splice", "{}"),
    ]
    if rows and rows[0].get("system") == "isdbc":    # ISDB-C は FPGA 前段・ウィンドウの列が無いので同期系に差し替える
        cols = [c for c in cols if c[0] not in ("d_frames_lost", "d_windows_failed", "d_splice_resets")]
        cols += [("d_sync_losses", "+sync", "{}"), ("d_reacquisitions", "+reacq", "{}"),
                 ("d_tsmf_bad_headers", "+tsmf", "{}"), ("clock_ppm", "ppm", "{:.2f}")]
    lines = ["  ".join(f"{name:>8}" for _, name, _ in cols)]
    for i, r in enumerate(rows):
        if every > 1 and i % every and i != len(rows) - 1:
            continue
        cells = []
        for key, _, fmt in cols:
            v = r.get(key)
            if key == "bitrate" and isinstance(v, (int, float)):
                v = v / 1e6
            if key == "locked":
                v = "Y" if v else "-"
            cells.append(f"{(fmt.format(v) if v is not None else '-'):>8}")
        lines.append("  ".join(cells))
    return "\n".join(lines)


def format_summary(s: dict, bt: dict | None = None) -> str:
    if not s.get("intervals"):
        return "status 行なし（ini の [Debug] Log=1 で 10 秒ごとに記録される）"
    L = []
    dur = s["duration_s"]
    L.append(f"区間数 {s['intervals']}  期間 {dur / 60:.1f} 分  選局 {s['tunes']} 回  チャンネル {s['channels']}")
    L.append(f"ロック率 {100 * s['locked_ratio']:.1f}%  非ロック区間 {len(s['unlocked_intervals'])}"
             + (f"（{', '.join(s['unlocked_intervals'][:8])}{' ...' if len(s['unlocked_intervals']) > 8 else ''}）"
                if s["unlocked_intervals"] else ""))
    if "mer_min" in s:
        L.append(f"MER（ロック中、最悪階層） min {s['mer_min']:.1f} / median {s['mer_median']:.1f} / max {s['mer_max']:.1f} dB")
    if "gain_min" in s:
        L.append(f"ソフト AGC ゲイン {s['gain_min']}〜{s['gain_max']} dB")
    if "cfo_hz_min" in s:
        L.append(f"ISDB-C 搬送波オフセット {s['cfo_hz_min']:.0f}〜{s['cfo_hz_max']:.0f} Hz"
                 f"  クロック偏差 {s['clock_ppm_min']:.2f}〜{s['clock_ppm_max']:.2f} ppm")
    grown = [(c, s["totals"][c], s["bad_intervals"][c]) for c in CUMULATIVE if s["totals"][c]]
    if grown:
        L.append("増えたカウンタ（合計 / 増えた区間数）:")
        for c, tot, n in grown:
            L.append(f"  {c:<18} {tot:>10}  {n} 区間")
    else:
        L.append("増えたカウンタなし（rs_uncorrectable / hw_dropped / frames_lost / underruns / reconnects すべて 0）")
    if s["events"]:
        L.append("その他のログ行:")
        for clock, text in s["events"][:20]:
            L.append(f"  {clock} {text[:120]}")
    if bt is not None:
        if bt.get("intervals"):
            L.append(f"bontest: {bt['elapsed_s'] / 60:.1f} 分  {bt['packets']} パケット  bad sync {bt['bad_sync']}"
                     f"  drop {bt['drop']}（{len(bt['drop_intervals'])} 区間）  error(TEI) {bt['error']}"
                     f"（{len(bt['error_intervals'])} 区間）  レート median {bt['mbps_median']:.2f} / min {bt['mbps_min']:.2f} Mbps")
            if bt["drop_intervals"]:
                L.append("  drop のあった時刻: " + ", ".join(bt["drop_intervals"][:12])
                         + (" ..." if len(bt["drop_intervals"]) > 12 else ""))
        else:
            L.append("bontest: 統計行なし（--interval N 付きで実行した出力を渡す）")
    return "\n".join(L)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", default="rt/build/bondriver-x64/Release/BonDriver_PlutoSDR_T.log")
    ap.add_argument("--bontest", help="bontest の標準出力を保存したファイル")
    ap.add_argument("--all", action="store_true", help="最後のセッションだけでなく全セッションを出す")
    ap.add_argument("--every", type=int, default=1, help="表を N 行ごとに間引く（10 秒間隔なので 6 = 1 分）")
    ap.add_argument("--no-table", action="store_true")
    args = ap.parse_args(argv)

    path = Path(args.log)
    if not path.exists():
        print(f"not found: {path}", file=sys.stderr)
        return 1
    sessions = parse_log(path.read_text(encoding="utf-8", errors="replace").splitlines())
    if not sessions:
        print("ログに BonDriver_PlutoSDR の行がない")
        return 1
    chosen = sessions if args.all else sessions[-1:]
    bt = None
    if args.bontest:
        bt = parse_bontest(Path(args.bontest).read_text(encoding="utf-8", errors="replace").splitlines())
    for i, sess in enumerate(chosen):
        if len(chosen) > 1:
            print(f"=== session {i + 1}/{len(chosen)} ({sess.events[0].clock if sess.events else '?'})")
        if not args.no_table and sess.statuses:
            print(format_table(deltas(sess.statuses), args.every))
            print()
        print(format_summary(summarize(sess), bt if sess is chosen[-1] else None))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
