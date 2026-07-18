"""PRD-DF01 (shared/replicated co-op dogfights): an alien Hunter-Killer (HK)
attack on a CLIENT-commanded craft is now SPECTATED by both machines.

SUPERSEDES the GAP-2 host-only regression. GAP-2 investigation
(.agents/prds/joint/session-notes-gap2.md) concluded that "who flies the HK
dogfight" was undefined because craft control in JOINT is last-command-wins with
no per-craft owner. The owner resolved it by building shared/replicated dogfights:
the HOST simulates EVERY dogfight and EVERY player opens a render-only DogfightState
that renders a per-tick host state stream (df_state), with membership carried by
df_open. GAP-2 dissolves - there is no exclusive "flyer" to route to.

So the assertion that was RED in the GAP-2 repro (host_df=1, client_df=0) and then
PINNED as "host-flies interim" now FLIPS: after an HK attack BOTH machines open the
SAME fight (host_df>=1 AND client_df>=1), render the same craft/UFO/currentDist, and
resolve to the SAME authoritative outcome (the host is the sole sim; the client
mirrors it via df_state + the geo position snapshot).

Sequence:
  * spawn an armed Avenger on BOTH machines (ids lock-step, the test_joint_craft
    precedent);
  * the CLIENT commands it to a far waypoint -> craft OUT, last-command seat = the
    client's seat (>0) - the exact GAP-2 setup that used to route host-only;
  * seed an HK UFO on the host at the craft's position and make it HUNT the craft
    (set_ufo_hunt: setHunterKiller + setTargetedXcomCraft);
  * advance and observe: the host opens its OWN DogfightState (Lane 1, unchanged),
    df_open/df_state replicate it, and the client opens a render-only replica.

Assertions (PRD-DF01):
  1. BOTH machines have the fight open (host_df>=1 AND client_df>=1) on the same
     (craftId, ufoId) - inverting the GAP-2 host-only regression;
  2. the client's rendered currentDist tracks the host's (within tolerance) - it is
     rendering the live df_state stream, not simulating;
  3. the fight resolves to ONE authoritative outcome: after the host's dogfight
     ends, host and client agree on the UFO's status + crashId (the host mutates its
     own world; the client mirrors via the geo snapshot). A deterministic CRASH +
     world-equality is exercised by test_joint_intercept_spectate.py (non-HK).

Run:  python tools/coop_test/test_joint_hk_dogfight.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo


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
    for u in _geo(gc).get("ufos", []):
        if u["id"] == ufo_id:
            return u
    return None


def _dogfights(gc):
    d = gc.ok({"cmd": "dogfight_state"})
    return d["count"] + d["pending"], d


def _df_for(gc, craft_id, ufo_id):
    """The started (non-pending) dogfight for this (craft, ufo) pair, or None."""
    d = gc.ok({"cmd": "dogfight_state"})
    for df in d.get("dogfights", []):
        if df["craftId"] == craft_id and df["ufoId"] == ufo_id:
            return df
    return None


def main():
    js = joint_fixture.bring_up("jhk", (48764, 48765, 48064))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]

        # An armed craft on BOTH machines (deterministic, ids lock-step).
        sc_h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                        "weapon": "STR_CANNON_UC"})
        sc_c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                          "weapon": "STR_CANNON_UC"})
        assert sc_h["craft_id"] == sc_c["craft_id"], \
            f"avenger ids diverged: {sc_h['craft_id']} vs {sc_c['craft_id']}"
        avenger_id = sc_h["craft_id"]
        print(f"setup: base ({blon:.3f},{blat:.3f}); avenger id {avenger_id}")

        # CLIENT commands the craft to a FAR waypoint -> it flies OUT and the
        # last-command seat becomes the CLIENT's (>0). Exactly the GAP-2 setup.
        client.ok({"cmd": "craft_order", "order": "target",
                   "craft_id": avenger_id, "craft_type": "STR_AVENGER",
                   "lon": blon + 3.0, "lat": blat})

        def _out(gc):
            c = _craft(gc, avenger_id, "STR_AVENGER")
            if not c:
                return "craft missing"
            return True if c["status"] == "STR_OUT" else c["status"]

        _poll_both(host, client, "avenger OUT (client-commanded)", _out, timeout=45)
        print("client commanded the avenger OUT (last-command seat = client)")

        # Seed an HK UFO on the host, right on the craft, and make it HUNT the
        # craft (UFO-initiated attack). spawn_ufo is host-only; the replica
        # materialises it via the position snapshot.
        ch = _craft(host, avenger_id, "STR_AVENGER")
        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH",
                       "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID",
                       "trajectory": "P0", "state": "flying", "speed": 1,
                       "lon": ch["lon"], "lat": ch["lat"]})
        ufo_id = ufo["ufo_id"]
        hk = host.ok({"cmd": "set_ufo_hunt", "ufo_id": ufo_id,
                      "craft_id": avenger_id, "craft_type": "STR_AVENGER"})
        assert hk.get("isHunterKiller") and hk.get("isHunting"), \
            f"HK/hunt not set: {hk}"
        print(f"seeded HK UFO id {ufo_id} hunting the client's avenger")

        # AC1: advance until the HK dogfight is open on BOTH machines for the same
        # (craft, ufo). The host opens its own DogfightState (Lane 1, unchanged);
        # df_open makes the client open a render-only replica of the same fight.
        deadline = time.time() + 120
        both = False
        while time.time() < deadline:
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            hd = _df_for(host, avenger_id, ufo_id)
            cd = _df_for(client, avenger_id, ufo_id)
            hn, _ = _dogfights(host)
            cn, _ = _dogfights(client)
            if hd and cd:
                both = True
                break
            time.sleep(0.2)

        hn, _ = _dogfights(host)
        cn, _ = _dogfights(client)
        print(f"observed after HK attack: host_df={hn} client_df={cn}")
        assert both, (
            "PRD-DF01 FAILED: the HK dogfight did not open on BOTH machines for "
            f"the same (craft {avenger_id}, ufo {ufo_id}) within 120s "
            f"(host_df={hn}, client_df={cn}). Expected host_df>=1 AND client_df>=1 "
            "(the GAP-2 host-only regression is inverted).")
        assert hn >= 1 and cn >= 1, f"host_df={hn} client_df={cn}, both must be >=1"
        print(f"PASS AC1: BOTH machines opened the same HK fight "
              f"(host_df={hn}, client_df={cn}) - GAP-2 inverted")

        # AC2: the client's rendered distance tracks the host's (it renders df_state,
        # it does not simulate). Sample a few ticks; require convergence within
        # tolerance at least once (conflation = the client is at most a few ticks
        # behind the host).
        TOL = 160
        converged = False
        best = None
        for _ in range(30):
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            hd = _df_for(host, avenger_id, ufo_id)
            cd = _df_for(client, avenger_id, ufo_id)
            if hd and cd:
                delta = abs(hd["dist"] - cd["dist"])
                best = (hd["dist"], cd["dist"], delta) if best is None or delta < best[2] else best
                if delta <= TOL:
                    converged = True
                    break
            else:
                break  # one side resolved; stop sampling distance
            time.sleep(0.2)
        if best is not None:
            print(f"currentDist host={best[0]} client={best[1]} (min |delta|={best[2]})")
            assert converged, (
                f"PRD-DF01 FAILED: client currentDist never tracked the host within "
                f"{TOL} (best delta {best[2]}) - the client is not rendering df_state")
            print("PASS AC2: client currentDist tracks the host (rendering df_state)")

        # AC3: let the host's authoritative sim resolve the fight, then confirm host
        # and client agree on the UFO outcome (status + crashId). The host mutates
        # its own world; the client mirrors it via the geo position snapshot.
        deadline = time.time() + 120
        while time.time() < deadline:
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            hn, _ = _dogfights(host)
            if hn == 0:
                break
            time.sleep(0.2)
        # settle the snapshot to the replica
        geo.skip_realtime(host, client, 2, speed_idx=0, stuck_timeout=None)

        uh = _ufo(host, ufo_id)
        uc = _ufo(client, ufo_id)
        assert uh is not None, "host lost the UFO from geo_state"
        # If the UFO despawned entirely on the client it is a mirror failure only if
        # the host still has it flying; a downed UFO stays as a crash site on both.
        assert uc is not None, "client lost the UFO from geo_state (mirror failure)"
        print(f"outcome: host ufo status={uh['status']} crashId={uh['crashId']}; "
              f"client status={uc['status']} crashId={uc['crashId']}")
        assert uh["status"] == uc["status"], (
            f"PRD-DF01 FAILED: UFO status diverged host={uh['status']} "
            f"client={uc['status']} - the client did not mirror the host outcome")
        assert uh["crashId"] == uc["crashId"], (
            f"PRD-DF01 FAILED: UFO crashId diverged host={uh['crashId']} "
            f"client={uc['crashId']}")
        print("PASS AC3: host and client agree on the UFO outcome "
              "(status + crashId equal) - single authoritative sim, mirrored")

        js.finish()
        print("ALL JOINT HK SPECTATE TESTS PASSED")
    finally:
        js.shutdown()


def _poll_both(host, client, label, pred, timeout=60, speed_idx=0):
    deadline = time.time() + timeout
    last = ("", "")
    while time.time() < deadline:
        geo.skip_realtime(host, client, 1, speed_idx=speed_idx, stuck_timeout=None)
        h, c = pred(host), pred(client)
        last = (h, c)
        if h is True and c is True:
            return
        time.sleep(0.2)
    raise AssertionError(f"{label}: not converged after {timeout}s "
                         f"(host={last[0]!r} client={last[1]!r})")


if __name__ == "__main__":
    main()
