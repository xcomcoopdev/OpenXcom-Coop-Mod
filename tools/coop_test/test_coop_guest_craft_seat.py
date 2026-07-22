"""Regression test: taking a guest soldier OFF a craft at a peer's co-op base
must stick across a trip to the geoscape.

Reported from a SEPARATE playtest: "when I unassign some soldiers from the
Skyranger in another player's base and then exit to the geoscape, when I come
back to the equip craft screen immediately after then they are re-assigned to
the skyranger again".

`Soldier::_coopCraft` / `_coopCraftType` is the PERSISTED "which craft is this
guest seated on at the peer base" record. A co-op base is rebuilt from it every
time it is entered - `CoopState` re-seats every guest whose `CoopCraft` matches a
craft there (`setCraftAndMoveEquipment`), and `BasescapeState` does the same.

Three of the writers only ever handled the *assigned* direction - they set
CoopCraft inside `if (soldier->getCraft())` with no else - so unassigning left a
stale craft id behind and the next rebuild put the soldier straight back on the
craft:

  SoldiersState::btnOkClick   (the screen that persists guests to the blob)
  CraftInfoState::btnOkClick  (same block, same omission)
  CoopState (sendCraft)       (had an else, but it only cleared CoopBase)

Both directions are covered: the bug is symmetric (a visitor's own guest at the
peer's base), so it reproduces identically whether the host or the client is the
one visiting.

Run:  python tools/coop_test/test_coop_guest_craft_seat.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session

OPT = "oxceAlternateCraftEquipmentManagement"
GEAR = [("STR_RIFLE", "right", 1), ("STR_PISTOL", "left", 1), ("STR_SMALL_ROCKET", "backpack", 2)]


def _has_state(gc, n):
    return any(n in s for s in gc.cmd({"cmd": "get_state"})["states"])


def own_base(gc):
    s = gc.ok({"cmd": "get_soldiers"})
    return next(b for b in s["bases"]
                if not b["coopBaseFlag"] and not b["coopIcon"] and b["soldiers"])


def enter_peer_base(gc, base_name):
    gc.ok({"cmd": "visit_coop_base", "base": base_name})
    gc.wait_for("inside peer base",
                lambda: gc.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None, timeout=60)


def leave_peer_base(gc):
    gc.ok({"cmd": "leave_base"})
    gc.wait_for("back on the geoscape",
                lambda: (not gc.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None, timeout=60)


def cycle_soldiers_screen(gc, base_name):
    """Open and OK the real SoldiersState - the screen whose btnOkClick persists
    each guest's craft seat into the co-op blob."""
    gc.ok({"cmd": "open_soldiers", "base": base_name})
    gc.wait_for("soldiers screen", lambda: _has_state(gc, "SoldiersState") or None, timeout=30)
    gc.ok({"cmd": "soldiers_ok"})
    gc.wait_for("soldiers screen closed",
                lambda: (not _has_state(gc, "SoldiersState")) or None, timeout=30)


def peer_state(gc, craft_id, name="Zzz"):
    """(crew of craft_id, the guest's live craft id, its persisted CoopCraft)."""
    r = gc.ok({"cmd": "base_report", "coop": True})
    crew = next(c for c in r["crafts"] if c["id"] == craft_id)["soldiers"]
    guest = next((s for s in r["soldiers"] if name in s["name"]), None)
    assert guest is not None, f"guest '{name}' vanished from the visited base"
    return crew, guest["craft"], guest["coopCraft"], guest


def run(visitor, peer_base_name, label):
    print(f"--- {label} ---")

    # a uniquely named, equipped soldier, transferred to the peer's base
    hb = own_base(visitor)
    soldier = next(s for s in hb["soldiers"] if not s.get("craft"))["name"]
    visitor.ok({"cmd": "rename_soldier", "name": soldier, "newName": "Zzz Xfer"})
    for item, slot, qty in GEAR:
        visitor.ok({"cmd": "give_layout", "item": item, "slot": slot,
                    "name": "Zzz", "count": 1, "qty": qty})
    tr = visitor.ok({"cmd": "transfer_to_coop_base", "name": "Zzz", "toBase": peer_base_name})
    assert tr.get("transferred"), f"{label}: transfer failed: {tr}"

    enter_peer_base(visitor, peer_base_name)
    rep = visitor.wait_for(
        "guest visible at the peer base",
        lambda: (lambda r: r if any("Zzz" in s["name"] for s in r["soldiers"]) else None)(
            visitor.ok({"cmd": "base_report", "coop": True})), timeout=30)
    guest_id = next(s for s in rep["soldiers"] if "Zzz" in s["name"])["id"]
    craft_id = next(c for c in rep["crafts"] if c["type"] == "STR_SKYRANGER")["id"]

    # seat it, then persist that seat by closing the Soldiers screen
    visitor.ok({"cmd": "craft_assign", "soldier_id": guest_id,
                "craft_id": craft_id, "coop": True, "on": True})
    cycle_soldiers_screen(visitor, peer_base_name)
    crew, live, persisted, _ = peer_state(visitor, craft_id)
    assert (crew, live, persisted) == (1, craft_id, craft_id), (
        f"{label}: precondition failed - after seating the guest expected "
        f"crew=1 craft={craft_id} coopCraft={craft_id}, got "
        f"crew={crew} craft={live} coopCraft={persisted}")
    print(f"{label}: seated - crew={crew}, coopCraft={persisted}")

    # now take it back off, and close the screen again
    _, _, _, guest = peer_state(visitor, craft_id)
    visitor.ok({"cmd": "craft_assign", "soldier_id": guest["id"],
                "craft_id": craft_id, "coop": True, "on": False})
    cycle_soldiers_screen(visitor, peer_base_name)
    crew_off, live_off, persisted_off, _ = peer_state(visitor, craft_id)
    assert crew_off == 0 and live_off == -1, (
        f"{label}: the unassign did not take at all (crew={crew_off} craft={live_off})")
    assert persisted_off == -1, (
        f"{label}: the guest was taken off the craft but its PERSISTED seat still "
        f"says craft {persisted_off} - the co-op base rebuild will re-seat it")
    print(f"{label}: unassigned - crew={crew_off}, coopCraft={persisted_off}")

    # a round trip to the geoscape rebuilds the co-op base: it must stay off
    leave_peer_base(visitor)
    enter_peer_base(visitor, peer_base_name)
    crew_back, live_back, persisted_back, _ = peer_state(visitor, craft_id)
    assert crew_back == 0 and live_back == -1, (
        f"{label}: RE-ASSIGNED - the guest was put back on craft {live_back} by the "
        f"co-op base rebuild (crew {crew_off} -> {crew_back}, "
        f"persisted seat {persisted_off} -> {persisted_back})")
    print(f"PASS {label}: still off the craft after leaving and re-entering "
          f"(crew={crew_back}, coopCraft={persisted_back})")
    leave_peer_base(visitor)


def run_direction(visitor_is_host, label):
    host = GameClient("host", 47811, make_user_dir("host-user"))
    client = GameClient("client", 47812, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client)
        host.ok({"cmd": "set_option", "name": OPT, "value": True})
        client.ok({"cmd": "set_option", "name": OPT, "value": True})
        if visitor_is_host:
            run(host, own_base(client)["name"], label)
        else:
            run(client, own_base(host)["name"], label)
    finally:
        host.shutdown(); client.shutdown()


def main():
    run_direction(True, "host visiting the client's base")
    run_direction(False, "client visiting the host's base")
    print("TEST PASSED")


if __name__ == "__main__":
    main()
