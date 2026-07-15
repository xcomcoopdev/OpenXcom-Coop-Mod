# PRD-01: Wait-dialog identity — name the CoopState codes, fix freeze-dialog stacking

**Fixes:** S3 (magic dialog codes + duplicated exclusion chain), C9 (freeze dialog
stacks over resume-ack dialog → double RESUME, double `campaign_begun`).

**Files:** `src/CoopMod/CoopState.h`, `src/CoopMod/CoopState.cpp`,
`src/CoopMod/connectionTCP.cpp`.

## Background

`CoopState` is a multi-purpose dialog whose behavior is selected by a raw int
code passed to its constructor (`getStateCode()` accessor in `CoopState.h`).
Codes in use include 52 (client "loading" wait), 54 (host "saving" wait),
60 (host wait-for-bases), 62 (resume-ack wait), 64 (mid-session freeze /
disconnect wait), 65 (client campaign hold), 666 (battle transfer), 994 (error
popup). Every push site writes the literal int, e.g. `new CoopState(60, ...)`.

### Verified defect S3 — duplicated magic exclusion chain

`connectionTCP.cpp:6723-6724` and `connectionTCP.cpp:6767-6768` (the
`close_save_progress` / `close_load_progress` message handlers) both contain the
verbatim chain:

```cpp
top->getStateCode() != 60 && top->getStateCode() != 62 &&
top->getStateCode() != 64 && top->getStateCode() != 65
```

with identical "campaign wait dialogs manage their own lifetime" comments. Any
future wait dialog must be added to both lists or one handler pops a live wait
dialog mid-wait (the exact bug family this branch fixed).

### Verified defect C9 — freeze dialog stacks over resume-ack dialog

The mid-session freeze dedup at `connectionTCP.cpp:9356-9361` (inside
`disconnectTCP`) inspects **only** `_game->getStates().back()` for code 64
before pushing a new CoopState(64). Sequence that breaks it:

1. Host clicks RESUME CAMPAIGN → CoopState(62) pushed
   (`LobbyMenu.cpp:558`, with `lobbyClosed=true`, resume `lobbyMode=2` — which
   satisfies the freeze-push condition `lobbyMode != 0 && lobbyClosed`).
2. Client connection drops before acking → `disconnectTCP` sees top=62 (not
   64) → pushes CoopState(64) **on top of** 62. Nothing ever pops the buried 62.
3. Client rejoins and acks → `session.resumeAck` set
   (`connectionTCP.cpp:2706`); the think() branch shared by both dialogs
   (`CoopState.cpp:776-786`) shows RESUME on **both**.
4. Host clicks RESUME on 64 → `CoopState::previous` (`CoopState.cpp:859-868`)
   broadcasts `campaign_begun` and pops — revealing 62, which also offers
   RESUME. Second click → **second `campaign_begun` broadcast**.

(Note: the CoopState(65) constructor already consumes a stale
`session.campaignBegun` — `CoopState.cpp:214-227` — so the duplicate broadcast
does not bypass a future hold; the user-visible bug is the stacked double
dialog and duplicate broadcast.)

## Required behavior

1. Dialog codes have names. A single predicate identifies campaign wait
   dialogs; both handler exclusion chains use it.
2. `disconnectTCP` never stacks a freeze dialog on top of another campaign wait
   dialog that already covers the same "wait for the player to come back"
   purpose. After a drop-during-resume-wait, exactly one dialog is on the
   stack, one RESUME click resumes, and `campaign_begun` is broadcast once.

## Implementation plan

1. **Enumerate codes.** `grep -n "CoopState(" src/` — list every constructor
   call and its code literal. Put the inventory in the commit body.
2. **Add an enum** in `CoopState.h` (plain `enum` or `enum class` with explicit
   values so nothing renumbers — the ints appear in logs and tests):
   ```cpp
   enum CoopDialogCode {
     COOP_DLG_CLIENT_LOAD_WAIT = 52,
     COOP_DLG_HOST_SAVE_WAIT   = 54,
     COOP_DLG_WAIT_BASES       = 60,
     COOP_DLG_RESUME_ACK_WAIT  = 62,
     COOP_DLG_FREEZE           = 64,
     COOP_DLG_CLIENT_HOLD      = 65,
     // ... every other code found in step 1, with a comment naming its use
   };
   ```
   Replace the raw literals at all push sites and in CoopState.cpp's own
   switch/if chains with the named constants. **Do not change any numeric
   value.**
3. **Add the predicate** to `CoopState`:
   ```cpp
   bool isCampaignWaitDialog() const; // true for 60, 62, 64, 65
   ```
   Replace both exclusion chains (`connectionTCP.cpp:6723`, `:6767`) with
   `!top->isCampaignWaitDialog()` (mind the surrounding logic's polarity — read
   each condition carefully before substituting).
4. **Fix the freeze dedup** in `disconnectTCP` (~9356): replace the
   `back()`-only check with a scan of the whole `_game->getStates()` stack. Do
   not push CoopState(64) if any `CoopState` with code `COOP_DLG_FREEZE` **or**
   `COOP_DLG_RESUME_ACK_WAIT` is already present — the 62 dialog already shows
   RESUME once `resumeAck` arrives, so it covers the freeze dialog's job.
   Add a `Log(LOG_INFO)` line when the push is skipped for this reason.

## Acceptance criteria

- `grep -rn "!= 60" src/` returns nothing (chain replaced).
- Build passes (serial msbuild, see README).
- `python tools/coop_test/boot_check.py` passes.
- `python tools/coop_test/test_rejoin_flow.py` passes (exercises freeze dialog).
- `python tools/coop_test/test_resume_flow.py` passes (exercises 62/60 dialogs).
- Optional but preferred: extend `test_rejoin_flow.py` with a
  drop-while-resume-waiting scenario — host in resume lobby clicks RESUME
  (CoopState 62 up), hard-kill client, assert host stack contains exactly one
  campaign wait dialog (use the `get_state` command to read the stack), rejoin
  client, ack, one RESUME, assert campaign resumes and geoscape unpauses.

## Out of scope

- Renumbering codes, changing dialog text, refactoring CoopState into
  subclasses (a fine future idea; not now).
