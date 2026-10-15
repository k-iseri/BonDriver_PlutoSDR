"""BonDriver_BDA の .ch2（TransModulation = ケーブル TV の BS/CS 再送信）を Mirakurun の channels.yml に変換する。

    python tools/mirakurun/ch2_to_channels.py BonDriver_BDA.ch2 > channels.catv.yml

.ch2 の列: 名称, チューニング空間, チャンネル（BonDriver_BDA の通し番号 0〜112）, リモコン番号, , サービスID,
ネットワークID, TSID, 状態。通し番号を recpluto のチャンネル表記（UHF 帯は '56'、C 帯は 'C41'）にし、
TSID を commandVars.tsid（recpluto --tsid=<tsid>）で渡す。type は networkID で決める（4 = BS、6 / 7 = CS110、
それ以外 = SKY）。serviceId は書かない（Mirakurun が SDT から全サービスを拾う。serviceId を書くとそのサービスだけになる）。
"""
import argparse
import sys
from collections import OrderedDict
from pathlib import Path


def channel_label(index: int) -> str | None:
    """BonDriver_BDA の通し番号 → recpluto の表記。VHF（0〜11）は受信できないので None"""
    if 12 <= index <= 61:
        return str(13 + index - 12)
    if 62 <= index <= 71:
        return f"C{13 + index - 62}"
    if 72 <= index <= 112:
        return f"C{23 + index - 72}"
    return None


def channel_type(onid: int) -> str:
    if onid == 4:
        return "BS"
    if onid in (6, 7):
        return "CS"
    return "SKY"


def read_text(path: Path) -> str:
    data = path.read_bytes()
    for enc in ("utf-8-sig", "cp932"):
        try:
            return data.decode(enc)
        except UnicodeDecodeError:
            continue
    raise SystemExit(f"{path}: 文字コードが utf-8 / cp932 のどちらでもない")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ch2", type=Path)
    ap.add_argument("--space", type=int, default=0, help="対象のチューニング空間番号 (default 0)")
    args = ap.parse_args()

    groups: OrderedDict[tuple[int, int], dict] = OrderedDict()   # (index, tsid) → {name, onid, services}
    for raw in read_text(args.ch2).splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        cols = line.split(",")
        if len(cols) < 8:
            continue
        name, space, index, _remocon, _, sid, onid, tsid = cols[:8]
        try:
            space_i, index_i, sid_i, onid_i, tsid_i = int(space), int(index), int(sid), int(onid), int(tsid)
        except ValueError:
            continue
        if space_i != args.space:
            continue
        g = groups.setdefault((index_i, tsid_i), {"name": name, "onid": onid_i, "services": []})
        g["services"].append((sid_i, name))

    seen_channels: dict[tuple[str, str], int] = {}
    print("# BonDriver_BDA の .ch2 から tools/mirakurun/ch2_to_channels.py で生成（tuners.yml の command は")
    print("# `recpluto --type=<type> --tsid=<tsid> <channel> - -`）。name は最初のサービス名")
    for (index, tsid), g in groups.items():
        label = channel_label(index)
        if label is None:
            print(f"# 通し番号 {index}（{g['name']}）は VHF 帯で受信できないので省く", file=sys.stdout)
            continue
        ctype = channel_type(g["onid"])
        key = (ctype, label)
        if key in seen_channels:
            print(f"# 注意: {ctype}/{label} は TSID {seen_channels[key]} と {tsid} の 2 つの TS が同じ周波数にある。"
                  f" Mirakurun は type + channel で区別するので、2 つ目は tsmfRelTs 方式などで分ける", file=sys.stdout)
        seen_channels[key] = tsid
        services = ", ".join(f"{sid} {nm}" for sid, nm in g["services"])
        print(f"- name: {g['name']}")
        print(f"  type: {ctype}")
        print(f"  channel: '{label}'")
        print(f"  commandVars: {{ tsid: {tsid} }}      # onid {g['onid']}, services: {services}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
