"""PRD-DF01 (shared/replicated co-op dogfights): a REGULAR (non-HK) interception
is spectated by both machines and resolves to ONE authoritative outcome.

The client commands its craft to intercept a seeded UFO. The host runs the only
geoscape sim, so the host opens its OWN DogfightState when the craft reaches the
UFO (vanilla Lane 2, after the DF01 rip-out of the J08 initiator broker); df_open
makes the client open a render-only replica, and df_state streams the live fight.
Because the host is the sole sim and the host drives the fight to a shoot-down, the
UFO crashes on the host and the crash (status + crashId) reaches the client via the
geo position snapshot - identical, world-equal.

Sequence:
  * armed Avenger on BOTH machines (ids lock-step);
  * seed a slow STR_MEDIUM_SCOUT on the host near the base, damaged to ~half hull
    (a single cannon hit then crashes it deterministically), materialised on the
    client via the position snapshot;
  * the CLIENT commands the craft to intercept the UFO (craft_retarget joint_cmd);
  * advance: the host opens its DogfightState, the client opens a render-only
    replica; drive the HOST fight aggressive (DF01 replica buttons are inert - DF02
    wires df_cmd) so it fires;
  * the scout crashes on the host; assert the client mirrors the same crash.

Assertions (PRD-DF01):
  1. BOTH machines open the dogfight for the same (craftId, ufoId);
  2. the client's rendered currentDist tracks the host's (it renders df_state);
  3. deterministic outcome: host UFO CRASHED with crashId>0; client mirrors the
     SAME status + crashId; funds equal on both (world-equal).

Run:  python tools/coop_test/test_joint_intercept_spectate.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo

# UFO status ints (Ufo::UfoStatus): FLYING=0, LANDED=1, CRASHED=2, DESTROYED=3.
CRASHED = 2


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


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


def _df_count(gc):
    d = gc.ok({"cmd": "dogfight_state"})
    return d["count"] + d["pending"]


def main():
    js = joint_fixture.bring_up("jint", (48774, 48775, 48074))
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

        # Seed a slow scout very close to the base (coords are RADIANS) so the fast
        # Avenger reaches it in a few geoscape ticks.
        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH",
                       "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID",
                       "trajectory": "P0", "state": "flying", "speed": 1,
                       "lon": blon + 0.03, "lat": blat})
        ufo_id = ufo["ufo_id"]
        # Damage it to ~half hull: still FLYING (crash threshold is strictly >),
        # so the FIRST cannon hit in the dogfight crashes it deterministically.
        probe = host.ok({"cmd": "set_ufo_damage", "ufo_id": ufo_id, "damage": 0})
        dmax = probe["damageMax"]
        host.ok({"cmd": "set_ufo_damage", "ufo_id": ufo_id, "damage": dmax // 2})
        print(f"setup: avenger id {avenger_id}; scout id {ufo_id} at half hull "
              f"(damageMax={dmax})")

        # Wait until the client has materialised the UFO via the position snapshot.
        deadline = time.time() + 45
        while time.time() < deadline:
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            if _ufo(client, ufo_id) is not None:
                break
            time.sleep(0.2)
        assert _ufo(client, ufo_id) is not None, \
            "client never received the seeded UFO via the position snapshot"
        print("client materialised the scout (position snapshot)")

        # CLIENT commands the craft to intercept the UFO (craft_retarget joint_cmd).
        client.ok({"cmd": "craft_order", "order": "target",
                   "craft_id": avenger_id, "craft_type": "STR_AVENGER",
                   "ufo_id": ufo_id})
        print("client commanded the avenger to intercept the scout")

        # AC1: advance until BOTH machines open the dogfight for this (craft, ufo).
        deadline = time.time() + 120
        both = False
        while time.time() < deadline:
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            if _df_for(host, avenger_id, ufo_id) and _df_for(client, avenger_id, ufo_id):
                both = True
                break
            time.sleep(0.2)
        hn, cn = _df_count(host), _df_count(client)
        print(f"dogfight open: host_df={hn} client_df={cn}")
        assert both, ("PRD-DF01 FAILED: the interception did not open on BOTH "
                      f"machines for (craft {avenger_id}, ufo {ufo_id}) within 120s "
                      f"(host_df={hn}, client_df={cn})")
        print("PASS AC1: both machines opened the interception dogfight")

        # Drive the HOST fight aggressive so it fires (DF01 replica buttons are
        # inert; the host is the sim authority). Repeat - the window may still be
        # zooming/starting on the first calls.
        for _ in range(6):
            r = host.cmd({"cmd": "dogfight_action", "action": "aggressive"})
            if r.get("ok"):
                break
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)

        # AC2: the client's rendered distance tracks the host's.
        TOL = 160
        converged = False
        best = None
        for _ in range(40):
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            hd = _df_for(host, avenger_id, ufo_id)
            cd = _df_for(client, avenger_id, ufo_id)
            if hd and cd:
                delta = abs(hd["dist"] - cd["dist"])
                best = (hd["dist"], cd["dist"], delta) if best is None or delta < best[2] else best
                if delta <= TOL:
                    converged = True
                    break
            uh = _ufo(host, ufo_id)
            if uh and uh["status"] == CRASHED:
                break  # already resolved before we sampled convergence
            time.sleep(0.2)
        if best is not None:
            print(f"currentDist host={best[0]} client={best[1]} (min |delta|={best[2]})")
            assert converged, (f"PRD-DF01 FAILED: client currentDist never tracked the "
                               f"host within {TOL} (best {best[2]})")
            print("PASS AC2: client currentDist tracks the host (rendering df_state)")

        # AC3: the host shoots the scout down; the client mirrors the crash.
        deadline = time.time() + 150
        crashed = False
        while time.time() < deadline:
            host.cmd({"cmd": "dogfight_action", "action": "aggressive"})
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            uh = _ufo(host, ufo_id)
            if uh and uh["status"] == CRASHED:
                crashed = True
                break
            time.sleep(0.2)
        assert crashed, ("PRD-DF01 FAILED: the host never shot the scout down within "
                         "150s (aggressive host dogfight vs a half-hull scout)")
        # settle the crash snapshot onto the replica
        geo.skip_realtime(host, client, 3, speed_idx=0, stuck_timeout=None)

        uh = _ufo(host, ufo_id)
        uc = _ufo(client, ufo_id)
        assert uh and uc, "lost the UFO from geo_state after the crash"
        print(f"outcome: host status={uh['status']} crashId={uh['crashId']}; "
              f"client status={uc['status']} crashId={uc['crashId']}")
        assert uh["status"] == CRASHED and uh["crashId"] > 0, \
            f"host did not record a crash site: {uh}"
        assert uc["status"] == uh["status"], (
            f"PRD-DF01 FAILED: client UFO status {uc['status']} != host {uh['status']}")
        assert uc["crashId"] == uh["crashId"], (
            f"PRD-DF01 FAILED: client crashId {uc['crashId']} != host {uh['crashId']}")
        # world-equal: funds identical on both machines.
        fh = _geo(host)["funds"]
        fc = _geo(client)["funds"]
        assert fh == fc, f"PRD-DF01 FAILED: funds diverged host={fh} client={fc}"
        print(f"PASS AC3: identical crash outcome (status={uh['status']} "
              f"crashId={uh['crashId']}) + world-equal (funds={fh})")

        js.finish()
        print("ALL JOINT INTERCEPT SPECTATE TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
