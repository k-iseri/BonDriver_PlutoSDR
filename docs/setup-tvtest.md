# Windows 11 + TVTest で地デジ / ケーブルテレビの BS を視聴するまで（一般ユーザー向け）

Pluto SDR 互換機と本プロジェクトの BonDriver を使い、Windows 11 の TVTest で
地上デジタル放送（フルセグ）と、ケーブルテレビの BS/CS トランスモジュレーションを視聴するための手順。
開発者向けの詳細は [rt/bondriver/README.md](../rt/bondriver/README.md) と [hw-setup.md](hw-setup.md) を参照。

DLL は 2 種類あり、**どちらか片方**を入れる:

| DLL | 受信できるもの | チューニング空間 |
|---|---|---|
| `BonDriver_PlutoSDR_T.dll` | 地デジのみ | UHF（13〜52ch） |
| `BonDriver_PlutoSDR_TC.dll` | 地デジ + ケーブルテレビの BS/CS（トラモジ、64QAM） | UHF と CATV |

以下では `_T` で説明するが、`_TC` を使う場合はファイル名をすべて `_TC` に読み替える
（ini も `BonDriver_PlutoSDR_TC.ini`）。

## 1. 必要なもの

| もの | 備考 |
|---|---|
| Windows 11 の PC（64bit） | USB 2.0 以上の空きポート |
| Pluto SDR（互換機可） | AD9363 搭載。USB 接続 |
| UHF アンテナ | **受信品質はほぼアンテナで決まる**。付属の小型アンテナではフルセグは映らないことが多い。壁のアンテナ端子か屋外アンテナを推奨 |
| F型 → SMA 変換アダプタ | Pluto の RX 端子は SMA。テレビの同軸ケーブル（F型）をつなぐのに必要 |
| TVTest（x64 版） | 0.10 系を推奨。入手・ビルドは TVTest 側の配布に従う |
| B-CAS カード + IC カードリーダー | スクランブル解除に必要（TVTest 側の設定。本プロジェクトの範囲外） |
| BonDriver_PlutoSDR_T.dll 一式 | 配布 zip（[GitHub Releases](https://github.com/k-iseri/BonDriver_PlutoSDR/releases) からダウンロード）、または下記 1-b で自分でビルド |

### 1-b. BonDriver を自分でビルドする場合

配布 zip が手元にない場合のみ。Visual Studio 2022（Build Tools 可）の C++ ツールセットと
CMake が必要。リポジトリのルートで:

```powershell
.\rt\third_party\fetch_libiio.ps1   # libiio を取得（これが無いと SDR 直結モードが無効になる）
.\dev.ps1 bondriver                 # make があるなら make bondriver
```

`rt\build\bondriver-x64\Release\` に `BonDriver_PlutoSDR_T.dll` と libiio の DLL 一式ができる。
以降の手順ではこのフォルダの中身を使う。

## 2. Pluto SDR のドライバを入れる

1. Analog Devices の **PlutoSDR-M2k-USB-Drivers.exe** をダウンロードして実行する
   （ADI の Wiki「PlutoSDR Drivers for Windows」から入手）。
2. Pluto SDR を USB でつなぐ。
3. デバイスマネージャーに「**IIO** (USBDevice)」「PlutoSDR Serial Console (COMx)」などが
   出ていれば認識できている。

## 3. アンテナをつなぐ

F型 → SMA 変換アダプタで、アンテナ線を Pluto の **RX** 端子（TX ではない方）につなぐ。
室内アンテナの場合は窓際・高い位置に置く。フルセグ（64QAM）は C/N 約 19 dB 以上が必要で、
ワンセグ（約 8 dB 以上）より条件が厳しい。

## 4. TVTest に BonDriver を配置する

**配布 zip の場合**: zip の中身を全部 TVTest の `BonDriver` フォルダ（無ければ TVTest.exe と
同じフォルダ）へコピーし、`BonDriver_PlutoSDR_T.ini.sample` を `BonDriver_PlutoSDR_T.ini` に
リネームすれば完了。以下は自分でビルドした場合などに個別に揃える内訳:

1. `BonDriver_PlutoSDR_T.dll`
2. `BonDriver_PlutoSDR_T.ini` — [BonDriver_PlutoSDR.ini.sample](../rt/bondriver/BonDriver_PlutoSDR.ini.sample)
   をコピーしてこの名前に変更する。**そのままで動く**（`[SDR] Enable=1`、`Uri=usb:` が既定）
3. libiio の DLL 一式（`libiio1.dll` など。ビルドした場合は
   `rt\build\bondriver-x64\Release\` に入っている）— DLL と同じフォルダに置く

> TVTest が 32bit 版だとこの DLL（64bit）は読み込めない。必ず x64 の TVTest を使う。

## 5. TVTest の初期設定

1. TVTest を起動すると初期設定ダイアログが出る。
   - **ドライバ**: `BonDriver_PlutoSDR_T.dll` を選ぶ（チューナ名は「ISDB-T SDR」）
   - **デコーダ / レンダラ**: 既定（自動）でよい
   - カードリーダー: B-CAS リーダーを選ぶ（TVTest 側のスクランブル解除設定に従う）
2. **チャンネルスキャン**（設定 → チャンネルスキャン）を実行する。
   チューニング空間は **UHF**、物理チャンネルは 13〜52ch。
   - スキャンには時間がかかる（1ch あたり選局〜判定で十数秒）。
   - 地域の送信チャンネルが分かっていれば、対象を絞ると速い。

## 6. 視聴する

チャンネルを選んでから映像が出るまで **約 10 秒かかる**（復調パイプラインの起動時間。
その間 TVTest は「無信号」表示になるが異常ではない）。チャンネル切替のたびに同程度かかる。
選局の約 5 秒後に番組情報（PSI/SI）だけ先に出るので、チャンネルスキャンは既定の待ち時間でも検出できる
（ini の `[SDR] FirstBurst`。`all` にすると 5 秒で一瞬映るが数秒止まる）。
TVTest のチャンネルスキャンは既定で 1 チャンネルあたり約 9 秒（待ち時間 5 秒 + リトライ 4 回）で
次へ進むので、これより遅いと 1 局も見つからない。スキャンで局が見つからないときは、スキャンの
「詳細設定」で待ち時間を 10 秒以上にし、「信号レベルが低いチャンネルを飛ばす」を外す。

TVTest の信号レベル表示は受信品質（MER、dB）を示す。目安:

- **19 dB 以上**: フルセグが安定して映る
- **8〜19 dB**: ワンセグ相当の階層のみ復調できる可能性（映像は乱れる・止まる）
- それ未満: アンテナの改善が必要

## 7. うまくいかないとき

| 症状 | 確認すること |
|---|---|
| TVTest がドライバを読み込めない | TVTest が x64 版か。ini が DLL と同名・同フォルダにあるか |
| チューナを開くとエラー | libiio の DLL が DLL と同じフォルダにあるか。Pluto が USB で認識されているか（デバイスマネージャーに IIO）。他のソフト（SDR# など）が Pluto を掴んでいないか |
| ずっと無信号（15 秒以上） | まずアンテナ。信号レベル（MER）が 19 dB 未満ならアンテナの位置・種類を見直す。壁のアンテナ端子につなぐのが確実 |
| チャンネルスキャンで 1 局も見つからない | 選局から番組情報が出るまで約 5 秒、映像は約 10 秒かかる。スキャンの詳細設定で待ち時間を 10 秒以上にし、信号レベルによる除外を外す。ini の `[Debug] Log=1` でログを見ると `tune` の間隔と `locked` の時刻が分かる |
| 映像が出るがスクランブル表示 | B-CAS カードリーダーと TVTest のデコード設定（本プロジェクトの範囲外） |
| 急に受信できなくなった | Pluto の USB を抜き差しして TVTest を再起動（SDR 側のセッションが残ることがある） |
| 原因を調べたい | ini の `[Debug] Log=1` にすると DLL の隣に `BonDriver_PlutoSDR_T.log` が書かれる。`rs_uncorrectable` が増える = 電波品質不足、`underruns` / `frames_lost` = PC 側の取り込み・処理落ち |

TVTest を使わずに受信だけ確認したい場合は、DLL と同じフォルダで
`bontest.exe BonDriver_PlutoSDR_T.dll 27 5 out.ts`（物理 27ch を 5 秒受信して TS 保存）が使える。

## 8. ケーブルテレビの BS/CS を見る（`_TC` 版）

ケーブルテレビの宅内端子には、BS/CS を 64QAM に変換した信号（トランスモジュレーション）が
UHF/CATV 帯のチャンネルに載っている。`BonDriver_PlutoSDR_TC.dll` のチューニング空間 **CATV** で受信する。

1. ケーブルテレビの同軸を Pluto の RX 端子へつなぐ（地デジのパススルーも同じ端子に来ているので
   UHF 空間の地デジもそのまま映る）。
2. TVTest のチャンネルスキャンでチューニング空間 **CATV** を選ぶ。チャンネル番号は VHF 1〜12ch、
   UHF 13〜62ch、C13〜C63 の通し番号（0〜112）。BS 再送信は局によって UHF 帯（例: 49〜56ch）や
   C40 台に置かれている。全部スキャンすると 113ch × 数秒かかるので、分かっていれば絞る。
3. **BonDriver_BDA から乗り換える場合**: `BonDriver_BDA.ch2` の 2 列目（チューニング空間）を
   `0` から `1` に置き換えて `BonDriver_PlutoSDR_TC.ch2` として置けば、スキャンせずにそのまま使える
   （チャンネル番号の付け方が BonDriver_BDA の `ChannelSettingsAuto="TransModulation"` と同じ）。
4. 選局から映像まで 2〜3 秒。信号レベル表示は MER で、ケーブルなら 30 dB 以上が普通。
   20 dB 前後に張り付いて映らない場合は ini の `[SDR] Gain=` が空（ソフトウェア AGC）になっているか確認する
   （`Gain=hw` だとフロントエンドが歪む）。
5. 1 つのチャンネルに複数の TS が多重（TSMF）されている局では、ini の `[CATV] Ts=` で
   相対 TS 番号を指定できる（既定は最小番号の TS）。

## 補足

- 録画（EDCB）でも同じ DLL・ini が使える。配置方法は TVTest と同じ。
- Gigabit Ethernet 付きの Pluto+ / LibreSDR を使う場合は、ini の `Uri=ip:192.168.x.x` に
  変え、`Fs=8126984` に上げられる（USB 2.0 の帯域制限が無いため）。
- 1 台の Pluto で同時に受信できるのは 1 チャンネルのみ（裏録画は不可）。
