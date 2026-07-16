# Runs the coop test suite headless (set SDL_VIDEODRIVER/SDL_AUDIODRIVER=dummy in the
# caller's env). Exits nonzero if any non-quarantined test fails. Shared by both CI
# workflows so the quarantine list and retry policy live in exactly one place.

# Discover the checkout's actual harness so the suite never goes stale.
$tests = Get-ChildItem tools\coop_test\boot_check.py, tools\coop_test\test_*.py |
         Select-Object -ExpandProperty BaseName | Sort-Object

# Known-broken on main (real failures, not flakes) - run but do not gate.
# Add entries here if a test regresses; remove them as they are fixed so they gate
# again. Empty = the whole suite gates (all green as of 2026-07-15).
$quarantine = @()

$fail = 0
foreach ($t in $tests) {
  python "tools\coop_test\$t.py"; $rc = $LASTEXITCODE
  if ($rc -ne 0) { python "tools\coop_test\$t.py"; $rc = $LASTEXITCODE }   # retry once (flake tolerance)
  if ($rc -eq 0)                    { Write-Host "PASS        $t" }
  elseif ($quarantine -contains $t) { Write-Host "KNOWN-FAIL  $t (quarantined, rc=$rc)" }
  else                              { Write-Host "FAIL        $t (rc=$rc)"; $fail++ }
}
if ($fail -gt 0) { throw "$fail non-quarantined test(s) failed" }
exit 0
