"""rt/isdbt/isdbt_tables.h（自動生成ヘッダ）が ref/ の表と一致していることの検証。"""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def test_generated_header_is_up_to_date():
    spec = importlib.util.spec_from_file_location("gen_rt_tables", ROOT / "tools" / "gen_rt_tables.py")
    m = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = m
    spec.loader.exec_module(m)
    header = ROOT / "rt" / "isdbt" / "isdbt_tables.h"
    assert header.exists(), "rt/isdbt/isdbt_tables.h が無い（python tools/gen_rt_tables.py で生成）"
    assert header.read_text(encoding="utf-8") == m.generate(), \
        "isdbt_tables.h が ref/ の表と食い違っている。python tools/gen_rt_tables.py で再生成すること"
