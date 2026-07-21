"""Bug: geoscape alerts don't reach the CLIENT in a JOINT campaign (reported for "UFO
lost", but it was systemic - ~30 informational popups were host-only).

In JOINT only the host runs the geoscape sim (every timeXxx handler early-returns for a
replica), so ANY popup the sim raises is invisible to clients unless it is explicitly
replicated. There was no general mechanism - each alert had to be hand-brokered, so they
kept going missing one at a time.

Fix: ONE generic `alert` joint_cmd. The host names the dialog class plus the ids / rule
names needed to rebuild it (JointEcon::hostAlert), and a replica-side factory
(alertApply) reconstructs and pops the REAL dialog. All 25 host-sim alert call sites in
GeoscapeState now emit it.

This drives that lane per dialog class and asserts the client actually pops each one.

Run:  python tools/coop_test/test_joint_alerts.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo


def _top(gc):
    return geo.top_state(gc)


def _wait_popup(gc, name, timeout=20):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = _top(gc)
        if name in last:
            return last
        time.sleep(0.3)
    raise AssertionError(f"{gc.name}: {name} never appeared (top={last!r})")


def _clear(host, client):
    for gc in (host, client):
        geo.drain_popups(gc)


def _check(host, client, cls, payload, label):
    """Host raises the alert through the real hostAlert lane; the CLIENT must pop it."""
    _clear(host, client)
    req = {"cmd": "joint_alert", "cls": cls}
    req.update(payload)
    host.ok(req)
    _wait_popup(client, cls)
    print(f"PASS {label}: '{cls}' replicated to the client")
    _clear(host, client)


def main():
    js = joint_fixture.bring_up("jalert", (48904, 48905, 48204))
    host, client = js.host, js.client
    try:
        # A craft id for the craft-scoped alerts.
        base = None
        for b in host.ok({"cmd": "geo_state"})["bases"]:
            if not b.get("coopBase") and not b.get("coopIcon"):
                base = b
                break
        craft_id = base["crafts"][0]["id"]
        soldiers = [s["id"] for s in host.ok({"cmd": "get_soldiers"})["bases"][0]["soldiers"]]

        # The one the user reported.
        _check(host, client, "UfoLostState", {"msg": "STR_UFO_1"}, "UFO lost")

        # Text-only craft/economy warnings (the CraftErrorState family covers UFO landed,
        # hunter-killer alert, refuel/rearm shortages, economy warning).
        _check(host, client, "CraftErrorState",
               {"msg": "STR_NOT_ENOUGH_ITEM_TO_REARM_CRAFT"}, "craft error")

        _check(host, client, "DogfightErrorState",
               {"msg": "STR_UNABLE_TO_ENGAGE_AIRBORNE", "craft_id": craft_id},
               "dogfight error")

        _check(host, client, "LowFuelState", {"craft_id": craft_id}, "low fuel")

        _check(host, client, "ItemsArrivingState", {}, "items arriving")

        # New-possibility family (research/manufacture/purchase/craft/facility).
        _check(host, client, "NewPossibleResearchState",
               {"names": ["STR_LASER_WEAPONS"]}, "new possible research")
        _check(host, client, "NewPossibleManufactureState",
               {"names": ["STR_LASER_PISTOL"]}, "new possible manufacture")

        # Soldier training finished.
        _check(host, client, "TrainingFinishedState",
               {"ids": soldiers[:2], "flag": False}, "training finished")

        js.finish()
        print("ALL JOINT ALERT-REPLICATION TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
