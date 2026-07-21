# Merges this run's per-shard timings into the rolling weights that feed the next
# run's shard plan, so the split re-balances itself as tests are added, removed, or
# get slower. Never fails the build: bad/missing timings only cost balance.
#
# New measurements are blended with the previous value (EMA, 50/50) so one slow
# runner or a noisy test cannot swing the plan; a test with no previous value takes
# the new measurement as-is. Tests that no longer exist are dropped.
#
# Usage: ./tools/ci/merge_timings.ps1 -In <dir-of-shard-jsons> [-Prev <json>] [-Out <json>]
param(
  [Parameter(Mandatory)][string]$In,
  [string]$Prev,
  [string]$Out = "timings.json"
)
$ErrorActionPreference = "Stop"

function Read-Timings([string]$path) {
  $h = @{}
  if ($path -and (Test-Path $path)) {
    (Get-Content $path -Raw | ConvertFrom-Json).PSObject.Properties |
      ForEach-Object { $h[$_.Name] = [double]$_.Value }
  }
  return $h
}

$merged = Read-Timings $Prev
$before = $merged.Count

$prevPath = if ($Prev -and (Test-Path $Prev)) { (Resolve-Path $Prev).Path } else { "" }
$files = @(Get-ChildItem -Path $In -Filter timings-*.json -Recurse -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -ne $prevPath })   # only shard output; never Prev itself
$new = @{}
foreach ($f in $files) {
  foreach ($kv in (Read-Timings $f.FullName).GetEnumerator()) { $new[$kv.Key] = $kv.Value }
}
foreach ($kv in $new.GetEnumerator()) {
  $merged[$kv.Key] = if ($merged.ContainsKey($kv.Key)) {
    [math]::Round(0.5 * $merged[$kv.Key] + 0.5 * $kv.Value, 1)     # EMA: damp one-off noise
  } else { $kv.Value }
}

# Forget tests that are gone, so the weights file cannot grow forever.
$live = @(Get-ChildItem tools\coop_test\boot_check.py, tools\coop_test\test_*.py -ErrorAction SilentlyContinue |
          Select-Object -ExpandProperty BaseName)
$dropped = @()
if ($live.Count) {
  $dropped = @($merged.Keys | Where-Object { $live -notcontains $_ })
  foreach ($k in $dropped) { $merged.Remove($k) }
}

$ordered = [ordered]@{}
foreach ($k in ($merged.Keys | Sort-Object)) { $ordered[$k] = $merged[$k] }
$ordered | ConvertTo-Json -Depth 2 | Set-Content $Out -Encoding utf8

Write-Host ("merged {0} shard file(s): {1} measured, {2} -> {3} weights, {4} dropped" -f
            $files.Count, $new.Count, $before, $ordered.Count, $dropped.Count)
if ($dropped.Count) { Write-Host "  dropped: $($dropped -join ', ')" }
exit 0
