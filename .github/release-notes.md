## v1.1.1 の変更点（v1.1.0 から）

- TVTest のチャンネルスキャンで地デジが検出できない問題を修正（選局直後から暫定の信号レベルを返す。
  スキャンは既定の待ち時間 5 秒・リトライ 4 回のままでよい）
- 地デジのチャンネル切替後に映像が一瞬出てから止まる・かくつく問題を修正。選局から約 6 秒で番組情報、
  約 10〜12 秒で映像が連続して始まる（`[SDR] FirstBurst` で切替可。既定 psi）
- 信号のある局が続くとスキャンで交互に落ちる問題を修正
- 地デジのウィンドウ既定を `WinFrames=8` / `Workers=3` に変更（v1.1.0 の ini を使い回している場合は
  `WinFrames=16` / `Workers=2` を書き換えるか、新しい ini.sample から作り直す）

Pluto SDR で地上デジタル放送（ISDB-T フルセグ）と、ケーブルテレビの BS/CS トランスモジュレーション
（ISDB-C、64QAM）を受信する TVTest / EDCB 用 BonDriver。
復調は DLL 内で行うため、添付の zip だけで動作します（復調デーモン不要）。

zip には 2 種類の DLL が入っています。**どちらか片方**をインストールしてください:

| DLL | 受信できるもの | チューニング空間 |
|---|---|---|
| `BonDriver_PlutoSDR_T.dll` | 地デジのみ | UHF |
| `BonDriver_PlutoSDR_TC.dll` | 地デジ + ケーブルテレビのトラモジ（BS/CS） | UHF、CATV |

## インストール

1. Analog Devices の **PlutoSDR-M2k-USB-Drivers** をインストールし、Pluto SDR（互換機可）を USB で接続
2. 使う方の DLL と libiio の DLL 一式（`lib*.dll`、`msvcp140.dll`、`vcruntime140.dll`）を TVTest（**x64 版**）の BonDriver フォルダへコピー
3. 同名の `.ini.sample`（例: `BonDriver_PlutoSDR_TC.ini.sample`）を同じ場所へコピーし `.sample` を外す（そのままで動く）
4. アンテナ線またはケーブルテレビの同軸を Pluto の RX 端子へ（F型→SMA 変換）
5. TVTest のドライバに DLL を選び、チャンネルスキャン（地デジ = UHF、ケーブルの BS/CS = _TC 版の CATV）

CATV 空間のチャンネル番号は BonDriver_BDA の TransModulation と同じ通し番号（VHF 1〜12ch、UHF 13〜62ch、C13〜C63）なので、
BonDriver_BDA 用の `.ch2` は空間番号（2 列目）を 0 → 1 に書き換えれば流用できます。

選局から映像が出るまで地デジは約 10 秒、ケーブルは 2〜3 秒かかります（異常ではありません。地デジは番組情報だけ約 5 秒で先に出るので、チャンネルスキャンは既定設定で検出できます）。
チャンネルスキャンで局が見つからないときは、スキャンの詳細設定で待ち時間を 10 秒以上にしてください。
受信ゲインは帯域内 −24 dBFS 目標のソフトウェア AGC で、ケーブル端子のように全チャンネルが強い入力でも
フロントエンドが歪まないようにしています（AD936x 内蔵 AGC では MER が 20 dB 前後に張り付く）。
B-CAS / スクランブル解除は TVTest 側の設定で、本パッケージの範囲外です。
詳細な手順とトラブルシュートは [docs/setup-tvtest.md](https://github.com/k-iseri/BonDriver_PlutoSDR/blob/main/docs/setup-tvtest.md) を参照してください。

## 動作要件

- Windows 11 x64 / 64bit 版 TVTest（32bit 版では読み込めません）
- Pluto SDR 互換機（AD9363、USB 2.0。Pluto+ / LibreSDR は ini の `Uri=ip:…` で LAN 接続可）
- CPU は AVX2 対応（Haswell 以降）が必要
