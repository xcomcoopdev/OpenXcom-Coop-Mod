"""PRD-DF02 §3b (window presentation policy): the commanding seat gets the FULL
window; every other player gets a MINIMIZED icon.

The commanding seat = lastCraftOrderSeat(craft), carried per-dogfight in df_open. When
the HOST commands the craft (seat <= 0), the host is the commander, so ALL clients open
the fight minimized (an icon they can click to spectate/command). The host always opens
full (it is the sim / vanilla path). The initial minimize is a per-machine VIEW choice
only - it never drives the world clock (the host's authoritative hostAnyOpen does), and
a minimized replica can still issue df_cmd to the host.

Acceptance:
  * host-commanded intercept -> HOST window FULL, CLIENT window MINIMIZED (asserted via
    the dogfight_state `minimized` field on each machine);
  * a minimized (non-commanding) client can still command the host: it issues
    aggressive and the host's authoritative _mode changes, proving the command lane is
    independent of the local minimize/presentation state.

Run:  python tools/coop_test/test_joint_dogfight_present.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
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
    js = joint_fixture.bring_up("jdfp", (48794, 48795, 48094))
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
        # commanding seat <= 0 (host) -> every client opens minimized.
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

        # Presentation policy: host FULL, client MINIMIZED.
        assert not hd["minimized"], "host (sim) must open FULL"
        assert cd["minimized"], (
            "PRD-DF02 §3b FAILED: a non-commanding client must open MINIMIZED for a "
            "host-commanded craft")
        print("PASS: presentation policy - host FULL, client MINIMIZED "
              "(host is the commanding seat)")

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
        print("ALL JOINT DOGFIGHT PRESENTATION TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
