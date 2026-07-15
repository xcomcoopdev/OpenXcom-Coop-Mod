"""Regression test for the "extra rocket after transferring an equipped soldier"
report (issue investigation).

Scenario: the host equips a base soldier with a rocket (persistent layout),
then does a *vanilla base transfer* (NOT a gift/ownership change) of that
soldier to the client's base. Both players then look at the soldier equip
screen for the client's base.

The concern raised was that a soldier's equipment might be *duplicated* by the
transfer. This test locks in the conservation invariant that proves it is not:

  1. The total number of the item across the whole co-op world (host base
     storage + client base storage) is unchanged by the transfer.
  2. In the equip screen at the client's base, the total number of rocket
     *instances* actually present (ground + carried + loaded) never exceeds the
     client base's real storage - and is the same whether the host (visiting)
     or the client (at home) opens it.

Run:  python tools/coop_test/test_coop_transferred_equipment.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session

ROCKET = "STR_SMALL_ROCKET"


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


def equip_rocket_instances(gc, base_name):
    """Open the base equip screen and return (all_instances, ground, carried, units)."""
    gc.ok({"cmd": "open_soldiers", "base": base_name})
    gc.wait_for("soldiers", lambda: _has_state(gc, "SoldiersState") or None, timeout=30)
    r = gc.ok({"cmd": "soldiers_inventory"})
    assert r.get("opened"), f"equip screen did not open: {r}"
    gc.wait_for("inventory", lambda: _has_state(gc, "InventoryState") or None, timeout=30)
    g = gc.ok({"cmd": "inventory_ground"})
    # close back out cleanly
    gc.ok({"cmd": "battle_inventory", "action": "ok"})
    gc.wait_for("closed", lambda: (not _has_state(gc, "InventoryState")) or None, timeout=30)
    gc.ok({"cmd": "soldiers_ok"})
    return (g["all"].get(ROCKET, 0), g["items"].get(ROCKET, 0),
            g["carried"].get(ROCKET, 0), g["units"])


def main():
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
        print("TEST PASSED")
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
