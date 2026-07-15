# Follow-up: bugs introduced by PR #38 (`purchase-sync-fixes`)

**Author:** review session, 2026-07-15
**For:** the agent iterating on CI / automated builds who now owns cleanup of this merge.
**Status:** PR #38 **has landed in `main`**. Two real defects + several minor issues remain. Fixes are **not** implemented — this is a handoff so they can be cleaned up.

---

## 1. What landed and where

- Merge commit: `1768603a4` — *"Merge pull request #38 from OpenXcom-Coop/purchase-sync-fixes"*, now the tip of `origin/main`.
- The branch merged `main` into itself first (`f83d4c7ea Merge branch 'main' into purchase-sync-fixes`), so `main` now contains **both** the host-authoritative save work **and** this purchase/transfer rework.
- The single functional commit is `3e99215b4` — *"Item quantity synchronization after purchases has been fixed…"*.

The PR reworks the **co-op goods/craft/purchase transfer protocol** (buying into a co-op base and transferring items/craft/staff between a local base and a co-op base). Files touched:

| File | What changed |
|------|--------------|
| `src/Basescape/PurchaseState.cpp` | Co-op purchase now sends `state:"purchase"` w/ `base_to_id`+`total_funds`; funds deferred to peer ACK; **the real fix** (see below). |
| `src/Basescape/TransferItemsState.cpp` / `.h` | New `createPendingTransfers()`; old co-op block removed from `completeTransfer()`. |
| `src/Basescape/TransferConfirmState.cpp` | Co-op confirm now calls `createPendingTransfers()` instead of `completeTransfer()`. |
| `src/Savegame/Base.cpp` / `.h` | New `removePendingTransfers()` (validate-then-remove) and `decreaseCoopTransferLimits()`. |
| `src/CoopMod/connectionTCP.cpp` | New handshake states `transfer_completed` / `purchase_completed` / `transfer_failed` / `purchase_failed`; base lookup name→`_coop_base_id`. |
| `src/CoopMod/CoopState.cpp` | New error dialogs 551/552/553. |

**The legitimate fix in this PR (keep it):** in `PurchaseState::btnOkClick`, item transfers previously sent `amount:0` (the code called `trade->getQuantity()` but discarded the return). Peers then received zero-quantity items → desync / save corruption. Now `int item_amount = trade->getQuantity();`. This is the correct root-cause fix; the bugs below are collateral in the surrounding rework.

Line anchors are on `origin/main` (`1768603a4`); the host-auth merge shifted numbers, so grep the named symbols rather than trusting exact lines.

---

## 2. Bug #1 — craft-with-crew transfer leaves a dangling `Craft*` (use-after-free) — **HIGH**

**Location:** `Base::removePendingTransfers` (`src/Savegame/Base.cpp:2825`), invoked from the `transfer_completed` handler in `src/CoopMod/connectionTCP.cpp:3763`.

**Mechanism:**
1. `TransferItemsState::createPendingTransfers` (`src/Basescape/TransferItemsState.cpp:556`) builds, on the destination base, one `Transfer` for the craft **and** one `Transfer` per crew soldier assigned to that craft.
2. On ACK, `removePendingTransfers` validates, then removes the craft from the source base via `removeCraft(craft, false)` (`Base.cpp:2985`). **`removeCraft` only erases the craft from `_crafts`; it does not `delete` it and does not `unload()` it** (see `Base::removeCraft`, `src/Savegame/Base.cpp:2771` region).
3. Crew soldiers are deliberately **kept** in the source base (comment *"Soldiers remain in the source base"*, `Base.cpp:2980`). For soldiers, the code detaches them from their transfers (`transfer->setSoldier(nullptr)`) so the transfer dtor won't free them. **The craft gets no such treatment.**
4. Back in the `transfer_completed` handler, the destination transfers are deleted: `for (Transfer* t : *baseTo->getTransfers()) delete t;`. `Transfer::~Transfer` (`src/Savegame/Transfer.cpp:42`) with `_delivered == false` runs `delete _craft;` → **the craft object is freed**.
5. `Craft::~Craft` (`src/Savegame/Craft.cpp`) frees its weapons/vehicles/items but **does not reset `_craft` on soldiers** (soldiers are not owned by the craft). So every kept crew soldier now has `soldier->getCraft()` pointing at freed memory.

**Consequence:** dangling pointer / UAF. Next time those soldiers are iterated with a `getCraft()` deref (base view, geoscape soldier list, monthly report, save serialization) → crash or save corruption.

**Trigger:** transfer a craft **that has crew assigned** to a co-op base. (Craft with no crew: no dangling soldier, so it hides the bug — see repro note.)

