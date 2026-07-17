"""PRD-J09 GAP-1 - JOINT base defense uses the ownership control split.

Base defense (an alien retaliation UFO reaching a base) is a SEPARATE geoscape
entry point from the craft-landing flow: GeoscapeState::handleBaseDefense, NOT
ConfirmLandingState. The J09 landing rework stamped the in-battle control split
from soldier ownership only on the ConfirmLandingState path; the base-defense
path still took the SEPARATE two-world merge (CoopState(77)/sendCraft/
loadWorld(111)), which in JOINT forces EVERY shared soldier to host control (its
coopBase match never fires) instead of splitting by ownership.

This test proves the shared base's garrison fights split BY OWNERSHIP:

  1. Trigger a base defense on the shared base (host-side, via the real
     handleBaseDefense) with a mixed-owner garrison (some soldiers seat 0 / host,
     some seat 1 / client).
  2. On BOTH machines, every deployed BattleUnit's _coop matches its soldier's
     ownerPlayerId (owner 0/999 -> coop 0 host control, any other seat -> coop 1
     client control). The client-owned soldiers MUST be coop=1 - this is the
     assertion that fails against the unfixed code (they come out coop=0).
  3. No soldier duplication: every deployed soldier-unit id is a real roster id
     and appears once (the SEPARATE merge would push_back client soldiers as new
     ids).
  4. After the battle both worlds stay IDENTICAL (the JOINT post-battle merge /
     restream works for a base defense, exactly as for a craft mission).

Ending the battle: the only way to end a base defense headlessly is to abort
(there is no synchronized "kill all aliens" primitive in the coop lockstep
battle), and aborting a base defense always LOSES that base
(DebriefingState: target=="STR_BASE" + aborted -> _destroyBase). So we first
build a SECOND shared base (via the real JOINT base_new flow): losing the
attacked base then leaves a savable, non-game-over world, and both machines
converge on it. The GAP-1 assertion (control split) is checked at battle entry,
before any of this, so it stands on its own.

Run:  python tools/coop_test/test_joint_base_defense.py
Exit 0 = pass; 2 = failure.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo
from harness import LAND_LON, LAND_LAT


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _real_bases(gc):
    return [b for b in _geo(gc)["bases"]
            if not b.get("coopBase") and not b.get("coopIcon")]


def _base0(gc):
    bs = _real_bases(gc)
    if not bs:
        raise AssertionError("no real base")
    return bs[0]


def _roster(gc):
    """Every soldier at the shared base(s), flattened; JOINT has one world."""
    out = []
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        out.extend(b["soldiers"])
    return out


def _states(gc):
    return gc.cmd({"cmd": "get_state"})["states"]


def _has(gc, name):
    return any(name in s for s in _states(gc))


def _battle(gc):
    return gc.cmd({"cmd": "battle_state"})


def _deployed(gc):
    """soldierId -> unit dict for every deployed player soldier-unit."""
    bs = _battle(gc)
    if not bs.get("inBattle"):
        return {}
    return {u["soldierId"]: u for u in bs["units"]
            if u["soldierId"] != -1 and u["faction"] == 0}


def _want_coop(owner):
    """Ownership -> in-battle control: seat 0 or unstamped (999) is host control."""
    return 0 if owner in (0, 999) else 1


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
            print(f"  DBG {tag} bases: {[b['name'] for b in _real_bases(gc)]}")
        except Exception as e:
            print(f"  DBG {tag} dump failed: {e}")


def main():
    js = joint_fixture.bring_up("jbasedef", (48774, 48775, 48074))
    host, client = js.host, js.client
    fail = []
    try:

        rh = sorted(s["id"] for s in _roster(host))
        rc = sorted(s["id"] for s in _roster(client))
        assert rh == rc, f"rosters differ: host={rh} client={rc}"
        assert len(rh) >= 4, f"need >=4 starting soldiers for a mixed garrison, got {rh}"
        base0_name = _base0(host)["name"]
        print(f"PASS full-roster: both players see all {len(rh)} garrison soldiers {rh}")

        # ---- second shared base, so losing the attacked one keeps the world
        #      savable (and non-game-over). Real JOINT base_new flow -> both sides.
        r = host.ok({"cmd": "build_new_base", "lon": LAND_LON, "lat": LAND_LAT,
                     "name": "SpareBase", "liftX": 2, "liftY": 2})
        assert r.get("ok"), f"build_new_base failed: {r}"
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} has 2 bases",
                        lambda gc=gc: (len(_real_bases(gc)) == 2) or None, timeout=30, interval=0.5)
        print("PASS spare-base: a second shared base exists on both machines")

        # ---- mixed-owner garrison at the FIRST base (the one to be attacked):
        #      first two soldiers client-owned (seat 1), rest keep the
        #      campaign-start default (999 -> host control). Stamped on BOTH
        #      machines so the shared world stays identical.
        client_owned = [rh[0], rh[1]]
        for gc in (host, client):
            for sid in client_owned:
                gc.ok({"cmd": "set_soldier_owner", "soldier_id": sid, "owner": 1})
        owner = {s["id"]: s["owner"] for s in _roster(host)}
        assert all(owner[sid] == 1 for sid in client_owned)
        print(f"PASS mixed-owner: soldiers {client_owned} -> seat 1 (client); "
              f"the rest -> host control")

        # ---- seed a retaliation UFO and trigger the base defense host-side ----
        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_SMALL_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH", "region": "STR_NORTH_AMERICA",
                       "race": "STR_SECTOID", "trajectory": "P0", "state": "flying"})
        r = host.ok({"cmd": "trigger_base_defense", "ufo_id": ufo["ufo_id"]})
        assert r.get("base") == base0_name, \
            f"base defense hit '{r.get('base')}', expected '{base0_name}'"
        print(f"PASS trigger: base defense entered on host (base '{r.get('base')}')")

        # ---- both machines enter the JOINT base-defense battle ---------------
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} entered the base-defense battle",
                        lambda gc=gc: _battle(gc).get("inBattle") or None,
                        timeout=180, interval=1.0)
        assert _battle(host)["missionType"] == "STR_BASE_DEFENSE", \
            f"host missionType={_battle(host)['missionType']} (expected STR_BASE_DEFENSE)"
        print("PASS entry: BOTH machines entered the JOINT base defense from the shared world")

        # ==== THE GAP-1 ASSERTION: control split = ownership, on BOTH machines =
        for tag, gc in (("host", host), ("client", client)):
            dep = _deployed(gc)
            # no phantom/duplicate ids (the SEPARATE merge push_backs new soldiers)
            assert all(sid in rh for sid in dep), \
                f"{tag}: deployed non-roster soldier ids {sorted(set(dep) - set(rh))} (duplication?)"
            # every client-owned soldier deployed, so the coop=1 check below is real
            for sid in client_owned:
                assert sid in dep, \
                    f"{tag}: client-owned soldier {sid} did not deploy (deployed {sorted(dep)})"
            # in-battle control == ownership for every deployed unit
            for sid, u in dep.items():
                want = _want_coop(owner[sid])
                assert u["coop"] == want, \
                    f"{tag}: soldier {sid} (owner {owner[sid]}) coop={u['coop']} want {want}"
        print("PASS control-split: on BOTH machines the client-owned garrison soldiers are "
              "coop=1 and the host-owned ones coop=0 - split by ownership, no duplication")

        # ---- briefing -> coop pre-battle inventory -> tactical --------------
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

        # ---- abort -> debriefing -> back to geoscape ------------------------
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

        # ---- single-world post-battle merge: both worlds identical ----------
        # The attacked base + its garrison are lost (aborted base defense); the
        # spare base survives on BOTH machines and the host's post-battle restream
        # reseeds the replica, so the worlds must converge.
        host.wait_for("post-battle base count settled",
                      lambda: (len(_real_bases(host)) == len(_real_bases(client)) == 1) or None,
                      timeout=150, interval=1.0)
        assert base0_name not in [b["name"] for b in _real_bases(host)], \
            f"attacked base '{base0_name}' should be lost after the aborted base defense"
        print("PASS outcome: attacked base lost on both machines; the spare base survived")

        # PRD-J11 shared final-state assertions: worlds identical + zero-disk.
        js.finish()

        print("TEST PASSED")
    except Exception as e:
        print(f"[FAIL] {e}")
        fail.append(str(e))
        _dbg(host, client)
    finally:
        js.shutdown()
    sys.exit(2 if fail else 0)


if __name__ == "__main__":
    main()
