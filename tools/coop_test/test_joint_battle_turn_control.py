"""Bug: in a JOINT battle BOTH players said "your turn" and could command the SAME set of
soldiers (both drove the host's units; the client's own units were controllable by no one).

Root cause: BattlescapeState's coop turn-init derived the battle host role from
coop_save_owner_player_id, which is set MACHINE-LOCAL (0 on host, 1 on client). The host
matched `==0` and the client matched `==1`, so BOTH called setHost(true) -> both machines
behaved as host -> BattleUnit::isSelectable let both command the coop==0 (host-owned) units.
Fix: in JOINT derive the role from the unambiguous server-owner flag (host machine = battle
host, client = not), so isSelectable splits by ownership.

This drives the REAL flow to the tactical map (briefing -> pre-battle inventory ->
NextTurn) and asserts, using the live BattleUnit::isSelectable gate:
  * host is on its turn and can command exactly its OWN (coop 0 / seat 0) soldiers,
  * the client is NOT on the host's turn and can command NOTHING (it waits),
  * the two never command the same unit.

A control-value-only check is NOT enough - isSelectable only splits when the coop turn
actually initialized (isYourTurn==2 + the correct host role), which is exactly what broke.

Run:  python tools/coop_test/test_joint_battle_turn_control.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import test_joint_battle as B


def _aboard(gc, cid):
    return sorted(s["id"] for s in B._roster(gc) if s["craftId"] == cid)


def _selectable(bs):
    return sorted(u["soldierId"] for u in bs["units"]
                  if u["soldierId"] != -1 and u.get("selectable"))


def _top(gc):
    return gc.cmd({"cmd": "get_state"})["states"][-1].split("::")[-1]


def _drain_to_tactical(host, client, rounds=8):
    for _ in range(rounds):
        moved = False
        for gc in (host, client):
            if _top(gc) != "BattlescapeState":
                gc.cmd({"cmd": "dismiss_popup"})
                moved = True
        time.sleep(1.0)
        if not moved and all(_top(gc) == "BattlescapeState" for gc in (host, client)):
            return
    return


def main():
    js = joint_fixture.bring_up("jturn", (48888, 48889, 48188))
    host, client = js.host, js.client
    try:
        owner = {s["id"]: s["owner"] for s in B._roster(host)}
        seat0 = sorted(sid for sid, o in owner.items() if o == 0)   # host-owned
        seat1 = sorted(sid for sid, o in owner.items() if o == 1)   # client-owned
        assert len(seat0) >= 2 and len(seat1) >= 2, \
            f"need >=2 per seat: seat0={seat0} seat1={seat1}"
        cid = B._skyranger(host)["id"]

        # Mixed squad: host boards its own, client boards its own.
        for sid in owner:
            host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": False})
        host_squad, client_squad = seat0[:2], seat1[:2]
        squad = sorted(host_squad + client_squad)
        for sid in host_squad:
            host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})
        for sid in client_squad:
            client.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})
        for gc in (host, client):
            gc.wait_for("squad aboard", lambda gc=gc: (_aboard(gc, cid) == squad) or None,
                        timeout=45, interval=0.5)

        # Enter the mission (real brokered landing).
        b0 = B._base0(host)
        blon, blat = b0["lon"], b0["lat"]
        site_id = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                           "deployment": "STR_TERROR_MISSION", "lon": blon + 0.35,
                           "lat": blat + 0.10, "race": "STR_SECTOID", "hours": 240})["site_id"]
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

        # Real player path to the tactical map: briefing -> pre-battle inventory -> NextTurn.
        for gc in (host, client):
            gc.wait_for("briefing", lambda gc=gc: B._has(gc, "BriefingState") or None,
                        timeout=30, interval=0.5)
            gc.ok({"cmd": "close_briefing"})
        for gc in (host, client):
            gc.wait_for("inventory", lambda gc=gc: B._has(gc, "InventoryState") or None,
                        timeout=30, interval=0.5)
            gc.ok({"cmd": "battle_inventory", "action": "ok"})
        _drain_to_tactical(host, client)

        # Wait for the coop turn to initialize (host active).
        host.wait_for("host turn active",
                      lambda: (B._battle(host).get("coopTurn") == 2) or None,
                      timeout=30, interval=0.5)
        # Let it settle, then assert on a stable sample.
        time.sleep(2)
        hb, cb = B._battle(host), B._battle(client)

        # SAME WORLD: host and client must be standing on the IDENTICAL battle map. The
        # host generates once and ships it; a re-entry that called bgen.run() a second time
        # used to hand the host a brand-new random map while the client kept the first, so
        # both players fought on different maps (soldiers on open ground, no craft).
        for key in ("mapSizeXYZ", "mapObjTiles", "mapDiscoveredFloor", "mapFingerprint"):
            assert hb.get(key) == cb.get(key), (
                f"MAP DIVERGENCE: host and client are on DIFFERENT maps - "
                f"{key} host={hb.get(key)} client={cb.get(key)}")
        h_pos = sorted((u["soldierId"], u["x"], u["y"], u["z"]) for u in hb["units"]
                       if u["soldierId"] != -1)
        c_pos = sorted((u["soldierId"], u["x"], u["y"], u["z"]) for u in cb["units"]
                       if u["soldierId"] != -1)
        assert h_pos == c_pos, f"soldier positions diverged: host={h_pos} client={c_pos}"
        print(f"PASS same map: host and client share ONE battle map "
              f"(fp={hb.get('mapFingerprint')}, {hb.get('mapObjTiles')} object tiles) "
              f"with identical soldier positions")

        h_sel, c_sel = _selectable(hb), _selectable(cb)

        # HOST: on its turn, commands EXACTLY its own seat-0 soldiers.
        assert hb["coopTurn"] == 2, f"host not on its turn: coopTurn={hb['coopTurn']}"
        assert h_sel == host_squad, \
            f"host commands {h_sel}, expected its OWN {host_squad}"
        # CLIENT: it is the host's turn, so the client waits and commands NOTHING - it must
        # NOT be able to drive the host's units (the reported bug).
        assert cb["coopTurn"] != 2, \
            f"BUG: client also on its turn (coopTurn={cb['coopTurn']}) - both 'your turn'"
        assert c_sel == [], \
            f"BUG: client can command {c_sel} during the host's turn (both control same team)"
        assert not (set(h_sel) & set(c_sel)), "host and client command overlapping units"
        print(f"PASS start-of-battle: host is on its turn commanding its OWN {h_sel}; "
              f"client waits (coopTurn={cb['coopTurn']}) and commands nothing - "
              "NOT both-control-same-team")
        print("ALL JOINT BATTLE-TURN-CONTROL TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
