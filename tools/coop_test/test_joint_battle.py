"""PRD-J09 AC2/AC3 - JOINT squad battle + single-world post-battle merge.

In a JOINT campaign there is ONE shared world. A mission flown from a shared
craft must:

  1. START host-side from the shared world with NO two-world merge (the SEPARATE
     CoopState(88)/sendCraft/"battleclient" dance would duplicate the shared
     soldiers). The host generates the battle and ships "battlehost"; the client
     loads it and both run the existing lockstep coop battle.
  2. Split control by OWNERSHIP: each BattleUnit inherits _coop from its
     soldier's ownerPlayerId (seat 0 -> host control, any other seat -> client).
     Asserted on BOTH machines (the split rides the shared battlehost blob).
  3. After debriefing, both worlds are IDENTICAL (funds, roster, base stores).
     The host's lockstep-debriefed world is authoritative and is restreamed whole
     to the replica (the PRD's sledgehammer merge). Crucially the host's SEPARATE
     "delete the other player's battle copies" cleanup must NOT run: in JOINT the
     client-owned soldiers carry _coop=1 but are legitimate members of the single
     world and must survive.

Two scenarios:
  mixed       AC2 - host-owned (seat 0) + client-owned (seat 1) soldier aboard;
                    units split coop=0 / coop=1 on both machines.
  solo_client AC3 - only CLIENT-owned soldiers aboard; every deployed unit is
                    coop=1 (the host controls none) and the worlds still merge.
                    NOTE: in the host-authoritative JOINT model the host is the
                    battle authority even for a solo-seat squad, so it enters the
                    battle as a unit-less spectator rather than staying on the
                    geoscape - see session-notes-9.md (PRD-anchor discrepancy).

Run:  python tools/coop_test/test_joint_battle.py
Exit 0 = pass; 2 = failure.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base")


def _roster(gc):
    out = []
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        out.extend(b["soldiers"])
    return out


def _skyranger(gc):
    for c in _base0(gc)["crafts"]:
        if "SKYRANGER" in c["type"]:
            return c
    raise AssertionError("no skyranger")


def _states(gc):
    return gc.cmd({"cmd": "get_state"})["states"]


def _has(gc, name):
    return any(name in s for s in _states(gc))


def _battle(gc):
    return gc.cmd({"cmd": "battle_state"})


def _world_fingerprint(gc):
    """Post-battle world equality probe: funds + surviving roster + stores."""
    g = _geo(gc)
    rep = gc.ok({"cmd": "base_report"})
    return {
        "funds": g["funds"],
        "roster": sorted((s["id"], s["owner"]) for s in _roster(gc)),
        "storage": dict(sorted(rep["storage"].items())),
    }


def _dbg(host, client):
    for tag, gc in (("host", host), ("client", client)):
        try:
            print(f"  DBG {tag} states: {_states(gc)[-3:]}")
            c = gc.cmd({"cmd": "get_coop"})
            print(f"  DBG {tag} coop: host={c.get('host')} joint={c.get('joint')} "
                  f"missionEnd={c.get('coopMissionEnd')} dialog={c.get('coopDialog')}")
            bs = _battle(gc)
            if bs.get("inBattle"):
                print(f"  DBG {tag} units(soldier,coop,owner): "
                      f"{[(u['soldierId'], u['coop'], u['owner']) for u in bs['units'] if u['soldierId'] != -1]}")
            print(f"  DBG {tag} world: {_world_fingerprint(gc)}")
        except Exception as e:
            print(f"  DBG {tag} dump failed: {e}")


def run_scenario(label, owners, want_coop, ports, fail):
    """owners: {slot: seat} for the two squad soldiers (slot 0/1 of the roster).
       want_coop: {slot: expected BattleUnit _coop}."""
    print(f"\n===== scenario '{label}' =====")
    js = joint_fixture.bring_up(f"jbat_{label}", ports)
    host, client = js.host, js.client
    try:

        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]
        cid = _skyranger(host)["id"]
        rh = sorted(s["id"] for s in _roster(host))
        squad = [rh[0], rh[1]]

        # ---- squad ownership (stamped identically on both machines) ------
        for gc in (host, client):
            for slot, sid in enumerate(squad):
                gc.ok({"cmd": "set_soldier_owner", "soldier_id": sid, "owner": owners[slot]})
        for sid in rh:
            host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": False})
        for sid in squad:
            host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})

        def _aboard(gc):
            return sorted(s["id"] for s in _roster(gc) if s["craftId"] == cid)

        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} squad aboard",
                        lambda gc=gc: (_aboard(gc) == sorted(squad)) or None,
                        timeout=40, interval=0.5)
        seats = {sid: owners[i] for i, sid in enumerate(squad)}
        print(f"PASS squad: {seats} aboard shared craft {cid} on both machines")

        # ---- seed a site and fly the shared craft to it -------------------
        site = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                        "deployment": "STR_TERROR_MISSION", "lon": blon + 0.35,
                        "lat": blat + 0.10, "race": "STR_SECTOID", "hours": 240})
        site_id = site["site_id"]
        host.wait_for("site on host",
                      lambda: any(s["id"] == site_id for s in _geo(host)["missionSites"]) or None,
                      timeout=30)
        host.ok({"cmd": "craft_force", "craft_id": cid, "status": "STR_OUT",
                 "lon": blon + 0.34, "lat": blat + 0.10, "dest": f"site:{site_id}",
                 "fuel": 999999, "lowFuel": False})

        def _landing_prompt():
            if _has(host, "ConfirmLandingState"):
                return True
            host.cmd({"cmd": "geo_set_speed", "idx": 2})  # not geo_run: it auto-declines
            return None

        host.wait_for("ConfirmLandingState on host", _landing_prompt, timeout=90, interval=0.5)
        print("PASS arrival: shared craft reached the site; host got the landing prompt")

        # ---- JOINT battle entry: host confirms; NO two-world merge --------
        host.ok({"cmd": "confirm_landing"})
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} entered the battle",
                        lambda gc=gc: _battle(gc).get("inBattle") or None,
                        timeout=180, interval=1.0)
        print("PASS entry: BOTH machines entered the JOINT battle from the shared world")

        # ---- control split = ownership, on BOTH machines ------------------
        for tag, gc in (("host", host), ("client", client)):
            us = {u["soldierId"]: u for u in _battle(gc)["units"] if u["soldierId"] != -1}
            assert sorted(us) == sorted(squad), \
                f"{tag}: deployed {sorted(us)} want {sorted(squad)} (two-world merge duplicated?)"
            for i, sid in enumerate(squad):
                assert us[sid]["coop"] == want_coop[i], \
                    f"{tag}: soldier {sid} (seat {owners[i]}) coop={us[sid]['coop']} want {want_coop[i]}"
        if set(want_coop.values()) == {1}:
            print("PASS control-split: every deployed unit is coop=1 (client-controlled) on "
                  "BOTH machines - the host commands none of this solo-seat squad")
        else:
            print("PASS control-split: on BOTH machines the host-owned unit is coop=0 and the "
                  "client-owned unit is coop=1; exactly the squad deployed")

        # ---- briefing -> coop pre-battle inventory -> tactical ------------
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} briefing", lambda gc=gc: _has(gc, "BriefingState") or None,
                        timeout=120, interval=0.5)
            gc.ok({"cmd": "close_briefing"})
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} pre-battle inventory",
                        lambda gc=gc: _has(gc, "InventoryState") or None, timeout=120, interval=0.5)
            gc.ok({"cmd": "battle_inventory", "action": "ok"})
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} tactical map",
                        lambda gc=gc: _has(gc, "BattlescapeState") or None, timeout=120, interval=0.5)
        print("PASS tactical: both machines reached the battlescape (shared lockstep battle)")

        # ---- abort -> debriefing -> back to geoscape ----------------------
        host.ok({"cmd": "battle_action", "action": "abort"})

        def _drain(gc, deadline):
            while time.time() < deadline:
                if "GeoscapeState" in _states(gc)[-1]:
                    return True
                gc.cmd({"cmd": "dismiss_popup"})
                time.sleep(0.4)
            return None

        deadline = time.time() + 200
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} back on geoscape after debriefing",
                        lambda gc=gc: _drain(gc, deadline), timeout=220, interval=1.0)
        print("PASS debriefing: both machines returned to the geoscape")

        # ---- single-world post-battle merge: worlds identical -------------
        def _equal():
            return True if _world_fingerprint(host) == _world_fingerprint(client) else None

        host.wait_for("post-battle worlds identical (restream settled)", _equal,
                      timeout=150, interval=1.0)
        fh = _world_fingerprint(host)
        ids = [s for s, _ in fh["roster"]]
        for i, sid in enumerate(squad):
            assert sid in ids, \
                f"squad soldier {sid} (seat {owners[i]}) was deleted post-battle (guest cleanup ran!)"
        print(f"PASS merge: post-battle worlds IDENTICAL on both machines "
              f"(funds={fh['funds']}, roster={ids}); every squad soldier survived")

        # PRD-J11: the shared final-state assertions. Strictly stronger than the
        # local fingerprint above (facilities/stores/transfers/research/craft
        # identity too), so the post-battle restream is checked whole.
        js.finish()

        print(f"scenario '{label}': PASSED")
    except Exception as e:
        print(f"[FAIL] {label}: {e}")
        fail.append(f"{label}: {e}")
        _dbg(host, client)
    finally:
        js.shutdown()


def main():
    fail = []
    # AC2: mixed squad - one host-owned (seat 0) + one client-owned (seat 1).
    run_scenario("mixed", {0: 0, 1: 1}, {0: 0, 1: 1}, (48770, 48771, 48070), fail)
    # AC3: solo-seat squad - only CLIENT-owned soldiers aboard.
    run_scenario("solo_client", {0: 1, 1: 1}, {0: 1, 1: 1}, (48772, 48773, 48072), fail)

    print("\n==== PRD-J09 battle summary ====")
    if fail:
        print(f"  FAILURES: {fail}")
        sys.exit(2)
    print("  mixed + solo_client: JOINT battles start host-side from the shared world, "
          "split control by ownership on both machines, and merge to identical worlds.")
    sys.exit(0)


if __name__ == "__main__":
    main()
