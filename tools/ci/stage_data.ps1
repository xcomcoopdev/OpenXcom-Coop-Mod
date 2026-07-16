# Stages runtime data next to the built exe (bin\x64\Release) for the coop test suite.
# Expects: the built OpenXcom.exe already at bin\x64\Release (build output or CI artifact)
# and the licensed UFO game data checked out at .ufo_data (private OpenXcom-Coop/UFO_Data).
$d = "bin\x64\Release"
robocopy deps\lib\x64 $d *.dll /NFL /NDL /NJH /NJS
robocopy bin\standard "$d\standard" /E /NFL /NDL /NJH /NJS
robocopy bin\common   "$d\common"   /E /NFL /NDL /NJH /NJS
robocopy .ufo_data "$d\UFO" /E /XD .git /NFL /NDL /NJH /NJS               # licensed UFO data from private repo
robocopy bin\UFO\multiplayer "$d\UFO\multiplayer" /E /NFL /NDL /NJH /NJS   # coop art, from the public repo
if ($LASTEXITCODE -lt 8) { $global:LASTEXITCODE = 0 }                      # robocopy: <8 = success

# Rendezvous config (git-ignored real one is per-deployment): the coop harness needs
# a parseable rendezvous.json next to the exe or ServerList's ctor errors into a
# CoopState (lobby tests) and the UDP-public host path crashes. Ship the CI config
# (blackhole host + throwaway valid keys).
Copy-Item tools\coop_test\rendezvous.ci.json "$d\rendezvous.json" -Force

if (-not (Test-Path "$d\OpenXcom.exe"))             { throw "no exe at $d (build output / artifact missing?)" }
if (-not (Test-Path "$d\UFO\GEODATA"))              { throw "UFO data not staged (deploy key / private repo?)" }
if (-not (Test-Path "$d\UFO\multiplayer\base.png")) { throw "coop assets not staged" }
if (-not (Test-Path "$d\rendezvous.json"))          { throw "rendezvous.json not staged" }
exit 0
