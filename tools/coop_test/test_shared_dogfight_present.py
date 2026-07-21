"""Window presentation policy: EVERY player opens the fight FULL, and the local
minimize state never gates the command lane.

PRD-DF02 §3b originally gave the commanding seat (lastCraftOrderSeat(craft), carried
per-dogfight in df_open) the full window and every other player a minimized icon. That
policy was DROPPED in 2f8457d21 after the playtest report "dogfight totally broken for
clients": a host-commanded craft opened as a mute icon on every client, which read as
the fight being missing. GeoscapeState::sharedReconcileReplicaDogfights now always
passes startMinimized=false, so both machines open full whoever commanded.

Minimize survives as a per-machine VIEW choice the player makes themselves. It never
drives the world clock (the host's authoritative hostAnyOpen does), and a minimized
replica can still issue df_cmd to the host - which is what this test pins down for the
HOST-commanded case (test_shared_dogfight_control AC5 covers the client-commanded one).

Acceptance:
  * host-commanded intercept -> BOTH windows open FULL (asserted via the
    dogfight_state `minimized` field on each machine);
  * after the client minimizes its own window, it can still command the host: it issues
    aggressive and the host's authoritative _mode changes, proving the command lane is
    independent of the local minimize/presentation state, and the client stays minimized
    (commanding does not force the view back open).

Run:  python tools/coop_test/test_shared_dogfight_present.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import geo

AGGRESSIVE = 3


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


def _pump(host, client, n=1):
    geo.skip_realtime(host, client, n, speed_idx=0, stuck_timeout=None)


def main():
    js = shared_fixture.bring_up("jdfp", (48794, 48795, 48094))
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

        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH",
                       "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID",
                       "trajectory": "P0", "state": "flying", "speed": 1,
                       "lon": blon + 0.03, "lat": blat})
        ufo_id = ufo["ufo_id"]

        # Materialise the UFO on the client, then the HOST commands the intercept ->
        # commanding seat <= 0 (host). The client is NOT the commanding seat, which is
        # exactly the case that used to open minimized.
        deadline = time.time() + 45
        while time.time() < deadline and _ufo(client, ufo_id) is None:
            _pump(host, client, 1)
            time.sleep(0.2)
        assert _ufo(client, ufo_id) is not None, "client never materialised the UFO"
        host.ok({"cmd": "craft_order", "order": "target",
                 "craft_id": avenger_id, "craft_type": "STR_AVENGER",
                 "ufo_id": ufo_id})
        print(f"setup: host commanded the intercept (avenger {avenger_id} vs "
              f"scout {ufo_id})")

        # Wait until BOTH machines have the fight open.
        deadline = time.time() + 120
        while time.time() < deadline:
            _pump(host, client, 1)
            if _df_for(host, avenger_id, ufo_id) and _df_for(client, avenger_id, ufo_id):
                break
            time.sleep(0.2)
        hd = _df_for(host, avenger_id, ufo_id)
        cd = _df_for(client, avenger_id, ufo_id)
        assert hd and cd, "the host-commanded intercept did not open on BOTH machines"

        # Presentation policy: FULL on both, whoever commanded.
        assert not hd["minimized"], "host (sim) must open FULL"
        assert not cd["minimized"], (
            "presentation policy FAILED: a non-commanding client must open FULL for a "
            "host-commanded craft (the PRD-DF02 §3b minimize was dropped in 2f8457d21)")
        print("PASS: presentation policy - BOTH windows FULL "
              "(host commanded; the client is not the commanding seat)")

        # The client minimizes its OWN window - a per-machine view choice.
        client.ok({"cmd": "dogfight_action", "action": "minimize"})
        _pump(host, client, 2)
        cd = _df_for(client, avenger_id, ufo_id)
        hd = _df_for(host, avenger_id, ufo_id)
        assert cd and cd["minimized"], "client view did not minimize"
        assert hd and not hd["minimized"], (
            "a client-local minimize must not minimize the host window (view state is "
            "per-machine)")
        print("PASS: client-local minimize is view-only; the host window stays FULL")

        # A minimized (non-commanding) client can STILL command the host: aggressive.
        client.ok({"cmd": "dogfight_action", "action": "aggressive"})
        ok = False
        for _ in range(40):
            _pump(host, client, 1)
            hd = _df_for(host, avenger_id, ufo_id)
            if hd and hd["mode"] == AGGRESSIVE:
                ok = True
                break
            time.sleep(0.15)
        assert ok, ("PRD-DF02 FAILED: a minimized client could not command the host "
                    "(the df_cmd lane must be independent of local minimize state)")
        # The client stays minimized - commanding does not force it open.
        cd = _df_for(client, avenger_id, ufo_id)
        assert cd is not None and cd["minimized"], \
            "client should still be minimized after commanding (view state unchanged)"
        print("PASS: a minimized client still commands the host (host _mode aggressive); "
              "the command lane is independent of the presentation/minimize state")

        js.finish()
        print("ALL SHARED DOGFIGHT PRESENTATION TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
