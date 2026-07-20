"""Playtest B4: JOINT starting soldiers must be split between the two seats.

A JOINT campaign is one shared roster. Every soldier carries an explicit owner
(_ownerPlayerId): host=0, client=1. Left at the default 999 ("unowned"), the
battlescape entry (ConfirmLandingState) maps 999 -> coop 0, so every starting
soldier goes to the host and both players can command the whole roster - the
reported "both players co-own all soldiers, which breaks the battlescape".

The fix splits the newSave starting roster evenly between seats 0 and 1 at
campaign creation (host-side, before the world is streamed to the replica).
Thereafter ownership is like SEPARATE: a hire owns itself (J05).

  SPLIT   after bootstrap, every starting soldier is owned by seat 0 or 1
          (none left at 999), and the split is even (+/- 1).
  MIRROR  the host and the client agree on every soldier's owner (streamed world).

(Hire ownership - a bought soldier owns the buying seat - is J05 and already
covered by test_joint_purchase.)

Run:  python tools/coop_test/test_joint_soldier_ownership.py
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


def _owner_map(gc):
    return {s["id"]: s["owner"] for s in _roster(gc)}


def main():
    js = joint_fixture.bring_up("jsown", (48790, 48791, 48090))
    host, client = js.host, js.client
    try:
        oh = _owner_map(host)
        oc = _owner_map(client)
        assert oh, "empty starting roster"
        print(f"PASS setup: {len(oh)} starting soldiers")

        # ---- SPLIT: no soldier left unowned; even 0/1 division ---------------
        unowned = [sid for sid, o in oh.items() if o not in (0, 1)]
        assert not unowned, f"soldiers with no valid seat owner (co-owned): {unowned}"
        n0 = sum(1 for o in oh.values() if o == 0)
        n1 = sum(1 for o in oh.values() if o == 1)
        assert abs(n0 - n1) <= 1, f"starting roster not split evenly: seat0={n0} seat1={n1}"
        print(f"PASS split: seat0={n0} seat1={n1} (every soldier owned, even split)")

        # ---- MIRROR: host and client agree on every owner -------------------
        assert oh == oc, f"host/client disagree on ownership:\n host={oh}\n client={oc}"
        print("PASS mirror: host and client agree on every soldier's owner")

        print("ALL JOINT SOLDIER-OWNERSHIP TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
