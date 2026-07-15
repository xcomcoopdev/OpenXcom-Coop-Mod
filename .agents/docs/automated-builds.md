# Automated builds (GitHub Actions + self-hosted runner) — state & handoff

Last updated 2026-07-15. Owner-facing: this documents the CI pipeline, the
self-hosted build VM, and an in-progress bug investigation. A follow-up session
will fix the failing tests + the #38 regressions.

## 1. What exists (branches on origin = `OpenXcom-Coop/OpenXcom-Coop-Mod`)

All pushed, **none merged yet** — awaiting review/merge in this order:

1. **`fix/main-compile`** — declares `TransferItemsState::createPendingTransfers()`
   in the header. `origin/main` does not MSBuild without it (`C2039: not a member`).
   NOTE: the offending method was actually added by **PR #38**, not #39 (commit
   message says #39 — mislabeled). If #38 is reverted/fixed, this branch is moot.
2. **`fix/coop-version-from-build`** — coop `gameVersion` now comes from
   `OPENXCOM_VERSION_LONG` (the built exe) instead of a `gameVersion` field in
   `rendezvous.json`. Removed that field from `rendezvous_config.cpp` parsing;
   the two config accessors return the build version. Full-version compat
   (two builds must match exactly to matchmake — user's choice).
3. **`ci/main-pipeline`** — `.github/workflows/ci-main.yml` + `CHANGELOG.md`.
   Merge LAST; merging to `main` fires the first real nightly.

Throwaway branches already deleted: `ci/runner-smoke`, `verify/combined`.

## 2. The pipeline (`ci-main.yml`, on `ci/main-pipeline`)

- Triggers: push to `main` (rolling **nightly** prerelease), push tag `v*`
  (versioned **release**, marked Latest), `workflow_dispatch`.
- `runs-on: [self-hosted, windows, coop-build]`, `permissions: contents: write`,
  `concurrency: ci-main` (no overlap; tests are stateful).
- Steps: checkout → setup-msbuild → **stamp `src/version.h`** → build x64 Release
  (serial; `/m` OOMs the 2-vCPU VM) → **stage data** → **coop test suite** (gate)
  → package zip → move rolling `nightly` tag → publish (softprops/action-gh-release).
- **Version stamping** (build-time edit of `version.h`, not committed):
  - Release: version = tag (`v8.4.3` → `8.4.3`), `OPENXCOM_VERSION_GIT=" (v8.4.3)"`.
  - Nightly: patch = `<last-tag patch> + <commits since tag>` (git-describe),
    `OPENXCOM_VERSION_GIT=" (nightly <date> <sha>)"`.
- **Release notes**: from the matching `## [x.y.z]` section of `CHANGELOG.md`
  (release); auto-generated for nightlies. Add the section + merge to main
  BEFORE tagging a release. Have the agent author the changelog from commits.
- **Data staging**: robocopy `deps/lib/x64/*.dll`, `bin/standard`, `bin/common`,
  `C:\oxc-data\UFO` (proprietary, runner-stored), and repo `bin/UFO/multiplayer`
  into `bin/x64/Release`.
- **Test step**: globs `tools/coop_test/{boot_check,test_*}.py` (never stale),
  **retry-once** per test (2-core flake tolerance), and **quarantines** the
  currently-broken tests (run+report, non-gating). Remove from the `$quarantine`
  array as they are fixed. Runner user needs `python` + `pwsh` on PATH.

## 3. The build VM (self-hosted runner)

- `ssh mcservers` — Win Server 2019, **2 vCPU** (clean build ~45-60 min), VS2022
  Community + **BuildTools at `C:\BuildTools`** (MSVC 14.44), Git, Python 3.12,
  **PowerShell 7** (workflows use `shell: pwsh`). See [[azure-ci-build-machine]].
- Runner registered to the repo, name `NonPolynomialTimsAzureVM`, labels
  `self-hosted,Windows,X64,coop-build`, **workFolder `D:\actions-work`**.
- Persistence = **SYSTEM boot scheduled task `github-runner`** running
  `C:\actions-runner\run.cmd` (NOT a Windows service — chosen so no password over
  SSH). Survives reboot/logoff. `python`+`pwsh` are on the **machine** PATH so the
  SYSTEM listener resolves them. Restart gotcha: hard-killing `Runner.Listener`
  leaves a stale GitHub session → new instance loops on `Conflict` ~1-2 min then
  self-heals; jobs orphaned by a mid-restart kill stay Queued (push a fresh run).
- Defender exclusions: `C:\oxc*`, `C:\BuildTools`, `C:\actions-runner`,
  `D:\actions-work` (~2.6x build speed).
- Smoke test green; the runner works end-to-end.

## 4. Runner-local vs checked-in (recap)

`origin/main` was NOT clean-buildable via MSBuild (only CMake/Linux built). The
`fix/msbuild-clean-checkout-build` PR (already merged to origin/main) fixed:
libsodium (in-repo lib+include), 15 CoopMod `.cpp` missing from the vcxproj,
**jsoncpp compiled from source** (`deps/src/jsoncpp/`, dropped the /MT prebuilt
DLL — it crashed under MSVC 14.44), `multiplayer/*.png` assets, a CrashHandler
stack-walk (superseded by a fuller rewrite already on main). Proprietary UFO/TFTD
data stays runner-stored (`C:\oxc-data\`).

## 5. IN-PROGRESS: failing coop tests (for the next session)

On current `origin/main`, **5 coop tests fail (real bugs, confirmed on both the
VM and a fast local machine — NOT timing)**:

| Test | Cause (bisected) | Repro when #38 reverted |
|---|---|---|
| `test_coop_transfer_equipment_counts` | **PR #38** (purchase-sync-fixes) | PASS |
| `test_coop_transfer_equipment_option` | **PR #38** | PASS |
| `test_coop_transferred_equipment` | **PR #38** | PASS |
| `test_lobby_dialogs` | earlier commit (NOT #38) | still FAIL |
| `test_session_hardening` | earlier commit (NOT #38) | still FAIL |

### #38 = `1768603a4` "Merge pull request #38 from OpenXcom-Coop/purchase-sync-fixes"
- Changed: `TransferItemsState.cpp` (+252), `Base.cpp` (+240), `connectionTCP.cpp`,
  `CoopState.cpp`, `PurchaseState.cpp`, `TransferConfirmState.cpp`, `Base.h`.
- Added `createPendingTransfers()` to the .cpp + a call in `TransferConfirmState`
  but not to the header → the MSBuild compile break.
- **Confirmed root cause of the 3 equipment failures**: reverting the #38 merge
  (`git revert -m 1 1768603a4`) → those 3 PASS. Symptom (from harness):
  `TimeoutError: timed out waiting for all gear stored at receiving base`.
- Decision pending (owner): revert #38 on main, OR fix its transfer-with-gear
  sync regression in place (keeping the purchase-sync improvements).

### `test_lobby_dialogs` / `test_session_hardening`
- Fail even with #38 reverted → a DIFFERENT/earlier commit. Not yet bisected.
- `test_lobby_dialogs` symptom: `TimeoutError: timed out waiting for client
  browser` (client `ServerList` state never appears in the lobby flow).

### Local repro setup (fast iteration; the VM is 2 vCPU)
- Build: `& "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe" src\OpenXcom.2010.sln /m /p:Configuration=Release /p:Platform=x64`
- Local python: `C:\Users\bentl\AppData\Local\Programs\Python\Python312\python.exe`
- Stage next to exe (`bin\x64\Release`): robocopy `deps\lib\x64\*.dll`,
  `bin\standard`, `bin\common`, UFO from
  `C:\Program Files (x86)\Steam\steamapps\common\XCom UFO Defense\XCOM`,
  and `bin\UFO\multiplayer`.
- Run headless: set `SDL_VIDEODRIVER=dummy` + `SDL_AUDIODRIVER=dummy`, then
  `python tools\coop_test\<test>.py`. rc=0 pass; rc!=0 fail. Write logs to the
  scratchpad, NOT `C:\` root (non-elevated user can't write there).
- Existing local scratch trees: `C:\oxc-revert38` (main with #38 reverted, built,
  3 equipment tests already green) and `C:\oxc-localverify` (main + the 3 fix
  branches). Both can be deleted/rebuilt.
- Full harness on origin/main = ~23 tests (transfer→**gift** renamed by #39;
  `test_gift_fresh/rollback`). 18 pass, the 5 above fail.

## 6. Open decisions for the owner
1. #38: revert vs fix-in-place vs hand to #38 author.
2. `lobby_dialogs` + `session_hardening`: bisect to find the breaking commit.
3. Merge order once green: `fix/main-compile` (or #38 fix) → `fix/coop-version-from-build` → `ci/main-pipeline`.
4. GitHub org "Workflow permissions" default is greyed/read-only at repo level;
   the pipeline declares `permissions: contents: write` which should override,
   but confirm on the first nightly (else org-level toggle or a PAT secret).
