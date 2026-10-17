"""tools/bonlog_report.py: BonDriver ログ / bontest 出力の長時間試験集計。"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from bonlog_report import deltas, format_summary, format_table, parse_bontest, parse_log


def _status(clock, **kw):
    base = {
        "ok": True, "locked": True, "signal_db": 24.0, "channel": 24, "rs_uncorrectable": 0, "dropped": 0,
        "underruns": 0, "frames_lost": 0, "hw_dropped": 0, "hw_ovf_events": 0, "sdr_reconnects": 0,
        "splice_resets": 0, "fpga_symbol_gaps": 0, "fpga_timeouts": 0, "windows_failed": 0,
        "gain_db": 30, "rx_level_dbfs": -24.0, "bitrate": 17267628.0,
    }
    base.update(kw)
    import json
    return f"{clock}.000 [BonDriver_PlutoSDR] live: {json.dumps(base)}"


LOG = [
    "23:59:40.000 [BonDriver_PlutoSDR] loaded; ini=x host=127.0.0.1 port=47188 spawn=0",
    "23:59:40.100 [BonDriver_PlutoSDR] libiio loaded from x",
    "23:59:41.000 [BonDriver_PlutoSDR] tune isdbt 24ch 539.143 MHz (standalone)",
    _status("23:59:50", locked=False, signal_db=10.0, rs_uncorrectable=3),
    _status("00:00:00", rs_uncorrectable=5, hw_dropped=100),            # 0 時またぎ
    _status("00:00:10", rs_uncorrectable=5, hw_dropped=100, signal_db=22.5),
    "00:00:12.000 [BonDriver_PlutoSDR] receive queue full; dropped 48128 bytes so far",
    _status("00:00:20", rs_uncorrectable=0, hw_dropped=0, sdr_reconnects=1, signal_db=23.5),  # 再接続でリセット
    "00:00:25.000 [BonDriver_PlutoSDR] closed (standalone)",
]


def test_parse_single_session_with_midnight_wrap():
    sessions = parse_log(LOG)
    assert len(sessions) == 1
    s = sessions[0]
    st = s.statuses
    assert len(st) == 4
    # 0 時またぎで時刻が単調増加のまま
    assert st[0].t < st[1].t < st[2].t < st[3].t
    assert abs((st[1].t - st[0].t) - 10.0) < 1e-6
    assert [e.kind for e in s.events].count("tune") == 1


def test_deltas_and_reset_handling():
    rows = deltas(parse_log(LOG)[0].statuses)
    assert [r["d_rs_uncorrectable"] for r in rows] == [0, 2, 0, 0]     # 負の差（リセット）は 0 扱い
    assert [r["d_hw_dropped"] for r in rows] == [0, 100, 0, 0]
    assert [r["d_sdr_reconnects"] for r in rows] == [0, 0, 0, 1]


def test_summary_text():
    from bonlog_report import summarize
    sess = parse_log(LOG)[0]
    s = summarize(sess)
    assert s["intervals"] == 4
    assert s["tunes"] == 1
    assert s["channels"] == [24]
    assert abs(s["locked_ratio"] - 0.75) < 1e-9
    assert s["mer_min"] == 22.5 and s["mer_max"] == 24.0
    assert s["totals"]["rs_uncorrectable"] == 2 and s["bad_intervals"]["rs_uncorrectable"] == 1
    assert s["totals"]["sdr_reconnects"] == 1
    assert any("receive queue full" in text for _, text in s["events"])
    text = format_summary(s)
    assert "ロック率 75.0%" in text
    assert "rs_uncorrectable" in text and "sdr_reconnects" in text
    table = format_table(deltas(sess.statuses))
    assert table.count("\n") == 4          # ヘッダ + 4 行
    assert "17.27" in table                 # bitrate は Mbps 表示


def test_summary_without_status_lines():
    from bonlog_report import summarize
    sess = parse_log(LOG[:3])[0]
    assert "status 行なし" in format_summary(summarize(sess))


def test_multiple_sessions_split_on_loaded():
    sessions = parse_log(LOG + LOG)
    assert len(sessions) == 2
    assert len(sessions[1].statuses) == 4


def _isdbc_status(clock, **kw):
    import json
    base = {
        "ok": True, "system": "isdbc", "locked": True, "signal_db": 30.0, "channel": 55, "cfo_hz": -612.9,
        "clock_ppm": 0.80, "ts_packets": 0, "rs_uncorrectable": 0, "sync_losses": 0, "reacquisitions": 0,
        "tsmf": {"frames": 10, "bad_headers": 0, "selected": 1, "streams": []}, "dropped": 0, "underruns": 0,
        "sdr_reconnects": 0, "hw_dropped": 0, "hw_ovf_events": 0, "gain_db": 28, "rx_level_dbfs": -23.7,
        "bitrate": 29162117.0,
    }
    base.update(kw)
    return f"{clock}.000 [BonDriver_PlutoSDR] live: {json.dumps(base)}"


def test_isdbc_counters_and_gauges():
    from bonlog_report import summarize
    log = [
        "08:00:00.000 [BonDriver_PlutoSDR] loaded; ini=x",
        "08:00:01.000 [BonDriver_PlutoSDR] tune isdbc 56ch 731.000 MHz (standalone)",
        _isdbc_status("08:00:10"),
        _isdbc_status("08:00:20", sync_losses=1, reacquisitions=1, cfo_hz=-600.0, clock_ppm=0.85,
                      tsmf={"frames": 20, "bad_headers": 2, "selected": 1, "streams": []}),
    ]
    sess = parse_log(log)[0]
    rows = deltas(sess.statuses)
    assert [r["d_sync_losses"] for r in rows] == [0, 1]
    assert [r["d_tsmf_bad_headers"] for r in rows] == [0, 2]
    assert rows[0]["d_frames_lost"] == 0          # 地デジ専用カウンタが無くても落ちない
    s = summarize(sess)
    assert s["systems"] == ["isdbc"]
    assert s["totals"]["reacquisitions"] == 1 and s["totals"]["tsmf_bad_headers"] == 2
    assert s["cfo_hz_min"] == -612.9 and s["clock_ppm_max"] == 0.85
    text = format_summary(s)
    assert "クロック偏差 0.80〜0.85 ppm" in text and "tsmf_bad_headers" in text
    table = format_table(rows)
    assert "+sync" in table and "+reacq" in table and "+wfail" not in table


def test_parse_bontest():
    lines = [
        "tuned 24ch (space 0 ch 11) for 3600 s",
        "00:10:10    10.0 s   17.268 Mbps  level  24.1  pkts 114800  bad 0  drop 0 (+0)  error 0 (+0)",
        "00:10:20    20.1 s   17.270 Mbps  level  24.0  pkts 229600  bad 0  drop 3 (+3)  error 1 (+1)",
        "00:10:30    30.1 s   17.265 Mbps  level  23.9  pkts 344400  bad 0  drop 3 (+0)  error 1 (+0)",
        "total 64747200 bytes, 344400 packets, 0 bad sync, 3 drop, 1 error (TEI), 0 scrambled",
    ]
    bt = parse_bontest(lines)
    assert bt["intervals"] == 3
    assert bt["packets"] == 344400 and bt["bad_sync"] == 0
    assert bt["drop"] == 3 and bt["drop_intervals"] == ["00:10:20"]
    assert bt["error"] == 1 and bt["error_intervals"] == ["00:10:20"]
    assert abs(bt["mbps_median"] - 17.268) < 1e-9
    text = format_summary({"intervals": 1, "duration_s": 30, "tunes": 1, "channels": [24], "locked_ratio": 1.0,
                           "unlocked_intervals": [], "totals": {c: 0 for c in __import__("bonlog_report").CUMULATIVE},
                           "bad_intervals": {c: 0 for c in __import__("bonlog_report").CUMULATIVE}, "events": []}, bt)
    assert "drop 3（1 区間）" in text
