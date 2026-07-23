"""Mid-battle coop RESUME must restore split control (SEPARATE mode).

Root cause (see .agents/docs/midbattle-resume-plan.md): the host-authoritative
save redesign rerouted campaign resume so the battle-resume path never completes
the COOP_READY handshake. `coopSession` stays false on both machines, so the
BattlescapeState coop-init block (which sets `_battleInit`) never fires, and
`BattleUnit::isSelectable` falls through to the vanilla "all player units
selectable" branch on BOTH machines. Result: both players command every soldier
instead of the host commanding its coop==0 units and the client its coop==1 units.

This test proves it end to end:

  1. SEPARATE campaign; assemble a MIXED-ownership squad on the host's craft -
     the host's own soldiers (become coop==0 in battle) plus one CLIENT-owned
     guest seated on the host craft (becomes coop==1 via the two-world merge).
  2. Enter the battle live (coop_mission_start) and reach the battlescape.
  3. SANITY: the LIVE split works - _battleInit true on both, exactly one machine
     has coopTurn==2, and the two machines' selectable sets are disjoint and each
     is a subset of the coop set it owns. (If this fails the test itself is
     unsound, not the engine.)
  4. Host saves mid-battle, both instances are killed, the host relaunches with
     the save seeded (client dir empty), and the pair RESUMES into the battle.
  5. ASSERT the same split holds after resume. On the unfixed engine this FAILS:
     _battleInit is false and the selectable sets overlap.

Run:  python tools/coop_test/test_coop_resume_battle_control.py
Exit 0 = pass (split restored); 2 = failure (split broken / never resumed).
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session

SAVE = "coop_resume_battle.sav"
PORT = "47960"


# ---- small state / world probes -------------------------------------------

def states(gc):
    return gc.cmd({"cmd": "get_state"})["states"]


def has(gc, name):
    return any(name in s for s in states(gc))


def geo(gc):
    return gc.ok({"cmd": "geo_state"})


def base0(gc):
    for b in geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base")


def own_base(gc):
    s = gc.ok({"cmd": "get_soldiers"})
    return next(b for b in s["bases"]
                if not b["coopBaseFlag"] and not b.get("coopIcon") and b["soldiers"])


def skyranger(gc):
    for c in base0(gc)["crafts"]:
        if "SKYRANGER" in c["type"]:
            return c
    raise AssertionError("no skyranger")


def battle(gc):
    return gc.cmd({"cmd": "battle_state"})


def top(gc):
    return states(gc)[-1].split("::")[-1]


def drain_to_tactical(host, client, rounds=12):
    """After the pre-battle inventory closes, coop pops turn/wait dialogs over the
    battlescape; BattlescapeState::think() (which runs the coop-init handshake that
    sets _battleInit) only ticks when BattlescapeState is the TOP state. Dismiss
    popups until both machines are on the tactical map."""
    for _ in range(rounds):
        moved = False
        for gc in (host, client):
            if top(gc) != "BattlescapeState":
                gc.cmd({"cmd": "dismiss_popup"})
                moved = True
        time.sleep(1.0)
        if not moved and all(top(gc) == "BattlescapeState" for gc in (host, client)):
            return


def drain_host_coop_notice(host, tries=15):
    """A guest transfer/visit pops an items-received CoopState over the host
    geoscape that freezes the sim. Pop only such notices (never a wait/merge
    dialog - those are gone by the time this runs) so the craft can fly."""
    for _ in range(tries):
        top = states(host)[-1]
        if "GeoscapeState" in top:
            return
        if "CoopState" in top:
            host.cmd({"cmd": "coop_dialog_back"})
        else:
            host.cmd({"cmd": "dismiss_popup"})
        time.sleep(0.4)


# ---- the split assertion (shared by the live-sanity and post-resume checks)-

def split_report(host, client):
    """Gather the observable split state on both machines."""
    out = {}
    for tag, gc in (("host", host), ("client", client)):
        b = battle(gc)
        units = [u for u in b.get("units", []) if u["soldierId"] != -1]
        out[tag] = {
            "inBattle": b.get("inBattle"),
            "battleInit": b.get("battleInit"),
            "coopSession": b.get("coopSession"),
            "coopTurn": b.get("coopTurn"),
            "host": b.get("host"),
            "coop0": sorted(u["soldierId"] for u in units if u["coop"] == 0),
            "coopN": sorted(u["soldierId"] for u in units if u["coop"] != 0),
            "selectable": sorted(u["soldierId"] for u in units if u["selectable"]),
            "units": [(u["soldierId"], u["coop"], u["selectable"]) for u in units],
        }
    return out


def assert_split(host, client, phase):
    """The mid-battle coop control-split invariant.

    host selectable subset of its coop==0 set; client selectable subset of the
    coop!=0 set; the two selectable id-sets disjoint; exactly one machine on turn
    (coopTurn==2); _battleInit and coopSession true on both.
    """
    r = split_report(host, client)
    h, c = r["host"], r["client"]
    # make the observed values impossible to miss in a failure
    detail = (
        f"\n  [{phase}] observed:"
        f"\n    host  : battleInit={h['battleInit']} coopSession={h['coopSession']} "
        f"coopTurn={h['coopTurn']} host={h['host']}"
        f"\n            coop0={h['coop0']} coopN={h['coopN']} selectable={h['selectable']}"
        f"\n    client: battleInit={c['battleInit']} coopSession={c['coopSession']} "
        f"coopTurn={c['coopTurn']} host={c['host']}"
        f"\n            coop0={c['coop0']} coopN={c['coopN']} selectable={c['selectable']}"
    )

    hsel, csel = set(h["selectable"]), set(c["selectable"])
    errs = []
    if not (h["battleInit"] and c["battleInit"]):
        errs.append(f"battleInit not true on both (host={h['battleInit']} client={c['battleInit']})")
    if not (h["coopSession"] and c["coopSession"]):
        errs.append(f"coopSession not true on both (host={h['coopSession']} client={c['coopSession']})")
    if not hsel.issubset(set(h["coop0"])):
        errs.append(f"host selectable {sorted(hsel)} is NOT a subset of its coop==0 set {h['coop0']}")
    if not csel.issubset(set(c["coopN"])):
        errs.append(f"client selectable {sorted(csel)} is NOT a subset of the coop!=0 set {c['coopN']}")
    if hsel & csel:
        errs.append(f"host and client selectable sets OVERLAP: {sorted(hsel & csel)} "
                    f"(both machines command the same soldiers)")
    on_turn = [tag for tag, m in (("host", h), ("client", c)) if m["coopTurn"] == 2]
    if len(on_turn) != 1:
        errs.append(f"exactly one machine must have coopTurn==2, got {on_turn or 'none'}")

    if errs:
        raise AssertionError(f"[{phase}] split BROKEN:" + detail + "\n  errors:\n    - "
                             + "\n    - ".join(errs))
    print(f"PASS [{phase}] split intact:" + detail)
    return r


# ---- battle bring-up (mixed-ownership squad, live SEPARATE battle) ---------

def bring_up_mixed_battle(host, client):
    session.new_campaign(host, client, port=PORT)

    hb = own_base(host)
    host_base_name = hb["name"]
    cid = skyranger(host)["id"]

    # seat three of the host's own soldiers on the host craft (unassign-all first)
    rh = sorted(s["id"] for s in hb["soldiers"])
    for sid in rh:
        host.cmd({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": False})
    host_squad = rh[:3]
    for sid in host_squad:
        r = host.cmd({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})
        assert r.get("seated"), f"host soldier {sid} not seated: {r}"

    # a CLIENT-owned guest, transferred to the host base and seated on the host
    # craft -> the two-world merge stamps it coop==1 in the battle.
    cb = own_base(client)
    spare = next(s for s in cb["soldiers"] if not s.get("craft"))["name"]
    client.ok({"cmd": "rename_soldier", "name": spare, "newName": "Guest Zzz"})
    tr = client.ok({"cmd": "transfer_to_coop_base", "name": "Guest", "toBase": host_base_name})
    assert tr.get("transferred"), f"guest transfer failed: {tr}"
    client.ok({"cmd": "visit_coop_base", "base": host_base_name})
    client.wait_for("client inside host base",
                    lambda: client.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None, timeout=60)
    rep = client.wait_for(
        "guest visible at host base",
        lambda: (lambda r: r if any("Guest" in s["name"] for s in r["soldiers"]) else None)(
            client.ok({"cmd": "base_report", "coop": True})), timeout=40)
    guest_id = next(s for s in rep["soldiers"] if "Guest" in s["name"])["id"]
    peer_craft = next(c for c in rep["crafts"] if "SKYRANGER" in c["type"])["id"]
    client.ok({"cmd": "craft_assign", "soldier_id": guest_id, "craft_id": peer_craft,
               "coop": True, "on": True})
    client.ok({"cmd": "open_soldiers", "base": host_base_name})
    client.wait_for("client soldiers screen", lambda: has(client, "SoldiersState") or None, timeout=30)
    client.ok({"cmd": "soldiers_ok"})
    client.ok({"cmd": "leave_base"})
    client.wait_for("client back on geoscape",
                    lambda: (not client.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None, timeout=60)
    print(f"squad assembled: host soldiers {host_squad} (coop==0) + client guest {guest_id} (coop==1)")

    # settle the host (the guest transfer popped a notice), spawn a site, fly there
    drain_host_coop_notice(host)
    b0 = base0(host)
    site = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                    "deployment": "STR_TERROR_MISSION", "lon": b0["lon"] + 0.35,
                    "lat": b0["lat"] + 0.10, "race": "STR_SECTOID", "hours": 240})
    site_id = site["site_id"]
    host.wait_for("site on host",
                  lambda: any(s["id"] == site_id for s in geo(host)["missionSites"]) or None, timeout=30)
    host.ok({"cmd": "craft_force", "craft_id": cid, "status": "STR_OUT",
             "lon": b0["lon"] + 0.34, "lat": b0["lat"] + 0.10, "dest": f"site:{site_id}",
             "fuel": 999999, "lowFuel": False})

    def landing_prompt():
        if has(host, "ConfirmLandingState"):
            return True
        top = states(host)[-1]
        if "CoopState" in top:
            host.cmd({"cmd": "coop_dialog_back"})
        elif "GeoscapeState" not in top:
            host.cmd({"cmd": "dismiss_popup"})
        host.cmd({"cmd": "geo_set_speed", "idx": 2})
        return None

    host.wait_for("host landing prompt", landing_prompt, timeout=120, interval=0.5)

    # SEPARATE two-world-merge battle entry (the new command)
    ms = host.ok({"cmd": "coop_mission_start"})
    print(f"coop_mission_start -> {ms}")
    for gc, tag in ((host, "host"), (client, "client")):
        gc.wait_for(f"{tag} entered the battle",
                    lambda gc=gc: battle(gc).get("inBattle") or None, timeout=180, interval=1.0)

    # briefing -> pre-battle coop inventory -> tactical map, on both machines
    for gc, tag in ((host, "host"), (client, "client")):
        gc.wait_for(f"{tag} briefing", lambda gc=gc: has(gc, "BriefingState") or None,
                    timeout=120, interval=0.5)
        gc.ok({"cmd": "close_briefing"})
    for gc, tag in ((host, "host"), (client, "client")):
        gc.wait_for(f"{tag} pre-battle inventory",
                    lambda gc=gc: has(gc, "InventoryState") or None, timeout=120, interval=0.5)
        gc.ok({"cmd": "battle_inventory", "action": "ok"})
    for gc, tag in ((host, "host"), (client, "client")):
        gc.wait_for(f"{tag} tactical map",
                    lambda gc=gc: has(gc, "BattlescapeState") or None, timeout=120, interval=0.5)
    drain_to_tactical(host, client)
    print("both machines reached the battlescape (live SEPARATE coop battle)")
    return host_squad, guest_id


def settle_and_assert(host, client, phase, timeout=60):
    """The coop-init handshake settles a few think() ticks after the battlescape
    opens. Give it a fair chance - drain popups so BattlescapeState ticks, wait
    (bounded) for _battleInit on both + exactly one machine on turn - then assert
    the split. On the unfixed engine after a resume this simply never settles, so
    the bounded wait elapses and the assert fails with the observed (false) flags."""
    drain_to_tactical(host, client)
    deadline = time.time() + timeout
    while time.time() < deadline:
        r = split_report(host, client)
        h, c = r["host"], r["client"]
        on_turn = sum(1 for m in (h, c) if m["coopTurn"] == 2)
        if h["battleInit"] and c["battleInit"] and on_turn == 1:
            break
        drain_to_tactical(host, client, rounds=2)
        time.sleep(2.0)
    return assert_split(host, client, phase)


def main():
    host_dir = make_user_dir("crbc_host")
    host = GameClient("host", 47861, host_dir)
    client = GameClient("client", 47862, make_user_dir("crbc_client"))
    fail = None
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        host_squad, guest_id = bring_up_mixed_battle(host, client)

        # sanity: the LIVE split works (proves the test/setup is sound)
        settle_and_assert(host, client, "live battle entry")

        # host saves mid-battle (SavedGame::save embeds the client world blob, so
        # the client is battle-eligible on resume), then both instances go down.
        host.ok({"cmd": "save_game", "file": SAVE})
        assert os.path.exists(os.path.join(host_dir, "xcom1", SAVE)), "save not on disk"
        print(f"host saved mid-battle -> {SAVE}")
        host.shutdown(); client.shutdown()

        # relaunch: host reuses its dir (save present); client gets an EMPTY dir.
        host = GameClient("host", 47863, host_dir)
        client = GameClient("client", 47864, make_user_dir("crbc_client2"))
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        # resume the mid-battle save; helper waits (bounded) for BOTH inBattle.
        session.resume_campaign_battle(host, client, SAVE, port="47961")

        # THE POINT: after resume the split must be restored. Unfixed engine: RED.
        settle_and_assert(host, client, "after resume")

        session.assert_client_zero_disk(client.user_dir)
        print("PASS zero-disk: resumed client user dir clean")
        print("ALL SEPARATE MID-BATTLE RESUME TESTS PASSED")
    except Exception as e:
        fail = e
        print(f"[FAIL] {e}")
    finally:
        host.shutdown(); client.shutdown()

    if fail:
        sys.exit(2)
    sys.exit(0)


if __name__ == "__main__":
    main()
