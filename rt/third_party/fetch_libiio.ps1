# libiio v1.0.0 (Windows VS2022 x64) を rt/third_party/libiio に取得する。
# third_party/ は gitignore。クリーン環境では最初にこれを実行する:
#   .\rt\third_party\fetch_libiio.ps1
# ライセンス: libiio は LGPL-2.1+（動的リンク）。docs/decisions.md 参照。
$ErrorActionPreference = "Stop"
$url = "https://github.com/analogdevicesinc/libiio/releases/download/v1.0.0/Windows.zip"
$root = Split-Path $PSScriptRoot -Parent | Split-Path -Parent   # リポジトリルート
$dst = Join-Path $PSScriptRoot "libiio"
$tmp = Join-Path $env:TEMP "libiio-fetch"
New-Item -ItemType Directory -Force $tmp | Out-Null
$zip = Join-Path $tmp "Windows.zip"
if (-not (Test-Path $zip)) {
    Write-Host "downloading $url ..."
    Invoke-WebRequest $url -OutFile $zip
}
Expand-Archive $zip -DestinationPath "$tmp\x" -Force
New-Item -ItemType Directory -Force "$dst\include", "$dst\lib", "$dst\bin" | Out-Null
Copy-Item -Recurse -Force "$tmp\x\include\iio" "$dst\include\"
Copy-Item "$tmp\x\Windows-VS-2022-x64\Release\libiio1.lib" "$dst\lib\"
Copy-Item "$tmp\x\Windows-VS-2022-x64\Release\*.dll" "$dst\bin\"
Write-Host "libiio -> $dst"
Write-Host "(Python 側で使う場合は $dst\bin の DLL を .venv\Scripts にもコピーする。docs/hw-setup.md 参照)"
