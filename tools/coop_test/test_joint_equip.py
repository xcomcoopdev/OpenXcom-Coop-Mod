"""PRD-J09 GAP-5: a client equipping a craft at the BASE screen must be
host-authoritative, so base stores never drift from the host.

J09 reused the battle Inventory sync and explicitly deferred the pre-battle
base-screen equip: a replica loading items onto a craft from base stores runs
CraftEquipmentState::moveRightByValue unconditionally, which does
`_base->getStorageItems()->removeItem(item, change)` on the replica with no
joint_cmd. Base storage total is exactly what the GAP-4 checksum sums
(chkItems = every base's ItemContainer::getTotalQuantity(); the craft's own
item container is NOT counted), so moving items base->craft drops chkItems on
the machine that did it. On a replica that means silent drift from the host -
which, now that GAP-4 makes it visible, trips a recurring auto-resync (which
reverts the client's equip) instead of persisting.

THE FIX routes the base-screen equip through a new host-authoritative
`craft_equip` joint_cmd (like buy/sell/transfer/craft_assign): the client
mutates nothing locally, the host validates + applies + broadcasts, and both
worlds move together.

  HOST-AUTHORITY  the client loads rifles onto its craft through the REAL
                  CraftEquipmentState path (the `craft_equip` harness driver).
                  Only a joint_cmd can move the HOST's own base stores; a
                  replica-local mutation never touches the host. So the host's
                  base rifle count dropping is proof the equip went through the
                  protocol. Before the fix the host never applies it.
  NO DRIFT        base stores stay identical on both machines (world equality +
                  chkItems), and NO auto-resync is needed to get there - the
                  drift never existed. Before the fix the replica-local mutation
                  makes chkItems diverge and trips the GAP-4 auto-repair.

Red/green: on the UNFIXED binary the immediate snapshot shows client
chkItems = host - moved (the drift), the host never applies the equip (the
host-authority poll times out), and the auto-resync fires. After the fix the
host applies it, stores stay equal, and no resync fires.

Run:  python tools/coop_test/test_joint_equip.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture

RIFLE = "STR_RIFLE"
STOCK = 12   # rifles handed to BOTH bases so STR_RIFLE is in stores + on the list
LOAD = 4     # rifles the client moves base -> craft (small enough to fit any craft)


def _chk(gc):
    return gc.ok({"cmd": "joint_checksum"})


def _resync(gc):
    return gc.ok({"cmd": "joint_resync_stats"})


def _base_rifles(gc):
    return gc.ok({"cmd": "geo_state"})["bases"][0]["items"].get(RIFLE, 0)


def main():
    js = joint_fixture.bring_up("jequip", (48820, 48821, 48120))
    host, client = js.host, js.client
    try:
        # Give both machines the same rifles so STR_RIFLE is stocked (and thus on
        # the craft equipment list); the shared world stays equal.
        for gc in (host, client):
            gc.ok({"cmd": "give_items", "item": RIFLE, "count": STOCK})
        js.assert_world_equal("bootstrap + equal rifles")

        base0 = _base_rifles(host)
        assert _base_rifles(client) == base0, "premise: base rifle stock not equal"
        h0, c0 = _chk(host), _chk(client)
        assert h0["chkItems"] == c0["chkItems"], (
            f"premise: chkItems not equal at start (host={h0['chkItems']} "
            f"client={c0['chkItems']})")
        print(f"PASS setup: both bases hold {base0} rifles, chkItems={h0['chkItems']} "
              "equal, world equal")

        host.ok({"cmd": "joint_reset_resync_stats"})
        client.ok({"cmd": "joint_reset_resync_stats"})

        # ================================================================
        # CLIENT loads LOAD rifles onto its craft through the real equip screen.
        # ================================================================
        r = client.ok({"cmd": "craft_equip", "item": RIFLE, "count": LOAD})
        assert r.get("moved"), f"harness could not drive the equip: {r}"

        # Immediate snapshot - where the UNFIXED replica-local drift shows up
        # (client base dropped by LOAD, host untouched). Printed for the record;
        # not asserted, because in the FIXED path an in-flight joint_apply is a
        # legitimate transient skew.
        hc, cc = _chk(host), _chk(client)
        print(f"[snapshot] host chkItems={hc['chkItems']} client chkItems={cc['chkItems']} "
              f"delta(client-host)={cc['chkItems'] - hc['chkItems']}  "
              f"(unfixed => -{LOAD}: replica-local store drift)")

        # ---- HOST-AUTHORITY: the host's OWN base stores must move. Only a
        #      joint_cmd can do that; a replica-local mutation never reaches the
        #      host. On the unfixed binary the host never applies -> this times
        #      out (the RED capture).
        host.wait_for(
            f"host to apply the client's equip (base rifles {base0} -> {base0 - LOAD})",
            lambda: (_base_rifles(host) == base0 - LOAD) or None,
            timeout=30, interval=0.5)
        print(f"PASS host-authority: host base rifles {base0} -> {_base_rifles(host)} "
              "(the equip went through the protocol, not a replica-local mutation)")

        # ---- NO DRIFT: both worlds equal, and chkItems moved together.
        js.assert_world_equal("after client equip (host-authoritative, no drift)")
        assert _base_rifles(client) == base0 - LOAD, (
            f"client base stores did not converge: {_base_rifles(client)} "
            f"!= {base0 - LOAD}")
        hc2, cc2 = _chk(host), _chk(client)
        assert hc2["chkItems"] == cc2["chkItems"] == h0["chkItems"] - LOAD, (
            f"chkItems did not move together by LOAD: host={hc2['chkItems']} "
            f"client={cc2['chkItems']} expected={h0['chkItems'] - LOAD}")
        print(f"PASS no-drift: both bases at {_base_rifles(host)} rifles, "
              f"chkItems host={hc2['chkItems']} client={cc2['chkItems']} (moved together)")

        # ---- NO SPURIOUS RESYNC: the equip never persistently drifted, so the
        #      GAP-4 auto-repair must never have ESCALATED to a resync. A single
        #      transient checksum mismatch is legitimate and expected here - the
        #      host stamps its post-apply heartbeat a few ms before the client's
        #      joint_apply lands, an in-flight skew the 3s debounce absorbs (the
        #      same skew assert_world_equal polls through). What must stay ZERO is
        #      `requests` (an actual resync). On the unfixed binary the drift is
        #      PERSISTENT: it survives the debounce and escalates to a request +
        #      a full world re-stream that reverts the client's equip.
        rc = _resync(client)
        assert rc["requests"] == 0 and not rc["gaveUp"], (
            f"GAP-5: the equip drifted and tripped the auto-resync instead of "
            f"being host-authoritative: client resync stats={rc}")
        rh = _resync(host)
        assert rh["requests"] == 0, f"host served an unexpected resync: {rh}"
        print(f"PASS no-resync: client escalated requests={rc['requests']} "
              f"(transient mismatches={rc['mismatches']} absorbed by the debounce), "
              f"host served={rh['requests']}")

        # PRD-J11 shared final-state assertions (world equality + zero-disk).
        js.finish()
        print("ALL JOINT EQUIP TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
