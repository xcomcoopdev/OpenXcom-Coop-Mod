"""PRD-J06: JOINT manufacture start / allocate / cancel via joint_cmd.

A JOINT campaign is one host-authoritative world. The manufacture screens mutate
NOTHING locally: starting a production (first-unit funds + material debit),
(re)allocating engineers/qty, and cancelling each ride the J03 joint_cmd
protocol. The host validates against the live world, applies, and broadcasts
joint_apply (carrying the new authoritative funds); replicas apply ONLY from
joint_apply. Completion is host-driven (J04 prod_done).

  START     client opens ManufactureInfoState and starts a production (REAL
            screen -> man_start) -> both sides debit the first unit's funds
            identically, production appears with the same engineers/qty.
  MATERIAL  start a production with required items -> funds AND materials debit
            identically on both.
  ALLOC     host re-allocates engineers -> both converge (absolute value).
  CANCEL    client cancels a production -> both free the engineers; funds +
            materials outcome identical (vanilla laser/armor do not refund).
  DAYS-LEFT host advances a day -> the replica's production _timeSpent tracks the
            host's (day_tick), so its "days left" column renders current.
  DONE      host advances to completion -> the produced item is delivered on BOTH
            machines and engineers are freed (J04 prod_done path).

Run:  python tools/coop_test/test_joint_manufacture.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo

LASER = "STR_LASER_PISTOL"      # manufacture (requires research STR_LASER_PISTOL)
ARMOR = "STR_PERSONAL_ARMOR"    # manufacture (requires research STR_PERSONAL_ARMOR)
ALLOYS = "STR_ALIEN_ALLOYS"     # STR_PERSONAL_ARMOR requiredItems: 4
LASER_COST = 8000               # STR_LASER_PISTOL manufacture cost (first unit)
ARMOR_COST = 22000              # STR_PERSONAL_ARMOR manufacture cost (first unit)
ARMOR_ALLOYS = 4                # STR_PERSONAL_ARMOR requiredItems STR_ALIEN_ALLOYS


def _base0(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _funds(gc):
    return gc.ok({"cmd": "geo_state"})["funds"]


def _free_eng(gc):
    return _base0(gc)["freeEngineers"]


def _prod(gc, item):
    for p in _base0(gc)["productions"]:
        if p["item"] == item:
            return p
    return None


def _storage(gc, item):
    st = gc.ok({"cmd": "base_report"})["storage"]
    return st.get(item, 0)


def _discover_both(host, client, topic):
    for gc in (host, client):
        gc.ok({"cmd": "discover_research", "topic": topic})


def _give_both(host, client, item, count):
    for gc in (host, client):
        gc.ok({"cmd": "give_items", "item": item, "count": count})


def main():
    host_dir = make_user_dir("jman_host")
    client_dir = make_user_dir("jman_client")
    host = GameClient("host", 48730, host_dir)
    client = GameClient("client", 48731, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        session.new_campaign(host, client, port="48030", campaign_mode="joint")
        geo.wait_both_ready(host, client)

        # Unlock the two manufactures on BOTH machines (deterministic; keeps the
        # shared world equal), and stock alloys for the material-debit test.
        _discover_both(host, client, LASER)
        _discover_both(host, client, ARMOR)
        _give_both(host, client, ALLOYS, 10)

        eng0 = _free_eng(host)
        assert eng0 == _free_eng(client), "starting engineer pool differs"
        assert eng0 >= 8, f"expected the vanilla starting workshop (>=8 eng), got {eng0}"
        assert _storage(host, ALLOYS) == _storage(client, ALLOYS) == 10, "alloy stock differs"
        print(f"PASS setup: {eng0} free engineers, 10 {ALLOYS} on both; {LASER}+{ARMOR} unlocked")

        # ================================================================
        # 1) START (funds debit): client drives the REAL ManufactureInfoState
        #    -> man_start. First-unit funds debit identical on both.
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        f0_h, f0_c = _funds(host), _funds(client)
        assert f0_h == f0_c, "pre-start funds differ"
        r = client.ok({"cmd": "manufacture_start", "item": LASER, "engineers": 5, "qty": 1})
        assert r.get("sent"), f"client manufacture_start not sent: {r}"
        host.wait_for("host started production",
                      lambda: (_prod(host, LASER) and _prod(host, LASER)["engineers"] == 5) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client started production",
                        lambda: (_prod(client, LASER) and _prod(client, LASER)["engineers"] == 5) or None,
                        timeout=30, interval=0.5)
        assert _funds(host) == _funds(client) == f0_h - LASER_COST, \
            f"first-unit funds debit mismatch: host={_funds(host)} client={_funds(client)}"
        assert _free_eng(host) == _free_eng(client) == eng0 - 5, "engineer pool mismatch after start"
        print(f"PASS start: {LASER} @ 5 eng on both; funds {f0_h} -> {_funds(host)} (-{LASER_COST})")

        # ================================================================
        # 2) MATERIAL debit: start a production with required items -> funds AND
        #    materials debit identically on both.
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        fa_h = _funds(host)
        al_h = _storage(host, ALLOYS)
        client.ok({"cmd": "joint_cmd", "jcmd": "man_start", "baseId": 0,
                   "payload": {"item": ARMOR, "engineers": 1, "qty": 1,
                               "infinite": False, "sell": False, "fallback": False}})
        host.wait_for("host started armor",
                      lambda: (_prod(host, ARMOR) is not None) or None, timeout=30, interval=0.5)
        client.wait_for("client started armor",
                        lambda: (_prod(client, ARMOR) is not None) or None, timeout=30, interval=0.5)
        assert _funds(host) == _funds(client) == fa_h - ARMOR_COST, \
            f"armor funds debit mismatch: host={_funds(host)} client={_funds(client)}"
        assert _storage(host, ALLOYS) == _storage(client, ALLOYS) == al_h - ARMOR_ALLOYS, \
            f"armor material debit mismatch: host={_storage(host, ALLOYS)} client={_storage(client, ALLOYS)}"
        print(f"PASS material: {ARMOR} debited funds -{ARMOR_COST} + {ARMOR_ALLOYS} {ALLOYS} on both")

        # ================================================================
        # 3) ALLOC converge: host re-allocates the laser's engineers (absolute).
        # ================================================================
        host.ok({"cmd": "joint_cmd", "jcmd": "man_alloc", "baseId": 0,
                 "payload": {"item": LASER, "engineers": 8, "qty": 1,
                             "infinite": False, "sell": False, "fallback": False}})
        host.wait_for("host laser realloc",
                      lambda: (_prod(host, LASER)["engineers"] == 8) or None, timeout=30, interval=0.5)
        client.wait_for("client laser realloc",
                        lambda: (_prod(client, LASER)["engineers"] == 8) or None, timeout=30, interval=0.5)
        assert _free_eng(host) == _free_eng(client), \
            f"engineer pool diverged after realloc: host={_free_eng(host)} client={_free_eng(client)}"
        print(f"PASS alloc: {LASER} @ 8 eng on both; free engineers {_free_eng(host)}")

        # ================================================================
        # 4) CANCEL: client cancels the armor -> both free the engineer; funds +
        #    materials identical (vanilla armor does not refund).
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        eng_before = _free_eng(host)
        f_before, al_before = _funds(host), _storage(host, ALLOYS)
        client.ok({"cmd": "joint_cmd", "jcmd": "man_cancel", "baseId": 0,
                   "payload": {"item": ARMOR, "refund": False}})
        host.wait_for("host cancelled armor",
                      lambda: (_prod(host, ARMOR) is None) or None, timeout=30, interval=0.5)
        client.wait_for("client cancelled armor",
                        lambda: (_prod(client, ARMOR) is None) or None, timeout=30, interval=0.5)
        assert _free_eng(host) == _free_eng(client) == eng_before + 1, \
            f"cancel did not free the engineer equally: host={_free_eng(host)} client={_free_eng(client)}"
        assert _funds(host) == _funds(client) == f_before, "funds changed on a non-refund cancel"
        assert _storage(host, ALLOYS) == _storage(client, ALLOYS) == al_before, \
            "materials changed on a non-refund cancel"
        print(f"PASS cancel: {ARMOR} gone on both; engineer freed ({eng_before} -> {_free_eng(host)}), funds/materials identical")

        # ================================================================
        # 5) DAYS-LEFT sync: a JOINT replica's timeXxx handlers are frozen, so a
        #    production's _timeSpent never advances locally. Park the laser at 0
        #    engineers (so a coarse speed-5 advance can never complete it) and seed
        #    a known _timeSpent on the HOST; crossing a day boundary must carry that
        #    value to the replica via day_tick (its own step() never runs).
        # ================================================================
        host.ok({"cmd": "joint_cmd", "jcmd": "man_alloc", "baseId": 0,
                 "payload": {"item": LASER, "engineers": 0, "qty": 1,
                             "infinite": False, "sell": False, "fallback": False}})
        host.wait_for("host laser to 0 eng",
                      lambda: (_prod(host, LASER) and _prod(host, LASER)["engineers"] == 0) or None,
                      timeout=30, interval=0.5)
        r = host.ok({"cmd": "set_production_progress", "item": LASER, "timeSpent": 150})
        assert r.get("found"), f"could not seed production progress on host: {r}"
        assert _prod(client, LASER)["timeSpent"] == 0, "replica progress leaked before day_tick"
        geo.skip_ingame_time(host, client, minutes=60 * 24, speed_idx=5, real_timeout=90)
        client.wait_for("client laser progress synced",
                        lambda: (_prod(client, LASER) and _prod(client, LASER)["timeSpent"] == 150) or None,
                        timeout=30, interval=0.5)
        assert _prod(host, LASER) is not None and _prod(client, LASER) is not None, \
            "production vanished during the days-left check"
        assert _prod(host, LASER)["timeSpent"] == 150 and _prod(client, LASER)["timeSpent"] == 150, \
            f"day_tick did not carry _timeSpent: host={_prod(host, LASER)['timeSpent']} client={_prod(client, LASER)['timeSpent']}"
        print("PASS days-left: replica production _timeSpent 0 -> 150 via day_tick (host-frozen replica)")

        # ================================================================
        # 6) COMPLETION: 1 engineer + seed _timeSpent one hour short on the HOST,
        #    then advance -> the host completes on the very next hourly step and
        #    broadcasts prod_done; the item is delivered on BOTH machines and the
        #    engineers are freed (J04 path, on a JOINT-started production).
        # ================================================================
        host.ok({"cmd": "joint_cmd", "jcmd": "man_alloc", "baseId": 0,
                 "payload": {"item": LASER, "engineers": 1, "qty": 1,
                             "infinite": False, "sell": False, "fallback": False}})
        host.wait_for("host laser to 1 eng",
                      lambda: (_prod(host, LASER) and _prod(host, LASER)["engineers"] == 1) or None,
                      timeout=30, interval=0.5)
        r = host.ok({"cmd": "set_production_progress", "item": LASER, "timeSpent": 299})
        assert r.get("found"), f"could not seed production progress on host: {r}"
        laser0_h, laser0_c = _storage(host, LASER), _storage(client, LASER)
        geo.skip_ingame_time(host, client, minutes=60 * 24, speed_idx=5, real_timeout=90)
        host.wait_for("host laser produced",
                      lambda: (_storage(host, LASER) == laser0_h + 1) or None, timeout=30, interval=0.5)
        client.wait_for("client laser produced",
                        lambda: (_storage(client, LASER) == laser0_c + 1) or None, timeout=30, interval=0.5)
        assert _prod(host, LASER) is None and _prod(client, LASER) is None, \
            "completed production still present"
        assert _storage(host, LASER) == _storage(client, LASER) == laser0_h + 1, \
            f"produced item count mismatch: host={_storage(host, LASER)} client={_storage(client, LASER)}"
        assert _free_eng(host) == _free_eng(client) == eng0, \
            f"engineers not freed on completion: host={_free_eng(host)} client={_free_eng(client)} (want {eng0})"
        print(f"PASS completion: 1 {LASER} delivered on both; engineers freed back to {eng0}")

        session.assert_client_zero_disk(client_dir)
        print("PASS zero-disk: client (replica) user dir clean")

        print("ALL JOINT MANUFACTURE TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
