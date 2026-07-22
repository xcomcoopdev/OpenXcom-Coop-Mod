"""Playtest bugs (SHARED geoscape craft arrival):

  A. The "craft reached destination" alert (CraftPatrolState) is not shown when a craft
     reaches a patrol WAYPOINT - not on the client, not even on the host / the seat that
     directed the craft. In SHARED only the host runs the sim; the waypoint-arrival popup
     was never brokered to the other seats (unlike the LANDING prompt, which is).
  B. The waypoint MARKER (and destination line) is never removed on the CLIENT after the
     craft arrives: the client's frozen sim never cleared the craft's _dest nor pruned the
     orphan waypoint, so the flag renders forever.

Fix: on waypoint arrival the host broadcasts patrol_prompt; each replica pops the SAME
alert and clears the stale destination + orphan waypoint (GeoscapeState::clientCraft-
ReachedWaypoint), exactly as the host's time5Seconds does.

The CLIENT directs the shared craft to a waypoint right next to the base, both machines
render the marker, we fly it in, and assert: the alert pops on BOTH machines, and after
it closes the waypoint marker + destination are gone on BOTH.

Run:  python tools/coop_test/test_shared_waypoint_arrival.py
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


def _craft(gc, cid):
    for c in _base0(gc)["crafts"]:
        if c["id"] == cid:
            return c
    return None


def _waypoints(gc):
    return _geo(gc).get("waypoints", [])


def _top(gc):
    return geo.top_state(gc)


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
    js = shared_fixture.bring_up("jwaypt", (48808, 48809, 48108))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]
        cid = _skyranger(host)["id"]

        # No waypoints at rest on either machine.
        assert not _waypoints(host) and not _waypoints(client), \
            "unexpected pre-existing waypoints"

        # CLIENT directs the shared craft to a WAYPOINT just next to the base (so it
        # arrives quickly). In SHARED this rides craft_retarget{targetType:point} ->
        # craftOrderApply creates the shared waypoint on host AND client.
        wlon, wlat = blon + 0.02, blat + 0.01
        client.ok({"cmd": "craft_order", "order": "target", "craft_id": cid,
                   "craft_type": "STR_SKYRANGER", "lon": wlon, "lat": wlat})

        # Marker appears on BOTH machines, and the craft is heading to it (destKind
        # "other" = a Waypoint target). This is the pre-arrival state.
        def _both_have_marker():
            hw, cw = _waypoints(host), _waypoints(client)
            hc, cc = _craft(host, cid), _craft(client, cid)
            return (len(hw) == 1 and len(cw) == 1
                    and hc and hc["destKind"] == "other"
                    and cc and cc["destKind"] == "other") or None
        _wait(_both_have_marker, "waypoint marker created on both machines", timeout=40)
        print("PASS setup: client-directed waypoint marker present on host AND client")

        # Fly it in. Stop the moment the "reached destination" alert pops (on either
        # side); skip_realtime leaves the triggering popup open.
        res = geo.skip_realtime(host, client, 120, speed_idx=3,
                                interest=geo.popup("CraftPatrolState"),
                                stuck_timeout=None)
        assert res["hit"], f"craft never reached the waypoint (no CraftPatrolState): {res}"

        # BUG A: the alert must appear on BOTH machines (host brokers patrol_prompt to
        # every seat). Poll both, pumping, since the client's copy lags the broadcast.
        def _both_show_alert():
            return ("CraftPatrolState" in _top(host)
                    and "CraftPatrolState" in _top(client)) or None
        try:
            _wait(_both_show_alert, "CraftPatrolState on BOTH machines", timeout=25)
        except AssertionError:
            raise AssertionError(
                "BUG A: 'reached destination' alert not shown on both - "
                f"host_top={_top(host)!r} client_top={_top(client)!r}")
        print("PASS bug A: 'reached destination' alert shown on BOTH host and client")

        # Close the alert (btnOk = keep patrolling) on both.
        for gc in (host, client):
            if "CraftPatrolState" in _top(gc):
                gc.cmd({"cmd": "dismiss_popup"})
        # Some builds route CraftPatrolState close through a generic pop; ensure geoscape.
        for gc in (host, client):
            geo.drain_popups(gc)

        # BUG B: after arrival the waypoint marker + destination line must be GONE on
        # BOTH machines (host sweep + client patrolPromptApply prune).
        def _both_cleared():
            hw, cw = _waypoints(host), _waypoints(client)
            hc, cc = _craft(host, cid), _craft(client, cid)
            return (not hw and not cw
                    and hc and hc["destKind"] == "none"
                    and cc and cc["destKind"] == "none") or None
        try:
            _wait(_both_cleared, "waypoint cleared on both machines", timeout=25)
        except AssertionError:
            raise AssertionError(
                "BUG B: waypoint marker/destination not cleared - "
                f"host_wp={_waypoints(host)} client_wp={_waypoints(client)} "
                f"host_dest={_craft(host, cid)['destKind']} "
                f"client_dest={_craft(client, cid)['destKind']}")
        print("PASS bug B: waypoint marker + destination removed on BOTH machines")

        js.finish()
        print("ALL SHARED WAYPOINT-ARRIVAL TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
