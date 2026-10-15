# isdbt-sdr

汎用 SDR ボード（Pluto SDR 互換機など）で日本の地上デジタル放送（ISDB-T）と、ケーブルテレビの
BS/CS トランスモジュレーション（ISDB-C、J.83 Annex C 64QAM + TSMF）を受信し、
PC 上のソフトウェアで復調して MPEG-2 TS を取り出すプロジェクト。

## 構成
- `ref/` — Python 参照実装（正しさ優先）とテスト。`ref/isdbt/` 地デジ、`ref/isdbc/` ケーブル TV トラモジ
- `rt/` — C++ 実装。`rt/isdbt/` / `rt/isdbc/` 復調コア、`rt/demod/` 復調デーモン isdbtd と
  ライブ受信（LiveDemod / LiveIsdbc / LiveSwitch）、`rt/bondriver/` は TVTest / EDCB 用 BonDriver
  （`_T` = 地デジのみ、`_TC` = 地デジ + CATV）、`rt/tests/` は ref/ との一致テスト
- `hw/` — ボード固有の設定・録音スクリプト・将来の HDL 改変
- `tools/` — 録音、I/Q 解析、受信品質レポート、TS 検査。`fe_*.py` は FPGA 前段（pluto-fw-isdbt v2）の実機検証
  （fe_fft_test: FFT テストモード、fe_sync_test: 同期、fe_carrier_demod: 同期 FFT 出力 → ref/ 後段、
  fe_eq_model: 等化器のアルゴリズムモデル、fe_eq_test: モード 4 の取り込みと ref/ 後段での MER / RS / TS 一致）
- `docs/` — 仕様ノート、設計、判断記録
- `testdata/` — 合成信号と実波録音（gitignore）

## はじめかた
```
make setup        # Windows で make が無い場合: .\dev.ps1 setup
make test-fast    #                             .\dev.ps1 test-fast
make synth        # 合成試験信号を testdata/synthetic/ に生成（約 10 秒）
python tools/demod_sigmf.py testdata/synthetic/std_mode3_gi8_clean.sigmf-meta --out out/   # 復調して TS を得る
```
Python 3.11 以上。開発は Windows ネイティブ環境で行っているが、`ref/` と `rt/`（復調コア・isdbtd）は
Linux / macOS でもビルド・実行できる（下記「Linux / macOS」）。TVTest 用 BonDriver だけが Windows 専用。
`make test`（ループバック・BER 曲線を含む slow テスト）は 10 分程度かかる。
ref/ の復調は 6 フレーム単位のブロック処理で、速度は実時間の約 14 倍遅、メモリは録音データ
（complex64、30 秒で約 2 GB）+ 数百 MB。C++ 版（`make rt` → isdbtd.exe）はライブ受信で CPU 約 2.7 コア
（raw I/Q、ホスト全段復調）。pluto-fw-isdbt v2 の FPGA 前段を使うと地デジ約 0.3 コア、CATV（ISDB-C）は raw のまま約 0.35 コア（i7-13700F）。

長時間・ドリフト・瞬断のある録音は `--track`（`demod_sigmf.py` / `isdbtd`）で追従復調。
受信品質の切り分けは `python tools/rx_report.py <録音>.sigmf-meta --plot out/r`
（Mode/GI ブラインド判定、MER、キャリア別 MER、遅延プロファイル）。
204 バイト放送 TS（BTS）が要る場合は `demod_sigmf.py --bts`。

## TVTest / EDCB で使う（BonDriver）

