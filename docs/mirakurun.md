# Mirakurun から使う（Linux / macOS / Raspberry Pi）

isdbtd の `--stdout` モードは recpt1 などと同じ「選局して TS を標準出力に流し、SIGTERM で止まる」
チューナーコマンドなので、Mirakurun の `tuners.yml` にそのまま書ける。BonDriver（Windows / TVTest）とは
独立で、同じ復調コアを使う。状態: 2026-10-15 に Windows と WSL（Ubuntu 22.04）で録音ファイル
（`--sigmf`）からの動作を確認。Mac / Raspberry Pi の実機と Mirakurun 本体との結合は未確認。

## 準備

1. libiio 1.x（`--sdr` に必要）。Homebrew に formula は無く、apt の libiio-dev は 0.x 系で API が
   違うので使えない。macOS / Ubuntu / Raspberry Pi OS とも
   [libiio](https://github.com/analogdevicesinc/libiio) v1 をソースからビルドする（macOS の依存は
   `brew install cmake pkgconf libusb zstd`、Ubuntu は `apt install cmake pkg-config libusb-1.0-0-dev
   libxml2-dev libzstd-dev`）:
   ```
   git clone --depth 1 https://github.com/analogdevicesinc/libiio.git && cd libiio
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/homebrew \
         -DOSX_FRAMEWORK=OFF -DOSX_PACKAGE=OFF -DWITH_USB_BACKEND=ON -DWITH_NETWORK_BACKEND=ON \
         -DWITH_SERIAL_BACKEND=OFF -DWITH_IIOD=OFF -DWITH_EXAMPLES=OFF -DWITH_TESTS=OFF
   cmake --build build -j && cmake --install build      # Linux は PREFIX を /usr/local にして sudo
   iio_info --version                                   # "Libiio version: 1.x ... backends: ip usb xml"
   ```
   `-DWITH_TESTS=ON`（libiio 自身の単体テスト）は macOS で `ar` が失敗するので付けない。
   macOS では dylib の install_name が `@rpath/libiio.1.dylib`、実行ファイルの rpath が framework
   配置向けのままで `iio_info` が `Library not loaded: @rpath/libiio.1.dylib` で落ちる。絶対パスに直す:
   ```
   install_name_tool -id /opt/homebrew/lib/libiio.1.dylib /opt/homebrew/lib/libiio.1.0.0.dylib
   codesign -f -s - /opt/homebrew/lib/libiio.1.0.0.dylib
   for b in /opt/homebrew/bin/iio_*; do
     install_name_tool -change @rpath/libiio.1.dylib /opt/homebrew/lib/libiio.1.dylib "$b"; codesign -f -s - "$b"
   done
   ```
   以降にリンクする isdbtd には絶対パスが記録されるので、この手当ては一度で済む。
2. `make rt` で `rt/build/rt-native/isdbtd` を作り、PATH の通る場所に置く。
3. Pluto を USB で繋ぎ、`iio_info -u usb:` で見えることを確認（Linux は udev ルールで一般ユーザーに許可）。
4. 動作確認（TS がファイルに出て、Ctrl-C で止まる）:
   ```
   isdbtd --sdr usb: --ch 24 --stdout > ch24.ts          # 地デジ UHF 24ch
   isdbtd --sdr usb: --system isdbc --ch 55 --stdout > catv.ts   # CATV トラモジ（BonDriver_BDA の通し番号 55 = 56ch）
   ```
   `--quiet` を付けないと 10 秒ごとに status（JSON）が stderr に出る。

## tuners.yml / channels.yml（Mirakurun 4.0 以降）

Pluto は 1 台で 1 チャンネルなので、地デジと CATV を同じ 1 本の tuner にする（Mirakurun は tuner ごとに
同時 1 プロセス）。方式の切り替えは channels.yml の `commandVars`（4.0 で追加。`<name>` が command の
同名プレースホルダに入る）で渡す。`<channel>` は channels.yml の `channel` がそのまま入る。

```yaml
# tuners.yml
- name: PlutoSDR
  types: [GR, BS, CS]
  command: "isdbtd --sdr usb: --system <system> --ch <channel> --stdout --quiet"   # "usb:" を含むので引用符必須
  decoder: arib-b25-stream-test     # スクランブル解除は本リポジトリの範囲外。要らなければ省略
  isDisabled: false
```
```yaml
# channels.yml（commandVars.system は全チャンネルに書く）
- name: NHK総合
  type: GR
  channel: '27'          # UHF 物理チャンネル
  commandVars: { system: isdbt }
- name: NHK BS
  type: BS
  channel: '55'          # CATV トラモジ。BonDriver_BDA の TransModulation と同じ通し番号（0〜112）
  serviceId: 101
  commandVars: { system: isdbc }
```

CATV は 1 周波数に複数 TS が多重（TSMF）されている局がある。isdbtd は既定で最小番号の TS を選んで
188 バイト TS にして出す。Mirakurun 側の `tsmfRelTs`（1〜15）で選ばせたい場合は command に
`--tsmf all` を足して全スロットを通す（どちらか一方で選ぶ）。VHF 1〜12ch は AD9363 の LO 範囲外で
受信できない。

## 既知の制約・未確認

- 選局から TS が出るまで約 5 秒（FPGA 前段ありでも同程度）。Mirakurun の既定タイムアウトで足りるはず。
- CPU: x86-64 は AVX2 版（地デジ raw 約 2.7 コア、FPGA 前段で約 0.3 コア、CATV 約 0.35 コア）。
  arm64（Apple Silicon / Raspberry Pi）はスカラー版で、Raspberry Pi 4/5 で実時間に足りるかは未測定。
  足りなければビタビ・RS の NEON 化（SIMDe で AVX2 版を変換）が次の手。
- FPGA 前段（pluto-fw-isdbt v2）は libiio 経由のレジスタアクセスなので OS を問わず使える。
- USB 2.0 帯域: 8bit ファームなら公称レート、純正ファームなら 7/8 レート（Windows と同じ）。
