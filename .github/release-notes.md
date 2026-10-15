## v1.3.0 の変更点（v1.2.0 から）

- **カスタムファームウェア [pluto-fw-isdbt](https://github.com/k-iseri/pluto-fw-isdbt) v2.0.0 の FPGA 前段（同期・FFT・等化）に対応。**
  ファームが対応していれば自動で使い（ini の `[SDR] FpgaEq=auto` が既定。`0` で従来どおり）、
  復調の CPU 使用率が **約 2.7 コア → 約 1.1 コア** に下がります。受信品質（MER）は従来と同等です。
  v1.0.0 のファームや純正ファームではこれまでどおり PC 側で全段復調します。
- FPGA 前段を使うときも、ファームの ADC レベル計を読んでソフトウェア AGC を回し続けます（長時間視聴で
  信号レベルが変わってもゲインが追従）。
- ケーブルテレビ（_TC 版の CATV 空間）のチャンネルスキャンで、受信できない VHF 1〜12ch に直前の局
  （例: NHK BS）が何度も見つかる問題を修正（周波数が設定できないときは無信号として扱う）。
- FPGA 前段使用時、地デジのチャンネルスキャンで局を取りこぼすことがあった問題を修正（選局 1.5 秒で
  実測の MER を信号レベルとして返す）。
- ログ統計に `fpga_eq` / `fpga_delta` / `fpga_symbols` / `fpga_symbol_gaps` を追加（デーモンの status にも同じ項目）。

## v1.2.0 の変更点（v1.1.1 から）

- **カスタムファームウェア [pluto-fw-isdbt](https://github.com/k-iseri/pluto-fw-isdbt) に対応**（任意。純正ファームのままでも従来どおり動きます）。
  接続時にファームが申告するサンプル幅を読み、8bit ファームなら公称 8.127 MSPS で取り込みます
  （純正 16bit は従来どおり 7/8 レート）。ファームのサンプルカウンタが読めれば、USB 帯域不足で
  捨てられたサンプル数 `hw_dropped` をログ統計に出します。ファームの判別は申告ベースで、
  DLL は 1 種類のままです。
- ini の `[SDR] Fs` 未指定時はファームに合わせて自動選択になりました（既存の ini はそのまま使えます）。
- 8bit ファームでは `Gain=hw`（AD936x 内蔵 AGC）を指定してもソフトウェア AGC に切り替えます
  （内蔵 AGC だと 8bit 側でクリップするため）。
- `tools/iq_record.py` も申告ベースでサンプル幅と既定レートを自動判別します。

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

## カスタムファームウェア（任意）

HamGeek 製 PlutoSDR Nano 向けのファームウェア [pluto-fw-isdbt](https://github.com/k-iseri/pluto-fw-isdbt) を書くと、
I/Q を 8bit で転送して USB 2.0 の帯域余裕が 2 倍になり（v1.0.0 以降）、さらに v2.0.0 以降では地デジの同期・FFT・等化を
FPGA が行うので PC 側の復調負荷が約 2.7 コアから約 1.1 コアに下がります。DLL は接続時に自動で判別して使い分けます
（ini の `[SDR] FpgaEq=auto` が既定。純正ファームとの併用可。入れなくても動作は変わりません）。
対象基板・書き込み方法・戻し方は向こうの README を参照してください。

## 動作要件

- Windows 11 x64 / 64bit 版 TVTest（32bit 版では読み込めません）
- Pluto SDR 互換機（AD9363、USB 2.0。Pluto+ / LibreSDR は ini の `Uri=ip:…` で LAN 接続可）
- CPU は AVX2 対応（Haswell 以降）が必要