ビルド済みの配布 zip は [GitHub Releases](https://github.com/k-iseri/BonDriver_PlutoSDR/releases) にある
（`v*` タグの push で GitHub Actions が自動ビルド・添付する。手順は `rt/bondriver/README.md`）。
インストール方法は `docs/setup-tvtest.md`（zip の中身を TVTest の BonDriver フォルダへコピーして
ini.sample をリネームするだけ）。Pluto に [pluto-fw-isdbt](https://github.com/k-iseri/pluto-fw-isdbt) v2 を書いてあれば
地デジの同期・FFT・等化を FPGA で行い（`[SDR] FpgaEq=auto` で自動判別）、PC 側は約 0.3 コアで済む。自分でビルドする場合:
```
make bondriver                                   # または .\dev.ps1 bondriver（VS 2022 Build Tools が必要）
make rt                                          # 復調デーモン isdbtd.exe と一致テスト rt_test.exe
rt\build\rt-x64\Release\isdbtd.exe --sigmf <録音>.sigmf-meta --loop    # C++ 復調デーモン（MER を status に返す）
python tools/demod_server.py --ts C:\rec\x.ts --loop                   # Python の偽デーモン（録画 TS / SigMF 再生）
rt\build\bondriver-x64\Release\bontest.exe rt\build\bondriver-x64\Release\BonDriver_PlutoSDR_T.dll 27 5
```
DLL と同名の ini（`.ini.sample` を編集）を TVTest の BonDriver フォルダへ置く。
視聴までの一般ユーザー向け手順は `docs/setup-tvtest.md`。
詳細は `rt/bondriver/README.md`、プロトコルは `docs/daemon-protocol.md`。

## Linux / macOS（Apple Silicon 含む）
`rt/` は CMake だけでビルドできる（C++17、g++ 11 / clang 15 以降）。AVX2 は x86-64 で自動で付き、
arm64 は NEON 版（ビタビ・RS。結果は同じ）になる。SDR ライブ受信には libiio 1.x が要る（Homebrew / apt には
1.x が無いのでソースからビルドする。手順は `docs/mirakurun.md`。無ければ `--sdr` だけ使えないままビルドは通る）。
`make setup` には Python 3.11 以上の `python3` が必要（`python3 --version` で確認）。
```
make setup && make test-fast            # ref/ のテスト
make rt                                 # rt/build/rt-native/{rt_test, isdbtd, recpluto}
make rt-vectors && make rt-test         # ref/ とのビット一致（46 項目）
rt/build/rt-native/isdbtd --sigmf testdata/captured/ch25.sigmf-meta --loop   # 録音の復調（TCP 47188 に TS）
rt/build/rt-native/isdbtd --sdr usb: --ch 24                                 # Pluto ライブ受信
```
GitHub Actions（`.github/workflows/ci.yml`）が ubuntu と macOS（arm64）で rt_test と isdbtd の
プロトコル試験を回す。Raspberry Pi（arm64 Linux）は同じ手順でビルドできる想定だが実機未確認。

## Mirakurun で使う（Linux / macOS / Raspberry Pi）
Mirakurun にはドライバ API が無く、recpt1 のような「選局して TS を標準出力に流すコマンド」がチューナーに
なる。このリポジトリでは `recpluto`（recpt1 互換の引数 `<channel> <rectime> <destfile>`）がそれで、地デジと
ケーブル TV の BS/CS 再送信（ISDB-C）を 1 本の tuner として扱える。詳細と Mirakurun 本体の起動方法は
`docs/mirakurun.md`、雛形は `tools/mirakurun/`。
1. libiio 1.x をソースからビルド（`docs/mirakurun.md`「準備」）、`make rt`、`make install-rt`
   （Linux は `sudo make install-rt`、macOS は `make install-rt PREFIX=/opt/homebrew`）
2. Linux は `tools/mirakurun/99-plutosdr.rules` を /etc/udev/rules.d/ に置いて Pluto の USB を一般ユーザーに許可
3. 動作確認: `recpluto 27 10 test.ts`（10 秒録画）、`recpluto --scan gr:13-52`（受信できる局を channels.yml の形で表示）
4. tuners.yml に 1 行（`command: recpluto <channel> - -`。`tools/mirakurun/tuners.yml.sample`）。channels.yml は
   地デジなら Mirakurun の走査 API、CATV なら `recpluto --scan catv` の出力（`channel: 'C56'` のように C 付き）
5. Mirakurun を起動（4.x は Docker 無しでも `lib/server.js` を直接起動できる。手順と常駐の雛形は `docs/mirakurun.md`）

MacBook Air M5 + Pluto で Mirakurun 4.1.5 からの視聴・EPG 取得まで確認済み（CPU は FPGA 前段で約 0.2 コア）。
Raspberry Pi は未確認。

## ケーブルテレビの BS/CS（トランスモジュレーション、ISDB-C）
ケーブル TV の宅内端子に来ている BS/CS 再送信（64QAM、5.274 Mbaud、TSMF 多重）は
`BonDriver_PlutoSDR_TC.dll` のチューニング空間 CATV で受信する（チャンネル番号は BonDriver_BDA の
TransModulation と同じ通し番号なので `.ch2` を流用できる）。仕様ノートは `docs/isdbc-spec-notes.md`。
```
python tools/iq_record.py --freq 731e6 --fs 7111111 --bw 6e6 --gain 30 --sec 5 --out testdata/captured/catv_56ch   # 録音
python tools/isdbc_demod_sigmf.py testdata/captured/catv_56ch.sigmf-meta --out out/                                # 復調 → TS
rt\build\rt-x64\Release\isdbtd.exe --sdr usb: --system isdbc --ch 55                                               # ライブ（56ch）
rt\build\bondriver-x64\Release\bontest.exe rt\build\bondriver-x64\Release\BonDriver_PlutoSDR_TC.dll 1/55 10 out.ts
```
受信ゲインに注意: ケーブル端子は全チャンネルが強く、AD936x 内蔵 AGC ではフロントエンドが圧縮して
MER が 20 dB に張り付く。既定のソフトウェア AGC（帯域内 −24 dBFS）はこれを避ける（`docs/hw-setup.md`）。

## 実機（Pluto SDR）での受信
セットアップ（ドライバ・libiio DLL 配置・pylibiio パッチ）は `docs/hw-setup.md`。
`testdata/captured/` は gitignore なので、実波データは以下で再取得する:
1. `python hw/scripts/scan_channels.py` で受信可能チャンネルを確認（GI 相関で ISDB-T 判定）
2. `python hw/scripts/antenna_meter.py --ch <N>` でアンテナ位置を調整（seg0 C/N 表示）
3. `python tools/iq_record.py --ch <N> --sec 10 --out testdata/captured/<name>` で録音
4. `tools/rx_report.py` で Mode/GI・MER・遅延プロファイル、`tools/demod_sigmf.py --track` で TS 化
5. **ライブ受信**: `.\rt\third_party\fetch_libiio.ps1` → `make rt` →
   `rt\build\rt-x64\Release\isdbtd.exe --sdr usb: --ch <N>` → TVTest/BonDriver で視聴
   （詳細・制約は docs/hw-setup.md の「SDR ライブ受信」）

## 範囲外
B-CAS / ACAS によるスクランブル解除は扱わない。

## ライセンス
MIT License（[LICENSE](LICENSE)）。

本リポジトリに GPL コードの複製は含まれない。定数表（パイロット位置・キャリアランダマイズ等）は
当初 gr-isdbt（GPLv3）から転記したが、仕様が一意に定める定数データであり、ARIB STD-B31 v2.2-E1
原本と全値を照合済み（docs/decisions.md 2026-10-02）。tezuka_fw（GPL-3.0）はファームウェアとして
実機に書き込んで使うのみ、libiio（LGPL-2.1+）は DLL 動的リンクで、いずれも本体ライセンスに影響しない。
