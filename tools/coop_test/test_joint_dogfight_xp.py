"""PRD-DF03 GAP-7 (shared/replicated dogfights): pilot dogfight XP is HOST-
authoritative - it lands on the authoritative host Soldier and rides the
host->replica roster stream, EQUAL on both machines. Replicas never award it.

HISTORICAL BUG (GAP-7, the pre-DF01 J08 initiator model): the CLIENT simulated
its own craft's dogfight locally and ran awardExperienceToPilots() against ITS
OWN (replica/guest) Soldier copy. So dogfight XP accrued replica-locally and
diverged from - or never reached - the authoritative host Soldier. Two players
watching "the same" fight ended up with different pilot stats.

THE FIX (DF01): the HOST simulates every JOINT dogfight; awardExperienceToPilots()
runs ONLY host-side, on the authoritative Soldier. A replica's DogfightState::
update() early-returns before any sim/award code, so a replica NEVER awards. The
awarded stats (currentStats + the daily dogfight-XP cache, both serialized by
Soldier::save) ride the existing host->replica world stream, so every player sees
the SAME pilot XP on the SAME host Soldier.

This test asserts the FIXED behavior:
  * a dogfight is visible on BOTH machines with a pilot aboard the craft;
  * award_dogfight_xp is HOST-ONLY (a replica is refused - it must never award);
  * the host awards dogfight XP; before the world streams, the replica's copy is
    UNCHANGED (it did not award locally);
  * after the host streams the authoritative world, the replica's pilot XP EQUALS
    the host's, on the SAME soldier id - it rode the roster path;
  * the run ends world-equal + replica zero-disk.

Harness note: the vanilla test ruleset defines no craft `pilots` and no
`dogfightExperience`, so the real awardExperienceToPilots() RNG path awards
nothing. award_dogfight_xp applies the SAME authoritative mutation host-side
(DogfightState::harnessAwardPilotXp) so the propagation invariant - which is the
whole of GAP-7 - is testable without a modded ruleset.

Run:  python tools/coop_test/test_joint_dogfight_xp.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo

FIRING_DELTA, REACT_DELTA, BRAVE_DELTA = 4, 2, 10


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _roster(gc):
    out = []
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        out.extend(b["soldiers"])
    return out


def _sol(gc, sid):
    for s in _roster(gc):
        if s["id"] == sid:
            return s
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
    js = joint_fixture.bring_up("jdfxp", (48794, 48795, 48094))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]

        # Armed craft on BOTH machines (ids lock-step, the test_joint_craft precedent).
        sc_h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                        "weapon": "STR_CANNON_UC"})
        sc_c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                          "weapon": "STR_CANNON_UC"})
        assert sc_h["craft_id"] == sc_c["craft_id"], "avenger ids diverged"
        avenger_id = sc_h["craft_id"]

        # Seat a pilot aboard the craft on BOTH machines (the spawn_craft idiom: a
        # direct, deterministic local assignment applied identically on each, keeping
        # the shared world equal). One shared roster, so the soldier id matches.
        roster = sorted(s["id"] for s in _roster(host))
        assert roster == sorted(s["id"] for s in _roster(client)), "rosters differ"
        pilot_id = roster[0]
        for gc in (host, client):
            gc.ok({"cmd": "assign_crew", "craft_id": avenger_id,
                   "craft_type": "STR_AVENGER", "soldier_id": pilot_id})

        def _aboard(gc):
            s = _sol(gc, pilot_id)
            return s is not None and s["craftId"] == avenger_id

        host.wait_for("host: pilot aboard avenger",
                      lambda: _aboard(host) or None, timeout=30, interval=0.5)
        client.wait_for("client: pilot aboard avenger",
                        lambda: _aboard(client) or None, timeout=30, interval=0.5)
        # baseline dogfight XP - one shared world, so equal on both.
        base_fire = _sol(host, pilot_id)["firing"]
        base_xp = _sol(host, pilot_id)["dogfightXp"]
        assert _sol(client, pilot_id)["firing"] == base_fire, "baseline firing differs"
        assert _sol(client, pilot_id)["dogfightXp"] == base_xp, "baseline dogfightXp differs"
        print(f"PASS setup: pilot {pilot_id} aboard avenger {avenger_id} on both; "
              f"baseline firing={base_fire} dogfightXp={base_xp}")

        # Open a dogfight visible on both machines. A full-hull scout near the base:
        # a non-HK intercept opens in STANDOFF and persists, so the fight stays live
        # while we exercise the award path.
        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH",
                       "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID",
                       "trajectory": "P0", "state": "flying", "speed": 1,
                       "lon": blon + 0.03, "lat": blat})
        ufo_id = ufo["ufo_id"]

        deadline = time.time() + 45
        while time.time() < deadline and _ufo(client, ufo_id) is None:
            _pump(host, client, 1)
            time.sleep(0.2)
        assert _ufo(client, ufo_id) is not None, "client never materialised the UFO"
        client.ok({"cmd": "craft_order", "order": "target",
                   "craft_id": avenger_id, "craft_type": "STR_AVENGER",
                   "ufo_id": ufo_id})

        deadline = time.time() + 120
        while time.time() < deadline:
            _pump(host, client, 1)
            if _df_for(host, avenger_id, ufo_id) and _df_for(client, avenger_id, ufo_id):
                break
            time.sleep(0.2)
        hd = _df_for(host, avenger_id, ufo_id)
        cd = _df_for(client, avenger_id, ufo_id)
        assert hd and cd, "the dogfight did not open on BOTH machines"
        assert _aboard(host) and _aboard(client), "pilot not aboard once the fight opened"
        print("PASS dogfight visible on both machines with the pilot aboard")

        # A replica must NEVER award XP: the host-only hook refuses a replica.
        r = client.cmd({"cmd": "award_dogfight_xp", "craft_id": avenger_id,
                        "ufo_id": ufo_id, "firing": FIRING_DELTA})
        assert not r.get("ok") and "host-only" in r.get("error", ""), (
            f"PRD-DF03 GAP-7 FAILED: a replica was allowed to award dogfight XP: {r}")
        print("PASS replica refused award_dogfight_xp (replicas never award XP)")

        # HOST awards dogfight XP to the fight's crew (the authoritative Soldier).
        aw = host.ok({"cmd": "award_dogfight_xp", "craft_id": avenger_id,
                      "ufo_id": ufo_id, "firing": FIRING_DELTA,
                      "reactions": REACT_DELTA, "bravery": BRAVE_DELTA})
        assert pilot_id in aw.get("pilots", []), \
            f"host award did not touch pilot {pilot_id}: {aw}"
        assert _sol(host, pilot_id)["firing"] == base_fire + FIRING_DELTA, \
            "host pilot firing did not increase"
        # BEFORE the world streams, the replica is UNCHANGED - it did not award locally
        # (a stat-only change does not trip the count checksum, so nothing auto-syncs).
        assert _sol(client, pilot_id)["firing"] == base_fire, (
            "PRD-DF03 GAP-7 FAILED: the replica's pilot firing changed with no host "
            "world stream - a replica must never award XP locally "
            f"(client firing={_sol(client, pilot_id)['firing']}, baseline={base_fire})")
        print("PASS host awarded XP; replica UNCHANGED before the world stream "
              "(no replica-local award)")

        # Host streams the authoritative world; the replica adopts the awarded stats.
        host.ok({"cmd": "force_resync"})
        want_fire = base_fire + FIRING_DELTA
        want_xp = base_xp + FIRING_DELTA
        deadline = time.time() + 45
        while time.time() < deadline:
            _pump(host, client, 1)
            cs = _sol(client, pilot_id)
            if cs and cs["firing"] == want_fire and cs["dogfightXp"] == want_xp:
                break
            time.sleep(0.3)
        cs = _sol(client, pilot_id)
        hs = _sol(host, pilot_id)
        assert cs["firing"] == hs["firing"] == want_fire, (
            "PRD-DF03 GAP-7 FAILED: pilot firing did not converge host==client after "
            f"the roster stream (host={hs['firing']} client={cs['firing']} want={want_fire})")
        assert cs["dogfightXp"] == hs["dogfightXp"] == want_xp, (
            "PRD-DF03 GAP-7 FAILED: dogfight-XP cache did not converge "
            f"(host={hs['dogfightXp']} client={cs['dogfightXp']} want={want_xp})")
        assert cs["reactions"] == hs["reactions"] and cs["bravery"] == hs["bravery"], \
            "pilot reactions/bravery diverged after the stream"
        print(f"PASS GAP-7 CLOSED: pilot {pilot_id} dogfight XP is EQUAL on both "
              f"(firing={want_fire}, dogfightXp={want_xp}) on the authoritative host "
              "Soldier - it rode the host->replica roster stream, not a local award")

        # Resolve the fight deterministically (down the scout), then the shared
        # final-state assertions. (force_resync briefly re-reconciles the replica
        # window, so we drive the host to a clean end rather than a client command.)
        probe = host.ok({"cmd": "set_ufo_damage", "ufo_id": ufo_id, "damage": 0})
        host.ok({"cmd": "set_ufo_damage", "ufo_id": ufo_id,
                 "damage": probe["damageMax"] // 2})
        for _ in range(60):
            host.cmd({"cmd": "dogfight_action", "action": "aggressive",
                      "craft_id": avenger_id, "ufo_id": ufo_id})
            _pump(host, client, 1)
            if _df_for(host, avenger_id, ufo_id) is None and _df_for(client, avenger_id, ufo_id) is None:
                break
            time.sleep(0.15)

        js.finish()
        print("ALL JOINT DOGFIGHT XP (GAP-7) TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
