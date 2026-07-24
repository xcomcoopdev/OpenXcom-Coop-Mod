"""Issue #78 (SHARED geoscape): "When the client moved the Skyranger, some
extra waypoints were left behind on the Geoscape."

Root cause: craft_retarget{targetType:point} -> SharedEcon::craftOrderApply
creates the Waypoint on host AND client (deliberately, to keep the id counter
in lock-step), but only the HOST ever sweeps followerless waypoints
(GeoscapeState::time5Seconds deleteIf; a replica's time5Seconds early-returns).
The only replica-side cleanup is patrol_prompt on ARRIVAL - so a craft
retargeted (or recalled) BEFORE reaching its waypoint orphans the old marker
on the client forever. The SHARED snapshot carries crafts/ufos/missions but no
waypoints, so nothing else reconciles them.

Repro: the client sends the Skyranger to distant waypoint A (marker on both),
then retargets to waypoint B before arrival -> the host converges to ONE
marker (B), the client must too. Then recall to base -> both must be clean.

EXPECTED RED until the fix lands (client keeps the orphaned markers).

Run:  python tools/coop_test/test_shared_waypoint_retarget.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import geo


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _skyranger(gc):
    for c in _base0(gc)["crafts"]:
        if "SKYRANGER" in c["type"]:
            return c
    raise AssertionError("no skyranger")


def _waypoint_ids(gc):
    return sorted(w["id"] for w in _geo(gc).get("waypoints", []))


def _wait(pred, label, timeout=30, interval=0.4):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"{label}: never satisfied (last={last})")


def main():
    js = shared_fixture.bring_up("jwpleak", (48984, 48985, 48184))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]
        cid = _skyranger(host)["id"]

        assert not _waypoint_ids(host) and not _waypoint_ids(client), \
            "unexpected pre-existing waypoints"

        # CLIENT sends the shared craft to a DISTANT waypoint A (~far enough
        # that it cannot arrive during this test - arrival has its own cleanup
        # path and its own test, test_shared_waypoint_arrival).
        client.ok({"cmd": "craft_order", "order": "target", "craft_id": cid,
                   "craft_type": "STR_SKYRANGER",
                   "lon": blon + 1.0, "lat": blat + 0.30})
        _wait(lambda: len(_waypoint_ids(host)) == 1
              and len(_waypoint_ids(client)) == 1 or None,
              "waypoint A marker on both machines", timeout=40)
        wp_a = _waypoint_ids(host)[0]
        print(f"PASS setup: client-directed waypoint A (id {wp_a}) on host AND client")

        # Retarget to waypoint B before arrival. Host: craftOrderApply creates
        # B, setDestination drops A's follower, the time5Seconds sweep deletes
        # A -> exactly one marker again. Client must mirror that.
        client.ok({"cmd": "craft_order", "order": "target", "craft_id": cid,
                   "craft_type": "STR_SKYRANGER",
                   "lon": blon - 1.0, "lat": blat - 0.30})
        # Let the shared cmd land and the host sweep run (needs the clock to
        # tick; slow speed so the craft covers no real distance).
        geo.skip_realtime(host, client, 6, speed_idx=1, stuck_timeout=None)
        _wait(lambda: (lambda h: len(h) == 1 and wp_a not in h)(
                  _waypoint_ids(host)) or None,
              "host converged to waypoint B only", timeout=30)
        wp_b = _waypoint_ids(host)[0]
        print(f"PASS: host swept orphaned waypoint A, keeps only B (id {wp_b})")

        # THE BUG (issue #78): the client must also hold exactly B. Today the
        # replica never sweeps, so A leaks and the client shows TWO markers.
        try:
            _wait(lambda: _waypoint_ids(client) == [wp_b] or None,
                  "client converged to waypoint B only", timeout=25)
        except AssertionError:
            raise AssertionError(
                "BUG issue #78: stray waypoint left on the CLIENT after "
                f"retarget - host={_waypoint_ids(host)} "
                f"client={_waypoint_ids(client)} (expected [{wp_b}] on both)")
        print("PASS: client swept orphaned waypoint A, keeps only B")

        # Recall to base: B goes followerless -> both machines must end clean.
        client.ok({"cmd": "craft_order", "order": "return", "craft_id": cid})
        geo.skip_realtime(host, client, 6, speed_idx=1, stuck_timeout=None)
        _wait(lambda: not _waypoint_ids(host) or None,
              "host swept waypoint B after recall", timeout=30)
        try:
            _wait(lambda: not _waypoint_ids(client) or None,
                  "client swept waypoint B after recall", timeout=25)
        except AssertionError:
            raise AssertionError(
                "BUG issue #78: stray waypoint left on the CLIENT after "
                f"recall to base - host={_waypoint_ids(host)} "
                f"client={_waypoint_ids(client)} (expected none on both)")
        print("PASS: recall to base leaves zero waypoints on both machines")

        js.finish()
        print("ALL SHARED WAYPOINT-RETARGET TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
