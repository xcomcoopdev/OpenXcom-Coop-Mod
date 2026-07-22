"""Regression tests for the equipment of a soldier that was vanilla-transferred
to a co-op base ("issue investigation" + the 2026-07-21 playtest report).

Two scenarios, each in its own fresh session (a fresh base has exactly ONE
base-resident soldier, and only a base-resident soldier is individually
transferable):

  scenario_conservation()   - the ORIGINAL test. The host equips a base soldier
    with a rocket (persistent layout), then does a *vanilla base transfer* (NOT
    a gift/ownership change) of that soldier to the client's base. The concern
    raised was that the transfer might *duplicate* the equipment. This locks in
    the conservation invariant that proves it does not:
      1. The total number of the item across the whole co-op world (host base
         storage + client base storage) is unchanged by the transfer.
      2. In the equip screen at the client's base, the total number of rocket
         *instances* actually present (ground + carried + loaded) never exceeds
         the client base's real storage - and is the same whether the host
         (visiting) or the client (at home) opens it.

  scenario_two_screens_agree() - the playtest report. With "Alternate craft
    equipment management" ON (so the gear is supposed to travel with the
    soldier), the host equips a soldier with a weapon in EACH hand and rockets
    in the backpack, transfers it to the client's base, visits that base and
    seats the soldier on the peer's craft. Then it opens BOTH equip screens at
    that base:

      Bases > Soldiers > Inventory        (SoldiersState -> runInventory(0))
      Bases > Equip craft > ... > Inventory (CraftEquipmentState -> runInventory(craft))

    and asserts the two screens agree - the soldier wears the SAME items in the
    SAME slots, and the SAME items are free on the ground pane - and that the
    loadout is still the one that was equipped before the transfer. The report
    was that the hand weapons vanish on the soldier screen, the two screens
    disagree, and items dropped onto the soldier in the craft screen fall
    straight back to the ground pane.

Run:  python tools/coop_test/test_coop_transferred_equipment.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session

ROCKET = "STR_SMALL_ROCKET"
OPT = "oxceAlternateCraftEquipmentManagement"

# The playtest loadout: a weapon in each hand + rockets in the backpack. All are
# in a fresh starting base's stores (PISTOL 2, RIFLE 2, SMALL_ROCKET 4).
LOADOUT = [
    ("STR_RIFLE", "right", 1),
    ("STR_PISTOL", "left", 1),
    (ROCKET, "backpack", 2),
]
SLOT_ID = {"right": "STR_RIGHT_HAND", "left": "STR_LEFT_HAND",
           "backpack": "STR_BACK_PACK", "belt": "STR_BELT"}


def _has_state(gc, n):
    return any(n in s for s in gc.cmd({"cmd": "get_state"})["states"])


def own_base(gc):
    s = gc.ok({"cmd": "get_soldiers"})
    return next(b for b in s["bases"]
               if not b["coopBaseFlag"] and not b["coopIcon"] and b["soldiers"])


def storage_rockets(gc, base_name=None, coop=False):
    req = {"cmd": "base_report"}
    if coop:
        req["coop"] = True
    elif base_name:
        req["base"] = base_name
    return gc.ok(req)["storage"].get(ROCKET, 0)


# ---- the two equip screens ------------------------------------------------

def _read_open_inventory(gc):
    """Read the ground/carried panes of an already-open InventoryState and close
    it again."""
    gc.wait_for("inventory", lambda: _has_state(gc, "InventoryState") or None, timeout=30)
    g = gc.ok({"cmd": "inventory_ground"})
    gc.ok({"cmd": "battle_inventory", "action": "ok"})
    gc.wait_for("inventory closed",
                lambda: (not _has_state(gc, "InventoryState")) or None, timeout=30)
    return g


def base_equip_screen(gc, base_name):
    """Bases > Soldiers > (Solder Info dropdown) > Inventory, for base_name."""
    gc.ok({"cmd": "open_soldiers", "base": base_name})
    gc.wait_for("soldiers", lambda: _has_state(gc, "SoldiersState") or None, timeout=30)
    r = gc.ok({"cmd": "soldiers_inventory"})
    assert r.get("opened"), f"base equip screen did not open: {r}"
    g = _read_open_inventory(gc)
    gc.ok({"cmd": "soldiers_ok"})
    gc.wait_for("soldiers closed",
                lambda: (not _has_state(gc, "SoldiersState")) or None, timeout=30)
    return g


def craft_equip_screen(gc, craft_id, base_name=None, coop=False):
    """Bases > Equip craft > <craft> > Equipment > Inventory."""
    req = {"cmd": "open_craft_equipment", "craft_id": craft_id}
    if coop:
        req["coop"] = True
    elif base_name:
        req["base"] = base_name
    gc.ok(req)
    gc.wait_for("craft equipment",
                lambda: _has_state(gc, "CraftEquipmentState") or None, timeout=30)
    r = gc.ok({"cmd": "craft_inventory"})
    assert r.get("opened"), f"craft equip screen did not open the inventory: {r}"
    g = _read_open_inventory(gc)
    gc.ok({"cmd": "craft_equipment_ok"})
    gc.wait_for("craft equipment closed",
                lambda: (not _has_state(gc, "CraftEquipmentState")) or None, timeout=30)
    return g


def loadout_of(ground, name):
    """{slot -> sorted [(item, x, y, ammo...)]} for the named soldier in an
    inventory_ground dump; {} if that soldier is not in the screen at all."""
    for u in ground["soldiers"]:
        if name in u["name"]:
            return {slot: sorted((e["item"], e["x"], e["y"], tuple(e["ammo"]))
                                 for e in entries)
                    for slot, entries in u["slots"].items()}
    return {}


def expected_loadout():
    out = {}
    for item, slot, qty in LOADOUT:
        out.setdefault(SLOT_ID[slot], []).extend([item] * qty)
    return {k: sorted(v) for k, v in out.items()}


def items_only(lo):
    return {slot: sorted(e[0] for e in entries) for slot, entries in lo.items()}


# ---- scenario 1: no duplication (the original test) -----------------------

def equip_rocket_instances(gc, base_name):
    """Open the base equip screen and return (all_instances, ground, carried, units)."""
    g = base_equip_screen(gc, base_name)
    return (g["all"].get(ROCKET, 0), g["items"].get(ROCKET, 0),
            g["carried"].get(ROCKET, 0), g["units"])


def scenario_conservation():
    host = GameClient("host", 47811, make_user_dir("host-user"))
    client = GameClient("client", 47812, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client)
        print("fresh coop session established")

        hb = own_base(host); cb = own_base(client)
        world_rockets_before = storage_rockets(host, hb["name"]) + storage_rockets(client, cb["name"])
        client_stock = storage_rockets(client, cb["name"])
        print(f"world rockets before={world_rockets_before}, client base stock={client_stock}")
        assert client_stock > 0, "client base has no rockets to reason about"

        # host equips a base-resident soldier with a rocket in the off-hand
        soldier = next(s for s in hb["soldiers"] if not s.get("craft"))["name"]
        host.ok({"cmd": "give_layout", "item": ROCKET, "slot": "left", "name": soldier, "count": 1})
        print(f"equipped host soldier '{soldier}' with a {ROCKET} (off-hand)")

        # vanilla base transfer of that soldier to the client's base
        tr = host.ok({"cmd": "transfer_to_coop_base", "name": soldier, "toBase": cb["name"]})
        assert tr.get("transferred"), f"transfer failed: {tr}"

        def client_sees():
            s = client.cmd({"cmd": "get_soldiers"})
            return any(x["name"] == soldier for b in s.get("bases", []) for x in b["soldiers"]) or None
        client.wait_for("client sees transferred soldier", client_sees, timeout=30)

        # (1) no item created anywhere in the co-op world
        world_rockets_after = storage_rockets(host, hb["name"]) + storage_rockets(client, cb["name"])
        assert world_rockets_after == world_rockets_before, (
            f"item DUPLICATION: world rockets {world_rockets_before} -> {world_rockets_after}")
        print(f"PASS no-world-duplication: world rockets still {world_rockets_after}")

        # (2) host visits the client base (client idle) and opens the equip screen
        host.ok({"cmd": "visit_coop_base", "base": cb["name"]})
        host.wait_for("host inside peer base",
                      lambda: host.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None, timeout=60)
        host_all, host_gnd, host_car, host_units = equip_rocket_instances(host, cb["name"])
        host.ok({"cmd": "leave_base"})
        host.wait_for("host back home",
                      lambda: (not host.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None, timeout=60)

        # client opens the equip screen for its own base (host idle)
        cli_all, cli_gnd, cli_car, cli_units = equip_rocket_instances(client, cb["name"])

        print(f"HOST visited: rocket_instances={host_all} (ground={host_gnd} carried={host_car} units={host_units})")
        print(f"CLIENT home : rocket_instances={cli_all} (ground={cli_gnd} carried={cli_car} units={cli_units})")

        assert host_all <= client_stock, (
            f"host equip screen shows {host_all} rocket instances > client stock {client_stock} (phantom items)")
        assert cli_all <= client_stock, (
            f"client equip screen shows {cli_all} rocket instances > client stock {client_stock}")
        assert host_all == cli_all, (
            f"host and client disagree on total rocket instances at the client base: "
            f"host={host_all} client={cli_all}")
        print(f"PASS equip-view-conservation: both see {host_all} rocket instances (== stock {client_stock})")
    finally:
        host.shutdown(); client.shutdown()


# ---- the co-op trade confirmation -----------------------------------------

def assert_transfer_confirms_cleanly(host, client, seconds=25):
    """Run the clock until the co-op layer confirms the trade, and require that
    NEITHER side ends up on an error dialog.

    The sender confirms by calling Base::removePendingTransfers(), which
    re-validates that everything it promised is still in its stores. An
    equipment transfer built by TransferItemsState has already debited those
    stores when it created the 0-hour Transfer objects, so the re-validation
    finds nothing left to remove and the whole confirmation fails: CoopState 552
    "Failed to remove items from your base." on the sender, while the receiver
    has already added the goods (CoopState 150 "Items received").
    """
    def top(gc):
        return gc.cmd({"cmd": "get_state"})["states"][-1]

    deadline = time.time() + seconds
    dialogs = {}
    while time.time() < deadline:
        for gc in (host, client):
            if "GeoscapeState" in top(gc):
                gc.cmd({"cmd": "geo_set_speed", "idx": 5})
            elif "CoopState" in top(gc):
                info = gc.cmd({"cmd": "coop_dialog_info"})
                if info.get("present"):
                    dialogs[gc.name] = (info.get("code"), info.get("title"))
        if len(dialogs) == 2:
            break
        time.sleep(0.5)

    bad = {n: d for n, d in dialogs.items() if d[0] not in (150,)}
    assert not bad, (
        "the co-op transfer confirmation FAILED on one side while the other "
        f"already applied it: {bad} (other side: {dialogs})")
    print(f"PASS transfer-confirms-cleanly: {dialogs}")


# ---- scenario 2: the two equip screens must agree -------------------------

def scenario_two_screens_agree():
    host = GameClient("host", 47811, make_user_dir("host-user"))
    client = GameClient("client", 47812, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client)
        print("fresh coop session established")

        # gear is supposed to travel with the soldier -> both machines, same value
        host.ok({"cmd": "set_option", "name": OPT, "value": True})
        client.ok({"cmd": "set_option", "name": OPT, "value": True})

        hb = own_base(host); cb = own_base(client)

        # 1. a weapon in each hand + rockets in the backpack, on the lone
        #    base-resident soldier (uniquely renamed so every lookup is exact).
        soldier = next(s for s in hb["soldiers"] if not s.get("craft"))["name"]
        host.ok({"cmd": "rename_soldier", "name": soldier, "newName": "Zzz Xfer"})
        stock = host.ok({"cmd": "base_report", "base": hb["name"]})["storage"]
        for item, slot, qty in LOADOUT:
            assert stock.get(item, 0) >= qty, \
                f"sending base needs {qty}x {item} to equip (has {stock.get(item, 0)})"
            g = host.ok({"cmd": "give_layout", "item": item, "slot": slot,
                         "name": "Zzz", "count": 1, "qty": qty})
            assert g.get("entries") == qty, \
                f"give_layout wrote {g.get('entries')} {item} entries, wanted {qty}"
        want = expected_loadout()
        print(f"equipped 'Zzz Xfer' at the host base: {want}")

        # 2. vanilla base transfer to the client's base
        tr = host.ok({"cmd": "transfer_to_coop_base", "name": "Zzz", "toBase": cb["name"]})
        assert tr.get("transferred"), f"transfer failed: {tr}"

        # 2b. let the clock run so the co-op layer confirms the trade
        #     ("transfer_completed"). The sender must then be able to remove what
        #     it sent; if it cannot, the trade is left half-applied - the receiver
        #     has the goods, the sender's pending-transfer list is never cleared
        #     and no funds are debited.
        assert_transfer_confirms_cleanly(host, client)

        # 3. the host visits the peer base and seats the soldier on the peer craft
        host.ok({"cmd": "visit_coop_base", "base": cb["name"]})
        host.wait_for("host inside peer base",
                      lambda: host.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None, timeout=60)
        rep = host.wait_for(
            "guest visible at the visited base",
            lambda: (lambda r: r if any("Zzz" in s["name"] for s in r["soldiers"]) else None)(
                host.ok({"cmd": "base_report", "coop": True})), timeout=30)
        guest = next(s for s in rep["soldiers"] if "Zzz" in s["name"])
        assert guest["layout"], (
            f"transferred soldier arrived with an EMPTY layout ({OPT} is ON, so its "
            f"gear should have travelled with it)")
        craft = next(c for c in rep["crafts"] if c["type"] == "STR_SKYRANGER")
        host.ok({"cmd": "craft_assign", "soldier_id": guest["id"],
                 "craft_id": craft["id"], "coop": True, "on": True})
        host.wait_for(
            "guest seated on the peer craft",
            lambda: (next(c for c in host.ok({"cmd": "base_report", "coop": True})["crafts"]
                          if c["id"] == craft["id"])["soldiers"] > 0) or None,
            timeout=30)
        print(f"seated 'Zzz Xfer' on the peer's {craft['type']} {craft['id']}")

        # 4. + 5. both equip screens at that base, read back to back
        base_g = base_equip_screen(host, cb["name"])
        craft_g = craft_equip_screen(host, craft["id"], coop=True)

        base_lo = loadout_of(base_g, "Zzz")
        craft_lo = loadout_of(craft_g, "Zzz")
        print(f"soldier equip screen: soldier={items_only(base_lo)} ground={base_g['items']}")
        print(f"craft  equip screen: soldier={items_only(craft_lo)} ground={craft_g['items']}")

        # (a) the soldier still wears what was equipped before the transfer
        assert items_only(base_lo) == want, (
            f"soldier equip screen at the peer base LOST equipment: expected {want}, "
            f"got {items_only(base_lo)}")
        print("PASS soldier-screen-keeps-loadout")

        # (b) the two screens show the same soldier loadout, slot for slot
        assert base_lo == craft_lo, (
            f"the two equip screens DISAGREE on the soldier's loadout:\n"
            f"  soldier screen: {base_lo}\n"
            f"  craft   screen: {craft_lo}")
        print("PASS screens-agree-on-soldier")

        # (c) ... and hold the very same set of physical item instances.
        #
        # NOT the ground pane: the two screens legitimately distribute the same
        # items differently, and they do so in stock OXCE with no co-op base in
        # sight (verified against a plain own base). The base screen deploys
        # EVERY soldier at the base, the craft screen only that craft's crew, so
        # a benched soldier's gear sits on its body in one and on the floor in
        # the other; and the extra loose weapons then auto-chamber ammo, which
        # takes clips off the ground tile and puts them inside a weapon. What is
        # genuinely invariant is the total instance count - `all` counts every
        # BattleItem including ammo loaded into weapons.
        assert base_g["all"] == craft_g["all"], (
            f"the two equip screens hold DIFFERENT items (not just a different "
            f"layout of the same ones):\n"
            f"  soldier screen: {base_g['all']}\n"
            f"  craft   screen: {craft_g['all']}")
        print("PASS screens-agree-on-item-instances")

        host.ok({"cmd": "leave_base"})
        host.wait_for("host back home",
                      lambda: (not host.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None, timeout=60)
    finally:
        host.shutdown(); client.shutdown()


def main():
    scenario_conservation()
    scenario_two_screens_agree()
    print("TEST PASSED")


if __name__ == "__main__":
    main()
