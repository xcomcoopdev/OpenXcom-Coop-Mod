# PRD-05: Structured blob store — kill stringly blob identity and the suffix-erase data loss

**Fixes:** S8 (blob identity round-trips through `host_<id>_<name>.data`
filename strings, reverse-parsed with find/substr; CONFIRMED data-loss bug:
saving player "Bob" erases player "Super_Bob"'s blob), E3 (multi-MB blob copies
by value under mutex).

**Files:** `src/CoopMod/connectionTCP.cpp/.h`, `src/Savegame/SavedGame.cpp/.h`,
`src/CoopMod/CoopState.cpp`, `src/CoopMod/TestServer.cpp`,
`tools/coop_test/session.py` (+ tests that build keys).

## Verified defects

1. **Key round-trip.** `hostBlobKey()` (`connectionTCP.cpp:567`) packs
   `saveID` + player name into `"host_<id>_<name>.data"`. `SavedGame::save`
   then **reverse-parses** those keys with find/substr (`SavedGame.cpp:1516-1522`)
   and orders IDs by **lexicographic string compare** (`:1524`) — silently
   assuming the fixed-width 14-digit datetime from `getDateTimeCoop`
   (`connectionTCP.cpp:2502`).
2. **Suffix-erase data loss (CONFIRMED).** `eraseStaleBlobEntries`
   (`connectionTCP.cpp:593-600`) prunes by suffix match: storing a blob for
   player `Bob` erases `host_<id>_Super_Bob.data` because that key **ends
   with** `_Bob.data`. Another client's campaign progress is silently deleted.
3. **Names are unsanitized.** Raw TextEdit (`CoopMenu.cpp:717`,
   `ServerList.cpp:1214`) and raw network JSON (`connectionTCP.cpp:7033`) — any
   character can appear in a player name.
4. **E3:** blob values are full serialized SavedGames (multi-MB). By-value
   copies under `coopFilesMutex` at `SavedGame.cpp:315`
   (`loadCoopSaveFromMemory`), `connectionTCP.cpp:3254`
   (`sendProgressLoadBlob`), `connectionTCP.cpp:777` (`loopData`, streamer
   thread).

## Required behavior

- Blob identity is structured data (player name + saveID as separate fields),
  never parsed back out of a packed string.
- Storing player X's blob can never touch player Y's entry.
- Snapshots of blob data are refcount bumps, not multi-MB copies.
- The TestServer `has_coop_file` command and the Python harness keep working
  (update them in lockstep if the key format they consume changes).

## Implementation plan

**Step 0 — inventory.** `grep -n "coopFilesHost\|coopFilesClient\|hasCoopFile\|hostBlobKey\|clientBlobKey\|eraseStaleBlobEntries\|loadCoopSaveFromMemory\|saveCoopToMemory" src/ tools/` and write the full list of keys and call sites into the commit body. Known reserved (non-per-client) keys include `basehost`, `battleclient`, `battlehost`, `coop_geoscape_return` — enumerate the rest before coding.

**Step 1 — value type.** Change the map value to a refcounted immutable blob:
```cpp
using BlobPtr = std::shared_ptr<const std::string>;
```
Keep the two maps' key type as `std::string` for reserved keys, but add a
structured per-client store alongside (or a small struct value):
```cpp
struct ClientBlob { std::string saveID; BlobPtr data; };
// keyed by exact player name
std::map<std::string, ClientBlob> hostClientBlobs;
```
Reserved keys stay in the flat map; per-client `host_*` entries move to
`hostClientBlobs`. `hostBlobKey()` remains ONLY as a wire/YAML/TestServer
compatibility encoder — nothing in C++ parses it anymore.

**Step 2 — replace the erase.** `eraseStaleBlobEntries` becomes trivial: the
per-client map is keyed by exact name, so storing a new blob for `Bob` is
`hostClientBlobs["Bob"] = {saveID, blob}` — replacement, no suffix scan. Delete
the suffix-matching code entirely.

**Step 3 — replace the reverse-parse.** `SavedGame::save`'s embed loop
(`SavedGame.cpp:1504-1541`) iterates `hostClientBlobs` directly (name and
saveID are fields). "Latest per client" selection: the per-client map holds one
entry per name by construction, so the lexicographic-compare selection code is
deleted. For the on-disk YAML: **decision — new schema, no back-compat.** Write
`coopClientSaves` entries as explicit fields:
```yaml
coopClientSaves:
  - player: Bob
    saveID: "20260711123456"
    blob: <base64>
```
and update the restore filter (`SavedGame.cpp:817-829`) to read that shape into
`hostClientBlobs`. This invalidates coop saves made on the branch so far —
acceptable pre-merge (they are dev saves). State this in the commit body.

**Step 4 — snapshots.** The three by-value copy sites take `BlobPtr` copies
under the mutex instead of string copies. `loadCoopSaveFromMemory` parses from
`*ptr` after releasing the lock. Note `connectionTCP.cpp:777` runs on the
streamer thread — the shared_ptr copy under mutex is exactly what makes that
safe; do not "optimize" the lock away.

**Step 5 — harness compatibility.** `has_coop_file` (TestServer) currently
takes a flat key. Keep accepting the `host_<id>_<name>.data` form by encoding
the lookup (split is now done ONCE, here, with the known current saveID from
`save_markers` semantics) — or better, add a `has_client_blob {player, saveID}`
command and migrate `tools/coop_test/session.py:72-77` and any test building
keys to it. Grep `tools/coop_test/*.py` for `host_` to find all builders.
While in `session.py`, hoist the `save_markers` call out of the poll lambda
(E5 nit: it re-fetches a constant saveID every poll — fetch once before
`wait_for`).

## Acceptance criteria

- Build + `boot_check.py`.
- Full suite green — most load-bearing: `test_client_zero_disk.py`,
  `test_resume_flow.py`, `test_rejoin_flow.py`, `test_transfer_rollback.py`.
- `grep -rn "substr\|find(" src/Savegame/SavedGame.cpp` shows no blob-key
  parsing; `eraseStaleBlobEntries` suffix match gone.
- Commit body documents: key inventory, the schema change, and the note that
  pre-existing branch dev saves are invalidated.

## Out of scope

- Sanitizing player names at entry (worth doing someday; the structured store
  makes it unnecessary for correctness).
- Any change to what blobs contain or when they are sent.
