# PRD-10: START CAMPAIGN must re-check eligibility at confirm time

**Fixes:** C4 (CONFIRMED: client drop while the confirm dialog is open still
starts the campaign — roster locked with the departed player, `campaign_start`
sent to a dead socket).

**Files:** `src/CoopMod/LobbyMenu.cpp/.h`, `src/CoopMod/TestServer.cpp`,
`tools/coop_test/test_lobby_gating.py`.

## Verified mechanics (current behavior)

- `ConfirmStartCampaignState::btnOkClick` (`LobbyMenu.cpp:446-451`) pops the
  dialog and calls `startCampaign()` with **no** re-check of
  `startEligible()` / `session.clientInLobby`.
- Nothing pops the confirm dialog on a client drop:
  - Host-side teardown pushes the freeze dialog only when
    `session.lobbyClosed == true` (`connectionTCP.cpp:9350-9366`), but the
    open lobby sets `lobbyClosed = false` (`LobbyMenu.cpp:117`).
  - `CoopSession::onClientDrop` (`connectionTCP.cpp:276-292`) clears
    `clientInLobby` without touching the state stack.
  - `LobbyMenu::think()`'s redirect branch is client-only
    (`getServerOwner() == false`, `LobbyMenu.cpp:963`).
- So: host clicks START CAMPAIGN → dialog up → lone client drops → host
  clicks OK → `startCampaign()` (`LobbyMenu.cpp:562-598`) locks
  `setCoopPlayers({host, getCurrentClientName()})` (the dropped player's real
  name — set at join, `connectionTCP.cpp:7088`, deliberately kept on drop per
  the comment at `:7036-7042`), writes `_autogeo_.asav`, sends
  `campaign_start` to a dead socket. The roster gate
  (`connectionTCP.cpp:7062-7085`) thereafter refuses any differently-named
  joiner. The dropped player can rejoin by name but never received
  `campaign_start`.

## Required behavior

- Clicking OK on the confirm dialog when the lobby is no longer start-eligible
  does NOT start the campaign; the host lands back on the lobby with the
  existing refusal/notice UX and can wait for a rejoin or leave.
- A client drop while the confirm dialog is open dismisses the dialog
  proactively (don't rely on the user clicking into a stale dialog).

## Implementation plan

1. **Re-check at commit point.** In `ConfirmStartCampaignState::btnOkClick`:
   after popping the dialog, if `!_lobby->startEligible()` (read
   `startEligible()`'s definition first — it is the same gate that decides
   whether START CAMPAIGN is clickable), show the lobby's existing
   "not eligible / player left" notice (grep LobbyMenu for the refusal UX the
   lobby-polish commit added; reuse it) and return WITHOUT calling
   `startCampaign()`.
2. **Dismiss on drop.** In the host branch of `LobbyMenu::think()` (or in the
   same place the lobby reacts to `clientInLobby` changes): if the top state
   is a `ConfirmStartCampaignState` (add a `dynamic_cast` check) and
   `!startEligible()`, pop it and show the notice. Keep it scoped to the
   confirm dialog — do not build a generic dialog-popper.
3. `startCampaign()` itself gets a defensive early-return on
   `!startEligible()` (belt-and-braces for any future caller — TestServer's
   `lobby_start_campaign` command calls it directly; check that command's
   current expectations in the tests before adding, and keep the return
   observable: log + no side effects).

## Regression tests

`test_lobby_gating.py` already drives lobby eligibility. Add a scenario:
host+client in lobby → open the confirm dialog. The TestServer
`lobby_start_campaign` command may call `startCampaign()` directly, bypassing
the dialog — read `TestServer.cpp`'s handler first. If it bypasses, add a
command variant that routes through the real dialog
(`lobby_start_campaign {"confirm": "dialog"}` pushing
`ConfirmStartCampaignState`, then a `coop_dialog_ok`-style command to click
OK), so the test exercises the real UI path:
1. dialog up → hard-kill client → host OK → assert: no campaign started
   (`lobby_state` still lobby, `save_markers` shows no locked roster /
   `coopPlayers` empty, no `_autogeo_.asav` in the host user dir).
2. dialog up → hard-kill client → assert dialog auto-dismissed (top state back
   to LobbyMenu) within the think cadence.

## Acceptance criteria

- Build + `boot_check.py`; full suite green — `test_lobby_gating.py`,
  `test_new_campaign_flow.py` (normal start path must be unaffected),
  `test_lobby_polish.py`.
- New scenarios pass.

## Out of scope

- Reworking `startEligible()` semantics; multi-client rosters.
