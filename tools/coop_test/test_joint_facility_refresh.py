"""Playtest B1: an open "build facilities" popup must refresh the base screen
behind it when the OTHER player's facility build lands.

The BuildFacilitiesState list is a small popup; the BasescapeState it covers
(funds header + facility grid) is visible around it. A covered screen never gets
think(), so before the fix a peer's fac_build joint_apply left the funds/grid
behind the popup stale until it was closed (BasescapeState::init runs on pop).
This is exactly the playtester's report: "they have to exit the build facilities
menu before they see the updated construction and the deducted funds."

The assertion reads the BasescapeState funds header CACHE (screen_state digs it
out from under the popup) - it only changes when that screen is rebuilt, so it
proves the covered screen refreshed live rather than the world merely being
correct underneath.

  OPEN   client opens BasescapeState + BuildFacilitiesState on top; record the
         funds header shown behind the popup.
  APPLY  host builds a facility (funds debited on the shared world, joint_apply).
  LIVE   the client's covered funds header updates WITHOUT closing the popup,
         and the popup is still a BuildFacilitiesState (rebuilt, not crashed).

Run:  python tools/coop_test/test_joint_facility_refresh.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo

FACILITY = "STR_LIVING_QUARTERS"
BASE_SIZE = 6


def _base0(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _funds(gc):
    return gc.ok({"cmd": "geo_state"})["funds"]


def _occupancy(base):
    occupied, built = set(), set()
    for f in base["facilities"]:
        for dx in range(f.get("sizeX", 1)):
            for dy in range(f.get("sizeY", 1)):
                t = (f["x"] + dx, f["y"] + dy)
                occupied.add(t)
                if f["buildTime"] == 0:
                    built.add(t)
    return occupied, built


def _free_tile_next_to_built(base):
    occupied, built = _occupancy(base)
    for y in range(BASE_SIZE):
        for x in range(BASE_SIZE):
            if (x, y) in occupied:
                continue
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if (nx, ny) in built:
                    return (x, y)
    raise AssertionError("no buildable tile")


def _digits(text):
    return "".join(c for c in text if c.isdigit())


def _screen(gc):
    return gc.ok({"cmd": "screen_state"})


def main():
    js = joint_fixture.bring_up("jfacref", (48760, 48761, 48060))
    host, client = js.host, js.client
    try:
        (tx, ty) = _free_tile_next_to_built(_base0(host))
        f0 = _funds(host)
        assert f0 == _funds(client), "starting funds differ"
        print(f"PASS setup: buildable tile ({tx},{ty}); funds {f0}")

        # ---- OPEN: client sits in the build-facilities popup ----------------
        r = client.ok({"cmd": "open_screen", "screen": "build_facilities"})
        assert r.get("ok"), f"client could not open build_facilities: {r}"
        s0 = _screen(client)
        assert s0["top"] == "build_facilities", \
            f"client top state is {s0['top']}, want build_facilities"
        funds_text0 = s0["funds"]
        assert _digits(funds_text0) == str(f0), \
            f"popup's base funds header '{funds_text0}' != starting funds {f0}"
        print(f"PASS open: client in BuildFacilitiesState; base header shows '{funds_text0}'")

        # ---- APPLY: host builds a facility (debits the shared world) ---------
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        r = host.ok({"cmd": "fac_build", "facility": FACILITY, "x": tx, "y": ty})
        assert r.get("ok"), f"host fac_build not accepted: {r}"

        # world settles first (proves the apply really landed under the popup)
        client.wait_for("client's shared world debited by the host build",
                        lambda: (_funds(client) < f0) or None,
                        timeout=30, interval=0.5)
        f1 = _funds(client)
        assert f1 == _funds(host), f"funds diverged: client={f1} host={_funds(host)}"
        assert f1 < f0, f"build did not debit funds: {f0} -> {f1}"

        # ---- LIVE: the covered base header must have rebuilt to the new funds -
        def _refreshed():
            s = _screen(client)
            if s["top"] != "build_facilities":
                return None
            if s["funds"] == funds_text0:
                return None
            return s

        client.wait_for("covered BasescapeState funds header refreshed under the popup",
                        _refreshed, timeout=30, interval=0.5)
        s1 = _screen(client)
        assert s1["top"] == "build_facilities", \
            f"popup gone/crashed after apply: top={s1['top']}"
        assert s1["funds"] != funds_text0, \
            "funds header behind the popup STILL showing the pre-build value (B1)"
        assert _digits(s1["funds"]) == str(f1), \
            f"refreshed header '{s1['funds']}' does not reflect new funds {f1}"
        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive after refresh"
        print(f"PASS live: header '{funds_text0}' -> '{s1['funds']}' "
              f"(funds {f0} -> {f1}) with the popup still open, no crash")

        client.ok({"cmd": "close_screens"})
        print("ALL JOINT FACILITY-REFRESH TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
