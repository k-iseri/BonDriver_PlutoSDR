# make が無い Windows 環境向けの代替。
# 使い方: .\dev.ps1 setup / test-fast / test / lint / synth / bondriver / rt / rt-vectors / rt-test
param([Parameter(Mandatory=$true)][string]$Task)
$ErrorActionPreference = "Stop"
$venv = ".venv"
switch ($Task) {
  "setup" {
    python -m venv $venv
    & "$venv\Scripts\python" -m pip install -U pip
    & "$venv\Scripts\pip" install -e ".[dev]"
  }
  "test-fast" { & "$venv\Scripts\pytest" -q -m "not slow" }
  "test"      { & "$venv\Scripts\pytest" -q }
  "lint"      { & "$venv\Scripts\ruff" check ref tools }
  "synth"     { & "$venv\Scripts\python" tools/make_synthetic.py --out testdata/synthetic }
  "bondriver" { & "$PSScriptRoot\rt\bondriver\build.ps1" }
  "rt"         { & "$PSScriptRoot\rt\build.ps1" }
  "rt-vectors" { & "$venv\Scripts\python" tools/gen_rt_vectors.py --out testdata/rt_vectors }
  "rt-test"    { & "$PSScriptRoot\rt\build\rt-x64\Release\rt_test.exe" testdata/rt_vectors }
  "libiio-fetch" { & "$PSScriptRoot\rt\third_party\fetch_libiio.ps1" }
  default     { Write-Error "unknown task: $Task" }
}
