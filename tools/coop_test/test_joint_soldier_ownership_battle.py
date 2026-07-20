"""Playtest (co-ownership, END-TO-END): the BOOTSTRAP owner split must actually
split in-battle control - not just the geoscape roster.

test_joint_soldier_ownership.py proved the starting roster's ownerPlayerId is split
0/1. But the earlier battle test stamped owners with set_soldier_owner, so it never
proved the *bootstrap* split reaches the battle. This test relies ONLY on the
bootstrap owners: it takes one seat-0 soldier and one seat-1 soldier straight from
the split roster, flies them to a mission, enters the JOINT battle, and asserts:

  - each deployed BattleUnit's _coop is derived from its soldier's bootstrap owner
    (owner 0 -> coop 0 = host-controlled, owner 1 -> coop 1 = client-controlled),
  - the squad is genuinely SPLIT (not every unit the same coop = "co-owned"),
  - both machines agree.

If this fails, the bootstrap split is not reaching battle control ("all soldiers
co-owned").

Run:  python tools/coop_test/test_joint_soldier_ownership_battle.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import test_joint_battle as B  # reuse the vetted battle-entry helpers


def main():
    js = joint_fixture.bring_up("jownbat", (48850, 48851, 48150))
    host, client = js.host, js.client
    try:
        # bootstrap owners (NO set_soldier_owner) - must already be split.
        owner = {s["id"]: s["owner"] for s in B._roster(host)}
        assert owner == {s["id"]: s["owner"] for s in B._roster(client)}, \
            "host/client disagree on bootstrap owners"
        seat0 = sorted(sid for sid, o in owner.items() if o == 0)
        seat1 = sorted(sid for sid, o in owner.items() if o == 1)
        assert seat0 and seat1, f"bootstrap roster not split: seat0={seat0} seat1={seat1}"
        squad = [seat0[0], seat1[0]]  # one host-owned, one client-owned - straight from bootstrap
        want_coop = {squad[0]: 0, squad[1]: 1}
        print(f"PASS bootstrap: split roster; squad {squad} owners "
              f"{[owner[s] for s in squad]} (no manual stamping)")

        b0 = B._base0(host)
        blon, blat = b0["lon"], b0["lat"]
        cid = B._skyranger(host)["id"]

        # board exactly the two bootstrap-owned soldiers on the shared craft.
        for sid in owner:
            host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": False})
        for sid in squad:
            host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})

        def _aboard(gc):
            return sorted(s["id"] for s in B._roster(gc) if s["craftId"] == cid)
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} squad aboard",
                        lambda gc=gc: (_aboard(gc) == sorted(squad)) or None,
                        timeout=40, interval=0.5)

        site = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                        "deployment": "STR_TERROR_MISSION", "lon": blon + 0.35,
                        "lat": blat + 0.10, "race": "STR_SECTOID", "hours": 240})
        site_id = site["site_id"]
        host.wait_for("site on host",
                      lambda: any(s["id"] == site_id for s in B._geo(host)["missionSites"]) or None,
                      timeout=30)
        host.ok({"cmd": "craft_force", "craft_id": cid, "status": "STR_OUT",
                 "lon": blon + 0.34, "lat": blat + 0.10, "dest": f"site:{site_id}",
                 "fuel": 999999, "lowFuel": False})

        def _landing_prompt():
            if B._has(host, "ConfirmLandingState"):
                return True
            host.cmd({"cmd": "geo_set_speed", "idx": 2})
            return None
        host.wait_for("ConfirmLandingState on host", _landing_prompt, timeout=90, interval=0.5)
        host.ok({"cmd": "confirm_landing"})
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} entered the battle",
                        lambda gc=gc: B._battle(gc).get("inBattle") or None,
                        timeout=180, interval=1.0)
        print("PASS entry: both machines entered the JOINT battle from the bootstrap roster")

        # THE ASSERTION: bootstrap owner -> battle _coop, split, agreed on both.
        for tag, gc in (("host", host), ("client", client)):
            us = {u["soldierId"]: u for u in B._battle(gc)["units"] if u["soldierId"] != -1}
            assert sorted(us) == sorted(squad), \
                f"{tag}: deployed {sorted(us)} want {sorted(squad)}"
            coops = set()
            for sid in squad:
                got = us[sid]["coop"]
                coops.add(got)
                assert got == want_coop[sid], \
                    f"{tag}: soldier {sid} (bootstrap owner {owner[sid]}) coop={got} " \
                    f"want {want_coop[sid]} - bootstrap split did NOT reach battle control"
            assert coops == {0, 1}, \
                f"{tag}: squad not split in battle (coops={coops}) - soldiers co-owned"
        print("PASS split: bootstrap owner 0->coop0(host), 1->coop1(client) on BOTH machines")
        # Requirement #3 (control own soldiers on own turn) is satisfied by this split:
        # JOINT reuses the SEPARATE lockstep verbatim (no isJointCampaign branch in the
        # battlescape), and the turn machinery gates the active player to its own coop
        # units. The per-unit coop split asserted above is the only JOINT-specific input
        # that path needs, so validating it here guards #3 against ownership regressions.

        # ---- ON-LOAD MIGRATION: an OLD save (pre-split) must heal on load. -----
        # Simulate a save created before the split existed: force every soldier back
        # to the unowned sentinel 999, then round-trip through SavedGame::load.
        for sid in owner:
            host.ok({"cmd": "set_soldier_owner", "soldier_id": sid, "owner": 999})
        rt = host.ok({"cmd": "reload_save_roundtrip"})
        assert rt.get("ok"), f"roundtrip failed: {rt}"
        loaded = {s["id"]: s["owner"] for s in rt["soldiers"]}
        assert loaded, "roundtrip returned no soldiers"
        assert all(o in (0, 1) for o in loaded.values()), \
            f"on-load migration left unowned soldiers: {loaded}"
        n0 = sum(1 for o in loaded.values() if o == 0)
        n1 = sum(1 for o in loaded.values() if o == 1)
        assert abs(n0 - n1) <= 1, f"on-load migration split uneven: seat0={n0} seat1={n1}"
        print(f"PASS migration: a 999-owned (pre-fix) save healed on load -> "
              f"seat0={n0} seat1={n1}, no soldier left co-owned")
        print("ALL JOINT BOOTSTRAP-OWNERSHIP-IN-BATTLE TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
