# PRD-07: Validate client blobs BEFORE installing them; never displace the last good blob

**Fixes:** C10 (CONFIRMED: an invalid client blob permanently displaces the last
good one; main's validation-gates-persistence safety was removed).

**Files:** `src/CoopMod/connectionTCP.cpp`.

## Verified mechanics (current behavior)

`writeHostMapSaveProgressFile` (`connectionTCP.cpp:9534-9595`):

1. Installs the incoming client blob into `coopFilesHost[hostBlobKey]` and
   erases the client's other entries (`eraseStaleBlobEntries`) at `:9544-9545`
   — **before any validation**.
2. Then validates (parses the blob into a SavedGame; sanity checks — base with
   empty name, lon/lat 0,0, load errors).
3. On validation failure: pushes the CoopState(994) error popup and returns
   false (`:9583-9595`) — **no rollback**. The previous good blob is gone
   (overwritten by the same-key assignment; siblings erased).
4. Downstream, nothing re-validates: the resume path streams
   `coopFilesHost[hostBlobKey(client)]` unvalidated (`:3245-3282`), and
   `SavedGame::save` embeds any non-empty blob (`SavedGame.cpp:1504-1541`).
   The only softening is that the caller skips the immediate host re-save when
   `stored == false` (`:6784`) — which merely delays persistence; any later
   manual save or autosave embeds the poisoned blob.

On main, the in-memory install was also pre-validation, but validation gated
the **disk** write (`coopFile->save`, main `connectionTCP.cpp:9075-9080`) and
the resume path served from the on-disk `.data` file — so the last good copy
stayed authoritative. The branch (RAM-only blobs) removed that property.

## Required behavior

A blob that fails validation must leave the store exactly as it was: the
previous good blob remains served, embedded, and streamable. The error popup
still appears.

## Implementation plan

1. Reorder `writeHostMapSaveProgressFile`:
   - Parse + run ALL existing sanity checks on the incoming blob **first**,
     into locals. Reuse the exact checks the function already performs — no new
     validation rules, no removed ones.
   - Only on success: install into the store + prune stale entries (or, after
     PRD-05, assign the per-client entry).
   - On failure: push the existing CoopState(994) error, log which check
     failed and the offending player name, return false. Store untouched.
2. Audit the function's other side effects (the review noted the validation
   step itself produces a normalized re-emit — RNG seed re-stamp, host header;
   see also `saveCoopToMemory` mechanics). Whatever transformed form the
   current code stores on success, keep storing exactly that — this PRD only
   moves WHERE the install happens relative to validation, not what is stored.
3. Check the sibling path `writeHostMapFile` (`connectionTCP.cpp:~9513`, the
   battle-sync variant) for the same install-before-validate ordering; if it
   has it, apply the same reorder there and say so in the commit body.

## Acceptance criteria

- Build + `boot_check.py`.
- Full suite green — most relevant: `test_transfer_rollback.py`,
  `test_client_zero_disk.py`, `test_resume_flow.py`.
- Code inspection (put the before/after order in the commit body): validation
  → install, and the failure path provably leaves the map untouched (no write
  before the first early-return).
- No harness scenario can easily inject a corrupt blob today; if a cheap
  TestServer hook exists (e.g. a debug command to corrupt the next outgoing
  blob), add one and a scenario — otherwise document manual verification:
  hex-edit a client blob in a debugger or skip, citing the code-order proof.

## Out of scope

- New validation rules; changing the 994 error UX; blob store structure
  (PRD-05 handles that — if PRD-05 landed first, adapt mechanically).
