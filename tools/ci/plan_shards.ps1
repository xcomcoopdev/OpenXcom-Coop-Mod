# Splits the coop suite into N shards of roughly equal RUNTIME (not equal test count)
# and writes the assignment to a JSON plan the shard jobs consume.
#
# Weights come from the previous CI run's measured durations, restored from the
# Actions cache (-Timings); tools/ci/test_weights.json is the checked-in fallback for
# a cold cache. A test with no recorded weight (i.e. one added since the last run)
# gets the median, which is close enough - greedy LPT tolerates large weight error.
#
# The plan is computed ONCE per run, here, and handed to every shard, so all shards
# agree on the split even if a concurrent run refreshes the cache mid-flight.
#
# Usage: ./tools/ci/plan_shards.ps1 [-Of 4] [-Timings <json>] [-Out shard-plan.json]
param(
  [int]$Of = 4,
  [string]$Timings,
  [string]$Out = "shard-plan.json"
)
$ErrorActionPreference = "Stop"

# Same discovery the suite uses, so the plan can never disagree with the runner.
$tests = Get-ChildItem tools\coop_test\boot_check.py, tools\coop_test\test_*.py |
         Select-Object -ExpandProperty BaseName | Sort-Object

$weights = @{}
$source = "none"
foreach ($f in @($Timings, "tools\ci\test_weights.json")) {
  if ($f -and (Test-Path $f)) {
    (Get-Content $f -Raw | ConvertFrom-Json).PSObject.Properties |
      ForEach-Object { $weights[$_.Name] = [double]$_.Value }
    $source = $f
    break
  }
}

$known = @($tests | Where-Object { $weights.ContainsKey($_) } | ForEach-Object { $weights[$_] })
$median = if ($known.Count) { ($known | Sort-Object)[[int]($known.Count / 2)] } else { 10.0 }
$unknown = @($tests | Where-Object { -not $weights.ContainsKey($_) })

# Greedy LPT: heaviest test first into the lightest shard. Within ~1% of optimal
# here because the longest single test is a small fraction of a shard.
$bins = @(1..$Of | ForEach-Object { [pscustomobject]@{ Shard = $_; Seconds = 0.0; Tests = @() } })
foreach ($t in ($tests | Sort-Object @{ Expression = { if ($weights.ContainsKey($_)) { $weights[$_] } else { $median } } } -Descending)) {
  $w = if ($weights.ContainsKey($t)) { $weights[$t] } else { $median }
  $b = $bins | Sort-Object Seconds | Select-Object -First 1
  $b.Seconds += $w
  $b.Tests += $t
}

$plan = [ordered]@{
  of      = $Of
  source  = $source
  median  = $median
  unknown = $unknown
  shards  = [ordered]@{}
}
foreach ($b in $bins) { $plan.shards["$($b.Shard)"] = $b.Tests }
$plan | ConvertTo-Json -Depth 5 | Set-Content $Out -Encoding utf8

$span = ($bins | Measure-Object Seconds -Maximum).Maximum - ($bins | Measure-Object Seconds -Minimum).Minimum
Write-Host ("plan: {0} tests -> {1} shards, weights from {2}" -f $tests.Count, $Of, $source)
if ($unknown.Count) { Write-Host ("  {0} test(s) with no recorded time, assumed {1}s: {2}" -f $unknown.Count, $median, ($unknown -join ', ')) }
foreach ($b in $bins) { Write-Host ("  shard {0}: {1,2} tests, ~{2,6:N0}s" -f $b.Shard, $b.Tests.Count, $b.Seconds) }
Write-Host ("  slowest-vs-fastest spread: {0:N0}s" -f $span)

if ($env:GITHUB_STEP_SUMMARY) {
  $md = @("## Shard plan ($($tests.Count) tests, weights from $source)", "",
          "| Shard | Tests | Estimated |", "| --- | ---: | ---: |")
  foreach ($b in $bins) { $md += "| $($b.Shard) | $($b.Tests.Count) | $([math]::Round($b.Seconds))s |" }
  if ($unknown.Count) { $md += ""; $md += "New/unweighted tests assumed ${median}s: $($unknown -join ', ')" }
  $md -join "`n" | Add-Content $env:GITHUB_STEP_SUMMARY
}
exit 0
