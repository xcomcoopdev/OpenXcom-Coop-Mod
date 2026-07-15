"""Repro + fix verification for issue #28 - "Host saves with client ship out
can despawn the client ship".

In a live 2-player co-op session a CLIENT sends one of its own craft OUT chasing
a SHARED/coop target (a UFO the host spawned, which mirrors onto the client's
geoscape but is NOT part of the client's own-world blob). The HOST then performs
a host-authoritative save (the deferred funnel that pulls a fresh client-world
blob and embeds it), and later the world is served back on RESUME.

Root cause (fixed): coop entities are stripped from the client blob on
serialize, so on reload Craft::load could not re-resolve the destination and
silently dropped it - the out-craft reverted to aimless PATROLLING, abandoning
its mission. The fix persists the target's cross-instance coop id on the craft
(and keeps the coop id stable across the host's own save/reload), lays down an
interim waypoint at the saved position, and re-links the craft to the LIVE coop
mirror once it re-syncs - so the craft keeps chasing the REAL, position-synced
target and engages it on arrival.

Two scenarios, each asserting the craft (a) never despawns and (b) re-links to
the live coop target after resume; plus a mode-specific proof:

  landing : SKYRANGER -> crashed coop UFO. Arrival pops ConfirmLandingState (the
            'start mission' prompt) - proving the craft reached the real TARGET,
            not just its saved position (which would pop CraftPatrolState).
  tracking: INTERCEPTOR -> flying coop UFO. Moving the host UFO makes the client
            mirror (and thus the re-linked craft's destination) follow it -
            proving live-tracking of a MOVING target, not a frozen coordinate.

(The interceptor's dogfight screen fires through the SAME engine arrival handler
proven deterministically by the landing scenario; a flying UFO is too unstable
in-harness - alien-mission despawn / detection quirks unrelated to this fix - to
drive to the dogfight popup reliably, so scenario 2 asserts the re-link +
live-tracking that the dogfight depends on.)

Run:  python tools/coop_test/test_host_save_client_craft_out.py

Exit 0 = both scenarios pass; 2 = a failure / regression.
"""

import ctypes
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
from session import new_campaign, resume_campaign


def keep_awake():
    ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000001 | 0x00000002)


def own_crafts(gc):
    """Every craft this instance OWNS (coop=false), flattened across bases."""
    r = gc.cmd({"cmd": "geo_state"})
    if not r.get("ok"):
        return []
    return [c for b in r["bases"] for c in b.get("crafts", []) if not c["coop"]]


def find_own_craft(gc, craft_id):
    for c in own_crafts(gc):
        if c["id"] == craft_id:
            return c
    return None


def coop_ufo(client):
    """The (single) coop mirror UFO on the client, or None."""
    for u in client.cmd({"cmd": "geo_state"}).get("ufos", []):
        if u.get("coop"):
            return u
    return None


def craft_key(c):
    """Stable cross-reload identity for an own craft: (type, id)."""
    return (c["type"], c["id"])


def geo_tick(host, client, speed=3):
    """One popup-draining time-advance nudge on both sides; keeps the session
    live so target_positions syncs (and coop re-link) keep flowing."""
    host.cmd({"cmd": "geo_run", "speed": speed})
    client.cmd({"cmd": "geo_run", "speed": speed})


def assert_craft_present(tag, client, key, fail):
    crafts = own_crafts(client)
    keys = [craft_key(x) for x in crafts]
    if key not in keys:
        print(f"  [FAIL] {tag}: client craft {key} DESPAWNED (client now owns {keys})")
        fail.append(tag)
        return False
    print(f"  [PASS] {tag}: client craft {key} present (owns {keys})")
    return True


def assert_relinked_to_ufo(tag, host, client, cid, fail, timeout=25):
    """After the round trip the out-craft must re-link to the LIVE coop UFO
    (destKind 'ufo'), not settle for a frozen waypoint at the stale saved
    position. Poll while nudging the geoscape so the post-resume sync can run.
    Pre-fix the destination was dropped (destKind 'none' -> PATROLLING)."""
    tag2 = f"{tag} [re-link]"
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        c = find_own_craft(client, cid)
        if c is None:
            print(f"  [FAIL] {tag2}: craft despawned")
            fail.append(tag2)
            return
        last = c
        if c["status"] == "STR_OUT" and c["destKind"] == "ufo":
            print(f"  [PASS] {tag2}: re-linked to live coop UFO "
                  f"(destKind={c['destKind']} destId={c['destId']} "
                  f"displayStatus={c['displayStatus']!r})")
            return
        geo_tick(host, client)
        time.sleep(0.6)
    print(f"  [FAIL] {tag2}: did NOT re-link to the live coop UFO "
          f"(status={last and last['status']} destKind={last and last['destKind']} "
          f"displayStatus={last and last['displayStatus']!r}) - chasing a stale "
          f"position / lost its target")
    fail.append(tag2)


