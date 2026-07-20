"""Playtest (co-ownership REPRO): each player's soldier LIST must show only the
soldiers they own - not the whole shared roster.

A JOINT campaign holds ONE shared roster on both machines (the streamed world), so
`get_soldiers` (the raw base roster) legitimately returns every soldier on both -
that is NOT the bug and is what my earlier tests wrongly checked. The bug is in the
soldier-list SCREEN (SoldiersState): in JOINT it filters nothing, so BOTH players
SEE every soldier. Each should see only the half they own (host = owner 0, client =
owner 1), the way SEPARATE mode shows each machine only its own soldiers.

This test is a REPRO ONLY (no fix yet): open the real SoldiersState on both machines
and read what it displays (screen_state.displayed). Today both show all N -> the
assertions fail, proving the co-ownership is a view-filtering gap.

Run:  python tools/coop_test/test_joint_soldier_visibility.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo


def _roster(gc):
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        if not b.get("coopBaseFlag") and not b.get("coopIcon"):
            return b["soldiers"]
    raise AssertionError("no real base in get_soldiers")


def _displayed(gc):
    """The soldier ids the SoldiersState screen actually shows on this machine."""
    r = gc.ok({"cmd": "open_screen", "screen": "soldiers"})
    assert r.get("ok"), f"could not open soldiers screen: {r}"
    gc.wait_for("soldiers screen up",
                lambda: (gc.cmd({"cmd": "screen_state"}).get("top") == "soldiers") or None,
                timeout=15, interval=0.3)
    ss = gc.cmd({"cmd": "screen_state"})
    ids = ss.get("displayed", [])
    gc.ok({"cmd": "close_screens"})
    return sorted(ids)


def main():
    js = joint_fixture.bring_up("jsvis", (48860, 48861, 48160))
    host, client = js.host, js.client
    try:
        # owners are split on the shared roster (both machines agree) - confirmed.
        owner = {s["id"]: s["owner"] for s in _roster(host)}
        assert owner == {s["id"]: s["owner"] for s in _roster(client)}, "owners differ"
        host_owned = sorted(sid for sid, o in owner.items() if o == 0)
        client_owned = sorted(sid for sid, o in owner.items() if o == 1)
        assert host_owned and client_owned, f"roster not split: {owner}"
        print(f"setup: {len(owner)} soldiers; host owns {host_owned}, client owns {client_owned}")

        # what each machine's soldier LIST actually shows.
        host_sees = _displayed(host)
        client_sees = _displayed(client)
        print(f"host soldier list shows:   {host_sees}")
        print(f"client soldier list shows: {client_sees}")

        # THE repro: today both see the WHOLE roster (bug). Assert the DESIRED state so
        # the test is RED now and turns green when the view is filtered by ownership.
        all_ids = sorted(owner)
        both_see_all = (host_sees == all_ids and client_sees == all_ids)
        if both_see_all:
            print("REPRO CONFIRMED: BOTH players' soldier lists show the ENTIRE roster "
                  f"({all_ids}) - soldiers are co-owned in the view.")
        assert host_sees == host_owned, \
            f"host soldier list shows {host_sees}, should show only its own {host_owned} " \
            f"(sees {'ALL' if host_sees == all_ids else 'extra'})"
        assert client_sees == client_owned, \
            f"client soldier list shows {client_sees}, should show only its own {client_owned}"
        print("PASS: each player sees only their owned half of the roster")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
