"""Validate that a craft's geoscape status matches on the host and the
non-owning client for every status the code can produce.

Background: a peer's craft is simulated on its owner's client; the client only
receives a periodic target_positions snapshot. The fix syncs a pre-localized
status string (destination / dogfight state is NOT replicated, so the client
cannot derive it locally). This test drives the OWNER (host) into each status
and asserts host.displayStatus == client.displayStatus (and status parity).

Hybrid coverage (see plan):
  * Real scenario  : the craft actually flies to a spawned mission site
                     (dispatch -> "heading to <site>" / OUT).
  * Forced scenario: for states whose *producer* is a dogfight/battle/fuel-burn
                     (owned elsewhere, not the sync layer under test), the owner
                     state is force-set, then the real derive+sync+render path
                     runs. Parity is what we assert, and it is what regressed.

Determinism: the parity assertion compares two live sides of the SAME run, so it
needs no RNG pinning. set_seed is called only so the scenario setup reproduces.

Run (point EXE via the build under test):
    python tools/coop_test/test_craft_status_sync.py

Exit 0 = all scenarios parity-matched; 2 = a mismatch/failure.
"""

import ctypes
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, TEST_ROOT, LAND_LON, LAND_LAT
# Reuse the known-good fresh-coop bring-up from the geoscape-sync test.
from test_geoscape_sync import bringup, HOST_LON, HOST_LAT  # PRD-13 S7: local `states` removed (was unused here)

# Mission site + UFO placed FAR from the host base so a dispatched craft is
# perpetually en route (never arrives -> never triggers ConfirmLanding / a
# battle), which keeps it airborne for the whole B-family run.
SITE_LON, SITE_LAT = HOST_LON + 2.6, HOST_LAT - 0.30
UFO_LON, UFO_LAT = HOST_LON + 2.4, HOST_LAT + 0.20


def keep_awake():
    ctypes.windll.kernel32.SetThreadExecutionState(0x80000000 | 0x00000001 | 0x00000002)


def crafts_of(gc):
    """Flat list of every craft geo_state reports for this instance."""
    r = gc.cmd({"cmd": "geo_state"})
    if not r.get("ok"):
        return []
    out = []
    for b in r["bases"]:
        out.extend(b.get("crafts", []))
    return out


def find_craft(gc, craft_id, coop):
    for c in crafts_of(gc):
        if c["id"] == craft_id and bool(c["coop"]) == coop:
            return c
    return None


# Keep game-time creep tiny: a fast speed (1 day/tick) over a poll loop advances
# game-WEEKS, which auto-completes refuel/rearm and unleashes a month of alien
# activity (crashes). idx 1 = 1 game-minute/tick -> negligible sim, still drains
# popups and lets target_positions keep syncing.
GEO_SPEED_SLOW = 1   # 1 min
GEO_SPEED_FLY = 3    # 30 min, used only to get a craft airborne + away


def geo_tick(gc, speed=GEO_SPEED_SLOW):
    """One popup-draining time-advance nudge; keeps the geoscape free-running."""
    return gc.cmd({"cmd": "geo_run", "speed": speed})


class Result:
    def __init__(self):
        self.rows = []      # (label, flag, detail)  flag in {PASS,FAIL,XFAIL}
        self.failed = 0

    def record(self, label, ok, detail, xfail=False):
        # XFAIL = known harness limitation (host-side verified, client parity not
        # exercisable here); reported but does not fail the suite.
        flag = "XFAIL" if (xfail and not ok) else ("PASS" if ok else "FAIL")
        self.rows.append((label, flag, detail))
        if flag == "FAIL":
            self.failed += 1
        print(f"  [{flag}] {label}: {detail}")


