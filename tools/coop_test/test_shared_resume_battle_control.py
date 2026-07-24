"""Mid-battle coop RESUME must restore split control (SHARED mode).

SHARED is strictly worse off than SEPARATE here. Besides the shared COOP_READY
re-arm gap (coopSession/_battleInit never restored -> control not split), the
SHARED branch of `request_load_progress` (connectionTCP.cpp:3734, comment: "Battle-
scape resume stays 2-player/out of scope") streams ONLY the geoscape world on
resume and never arms `resumeBattlePending`, so the host's resume_ack handler
takes the plain campaign-resume path and NEVER emits `campaign_resume_battle`.
The client is therefore never streamed the battle at all - it never re-enters the
battlescape.

This test:

  1. SHARED campaign; a MIXED-ownership squad on the shared craft - seat 0 (host-
     owned, coop==0 in battle) + seat 1 (client-owned, coop==1). The split rides
     the shared battlehost blob, so it is asserted on BOTH machines live.
  2. Enter the SHARED battle (host confirms; NO two-world merge) and reach the
     battlescape; SANITY-check the live split.
  3. Host saves mid-battle, both instances killed, host relaunches (client dir
     empty), pair RESUMES.
  4. ASSERT the split is restored after resume. On the unfixed engine this FAILS
     up front: the client never enters the battle (bounded wait -> assertion,
     never a hang), so the resume helper reports a clean failure.

Run:  python tools/coop_test/test_shared_resume_battle_control.py
Exit 0 = pass; 2 = failure (client never resumed into battle / split broken).
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import shared_fixture
import session
# reuse the split probe + assertion from the SEPARATE test (same invariant)
import test_coop_resume_battle_control as rc

SAVE = "shared_resume_battle.sav"
PORTS = (48780, 48781, 48082)   # host test / client test / coop session
RESUME_PORT = "48083"


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


def roster(gc):
    out = []
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        out.extend(b["soldiers"])
    return out


def skyranger(gc):
    for c in base0(gc)["crafts"]:
        if "SKYRANGER" in c["type"]:
            return c
    raise AssertionError("no skyranger")


def bring_up_shared_battle(js):
    """Seat a seat0/seat1 mixed squad on the shared craft and enter the SHARED
    battle; drive to the battlescape on both machines. Mirrors test_shared_battle."""
    host, client = js.host, js.client
    b0 = base0(host)
    blon, blat = b0["lon"], b0["lat"]
    cid = skyranger(host)["id"]
    rh = sorted(s["id"] for s in roster(host))
    squad = [rh[0], rh[1]]
    owners = {0: 0, 1: 1}   # seat 0 -> host, seat 1 -> client

    for gc in (host, client):
        for slot, sid in enumerate(squad):
            gc.ok({"cmd": "set_soldier_owner", "soldier_id": sid, "owner": owners[slot]})
    for sid in rh:
        host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": False})
    for sid in squad:
        host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})

    def aboard(gc):
        return sorted(s["id"] for s in roster(gc) if s["craftId"] == cid)

    for gc, tag in ((host, "host"), (client, "client")):
        gc.wait_for(f"{tag} squad aboard",
                    lambda gc=gc: (aboard(gc) == sorted(squad)) or None, timeout=40, interval=0.5)
    print(f"shared squad {squad} aboard craft {cid} (seat0->host coop0, seat1->client coop1)")

    site = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                    "deployment": "STR_TERROR_MISSION", "lon": blon + 0.35,
                    "lat": blat + 0.10, "race": "STR_SECTOID", "hours": 240})
    site_id = site["site_id"]
    host.wait_for("site on host",
                  lambda: any(s["id"] == site_id for s in geo(host)["missionSites"]) or None, timeout=30)
    host.ok({"cmd": "craft_force", "craft_id": cid, "status": "STR_OUT",
             "lon": blon + 0.34, "lat": blat + 0.10, "dest": f"site:{site_id}",
             "fuel": 999999, "lowFuel": False})

    def landing_prompt():
        if has(host, "ConfirmLandingState"):
            return True
        host.cmd({"cmd": "geo_set_speed", "idx": 2})
        return None

    host.wait_for("host landing prompt", landing_prompt, timeout=90, interval=0.5)
    host.ok({"cmd": "confirm_landing"})
    for gc, tag in ((host, "host"), (client, "client")):
        gc.wait_for(f"{tag} entered the SHARED battle",
                    lambda gc=gc: rc.battle(gc).get("inBattle") or None, timeout=180, interval=1.0)

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
    rc.drain_to_tactical(host, client)
    print("both machines reached the battlescape (live SHARED coop battle)")
    return squad


def main():
    js = shared_fixture.bring_up("srbc", PORTS)
    host, client = js.host, js.client
    host_dir = js.host_dir
    fail = None
    host2 = client2 = None
    try:
        bring_up_shared_battle(js)

        # sanity: the LIVE split works (proves the test/setup is sound)
        rc.settle_and_assert(host, client, "live battle entry")

        # host saves the SHARED world mid-battle, both instances down.
        host.ok({"cmd": "save_game", "file": SAVE})
        assert os.path.exists(os.path.join(host_dir, "xcom1", SAVE)), "save not on disk"
        print(f"host saved mid-battle -> {SAVE}")
        js.shutdown()

        # relaunch: host reuses its dir (save present); client gets an EMPTY dir.
        host2 = GameClient("host", 48782, host_dir)
        client2 = GameClient("client", 48783, make_user_dir("srbc_client2"))
        host2.spawn(); client2.spawn()
        host2.connect(); client2.connect()

        # resume the mid-battle SHARED save. On the unfixed engine the client is
        # never streamed the battle, so this raises TimeoutError (bounded, not a
        # hang). The bound is generous vs a fixed resume (the SEPARATE resume
        # reaches both-inBattle in seconds) but far below a hang. If it DOES enter,
        # assert the split too.
        session.resume_campaign_battle(host2, client2, SAVE, port=RESUME_PORT, timeout=120)
        rc.assert_split(host2, client2, "after resume")

        session.assert_client_zero_disk(client2.user_dir)
        print("PASS zero-disk: resumed client user dir clean")
        print("ALL SHARED MID-BATTLE RESUME TESTS PASSED")
    except Exception as e:
        fail = e
        print(f"[FAIL] {e}")
    finally:
        if host2:
            host2.shutdown()
        if client2:
            client2.shutdown()
        js.shutdown()

    if fail:
        sys.exit(2)
    sys.exit(0)


if __name__ == "__main__":
    main()
