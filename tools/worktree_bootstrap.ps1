# Bootstrap a worktree (or any fresh checkout) into a runnable, testable tree.
# Mirrors the CI "Stage data next to exe" step (.github/workflows/ci-main.yml)
# with the proprietary UFO data sourced from the local machine store.
#
#   .\tools\worktree_bootstrap.ps1 -Build -BootCheck
#
# -Build      serial x64 Release msbuild first (never /m: C1060 out-of-heap)
# -BootCheck  run tools/coop_test/boot_check.py headless after staging
# -UfoSource  override the proprietary UFO data source directory
[CmdletBinding()]
param(
    [switch]$Build,
    [switch]$BootCheck,
    [string]$UfoSource = "C:\oxc-data\UFO"
)
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$d = Join-Path $root "bin\x64\Release"
Write-Host "== worktree_bootstrap: $root"

if ($Build) {
    $msbuild = $null
    $cmd = Get-Command msbuild -ErrorAction SilentlyContinue
    if ($cmd) { $msbuild = $cmd.Source }
    if (-not $msbuild) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe | Select-Object -First 1
        }
    }
    if (-not $msbuild) { throw "MSBuild not found (PATH or vswhere)" }
    Write-Host "== building (serial) via $msbuild"
    & $msbuild (Join-Path $root "src\OpenXcom.2010.sln") /v:minimal /p:Configuration=Release /p:Platform=x64
    if ($LASTEXITCODE -ne 0) { throw "build failed rc=$LASTEXITCODE" }
}

New-Item -ItemType Directory -Force $d | Out-Null

function Copy-Tree($src, $dst, $files) {
    if (-not (Test-Path $src)) { throw "source missing: $src" }
    if ($files) { robocopy $src $dst $files /NFL /NDL /NJH /NJS | Out-Null }
    else        { robocopy $src $dst /E /NFL /NDL /NJH /NJS | Out-Null }
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($src -> $dst) rc=$LASTEXITCODE" }
}

Write-Host "== staging data next to exe"
Copy-Tree (Join-Path $root "deps\lib\x64") $d "*.dll"
Copy-Tree (Join-Path $root "bin\standard") (Join-Path $d "standard")
Copy-Tree (Join-Path $root "bin\common")   (Join-Path $d "common")
Copy-Tree $UfoSource                       (Join-Path $d "UFO")
Copy-Tree (Join-Path $root "bin\UFO\multiplayer") (Join-Path $d "UFO\multiplayer")
# Parseable rendezvous.json next to the exe or ServerList's ctor errors into a
# CoopState (lobby tests) and the UDP-public host path crashes.
Copy-Item (Join-Path $root "tools\coop_test\rendezvous.ci.json") (Join-Path $d "rendezvous.json") -Force

if ($Build -and -not (Test-Path (Join-Path $d "OpenXcom.exe"))) { throw "build produced no exe" }
if (-not (Test-Path (Join-Path $d "UFO\GEODATA")))              { throw "UFO data not staged" }
if (-not (Test-Path (Join-Path $d "UFO\multiplayer\base.png"))) { throw "coop assets not staged" }
if (-not (Test-Path (Join-Path $d "rendezvous.json")))          { throw "rendezvous.json not staged" }
Write-Host "== staging OK"

if ($BootCheck) {
    if (-not (Test-Path (Join-Path $d "OpenXcom.exe"))) { throw "no exe to boot-check (pass -Build?)" }
    Write-Host "== boot_check (headless)"
    $env:SDL_VIDEODRIVER = "dummy"
    $env:SDL_AUDIODRIVER = "dummy"
    python (Join-Path $root "tools\coop_test\boot_check.py")
    if ($LASTEXITCODE -ne 0) { throw "boot_check failed rc=$LASTEXITCODE" }
    Write-Host "== boot_check OK"
}
Write-Host "== bootstrap complete: $d"
