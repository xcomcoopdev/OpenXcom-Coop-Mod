"""Feature (JOINT craft capacity): the shared craft is ONE physical ship with a single
real capacity N. Both players load the same ship, so:

  * "space used" = the COMBINED aboard soldiers of BOTH players (host-authoritative,
    identical on both machines via the shared roster + craft_assign) - NOT just this
    player's own soldiers, and NOT the old SEPARATE per-seat HALF split that locked each
    player to N/2.
  * "space available" = full N - combined used (so players can split the seats however
    they like, up to the ship's real capacity).
  * the assignable LIST is still own-only (visibleSoldiers), so each player manages only
    their own soldiers while the capacity reflects the shared ship.

Pre-fix, getSpaceAvailable() returned maxUnits/2 - used for a coop craft (halved), and the
displayed roster was own-only while nothing tied the two together. This asserts the new
"shared cap, host-synced used" behaviour on BOTH machines.

Run:  python tools/coop_test/test_joint_craft_capacity.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture


def _roster(gc):
    out = []
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        out.extend(b["soldiers"])
    return out


def _base0(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base")


def _skyranger(gc):
    for c in _base0(gc)["crafts"]:
        if "SKYRANGER" in c["type"]:
            return c
    raise AssertionError("no skyranger")


def _aboard(gc, cid):
    return sorted(s["id"] for s in _roster(gc) if s["craftId"] == cid)


def _craft_screen(gc, cid):
    gc.ok({"cmd": "open_screen", "screen": "craft_soldiers", "craft_id": cid})
    st = gc.ok({"cmd": "screen_state"})
    assert st.get("top") == "craft_soldiers", f"craft_soldiers not on top: {st.get('top')}"
    return st


def main():
    js = joint_fixture.bring_up("jcap", (48820, 48821, 48120))
    host, client = js.host, js.client
    try:
        owner = {s["id"]: s["owner"] for s in _roster(host)}
        assert owner == {s["id"]: s["owner"] for s in _roster(client)}, \
            "host/client disagree on owners"
        seat0 = sorted(sid for sid, o in owner.items() if o == 0)  # host-owned
        seat1 = sorted(sid for sid, o in owner.items() if o == 1)  # client-owned
        assert len(seat0) >= 3 and len(seat1) >= 3, \
            f"need >=3 soldiers per seat: seat0={seat0} seat1={seat1}"
        cid = _skyranger(host)["id"]

        # Start from an empty craft (host-authoritative clear).
        for sid in owner:
            host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": False})
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} craft empty", lambda gc=gc: (_aboard(gc, cid) == []) or None,
                        timeout=30, interval=0.5)

        # Board a MIXED crew: 3 host-owned + 3 client-owned = 6 combined.
        board0, board1 = seat0[:3], seat1[:3]
        board = sorted(board0 + board1)
        for sid in board:
            host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} mixed crew aboard",
                        lambda gc=gc: (_aboard(gc, cid) == board) or None,
                        timeout=40, interval=0.5)
        print(f"PASS setup: mixed crew aboard {board} (3 host-owned + 3 client-owned)")

        hs = _craft_screen(host, cid)
        cs = _craft_screen(client, cid)
        N = hs["maxUnits"]
        assert N == cs["maxUnits"], f"maxUnits differ host={N} client={cs['maxUnits']}"
        assert N >= 6, f"skyranger capacity {N} too small for this test"

        # 1) USED = combined (6), identical on both machines - NOT own-only (3).
        assert hs["usedNum"] == 6 and cs["usedNum"] == 6, \
            f"space used not combined/synced: host={hs['usedNum']} client={cs['usedNum']} (want 6)"
        print(f"PASS used: both machines report combined space used = 6 "
              f"(each contributed only 3) - capacity counts BOTH players")

        # 2) AVAILABLE = full N - used (NOT halved N/2 - used), identical on both.
        assert hs["availableNum"] == N - 6, \
            f"host available {hs['availableNum']} != full {N} - 6 (halved? = {N // 2 - 6})"
        assert cs["availableNum"] == N - 6, \
            f"client available {cs['availableNum']} != full {N} - 6"
        assert N - 6 != N // 2 - 6, "test degenerate: pick a craft where half != full"
        print(f"PASS available: both report full-cap availability {N}-6={N - 6} "
              f"(not the old halved {N // 2}-6={N // 2 - 6})")

        # 3) The assignable LIST stays own-only while capacity is shared.
        hd, cd = sorted(hs["displayed"]), sorted(cs["displayed"])
        assert set(hd) <= set(seat0), f"host list not own-only: {hd} vs seat0 {seat0}"
        assert set(cd) <= set(seat1), f"client list not own-only: {cd} vs seat1 {seat1}"
        assert not (set(hd) & set(cd)), f"host/client lists overlap: {hd} {cd}"
        print("PASS list: each player's assignable list is own-only (shared capacity, "
              "separate rosters)")

        # 4) Not locked down: with 6 aboard (> N/2 when N<12 ... assert additive room).
        # A player may keep adding while combined < N: available > 0 here proves the ship
        # is not fixed at a per-seat half.
        assert hs["availableNum"] > 0, "no room left despite combined < capacity (locked down?)"

        host.ok({"cmd": "close_screens"})
        client.ok({"cmd": "close_screens"})
        js.finish()
        print("ALL JOINT CRAFT-CAPACITY TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
