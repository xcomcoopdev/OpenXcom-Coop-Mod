# PRD-13: Test infrastructure dedup — findState template, shared Python helpers

**Fixes:** S6 (state-stack scan pasted ~20× in TestServer.cpp), S7 (test files
re-implement session.py/geo.py helpers).

**Files:** `src/CoopMod/TestServer.cpp`, `tools/coop_test/session.py`,
`tools/coop_test/test_client_zero_disk.py`,
`tools/coop_test/test_session_hardening.py`,
`tools/coop_test/test_lobby_polish.py`, `tools/coop_test/test_lobby_gating.py`,
`tools/coop_test/test_geoscape_sync.py`.

Test-only risk; safe to do in any session. No engine behavior changes.

## S6 — TestServer find-state scans

The pattern
```cpp
for (auto* s : _game->getStates())
    if (auto* x = dynamic_cast<SomeState*>(s)) ...
```
appears at TestServer.cpp 260, 382, 416, 596, 656, 672, 703, 776, 1058, 1126,
1154, 1178, 1197, 1218, 1318, 1375, 1518, 1538, 1681, 1724, 1759, 2045, 2094
(snapshot lines). Nearly all are pure find-LAST-of-type; a few cast only the
top state.

Fix: add file-local helpers near the top of TestServer.cpp:
```cpp
namespace {
template <class T> T* findState(Game* game)      // last (topmost) instance of T
{
    T* found = nullptr;
    for (auto* s : game->getStates())
        if (auto* t = dynamic_cast<T*>(s)) found = t;
    return found;
}
template <class T> T* topState(Game* game)       // T only if it is the top state
{
    return game->getStates().empty() ? nullptr
        : dynamic_cast<T*>(game->getStates().back());
}
}
```
Then classify each of the ~23 sites BEFORE converting:
- pure find-last → `findState<T>`;
- top-only cast → `topState<T>`;
- anything with extra per-iteration logic (breaks early on first match, counts
  instances, touches multiple types in one loop) → leave as-is and note it.
Semantics must not change: find-FIRST vs find-LAST matters when two instances
of a state are stacked — check each loop's exit behavior (a loop that `break`s
on first match is find-FIRST; do not convert it to `findState` silently — use
a `findFirstState<T>` variant if any exist).

## S7 — Python helper duplication

Verified duplication:
- `test_client_zero_disk.py:22-31` copies `SAVE_EXTS` (session.py:24) and
  `save_files()` (session.py:137-143) verbatim without importing session, and
  inlines the zero-disk assert (line 84 `assert client_saves == []`) that
  `session.assert_client_zero_disk` (session.py:146-148) provides. **This test
  guards the branch's core invariant with a private copy of the rule** — if
  SAVE_EXTS grows, the test silently checks the old rule.
- Local `top(gc)`/`states(gc)`/`top_state(gc)` one-liners re-defined in
  `test_session_hardening.py:29/33`, `test_lobby_polish.py:26`,
  `test_lobby_gating.py:23`, `test_geoscape_sync.py:56` — duplicating
  `geo.top_state` (geo.py:64-67, which additionally handles the empty stack)
  and `session._states` (session.py:27-28). All these files already
  `import session`.

Fix:
1. In `session.py`: rename `_states` → `states` and `_has_state` →
   `has_state` (public API), keeping `_states = states` / `_has_state =
   has_state` aliases for any straggler.
2. `test_client_zero_disk.py`: delete the local `SAVE_EXTS`/`save_files`,
   import from `session`, replace the inline assert with
   `session.assert_client_zero_disk(...)` (match its signature).
3. Delete the local `top`/`states`/`top_state` defs in the four test files;
   use `geo.top_state` and `session.states`. Where a local variant would have
   crashed on an empty stack and the test relied on that (unlikely), keep
   `geo.top_state`'s safe `''` return and adjust the assertion.

## Acceptance criteria

- Build + `boot_check.py` (TestServer changed → build required).
- **Full suite green** — these files ARE the suite; every test must run:
  `for f in tools/coop_test/test_*.py: python $f` (run serially; each test
  spawns its own game instances).
- `grep -n "SAVE_EXTS" tools/coop_test/*.py` → session.py only.
- Commit body: site classification table for the ~23 TestServer conversions
  (converted-to-findState / converted-to-topState / left-alone + why).

## Out of scope

- New TestServer commands; harness feature work; touching non-listed tests.
