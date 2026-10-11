# BonDriver_PlutoSDR_T の配布 zip を dist/ に作る（GitHub Actions のリリースとローカル確認の両方で使う）。
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
if (-not (Test-Path (Join-Path $bin "BonDriver_PlutoSDR_T.dll"))) {
  throw "BonDriver_PlutoSDR_T.dll が無い: $bin（先に .\rt\bondriver\build.ps1 を実行する）"
}
if (-not (Test-Path (Join-Path $bin "libiio1.dll"))) {
  throw "libiio1.dll が無い: $bin（.\rt\third_party\fetch_libiio.ps1 を実行してからビルドし直す）"
}

$name = "BonDriver_PlutoSDR_T-$Version-$Arch"
$stage = Join-Path $repo "dist\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# DLL 本体 + libiio 一式（*.dll）+ サンプル ini + 動作確認用 bontest
Copy-Item (Join-Path $bin "*.dll") $stage
Copy-Item (Join-Path $bin "BonDriver_PlutoSDR_T.ini.sample") $stage
Copy-Item (Join-Path $bin "bontest.exe") $stage
Copy-Item (Join-Path $repo "LICENSE") $stage
Copy-Item (Join-Path $repo "docs\setup-tvtest.md") $stage

# インストール手順（詳細は同梱の setup-tvtest.md / リポジトリの docs）
@'
BonDriver_PlutoSDR_T — Pluto SDR で地上デジタル放送（ISDB-T フルセグ）を受信する
TVTest / EDCB 用 BonDriver。復調は DLL 内で行うので、この zip だけで動く。

== インストール ==

1. Analog Devices の「PlutoSDR-M2k-USB-Drivers」をインストールし、
   Pluto SDR（互換機可）を USB で接続する。
2. この zip の中身を全部、TVTest（x64 版）の BonDriver フォルダ
   （無ければ TVTest.exe と同じフォルダ）へコピーする。
3. BonDriver_PlutoSDR_T.ini.sample を「BonDriver_PlutoSDR_T.ini」に
   リネームする（設定はそのままで動く。LAN 接続機は Uri=ip:… に変更）。
4. アンテナを Pluto の RX 端子へつなぐ（F型→SMA 変換アダプタ使用。
   壁のアンテナ端子か屋外アンテナを推奨）。
5. TVTest のドライバに BonDriver_PlutoSDR_T.dll を選び（チューナ名
   「ISDB-T SDR」）、チューニング空間 UHF でチャンネルスキャンする。

- 選局から映像が出るまで 10〜15 秒かかる（異常ではない）。
- B-CAS / スクランブル解除は TVTest 側の設定（本パッケージの範囲外）。
- TVTest 無しの動作確認: bontest.exe BonDriver_PlutoSDR_T.dll 27 5 out.ts
  （物理 27ch を 5 秒受信して TS 保存）
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

BonDriver_PlutoSDR_T.dll / bontest.exe 本体のライセンスは同梱の LICENSE（MIT）。
各ライブラリのライセンス全文は上記の各プロジェクトを参照してください。
'@ | Set-Content (Join-Path $stage "THIRD_PARTY_NOTICES.txt") -Encoding UTF8

$zip = Join-Path $repo "dist\$name.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Write-Host "packaged: $zip"
Get-ChildItem $stage | ForEach-Object { Write-Host ("  " + $_.Name) }
