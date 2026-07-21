# Runs the coop test suite headless (set SDL_VIDEODRIVER/SDL_AUDIODRIVER=dummy in the
# caller's env). Exits nonzero if any non-quarantined test fails. Shared by both CI
# workflows so the quarantine list and retry policy live in exactly one place.
# Per-test durations go to the console and, on CI, to the run's Summary page
# ($GITHUB_STEP_SUMMARY) as a slowest-first table.
#
# CI runs this sharded: tools/ci/plan_shards.ps1 splits the suite by measured runtime
# and each shard job passes -PlanFile/-Shard. -TimingsOut writes this shard's measured
# durations, which the coop-tests job merges back into the cache that feeds the next
# run's plan, so a new test gets a real weight after one run.
#
#   ./tools/ci/run_coop_suite.ps1                                   # whole suite
#   ./tools/ci/run_coop_suite.ps1 -PlanFile p.json -Shard 2         # one shard
#   ./tools/ci/run_coop_suite.ps1 -PlanFile p.json -Shard 2 -ListOnly
param(
  [string]$PlanFile,
  [int]$Shard,
  [string]$TimingsOut,
  [switch]$ListOnly
)
$ErrorActionPreference = "Stop"

# Discover the checkout's actual harness so the suite never goes stale.
$tests = Get-ChildItem tools\coop_test\boot_check.py, tools\coop_test\test_*.py |
         Select-Object -ExpandProperty BaseName | Sort-Object

if ($PlanFile) {
  if (-not $Shard) { throw "-PlanFile requires -Shard" }
  $plan = Get-Content $PlanFile -Raw | ConvertFrom-Json
  $mine = @($plan.shards."$Shard")
  if (-not $mine) { throw "shard $Shard is not in $PlanFile (of=$($plan.of))" }
  # The plan comes from the same discovery on the same commit; if it does not, some
  # test belongs to no shard and would silently go untested. Fail loudly instead.
  $planned = @($plan.shards.PSObject.Properties.Value | ForEach-Object { $_ })
  $missing = @($tests | Where-Object { $planned -notcontains $_ })
  if ($missing.Count) { throw "stale shard plan; test(s) assigned to no shard: $($missing -join ', ')" }
  $tests = @($tests | Where-Object { $mine -contains $_ })
  Write-Host "shard $Shard/$($plan.of): $($tests.Count) test(s)"
}

if ($ListOnly) { $tests; exit 0 }   # to stdout, so callers can diff the shard split

# Known-broken on main (real failures, not flakes) - run but do not gate.
# Add entries here if a test regresses; remove them as they are fixed so they gate
# again. Empty = the whole suite gates (all green as of 2026-07-15).
$quarantine = @()

$results = @()
$fail = 0
foreach ($t in $tests) {
  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  python "tools\coop_test\$t.py"; $rc = $LASTEXITCODE
  $attempts = 1
  $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
  if ($rc -ne 0) {
    $sw.Restart()
    python "tools\coop_test\$t.py"; $rc = $LASTEXITCODE   # retry once (flake tolerance)
    $attempts = 2
    $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)    # time the LAST attempt only:
  }                                                       # a retry must not inflate the
  $sw.Stop()                                              # weight the next plan uses

  if ($rc -eq 0)                    { $status = "PASS" }
  elseif ($quarantine -contains $t) { $status = "KNOWN-FAIL" }
  else                              { $status = "FAIL"; $fail++ }

  $note = @()
  if ($attempts -gt 1)              { $note += "retried" }
  if ($status -eq "KNOWN-FAIL")     { $note += "quarantined" }
  if ($rc -ne 0)                    { $note += "rc=$rc" }
  $suffix = if ($note) { " ($($note -join ', '))" } else { "" }
  Write-Host ("{0,-11} {1,8:N1}s  {2}{3}" -f $status, $secs, $t, $suffix)

  $results += [pscustomobject]@{ Test = $t; Status = $status; Seconds = $secs; Attempts = $attempts; Rc = $rc }
}

# Feeds the next run's plan. PASSes only: a failing test bails out early and its
# duration says nothing about how long the test actually takes.
if ($TimingsOut) {
  $timings = [ordered]@{}
  foreach ($r in ($results | Where-Object { $_.Status -eq "PASS" } | Sort-Object Test)) { $timings[$r.Test] = $r.Seconds }
  $timings | ConvertTo-Json -Depth 2 | Set-Content $TimingsOut -Encoding utf8
  Write-Host "wrote $($timings.Count) timing(s) to $TimingsOut"
}

# Duration table on the Actions run Summary page, slowest first, so the tests
# dominating CI wall time are visible without opening the logs.
if ($env:GITHUB_STEP_SUMMARY) {
  $total = [math]::Round(($results | Measure-Object Seconds -Sum).Sum, 1)
  $title = if ($PlanFile) { "Coop test durations - shard $Shard (total $($total)s)" } else { "Coop test durations (total $($total)s)" }
  $md = @("## $title", "",
          "| Test | Status | Duration | Attempts |", "| --- | --- | ---: | ---: |")
  foreach ($r in ($results | Sort-Object Seconds -Descending)) {
    $md += "| $($r.Test) | $($r.Status) | $($r.Seconds)s | $($r.Attempts) |"
  }
  $md -join "`n" | Add-Content $env:GITHUB_STEP_SUMMARY
}

if ($fail -gt 0) { throw "$fail non-quarantined test(s) failed" }
exit 0
