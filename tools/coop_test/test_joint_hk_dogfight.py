"""GAP-2 (intended-behaviour regression): alien Hunter-Killer (HK) attack on a
CLIENT-commanded craft is flown by the HOST.

GAP-2 investigation (see .agents/prds/joint/session-notes-gap2.md) concluded this
is NOT a bug: craft control in JOINT is purely last-command-wins with no per-craft
owner field, and an HK attack is UFO-initiated (no initiator seat to key on), so
"the host flies HK defence" is the direct reading of locked decision #5. The host
is the only machine that runs geoscape sim in JOINT (the replica returns early in
time5Seconds), and the HK-attack lane (UFO loop `ufo->reachedDestination() &&
ufo->isHunting()` ~2353) deliberately has no seat routing.

This test PINS that intended interim behaviour so a future change is visible.

NOTE - scheduled to be SUPERSEDED: the owner chose to build shared/replicated
co-op dogfights (host sims every dogfight; ALL players render it and can issue
commands; host arbitrates in receive-order) as a phased feature AFTER the gap
hardening pass. When that lands, "who flies" dissolves entirely and this
assertion flips to "BOTH machines have the dogfight open". Until then, host-flies
is correct and asserted here. The set_ufo_hunt harness hook + this scaffolding are
the ready-made seed for that feature's acceptance test.

Sequence:
  * spawn an armed Avenger on BOTH machines (ids lock-step, the test_joint_craft
    precedent);
  * the CLIENT commands it to a far waypoint  ->  craft OUT, last-command seat =
    the client's seat (>0), i.e. "the client is flying this craft";
  * seed an HK UFO on the host at the craft's position and make it HUNT the
    craft (set_ufo_hunt: setHunterKiller + setTargetedXcomCraft);
  * advance and observe which machine opens the dogfight.

Assertion (intended interim behaviour): the HOST opens the HK dogfight and the
client does not - host is authoritative for the sim, and there is no per-craft
owner to route to.

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


def _dogfights(gc):
    d = gc.ok({"cmd": "dogfight_state"})
    return d["count"] + d["pending"], d


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
        # last-command seat becomes the CLIENT's (>0). This is the JOINT sense
        # of "the client is flying this craft"; it is NOT chasing the UFO.
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
        # craft (UFO-initiated attack).
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

        # Advance until an HK dogfight materialises on EITHER machine.
        deadline = time.time() + 90
        host_df = client_df = 0
        while time.time() < deadline:
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            host_df, dh = _dogfights(host)
            client_df, dc = _dogfights(client)
            if host_df or client_df:
                break
            time.sleep(0.2)

        print(f"observed after HK attack: host_df={host_df} client_df={client_df}")
        assert host_df or client_df, \
            "no HK dogfight opened on EITHER machine within 90s (engagement dropped?)"

        # Intended interim behaviour (GAP-2 = design, not bug): the HOST is
        # authoritative for the geoscape sim and there is no per-craft owner to
        # route to, so the host opens the HK dogfight and the client does not.
        # (Superseded when shared/replicated dogfights land - see module docstring.)
        assert host_df >= 1 and client_df == 0, (
            f"expected the HOST to fly the HK dogfight (host_df>=1, client_df==0), "
            f"got host_df={host_df} client_df={client_df}. If shared/replicated "
            "dogfights have landed, this assertion must flip to 'both machines "
            "have the dogfight open' - see the module docstring.")

        print("PASS: host flew the HK dogfight (intended interim behaviour); "
              "client opened nothing")
        js.finish()
        print("ALL JOINT HK TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
