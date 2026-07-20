"""Playtest (P4): gifting a soldier in JOINT transfers ownership and moves it
between the players' soldier lists.

Decision: keep the give-unit feature in JOINT. Gifting a soldier changes its owner;
it must then leave the giver's list and appear in the recipient's, and BOTH machines
must agree on the new owner (shared world).

Run:  python tools/coop_test/test_joint_gift.py
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


def _owner(gc, sid):
    for s in _roster(gc):
        if s["id"] == sid:
            return s["owner"]
    return None


def _name(gc, sid):
    for s in _roster(gc):
        if s["id"] == sid:
            return s["name"]
    return None


def _displayed(gc):
    gc.ok({"cmd": "open_screen", "screen": "soldiers"})
    gc.wait_for("soldiers up",
                lambda: (gc.cmd({"cmd": "screen_state"}).get("top") == "soldiers") or None,
                timeout=15, interval=0.3)
    ids = gc.cmd({"cmd": "screen_state"}).get("displayed", [])
    gc.ok({"cmd": "close_screens"})
    return sorted(ids)


def main():
    js = joint_fixture.bring_up("jgift", (48890, 48891, 48190))
    host, client = js.host, js.client
    try:
        owner = {s["id"]: s["owner"] for s in _roster(host)}
        host_owned = sorted(sid for sid, o in owner.items() if o == 0)
        client_owned = sorted(sid for sid, o in owner.items() if o == 1)
        gift_id = host_owned[0]  # a host-owned soldier we hand to the client
        gname = _name(host, gift_id)
        print(f"setup: host owns {host_owned}, client owns {client_owned}; "
              f"gifting soldier {gift_id} ('{gname}') host->client")

        # host gifts its soldier to seat 1 (the client)
        r = host.ok({"cmd": "gift", "name": gname, "owner": 1})
        assert r.get("ok"), f"gift failed: {r}"

        # BOTH machines must agree the owner is now 1
        client.wait_for("client sees the gifted soldier as owner 1",
                        lambda: (_owner(client, gift_id) == 1) or None, timeout=30, interval=0.5)
        assert _owner(host, gift_id) == 1, f"host still owns the gifted soldier: {_owner(host, gift_id)}"
        assert _owner(client, gift_id) == 1, "client did not adopt the new owner"
        print(f"PASS owner: soldier {gift_id} now owner 1 on BOTH machines")

        # the VIEWS must move it: gone from host's list, present on client's
        host_sees = _displayed(host)
        client_sees = _displayed(client)
        assert gift_id not in host_sees, \
            f"gifted soldier {gift_id} still in the HOST's list {host_sees}"
        assert gift_id in client_sees, \
            f"gifted soldier {gift_id} not in the CLIENT's list {client_sees}"
        assert sorted(host_sees) == sorted(s for s in host_owned if s != gift_id), \
            f"host list wrong after gift: {host_sees}"
        assert sorted(client_sees) == sorted(client_owned + [gift_id]), \
            f"client list wrong after gift: {client_sees}"
        print(f"PASS views: host now {host_sees}, client now {client_sees}")
        print("ALL JOINT GIFT TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
