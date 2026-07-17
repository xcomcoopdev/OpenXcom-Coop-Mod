"""PRD-J08: JOINT shared craft command, interception + dogfights.

A JOINT campaign is one host-authoritative world: ANY player commands ANY
craft. Orders ride the J03 joint_cmd protocol (craft_launch / craft_retarget /
craft_return / craft_patrol); the host validates the vanilla fuel/crew/status
rules against the live world and applies them in ARRIVAL order
(last-command-wins). Replica craft positions/status/fuel flow through the
joint:true position snapshot. Dogfights follow locked decision (a): the seat
whose order engaged the UFO simulates the DogfightState on ITS machine; the
result is reported to the host (joint_cmd{dogfight_result}), applied to the
authoritative world and rebroadcast.

  LIST    intercept_list rows identical on host + client (fenced SEPARATE
          peer-base hiding); the geoscape craft dialog shows ENABLED command
          buttons on the CLIENT for a shared craft.
  LAUNCH  client launches a host-hangared interceptor at a seeded (far)
          mission site via the REAL ConfirmDestinationState -> craft leaves,
          both machines agree on status/destination/position.
  RETARGET host redirects the SAME craft mid-flight to a map point ->
          replica follows (last-command-wins; shared waypoint id lock-step).
  RETURN  client orders return-to-base -> lands + refuels host-side; status
          and fuel visible on the replica (snapshot fuel/damage sync).
  DOGFIGHT client launches at a seeded nearby UFO -> host brokers
          dogfight_start -> ONLY the client gets the interactive dogfight ->
          aggressive attack crashes the UFO -> host applies dogfight_result ->
          crash (status+crashId) identical on both; craft damage synced.

Scaffolding note: spawn_craft (the Avenger used for the dogfight) mutates one
machine's world directly, so it is called on BOTH machines - deterministic and
id-lock-step, the same give_items/add_base precedent from J05. spawn_ufo runs
on the HOST only; the joint snapshot materializes the UFO on the replica.

Run:  python tools/coop_test/test_joint_craft.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo

UFO_STATUS_CRASHED = 2


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _craft(gc, craft_id, craft_type):
    for b in _geo(gc)["bases"]:
        for c in b.get("crafts", []):
            if c["id"] == craft_id and c["type"] == craft_type and not c["coop"]:
                return c
    return None


def _ufo(gc, ufo_id):
    for u in _geo(gc)["ufos"]:
        if u["id"] == ufo_id:
            return u
    return None


def _dogfights(gc):
    return gc.ok({"cmd": "dogfight_state"})


def _rows(gc):
    """intercept_list rows as a sorted, comparable list of tuples."""
    r = gc.ok({"cmd": "intercept_list"})
    return sorted((row["type"], row["craftId"], row["base"]) for row in r["rows"])


def _poll_both(host, client, label, pred, timeout=60, speed_idx=1):
    """Advance time on both machines (popups auto-drained) until
    pred(host_snapshot_fn) and pred(client_snapshot_fn) both hold."""
    deadline = time.time() + timeout
    last = ("", "")
    while time.time() < deadline:
        geo.skip_realtime(host, client, 1, speed_idx=speed_idx)
        h, c = pred(host), pred(client)
        last = (h, c)
        if h is True and c is True:
            return
        time.sleep(0.3)
    raise AssertionError(f"{label}: not converged after {timeout}s "
                         f"(host={last[0]!r} client={last[1]!r})")


def main():
    host_dir = make_user_dir("jcraft_host")
    client_dir = make_user_dir("jcraft_client")
    host = GameClient("host", 48760, host_dir)
    client = GameClient("client", 48761, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        session.new_campaign(host, client, port="48060", campaign_mode="joint")
        geo.wait_both_ready(host, client)

        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]
        interceptors = [c for c in b0["crafts"] if c["type"] == "STR_INTERCEPTOR"]
        assert len(interceptors) >= 1, f"no starting interceptor: {b0['crafts']}"
        icept = interceptors[0]
        print(f"setup: base at ({blon:.3f},{blat:.3f}); "
              f"interceptor id {icept['id']}")

        # ================================================================
        # 1) LIST: intercept rows identical; client sees ENABLED buttons.
        # ================================================================
        rows_h, rows_c = _rows(host), _rows(client)
        assert rows_h == rows_c and len(rows_h) >= 2, \
            f"intercept lists differ: host={rows_h} client={rows_c}"
        btn = client.ok({"cmd": "geo_craft_buttons", "craft_id": icept["id"],
                         "coop": False})
        assert btn["buttons_visible"] is True, \
            f"client craft-command buttons hidden in JOINT: {btn}"
        print(f"PASS list: {len(rows_h)} identical intercept rows; "
              f"client buttons ENABLED")

        # ================================================================
        # 2) LAUNCH: client sends the interceptor at a far seeded site.
        # ================================================================
        site = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                        "deployment": "STR_TERROR_MISSION", "lon": blon + 2.6,
                        "lat": blat - 0.30, "race": "STR_SECTOID", "hours": 96})
        site_id = site["site_id"]
        _poll_both(host, client, "site visible",
                   lambda gc: any(s["id"] == site_id
                                  for s in _geo(gc)["missionSites"]),
                   timeout=30)

        client.ok({"cmd": "craft_order", "order": "target",
                   "craft_id": icept["id"], "craft_type": "STR_INTERCEPTOR",
                   "site_id": site_id})

        def _flying_at_site(gc):
            c = _craft(gc, icept["id"], "STR_INTERCEPTOR")
            if not c:
                return "craft missing"
            ok = (c["status"] == "STR_OUT" and c["destKind"] == "site"
                  and c["destId"] == site_id)
            return True if ok else f"{c['status']}/{c['destKind']}/{c['destId']}"

        _poll_both(host, client, "launch parity", _flying_at_site, timeout=60)

        # positions converge (replica tracks host through the snapshot)
        def _pos_synced(gc):
            if gc is host:
                return True
            ch = _craft(host, icept["id"], "STR_INTERCEPTOR")
            cc = _craft(client, icept["id"], "STR_INTERCEPTOR")
            if not ch or not cc:
                return "missing"
            near = (abs(ch["lon"] - cc["lon"]) < 0.5
                    and abs(ch["lat"] - cc["lat"]) < 0.5)
            left = abs(cc["lon"] - blon) > 1e-6 or abs(cc["lat"] - blat) > 1e-6
            return True if (near and left) else f"h={ch['lon']:.3f} c={cc['lon']:.3f}"

        _poll_both(host, client, "position sync", _pos_synced, timeout=45)
        print("PASS launch: client order -> craft OUT at site on both, "
              "replica position tracks host")

        # ================================================================
        # 3) RETARGET: HOST redirects the same craft mid-flight to a point
        #    (last-command-wins by host arrival order). The point is FAR so
        #    the craft cannot reach it while we poll (reaching a waypoint
        #    pops a patrol dialog and nulls the destination).
        # ================================================================
        host.ok({"cmd": "craft_order", "order": "target",
                 "craft_id": icept["id"], "craft_type": "STR_INTERCEPTOR",
                 "lon": blon - 2.0, "lat": blat})

        def _at_waypoint(gc):
            c = _craft(gc, icept["id"], "STR_INTERCEPTOR")
            if not c:
                return "craft missing"
            ok = c["status"] == "STR_OUT" and c["destKind"] == "other"
            return True if ok else f"{c['status']}/{c['destKind']}"

        _poll_both(host, client, "retarget parity", _at_waypoint, timeout=45)
        print("PASS retarget: host mid-flight redirect -> waypoint dest on both "
              "(last-command-wins)")

        # ================================================================
        # 4) RETURN: client orders it home; lands + refuels host-side;
        #    status + fuel visible on the replica.
        # ================================================================
        client.ok({"cmd": "craft_order", "order": "return",
                   "craft_id": icept["id"], "craft_type": "STR_INTERCEPTOR"})

        def _heading_home(gc):
            c = _craft(gc, icept["id"], "STR_INTERCEPTOR")
            if not c:
                return "craft missing"
            return True if c["destKind"] == "base" else c["destKind"]

        _poll_both(host, client, "return parity", _heading_home, timeout=45)

        # land + refuel: advance until the HOST craft is fully READY again,
        # then the replica must show the same status and the same fuel.
        deadline = time.time() + 180
        while time.time() < deadline:
            geo.skip_ingame_time(host, client, minutes=120, speed_idx=4,
                                 real_timeout=30)
            ch = _craft(host, icept["id"], "STR_INTERCEPTOR")
            if ch and ch["status"] == "STR_READY":
                break
        else:
            raise AssertionError("interceptor never landed+refuelled to READY")

        def _ready_synced(gc):
            ch = _craft(host, icept["id"], "STR_INTERCEPTOR")
            cc = _craft(client, icept["id"], "STR_INTERCEPTOR")
            if not ch or not cc:
                return "missing"
            ok = (ch["status"] == "STR_READY" and cc["status"] == ch["status"]
                  and cc["fuel"] == ch["fuel"])
            return True if ok else f"h={ch['status']}/{ch['fuel']} c={cc['status']}/{cc['fuel']}"

        _poll_both(host, client, "landed status+fuel sync", _ready_synced,
                   timeout=45)
        print("PASS return: landed + refuelled host-side; READY status and "
              "fuel identical on replica")

        # ================================================================
        # 5) DOGFIGHT: client engages a seeded UFO; ONLY the client gets the
        #    interactive dogfight; the crash lands identically on both.
        # ================================================================
        # A craft faster than the scout so a break-off cannot escape, armed
        # with cannons only (low damage -> crash, never destroy). BOTH
        # machines spawn it (deterministic scaffolding, ids lock-step).
        sc_h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                        "weapon": "STR_CANNON_UC"})
        sc_c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                          "weapon": "STR_CANNON_UC"})
        assert sc_h["craft_id"] == sc_c["craft_id"], \
            f"avenger ids diverged: {sc_h['craft_id']} vs {sc_c['craft_id']}"
        avenger_id = sc_h["craft_id"]

        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH",
                       "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID",
                       "trajectory": "P0", "state": "flying", "speed": 1,
                       "lon": blon + 0.10, "lat": blat - 0.15})
        ufo_id = ufo["ufo_id"]
        dmg = host.ok({"cmd": "set_ufo_damage", "ufo_id": ufo_id, "damage": 0})
        half = dmg["damageMax"] // 2
        host.ok({"cmd": "set_ufo_damage", "ufo_id": ufo_id, "damage": half})

        _poll_both(host, client, "ufo visible",
                   lambda gc: (_ufo(gc, ufo_id) is not None
                               and _ufo(gc, ufo_id)["detected"]) or "not seen",
                   timeout=30)

        client.ok({"cmd": "craft_order", "order": "target",
                   "craft_id": avenger_id, "craft_type": "STR_AVENGER",
                   "ufo_id": ufo_id})

        # launch parity at a seeded UFO (both agree before/while engaging)
        def _at_ufo(gc):
            c = _craft(gc, avenger_id, "STR_AVENGER")
            if not c:
                return "craft missing"
            ok = (c["status"] == "STR_OUT" and c["destKind"] == "ufo"
                  and c["destId"] == ufo_id)
            return True if ok else f"{c['status']}/{c['destKind']}"

        _poll_both(host, client, "ufo launch parity", _at_ufo, timeout=45)

        # the interactive dogfight must open on the CLIENT (initiator) only
        deadline = time.time() + 120
        opened = False
        while time.time() < deadline:
            geo.skip_realtime(host, client, 1, speed_idx=1)
            dc = _dogfights(client)
            dh = _dogfights(host)
            assert dh["count"] == 0, \
                f"host opened a dogfight UI for a client-commanded craft: {dh}"
            if dc["count"] >= 1:
                opened = True
                break
        assert opened, "client dogfight UI never opened (dogfight_start lost?)"
        print("PASS dogfight start: interactive dogfight on the CLIENT only")

        # From here keep geo time at 5-sec speed: the dogfight sim is real-time
        # (independent of geo speed), and a slow clock keeps the host's craft
        # from burning through its fuel while the client fights.

        client.ok({"cmd": "dogfight_action", "action": "aggressive"})

        # crash lands on the HOST (authoritative apply), then on the client.
        # Re-assert aggressive every poll: if the UFO broke off and the craft
        # re-engaged, the fresh dogfight would otherwise idle at standoff.
        def _crashed(gc):
            if gc is client:
                client.cmd({"cmd": "dogfight_action", "action": "aggressive"})
            u = _ufo(gc, ufo_id)
            if not u:
                return "ufo missing"
            ok = u["status"] == UFO_STATUS_CRASHED and u["crashId"] > 0 \
                and u["detected"]
            return True if ok else f"status={u['status']} crash={u['crashId']}"

        _poll_both(host, client, "crash on both", _crashed, timeout=150,
                   speed_idx=0)
        uh, uc = _ufo(host, ufo_id), _ufo(client, ufo_id)
        assert uh["crashId"] == uc["crashId"], \
            f"crash marker id differs: host={uh['crashId']} client={uc['crashId']}"
        assert uh["damage"] == uc["damage"], \
            f"ufo damage differs: {uh['damage']} vs {uc['damage']}"

        # the client dogfight closed, and the craft outcome synced to the host
        def _fight_over(gc):
            if gc is client and _dogfights(client)["count"] != 0:
                return "client dogfight still open"
            c = _craft(gc, avenger_id, "STR_AVENGER")
            if not c:
                return "craft missing"
            return True if not c["inDogfight"] else "still inDogfight"

        _poll_both(host, client, "dogfight closed", _fight_over, timeout=45,
                   speed_idx=0)
        ch = _craft(host, avenger_id, "STR_AVENGER")
        cc = _craft(client, avenger_id, "STR_AVENGER")
        assert ch["damage"] == cc["damage"], \
            f"craft damage not synced: host={ch['damage']} client={cc['damage']}"
        print(f"PASS dogfight: UFO crashed (crashId {uh['crashId']}) identically "
              f"on both; craft damage synced ({ch['damage']})")

        # standing invariant: the JOINT replica never wrote save data to disk.
        session.assert_client_zero_disk(client_dir)
        print("PASS zero-disk: client (replica) user dir clean")

        print("ALL JOINT CRAFT TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
