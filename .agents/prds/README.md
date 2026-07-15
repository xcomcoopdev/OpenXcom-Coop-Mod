# Coop cleanup PRDs — index and ground rules

Thirteen PRDs covering every verified finding from the 2026-07-11 review of
`feat/host-authoritative-save` (review baseline: commit `ff18e546d`). Each PRD is
self-contained: context, verified defect(s) with code anchors, required behavior,
implementation plan, acceptance criteria.

## Read this first — global constraints (apply to EVERY PRD)

1. **Line numbers are a snapshot at `ff18e546d`.** Earlier PRDs shift later line
   numbers. Always re-anchor by the quoted symbol/function name and surrounding
   code, never trust a raw line number. If a cited symbol is missing, stop and
   re-read the file before assuming the PRD is wrong.
2. **Never clean-build.** The vcxproj omits several coop `.cpp` files; the tree
   builds only via existing incremental `obj/`. A clean or fresh checkout fails
   with LNK2019. Build incrementally:
   ```
   msbuild src/OpenXcom.2010.sln /p:Configuration=Release /p:Platform=x64
   ```
   Serial only — **do not pass `/m`** (parallel full builds die with C1060
   out-of-heap). Output: `bin/x64/Release/OpenXcom.exe`.
3. **Do not add new `.cpp` files.** Same vcxproj trap. Put new helpers in
   existing `.cpp`/`.h` files (each PRD names the target file).
4. **Test harness** (`tools/coop_test/`, spec in
   `.agents/docs/coop-test-harness.md`): every test is a standalone script that
   spawns real game instances (in-game TestServer via `OXC_TEST_PORT`):
   ```
   python tools/coop_test/boot_check.py            # smoke, run after every build
   python tools/coop_test/test_<name>.py           # targeted
   ```
   Full suite = every `test_*.py` in `tools/coop_test/`. Run the PRD's named
   targeted tests during development and the full suite before committing.
5. **Scope discipline.** Implement exactly the PRD. If you find an adjacent bug,
   note it in the commit body; do not fix it.
6. **Commits**: conventional style used on this branch — `fix(coop): ...`,
   `refactor(coop): ...`, `test(coop): ...`. One commit per PRD minimum.
7. **Branch**: work on top of `feat/host-authoritative-save` (these defects live
   in that branch's new code; its PR has not merged).

## PRD list and required order

Dependencies flow downward; do not reorder across phases.

| # | File | Fixes | Type |
|---|------|-------|------|
| 01 | prd-01-wait-dialog-identity.md | C9, S3 | mechanical + bug |
| 02 | prd-02-shared-helpers.md | S1, S2, S5 | mechanical |
| 03 | prd-03-local-save-gate.md | S9 | mechanical + bug |
| 04 | prd-04-session-identity-reset.md | C1, C2, C3 | **critical bugs** |
| 05 | prd-05-blob-store.md | S8, E3 | refactor + data-loss bug |
| 06 | prd-06-deferred-save-integrity.md | C5, E1 | bug + perf |
| 07 | prd-07-blob-validation-rollback.md | C10 | bug |
| 08 | prd-08-host-quickload-policy.md | C7 | bug (policy decision inside) |
| 09 | prd-09-mission-end-restore.md | C12 | bug |
| 10 | prd-10-lobby-start-gate.md | C4 | bug |
| 11 | prd-11-network-guards.md | C13, C8, spoofed server_full | hardening |
| 12 | prd-12-coop-session-phase.md | S4 | refactor (last — touches everything) |
| 13 | prd-13-harness-cleanup.md | S6, S7 | test-only (safe any time) |

Suggested session grouping (one Claude session each):
- **Session 1:** PRD-01 + PRD-02 + PRD-03
- **Session 2:** PRD-04
- **Session 3:** PRD-05
- **Session 4:** PRD-06 + PRD-07
- **Session 5:** PRD-08 + PRD-09
- **Session 6:** PRD-10 + PRD-11
- **Session 7:** PRD-12
- **Session 8:** PRD-13 (or fold into any earlier session)

Ready-to-paste prompts for each session: `PROMPTS.md` in this directory.

## Finding-ID key

IDs (C1..C13, S1..S9, E1..E5) refer to the 2026-07-11 review. Each PRD restates
the relevant findings in full, so you never need the original review transcript.
