# PRD-11: Network robustness — no silent drops, no unearned battle streams, no spoofed host stop

**Fixes:** C13 (PLAUSIBLE: silently dropped `request_load_progress` + parked
streamer thread → rejoining client hangs forever), C8 (PLAUSIBLE: resume_ack
streams the old battle into a freshly built world), plus a defensive guard
against a client-spoofed `server_full` stopping the host's listen thread.

**Files:** `src/CoopMod/connectionTCP.cpp`, `src/CoopMod/CoopState.cpp`.

These are hardening fixes for realistic-but-timing-dependent paths. Each is
small and independent; implement all three.

## Guard 1 — C13: answer or retry, never silently drop; unpark the streamer

Verified mechanics:
- The `request_load_progress` handler's new `!sendFileClient` guard
  (`connectionTCP.cpp:3238`) sends **nothing** on the false branch — no
  refusal, no stream. The requesting client sits in the CoopState(52)
  "loading" wait (pushed at `Profile.cpp:114-118` / `connectionTCP.cpp:6941`),
  which has **no timeout**, and its "Disconnect" button merely pops the dialog
  (state 52 is absent from the disconnect list at `CoopState.cpp:869`).
- The streamer thread never clears `sendFileClient` on a mid-transfer drop:
  sends only enqueue to `g_txQ` (`connectionTCP.cpp:530-533`), so it parks
  forever at the `while (!isWaitMap)` waits (`connectionTCP.cpp:671/693/715`)
  with the flag still true; the catch block (`:850-857`) doesn't reset it
  either. The normal drop path DOES clear it (`disconnectTCP` forces
  `sendFileClient = false`, `:9420`) — but only if the main thread pumps the
  drop before a fast rejoin overwrites `onConnect` (`-2` → `1` at `:2316`).

Fix:
1. **Reply on the busy branch.** When `sendFileClient` blocks the request,
   send a small `load_progress_busy` JSON message back. Client handler for it:
   re-send `request_load_progress` after ~2 seconds, up to ~15 retries, then
   convert the CoopState(52) dialog into the existing error UX. Implement the
   retry with the same timer mechanism other CoopState think() handlers use
   (they are already time-gated — see the 500ms gate at
   `CoopState.cpp:705-711`).
2. **Fix the Disconnect button.** Add state 52 to the list at
   `CoopState.cpp:869` so its Disconnect actually runs `disconnectTCP`.
3. **Unpark the streamer.** Every `while (!isWaitMap)` wait
   (`:671/693/715`) must also exit when the connection is torn down (check the
   same abort signal `disconnectTCP` sets — read what flag the loop can see;
   `onConnect` or a dedicated abort bool). On abnormal exit, the catch/exit
   path (`:850-857`) must reset `sendFileClient = false`.

## Guard 2 — C8: only stream the battle to a client that got the battle world

Verified mechanics:
- The `resume_ack` handler (`connectionTCP.cpp:2696-2703`) sends
  `campaign_resume_battle` to whichever client acks whenever
  `resumeBattlePending` is set — no per-client check.
- Both battle-resume entry points set the flag BEFORE the blob lookup and do
  not clear it on the no-blob branch (`LobbyMenu.cpp:519 + 530-543`;
  `connectionTCP.cpp:3243 + 3259-3277`, which sends `campaign_start`
  instead), and a fresh-base client unconditionally sends `resume_ack` after
  naming its first base (`BaseNameState.cpp:183-189`).
- Result: a registered client whose blob is missing (corrupt/legacy save) is
  routed through fresh base building, acks, and gets the old battle
  (`connectionTCP.cpp:2713-2722`) streamed into a world with none of those
  units.

Fix: track eligibility. Where the resume flow FINDS a blob for a client and
serves the resume world, record that client's name (a small
`std::set<std::string> resumeBattleEligible` on the session, cleared with
`resumeBattlePending`). The `resume_ack` handler sends
`campaign_resume_battle` only if the acker is in the set; otherwise treat the
ack as a plain campaign resume ack (whatever the non-battle path does — read
the handler's else flow). The no-blob `campaign_start` fallback conspicuously
does NOT add to the set.

## Guard 3 — spoofed `server_full` must not stop the host

Verified mechanics: the `server_full` message handler sets `onConnect = -1`
(`connectionTCP.cpp:3211`) without checking role; `-1` is the host listen
thread's exit condition (`:2295`). A misbehaving client can send it and stop
the host's thread.

Fix: guard the handler — process `server_full` only when this machine is a
client (`session.role`-based check consistent with how other client-only
messages are guarded; grep for an existing role check pattern in the message
dispatch). Log and ignore otherwise.

## Acceptance criteria

- Build + `boot_check.py`.
- Full suite green — `test_rejoin_flow.py` (fast hard-kill rejoin is the C13
  window; run it 3× back-to-back to shake the timing), `test_resume_flow.py`
  (its empty-user-dir scenario exercises the no-blob `campaign_start` path —
  now also proving the client does NOT receive a battle stream),
  `test_session_hardening.py`.
- Commit body lists the three guards and their trigger scenarios.

## Out of scope

- Full request/reply reliability layer; message authentication (single
  trusted-peer model stands); changing the resume protocol shape.
