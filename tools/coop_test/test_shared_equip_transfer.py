"""PRD-J05 smoke test: equipping and moving soldiers around a SHARED world.

The SEPARATE suite covers this ground thoroughly (test_coop_peer_equip_screens,
test_coop_transferred_equipment, test_coop_guest_craft_seat); SHARED had no
coverage of it at all, so this is the equivalent walk-through in one shared
world:

  1. equip a soldier by REALLY dragging items onto it in the base inventory
     (the path that writes the craft's coopItems manifest), and check that the
     soldier equip screen and the craft equip screen agree about it;
  2. seat it on a craft, take it off again - the seat must stick;
  3. transfer it to the OTHER player's base through the intra-world "transfer"
     shared_cmd, let the transfer arrive, and check it lands unassigned from any
     craft, at the right base, identically on both machines;
  4. check both equip screens agree at the destination base too;
  5. world equality + the replica's zero-disk invariant (shared_fixture.finish).

SHARED ownership note: a transfer moves a soldier between bases, it does NOT
change its owner (that is what a GIFT does), so the transferred soldier stays
owned by the sending seat even though it now lives at the other player's base.
The shared roster is compared on both machines, which asserts exactly that.

Run:  python tools/coop_test/test_shared_equip_transfer.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import geo
from harness import LAND_LON, LAND_LAT
import test_coop_transferred_equipment as T
import test_coop_peer_equip_screens as P

GEAR = [("STR_RIFLE", "right", 1), ("STR_PISTOL", "left", 1), ("STR_SMALL_ROCKET", "backpack", 2)]
# A SHARED campaign starts with ONE shared base, so the second base a
# base->base transfer needs has to be built first (PRD-J07).
DEST_NAME = "Shared Dest"
LIFT_X, LIFT_Y = 3, 3


def _has(gc, n):
    return any(n in s for s in gc.cmd({"cmd": "get_state"})["states"])


# CoopState codes whose back/OK button tears the session down rather than just
# popping the dialog (see CoopState::previous) - never click those.
_TEARDOWN_CODES = {1, 3, 4, 15, 50, 52, 53, 88, 979}


def run_clock(host, client, seconds, until=None):
    """Advance both geoscapes, clearing whatever pops up on either side.

    geo.skip_ingame_time cannot do this on its own: its drain gives up on a
    CoopState (dismiss_popup refuses them as "wait" dialogs) and its watchdog
    then kills the run. Here a CoopState with a visible back button is simply
    OK'd, except for the codes that would disconnect.
    """
    import time as _t
    deadline = _t.time() + seconds
    while _t.time() < deadline:
        if until is not None and until():
            return True
        for gc in (host, client):
            top = gc.cmd({"cmd": "get_state"})["states"][-1]
            if "GeoscapeState" in top:
                gc.cmd({"cmd": "geo_set_speed", "idx": 5})
            elif "CoopState" in top:
                info = gc.cmd({"cmd": "coop_dialog_info"})
                if info.get("present") and info.get("backVisible") \
                        and info.get("code") not in _TEARDOWN_CODES:
                    gc.cmd({"cmd": "coop_dialog_back"})
            else:
                gc.cmd({"cmd": "dismiss_popup"})
        _t.sleep(0.5)
    return until() if until is not None else True


def base_named(gc, name):
    return gc.ok({"cmd": "base_report", "base": name})


def soldier_row(gc, base_name, who):
    r = base_named(gc, base_name)
    return next((s for s in r["soldiers"] if who in s["name"]), None)


def find_soldier_anywhere(gc, who):
    """(base name, row) for a soldier, wherever it is in the shared world."""
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        r = base_named(gc, b["name"])
        for s in r["soldiers"]:
            if who in s["name"]:
                return b["name"], s
    return None, None


def main():
    js = shared_fixture.bring_up("jxfer", (48690, 48691, 47990))
    host, client = js.host, js.client
    try:
        for gc in (host, client):
            gc.ok({"cmd": "set_option", "name": P.OPT, "value": True})

        hb, cb = "HostBase", DEST_NAME

        # a SHARED campaign starts with ONE shared base; build the destination
        nb = client.ok({"cmd": "build_new_base", "lon": LAND_LON, "lat": LAND_LAT,
                        "name": DEST_NAME, "liftX": LIFT_X, "liftY": LIFT_Y})
        assert nb.get("ok"), f"build_new_base failed: {nb}"
        for gc in (host, client):
            gc.wait_for("second shared base",
                        lambda g=gc: (len(g.ok({"cmd": "get_soldiers"})["bases"]) == 2) or None,
                        timeout=45, interval=0.5)
        print(f"second shared base '{DEST_NAME}' exists on both machines")

        r = base_named(host, hb)
        craft_h = next(c for c in r["crafts"] if c["type"] == "STR_SKYRANGER")["id"]
        # Pick a soldier this seat OWNS - a SHARED roster is mixed-owner and the
        # shared commands are owner-gated (TransferItemsState only offers rows
        # that pass SharedEcon::ownsSoldier).
        seat = host.ok({"cmd": "get_coop"})["localSeat"]
        target = next(s for s in r["soldiers"] if s["owner"] == seat)
        # every starting soldier is seated on the Skyranger; free this one, or it
        # is not a transferable row at all (TransferItemsState wants getCraft()==0)
        if target["craft"] != -1:
            host.ok({"cmd": "craft_assign", "soldier_id": target["id"],
                     "craft_id": craft_h, "base": hb, "on": False})
            host.wait_for(
                "target unseated (shared apply)",
                lambda: (soldier_row(host, hb, target["name"])["craft"] == -1) or None,
                timeout=30)
        host.ok({"cmd": "rename_soldier", "name": target["name"], "newName": "Zzz Shared"})
        print(f"host seat={seat}, target='{target['name']}' owner={target['owner']}")
        for item, slot, qty in GEAR:
            host.ok({"cmd": "give_layout", "item": item, "slot": slot,
                     "name": "Zzz", "count": 1, "qty": qty})

        # 1. really drag gear onto the local crew (populates the coopItems manifest)
        moved = P.equip_by_hand(host, hb)
        print(f"host equipped {moved} items by hand at {hb}")

        # 1b. both equip screens must agree about the target at its home base
        def seat_home():
            host.ok({"cmd": "craft_assign", "soldier_id": target["id"],
                     "craft_id": craft_h, "base": hb, "on": True})
            host.wait_for("target seated (shared apply)",
                          lambda: (soldier_row(host, hb, "Zzz")["craft"] == craft_h) or None,
                          timeout=30)
        seat_home()
        b, c = P.read_both_screens(host, hb, craft_h, seat_home)
        P.assert_screens_agree(b, c, "Zzz", "SHARED home base")

        # 2. the seat must stick when taken off
        host.ok({"cmd": "craft_assign", "soldier_id": target["id"],
                 "craft_id": craft_h, "base": hb, "on": False})
        host.wait_for("target unseated (shared apply)",
                      lambda: (soldier_row(host, hb, "Zzz")["craft"] == -1) or None, timeout=30)
        row = soldier_row(host, hb, "Zzz")
        assert row["craft"] == -1 and row["coopCraft"] == -1, (
            f"SHARED: unseating left a stale craft seat: craft={row['craft']} "
            f"coopCraft={row['coopCraft']}")
        print("PASS SHARED unseat sticks (craft=-1, coopCraft=-1)")
        js.assert_world_equal("after equip + unseat")

        # 3. transfer to the other player's base through the shared_cmd
        # 3a. the destination is a brand-new base: an access lift and NOTHING
        #     else, so it has ZERO living quarters. The move must be refused -
        #     the base that houses a soldier has to be able to house it.
        host.ok({"cmd": "shared_reset_stats"})
        host.ok({"cmd": "transfer_to_coop_base", "name": "Zzz", "toBase": cb})
        host.wait_for(
            "the host rejected the transfer (no living quarters)",
            lambda: (host.ok({"cmd": "shared_stats"})["failCount"] > 0) or None, timeout=30)
        why = host.ok({"cmd": "shared_stats"})["lastFail"]
        assert why == "STR_NO_FREE_ACCOMODATION", (
            f"expected the transfer refused for want of living quarters, got {why!r}")
        assert find_soldier_anywhere(host, "Zzz")[0] == hb, \
            "the soldier moved even though the destination cannot house it"
        print(f"PASS refused: {cb} has no living quarters ({why})")

        # give the destination somewhere to sleep, then try again
        for gc in (host, client):
            gc.ok({"cmd": "fac_build", "facility": "STR_LIVING_QUARTERS",
                   "base": cb, "x": 4, "y": 3})
        for gc in (host, client):
            gc.wait_for("living quarters queued",
                        lambda g=gc: (len(base_named(g, cb)["facilities"]) > 1
                                      if "facilities" in base_named(g, cb) else True) or None,
                        timeout=30)
            # finish it instantly on BOTH machines, or the shared world diverges
            gc.ok({"cmd": "set_facility_build_time", "baseId": 1, "index": 1, "time": 0})
        host.wait_for("destination has living quarters",
                      lambda: (base_named(host, cb)["availableQuarters"] > 0) or None, timeout=30)
        print(f"built living quarters at {cb} "
              f"(available={base_named(host, cb)['availableQuarters']})")

        src0 = base_named(host, hb)
        dst0 = base_named(host, cb)
        tr = host.ok({"cmd": "transfer_to_coop_base", "name": "Zzz", "toBase": cb})
        assert tr.get("transferred"), f"SHARED transfer failed: {tr}"
        print(f"submitted the intra-world transfer {hb} -> {cb}")

        # 3a. LIVING QUARTERS: the base that houses the soldier pays, and the
        #     destination reserves the space the moment the transfer is sent
        #     (getTotalSoldiers counts personnel en route). So the source must
        #     drop by one and the destination rise by one straight away, on both
        #     machines - long before the transfer actually lands.
        for gc in (host, client):
            gc.wait_for(
                f"{gc.name}: quarters moved to the destination",
                lambda g=gc: (base_named(g, cb)["usedQuarters"] == dst0["usedQuarters"] + 1
                              and base_named(g, hb)["usedQuarters"] == src0["usedQuarters"] - 1) or None,
                timeout=45)
            print(f"PASS {gc.name}: quarters {hb} {src0['usedQuarters']}->"
                  f"{base_named(gc, hb)['usedQuarters']}, {cb} {dst0['usedQuarters']}->"
                  f"{base_named(gc, cb)['usedQuarters']} (charged on send)")

        # a SHARED transfer is TIMED (6h + distance); run the clock until it lands
        def arrived(gc):
            base, s = find_soldier_anywhere(gc, "Zzz")
            return (base == cb) or None
        landed = run_clock(host, client, 180,
                           until=lambda: arrived(host) and arrived(client))
        assert landed, f"the transfer never arrived at {cb} on both machines"

        # ... and must land unassigned, at the right base, on BOTH machines
        for gc in (host, client):
            base, s = find_soldier_anywhere(gc, "Zzz")
            assert base == cb, f"{gc.name}: soldier landed at {base}, expected {cb}"
            assert s["craft"] == -1, (
                f"{gc.name}: the transferred soldier arrived still assigned to craft "
                f"{s['craft']} - it must arrive with no craft")
            assert s["coopCraft"] == -1, (
                f"{gc.name}: the transferred soldier arrived with a stale persisted "
                f"craft seat (coopCraft={s['coopCraft']})")
            print(f"PASS {gc.name}: arrived at {cb}, craft=-1, owner={s['owner']}")

        # a transfer preserves ownership - only a gift changes it
        h_owner = find_soldier_anywhere(host, "Zzz")[1]["owner"]
        c_owner = find_soldier_anywhere(client, "Zzz")[1]["owner"]
        assert h_owner == c_owner, (
            f"the two machines disagree on the transferred soldier's owner: "
            f"host={h_owner} client={c_owner}")
        print(f"PASS transfer preserved ownership (owner={h_owner} on both machines)")

        # 4. the arrival's equipment LAYOUT must survive both the move and being
        #    looked at. A freshly built base has no stores, so the physical items
        #    are not there to re-equip - and InventoryState::saveEquipmentLayout
        #    rebuilds a soldier's layout from what it is actually wearing when the
        #    screen closes, so merely OPENING the equip screen there would wipe
        #    every layout entry that could not be materialised.
        want_layout = sorted(i for i, _, q in GEAR for _ in range(q))
        before = sorted(soldier_row(host, cb, "Zzz")["layout"])
        assert before == want_layout, (
            f"SHARED: the transfer dropped the soldier's equipment layout - "
            f"expected {want_layout}, got {before}")
        print(f"PASS SHARED transfer kept the layout: {before}")

        # the gear must have ridden the SAME transfer, so it is already here
        for gc in (host, client):
            store = base_named(gc, cb)["storage"]
            missing = {i: (q, store.get(i, 0)) for i, _s, q in GEAR if store.get(i, 0) < q}
            assert not missing, (
                f"{gc.name}: the soldier's gear did not travel with it - {cb} is "
                f"short {missing} (item: (wanted, got))")
        print(f"PASS the gear rode the soldier's own transfer to {cb}")

        g = T.base_equip_screen(host, cb)
        lo = T.items_only(T.loadout_of(g, "Zzz"))
        assert lo == P.expected_layout(), (
            f"SHARED: the arrival is not wearing its loadout at {cb} - expected "
            f"{P.expected_layout()}, got {lo} (ground pane: {g['items']})")
        after = sorted(soldier_row(host, cb, "Zzz")["layout"])
        assert after == before, (
            f"SHARED: opening the equip screen at {cb} changed the soldier's "
            f"layout ({before} -> {after})")
        print(f"PASS SHARED destination equip screen: wearing {lo}, layout intact")

        js.finish()
        print("TEST PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
