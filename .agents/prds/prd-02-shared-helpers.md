# PRD-02: Deduplicate campaign-start packet, lobby close, base-placement kickoff, browser push

**Fixes:** S1 (campaign_start packet built 3×, lobby pop-loop 2×), S2 (initial
base-placement kickoff cloned 3×), S5 (`pushServerListUnlessPresent` re-inlined
in the same file).

**Files:** `src/CoopMod/LobbyMenu.cpp/.h`, `src/CoopMod/connectionTCP.cpp/.h`,
`src/Menu/NewGameState.cpp/.h`.

This PRD is pure consolidation. **No behavior change is intended anywhere.**
After each extraction, diff the new call sites' behavior against the removed
code line-by-line.

## Verified duplication S1 — campaign_start packet + lobby close loop

The JSON packet that creates a client world (fields: `state`, `difficulty`,
`gamemode`, `saveID`, plus a `players` array loop) is hand-built byte-for-byte
identically in three places:

- `LobbyMenu.cpp:532-542` — `LobbyMenu::resumeCampaign()`
- `LobbyMenu.cpp:588-598` — `LobbyMenu::startCampaign()`
- `connectionTCP.cpp:3264-3275` — `request_load_progress` no-blob fallback

And the pop-until-LobbyMenu close loop (including `session.lobbyClosed = true`)
is duplicated verbatim at `LobbyMenu.cpp:547-556` and `602-611`.

**Fix:**
1. Add a builder next to the existing blob-key helpers in connectionTCP
   (declare in `connectionTCP.h`):
   ```cpp
   // one authority for the packet that creates/refreshes a client world
   static Json::Value buildCampaignStartPacket(const SavedGame* save);
   ```
   (Match however the three sites actually construct JSON — if they build a
   string or a different JSON type, mirror that; the point is one function.)
   Replace all three sites. If any site sets an extra field the others do not,
   STOP and re-read — the review found them byte-identical; a difference means
   the code moved. Preserve exactly what the common block does.
2. Add `void LobbyMenu::closeLobby();` containing the pop-loop +
   `session.lobbyClosed = true`, call it from `startCampaign()` and
   `resumeCampaign()`.

## Verified duplication S2 — initial base-placement kickoff

The ~20-line sequence (coop-marker check → `calculateServices` → globe
`center(lon, lat + 0.61)` → route to `BaseNameState` / `PlaceLiftState` /
`BuildNewBaseState` depending on `Options::customInitialBase`) exists in three
copies:

- `src/Menu/NewGameState.cpp:201-225` — the vanilla original
- `src/CoopMod/LobbyMenu.cpp:613-641` — inside `startCampaign()`
- `src/CoopMod/connectionTCP.cpp:2648-2665` — the client `campaign_start`
  handler

Known cosmetic drift (confirmed by review): the LobbyMenu copy finds the
GeoscapeState by scanning the state stack and guards with
`if (gs && ...) / else if (gs)`, while the other two use a freshly constructed
`gs` and a plain `else`. Semantics are identical when `gs != nullptr`.

**Fix:** add a helper declared in `src/Menu/NewGameState.h` and implemented in
`NewGameState.cpp` next to the original (keeping it beside the vanilla copy
means the next OXCE rebase conflicts surface in one file):

```cpp
// Shared by solo new game, host lobby start, and client campaign_start:
// centers the globe on the marker and routes into base naming/placement.
void beginInitialBasePlacement(Game* game, GeoscapeState* gs, Base* base);
```

Null-tolerant on `gs` (mirror the LobbyMenu guard). Replace all three sites.
Do not change the `0.61` latitude offset — give it a named constant inside the
helper (`kInitialBaseLatOffset`) with a comment that it matches vanilla.

## Verified duplication S5 — browser push re-inlined

`LobbyMenu.cpp:751-761` added helper `pushServerListUnlessPresent()` (scan the
stack for an existing `ServerList`, push a new one only if absent). The
connection-lost branch of `LobbyMenu::think()` (`LobbyMenu.cpp:975-986`)
re-implements the same scan inline with a `browserBelow` flag. Replace the
inline block with a call to the helper. The only difference is loop shape
(flag+push vs early-return) — behavior is identical.

## Acceptance criteria

- Build passes (serial msbuild, incremental — see README).
- `python tools/coop_test/boot_check.py`
- `python tools/coop_test/test_new_campaign_flow.py` (covers startCampaign
  packet + host and client base placement)
- `python tools/coop_test/test_resume_flow.py` (covers resumeCampaign packet +
  the request_load_progress fallback path via the empty-user-dir scenario)
- `python tools/coop_test/test_lobby_polish.py` (covers the think()
  connection-lost browser push)
- Grep shows exactly one occurrence of `0.61` in base-placement context, one
  construction site for the campaign_start packet, zero pop-until-LobbyMenu
  loops outside `closeLobby()`.

## Out of scope

- Changing packet contents, adding fields, altering base-placement flow.
