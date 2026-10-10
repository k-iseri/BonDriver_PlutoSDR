# Pluto 互換機メモ（未検証）

- 接続: `usb:` または `ip:192.168.2.1`（USB-Ethernet gadget）。Pluto+/LibreSDR は実 GbE で `ip:`。
- 周波数拡張: AD9363 の正規範囲（325 MHz〜3.8 GHz）に地デジ UHF は収まるため、
  `ad9364 互換モード` へのハックは不要。
- USB 2.0 での連続受信は 4〜6 MSPS が実用上限との報告が多い。8.127 MSPS で落ちる場合:
  1. `--fs 7000000` で録音し、`ref/` 側でリサンプル（隣接チャンネル折り返しに注意、下限 ≈ 7 MSPS）
  2. FPGA で 8 bit パッキング → **tezuka_fw を採用**（自前 HDL 改造はしない）。導入手順・
     実機確認項目は `fw-8bit.md`、ホスト側は `pack8.py` と `tools/iq_record.py --bits 8`
  3. GbE 付きボードへ移行
- サンプル落ち検出: 録音の GI 相関ピーク間隔が一定でなければ落ちている（tools/iq_inspect.py）
- 外部基準: 互換機のクロック精度は不明。長時間安定が要るなら 40 MHz TCXO 交換か外部基準。
