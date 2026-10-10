#!/usr/bin/env python3
"""ref/isdbt/tables/*.py から rt/isdbt/isdbt_tables.h を生成する。

表の一次情報は ref/ 側（出典コメント付き）。C++ へは手で転記せずこのスクリプトで
機械生成し、生成結果はチェックインする。ref/tests/test_rt_tables.py が
「チェックイン済みヘッダ == 再生成結果」を検証するので、ref/ の表を変更したら
このスクリプトを再実行すること。
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ref"))

from isdbt import params as P

HEADER = ROOT / "rt" / "isdbt" / "isdbt_tables.h"


def fmt_array(name: str, values, ctype: str = "int16_t", per_line: int = 20) -> str:
    vals = list(values)
    lines = []
    for i in range(0, len(vals), per_line):
        lines.append("    " + ", ".join(str(v) for v in vals[i:i + per_line]) + ",")
    body = "\n".join(lines)
    return (f"inline constexpr std::array<{ctype}, {len(vals)}> {name} = {{{{\n{body}\n}}}};\n")


def generate() -> str:
    out = []
    out.append("// 自動生成ファイル。手で編集しない。\n")
    out.append("// 生成: python tools/gen_rt_tables.py（出典は ref/isdbt/tables/*.py の冒頭コメント）\n")
    out.append("// 検証: ref/tests/test_rt_tables.py が ref/ の表との一致を確認する。\n")
    out.append("#pragma once\n#include <array>\n#include <cstdint>\n\n")
    out.append("namespace isdbt::tables {\n\n")
    out.append("// キャリアランダマイズ表（STD-B31 表 3-13）。意味は ref/isdbt/tables/carrier_randomize.py\n")
    for mode in P.Mode:
        out.append(fmt_array(f"kCarrierRandomizeMode{mode.value}", P.CARRIER_RANDOMIZE_TABLE[mode]))
    out.append("\n// TMCC キャリア絶対位置（同期変調部、STD-B31 表 3-15）\n")
    for mode in P.Mode:
        out.append(fmt_array(f"kTmccAbsMode{mode.value}", P.TMCC_POSITIONS_ABS[mode]))
    out.append("\n// AC1 キャリア絶対位置（同期変調部、STD-B31 表 3-15）\n")
    for mode in P.Mode:
        out.append(fmt_array(f"kAc1AbsMode{mode.value}", P.AC1_POSITIONS_ABS[mode]))
    out.append("\n}  // namespace isdbt::tables\n")
    return "".join(out)


def main() -> None:
    text = generate()
    if "--check" in sys.argv:
        current = HEADER.read_text(encoding="utf-8") if HEADER.exists() else ""
        if current != text:
            raise SystemExit(f"{HEADER} が最新でない。python tools/gen_rt_tables.py で再生成すること")
        print("ok: isdbt_tables.h is up to date")
        return
    HEADER.parent.mkdir(parents=True, exist_ok=True)
    HEADER.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {HEADER}")


if __name__ == "__main__":
    main()
