# Asserts a release archive is shippable. Two ways it can be wrong:
#
#   1. Too much: licensed retail X-COM data rides along. UFO/ and TFTD/ are
#      whitelisted down to their multiplayer/ subdirectory, so any other entry
#      under them fails the build (stronger than the old GEODATA-only canary,
#      which only caught one directory name).
#   2. Too little: the coop art is missing (Globe's ctor loads multiplayer/base.png
#      unguarded, so a zip without it crashes the moment a player starts a new game -
#      shipped that way in nightly 8.4.13203), or one of the files that is not build
#      output and has to be placed deliberately: rendezvous.json, LICENSE.txt.
#
# Usage: ./tools/ci/assert_package.ps1 <archive.zip>
param([Parameter(Mandatory)][string]$Archive)
$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $Archive).Path)
# Normalize separators: pwsh 7 (CI) writes '/', Windows PowerShell 5.1 writes '\'.
# Keep files only - directory entries carry no data.
try   { $names = @($zip.Entries.FullName -replace '\\', '/' | Where-Object { $_ -notmatch '/$' }) }
finally { $zip.Dispose() }

$bad = @($names | Where-Object { $_ -match '^(UFO|TFTD)/' -and $_ -notmatch '^(UFO|TFTD)/(multiplayer/|README\.txt$)' })
$bad += @($names | Where-Object { $_ -match 'GEODATA' })     # named canary, kept for clarity
if ($bad.Count) { throw "licensed retail data leaked into $Archive`: $(($bad | Select-Object -Unique) -join ', ')" }

foreach ($req in @('UFO/multiplayer/base.png', 'TFTD/multiplayer/base.png')) {
  if ($names -notcontains $req) { throw "$Archive is missing $req - new game would crash in Globe's ctor" }
}

# Not build output, so every package has to place these deliberately - the WinXP zip
# shipped without either through 8.4.13203 (empty Official server list).
foreach ($req in @('rendezvous.json', 'LICENSE.txt')) {
  if ($names -notcontains $req) { throw "$Archive is missing $req" }
}

Write-Host "package OK ($Archive): coop art + rendezvous.json + LICENSE.txt present, no licensed retail data ($($names.Count) entries)"
