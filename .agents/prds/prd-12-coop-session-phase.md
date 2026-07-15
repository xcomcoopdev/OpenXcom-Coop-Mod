# PRD-12: Finish the CoopSession refactor — one encoding of the lifecycle, writes through transitions

**Fixes:** S4 (CONFIRMED: the `CoopPhase phase` enum is write-only — zero reads
outside CoopSession's own transition warnings — while ~30 call sites mutate
session fields raw, bypassing the logged transitions the struct exists for).

**Files:** `src/CoopMod/connectionTCP.cpp/.h` and every raw-write site listed
below.

**Do this PRD last.** It touches everything; all behavior fixes must already
be in.

## Verified facts

- `phase` is read only inside CoopSession's own methods
  (`connectionTCP.cpp:211/217/235/254/263/278`); `CoopPhase::` appears nowhere
  else. All routing keys off the mirrored booleans
  (`lobbyMode`, `clientInLobby`, `sessionLocked`, `lobbyClosed`,
  `campaignBegun`).
- Raw writes bypassing the transition methods (snapshot at `ff18e546d` —
  re-grep before editing, earlier PRDs moved some):
  `LoadGameState.cpp:408-410` (lobbyMode/sessionLocked/resumeAck),
  `NewGameState.cpp:196` (lobbyMode=1), `HostMenu.cpp:706` (lobbyMode
  re-derived from save), `CoopState.cpp:223/793` (campaignBegun),
  `SaveGameState.cpp:220` (pendingHostSaveName), `LobbyMenu.cpp`
  117/517/519/547/602/697/703/742/766/1120 (lobbyClosed, sessionLocked, etc. —
  697/1120 set `sessionLocked = true` raw even though `campaignStarted()`
  exists), `TestServer.cpp:1015`, `connection_udp_glue.cpp:320/405`,
  `connection_rendezvous_glue.cpp:336/456`, `setServerOwner`
  (`connectionTCP.cpp:8301`) writes `session.role` raw, plus ~14 sites inside
  `connectionTCP.cpp` itself.

## Decision (made): delete `phase`, consolidate writes into named transitions

Two exits from the dual encoding: (a) make `phase` authoritative and derive
the booleans, or (b) delete `phase` and keep the booleans as the single
encoding, with all multi-field writes funneled through named, logged
transition methods. **(b) is chosen**: zero routing risk (no predicate changes
semantics), and it preserves what the refactor actually wanted — named,
logged, coherent transitions. If the maintainer later wants a real state
machine, (a) can be built on top of the consolidated writers this PRD
produces.

## Implementation plan

1. **Inventory first.** `grep -n "session\.\(lobbyMode\|clientInLobby\|sessionLocked\|lobbyClosed\|campaignBegun\|resumeAck\|role\|pendingHostSaveName\) *=" src/`
   (adjust to the real field list in `connectionTCP.h:209`'s CoopSession).
   Classify every write by INTENT — e.g. "host opens lobby", "client joins
   lobby", "campaign locks", "resume save adopted", "session torn down".
   Put the intent table in the commit body.
2. **One method per intent** on CoopSession, absorbing the multi-field writes,
   each with the same `Log()` style the existing transitions use
   (`beginHosting`, `campaignStarted`, `freeze`, `resetSession` already
   exist — extend the family). Expected additions, judging by the intents at
   the listed sites (name them by what the code does, not this list):
   - `adoptSave(const SavedGame*)` — the LoadGameState/HostMenu "derive
     lobbyMode + locked identity from a loaded save" rule, currently
     duplicated at `LoadGameState.cpp:408-410` and `HostMenu.cpp:706`;
   - `enterLobbyAsHost()` / `enterLobbyAsClient()`;
   - `lobbyOpened()` / `lobbyClosed()` (the LobbyMenu:117/547/602 writes);
   - `armDeferredSave(name)` / `clearDeferredSave()` (SaveGameState:220);
   - `consumeCampaignBegun()` (CoopState:223/793).
   Single-field, single-intent writes with an obvious local meaning MAY stay
   raw if wrapping adds nothing — judge per site, bias toward wrapping
   anything touching two or more fields or written from more than one file.
3. **Delete the `phase` member** and the `CoopPhase` enum. Keep every
   transition method's log line (that was the phase's only value). Fold the
   old phase-mismatch warnings into coherence asserts inside the transitions
   where they still make sense (e.g. `campaignStarted()` warns if no lobby was
   open).
4. **setServerOwner** (`connectionTCP.cpp:8301`): route the `session.role`
   write through a transition (or into `beginHosting`/join flow) so role
   changes are logged like everything else.
5. Re-run the grep from step 1: remaining raw writes should be only the
   deliberately-left single-field cases; list them with one-line
   justifications in the commit body.

## Acceptance criteria

- Build + `boot_check.py`; **full suite green** — this PRD has the broadest
  blast radius; run every `test_*.py`.
- `grep -rn "CoopPhase" src/` returns nothing.
- Commit body: intent table + residual raw-write list with justifications.

## Out of scope

- Changing any predicate's truth value; adding new lifecycle states; the
  ICoopSession engine-decoupling facade (separate, pre-approved future work —
  do not start it here).
