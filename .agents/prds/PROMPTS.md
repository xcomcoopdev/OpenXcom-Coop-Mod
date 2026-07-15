# Session prompts for implementing the coop cleanup PRDs

Paste one prompt per fresh Claude Code session (Opus 4.8), in order. Each
prompt is self-contained. Do not run two sessions concurrently — every PRD
builds the same exe and the PRDs are ordered by dependency.

If a session ends with failing tests it cannot fix, have it commit nothing and
write its findings to `.agents/prds/session-notes-<n>.md`; start the next
session by pointing at that file.

---

## Session 1 — PRD-01 + PRD-02 + PRD-03 (mechanical consolidation)

```
Read .agents/prds/README.md fully and obey every global constraint in it
(especially: incremental serial msbuild only, never clean, no new .cpp files,
re-anchor cited line numbers by symbol).

Then implement, in order, committing after each:
1. .agents/prds/prd-01-wait-dialog-identity.md
2. .agents/prds/prd-02-shared-helpers.md
3. .agents/prds/prd-03-local-save-gate.md

Process for each PRD: read the PRD fully; read every cited function in the
repo before editing anything; if a cited symbol or behavior does not match
the PRD's description, STOP work on that item and record the discrepancy in
.agents/prds/session-notes-1.md instead of guessing. Implement exactly the
PRD scope. Build (msbuild src/OpenXcom.2010.sln /p:Configuration=Release
/p:Platform=x64 — serial, no /m). Run the PRD's named harness tests. Commit
with the branch's conventional style (refactor(coop): / fix(coop):), one
commit per PRD, listing in the body what the PRD's acceptance criteria
required.

After all three: run the full harness suite (every tools/coop_test/test_*.py,
serially) and report pass/fail per test.
```

## Session 2 — PRD-04 (session identity reset — the critical one)

```
Read .agents/prds/README.md fully and obey every global constraint in it.

Implement .agents/prds/prd-04-session-identity-reset.md. This fixes the three
highest-severity bugs on the branch; the PRD contains verified code anchors —
trust its facts, but re-read every cited site before editing (line numbers may
have drifted from Session 1; anchor by symbol).

Non-negotiables from the PRD: the freeze/rejoin path must NOT lose blobs
(resetSession is only for full teardown — verify callers before adding the
clears); resume must still adopt the LOADED saveID; the two new regression
scenarios (solo-after-coop, back-to-back campaigns) must be added to
tools/coop_test/test_session_hardening.py and must pass.

Build serially, run the new scenarios plus test_client_zero_disk.py,
test_resume_flow.py, test_rejoin_flow.py, then the full suite. Commit as
fix(coop) with the PRD's verification notes in the body. If anything cannot
be made green, commit nothing and write .agents/prds/session-notes-2.md.
```

## Session 3 — PRD-05 (structured blob store)

```
Read .agents/prds/README.md fully and obey every global constraint in it.

Implement .agents/prds/prd-05-blob-store.md. Start with the PRD's Step 0
inventory grep and paste the inventory into your working notes BEFORE
changing code; the refactor touches a save-format schema (decision already
made in the PRD: new schema, no back-compat, dev saves invalidated — record
that in the commit body).

Keep the TestServer has_coop_file command and tools/coop_test working in
lockstep (the PRD tells you where the harness builds blob keys). Build
serially; run test_client_zero_disk.py, test_resume_flow.py,
test_rejoin_flow.py, test_transfer_rollback.py, then the full suite. Commit
as refactor(coop). If blocked, commit nothing and write
.agents/prds/session-notes-3.md.
```

## Session 4 — PRD-06 + PRD-07 (save-cycle integrity)

```
Read .agents/prds/README.md fully and obey every global constraint in it.

Implement, in order, committing after each:
1. .agents/prds/prd-06-deferred-save-integrity.md
2. .agents/prds/prd-07-blob-validation-rollback.md

Both PRDs change WHEN existing writes happen, not what is written — before
editing, read each cited function end-to-end and write down (in your notes)
the current order of operations, then the target order. The PRD-06 regression
scenarios (abort-on-load, cancel-writes) must be added and green; PRD-07's
acceptance is primarily the code-order proof in the commit body plus a green
suite.

Build serially after each PRD; run the named tests; full suite at the end.
Commits: fix(coop). If blocked: .agents/prds/session-notes-4.md.
```

## Session 5 — PRD-08 + PRD-09 (load/restore policy)

```
Read .agents/prds/README.md fully and obey every global constraint in it.

Implement, in order, committing after each:
1. .agents/prds/prd-08-host-quickload-policy.md — note the policy decision is
   already made in the PRD (block, don't resync); part of the work is
   flipping an existing expectation in test_session_hardening.py.
2. .agents/prds/prd-09-mission-end-restore.md — battle flows are the least
   automated area; if the optional test_battle_resume_fresh.py proves
   infeasible with the existing battle driver commands, implement the code
   changes + logging, keep the suite green, and write the manual test
   checklist into the commit body exactly as the PRD specifies.

Build serially after each; run test_resume_flow.py, test_rejoin_flow.py,
test_session_hardening.py per PRD; full suite at the end. Commits: fix(coop).
If blocked: .agents/prds/session-notes-5.md.
```

## Session 6 — PRD-10 + PRD-11 (lobby gate + network guards)

```
Read .agents/prds/README.md fully and obey every global constraint in it.

Implement, in order, committing after each:
1. .agents/prds/prd-10-lobby-start-gate.md — includes TestServer command work
   so the regression test exercises the REAL confirm dialog; read the
   existing lobby_start_campaign handler before adding the variant.
2. .agents/prds/prd-11-network-guards.md — three independent guards; read the
   verified mechanics in the PRD, then the cited code, before each.

Build serially after each; run test_lobby_gating.py,
test_new_campaign_flow.py, test_rejoin_flow.py (3x back-to-back for the
timing window), test_resume_flow.py; full suite at the end. Commits:
fix(coop). If blocked: .agents/prds/session-notes-6.md.
```

## Session 7 — PRD-12 (CoopSession phase — last, widest blast radius)

```
Read .agents/prds/README.md fully and obey every global constraint in it.

Implement .agents/prds/prd-12-coop-session-phase.md. The decision is already
made in the PRD: DELETE the phase enum, funnel multi-field session writes
through named logged transition methods. Do the Step-1 inventory grep first
and build the intent table before touching any code; the PRD's site list is a
snapshot and earlier sessions moved some sites.

This PRD must not change any predicate's truth value — after each batch of
sites, build and run boot_check.py plus one fast coop test
(test_geoscape_sync.py) to catch regressions early. Finish with the FULL
suite, every test. Commit as refactor(coop) with the intent table and
residual raw-write justifications in the body. If blocked:
.agents/prds/session-notes-7.md.
```

## Session 8 — PRD-13 (harness cleanup — can also run earlier)

```
Read .agents/prds/README.md fully and obey every global constraint in it.

Implement .agents/prds/prd-13-harness-cleanup.md. Classify every TestServer
scan site BEFORE converting (the PRD explains find-first vs find-last vs
top-only — semantics must not change); then do the Python dedup. Build
serially, then run the ENTIRE harness suite — these files are the suite, so
every single test_*.py must pass. Commit as test(coop) with the site
classification table in the body.
```

---

## After all sessions

Run the full suite one final time, then review the combined diff
(`git diff <pre-session-1-commit>...HEAD --stat`) against the PRD index in
README.md — every PRD should map to at least one commit. The battle-resume
manual checklist from Session 5 (if the automated test was infeasible) still
needs a human pass before merging.