def assert_parity(res, host, client, craft_id, label,
                  expect_status=None, expect_dest=None,
                  expect_low=None, expect_mission=None, expect_dogfight=None,
                  timeout=25, refresh=None, expect_host_display=None, known_limit=False):
    """Wait for the client's mirror of the owner craft to converge, then assert
    displayStatus + status parity. `expect_*` check the HOST reached the intended
    state (host structured fields; the client only receives the synced string).
    `refresh`, if given, is re-applied each poll to hold a state the sim keeps
    clearing (e.g. a dest onto a UFO the mission layer keeps despawning)."""
    t0 = time.time()
    deadline = time.time() + timeout
    h = c = None
    while time.time() < deadline:
        # drain popups first (a low-speed tick), THEN (re)apply the state and read
        # immediately, so a state the sim clears on its next think is still set
        # when we sample the host. The client syncs from the host's own frame loop.
        geo_tick(host); geo_tick(client)
        if refresh:
            refresh()
        h = find_craft(host, craft_id, coop=False)
        c = find_craft(client, craft_id, coop=True)
        if h and c and c["displayStatus"] and c["displayStatus"] == h["displayStatus"] \
                and c["status"] == h["status"]:
            break
        time.sleep(0.8)

    if not h:
        return res.record(label, False, "host craft not found")
    if not c:
        return res.record(label, False, "client mirror craft not found")

    # sanity: host actually reached the intended scenario state
    reached = []
    if expect_status is not None and h["status"] != expect_status:
        reached.append(f"host.status={h['status']} != {expect_status}")
    if expect_dest is not None and h["destKind"] != expect_dest:
        reached.append(f"host.destKind={h['destKind']} != {expect_dest}")
    if expect_low is not None and bool(h["lowFuel"]) != expect_low:
        reached.append(f"host.lowFuel={h['lowFuel']} != {expect_low}")
    if expect_mission is not None and bool(h["mission"]) != expect_mission:
        reached.append(f"host.mission={h['mission']} != {expect_mission}")
    if expect_dogfight is not None and bool(h["inDogfight"]) != expect_dogfight:
        reached.append(f"host.inDogfight={h['inDogfight']} != {expect_dogfight}")

    # the actual parity assertion
    parity = (c["displayStatus"] == h["displayStatus"] and c["status"] == h["status"])
    dt = time.time() - t0
    detail = (f"[{dt:4.1f}s] host=({h['status']!r},{h['displayStatus']!r}) "
              f"client=({c['status']!r},{c['displayStatus']!r})")

    # Known-limitation path (UFO-dest statuses): the harness can't hold a stable
    # spawned UFO -- the alien-mission layer despawns it and all UFOs collide on
    # id 0, so the craft's dest is cleared/mis-targeted and the host's serialized
    # status oscillates. The three UFO strings flow through the SAME single synced
    # field as the 10 stable cases, so client parity is covered by construction;
    # it just isn't independently *stageable* here. Record a genuine UFO-status
    # parity match as PASS if we catch one, otherwise XFAIL (never a false PASS on
    # both sides merely agreeing on the non-UFO fallback, nor a hard FAIL).
    if known_limit:
        host_is_ufo = bool(expect_host_display) and expect_host_display in h["displayStatus"]
        if parity and host_is_ufo:
            return res.record(label, True, detail)
        return res.record(label, False,
                          f"UFO dest not stably stageable in-harness "
                          f"(host derived {h['displayStatus']!r}) | {detail}",
                          xfail=True)

    if reached:
        return res.record(label, False, "scenario not reached: " + "; ".join(reached) + " | " + detail)
    return res.record(label, parity, detail)


def pick_crafts(host):
    """Return (transport_id, armed_id): a transport (weapon slots == 0, has
    soldier capacity) for the airborne B-family, and an armed craft (weapon
    slots > 0) for the REARMING case. Falls back to the same craft if the base
    only has one kind."""
    own = [c for c in crafts_of(host) if not c["coop"]]
    transport = next((c for c in own if c.get("weapons", 0) == 0), None)
    armed = next((c for c in own if c.get("weapons", 0) > 0), None)
    t = (transport or armed or own[0])["id"] if own else None
    a = (armed or transport or own[0])["id"] if own else None
    return t, a


def force(host, craft_id, **fields):
    fields["cmd"] = "craft_force"
    fields["craft_id"] = craft_id
    return host.ok(fields)


def fly_out(host, craft_id, max_ticks=30):
    """Get the craft airborne and a meaningful distance from its base, so a
    subsequent dest=base ('returning') isn't instantly cancelled by think()
    (which fires when the craft is already AT its base)."""
    base = None
    for b in host.cmd({"cmd": "geo_state"}).get("bases", []):
        for c in b.get("crafts", []):
            if c["id"] == craft_id:
                base = None  # base coords not in payload; use displacement below
    start = find_craft(host, craft_id, coop=False)
    sx, sy = (start or {}).get("lon", 0.0), (start or {}).get("lat", 0.0)

    def gt(gc):
        t = gc.cmd({"cmd": "geo_state"}).get("time", {})
        return f"{t.get('day')}d {t.get('hour')}:{t.get('minute'):02d}" if t else "?"

    print(f"    [fly_out] host time start={gt(host)} client={gt(client_ref[0])}")
    for i in range(max_ticks):
        rh = geo_tick(host, speed=GEO_SPEED_FLY)
        rc = geo_tick(client_ref[0], speed=GEO_SPEED_FLY)
        c = find_craft(host, craft_id, coop=False)
        if i % 6 == 0:
            hs = host.cmd({"cmd": "get_state"})["states"]
            cs = client_ref[0].cmd({"cmd": "get_state"})["states"]
            print(f"    [fly_out] i={i} htime={gt(host)} craft=({c['status']},{c['lon']:.3f}) "
                  f"hdrain={rh.get('drained')}/{rh.get('topType','?')[-24:]} "
                  f"cdrain={rc.get('drained')}/{rc.get('topType','?')[-24:]} "
                  f"htop={hs[-1][-20:]} ctop={cs[-1][-20:]}")
        # movement from the base is the reliable airborne signal (a craft that
        # has physically left its base can't have dest=base instantly cancelled)
        if c and (abs(c["lon"] - sx) + abs(c["lat"] - sy)) > 0.15:
            print(f"    [fly_out] airborne+away at i={i} htime={gt(host)} lon={c['lon']:.3f}")
            return True
        time.sleep(0.5)
    return False


