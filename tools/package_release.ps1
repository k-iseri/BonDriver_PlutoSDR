# BonDriver_PlutoSDR（_T / _TC）の配布 zip を dist/ に作る（GitHub Actions のリリースとローカル確認の両方で使う）。
# 使い方: .\tools\package_release.ps1 [-Version v1.0.0] [-Config Release] [-Arch x64]
# 前提: .\rt\bondriver\build.ps1 でビルド済み（libiio ありの x64 ビルドを想定。
#       rt\third_party\fetch_libiio.ps1 を先に実行していないとスタンドアロンモードが無効の DLL になる）
param(
  [string]$Version = "dev",
  [string]$Config = "Release",
  [ValidateSet("x64", "Win32")][string]$Arch = "x64"
)
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$bin = Join-Path $repo "rt\build\bondriver-$Arch\$Config"
foreach ($dll in @("BonDriver_PlutoSDR_T.dll", "BonDriver_PlutoSDR_TC.dll")) {
  if (-not (Test-Path (Join-Path $bin $dll))) {
    throw "$dll が無い: $bin（先に .\rt\bondriver\build.ps1 を実行する）"
  }
}
if (-not (Test-Path (Join-Path $bin "libiio1.dll"))) {
  throw "libiio1.dll が無い: $bin（.\rt\third_party\fetch_libiio.ps1 を実行してからビルドし直す）"
}

$name = "BonDriver_PlutoSDR-$Version-$Arch"
$stage = Join-Path $repo "dist\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# DLL 本体（_T / _TC）+ libiio 一式（*.dll）+ サンプル ini + 動作確認用 bontest
Copy-Item (Join-Path $bin "*.dll") $stage
Copy-Item (Join-Path $bin "*.ini.sample") $stage
Copy-Item (Join-Path $bin "bontest.exe") $stage
# recpluto.exe / isdbtd.exe（rt\build.ps1 の成果物。あれば同梱。libiio の DLL は上で入っている）
$rtbin = Join-Path $repo "rt\build\rt-$Arch\$Config"
foreach ($exe in @("recpluto.exe", "isdbtd.exe")) {
  if (Test-Path (Join-Path $rtbin $exe)) { Copy-Item (Join-Path $rtbin $exe) $stage } else { Write-Host "note: $exe not built; skipped" }
}
Copy-Item (Join-Path $repo "LICENSE") $stage
Copy-Item (Join-Path $repo "docs\mirakurun.md") $stage
Copy-Item (Join-Path $repo "docs\setup-tvtest.md") $stage

# インストール手順（詳細は同梱の setup-tvtest.md / リポジトリの docs）
@'
BonDriver_PlutoSDR — Pluto SDR で地上デジタル放送（ISDB-T フルセグ）と、ケーブルテレビの
BS/CS トランスモジュレーション（ISDB-C、64QAM）を受信する TVTest / EDCB 用 BonDriver。
復調は DLL 内で行うので、この zip だけで動く。

DLL は 2 種類入っている。どちらか片方だけをインストールする:
  BonDriver_PlutoSDR_T.dll  … 地デジのみ（チューニング空間 UHF）
  BonDriver_PlutoSDR_TC.dll … 地デジ + ケーブルテレビのトラモジ（空間 UHF と CATV）。
                              1 つのドライバで両方受信できる

== インストール ==

1. Analog Devices の「PlutoSDR-M2k-USB-Drivers」をインストールし、
   Pluto SDR（互換機可）を USB で接続する。
2. 使う方の DLL と、libiio の DLL 一式（lib*.dll, msvcp140.dll, vcruntime140.dll）、
   bontest.exe を TVTest（x64 版）の BonDriver フォルダ
   （無ければ TVTest.exe と同じフォルダ）へコピーする。
3. 使う DLL と同名の ini.sample（BonDriver_PlutoSDR_T.ini.sample または
   BonDriver_PlutoSDR_TC.ini.sample）を同じ場所へコピーし、「.sample」を外した
   名前（例: BonDriver_PlutoSDR_TC.ini）にリネームする（設定はそのままで動く。
   LAN 接続機は Uri=ip:… に変更）。
