"""Regression test: a soldier's equipment must survive being looked at from a
peer's co-op base, on BOTH equip screens.

Reported from a SEPARATE playtest: a soldier equipped with a weapon in each hand
and rockets in the backpack is transferred to another player's base; at that base
"they are missing the weapons in their hands", the craft equip screen disagrees
with the soldier equip screen, and items dropped onto the soldier there "drop
right back down to the ground items lower panel".

ROOT CAUSE. `BattlescapeGenerator::deployXCOM` assigned per-type sequential
`coopID`s only in its CRAFT branch; the base-storage branch left every item at
the default `coopID == 0`. A craft's `coopItems` manifest is written from that
very base-inventory screen (`SavedBattleGame::moveBaseCoopInventorySave`), so the
manifest was all-zero too - and `BattleUnit::hasCoopItem`, which matches on
`(id, type, owner)`, degenerated into a TYPE-ONLY match. `placeItemByLayout` then
refused to auto-place a visiting player's OWN weapon whenever the peer had ever
equipped that item type on that craft (and `moveCoopItemsToGround` dumped it back
to the floor for the same reason).

Both branches now share one per-type counter, so every item on the inventory tile
has a distinct (type, coopID).

WHAT IS ASSERTED. Three things, in both a co-op and a plain own-base setting
(that is the co-op-vs-vanilla parity: whatever holds at your own base must hold
at a peer's):

  1. every deployed soldier wears its COMPLETE equipment layout - nothing that
     the layout reserves is left lying on the ground pane;
  2. the two screens put the same items in the same slots on that soldier;
  3. the two screens hold the same set of physical item instances (`all`).

Deliberately NOT asserted: ground-pane equality. The base screen deploys every
soldier at the base while the craft screen deploys only that craft's crew, so a
benched soldier's gear is carried in one view and loose in the other; loose
weapons additionally auto-chamber ammo, which moves clips off the ground tile.
Both effects reproduce at a plain own base with no co-op involved - they are
stock OXCE, and `all` is the quantity that is genuinely conserved.

Run:  python tools/coop_test/test_coop_peer_equip_screens.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import test_coop_transferred_equipment as T

OPT = "oxceAlternateCraftEquipmentManagement"
GEAR = [("STR_RIFLE", "right", 1), ("STR_PISTOL", "left", 1), ("STR_SMALL_ROCKET", "backpack", 2)]
# what the peer equips on its own crew, by hand, to populate the craft manifest
PEER_DRAGS = (("STR_RIFLE", "right"), ("STR_PISTOL", "left"), ("STR_SMALL_ROCKET", "backpack"))


def _has(gc, n):
    return any(n in s for s in gc.cmd({"cmd": "get_state"})["states"])


def open_base_inventory(gc, base_name):
    gc.ok({"cmd": "open_soldiers", "base": base_name})
    gc.wait_for("soldiers", lambda: _has(gc, "SoldiersState") or None, timeout=30)
    r = gc.ok({"cmd": "soldiers_inventory"})
    assert r.get("opened"), f"base inventory did not open: {r}"
    gc.wait_for("inventory", lambda: _has(gc, "InventoryState") or None, timeout=30)


def close_base_inventory(gc):
    gc.ok({"cmd": "battle_inventory", "action": "ok"})
    gc.wait_for("inventory closed", lambda: (not _has(gc, "InventoryState")) or None, timeout=30)
    gc.ok({"cmd": "soldiers_ok"})
    gc.wait_for("soldiers closed", lambda: (not _has(gc, "SoldiersState")) or None, timeout=30)


def expected_layout():
    out = {}
    for item, slot, qty in GEAR:
        out.setdefault(T.SLOT_ID[slot], []).extend([item] * qty)
    return {k: sorted(v) for k, v in out.items()}


def read_both_screens(gc, base_name, craft_id, seat, coop=False):
    """Read the soldier equip screen and the craft equip screen for the same
    base. `seat` re-seats the soldier between the two: with alternate craft
    equipment management ON the base inventory de-assigns every soldier from its
    craft on open and re-assigns on OK, so the seat must be re-established
    before the craft screen is opened or its crew is empty."""
    base_g = T.base_equip_screen(gc, base_name)
    seat()
    craft_g = T.craft_equip_screen(gc, craft_id, base_name=None if coop else base_name, coop=coop)
    return base_g, craft_g


def assert_screens_agree(base_g, craft_g, who, label):
    want = expected_layout()
    base_lo = T.loadout_of(base_g, who)
    craft_lo = T.loadout_of(craft_g, who)

    # 1. nothing the layout reserves was left on the floor
    assert T.items_only(base_lo) == want, (
        f"{label}: the soldier equip screen STRIPPED equipment - expected {want}, "
        f"got {T.items_only(base_lo)} (the rest is on the ground pane: {base_g['items']})")
    assert T.items_only(craft_lo) == want, (
        f"{label}: the craft equip screen STRIPPED equipment - expected {want}, "
        f"got {T.items_only(craft_lo)}")

    # 2. same items in the same slots on both screens
    assert base_lo == craft_lo, (
        f"{label}: the two equip screens disagree on the soldier's loadout:\n"
        f"  soldier screen: {base_lo}\n  craft   screen: {craft_lo}")

    # 3. same physical items present (see the module docstring on why NOT `items`)
    assert base_g["all"] == craft_g["all"], (
        f"{label}: the two equip screens hold different item instances:\n"
        f"  soldier screen: {base_g['all']}\n  craft   screen: {craft_g['all']}")
    print(f"PASS {label}: full loadout {T.items_only(base_lo)}, both screens agree")


def equip_by_hand(gc, base_name, crew_limit=3):
    """Really drag items onto the local crew, the way a player does - this is what
    writes the craft's coopItems manifest."""
    open_base_inventory(gc, base_name)
    g = gc.ok({"cmd": "inventory_ground"})
    moved = 0
    for unit in g["soldiers"][:crew_limit]:
        for item, slot in PEER_DRAGS:
            r = gc.cmd({"cmd": "inventory_move", "name": unit["name"], "item": item, "slot": slot})
            if r.get("moved"):
                moved += 1
    close_base_inventory(gc)
    return moved


