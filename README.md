# isdbt-sdr

汎用 SDR ボード（Pluto SDR 互換機など）で日本の地上デジタル放送（ISDB-T）を受信し、
PC 上のソフトウェアで復調して MPEG-2 TS を取り出すプロジェクト。

## 構成
- `ref/` — Python 参照実装（正しさ優先）とテスト
- `rt/` — C++ 実装。`rt/isdbt/` 復調コア、`rt/demod/` 復調デーモン isdbtd、
  `rt/bondriver/` は TVTest / EDCB 用 BonDriver、`rt/tests/` は ref/ との一致テスト
- `hw/` — ボード固有の設定・録音スクリプト・将来の HDL 改変
- `tools/` — 録音、I/Q 解析、受信品質レポート、TS 検査
- `docs/` — 仕様ノート、設計、判断記録
- `testdata/` — 合成信号と実波録音（gitignore）

## はじめかた
```
make setup        # Windows で make が無い場合: .\dev.ps1 setup
make test-fast    #                             .\dev.ps1 test-fast
make synth        # 合成試験信号を testdata/synthetic/ に生成（約 10 秒）
python tools/demod_sigmf.py testdata/synthetic/std_mode3_gi8_clean.sigmf-meta --out out/   # 復調して TS を得る
```
Python 3.11 以上。Windows ネイティブ環境を前提にしている。
`make test`（ループバック・BER 曲線を含む slow テスト）は 10 分程度かかる。
ref/ の復調は 6 フレーム単位のブロック処理で、速度は実時間の約 14 倍遅、メモリは録音データ
（complex64、30 秒で約 2 GB）+ 数百 MB。C++ 版（`make rt` → isdbtd.exe）は実時間の約 1 倍。

長時間・ドリフト・瞬断のある録音は `--track`（`demod_sigmf.py` / `isdbtd`）で追従復調。
受信品質の切り分けは `python tools/rx_report.py <録音>.sigmf-meta --plot out/r`
（Mode/GI ブラインド判定、MER、キャリア別 MER、遅延プロファイル）。
204 バイト放送 TS（BTS）が要る場合は `demod_sigmf.py --bts`。

## TVTest / EDCB で使う（BonDriver）

ビルド済みの配布 zip は [GitHub Releases](https://github.com/k-iseri/BonDriver_PlutoSDR/releases) にある
（`v*` タグの push で GitHub Actions が自動ビルド・添付する。手順は `rt/bondriver/README.md`）。
インストール方法は `docs/setup-tvtest.md`（zip の中身を TVTest の BonDriver フォルダへコピーして
ini.sample をリネームするだけ）。自分でビルドする場合:
```
make bondriver                                   # または .\dev.ps1 bondriver（VS 2022 Build Tools が必要）
make rt                                          # 復調デーモン isdbtd.exe と一致テスト rt_test.exe
rt\build\rt-x64\Release\isdbtd.exe --sigmf <録音>.sigmf-meta --loop    # C++ 復調デーモン（MER を status に返す）
python tools/demod_server.py --ts C:\rec\x.ts --loop                   # Python の偽デーモン（録画 TS / SigMF 再生）
rt\build\bondriver-x64\Release\bontest.exe rt\build\bondriver-x64\Release\BonDriver_PlutoSDR_T.dll 27 5
```
DLL と `BonDriver_PlutoSDR_T.ini`（`.ini.sample` を編集）を TVTest の BonDriver フォルダへ置く。
視聴までの一般ユーザー向け手順は `docs/setup-tvtest.md`。
詳細は `rt/bondriver/README.md`、プロトコルは `docs/daemon-protocol.md`。
SDR 実機ソースはボード到着後に isdbtd へ追加する（それまでは録音の復調再生）。

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
