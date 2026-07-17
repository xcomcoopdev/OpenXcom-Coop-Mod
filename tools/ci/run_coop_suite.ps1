# Runs the coop test suite headless (set SDL_VIDEODRIVER/SDL_AUDIODRIVER=dummy in the
# caller's env). Exits nonzero if any non-quarantined test fails. Shared by both CI
# workflows so the quarantine list and retry policy live in exactly one place.
# Per-test durations go to the console and, on CI, to the run's Summary page
# ($GITHUB_STEP_SUMMARY) as a slowest-first table.

# Discover the checkout's actual harness so the suite never goes stale.
$tests = Get-ChildItem tools\coop_test\boot_check.py, tools\coop_test\test_*.py |
         Select-Object -ExpandProperty BaseName | Sort-Object

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
  if ($rc -ne 0) {
    python "tools\coop_test\$t.py"; $rc = $LASTEXITCODE   # retry once (flake tolerance)
    $attempts = 2
  }
  $sw.Stop()

  if ($rc -eq 0)                    { $status = "PASS" }
  elseif ($quarantine -contains $t) { $status = "KNOWN-FAIL" }
  else                              { $status = "FAIL"; $fail++ }

  $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
  $note = @()
  if ($attempts -gt 1)              { $note += "retried" }
  if ($status -eq "KNOWN-FAIL")     { $note += "quarantined" }
  if ($rc -ne 0)                    { $note += "rc=$rc" }
  $suffix = if ($note) { " ($($note -join ', '))" } else { "" }
  Write-Host ("{0,-11} {1,8:N1}s  {2}{3}" -f $status, $secs, $t, $suffix)

  $results += [pscustomobject]@{ Test = $t; Status = $status; Seconds = $secs; Attempts = $attempts; Rc = $rc }
}

# Duration table on the Actions run Summary page, slowest first, so the tests
# dominating CI wall time are visible without opening the logs.
if ($env:GITHUB_STEP_SUMMARY) {
  $total = [math]::Round(($results | Measure-Object Seconds -Sum).Sum, 1)
  $md = @("## Coop test durations (total $($total)s)", "",
          "| Test | Status | Duration | Attempts |", "| --- | --- | ---: | ---: |")
  foreach ($r in ($results | Sort-Object Seconds -Descending)) {
    $md += "| $($r.Test) | $($r.Status) | $($r.Seconds)s | $($r.Attempts) |"
  }
  $md -join "`n" | Add-Content $env:GITHUB_STEP_SUMMARY
}

if ($fail -gt 0) { throw "$fail non-quarantined test(s) failed" }
exit 0
