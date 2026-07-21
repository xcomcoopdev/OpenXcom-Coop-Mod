"""JOINT base-attack replication: the base under attack is the ONE shared base, but only
the host's geoscape sim reaches handleBaseDefense (the replica's sim is frozen), so every
outcome had to be pushed to the clients explicitly.

Three things were missing:

  1. UNDER-ATTACK ALERT. The turret sequence (BaseDefenseState) is host-only, so a client
     was never told its shared base was attacked at all - and if the turrets killed the UFO
     there was no battle to reveal it either. The sequence itself stays host-only on
     purpose (its OK handler runs handleBaseDefense + retaliation RNG, so a replica copy
     would double-apply); the client gets the alert instead.

  2. PARTIAL DESTRUCTION DIVERGED THE WORLD. A missile-armed UFO bombards the base:
     Base::damageFacilities() destroys/replaces facilities and the base survives, with NO
     battle. That ran host-side only, and BaseDestroyedState::btnOkClick returns early for
     a partial destruction before it ever broadcast - so the client's copy of the shared
     base kept the facilities the host had lost. Permanent, silent divergence.
     The roll is RNG-driven, so the host now ships the resulting layout as an absolute
     end-state (base_damaged).

  3. DESTRUCTION MESSAGE. The replica always said "undefended base" even for a missile
     strike; the cause now travels with base_destroyed.

Run:  python tools/coop_test/test_joint_base_attack.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base")


def _facilities(gc):
    """The shared base's facility layout, comparable across machines."""
    return sorted((f["type"], f["x"], f["y"]) for f in _base0(gc)["facilities"])


def _wait(pred, label, timeout=30, interval=0.4):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"{label}: never satisfied (last={last})")


def main():
    js = joint_fixture.bring_up("jbatk", (48944, 48945, 48244))
    host, client = js.host, js.client
    try:
        # Both machines start on the same shared base layout.
        assert _facilities(host) == _facilities(client), "shared base already diverged"
        before = _facilities(host)
        print(f"PASS setup: shared base layout agrees on both ({len(before)} facilities)")

        # ---- 2. PARTIAL DESTRUCTION: host loses facilities, replica must follow. -----
        r = host.ok({"cmd": "host_base_damaged", "count": 2})
        assert r["removed"] > 0, f"nothing was destroyed: {r}"
        host_after = _facilities(host)
        assert host_after != before, "host layout unchanged - test did not damage anything"

        _wait(lambda: _facilities(client) == host_after,
              "client adopted the host's post-damage base layout")
        assert len(host_after) == len(before) - r["removed"], \
            f"unexpected host layout: {len(before)} -> {len(host_after)}"
        print(f"PASS partial destruction: {r['removed']} facilities lost on the host and "
              f"the client's copy of the SHARED base followed "
              f"({len(before)} -> {len(host_after)})")

        # The client is also told, with the "damaged but survived" dialog.
        _wait(lambda: "BaseDestroyedState" in geo.top_state(client) or None,
              "client shown the base-damaged dialog")
        print("PASS partial destruction: client shown the 'base damaged' dialog")
        for gc in (host, client):
            geo.drain_popups(gc)

        # World equality is the real proof: facilities are compared field by field.
        joint_fixture.assert_world_equal(host, client, "after partial base destruction")

        # ---- 1. UNDER-ATTACK ALERT reaches the client. ------------------------------
        b0 = _base0(host)
        for gc in (host, client):
            geo.drain_popups(gc)
        assault = host.ok({"cmd": "spawn_base_assault"})
        print(f"spawned base-assault UFO {assault['ufo_id']} on base {assault['base']!r}")

        # Advance until the host's sim runs the base-attack handler; the client must be
        # alerted even though its own sim never reaches that code.
        got = geo.skip_realtime(host, client, 120, speed_idx=2,
                                interest=geo.popup("CraftErrorState", "BaseDefenseState",
                                                   "BaseDestroyedState", "BriefingState"),
                                stuck_timeout=None)
        assert got["hit"], f"the base assault never resolved on the host: {got}"

        def _client_alerted():
            top = geo.top_state(client)
            return top if any(s in top for s in
                              ("CraftErrorState", "BaseDestroyedState", "BriefingState",
                               "InventoryState", "BattlescapeState")) else None
        top = _wait(_client_alerted,
                    "client learned its shared base was attacked", timeout=40)
        print(f"PASS under-attack alert: client was told about the base assault ({top})")

        print("ALL JOINT BASE-ATTACK TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
