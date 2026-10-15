# Mirakurun から使う（Linux / macOS / Raspberry Pi）

Mirakurun にはドライバ API が無く、「選局して TS を標準出力に流し、SIGTERM で止まる」コマンドがチューナー
ドライバに相当する。このリポジトリの専用コマンドが `recpluto`（`rt/mirakurun/recpluto.cpp`、recpt1 と同じ
引数 `<channel> <rectime> <destfile>`）で、Pluto SDR を開いて BonDriver / isdbtd と同じ復調パイプライン
（`rt/demod/live_switch`）で地デジとケーブル TV の BS/CS 再送信（ISDB-C）を受ける。`isdbtd --stdout` でも
同じことができるが、Mirakurun からは recpluto を使う。
状態: 2026-10-15〜16 に MacBook Air（M5）+ Pluto で recpluto の録画・stdout（SIGTERM で終了）・走査と、Mirakurun
4.1.5 からのストリーム取得・視聴・EPG 取得まで確認。Windows は BonDriver / TVTest（docs/setup-tvtest.md）で別途
確認済み（Mirakurun は Windows 非対応）。WSL（Ubuntu 22.04）は isdbtd の録音ファイル再生のみ確認。Raspberry Pi は未確認。

最短手順（それぞれの詳細は下の節）:
1. libiio 1.x をソースからビルド → `make rt` → `make install-rt`（Linux は sudo、macOS は `PREFIX=/opt/homebrew`）
2. Linux は udev ルール（`tools/mirakurun/99-plutosdr.rules`）、`iio_info -u usb:` で Pluto が見えることを確認
3. `recpluto 27 10 test.ts` で TS が出ることを確認、`recpluto --scan gr:13-52` で受信できる局を調べる
4. tuners.yml（`tools/mirakurun/tuners.yml.sample`）と channels.yml（`tools/mirakurun/channels.yml.sample`）を書く
5. Mirakurun を起動して `curl http://127.0.0.1:40772/api/channels/GR/27/stream?decode=0 | head -c 1000000 > /dev/null`
   などでストリームが取れることを確認。EPG は起動後に自動で集まる

## 準備