**Suggested fix (not applied):** in `removePendingTransfers`, after `removeCraft(craft, false)`, unassign the kept crew of each removed craft:
```cpp
for (Craft* craft : crafts)
{
    for (Soldier* soldier : _soldiers)
        if (soldier && soldier->getCraft() == craft)
            soldier->setCraft(nullptr);   // was left dangling after the craft is freed
    removeCraft(craft, false);
}
```
Decide the intended semantics first: option A = crew moves with the craft (also remove them from the source, and fix the missing `soldier_rule` — see minor #M1 — so the peer recreates them); option B = crew stays, craft leaves, crew unassigned (the snippet above). The current code is neither — it keeps the crew **and** frees the craft they point to.

**Confidence:** confirmed by code inspection (each link in the chain verified in-tree). A live repro command was written but its synthetic setup was rejected by validation before reaching the free (see §5, "#1 open item") — the harness needs a one-line fix, not the product code.

---

## 3. Bug #2 — receiver ACKs a transfer/purchase before validating the target base exists → silent goods loss — **MEDIUM/HIGH** (reproduced live)

**Location:** the `purchase`/`transfer` receive handler, `src/CoopMod/connectionTCP.cpp:3842` (`if (stateString == "purchase" || stateString == "transfer")`), acceptance predicate at `:3846` (`if (obj.isMember("items") && !obj["items"].empty())`).

**Mechanism:**
1. Receiver accepts the packet **iff `items` is non-empty**. It does **not** check that a local base has `base_to_id`. On accept it appends to `waitedTrades` and immediately sends `transfer_completed` / `purchase_completed` back.
2. Sender, on `transfer_completed`, runs `removePendingTransfers` and removes the goods from its source base (and deducts funds); on `purchase_completed` it deducts `coopFunds`.
3. Receiver later drains `waitedTrades` in `updateCoopTask` (`connectionTCP.cpp:~1236`), matching `base->_coop_base_id == base_to_id`. If **no base matches**, the entry is silently re-queued forever and never applied.

**Consequence:** if `base_to_id` never resolves on the receiver, the sender has already removed the goods / deducted funds, but they arrive nowhere → **silent item loss**. This is the exact "could corrupt saves / lose items" class the PR claims to fix, just relocated. Receiver-side apply failure is never surfaced (only sender-side failures raise `CoopState(552/553)`).

**Why it can happen in practice:** the whole protocol assumes `_coop_base_id` is identical on both machines. Ids are random `1..100000`, assigned on load if `0` (`Base::load`). They only match if both saves embed the same ids (host-authoritative). A freshly built / not-yet-synced base is a realistic mismatch window. (Minor #M4.)

**Suggested fix (not applied):** validate on the receiver **before** ACKing — resolve `base_to_id` to a local base (and resolve every item rule) and send `transfer_failed`/`purchase_failed` if not; **or** move the ACK to the point in `updateCoopTask` where the trade is actually applied, so the sender only removes goods after confirmed application.

**Confidence:** **reproduced live** (see §5). Observed: `{'bogusBaseId': 424242, 'appliedToAnyBaseWhenNoMatch': False, 'receiverAcceptedAndQueued': True}` — the receiver accepted + queued (and would have ACKed) a transfer whose target base exists nowhere.

---

## 4. Minor issues

- **#M1 — transferred soldiers never materialize on the peer.** `createPendingTransfers` sets the soldier's coop base but never writes `root["items"][i]["soldier_rule"]`. `Base::syncTrade` (`src/Savegame/Base.cpp:118`) skips soldier entries with `soldier_rule == ""`. So a soldier transfer to a co-op base adds nothing on the receiver. `PurchaseState` **does** send `soldier_rule` via `coopSoldierID()`; the transfer path does not. (Pre-existing in the old `completeTransfer` block, carried into the new function. Interacts with Bug #1 option A.)
- **#M2 — inconsistent null guard.** `createPendingTransfers` uses `trade->getItems()->getName()` unguarded, while the same call in `PurchaseState.cpp` was just wrapped in `if (trade->getItems())`. Low risk (it builds its own transfers) but inconsistent.
- **#M3 — non-atomic across a save.** Between `createPendingTransfers` (pending transfers created on the destination) and the ACK (goods removed from source), a save writes goods in **both** places → duplication. Small window, but it undercuts the "storage unchanged on failure" guarantee.
- **#M4 — `_coop_base_id` sync assumption.** The new protocol keys entirely on `_coop_base_id` matching across machines (see Bug #2). More robust than the old base-name match, but verify ids are synced for newly built bases.
- **#M5 (non-issue, noted):** `Base.cpp` uses `std::set`/`std::map` without explicit `<set>`/`<map>` includes. **Compiles clean** (transitive includes) — confirmed by a full build; left as a note only.

---

## 5. Repro harness

### What exists on disk
- `tools/coop_test/test_purchase_sync_repro.py` — **present but git-untracked** (at risk of being lost to branch switches — commit it). Boots a two-instance co-op session (reuses `bootstrap_fresh_session` from `test_bug_fixes.py`) and calls two `TestServer` commands.

### What was lost and must be re-added
The two `TestServer::execute` commands the driver calls (`repro_craft_uaf`, `repro_receiver_ack_gap`) were added to `src/CoopMod/TestServer.cpp` and **compiled + ran successfully**, but a branch switch by the concurrent CI session reverted `TestServer.cpp` (the file is tracked; the edits were uncommitted). Re-add them before the final `else { resp["error"] = "unknown cmd: " + cmd; }` in `TestServer::execute`. Requires `#include "../Savegame/Transfer.h"` at the top. Full source below.

**`repro_craft_uaf`** (Bug #1) — single instance; drives the real `removePendingTransfers` + real `Transfer` dtor. **Includes the one-line setup fix** (clear default crew off the craft) that the first run was missing — the first run returned `removeOk:False` because the fresh-campaign craft already had default crew not in the transfer set, so validation rejected it before reaching the free:
```cpp
else if (cmd == "repro_craft_uaf")
{
    SavedGame* sg = _game->getSavedGame();
    if (!sg) { resp["error"] = "no save loaded"; }
    else {
        Base* baseFrom = nullptr;
        for (auto* b : *sg->getBases()) if (!b->_coopBase && !b->_coopIcon) { baseFrom = b; break; }
        if (!baseFrom) resp["error"] = "no own base";
        else if (baseFrom->getCrafts()->empty()) resp["error"] = "base has no craft";
        else if (baseFrom->getSoldiers()->empty()) resp["error"] = "base has no soldier";
        else {
            Craft* craft = baseFrom->getCrafts()->front();
            Soldier* crew = baseFrom->getSoldiers()->front();
            // SETUP FIX: strip any default crew so the transfer set is complete (else validation rejects).
            for (auto* s : *baseFrom->getSoldiers())
                if (s->getCraft() == craft && s != crew) s->setCraft(nullptr);
            crew->setCraft(craft);

            // Mirror createPendingTransfers: one soldier transfer for the crew + one craft transfer.
            std::vector<Transfer*> pending;
            Transfer* st = new Transfer(6); st->setSoldier(crew); pending.push_back(st);
            Transfer* ct = new Transfer(6); ct->setCraft(craft);  pending.push_back(ct);

            // Exactly the connectionTCP "transfer_completed" sequence:
            bool removeOk = baseFrom->removePendingTransfers(&pending);
            if (removeOk) { for (Transfer* t : pending) delete t; pending.clear(); } // dtor frees _craft

            // Detect the dangling ref WITHOUT dereferencing the freed craft:
            Craft* stored = crew->getCraft();
            bool craftStillLive = false;
            for (auto* c : *baseFrom->getCrafts()) if (c == stored) { craftStillLive = true; break; }

            resp["removeOk"] = removeOk;
            resp["crewName"] = crew->getName();
            resp["crewCraftNonNull"] = (stored != nullptr);
            resp["crewCraftDangling"] = (stored != nullptr && !craftStillLive); // TRUE == bug present
            crew->setCraft(nullptr); // repair the live instance so teardown/save is safe
            resp["ok"] = true;
        }
    }
}
```
Expected while the bug is present: `removeOk:true, crewCraftDangling:true`.

**`repro_receiver_ack_gap`** (Bug #2) — drives the real `onTCPMessage("transfer", …)` + `updateCoopTask()` (needs `getCoopStatic()==true`, hence the 2-instance session):
```cpp
else if (cmd == "repro_receiver_ack_gap")
{
    SavedGame* sg = _game->getSavedGame();
    if (!sg || !coop) { resp["error"] = "no save/coop"; }
    else {
        int bogus = 424242;
        for (auto* b : *sg->getBases()) if (b->_coop_base_id == bogus) bogus = 424243;
        int before = 0; for (auto* b : *sg->getBases()) before += (int)b->getTransfers()->size();

        Json::Value obj;
        obj["state"] = "transfer";
        obj["base_to_id"] = bogus; obj["base_from_id"] = bogus; obj["total_funds"] = 0;
        obj["items"][0]["name"] = "STR_PISTOL_CLIP"; obj["items"][0]["amount"] = 3;
        obj["items"][0]["hour"] = 1; obj["items"][0]["type"] = 0; obj["items"][0]["craft_rule"] = "";

        coop->onTCPMessage("transfer", obj);   // real receiver: accepts (items non-empty), would ACK
        coop->updateCoopTask();                // real drain: no base matches -> silently retained

        int afterNoMatch = 0; for (auto* b : *sg->getBases()) afterNoMatch += (int)b->getTransfers()->size();

        // Prove it was accepted+queued (not rejected): give a real base the bogus id, drain again.
        Base* victim = nullptr;
        for (auto* b : *sg->getBases()) if (!b->_coopBase && !b->_coopIcon) { victim = b; break; }
        bool acceptedAndQueued = false;
        if (victim) {
            int vBefore = (int)victim->getTransfers()->size();
            int saved = victim->_coop_base_id; victim->_coop_base_id = bogus;
            coop->updateCoopTask();
            acceptedAndQueued = ((int)victim->getTransfers()->size() > vBefore);
            victim->_coop_base_id = saved;
        }
        resp["bogusBaseId"] = bogus;
        resp["appliedToAnyBaseWhenNoMatch"] = (afterNoMatch > before); // FALSE == silent drop
        resp["receiverAcceptedAndQueued"] = acceptedAndQueued;         // TRUE  == bug present
        resp["ok"] = true;
    }
}
```
Observed live: `appliedToAnyBaseWhenNoMatch:false, receiverAcceptedAndQueued:true` → bug confirmed.

### #1 open item
Only remaining loose end: re-run `repro_craft_uaf` **with the SETUP FIX above** to see `crewCraftDangling:true` live. The first run (before the fix) returned `removeOk:false` because the fresh-campaign craft (Skyranger) starts with default crew that weren't in the synthetic transfer set, so `removePendingTransfers` rejected it at the craft-crew cross-check. Stripping that default crew (one loop, above) makes the removal proceed and exposes the dangling pointer. Bug #1 itself is confirmed by inspection regardless.

### Run
```
python tools/coop_test/test_purchase_sync_repro.py     # exits 1 while bugs present; prints per-concern assertions
```

---

## 6. Build & test (verified this session)

- **Build:** `MSBuild src/OpenXcom.2010.sln /p:Configuration=Release /p:Platform=x64` — **serial (no `/m`)**; `/m` triggers `C1060` out-of-heap on a full rebuild. MSBuild at `C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe`. A clean incremental build of `origin/main` = **0 errors** (1569 pre-existing warnings). Output: `bin/x64/Release/OpenXcom.exe`; data dirs (`standard`/`common`/`UFO`/`TFTD`) already deployed there.
- **Harness:** needs the built exe; `OXC_TEST_PORT` env activates the in-game `TestServer`. Drivers spawn their own hermetic instances (`tools/coop_test/harness.py::make_user_dir`).
- **Existing suite result on this code:** `test_transfer_fresh`, `test_transfer_rollback`, `test_bug_fixes`, `test_geoscape_sync`, `test_craft_status_sync`, `test_ufo_notice`, `test_server_browser` → **7/7 green**. **No regressions** from the PR.

### Coverage gap (important for whoever adds tests)
The existing transfer tests drive `connectionTCP::transferSoldierOwnership` (the **soldier-ownership** mechanism), which is a **different code path** from what PR #38 changed. **Nothing in the suite exercises `PurchaseState` / `TransferItemsState` / `createPendingTransfers` / `removePendingTransfers` / the `purchase`/`transfer` connectionTCP handshake.** There is no `TestServer` command that opens `PurchaseState` or drives `TransferItemsState`→`TransferConfirmState`. That is why the suite is green and catches neither bug. A real regression test needs a new command that drives the actual co-op purchase/goods-transfer UI end-to-end (the two repro commands above are lower-level shims, not UI-driven).

---

## 7. Working-tree / branch caveats

- The repo working dir was mid-review on a local branch `purchase-sync-fixes` (tip `3e99215b4`, forked off the **old** main `1d4b0be9`, so that local branch **lacks** the host-auth changes). The concurrent CI session has since switched the tree to **`fix/coop-client-craft-despawn`**, which reverted the uncommitted `TestServer.cpp` repro edits.
- Do the cleanup against **`origin/main` (`1768603a4`)**, which is the merged reality (host-auth + purchase-sync). The buggy code is byte-identical there to what was analyzed; only line numbers shifted.
- `tools/coop_test/test_purchase_sync_repro.py` is untracked — **commit it** (and the two `TestServer` commands) on the cleanup branch so it survives further branch churn.
