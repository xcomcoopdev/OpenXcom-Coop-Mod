"""GAP-6b: a SELL-flagged JOINT production must not drift the replica's stores.

A JOINT campaign is one host-authoritative world. When a production has the
manufacture "sell produced items" flag set, the host (Production::step) CREDITS
funds for the produced items and adds NOTHING to stores. Funds ride every
joint_apply, so they stay synced; but the prod_done broadcast used to carry no
sell flag and the replica applier UNCONDITIONALLY materialized getProducedItems(),
so the replica added items the host had sold -> its chkItems drifted ABOVE the
host (self-healed only by an expensive auto-resync).

  SELL      start a sell-flagged production (man_start carries sell=True -> the
            applier sets Production::setSellItems on BOTH machines), drive it to
            completion on the host. The host sells: stores UNCHANGED, funds up.
            The replica must mirror EXACTLY - stores UNCHANGED, funds equal - and
            NOT materialize the sold item.

RED  (pre-fix): the replica adds the sold item -> its STR_LASER_PISTOL count is
     host + 1, chkItems host < client, and the drift trips an auto-resync.
GREEN (post-fix): host and replica stores identical (neither added the sold
     item), funds equal, chkItems equal, no spurious resync, world-equality +
     zero-disk pass.

Produced-CRAFT note: the sell branch lives inside the NON-craft arm of
Production::step(), so a produced craft is never sold; the fix leaves the
replica's craft materialization unconditional. Craft completion is already
covered by test_joint_manufacture / test_joint_sim; this test targets the
item-sell drift specifically.

Run:  python tools/coop_test/test_joint_production_sell.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo

LASER = "STR_LASER_PISTOL"      # manufacture: time 300, produces 1x itself, costSell 20000
LASER_TIME = 300


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


def _chk_items(gc):
    return gc.ok({"cmd": "joint_checksum"})["chkItems"]


def _resync_requests(gc):
    return gc.ok({"cmd": "joint_resync_stats"})["requests"]


def _discover_both(host, client, topic):
    for gc in (host, client):
        gc.ok({"cmd": "discover_research", "topic": topic})


def main():
    js = joint_fixture.bring_up("jsell", (48750, 48751, 48050))
    host, client = js.host, js.client
    try:
        # Unlock the laser manufacture on BOTH machines (deterministic; keeps the
        # shared world equal).
        _discover_both(host, client, LASER)

        eng0 = _free_eng(host)
        assert eng0 == _free_eng(client) and eng0 >= 1, \
            f"engineer pool differs / too small: host={eng0} client={_free_eng(client)}"
        assert _storage(host, LASER) == _storage(client, LASER), "laser stock differs at start"
        print(f"PASS setup: {eng0} free engineers on both; {LASER} unlocked")

        # ================================================================
        # Start a SELL-flagged production: man_start carries sell=True, so the
        # applier sets Production::setSellItems on host AND replica. 1 engineer so
        # the very next hourly step (after seeding _timeSpent one short) completes.
        # ================================================================
        client.ok({"cmd": "joint_cmd", "jcmd": "man_start", "baseId": 0,
                   "payload": {"item": LASER, "engineers": 1, "qty": 1,
                               "infinite": False, "sell": True, "fallback": False}})
        host.wait_for("host started sell-flagged laser",
                      lambda: (_prod(host, LASER) and _prod(host, LASER)["sell"]) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client started sell-flagged laser",
                        lambda: (_prod(client, LASER) and _prod(client, LASER)["sell"]) or None,
                        timeout=30, interval=0.5)
        assert _prod(host, LASER)["sell"] and _prod(client, LASER)["sell"], \
            "sell flag did not replicate to both productions"
        assert _prod(host, LASER)["engineers"] == _prod(client, LASER)["engineers"] == 1, \
            "engineer allocation mismatch after sell-flagged start"
        print(f"PASS start: {LASER} sell-flagged on both machines @ 1 engineer")

        # Seed _timeSpent one short of completion on the HOST only (replica sim is
        # frozen). Clear the resync counters right before the advance so only a
        # completion-driven drift (the bug under test) could move them.
        r = host.ok({"cmd": "set_production_progress", "item": LASER, "timeSpent": LASER_TIME - 1})
        assert r.get("found"), f"could not seed production progress on host: {r}"
        host.ok({"cmd": "joint_reset_resync_stats"})
        client.ok({"cmd": "joint_reset_resync_stats"})

        laser0_h, laser0_c = _storage(host, LASER), _storage(client, LASER)
        funds0_h, funds0_c = _funds(host), _funds(client)
        chk0_h, chk0_c = _chk_items(host), _chk_items(client)
        assert laser0_h == laser0_c and funds0_h == funds0_c and chk0_h == chk0_c, \
            (f"pre-completion state differs: laser h={laser0_h} c={laser0_c}, "
             f"funds h={funds0_h} c={funds0_c}, chkItems h={chk0_h} c={chk0_c}")

        # ================================================================
        # Advance -> the host completes the production on the first hourly step and
        # broadcasts prod_done. The host SELLS: stores unchanged, funds credited.
        # ================================================================
        geo.skip_ingame_time(host, client, minutes=60 * 24, speed_idx=5, real_timeout=90)
        host.wait_for("host sold the produced laser (production gone)",
                      lambda: (_prod(host, LASER) is None) or None, timeout=30, interval=0.5)

        # HOST authority: it took the sell branch -> NOTHING added to stores, funds up.
        assert _storage(host, LASER) == laser0_h, \
            (f"host store changed on a SELL production: {laser0_h} -> {_storage(host, LASER)} "
             f"(expected unchanged; the sell branch adds funds, not items)")
        assert _funds(host) > funds0_h, \
            f"host did not credit the sale: funds {funds0_h} -> {_funds(host)}"
        print(f"PASS host sell: stores unchanged ({laser0_h}), funds {funds0_h} -> {_funds(host)} (credited)")

        # REPLICA must mirror EXACTLY. The produced item is added in the SAME
        # prod_done applier call that removes the production, so at "production gone"
        # the drift (pre-fix) is already present - assert immediately, before the
        # 3s checksum debounce could auto-resync it away.
        client.wait_for("client applied prod_done (production gone)",
                        lambda: (_prod(client, LASER) is None) or None, timeout=30, interval=0.5)
        assert _storage(client, LASER) == laser0_c, \
            (f"GAP-6b: replica materialized a SOLD item -> store drift "
             f"{laser0_c} -> {_storage(client, LASER)} (host sold it and added nothing)")

        # Funds + item checksum converge; the host and replica hold one world.
        assert _funds(host) == _funds(client), \
            f"funds diverged after sale: host={_funds(host)} client={_funds(client)}"
        assert _chk_items(host) == _chk_items(client), \
            f"chkItems diverged after sale: host={_chk_items(host)} client={_chk_items(client)}"
        assert _free_eng(host) == _free_eng(client) == eng0, \
            f"engineers not freed equally on completion: host={_free_eng(host)} client={_free_eng(client)}"
        print(f"PASS replica mirror: stores unchanged ({laser0_c}), funds + chkItems equal host")

        # No spurious resync: the fix keeps chkItems equal throughout, so the
        # checksum never mismatches and no world restream is requested. (Pre-fix the
        # drift above would already have failed; this guards the GREEN promise.)
        assert _resync_requests(client) == 0, \
            f"a SELL-flagged production spuriously triggered {_resync_requests(client)} resync(s)"
        print("PASS no-resync: replica requested 0 world restreams (no drift to repair)")

        # PRD-J11: the shared final-state assertions (world equality + zero-disk).
        js.finish()

        print("ALL JOINT PRODUCTION-SELL TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