**ビルド済みバイナリ**: [GitHub Releases](https://github.com/k-iseri/BonDriver_PlutoSDR/releases) の
`recpluto-<版>-linux-x86_64.tar.gz` / `-linux-arm64.tar.gz`（Raspberry Pi 4/5 の 64bit OS）/ `-macos-arm64.tar.gz`
（Apple Silicon）に recpluto と isdbtd、同梱の libiio（lib/）、雛形が入っている（v1.5.1 以降）。展開してそのまま使える
（Linux は `libusb-1.0-0` と `libxml2` が要る。通常は入っている。macOS は初回に
`xattr -d com.apple.quarantine recpluto isdbtd lib/*.dylib`）。Windows 版の recpluto.exe は BonDriver の zip に同梱。
自分でビルドする場合は以下。

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
2. `make rt` で `rt/build/rt-native/recpluto`（と isdbtd）を作り、`make install-rt`（Linux は `sudo`、macOS は
   `PREFIX=/opt/homebrew`）で PATH に置く。
3. Pluto を USB で繋ぎ、`iio_info -u usb:` で見えることを確認。Linux は `tools/mirakurun/99-plutosdr.rules` を
   /etc/udev/rules.d/ に置いて一般ユーザー（Mirakurun の実行ユーザー）から開けるようにする。
4. 動作確認（TS がファイルに出る。`-` は止めるまで / 標準出力）:
   ```
   recpluto 24 10 ch24.ts             # 地デジ UHF 24ch を 10 秒
   recpluto --type=BS 56 - - > catv.ts   # CATV トラモジ（BS/CS 再送信）のケーブル UHF 帯 56ch、Ctrl-C で止める
   recpluto C41 10 catv.ts            # 同じく C 帯 C41（C 付きなら --type 無しでもケーブル）
   recpluto --scan gr:13-52           # 受信できるチャンネルを channels.yml の形で出す（catv / all も可、1 ch 約 5 秒）
   ```
   `--quiet` を付けないと 10 秒ごとに status（JSON）が stderr に出る。recpt1 の `--b25` `--strip` `--sid` などは
   受け付けて無視する（スクランブル解除は Mirakurun の decoder に任せる）。`--device ip:192.168.2.1` で
   Pluto+ / LibreSDR の LAN、`--fpga-eq` `--gain` `--agc-target` `--tsmf` は isdbtd と同じ。
   isdbtd 自体も `--sdr usb: --ch 24 --stdout` で同じ TS を出す（`--sigmf` / `--ts` の再生は `--loop` 無しなら
   末尾まで出して終了する）。

## tuners.yml / channels.yml（Mirakurun 4.0 以降）

Pluto は 1 台で 1 チャンネルなので、地デジと CATV を同じ 1 本の tuner にする（Mirakurun は tuner ごとに
同時 1 プロセス）。`<channel>` は channels.yml の `channel`、`<type>` は type（GR / BS / CS / SKY）がそのまま入り、
recpluto は type が GR なら地デジ、それ以外ならケーブル（ISDB-C）として受ける。雛形は `tools/mirakurun/` にある。

```yaml
# tuners.yml（tools/mirakurun/tuners.yml.sample）
- name: PlutoSDR
  types: [GR, BS, CS, SKY]
  command: recpluto --type=<type> --tsid=<tsid> <channel> - -
  decoder: arib-b25-stream-test     # スクランブル解除は本リポジトリの範囲外。要らなければ省略
  isDisabled: false
```
```yaml
# channels.yml（tools/mirakurun/channels.yml.sample）
- name: NHK総合
  type: GR
  channel: '27'          # UHF 物理チャンネル。Mirakurun の走査 API で作れる
- name: ＮＨＫ　ＢＳ
  type: BS
  channel: '56'          # ケーブルの UHF 帯 56ch（BS/CS 再送信）。TS は TSID で選ぶ（下の「CATV を見る」）
  commandVars: { tsid: 16625 }
```
`--device ip:...` のように `:` を含む引数を command に書くときは行全体を引用符で囲む（js-yaml が mapping と
解釈して起動に失敗する）。isdbtd を使う場合は `"isdbtd --sdr usb: --system <system> --ch <channel> --stdout --quiet"`
と channels.yml の `commandVars: { system: isdbt | isdbc }`（CATV の ch は BonDriver_BDA と同じ通し番号 0〜112）。

ファーム pluto-fw-isdbt v2.0.1 以前では弱い入力で FPGA 前段の MER が raw より落ちる（相関器のオフセット。
v2.0.2 で修正）ので、その場合は command に `--fpga-eq 0`（raw 経路で復調）を足す。
CPU は Apple M5（NEON、2026-10-15）で FPGA 前段約 0.2 コア、raw 経路（`--fpga-eq 0`）約 0.5 コア（top で実測。recpluto も同じ）。

## CATV（ケーブルの BS/CS 再送信、ISDB-C）を見る

ケーブル TV の BS/CS 再送信（ISDB-C トラモジ）は、局によって C 帯（C13〜C63、111〜465 MHz）と UHF 帯
（13〜62ch、473 MHz〜。地デジと同じ番号だが変調は 64QAM の ISDB-C で、周波数に 1/7 MHz のオフセットが無い）の
両方に置かれる。recpluto は数字のチャンネルを既定では地デジと解釈するので、Mirakurun の `<type>`（GR = 地デジ、
BS / CS / SKY = ケーブル）を `--type=<type>` で渡して方式を決める。1 周波数に複数の TS が多重（TSMF）されて
いることがあるので、TS は `--tsid=<tsid>`（channels.yml の `commandVars.tsid`、TS_id）で選ぶ。
状態: 2026-10-16 に Mac M5 + Pluto をケーブル端子につなぎ、NHK BS（UHF 帯 56ch）と WOWOW ライブ（C41）の録画
（同期エラー 0、MER 約 30 dB）と、Mirakurun 4.1.5 の API からの BS ストリーム取得を確認。

1. **配線とゲイン**: Pluto の RX をケーブル TV の宅内端子につなぐ。ゲインは既定のソフト AGC（帯域内 −24 dBFS）
   のまま。ケーブルは全チャンネルが強く、`--gain hw`（AD936x の AGC）ではフロントエンドが圧縮して MER が
   20 dB に張り付く（docs/hw-setup.md）。C13〜C22（111〜165 MHz）は AD9363 の LO 範囲より下で未確認。
2. **チャンネル表（BonDriver_BDA の .ch2）があるなら変換する**: TVTest 用の BonDriver_BDA.ch2（TransModulation
   空間）には通し番号・TSID・networkID が入っているので、そのまま channels.yml にできる:
   ```
   python tools/mirakurun/ch2_to_channels.py BonDriver_BDA.ch2 >> channels.yml
   ```
   通し番号は recpluto の表記（UHF 帯 '56'、C 帯 'C41'）に、networkID は type（4 = BS、6 / 7 = CS、他 = SKY）に、
   TSID は `commandVars: { tsid: N }` になる。serviceId は書かない（Mirakurun が SDT から全サービスを拾う。
   書くとそのサービスだけになる）。
3. **表が無いなら探す**: `recpluto --scan catv`（C13〜C63 と UHF 帯 13〜62 を ISDB-C で走査。`catv:C23-C63` /
   `catv:50-60` で範囲）でロックするチャンネルを出し、`--quiet` 無しで数十秒受けて 10 秒ごとの status（JSON）の
   `streams`（`rel`・`tsid`・`onid`。onid 4 = BS、6・7 = CS110）から TS を決める:
   ```
   recpluto --type=BS 56 25 /dev/null
   ```
4. **tuners.yml**（`tools/mirakurun/tuners.yml.sample`）: types に BS / CS / SKY を足し、`--type=<type> --tsid=<tsid>`
   を渡す。未定義のプレースホルダは空文字になるので `=` で繋いだ形にする（`--tsid=` は自動 = 最小番号の TS）。
   BS/CS はスクランブルされているので decoder が要る（スクランブル解除は本リポジトリの範囲外）:
   ```yaml
   - name: PlutoSDR
     types: [GR, BS, CS, SKY]
     command: recpluto --type=<type> --tsid=<tsid> <channel> - -
     decoder: arib-b25-stream-test
     isDisabled: false
   ```
5. **channels.yml**（`tools/mirakurun/channels.yml.sample`）:
   ```yaml
   - name: NHK総合
     type: GR
     channel: '27'
   - name: ＮＨＫ　ＢＳ
     type: BS
     channel: '56'          # ケーブルの UHF 帯 56ch（731 MHz）。type が BS なので ISDB-C
     commandVars: { tsid: 16625 }
   - name: ＷＯＷＯＷライブ
     type: BS
     channel: 'C41'         # C 帯 C41（333 MHz）
     commandVars: { tsid: 17488 }
   ```
   同じ周波数に TS が 2 つ以上ある場合、Mirakurun は type + channel でチャンネルを区別するので 1 項目しか書けない。
   その場合は command を `recpluto --type=<type> --tsmf all <channel> - -` にして全スロットを通し、channels.yml の
   `tsmfRelTs`（相対 TS 番号 1〜15）で Mirakurun に選ばせる。
6. **確認**: Mirakurun を再起動すると、サービスの無いチャンネルを順に選局して SDT からサービスを拾う（1 チャンネル
   10〜20 秒）。`curl -s "http://127.0.0.1:40772/api/channels/BS/56/stream?decode=1" | head -c 5000000 > test.ts`
   で TS が取れればよい。あとは EPGStation などから普通に選べる。

VHF 1〜12ch（地デジ以前の帯域）は AD9363 の LO 範囲外で受信できない。

## Mirakurun 本体を動かす（Docker / PM2 無し。macOS / Linux / Raspberry Pi）

Mirakurun 4.x は CLI と PM2 連携を廃止し Linux では Docker を推奨しているが、公式コンテナには recpluto も
libiio も無く、macOS の Docker Desktop からは USB の Pluto に届かない。そこで Node 22 / 24 で `lib/server.js`
を直接起動し、設定と DB の場所を環境変数で指定する（既定は `/usr/local/etc/mirakurun` と
`/usr/local/var/db/mirakurun` で root が要る）。以下は macOS で確認した手順で、Linux / Raspberry Pi も同じ
（常駐は `tools/mirakurun/mirakurun.service.sample`、macOS のログイン時起動は
`tools/mirakurun/com.mirakurun.server.plist.sample`。どちらも雛形で未検証）。
グローバルインストールでなければ postinstall は何もしない:
```
mkdir -p ~/mirakurun && cd ~/mirakurun && npm init -y && npm install mirakurun
mkdir -p config data/logo-data run log     # config/{server,tuners,channels}.yml を置く
```
```bash
# ~/mirakurun/start.sh
D="$HOME/mirakurun"
export SERVER_CONFIG_PATH="$D/config/server.yml" TUNERS_CONFIG_PATH="$D/config/tuners.yml"
export CHANNELS_CONFIG_PATH="$D/config/channels.yml" SERVICES_DB_PATH="$D/data/services.json"
export PROGRAMS_DB_PATH="$D/data/programs.json" LOGO_DATA_DIR_PATH="$D/data/logo-data"
cd "$D/node_modules/mirakurun" && exec node --max-semi-space-size=64 -r source-map-support/register lib/server.js
```
server.yml の `path`（UNIX ソケット）は `/var/run` ではなく書ける場所（例 `~/mirakurun/run/mirakurun.sock`）にする。
止めるときは `pkill -f "Mirakurun: Server"`（process.title を変えるので `lib/server.js` では一致しない）。
二重起動すると両方が Pluto を取りに行き、後発の isdbtd が `Unable to claim interface: Permission denied`
で即終了して Mirakurun が 100 ms ごとに再起動を繰り返すので、起動前に必ず止める。
チャンネルは `curl -X PUT "http://127.0.0.1:40772/api/config/channels/scan?type=GR&minCh=13&maxCh=52&refresh=true"`
で走査して channels.yml に書かせ、再起動で反映する（1 チャンネル約 10 秒）。

## 既知の制約・未確認

- 選局から TS が出るまで約 5 秒（FPGA 前段ありでも同程度）。Mirakurun の既定タイムアウトで足りるはず。
- CPU: x86-64 は AVX2 版（地デジ raw 約 2.7 コア、FPGA 前段で約 0.3 コア、CATV 約 0.35 コア）。
  arm64 は FEC（ビタビ・RS）と分数リサンプルが NEON 版（2026-10-15）で、Apple M5 の実機で raw 約 0.5 コア、
  FPGA 前段約 0.2 コア（それ以前のスカラー版は raw 約 1.8 コア、FPGA 前段約 1 コア）。`rt_test --bench` で
  スカラー比 float ビタビ 2.4 倍、8bit ビタビ 6 倍、RS 5.7 倍。raw 経路の内訳（60 秒で約 32 秒の CPU）は
  ビタビ 7、リサンプル+CFO 5、FFT 2、チャネル推定 1.6、デインターリーブ 1.4、デマップ 1.9 秒ほど
  （`ISDBT_PROFILE=1` の `[profile] symbols / eq / deint` 行と層ごとの FEC 行）。
  Raspberry Pi 4/5 は未測定（`rt/build/rt-native/rt_test --bench` で FEC 段だけ先に測れる。raw 経路が足りなければ
  FPGA 前段ファーム（pluto-fw-isdbt v2）を使うのが先で、ホスト側の次の候補は FFT・等化の NEON 化）。
- FPGA 前段（pluto-fw-isdbt v2）は libiio 経由のレジスタアクセスなので OS を問わず使える（v2.0.2 以降を推奨）。
- USB 2.0 帯域: 8bit ファームなら公称レート、純正ファームなら 7/8 レート（Windows と同じ）。
