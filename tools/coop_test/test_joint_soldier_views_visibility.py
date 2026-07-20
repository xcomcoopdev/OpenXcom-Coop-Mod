"""Playtest (P2): every soldier LIST view shows only YOUR soldiers.

Covers the views with a harness `displayed` accessor: the base roster
(SoldiersState), the craft-crew assignment (CraftSoldiersState), and the crew-armor
screen (CraftArmorState). Each must show only the local player's own half of the
shared roster, on both machines.

(Sell/Transfer/Memorial/Pilots/Transform use the identical one-line build-loop
`&& JointEcon::ownsSoldier(...)` filter validated by the same pattern.)

Run:  python tools/coop_test/test_joint_soldier_views_visibility.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session


def _roster(gc):
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        if not b.get("coopBaseFlag") and not b.get("coopIcon"):
            return b["soldiers"]
    raise AssertionError("no real base")


def _craft_id(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b["crafts"][0]["id"]
    raise AssertionError("no craft")


def _displayed(gc, screen, top, **kw):
    r = gc.ok({"cmd": "open_screen", "screen": screen, **kw})
    assert r.get("ok"), f"open {screen}: {r}"
    gc.wait_for(f"{screen} up",
                lambda: (gc.cmd({"cmd": "screen_state"}).get("top") == top) or None,
                timeout=15, interval=0.3)
    ids = gc.cmd({"cmd": "screen_state"}).get("displayed", [])
    gc.ok({"cmd": "close_screens"})
    return sorted(ids)


def main():
    js = joint_fixture.bring_up("jviews", (48880, 48881, 48180))
    host, client = js.host, js.client
    try:
        owner = {s["id"]: s["owner"] for s in _roster(host)}
        host_owned = sorted(sid for sid, o in owner.items() if o == 0)
        client_owned = sorted(sid for sid, o in owner.items() if o == 1)
        cid = _craft_id(host)
        assert host_owned and client_owned, f"not split: {owner}"
        print(f"setup: host owns {host_owned}, client owns {client_owned}; craft {cid}")

        views = [
            ("soldiers", "soldiers", {}),
            ("craft_soldiers", "craft_soldiers", {"craft_id": cid}),
            ("craft_armor", "craft_armor", {"craft_id": cid}),
        ]
        for screen, top, kw in views:
            h = _displayed(host, screen, top, **kw)
            c = _displayed(client, screen, top, **kw)
            assert h == host_owned, f"{screen}: host sees {h}, want {host_owned}"
            assert c == client_owned, f"{screen}: client sees {c}, want {client_owned}"
            print(f"PASS {screen}: host {h} / client {c} (each only its own)")

        print("ALL JOINT SOLDIER-VIEW VISIBILITY TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