def main():
    host = GameClient("host", 47811, make_user_dir("host-user"))
    client = GameClient("client", 47812, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client)
        host.ok({"cmd": "set_option", "name": OPT, "value": True})
        client.ok({"cmd": "set_option", "name": OPT, "value": True})
        hb = T.own_base(host)["name"]
        cb = T.own_base(client)["name"]

        # The peer equips its own crew by hand, filling its craft's coopItems
        # manifest. This is the precondition the playtest had and the earlier
        # tests did not - without it hasCoopItem has nothing to match against.
        moved = equip_by_hand(client, cb)
        assert moved > 0, "precondition failed - the peer equipped nothing by hand"
        manifest = next(c for c in client.ok({"cmd": "base_report", "base": cb})["crafts"]
                        if c["type"] == "STR_SKYRANGER")["coopItems"]
        assert manifest, "precondition failed - the peer craft's coopItems manifest is empty"
        ids = sorted({(e["type"], e["id"]) for e in manifest})
        assert len(ids) == len(manifest), (
            f"the co-op item manifest has DUPLICATE (type, id) pairs - hasCoopItem "
            f"cannot tell those items apart and will match by type alone: {manifest}")
        print(f"peer manifest populated: {len(manifest)} entries, all (type,id) distinct")

        # equip the target soldier and transfer it to the peer's base
        r = host.ok({"cmd": "base_report", "base": hb})
        target = next(s for s in r["soldiers"] if s["craft"] == -1)
        host.ok({"cmd": "rename_soldier", "name": target["name"], "newName": "Zzz Target"})
        for item, slot, qty in GEAR:
            host.ok({"cmd": "give_layout", "item": item, "slot": slot,
                     "name": "Zzz", "count": 1, "qty": qty})

        # (parity) the same two screens at the host's OWN base must already agree
        own_craft = next(c for c in r["crafts"] if c["type"] == "STR_SKYRANGER")["id"]
        # bench one of the existing crew so there is a seat to take - this is also
        # the reported repro ("unequip another soldier from the craft and equip the
        # target soldier"), and it is what puts a benched soldier's gear on the
        # craft screen's floor but on its owner's body in the soldier screen.
        bench = next(s for s in r["soldiers"] if s["craft"] == own_craft)
        host.ok({"cmd": "craft_assign", "soldier_id": bench["id"],
                 "craft_id": own_craft, "base": hb, "on": False})

        def seat_own():
            host.ok({"cmd": "craft_assign", "soldier_id": target["id"],
                     "craft_id": own_craft, "base": hb, "on": True})
        seat_own()
        b, c = read_both_screens(host, hb, own_craft, seat_own)
        assert_screens_agree(b, c, "Zzz", "own base")
        host.ok({"cmd": "craft_assign", "soldier_id": target["id"],
                 "craft_id": own_craft, "base": hb, "on": False})

        tr = host.ok({"cmd": "transfer_to_coop_base", "name": "Zzz", "toBase": cb})
        assert tr.get("transferred"), f"transfer failed: {tr}"
        host.ok({"cmd": "visit_coop_base", "base": cb})
        host.wait_for("inside peer base",
                      lambda: host.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None, timeout=60)
        rep = host.wait_for(
            "guest at the peer base",
            lambda: (lambda x: x if any("Zzz" in s["name"] for s in x["soldiers"]) else None)(
                host.ok({"cmd": "base_report", "coop": True})), timeout=30)
        guest = next(s for s in rep["soldiers"] if "Zzz" in s["name"])
        peer_craft = next(c for c in rep["crafts"] if c["type"] == "STR_SKYRANGER")["id"]

        def seat_peer():
            g = next(s for s in host.ok({"cmd": "base_report", "coop": True})["soldiers"]
                     if "Zzz" in s["name"])
            host.ok({"cmd": "craft_assign", "soldier_id": g["id"],
                     "craft_id": peer_craft, "coop": True, "on": True})
        seat_peer()

        # ... and must agree exactly the same way at the PEER's base
        b, c = read_both_screens(host, cb, peer_craft, seat_peer, coop=True)
        assert_screens_agree(b, c, "Zzz", "peer base")

        host.ok({"cmd": "leave_base"})
        host.wait_for("back home",
                      lambda: (not host.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None, timeout=60)
        print("TEST PASSED")
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
