# rt/（復調コア + rt_test.exe + isdbtd.exe）をビルドする。
# 使い方: .\rt\build.ps1 [-Config Release|Debug]
# 出力: rt\build\rt-x64\<Config>\{rt_test.exe, isdbtd.exe}
# 必要: Visual Studio 2022（Build Tools 可）の C++ ツールセットと CMake（VS 同梱で可）
param(
  [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$src = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = (Resolve-Path (Join-Path $src "..")).Path
$build = Join-Path $repo "rt\build\rt-x64"

$cmakeCmd = Get-Command cmake -ErrorAction SilentlyContinue
$cmake = if ($cmakeCmd) { $cmakeCmd.Source } else { $null }
if (-not $cmake) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) { throw "cmake が PATH に無く vswhere も見つからない。Visual Studio 2022 Build Tools を入れること" }
  $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  if (-not (Test-Path $cmake)) { throw "VS 同梱の CMake が見つからない: $cmake（VS Installer で C++ CMake tools を追加）" }
}

& $cmake -S $src -B $build -A x64
if ($LASTEXITCODE) { throw "cmake configure failed" }
& $cmake --build $build --config $Config
if ($LASTEXITCODE) { throw "cmake build failed" }
Write-Host ""
Write-Host "built: $build\$Config\rt_test.exe"
Write-Host "       $build\$Config\isdbtd.exe"
