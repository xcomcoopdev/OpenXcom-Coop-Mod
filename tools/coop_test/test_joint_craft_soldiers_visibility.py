"""Playtest (P1): the craft-crew assignment screen shows only YOUR soldiers.

Same ownership parity as the base roster, for CraftSoldiersState. Each player sees
only their own soldiers to assign; each loads their own onto the shared craft (a
mixed-owner crew forms because both contribute). Non-destructive: filter a local
copy, never mutate the shared roster.

Run:  python tools/coop_test/test_joint_craft_soldiers_visibility.py
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
    raise AssertionError("no real base in get_soldiers")


def _first_craft_id(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            crafts = b.get("crafts", [])
            assert crafts, "no craft at the starting base"
            return crafts[0]["id"]
    raise AssertionError("no real base")


def _displayed(gc, craft_id):
    r = gc.ok({"cmd": "open_screen", "screen": "craft_soldiers", "craft_id": craft_id})
    assert r.get("ok"), f"could not open craft_soldiers: {r}"
    gc.wait_for("craft_soldiers up",
                lambda: (gc.cmd({"cmd": "screen_state"}).get("top") == "craft_soldiers") or None,
                timeout=15, interval=0.3)
    ids = gc.cmd({"cmd": "screen_state"}).get("displayed", [])
    gc.ok({"cmd": "close_screens"})
    return sorted(ids)


def main():
    js = joint_fixture.bring_up("jcs", (48870, 48871, 48170))
    host, client = js.host, js.client
    try:
        owner = {s["id"]: s["owner"] for s in _roster(host)}
        host_owned = sorted(sid for sid, o in owner.items() if o == 0)
        client_owned = sorted(sid for sid, o in owner.items() if o == 1)
        assert host_owned and client_owned, f"roster not split: {owner}"
        cid_h = _first_craft_id(host)
        cid_c = _first_craft_id(client)
        assert cid_h == cid_c, "craft id differs across machines"
        print(f"setup: craft {cid_h}; host owns {host_owned}, client owns {client_owned}")

        host_sees = _displayed(host, cid_h)
        client_sees = _displayed(client, cid_c)
        print(f"host craft-crew screen shows:   {host_sees}")
        print(f"client craft-crew screen shows: {client_sees}")

        assert host_sees == host_owned, \
            f"host craft-crew shows {host_sees}, want only its own {host_owned}"
        assert client_sees == client_owned, \
            f"client craft-crew shows {client_sees}, want only its own {client_owned}"
        print("PASS: each player's craft-crew screen shows only their own soldiers")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