client_ref = [None]  # set in run(), so fly_out can tick the client too


def assert_control_guard(res, client):
    """On the CLIENT, the geoscape craft dialog must hide the Base/Target/Patrol
    buttons for a PEER's craft (coop=true) so a non-owning player can't redirect
    another player's ship, while still showing them for its OWN craft."""
    ccrafts = crafts_of(client)
    peer = next((c for c in ccrafts if c["coop"]), None)
    own = next((c for c in ccrafts if not c["coop"]), None)

    if not peer:
        res.record("GUARD peer craft buttons hidden", False, "no peer (coop) craft on client")
    else:
        r = client.cmd({"cmd": "geo_craft_buttons", "craft_id": peer["id"], "coop": True})
        ok = bool(r.get("ok")) and r.get("buttons_visible") is False
        res.record("GUARD peer craft buttons hidden", ok,
                   f"peer craft id={peer['id']} buttons_visible={r.get('buttons_visible')} "
                   f"(err={r.get('error')})")

    if not own:
        res.record("GUARD own craft buttons shown", False, "no own craft on client")
    else:
        r = client.cmd({"cmd": "geo_craft_buttons", "craft_id": own["id"], "coop": False})
        ok = bool(r.get("ok")) and r.get("buttons_visible") is True
        res.record("GUARD own craft buttons shown", ok,
                   f"own craft id={own['id']} buttons_visible={r.get('buttons_visible')} "
                   f"(err={r.get('error')})")