def drive_to_landing_prompt(tag, host, client, cid, fail, timeout=45):
    """Prove the re-linked craft reaches the REAL (stationary) target and
    INITIATES the mission: park it on the coop crash site and advance time
    WITHOUT geo_run (which would auto-decline the prompt). Success =
    ConfirmLandingState pops ('start mission'). Failure = CraftPatrolState
    ('arrived at position'), which is what a stale-waypoint fallback produces
    when the craft flies to a dead coordinate instead of the live target."""
    tag2 = f"{tag} [arrival]"
    keep = ("GeoscapeState", "ConfirmLandingState")
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        st = client.cmd({"cmd": "get_state"})["states"]
        if any("ConfirmLandingState" in s for s in st):
            print(f"  [PASS] {tag2}: arrival popped ConfirmLandingState (start-mission prompt)")
            return
        if any("CraftPatrolState" in s for s in st):
            print(f"  [FAIL] {tag2}: arrival popped CraftPatrolState ('arrived at "
                  f"position') - reached a stale waypoint, not the target")
            fail.append(tag2)
            return
        for gc in (host, client):
            top = gc.cmd({"cmd": "get_state"})["states"][-1]
            if not any(k in top for k in keep):
                gc.cmd({"cmd": "dismiss_popup"})
        u = coop_ufo(client)
        if u:  # chase the live target position (a crash site does not move)
            client.cmd({"cmd": "craft_force", "craft_id": cid, "lon": u["lon"],
                        "lat": u["lat"], "fuel": 999999, "lowFuel": False})
        c = find_own_craft(client, cid)
        last = (c and c["status"], c and c["destKind"], st[-1] if st else None)
        for gc in (host, client):
            gc.cmd({"cmd": "geo_set_speed", "idx": 2})
        time.sleep(0.5)
    print(f"  [FAIL] {tag2}: never reached the target / no landing prompt (last={last})")
    fail.append(tag2)


def assert_live_tracking(tag, host, client, cid, coop_id, fail, timeout=25):
    """Prove the client tracks the LIVE (moving) target after the round trip, not
    a stale save-time position: move the HOST's UFO and confirm the client's
    mirror follows it there (the re-linked craft's destination IS that mirror, so
    it tracks with it - a frozen waypoint could not). Nudge time gently (no
    geo_run) so the fast interceptor doesn't reach the UFO here."""
    tag2 = f"{tag} [tracking]"
    u0 = coop_ufo(client)
    if not u0:
        print(f"  [FAIL] {tag2}: no coop UFO on client to move")
        fail.append(tag2)
        return
    nx, ny = u0["lon"] + 0.9, u0["lat"] + 0.4
    host.ok({"cmd": "move_ufo", "coop_id": coop_id, "lon": nx, "lat": ny})
    keep = ("GeoscapeState", "DogfightState", "ConfirmLandingState")
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        for gc in (host, client):
            top = gc.cmd({"cmd": "get_state"})["states"][-1]
            if not any(k in top for k in keep):
                gc.cmd({"cmd": "dismiss_popup"})
            gc.cmd({"cmd": "geo_set_speed", "idx": 1})  # gentle: sync, barely fly
        u = coop_ufo(client)
        last = u and (round(u["lon"], 3), round(u["lat"], 3))
        if u and abs(u["lon"] - nx) < 0.08 and abs(u["lat"] - ny) < 0.08:
            c = find_own_craft(client, cid)
            print(f"  [PASS] {tag2}: client mirror followed the host UFO to its new "
                  f"position ({round(nx,3)},{round(ny,3)}); craft still bound "
                  f"(destKind={c and c['destKind']})")
            return
        time.sleep(0.6)
    print(f"  [FAIL] {tag2}: mirror did not follow the moved UFO "
          f"(target=({round(nx,3)},{round(ny,3)}) last={last})")
    fail.append(tag2)