4. アンテナ線（またはケーブルテレビの同軸）を Pluto の RX 端子へつなぐ
   （F型→SMA 変換アダプタ使用）。
5. TVTest のドライバに DLL を選び、チャンネルスキャンする。
   - 地デジ: チューニング空間 UHF（13〜52ch）
   - ケーブルテレビの BS/CS: _TC 版のチューニング空間 CATV（VHF 1〜12ch、UHF 13〜62ch、
     C13〜C63 の通し番号。BonDriver_BDA の TransModulation と同じ番号なので、
     BonDriver_BDA の .ch2 は 2 列目（空間）を 0 → 1 に書き換えれば流用できる）

- 選局から映像が出るまで、地デジは約 10 秒、ケーブルは 2〜3 秒かかる（異常ではない。
  地デジは番組情報だけ約 5 秒で先に出るのでチャンネルスキャンは既定設定で検出できる）。
  チャンネルスキャンで局が見つからないときは、スキャンの詳細設定で待ち時間を
  10 秒以上にし、信号レベルによる除外を外す。
- 受信ゲインは自動（帯域内 −24 dBFS 目標のソフトウェア AGC）。ケーブル端子のように
  全チャンネルが強い入力でも歪まないように抑えてある。
- B-CAS / スクランブル解除は TVTest 側の設定（本パッケージの範囲外）。
- TVTest 無しの動作確認:
    bontest.exe BonDriver_PlutoSDR_T.dll 27 5 out.ts      （地デジ物理 27ch を 5 秒）
    bontest.exe BonDriver_PlutoSDR_TC.dll 1/55 5 out.ts   （CATV 空間の番号 55 = 56ch を 5 秒）
- recpluto.exe / isdbtd.exe は Mirakurun 用のチューナーコマンドと復調デーモン（Linux / macOS 版と同じもの。
  libiio の DLL と同じフォルダに置いて使う。使い方は同梱の mirakurun.md）:
    recpluto.exe 27 10 out.ts                              （地デジ 27ch を 10 秒録画）
    recpluto.exe --scan gr:13-52                           （受信できる局を channels.yml の形で表示）
- うまくいかないときの詳細手順は同梱の setup-tvtest.md、または
  https://github.com/k-iseri/BonDriver_PlutoSDR/blob/main/docs/setup-tvtest.md
'@ | Set-Content (Join-Path $stage "INSTALL.txt") -Encoding UTF8

# 同梱する他者バイナリの出所（libiio の Windows 配布 zip の中身。動的リンク）
@'
このパッケージには以下のサードパーティ製 DLL を同梱しています
（libiio v1.0.0 の Windows 配布物。いずれも動的リンク）。

- libiio1.dll / libiio.dll — libiio, LGPL-2.1-or-later
  https://github.com/analogdevicesinc/libiio （v1.0.0 リリースの Windows.zip より）
- libusb-1.0.dll — libusb, LGPL-2.1-or-later  https://libusb.info
- libserialport.dll — libserialport, LGPL-3.0-or-later  https://sigrok.org/wiki/Libserialport
- libxml2.dll — libxml2, MIT  https://gitlab.gnome.org/GNOME/libxml2
- libzstd.dll — zstd, BSD-3-Clause  https://github.com/facebook/zstd
- msvcp140.dll / vcruntime140.dll — Microsoft Visual C++ 再頒布可能ランタイム

BonDriver_PlutoSDR_T.dll / BonDriver_PlutoSDR_TC.dll / bontest.exe 本体のライセンスは同梱の LICENSE（MIT）。
各ライブラリのライセンス全文は上記の各プロジェクトを参照してください。
'@ | Set-Content (Join-Path $stage "THIRD_PARTY_NOTICES.txt") -Encoding UTF8

$zip = Join-Path $repo "dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "packaged: $zip"
Get-ChildItem $stage | ForEach-Object { Write-Host ("  " + $_.Name) }