def run(host, client):
    res = Result()
    client_ref[0] = client
    host.ok({"cmd": "set_seed", "seed": 12345})

    # Control guard: peer craft is uncommandable from the non-owning client.
    assert_control_guard(res, client)

    transport_id, _ = pick_crafts(host)
    if transport_id is None:
        res.record("setup", False, "no owner craft on host base")
        return res

    # The coop start base only has an unarmed transport (SKYRANGER). Add a fully
    # armed INTERCEPTOR for the REARMING case and the UFO-interception trio: only
    # a combat craft can hold a flying-UFO destination (a transport's dest is
    # cleared, which is why these were unstable before).
    ic = host.ok({"cmd": "spawn_craft", "type": "STR_INTERCEPTOR", "weapon": "STR_STINGRAY"})
    intercept_id = ic["craft_id"]
    print(f"  transport craft id={transport_id}  interceptor id={intercept_id} weapons={ic['weapons']}")

    # ---- A-family: base-lifecycle _status (craft stays docked) ----
    # A1 READY (fresh craft, at base)
    assert_parity(res, host, client, transport_id, "A1 STR_READY", expect_status="STR_READY")
    # A5 REPAIRS
    force(host, transport_id, damage=60, checkup=True)
    assert_parity(res, host, client, transport_id, "A5 STR_REPAIRS", expect_status="STR_REPAIRS")
    force(host, transport_id, damage=0, checkup=True)
    # A4 REARMING (INTERCEPTOR: has weapon hardpoints; drop ammo below max)
    force(host, intercept_id, ammo=0, checkup=True)
    assert_parity(res, host, client, intercept_id, "A4 STR_REARMING", expect_status="STR_REARMING")
    force(host, intercept_id, ammo=99999, checkup=True)
    # A3 REFUELLING
    force(host, transport_id, fuel=1, checkup=True)
    assert_parity(res, host, client, transport_id, "A3 STR_REFUELLING", expect_status="STR_REFUELLING")

    # ---- UFO-coupled trio: INTERCEPTOR holds the flying-UFO dest ----
    # Run this BEFORE the B-family: the B-family's fly-out advances hours of game
    # time, which spawns real alien UFOs and lets the mission layer despawn a lone
    # spawned one. Here almost no time has passed, so the spawned UFO is the only
    # one and persists. Re-apply dest each poll as insurance.
    fly = host.ok({"cmd": "spawn_ufo", "type": "STR_SMALL_SCOUT", "mission": "STR_ALIEN_RESEARCH",
                   "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID", "trajectory": "P0",
                   "state": "flying", "lon": UFO_LON, "lat": UFO_LAT})
    # B7 INTERCEPTING (dest = flying UFO)
    assert_parity(res, host, client, intercept_id, "B7 STR_INTERCEPTING_UFO", expect_dest="ufo",
                  refresh=lambda: force(host, intercept_id, dest=f"ufo:{fly['ufo_id']}"))
    # B6 TAILING (dest = flying UFO + in dogfight)
    assert_parity(res, host, client, intercept_id, "B6 STR_TAILING_UFO", expect_dest="ufo", expect_dogfight=True,
                  refresh=lambda: force(host, intercept_id, dest=f"ufo:{fly['ufo_id']}", dogfight=True))
    force(host, intercept_id, dogfight=False)
    # B8 DESTINATION (dest = crashed UFO)
    crash = host.ok({"cmd": "spawn_ufo", "type": "STR_SMALL_SCOUT", "mission": "STR_ALIEN_RESEARCH",
                     "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID", "trajectory": "P0",
                     "state": "crashed", "lon": UFO_LON + 0.1, "lat": UFO_LAT})
    assert_parity(res, host, client, intercept_id, "B8 STR_DESTINATION (ufo)", expect_dest="ufo",
                  refresh=lambda: force(host, intercept_id, dest=f"ufo:{crash['ufo_id']}"))
    force(host, intercept_id, dest="base")  # park the interceptor

    # Fully restore the transport so it can actually launch for the B-family.
    force(host, transport_id, fuel=999999, damage=0, checkup=True)

    # ---- B-family: airborne display string (transport, flown out) ----
    # B9 DESTINATION -> mission site (REAL: spawn site, dispatch, fly)
    site = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                    "deployment": "STR_TERROR_MISSION", "lon": SITE_LON, "lat": SITE_LAT,
                    "race": "STR_SECTOID", "hours": 48})
    disp = host.cmd({"cmd": "craft_dispatch", "site_id": site["site_id"], "soldiers": 2})
    if not disp.get("ok"):
        res.record("B9 STR_DESTINATION (site)", False, f"dispatch failed: {disp.get('error')}")
    else:
        airborne = fly_out(host, transport_id)
        assert_parity(res, host, client, transport_id, "B9 STR_DESTINATION (site)", expect_dest="site")
        if not airborne:
            res.record("fly_out", False, "craft did not get airborne+away; B4/B5 may be unreliable")

    # craft is now OUT and away from base -> destination-based statuses are stable
    # B5 RETURNING_TO_BASE
    force(host, transport_id, dest="base")
    assert_parity(res, host, client, transport_id, "B5 STR_RETURNING_TO_BASE", expect_dest="base")
    # B4 PATROLLING (dest cleared)
    force(host, transport_id, dest="patrol")
    assert_parity(res, host, client, transport_id, "B4 STR_PATROLLING", expect_dest="none")
    # B2 LOW_FUEL (precedence over destination)
    force(host, transport_id, lowFuel=True)
    assert_parity(res, host, client, transport_id, "B2 STR_LOW_FUEL", expect_low=True)
    force(host, transport_id, lowFuel=False)
    # B3 MISSION_COMPLETE
    force(host, transport_id, mission=True)
    assert_parity(res, host, client, transport_id, "B3 STR_MISSION_COMPLETE", expect_mission=True)
    force(host, transport_id, mission=False)

    return res


def main():
    keep_awake()
    host = GameClient("host", 47811, make_user_dir("craftstatus-host"))
    client = GameClient("client", 47812, make_user_dir("craftstatus-client"))
    t_start = time.time()
    try:
        print("[bringup] fresh 2-player coop -> live geoscape ...")
        t0 = time.time()
        bringup(host, client)
        t_bringup = time.time() - t0
        print(f"[bringup] done in {t_bringup:.1f}s")
        print("[run] driving craft statuses ...")
        t0 = time.time()
        res = run(host, client)
        t_run = time.time() - t0
    finally:
        host.shutdown(); client.shutdown()
    t_total = time.time() - t_start

    print("\n==== craft-status parity summary ====")
    for label, flag, detail in res.rows:
        # detail begins with "[  X.Xs]" for asserted scenarios
        secs = detail.split("]")[0].lstrip("[") + "]" if detail.startswith("[") else ""
        print(f"  {flag:5s}  {label:28s} {secs}")
    print(f"\n  timings: bringup {t_bringup:.1f}s | scenarios {t_run:.1f}s | total {t_total:.1f}s")
    npass = sum(1 for _, f, _ in res.rows if f == "PASS")
    nxfail = sum(1 for _, f, _ in res.rows if f == "XFAIL")
    print(f"  {npass} pass, {nxfail} xfail (known limit), {res.failed} fail  "
          f"/ {len(res.rows)} scenarios")
    sys.exit(2 if res.failed else 0)


if __name__ == "__main__":
    main()
