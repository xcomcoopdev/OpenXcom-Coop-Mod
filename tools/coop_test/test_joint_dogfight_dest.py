"""PRD-DF03 GAP-8 (shared/replicated dogfights): a dogfight that ends in an
auto-return leaves the replica craft's destination + status matching the host,
with NO lag and NO orphan waypoint - because the host is the sole geoscape sim
and the replica never sets _dest locally.

HISTORICAL BUG (GAP-8, the pre-DF01 J08 initiator model): the initiating client
ran the craft's dogfight and its geoscape follow-up locally, so it set the craft's
_dest (chase the UFO) on its OWN world. When the fight ended host-side (UFO downed
-> host auto-returns the craft to base) the client kept a STALE _dest pointing at
the now-dead UFO: an orphan waypoint the client's craft chased into nowhere, its
status lagging the host's for as long as the two sims disagreed.

THE FIX (DF01): the host simulates every dogfight AND owns the geoscape sim.
setDestination / returnToBase / waypoint cleanup all live inside DogfightState::
update() + the host geoscape loop - code a replica's update() early-returns before
ever reaching. The replica only RENDERS host state (df_state) + the position
snapshot, so it never sets _dest for a dogfight; there is nothing to leave orphaned
and nothing to lag.

This test asserts the FIXED behavior: after the host downs the UFO and auto-returns
the craft,
  * the crash mirrors identically (status + crashId) - one authoritative outcome;
  * the replica craft carries NO orphan waypoint to the downed UFO (destKind is not
    "ufo" / destId is not the dead UFO) on either machine;
  * the replica craft's status + destination CONVERGE to the host's (no lag);
  * the run ends world-equal + replica zero-disk.

Run:  python tools/coop_test/test_joint_dogfight_dest.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo

CRASHED = 2  # Ufo::UfoStatus: FLYING=0, LANDED=1, CRASHED=2, DESTROYED=3


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _craft(gc, craft_id, craft_type="STR_AVENGER"):
    # craft ids are per-TYPE (a Skyranger and an Avenger can both be id 1), so match
    # both id and type - matching id alone returns the wrong craft.
    for b in _geo(gc)["bases"]:
        for c in b.get("crafts", []):
            if c["id"] == craft_id and c["type"] == craft_type and not c["coop"]:
                return c
    return None


def _ufo(gc, ufo_id):
    for u in _geo(gc).get("ufos", []):
        if u["id"] == ufo_id:
            return u
    return None


def _df_for(gc, craft_id, ufo_id):
    d = gc.ok({"cmd": "dogfight_state"})
    for df in d.get("dogfights", []):
        if df["craftId"] == craft_id and df["ufoId"] == ufo_id:
            return df
    return None


def _pump(host, client, n=1):
    geo.skip_realtime(host, client, n, speed_idx=0, stuck_timeout=None)


def main():
    js = joint_fixture.bring_up("jdfd", (48804, 48805, 48104))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]

        sc_h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                        "weapon": "STR_CANNON_UC"})
        sc_c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                          "weapon": "STR_CANNON_UC"})
        assert sc_h["craft_id"] == sc_c["craft_id"], "avenger ids diverged"
        avenger_id = sc_h["craft_id"]

        # A slow scout very close to the base, damaged to ~half hull so the first
        # cannon hit crashes it deterministically (the intercept_spectate precedent).
        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH",
                       "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID",
                       "trajectory": "P0", "state": "flying", "speed": 1,
                       "lon": blon + 0.03, "lat": blat})
        ufo_id = ufo["ufo_id"]
        probe = host.ok({"cmd": "set_ufo_damage", "ufo_id": ufo_id, "damage": 0})
        host.ok({"cmd": "set_ufo_damage", "ufo_id": ufo_id,
                 "damage": probe["damageMax"] // 2})

        deadline = time.time() + 45
        while time.time() < deadline and _ufo(client, ufo_id) is None:
            _pump(host, client, 1)
            time.sleep(0.2)
        assert _ufo(client, ufo_id) is not None, "client never materialised the UFO"

        # CLIENT commands the intercept - the exact GAP-8 setup (the commanding client
        # used to run the follow-up locally and orphan its _dest).
        client.ok({"cmd": "craft_order", "order": "target",
                   "craft_id": avenger_id, "craft_type": "STR_AVENGER",
                   "ufo_id": ufo_id})

        deadline = time.time() + 120
        while time.time() < deadline:
            _pump(host, client, 1)
            if _df_for(host, avenger_id, ufo_id) and _df_for(client, avenger_id, ufo_id):
                break
            time.sleep(0.2)
        assert _df_for(host, avenger_id, ufo_id) and _df_for(client, avenger_id, ufo_id), \
            "the dogfight did not open on BOTH machines"
        print("PASS setup: dogfight open on both machines for the intercept")

        # Host downs the scout (drive aggressive; replica buttons render only). The
        # host then AUTO-RETURNS the craft to base - the dogfight-end path GAP-8 covers.
        deadline = time.time() + 150
        crashed = False
        while time.time() < deadline:
            host.cmd({"cmd": "dogfight_action", "action": "aggressive",
                      "craft_id": avenger_id, "ufo_id": ufo_id})
            _pump(host, client, 1)
            uh = _ufo(host, ufo_id)
            if uh and uh["status"] == CRASHED:
                crashed = True
                break
            time.sleep(0.2)
        assert crashed, "host never shot the scout down within 150s"

        # The fight ends host-side; the host AUTO-RETURNS the craft (sets _dest=base).
        # Wait for the dogfight to close on the host, then let the replica converge to
        # the host's craft _dest/status via the position snapshot. GAP-8 is about that
        # convergence (no lag, no orphan), so we assert host==client - not a fixed
        # terminal cell. Use a faster clock so the return actually progresses.
        def _fight_open(gc):
            return _df_for(gc, avenger_id, ufo_id) is not None

        deadline = time.time() + 60
        while time.time() < deadline and (_fight_open(host) or _fight_open(client)):
            geo.skip_realtime(host, client, 1, speed_idx=2, stuck_timeout=None)
            time.sleep(0.2)
        assert not _fight_open(host), "host dogfight never closed after the crash"

        def _converged():
            h, c = _craft(host, avenger_id), _craft(client, avenger_id)
            if not h or not c:
                return None
            same = (h["status"] == c["status"] and h["destKind"] == c["destKind"]
                    and h["destId"] == c["destId"] and not h["inDogfight"] and not c["inDogfight"])
            return (h, c) if same else None

        conv = None
        deadline = time.time() + 90
        while time.time() < deadline:
            geo.skip_realtime(host, client, 1, speed_idx=2, stuck_timeout=None)
            conv = _converged()
            if conv:
                break
            time.sleep(0.3)
        assert conv, (
            "PRD-DF03 GAP-8 FAILED: replica craft never converged to the host's "
            f"_dest/status after the dogfight (host={_craft(host, avenger_id)} "
            f"client={_craft(client, avenger_id)})")
        ch, cc = conv
        print(f"host craft: status={ch['status']} destKind={ch['destKind']} "
              f"destId={ch['destId']} inDogfight={ch['inDogfight']}")
        print(f"client craft: status={cc['status']} destKind={cc['destKind']} "
              f"destId={cc['destId']} inDogfight={cc['inDogfight']}")

        # GAP-8 core: NO orphan waypoint to the downed UFO on either machine.
        for tag, c in (("host", ch), ("client", cc)):
            assert not (c["destKind"] == "ufo" and c["destId"] == ufo_id), (
                f"PRD-DF03 GAP-8 FAILED: {tag} craft still points _dest at the downed "
                f"UFO {ufo_id} (orphan waypoint): destKind={c['destKind']} destId={c['destId']}")
        # The replica converges to the host craft's status + destination - no lag.
        assert cc["status"] == ch["status"], (
            "PRD-DF03 GAP-8 FAILED: replica craft status lags the host "
            f"(host={ch['status']} client={cc['status']})")
        assert cc["destKind"] == ch["destKind"] and cc["destId"] == ch["destId"], (
            "PRD-DF03 GAP-8 FAILED: replica craft destination lags the host "
            f"(host={ch['destKind']}/{ch['destId']} client={cc['destKind']}/{cc['destId']})")
        assert not cc["inDogfight"] and not ch["inDogfight"], "craft still flagged inDogfight"
        print("PASS GAP-8 CLOSED: replica craft _dest/status match the host after the "
              "auto-return - no lag, no orphan waypoint (replica never set _dest locally)")

        # One authoritative outcome (status + crashId mirror), as the DF01 tests assert.
        uh, uc = _ufo(host, ufo_id), _ufo(client, ufo_id)
        assert uh and uc, "lost the UFO from geo_state after the crash"
        assert uh["status"] == CRASHED and uh["crashId"] > 0, f"host recorded no crash: {uh}"
        assert uc["status"] == uh["status"] and uc["crashId"] == uh["crashId"], (
            f"PRD-DF03 FAILED: crash outcome diverged host={uh['status']}/{uh['crashId']} "
            f"client={uc['status']}/{uc['crashId']}")
        print(f"PASS mirrored crash (status={uh['status']} crashId={uh['crashId']})")

        js.finish()
        print("ALL JOINT DOGFIGHT DEST (GAP-8) TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
