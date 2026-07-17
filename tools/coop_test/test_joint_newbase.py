"""PRD-J07: JOINT subsequent-base creation end-to-end via base_new.

Any player may Build New Base in a JOINT campaign. The CLIENT drives the full
real flow - BuildNewBaseState (globe pick) -> ConfirmNewBaseState (cost gate,
NO local debit) -> BaseNameState (name) -> PlaceLiftState (lift click) - which
submits ONE atomic base_new joint_cmd. The host validates funds + region,
creates the base (minting the coopbaseid it serializes into the broadcast),
places the access lift, debits the region cost ONCE, and appends the base at
the same index on every machine (base add rides joint_apply = index lock-step).

  NEWBASE   client builds a second base end-to-end -> base exists on both
            machines with the same coopbaseid, name, lift position; funds
            debited exactly once (the region base cost).
  LOCKSTEP  a follow-up command routed at the NEW base's index (base_rename
            from the HOST) lands on the same base on both machines.

Run:  python tools/coop_test/test_joint_newbase.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT
import session
import geo

NEW_NAME = "Joint Base Two"
LIFT_X, LIFT_Y = 3, 3


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _funds(gc):
    return _geo(gc)["funds"]


def _bases(gc):
    return [b for b in _geo(gc)["bases"]
            if not b.get("coopBase") and not b.get("coopIcon")]


def _base_named(gc, name):
    for b in _bases(gc):
        if b["name"] == name:
            return b
    return None


def main():
    host_dir = make_user_dir("jnb_host")
    client_dir = make_user_dir("jnb_client")
    host = GameClient("host", 48760, host_dir)
    client = GameClient("client", 48761, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        session.new_campaign(host, client, port="48060", campaign_mode="joint")
        geo.wait_both_ready(host, client)

        assert len(_bases(host)) == 1 and len(_bases(client)) == 1, \
            "expected exactly one starting base on both"
        funds0 = _funds(host)
        assert funds0 == _funds(client), "starting funds differ"
        print(f"PASS setup: 1 shared base on both; funds {funds0}")

        # ================================================================
        # 1) NEWBASE: the CLIENT drives the real place/confirm/name/lift flow.
        # ================================================================
        r = client.ok({"cmd": "build_new_base", "lon": LAND_LON, "lat": LAND_LAT,
                       "name": NEW_NAME, "liftX": LIFT_X, "liftY": LIFT_Y})
        assert r.get("ok"), f"build_new_base flow failed: {r}"
        assert r.get("affordable"), f"region cost not affordable: {r}"
        cost = r["cost"]
        assert cost > 0, f"expected a positive region base cost, got {cost}"

        host.wait_for("host has 2 bases",
                      lambda: (len(_bases(host)) == 2) or None, timeout=30, interval=0.5)
        client.wait_for("client has 2 bases",
                        lambda: (len(_bases(client)) == 2) or None, timeout=30, interval=0.5)

        nb_h = _base_named(host, NEW_NAME)
        nb_c = _base_named(client, NEW_NAME)
        assert nb_h and nb_c, \
            f"new base name mismatch: host={[b['name'] for b in _bases(host)]} " \
            f"client={[b['name'] for b in _bases(client)]}"

        # same index on both machines (the load-bearing invariant)
        idx_h = [b["name"] for b in _bases(host)].index(NEW_NAME)
        idx_c = [b["name"] for b in _bases(client)].index(NEW_NAME)
        assert idx_h == idx_c == 1, f"new base index differs: host={idx_h} client={idx_c}"

        # same coords
        assert abs(nb_h["lon"] - nb_c["lon"]) < 1e-9 and abs(nb_h["lat"] - nb_c["lat"]) < 1e-9, \
            f"new base coords differ: {nb_h['lon']},{nb_h['lat']} vs {nb_c['lon']},{nb_c['lat']}"

        # same coopbaseid (host-minted, serialized into the joint_apply payload)
        rep_h = host.ok({"cmd": "base_report", "base": NEW_NAME})
        rep_c = client.ok({"cmd": "base_report", "base": NEW_NAME})
        assert rep_h["coopBaseId"] == rep_c["coopBaseId"] != 0, \
            f"coopbaseid mismatch: host={rep_h['coopBaseId']} client={rep_c['coopBaseId']}"

        # exactly one facility: the access lift at the clicked grid, built (vanilla
        # subsequent-base lifts are instant)
        for side, nb in (("host", nb_h), ("client", nb_c)):
            facs = nb["facilities"]
            assert len(facs) == 1, f"{side}: expected only the lift, got {facs}"
            lift = facs[0]
            assert lift["x"] == LIFT_X and lift["y"] == LIFT_Y, \
                f"{side}: lift at ({lift['x']},{lift['y']}), want ({LIFT_X},{LIFT_Y})"
            assert lift["buildTime"] == 0, f"{side}: lift not instant: {lift}"

        # funds debited exactly ONCE (region base cost), equal on both
        assert _funds(host) == _funds(client) == funds0 - cost, \
            f"funds after new base: host={_funds(host)} client={_funds(client)} " \
            f"want {funds0 - cost} (= {funds0} - {cost})"
        print(f"PASS newbase: '{NEW_NAME}' on both @ index 1, coopbaseid "
              f"{rep_h['coopBaseId']}, lift ({LIFT_X},{LIFT_Y}) built, "
              f"funds {funds0} -> {_funds(host)} (single {cost} debit)")

        # ================================================================
        # 2) LOCKSTEP: an index-routed command at the NEW base (rename from the
        #    HOST this time) must land on the same base on both machines.
        # ================================================================
        host.ok({"cmd": "base_rename", "base": NEW_NAME, "name": "Second Site"})
        host.wait_for("host renamed new base",
                      lambda: (_base_named(host, "Second Site") is not None) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client renamed new base",
                        lambda: (_base_named(client, "Second Site") is not None) or None,
                        timeout=30, interval=0.5)
        # the FIRST base must be untouched on both (the command routed by index)
        first_h = [b["name"] for b in _bases(host)][0]
        first_c = [b["name"] for b in _bases(client)][0]
        assert first_h == first_c != "Second Site", \
            f"rename leaked to the wrong base: first base host={first_h} client={first_c}"
        print("PASS lockstep: rename routed at index 1 landed on the new base on both")

        # standing invariant: the JOINT replica never wrote save data to disk.
        session.assert_client_zero_disk(client_dir)
        print("PASS zero-disk: client (replica) user dir clean")

        print("ALL JOINT NEWBASE TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
