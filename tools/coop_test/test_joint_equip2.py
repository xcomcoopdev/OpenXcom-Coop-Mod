"""PRD-J09 GAP-5b: the SIBLING base-screen store mutators GAP-5 deferred must be
host-authoritative too, so the shared base stores never drift from the host.

GAP-5 routed CraftEquipmentState (loading loose items onto a craft). Three more
base screens move items in/out of the same host-authoritative base stores and, on
a replica, ran ungated - silently dropping/adding to chkItems (= every base's
ItemContainer::getTotalQuantity(), the GAP-4 checksum), which then trips a
recurring auto-resync that reverts the client's action:

  1. CraftWeaponsState  - arm/rearm a craft weapon: the launcher (+ loaded clips)
                          move against the base stores (lstWeaponsClick).
  2. SoldierArmorState  - change a soldier's armor: the old armor's store item is
                          returned + the new one consumed (lstArmorClick).
  3. CraftArmorState    - the per-soldier / bulk de-equip armor sites, same store
                          moves (btnDeequipAllArmorClick and friends).

THE FIX routes each through a new host-authoritative joint_cmd carrying the
ABSOLUTE end-state (which weapon in which slot; which armor on which soldier),
last-write-wins - `craft_rearm` for #1, `soldier_armor` for #2 and #3 (a soldier
armor swap is the same identity operation on both screens). The replica mutates
nothing; the host validates + applies + broadcasts; both worlds move together.

Each screen is its OWN bring-up so it can be red/green-captured in isolation:

    python tools/coop_test/test_joint_equip2.py            # all three
    python tools/coop_test/test_joint_equip2.py weapon     # just CraftWeaponsState
    python tools/coop_test/test_joint_equip2.py armor      # just SoldierArmorState
    python tools/coop_test/test_joint_equip2.py deequip    # just CraftArmorState

HOST-AUTHORITY  the client drives the REAL screen path (a `craft_rearm` /
                `soldier_armor` / `craft_deequip_armor` harness driver). Only a
                joint_cmd can move the HOST's own base stores, so the host's stock
                moving is proof the action went through the protocol. Before the
                fix the host never applies it (the poll times out - the RED).
NO DRIFT        base stores stay identical on both machines (world equality +
                chkItems) and NO auto-resync is needed - the drift never existed.
                Before the fix the replica-local mutation makes chkItems diverge
                and trips the GAP-4 auto-repair (requests > 0).
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture

WEAPON = "STR_STINGRAY"
LAUNCHER = "STR_STINGRAY_LAUNCHER"
ARMOR = "STR_PERSONAL_ARMOR_UC"
ARMOR_ITEM = "STR_PERSONAL_ARMOR"
DEFAULT_ARMOR = "STR_NONE_UC"
STOCK = 4


def _chk(gc):
    return _wait_ok(gc, {"cmd": "joint_checksum"})["chkItems"]


def _resync(gc):
    return _wait_ok(gc, {"cmd": "joint_resync_stats"})


def _wait_ok(gc, obj):
    return gc.ok(obj)


def _base_items(gc, item):
    return gc.ok({"cmd": "geo_state"})["bases"][0]["items"].get(item, 0)


def _soldier(gc, sid):
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        for s in b["soldiers"]:
            if s["id"] == sid:
                return s
    return None


def _interceptor(gc, craft_id):
    for c in gc.ok({"cmd": "geo_state"})["bases"][0]["crafts"]:
        if c["id"] == craft_id and c["type"] == "STR_INTERCEPTOR":
            return c
    return None


def _reset_resync(host, client):
    host.ok({"cmd": "joint_reset_resync_stats"})
    client.ok({"cmd": "joint_reset_resync_stats"})


def _assert_no_resync(host, client):
    rc = _resync(client)
    assert rc["requests"] == 0 and not rc["gaveUp"], (
        f"drift tripped the auto-resync instead of being host-authoritative: "
        f"client resync stats={rc}")
    rh = _resync(host)
    assert rh["requests"] == 0, f"host served an unexpected resync: {rh}"
    print(f"PASS no-resync: client escalated requests={rc['requests']} "
          f"(transient mismatches={rc['mismatches']} absorbed by the debounce)")


# ======================================================================
# 1. CraftWeaponsState - arm a craft weapon (launcher leaves the shared stores).
# ======================================================================
def test_weapon(ports):
    js = joint_fixture.bring_up("jrearm", ports)
    host, client = js.host, js.client
    try:
        # Seed identical worlds: an EMPTY interceptor + launcher stock on both.
        craft_ids = set()
        for gc in (host, client):
            r = gc.ok({"cmd": "spawn_craft", "type": "STR_INTERCEPTOR", "weapon": "STR_NONE"})
            craft_ids.add(r["craft_id"])
            gc.ok({"cmd": "give_items", "item": LAUNCHER, "count": STOCK})
        assert len(craft_ids) == 1, f"spawn_craft gave different ids per machine: {craft_ids}"
        craft_id = craft_ids.pop()
        js.assert_world_equal("bootstrap + empty interceptor + launchers")

        base0 = _base_items(host, LAUNCHER)
        assert _base_items(client, LAUNCHER) == base0, "premise: launcher stock not equal"
        assert _interceptor(host, craft_id)["weaponLoadout"][0] == "", "premise: slot 0 not empty"
        h0, c0 = _chk(host), _chk(client)
        assert h0 == c0, f"premise: chkItems not equal (host={h0} client={c0})"
        print(f"PASS setup: both bases hold {base0} launchers, empty interceptor slot, "
              f"chkItems={h0} equal")
        _reset_resync(host, client)

        # CLIENT mounts STR_STINGRAY in slot 0 via the REAL CraftWeaponsState path.
        r = client.ok({"cmd": "craft_rearm", "weapon": WEAPON, "slot": 0, "craft_id": craft_id})
        assert r.get("moved"), f"harness could not drive the rearm: {r}"

        hc, cc = _chk(host), _chk(client)
        print(f"[snapshot] host chkItems={hc} client chkItems={cc} "
              f"delta(client-host)={cc - hc}  (unfixed => -1: replica-local store drift)")

        # HOST-AUTHORITY: the host's own base launcher must drop.
        host.wait_for(
            f"host to apply the client's rearm (base launchers {base0} -> {base0 - 1})",
            lambda: (_base_items(host, LAUNCHER) == base0 - 1) or None,
            timeout=30, interval=0.5)
        print(f"PASS host-authority: host base launchers {base0} -> {_base_items(host, LAUNCHER)} "
              "(the rearm went through the protocol)")

        # NO DRIFT: stores + weapon-mount equal, chkItems moved together.
        js.assert_world_equal("after client rearm (host-authoritative, no drift)")
        assert _base_items(client, LAUNCHER) == base0 - 1
        hc2, cc2 = _chk(host), _chk(client)
        assert hc2 == cc2 == h0 - 1, f"chkItems not moved together: host={hc2} client={cc2} exp={h0 - 1}"
        for name, gc in (("host", host), ("client", client)):
            assert _interceptor(gc, craft_id)["weaponLoadout"][0] == WEAPON, \
                f"{name}: weapon not mounted: {_interceptor(gc, craft_id)['weaponLoadout']}"
        print(f"PASS no-drift: both bases at {_base_items(host, LAUNCHER)} launchers, "
              f"STR_STINGRAY mounted on both, chkItems host={hc2} client={cc2}")

        _assert_no_resync(host, client)
        js.finish()
        print("ALL CRAFT-WEAPON REARM TESTS PASSED")
    finally:
        js.shutdown()


# ======================================================================
# 2. SoldierArmorState - change a soldier's armor (armor item leaves the stores).
# ======================================================================
def test_armor(ports):
    js = joint_fixture.bring_up("jsarmor", ports)
    host, client = js.host, js.client
    try:
        for gc in (host, client):
            gc.ok({"cmd": "give_items", "item": ARMOR_ITEM, "count": STOCK})
        js.assert_world_equal("bootstrap + personal-armor stock")

        sid = host.ok({"cmd": "get_soldiers"})["bases"][0]["soldiers"][0]["id"]
        assert _soldier(host, sid)["armor"] == DEFAULT_ARMOR, "premise: soldier not in default armor"
        assert _soldier(client, sid)["armor"] == DEFAULT_ARMOR
        base0 = _base_items(host, ARMOR_ITEM)
        assert _base_items(client, ARMOR_ITEM) == base0
        h0, c0 = _chk(host), _chk(client)
        assert h0 == c0, f"premise: chkItems not equal (host={h0} client={c0})"
        print(f"PASS setup: both bases hold {base0} personal armor, soldier {sid} in "
              f"{DEFAULT_ARMOR}, chkItems={h0} equal")
        _reset_resync(host, client)

        # CLIENT swaps the soldier None -> Personal via the REAL SoldierArmorState path.
        r = client.ok({"cmd": "soldier_armor", "soldier_id": sid, "armor": ARMOR})
        assert r.get("moved"), f"harness could not drive the armor swap: {r}"

        hc, cc = _chk(host), _chk(client)
        print(f"[snapshot] host chkItems={hc} client chkItems={cc} "
              f"delta(client-host)={cc - hc}  (unfixed => -1: replica-local store drift)")

        # HOST-AUTHORITY: the host's own base personal-armor stock must drop, and the
        # host's own soldier must actually change armor.
        host.wait_for(
            f"host to apply the client's armor swap (personal armor {base0} -> {base0 - 1})",
            lambda: (_base_items(host, ARMOR_ITEM) == base0 - 1) or None,
            timeout=30, interval=0.5)
        assert _soldier(host, sid)["armor"] == ARMOR, \
            f"host soldier armor not applied: {_soldier(host, sid)['armor']}"
        print(f"PASS host-authority: host base personal armor {base0} -> "
              f"{_base_items(host, ARMOR_ITEM)}, host soldier now wears {ARMOR}")

        js.assert_world_equal("after client armor swap (host-authoritative, no drift)")
        assert _base_items(client, ARMOR_ITEM) == base0 - 1
        hc2, cc2 = _chk(host), _chk(client)
        assert hc2 == cc2 == h0 - 1, f"chkItems not moved together: host={hc2} client={cc2} exp={h0 - 1}"
        assert _soldier(client, sid)["armor"] == ARMOR
        print(f"PASS no-drift: both bases at {_base_items(host, ARMOR_ITEM)} personal armor, "
              f"soldier wears {ARMOR} on both, chkItems host={hc2} client={cc2}")

        _assert_no_resync(host, client)
        js.finish()
        print("ALL SOLDIER-ARMOR TESTS PASSED")
    finally:
        js.shutdown()


# ======================================================================
# 3. CraftArmorState - bulk de-equip to default (armor item returns to the stores).
# ======================================================================
def test_deequip(ports):
    js = joint_fixture.bring_up("jcarmor", ports)
    host, client = js.host, js.client
    try:
        # Seed (scaffolding, no store change, called on BOTH): soldier 0 wears
        # Personal armor. The de-equip will return that store item to the stores.
        sid = host.ok({"cmd": "get_soldiers"})["bases"][0]["soldiers"][0]["id"]
        for gc in (host, client):
            gc.ok({"cmd": "seed_soldier_armor", "soldier_id": sid, "armor": ARMOR})
        js.assert_world_equal("seed: soldier in personal armor on both")
        assert _soldier(host, sid)["armor"] == ARMOR and _soldier(client, sid)["armor"] == ARMOR

        base0 = _base_items(host, ARMOR_ITEM)
        assert _base_items(client, ARMOR_ITEM) == base0
        h0, c0 = _chk(host), _chk(client)
        assert h0 == c0, f"premise: chkItems not equal (host={h0} client={c0})"
        print(f"PASS setup: soldier {sid} seeded in {ARMOR} on both, base personal-armor "
              f"stock={base0}, chkItems={h0} equal")
        _reset_resync(host, client)

        # CLIENT de-equips ALL base soldiers to default via the REAL CraftArmorState path.
        r = client.ok({"cmd": "craft_deequip_armor"})
        assert r.get("ok"), f"harness could not drive the de-equip: {r}"

        hc, cc = _chk(host), _chk(client)
        print(f"[snapshot] host chkItems={hc} client chkItems={cc} "
              f"delta(client-host)={cc - hc}  (unfixed => +1: replica-local store drift)")

        # HOST-AUTHORITY: the host's own base stock must gain the returned armor, and
        # the host's own soldier must return to default.
        host.wait_for(
            f"host to apply the client's de-equip (personal armor {base0} -> {base0 + 1})",
            lambda: (_base_items(host, ARMOR_ITEM) == base0 + 1) or None,
            timeout=30, interval=0.5)
        assert _soldier(host, sid)["armor"] == DEFAULT_ARMOR, \
            f"host soldier not de-equipped: {_soldier(host, sid)['armor']}"
        print(f"PASS host-authority: host base personal armor {base0} -> "
              f"{_base_items(host, ARMOR_ITEM)}, host soldier back to {DEFAULT_ARMOR}")

        js.assert_world_equal("after client de-equip (host-authoritative, no drift)")
        assert _base_items(client, ARMOR_ITEM) == base0 + 1
        hc2, cc2 = _chk(host), _chk(client)
        assert hc2 == cc2 == h0 + 1, f"chkItems not moved together: host={hc2} client={cc2} exp={h0 + 1}"
        assert _soldier(client, sid)["armor"] == DEFAULT_ARMOR
        print(f"PASS no-drift: both bases at {_base_items(host, ARMOR_ITEM)} personal armor, "
              f"soldier back to {DEFAULT_ARMOR} on both, chkItems host={hc2} client={cc2}")

        _assert_no_resync(host, client)
        js.finish()
        print("ALL CRAFT-ARMOR DE-EQUIP TESTS PASSED")
    finally:
        js.shutdown()


SECTIONS = {
    "weapon":  (test_weapon,  (48840, 48841, 48140)),
    "armor":   (test_armor,   (48842, 48843, 48142)),
    "deequip": (test_deequip, (48844, 48845, 48144)),
}


def main():
    which = sys.argv[1] if len(sys.argv) > 1 else None
    if which:
        fn, ports = SECTIONS[which]
        fn(ports)
    else:
        for name in ("weapon", "armor", "deequip"):
            fn, ports = SECTIONS[name]
            print(f"\n==== GAP-5b section: {name} ====")
            fn(ports)
    print("\nALL JOINT EQUIP2 (GAP-5b) TESTS PASSED")


if __name__ == "__main__":
    main()
