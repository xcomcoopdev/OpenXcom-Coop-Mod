# Plan — JOINT soldier ownership parity with SEPARATE (undo the J09 "shared roster" mistake)

**Goal (user):** In JOINT, soldiers behave exactly like SEPARATE:
1. A player NEVER sees another player's soldiers in ANY list.
2. Each player assigns their OWN soldiers to a craft (mixed-owner crews still form
   because each player adds their own to the shared craft).
3. Each player controls their OWN soldiers in the battlescape, on their own turn.

**Status:** investigation done, no code yet. This is a design reversal of PRD-J09,
which deliberately made JOINT show the whole roster ("every player must see ALL
soldiers (mixed-owner squads)", CraftSoldiersState.cpp:210-214). The user has
confirmed J09 was wrong.

---

## 1. What is already correct (data layer) — do NOT touch

Ownership DATA is right; only the VIEWS are wrong.

- `Soldier::_ownerPlayerId` (0 = host seat, 1 = client seat) is serialized
  (Soldier.cpp:190/332), split at campaign creation + on load
  (`SavedGame::migrateJointSoldierOwnership`), and mirrored on both machines.
- Hires own the buyer: `soldier->setOwnerPlayerId(seat)` in the buy applier
  (JointEcon.cpp ~464/481), `seat` = purchaser.
- `localSeat()` = 0 on host, 1 on client (connectionTCP.cpp:9138) — matches owner ids.
- Battle control derives from `BattleUnit::_coop`, copied from soldier `getCoop()`,
  which is stamped from `getOwnerPlayerId()` at battle entry (ConfirmLandingState.cpp
  ~282, base-defense GeoscapeState.cpp ~5749). Validated end-to-end in
  `test_joint_soldier_ownership_battle.py` (owner 0->coop 0, owner 1->coop 1 on both).

So requirement (3) is a VERIFY, not a rewrite (see §5).

The bug is purely: **every soldier LIST view shows the whole shared roster** because
the SEPARATE guest-filter is fenced off in JOINT and nothing replaces it.

---

## 2. Why we cannot reuse the SEPARATE filter (the core constraint)

SEPARATE filters each soldier view by DESTRUCTIVELY swapping the base roster:
`_base->base_oldsoldiers = *getSoldiers(); *getSoldiers() = <kept>;` restored on exit
(SoldiersState.cpp:256-272, CraftSoldiersState.cpp:215-231, each with its own slot).

Two blockers in JOINT:

- **Wrong key:** it keeps `getCoopBase() == -1`. Every JOINT soldier has
  `getCoopBase() == -1` (there are no mirror soldiers), so it filters NOTHING. JOINT
  ownership is `getOwnerPlayerId()`, a different field.
- **Destructive = unsafe on the shared roster.** `_base->getSoldiers()` in JOINT is
  the ONE host-authoritative roster and its COUNT is in the desync checksum
  (`chkSoldiers` via `worldAggregates`, JointEcon.cpp:3132-3135). Removing half the
  soldiers drops the count -> **false desync -> full world re-stream just for opening
  the soldier list.** Also a `joint_apply` can land while a base screen is open (J10
  ScreenRefresh), and would operate on the half-roster, then the restore of
  `base_oldsoldiers` would discard the apply -> soldier loss.

**Rule for the whole feature: a JOINT soldier view must filter its own LOCAL display
list and never mutate `_base->getSoldiers()` (not the contents, not the order).**

Corollary: the "move soldier up/down" reorder (SoldiersState.cpp:565-574,
CraftSoldiersState.cpp:545-554) also mutates the shared roster order in JOINT — it
must operate on the local list only (count-unchanged, so it won't trip the checksum,
but it still diverges roster order from the host; keep it local/cosmetic).

---

## 3. Architecture — per-view local filtered working list

Introduce one shared predicate + helper, then route each list view through a LOCAL
vector instead of `_base->getSoldiers()`.

**Helper (new, in JointEcon or a small SoldierOwnership util):**
```cpp
// Soldiers this machine's player may SEE/manage at `base`.
// JOINT: getOwnerPlayerId() == localSeat(). SEPARATE/solo: unchanged (existing
// getCoopBase() rule / full list). Non-destructive: returns a copy.
std::vector<Soldier*> visibleSoldiers(Game* game, Base* base);
bool  ownsSoldier(Game* game, const Soldier* s); // JOINT owner==localSeat, else true
```

**Per view:** give each list view a member `std::vector<Soldier*> _viewSoldiers`
(built from `visibleSoldiers()` in its list-build step) and replace EVERY
`_base->getSoldiers()` used for display / row-indexing / sort / move-up-down /
click->soldier with `_viewSoldiers`. Writes that actually mutate the world (sack,
transfer, assign-to-craft) keep going through the existing host-authoritative
joint_cmd path — they just resolve the target soldier via `_viewSoldiers[row]`.

This is the SEPARATE approach's INTENT (a filtered working list) done safely
(local copy, JOINT-keyed), not the destructive swap.

Note SoldiersState is already half-way: it has `_filteredListOfSoldiers` used for
some rows (line 1049) but `_base->getSoldiers()->at(row)` for others (565/932) —
the swap hid the inconsistency. The refactor makes ONE list authoritative per view.

---

## 4. Surfaces to change (each = filter to `_viewSoldiers` + a harness accessor + test)

Priority 1 — the visible bug the user hit:
- `SoldiersState` (base roster list). Reorder, click, sack-entry, gift-unit all via
  `_viewSoldiers`. (repro already exists: `test_joint_soldier_visibility.py`.)
- `CraftSoldiersState` (assign to craft). Filter to own; each player loads their own
  onto the shared craft -> mixed crew. Space Used/Available must still reflect the
  FULL craft occupancy (both owners), only the LIST is filtered.

Priority 2 — other soldier lists that currently leak the peer's soldiers:
- `SackSoldierState` / sack list entry (SoldiersState + `SellState` sack tab).
- `TransferItemsState` soldier rows (line 154/158 use the getCoopBase filter -> owner).
- `SoldierMemorialState` dead-soldier list (`getDeadSoldiers`) -> own only.
- `SoldierTransformationListState` / `SoldierTransformState` eligible list.
- `CraftPilotSelectState` / `CraftPilotsState` (pilot pick = craft crew subset).
- `CraftArmorState` (per-craft crew armor list) — crew shown filtered to own.
- `SoldierInfoState` prev/next navigation cycles `_base->getSoldiers()` — must cycle
  `_viewSoldiers` so you can't page into the peer's soldiers.
- `SoldierAvatarState`, `SoldierBonusState`, `SoldierRankState`,
  `SoldierDiaryOverview/Performance` — reached per-soldier; audit their prev/next.

Priority 3 — counts / capacity (DESIGN DECISION, see §6):
- `BaseInfoState` soldier bar `getAvailableSoldiers():getTotalSoldiers()` (line 274),
  `Base::getAvailableSoldiers/getTotalSoldiers` (Base.cpp:767/797) — shared-base
  totals today.

---

## 5. Requirement (3) battlescape — ALREADY WORKS, no code change (add a verify test)

Confirmed by a full trace of `src/Battlescape/`: there is **zero** `isJointCampaign()`
branching in the battlescape. A JOINT battle runs the **identical lockstep model
SEPARATE uses**, driven entirely by `_coopGamemode` (battle mode, 1=PvE co-op) +
host/client role — orthogonal to `isJointCampaign()` (economy/world model).

- Turn start: rendezvous at `BattlescapeState.cpp:1663-1771` — host `setPlayerTurn(2)`
  (:1767), client `setPlayerTurn(1)` (:1731). (The `connectionTCP.cpp:10304`
  `setPlayerTurn(2)` I cited earlier is inside `disconnectTCP()` teardown — NOT turn
  logic. Corrected.)
- Alternation: `next_turn` handler (`connectionTCP.cpp:7316-7367`) flips the client to
  active (`setPlayerTurn(2)` :7335) while the host `endTurnCoop()` (:7361). Only ONE
  player has `isYourTurn==2` at a time.
- Selection gate `BattlescapeGame.cpp:2873-2893` restricts the active player to their
  own `_coop` units (host coop!=1, client coop==1). Off-turn input is fully blocked
  (`setupCursor` :1865 + pervasive `isYourTurn==1/3/4` guards), so the co-owned
  fall-through at :2896 is unreachable when it matters. Auto-selectors on turn start
  pick only own-coop units (`BattlescapeState.cpp:1565-1578` client / :1616-1630 host).

**So (3) needs NO battle code.** Its ONLY two preconditions, both already met:
  (a) the JOINT battle is gamemode 1 (co-op PvE) — comes from the lobby/session
      setting (`CoopMenu.cpp:425-430`); and
  (b) every player `BattleUnit._coop` is correctly 0/1 from `getOwnerPlayerId()` — the
      geoscape entry stamp we already validated end-to-end. A wrong `_coop` would give
      a unit to the wrong player or drop its owner to spectator (`:1583`/`:1635`).

Action: add ONE regression test (bootstrap split, no manual stamping) that in a live
JOINT battle asserts each machine's selectable set == its own-coop units and the peer's
are NOT selectable, and turns alternate (`getCurrentTurn()` reaches 2 for each side).
Guards (b) against future ownership regressions. No production change expected.

---

## 6. Open design decisions (need a call before coding)

1. **Soldier COUNT / capacity.** The base is physically shared, so living-quarters
   capacity and `getTotalSoldiers()` are inherently shared. Options:
   (a) leave counts/bars SHARED (accurate capacity), filter only the LISTS — simplest,
       but you'd see "8 soldiers" in base info yet only 4 in your list; or
   (b) show OWN counts in soldier-facing UI (matches SEPARATE feel) while capacity math
       stays shared under the hood.
   Recommendation: (a) for v1 (lists filtered, counts shared+accurate), revisit (b) if
   it feels wrong in play.

2. **Craft crew capacity with mixed owners.** If each player only sees their own aboard,
   the Space Used bar must still count BOTH owners (or a player could over-fill). Plan:
   list filtered, capacity counts the whole craft. Confirm this is acceptable.

3. **Gifting (`GiftSoldierMenu`, "give unit").** It transfers ownership between players
   (setOwnerPlayerId). Keep it — it's how you hand a soldier to your teammate. After a
   gift the soldier leaves your list and appears in theirs. Confirm keep.

4. **Off-turn visibility in battle.** SEPARATE lets you SEE (not control) all units.
   Keep JOINT the same (see all on the map, control only your own on your turn)?
   Assume yes (matches SEPARATE); only the geoscape/base LISTS hide the peer's soldiers.

---

## 7. Harness + tests (per surface, red-first)

- Extend `screen_state` with a `displayed` soldier-id list for each list view (done for
  SoldiersState via `harnessDisplayedSoldierIds`; add for CraftSoldiers, Sack/Sell,
  Transfer, Memorial, Transform, Pilots, CraftArmor).
- Per surface: open on host + client from the BOOTSTRAP-split roster, assert each shows
  ONLY its owned half. `test_joint_soldier_visibility.py` is the template (currently
  red on SoldiersState).
- Battlescape control test (§5).
- Regression: world-equality + zero-disk + checksum-no-drift while every filtered view
  is open (proves non-destructive: `chkSoldiers` unchanged, no resync).
- A "buy soldier -> appears only in the buyer's list, on both machines" test.

---

## 8. Phasing

- **P0** helper `visibleSoldiers/ownsSoldier` + `SoldiersState` refactor + green test
  (turns `test_joint_soldier_visibility.py` green). Smallest end-to-end slice.
- **P1** `CraftSoldiersState` (assign own -> mixed crew) + test.
- **P2** Sack/Sell, Transfer, Memorial, Transform, Pilots, CraftArmor, SoldierInfo
  prev/next.
- **P3** counts/capacity decision (§6.1) if we go with (b).
- **P4** battlescape control verification test (§5); fix only if it fails.

---

## 9. Risks / watch-items

- Non-destructive is mandatory: any accidental `_base->getSoldiers()` mutation (filter
  OR reorder) in a JOINT view -> `chkSoldiers` drift or order divergence -> resync.
  Every touched view needs the "no shared-roster mutation" review.
- Index alignment: every `getSoldiers()->at(row)` in a touched view must move to
  `_viewSoldiers[row]` together, or clicks select the wrong soldier.
- J10 live-refresh: the filtered `_viewSoldiers` must be rebuilt in each view's
  jointRefresh/init so a peer's hire/sack/gift updates your list live.
- Some Latin-1 encoded files (SoldiersState.cpp is ISO-8859) — patch via latin-1, not
  UTF-8 (see [[source-file-encoding-latin1]]).
- Sorting persistence: SEPARATE persists soldier order to the roster; JOINT sort must
  be local-only, so sort order won't persist across screen reopen the same way. Accept.