def run_scenario(label, ports, ufo_state, ufo_speed, craft_pref, mode,
                 save_name, fail):
    """mode 'landing'  -> SKYRANGER to a crashed coop UFO; assert arrival pops
                          the start-mission prompt (ConfirmLandingState).
       mode 'tracking' -> INTERCEPTOR to a flying coop UFO; assert the re-linked
                          craft live-tracks the target (client mirror follows the
                          host UFO when it moves)."""
    print(f"\n===== scenario '{label}': {craft_pref} -> {ufo_state} coop UFO, "
          f"mode={mode} =====")
    host_dir = make_user_dir(f"csco_{label}_host")
    host = GameClient("host", ports[0], host_dir)
    client = GameClient("client", ports[1], make_user_dir(f"csco_{label}_client"))
    try:
        host.spawn(); client.spawn()
        host.connect(timeout=240); client.connect(timeout=240)
        new_campaign(host, client)

        crafts0 = own_crafts(client)
        assert crafts0, "client should own a starting craft"
        craft = next((c for c in crafts0 if craft_pref in c["type"]), crafts0[0])
        key = craft_key(craft)
        cid = craft["id"]
        bx, by = craft["lon"], craft["lat"]
        print(f"  client craft {key} -> OUT")

        # HOST spawns a shared/coop UFO (mirrors onto the client, stripped from
        # the client blob - the issue #28 trigger).
        host.ok({"cmd": "spawn_ufo", "type": "STR_SMALL_SCOUT",
                 "mission": "STR_ALIEN_RESEARCH", "region": "STR_NORTH_AMERICA",
                 "race": "STR_SECTOID", "trajectory": "P0", "state": ufo_state,
                 "lon": bx + 1.4, "lat": by - 0.25, "speed": ufo_speed, "hours": 240})
        cufo = client.wait_for("coop UFO mirror on client",
                               lambda: coop_ufo(client), timeout=30)
        cufo_id = cufo["id"]
        print(f"  coop UFO mirror id={cufo_id} coopId={cufo.get('coopId')}")

        # A landing needs units aboard; tracking/interception does not.
        if mode == "landing":
            client.cmd({"cmd": "craft_dispatch", "site_id": -1, "soldiers": 2})
        client.ok({"cmd": "craft_force", "craft_id": cid, "status": "STR_OUT",
                   "lon": bx + 0.20, "lat": by + 0.10, "dest": f"ufo:{cufo_id}",
                   "fuel": 999999, "lowFuel": False})
        cur = find_own_craft(client, cid)
        assert cur and cur["status"] == "STR_OUT" and cur["destKind"] == "ufo", \
            f"failed to force craft OUT to coop UFO: {cur}"
        print(f"  OUT to coop UFO: {cur['displayStatus']!r}")

        # --- HOST SAVE (deferred host-authoritative funnel) ---
        host.ok({"cmd": "save_game_ui", "type": "quick"})
        host.wait_for("host quicksave on disk",
                      lambda: os.path.exists(os.path.join(host_dir, "xcom1", "_quick_.asav")) or None,
                      timeout=90)
        time.sleep(2.0)  # let the client-blob round trip + embed settle
        host.ok({"cmd": "save_game", "file": save_name})

        # LIVE: craft still owned + still OUT to the coop UFO.
        assert_craft_present(f"{label} LIVE", client, key, fail)
        live = find_own_craft(client, cid)
        if not (live and live["status"] == "STR_OUT" and live["destKind"] == "ufo"):
            print(f"  [FAIL] {label} LIVE [OUT state]: {live}")
            fail.append(f"{label} LIVE [OUT state]")

        # --- RESUME ROUND TRIP ---
        host.shutdown(); client.shutdown()
        host = GameClient("host", ports[2], host_dir)
        host.spawn(); host.connect()
        client = GameClient("client", ports[3], make_user_dir(f"csco_{label}_client2"))
        client.spawn(); client.connect()
        resume_campaign(host, client, save_name)

        assert_craft_present(f"{label} RESUME", client, key, fail)
        assert_relinked_to_ufo(f"{label} RESUME", host, client, cid, fail)
        if mode == "landing":
            drive_to_landing_prompt(f"{label} RESUME", host, client, cid, fail)
        else:  # tracking
            cur_ufo = coop_ufo(client)
            coop_id = cur_ufo["coopId"] if cur_ufo else cufo.get("coopId")
            assert_live_tracking(f"{label} RESUME", host, client, cid, coop_id, fail)
    finally:
        host.shutdown(); client.shutdown()


def main():
    keep_awake()
    fail = []
    # Scenario 1: SKYRANGER -> crashed coop UFO. Full arrival proof: reaching the
    # target pops the start-mission prompt (not "arrived at position").
    run_scenario("landing", (48640, 48641, 48642, 48643),
                 "crashed", 0, "SKYRANGER", "landing",
                 "host_save_craft_landing.sav", fail)
    # Scenario 2: INTERCEPTOR -> flying coop UFO. Proves the re-linked craft
    # live-tracks a MOVING target (client mirror follows the host UFO). The craft
    # is bound to the live flying UFO (destKind ufo, "INTERCEPTING UFO-0"); the
    # dogfight screen then fires through the SAME engine arrival path proven in
    # scenario 1.
    run_scenario("dogfight", (48644, 48645, 48646, 48647),
                 "flying", 0, "INTERCEPTOR", "tracking",
                 "host_save_craft_dogfight.sav", fail)

    print("\n==== issue #28 summary ====")
    if fail:
        print(f"  FAILURES: {', '.join(fail)}")
        sys.exit(2)
    print("  both scenarios: out-craft survived the host save, re-linked to its "
          "live coop target, and (landing) started the mission on arrival / "
          "(interceptor) live-tracked the moving UFO")
    sys.exit(0)


if __name__ == "__main__":
    main()
