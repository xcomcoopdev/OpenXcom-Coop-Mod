"""PRD-J11: the JOINT long-run - two month boundaries with a battle in between.

Every other JOINT test proves ONE mechanism in isolation, from a fresh bootstrap.
This one is the closest thing the suite has to a playthrough: a single JOINT
campaign carried across TWO month ends with research and manufacture running the
whole time and a mixed-owner squad battle in the middle, asserting full world
equality after every phase.

Why that combination: each of these subsystems writes the shared world through a
DIFFERENT channel, and this is the only test where they overlap in one campaign.

  phase 0  bootstrap                -> the streamed world (PRD-J02)
  phase 1  research + manufacture   -> client-originated joint_cmd (J06)
  phase 2  MONTH END #1             -> the monthly settlement rides the extended
                                       monthly_report packet, NOT joint_apply (J04)
                                       - a second funds writer, over a world with
                                       live projects in it
  phase 3  mixed-owner BATTLE       -> lockstep parallel sim + the post-battle
                                       restream, which REPLACES the replica's
                                       whole world (J09) - projects, transfers,
                                       month history and all
  phase 4  MONTH END #2             -> the settlement again, this time on a world
                                       that came back from a restream

The interesting question is phase 4: the restream in phase 3 rebuilds the replica
from the host's blob mid-campaign, and the monthly settlement overwrites funds +
maintenance/income/expenditure TAILS by index. If a restream left the replica's
month history a different length from the host's, the next month-end would settle
onto the wrong slot. Nothing else in the suite covers that ordering.

Runtime: ~20-40 s measured (two month rolls + a full battle) - it does the most
per second of any test here, but it is NOT the slow one: the geoscape skips are
bounded by in-game time, not wall clock, and a headless battle drives itself.
Well inside any CI budget; see .agents/docs/coop-test-harness.md.

Run:  python tools/coop_test/test_joint_month_run.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo

LASER = "STR_LASER_PISTOL"
BIG_RESEARCH = 100000     # never completes inside this run
LONG_PROD = 40            # units; never completes at 1 engineer


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base")


def _prod(gc, item):
    for p in _base0(gc)["productions"]:
        if p["item"] == item:
            return p
    return None


def _research_names(gc):
    return [r["name"] for r in _base0(gc)["research"]]


def _roster(gc):
    out = []
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        out.extend(b["soldiers"])
    return out


def _states(gc):
    return gc.cmd({"cmd": "get_state"})["states"]


def _has(gc, name):
    return any(name in s for s in _states(gc))


def _roll_month(host, client, tag):
    """Jump to the 28th and pump until the host rolls the month; the replica must
    follow via monthly_report. Returns the new monthsPassed."""
    mp0 = _geo(host)["monthsPassed"]
    host.ok({"cmd": "set_geo_day", "day": 28, "hour": 12})
    t0 = time.time()
    while _geo(host)["monthsPassed"] <= mp0 and time.time() - t0 < 180:
        geo.skip_ingame_time(host, client, minutes=60 * 24 * 2, speed_idx=5,
                             real_timeout=60)
    assert _geo(host)["monthsPassed"] > mp0, f"{tag}: host did not roll the month"
    client.wait_for(f"{tag}: replica applied monthly_report",
                    lambda: (_geo(client)["monthsPassed"] > mp0) or None,
                    timeout=60, interval=0.5)
    # The loop above stops the instant the month rolls, which leaves the monthly
    # report (and whatever else the roll popped) sitting on top of BOTH geoscapes.
    # Clear them here, or the next phase's clock never moves.
    for g in (host, client):
        geo.drain_popups(g)
    gh, gc_ = _geo(host), _geo(client)
    assert gh["funds"] == gc_["funds"], \
        f"{tag}: month-end funds differ: host={gh['funds']} client={gc_['funds']}"
    assert gh["maintenanceTail"] == gc_["maintenanceTail"], \
        f"{tag}: maintenance tail differs: host={gh['maintenanceTail']} " \
        f"client={gc_['maintenanceTail']}"
    print(f"PASS {tag}: monthsPassed {gh['monthsPassed']}, funds {gh['funds']}, "
          f"maintenance {gh['maintenanceTail']} - settled identically on both")

    # GAP-9 (fixed): the income/expenditure graph tails are now host-authoritative.
    # Every joint_apply carries the host's _incomes.back()/_expenditures.back() and
    # the replica adopts them verbatim (setFundsRaw + copy) instead of net-inferring
    # them from setFunds; the monthly settlement overwrites the just-rolled tail the
    # same way. So they must be exactly equal at the roll, like funds/maintenance.
    # (Pre-fix this drifted net-vs-gross and was only printed; test_joint_graphs is
    # the focused red/green repro.)
    assert gh["incomeTail"] == gc_["incomeTail"], \
        f"{tag}: income tail differs: host={gh['incomeTail']} client={gc_['incomeTail']}"
    assert gh["expenditureTail"] == gc_["expenditureTail"], \
        f"{tag}: expenditure tail differs: host={gh['expenditureTail']} " \
        f"client={gc_['expenditureTail']}"
    print(f"  PASS {tag}: income {gh['incomeTail']} + expenditure "
          f"{gh['expenditureTail']} tails equal on both (GAP-9)")
    return gh["monthsPassed"]


def _fly_a_battle(host, client):
    """The J09 recipe: mixed-owner squad -> seeded site -> land -> abort -> geoscape."""
    b0 = _base0(host)
    cid = next(c["id"] for c in b0["crafts"] if "SKYRANGER" in c["type"])
    rh = sorted(s["id"] for s in _roster(host))
    squad = [rh[0], rh[1]]

    # mixed ownership: seat 0 + seat 1 (scaffolding -> stamp on BOTH machines)
    for gc in (host, client):
        gc.ok({"cmd": "set_soldier_owner", "soldier_id": squad[0], "owner": 0})
        gc.ok({"cmd": "set_soldier_owner", "soldier_id": squad[1], "owner": 1})
    # the starting Skyranger ships FULL - empty it first (J09 harness finding)
    for sid in rh:
        host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": False})
    for sid in squad:
        host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})

    def _aboard(gc):
        return sorted(s["id"] for s in _roster(gc) if s["craftId"] == cid)

    for gc in (host, client):
        gc.wait_for("squad aboard", lambda gc=gc: (_aboard(gc) == sorted(squad)) or None,
                    timeout=40, interval=0.5)

    site = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                    "deployment": "STR_TERROR_MISSION", "lon": b0["lon"] + 0.35,
                    "lat": b0["lat"] + 0.10, "race": "STR_SECTOID", "hours": 240})
    sid_site = site["site_id"]
    host.wait_for("site on host",
                  lambda: any(s["id"] == sid_site for s in _geo(host)["missionSites"]) or None,
                  timeout=30)
    host.ok({"cmd": "craft_force", "craft_id": cid, "status": "STR_OUT",
             "lon": b0["lon"] + 0.34, "lat": b0["lat"] + 0.10,
             "dest": f"site:{sid_site}", "fuel": 999999, "lowFuel": False})

    def _prompt():
        if _has(host, "ConfirmLandingState"):
            return True
        # Unlike test_joint_battle (which flies from a fresh bootstrap), this craft
        # launches MONTHS in, with alien missions live: the globe throws popups
        # (monthly report, UFO/mission detected) that stall the shared clock -
        # co-op only advances while BOTH players sit on the geoscape at the same
        # speed, so a dialog on EITHER machine freezes the craft in mid-air.
        # Drain both; the landing prompt itself is never dismissed (dismiss_popup
        # would decline it), which is what the interest predicate protects.
        for gc in (host, client):
            geo.drain_popups(gc, interest=geo.popup("ConfirmLandingState"))
            gc.cmd({"cmd": "geo_set_speed", "idx": 2})  # not geo_run: it auto-declines
        return None

    host.wait_for("host landing prompt", _prompt, timeout=180, interval=0.5)
    host.ok({"cmd": "confirm_landing"})
    for gc in (host, client):
        gc.wait_for("entered the battle", lambda gc=gc: gc.cmd({"cmd": "battle_state"}).get("inBattle") or None,
                    timeout=180, interval=1.0)

    # the control split still comes from ownership, months into the campaign
    for tag, gc in (("host", host), ("client", client)):
        us = {u["soldierId"]: u for u in gc.cmd({"cmd": "battle_state"})["units"]
              if u["soldierId"] != -1}
        assert sorted(us) == sorted(squad), f"{tag}: deployed {sorted(us)} want {sorted(squad)}"
        assert us[squad[0]]["coop"] == 0 and us[squad[1]]["coop"] == 1, \
            f"{tag}: control split wrong: {[(s, us[s]['coop']) for s in squad]}"

    for gc in (host, client):
        gc.wait_for("briefing", lambda gc=gc: _has(gc, "BriefingState") or None,
                    timeout=120, interval=0.5)
        gc.ok({"cmd": "close_briefing"})
    for gc in (host, client):
        gc.wait_for("pre-battle inventory", lambda gc=gc: _has(gc, "InventoryState") or None,
                    timeout=120, interval=0.5)
        gc.ok({"cmd": "battle_inventory", "action": "ok"})
    for gc in (host, client):
        gc.wait_for("tactical", lambda gc=gc: _has(gc, "BattlescapeState") or None,
                    timeout=120, interval=0.5)
    host.ok({"cmd": "battle_action", "action": "abort"})

    def _drain(gc, deadline):
        while time.time() < deadline:
            if "GeoscapeState" in _states(gc)[-1]:
                return True
            gc.cmd({"cmd": "dismiss_popup"})
            time.sleep(0.4)
        return None

    deadline = time.time() + 200
    for gc in (host, client):
        gc.wait_for("back on the geoscape", lambda gc=gc: _drain(gc, deadline),
                    timeout=220, interval=1.0)
    return squad


def main():
    t_start = time.time()
    js = joint_fixture.bring_up("jmonth", (48800, 48801, 48100))
    host, client = js.host, js.client
    try:
        js.assert_world_equal("phase 0: bootstrap")

        # ================================================================
        # phase 1: put research + manufacture IN FLIGHT and keep them there.
        # ================================================================
        for gc in (host, client):
            gc.ok({"cmd": "discover_research", "topic": LASER})
        r = host.ok({"cmd": "start_research", "cost": BIG_RESEARCH, "scientists": 2})
        topic = r["topic"]
        client.ok({"cmd": "start_research", "topic": topic, "cost": BIG_RESEARCH,
                   "scientists": 2})
        # a long production at 1 engineer: it must still be running at the end
        # (J06 finding: a headless geoscape overshoots days per poll).
        pr = client.ok({"cmd": "manufacture_start", "item": LASER, "engineers": 1,
                        "qty": LONG_PROD})
        assert pr.get("sent"), f"manufacture_start not sent: {pr}"
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} production running",
                        lambda gc=gc: (_prod(gc, LASER) is not None) or None,
                        timeout=30, interval=0.5)
        assert topic in _research_names(host) and topic in _research_names(client), \
            "research project missing on one side"
        js.assert_world_equal("phase 1: research + manufacture running")
        print(f"PASS phase 1: research {topic} + {LONG_PROD}x {LASER} running on both")

        # ================================================================
        # phase 2: MONTH END #1, over a world with live projects.
        # ================================================================
        mp1 = _roll_month(host, client, "phase 2: month end #1")
        assert topic in _research_names(host) and topic in _research_names(client), \
            "research project vanished across the month boundary"
        assert _prod(host, LASER) and _prod(client, LASER), \
            "production vanished across the month boundary"
        js.assert_world_equal("phase 2: after month end #1")

        # ================================================================
        # phase 3: a mixed-owner battle mid-campaign. The post-battle restream
        #          replaces the replica's whole world - projects, month history
        #          and all - so equality here is a much bigger claim than in
        #          test_joint_battle, which flies from a fresh bootstrap.
        # ================================================================
        squad = _fly_a_battle(host, client)
        js.assert_world_equal("phase 3: after the battle restream", timeout=150)
        ids = [s["id"] for s in _roster(host)]
        for sid in squad:
            assert sid in ids, f"squad soldier {sid} was deleted post-battle"
        assert _prod(host, LASER) and _prod(client, LASER), \
            "the post-battle restream lost the running production"
        assert topic in _research_names(host) and topic in _research_names(client), \
            "the post-battle restream lost the running research project"
        print("PASS phase 3: battle flown mid-campaign; the restream preserved the "
              "running research + production and both worlds are one")

        # ================================================================
        # phase 4: MONTH END #2 on a post-restream world. The settlement writes
        #          funds + the maintenance/income/expenditure TAILS by index, so
        #          a restream that left the replica's month history a different
        #          length would settle onto the wrong slot right here.
        # ================================================================
        mp2 = _roll_month(host, client, "phase 4: month end #2")
        assert mp2 == mp1 + 1, f"expected monthsPassed {mp1 + 1}, got {mp2}"
        js.assert_world_equal("phase 4: after month end #2")

        js.finish()
        mins = (time.time() - t_start) / 60.0
        print(f"ALL JOINT MONTH-RUN TESTS PASSED (2 month ends + 1 battle, "
              f"{mins:.1f} min)")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
