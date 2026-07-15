"""Autonomous regression test for issue #33:

  "Units at a friend's base can see equipment that is assigned to friend's
   soldiers (unavailable, but visible)."

Background: in OpenXcom a soldier's equipment-layout does NOT decrement base
storage - the physical items always live in storage and the layout is just a
re-equip blueprint. So the base-inventory "ground" pane (drawn from storage
minus what the *deployed* soldiers re-take via their layouts) leaks items
whenever a soldier that reserved items is NOT deployed:

  Cases 1 & 2 (visited base): visiting a peer's co-op base clears the peer's
    own soldiers but keeps their storage, so their reserved items show as free.
    Fixed in CoopState (global_state 55) by subtracting departing soldiers'
    layout items from the visited base's storage.

  Cases 3 & 4 (own base): a co-op "guest" soldier stationed at your base is
    stripped from your editable roster (SoldiersState ctor), so it is not
    deployed and its reserved items leak onto your own ground pane. Fixed in
    SoldiersState::btnInventoryClick by pulling guests' reserved items out of
    storage while runInventory builds the ground (then restoring them).

The test reserves an item on every soldier of both bases (via give_layout, which
mirrors equipping: writes a layout, leaves storage untouched) and then checks
all four visibility directions.

Run:  python tools/coop_test/test_coop_base_equip_visibility.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session

# A real soldier-inventory item present in the fresh starting base in a quantity
# (8) that matches the fresh soldier count, so every soldier can reserve one.
RESERVE_ITEM = "STR_PISTOL_CLIP"


def _has_state(gc, name):
    return any(name in s for s in gc.cmd({"cmd": "get_state"})["states"])


def own_base(gc):
    s = gc.ok({"cmd": "get_soldiers"})
    return next(b for b in s["bases"]
               if not b["coopBaseFlag"] and not b["coopIcon"] and b["soldiers"])


def reserve_all(gc):
    """Give every soldier at the local own base a layout entry for RESERVE_ITEM
    (capped at the item stock so reserved never exceeds storage)."""
    b = own_base(gc)
    stock = gc.ok({"cmd": "base_report"})["storage"].get(RESERVE_ITEM, 0)
    count = min(len(b["soldiers"]), stock)
    assert count > 0, f"base has no {RESERVE_ITEM} to reserve"
    g = gc.ok({"cmd": "give_layout", "item": RESERVE_ITEM, "count": count})
    assert g.get("given") == count, f"give_layout gave {g.get('given')} != {count}"
    return b, count


def open_base_inventory_ground(gc, base_name):
    """Open the base inventory (right-click-equip) for base_name and return the
    ground pane's item tally."""
    gc.ok({"cmd": "open_soldiers", "base": base_name})
    gc.wait_for("soldiers screen", lambda: _has_state(gc, "SoldiersState") or None, timeout=30)
    r = gc.ok({"cmd": "soldiers_inventory"})
    assert r.get("opened"), f"base inventory did not open: {r}"
    gc.wait_for("base inventory open", lambda: _has_state(gc, "InventoryState") or None, timeout=30)
    return gc.ok({"cmd": "inventory_ground"})


# -------------------- cases 1 & 2: the visited base --------------------

def assert_visited_no_leak(owner_report, visited_report, label):
    storage = owner_report["storage"]
    reserved = owner_report["reserved"]
    visited = visited_report["storage"]
    assert reserved, f"{label}: precondition failed - owner reserved nothing"
    leaks = {}
    for t, rc in reserved.items():
        assert rc <= storage.get(t, 0), f"{label}: reserved {rc} > stock {storage.get(t,0)} of {t}"
        free = storage.get(t, 0) - rc
        seen = visited.get(t, 0)
        if seen > free:
            leaks[t] = {"seen": seen, "free": free, "reserved": rc, "full": storage.get(t, 0)}
    assert not leaks, f"{label}: LEAK - visited base exposes owner-reserved items: {leaks}"
    print(f"PASS {label}: reserved items hidden from the visited base "
          f"(visited pool {RESERVE_ITEM} = {visited.get(RESERVE_ITEM, 0)})")


def run_visited_case(visitor, owner, owner_base_name, label):
    owner_report = owner.ok({"cmd": "base_report"})
    visitor.ok({"cmd": "visit_coop_base", "base": owner_base_name})
    visitor.wait_for("inside peer base",
                     lambda: visitor.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None, timeout=60)
    visited_report = visitor.ok({"cmd": "base_report", "coop": True})
    assert_visited_no_leak(owner_report, visited_report, label)
    visitor.ok({"cmd": "leave_base"})
    visitor.wait_for("back in own world",
                     lambda: (not visitor.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None, timeout=60)


# -------------------- cases 3 & 4: the own base --------------------

def run_ownbase_case(owner, base, label):
    """Turn one of the owner's soldiers into a stripped co-op "guest" (coopBase
    != -1) and confirm its reserved item no longer leaks onto the owner's own
    inventory ground."""
    guest = base["soldiers"][0]["name"]
    owner.ok({"cmd": "set_coop_base", "name": guest, "value": 999})
    rep = owner.ok({"cmd": "base_report"})
    stripped = [s["name"] for s in rep["soldiers"] if s["coopBase"] != -1]
    assert guest in stripped, f"{label}: '{guest}' was not stripped (coopBase not set)"

    g = open_base_inventory_ground(owner, base["name"])
    seen = g["items"].get(RESERVE_ITEM, 0)
    assert seen == 0, (f"{label}: LEAK - owner sees {seen} {RESERVE_ITEM} reserved by a "
                       f"stripped guest soldier on its own ground pane")
    print(f"PASS {label}: stripped guest's reserved {RESERVE_ITEM} hidden from owner's own ground")


def main():
    host = GameClient("host", 47811, make_user_dir("host-user"))
    client = GameClient("client", 47812, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client)
        print("fresh coop session established")

        hbase, _ = reserve_all(host)
        cbase, _ = reserve_all(client)
        print(f"reserved {RESERVE_ITEM} on all soldiers of '{hbase['name']}' and '{cbase['name']}'")

        # Case 1: host visits the client's base -> must not see client-equipped items.
        run_visited_case(host, client, cbase["name"], "case1 host@client-base")
        # Case 2: client visits the host's base -> must not see host-equipped items.
        run_visited_case(client, host, hbase["name"], "case2 client@host-base")

        # Case 3: client's own base holds a stripped guest -> client must not see
        #         that guest's reserved items on its own ground pane.
        run_ownbase_case(client, cbase, "case3 client-own-base guest")
        # Case 4: host's own base holds a stripped guest -> same, other direction.
        run_ownbase_case(host, hbase, "case4 host-own-base guest")

        print("TEST PASSED")
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
