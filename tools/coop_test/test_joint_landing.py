"""PRD-J10: the landing broker (the behaviour PRD-J09 deferred).

The host runs the only geoscape simulation, so ConfirmLandingState pops on the
HOST even for a craft the CLIENT ordered out - J09 shipped it that way and flagged
it. J10 re-addresses the question to the seat that gave the order.

This is UX ROUTING ONLY. Battle authority does not move: the coop battle is a
lockstep parallel sim (both machines load the same "battlehost" blob), so if the
commanding seat says yes, the HOST still generates the battle exactly as before.
test_joint_battle.py owns that half; this test owns the routing.

  ROUTE    the client orders the shared craft to a site (craft_launch records the
           commanding seat) -> on arrival the HOST does NOT pop the dialog, it
           brokers; the CLIENT gets the ConfirmLandingState.
  DECLINE  the client answers NO -> the HOST applies it (craft returns to base)
           and the pending prompt clears. The client mutated nothing locally.
  RE-ASK   the host does not re-prompt on every tick while a decision is pending
           (the reachedDestination loop re-enters ~every 5 game seconds).
  HOST     a craft the HOST commanded still pops the dialog on the HOST (the
           vanilla path is untouched for seat 0).
  CONFIRM  the client answers YES -> the HOST generates the authoritative battle
           and BOTH machines enter it. This is the half that proves routing did
           not move battle authority: the client never generates anything.

Run:  python tools/coop_test/test_joint_landing.py
"""

import os
import sys

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


def _skyranger(gc):
    for c in _base0(gc)["crafts"]:
        if "SKYRANGER" in c["type"]:
            return c
    raise AssertionError("no skyranger")


def _craft(gc, cid):
    for c in _base0(gc)["crafts"]:
        if c["id"] == cid:
            return c
    raise AssertionError(f"craft {cid} gone")


def _states(gc):
    return gc.cmd({"cmd": "get_state"})["states"]


def _has(gc, name):
    return any(name in s for s in _states(gc))


def _pending(gc):
    return gc.ok({"cmd": "joint_landing_state"})["pending"]


def _fly_to_site(host, cid, site_id, blon, blat):
    """Park the shared craft next to the site so the host's sim reaches it. Only
    the host is forced - craft_force never touches the commanding seat, which the
    craft_launch joint_apply already recorded on both machines."""
    host.ok({"cmd": "craft_force", "craft_id": cid, "status": "STR_OUT",
             "lon": blon + 0.34, "lat": blat + 0.10, "dest": f"site:{site_id}",
             "fuel": 999999, "lowFuel": False})


def main():
    js = joint_fixture.bring_up("jland", (48770, 48771, 48070))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]
        cid = _skyranger(host)["id"]
        print(f"PASS setup: shared skyranger {cid} at the starting base")

        # ================================================================
        # 1) ROUTE: the CLIENT orders the craft out -> the commanding seat is
        #    recorded from the craft_launch joint_apply on BOTH machines.
        # ================================================================
        site = host.ok({"cmd": "spawn_mission_site", "mission": "STR_ALIEN_TERROR",
                        "deployment": "STR_TERROR_MISSION", "lon": blon + 0.35,
                        "lat": blat + 0.10, "race": "STR_SECTOID", "hours": 240})
        site_id = site["site_id"]
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"site replicated to {tag}",
                        lambda gc=gc: any(s["id"] == site_id for s in _geo(gc)["missionSites"]) or None,
                        timeout=60, interval=0.5)

        r = client.ok({"cmd": "craft_order", "order": "target", "craft_id": cid,
                       "craft_type": _skyranger(client)["type"], "site_id": site_id})
        assert r.get("ok") or r.get("sent"), f"client craft_order not sent: {r}"
        host.wait_for("host applied the client's craft order",
                      lambda: (_craft(host, cid)["destKind"] == "site") or None,
                      timeout=30, interval=0.5)
        print(f"PASS order: CLIENT commanded craft {cid} to site {site_id}")

        _fly_to_site(host, cid, site_id, blon, blat)

        def _brokered():
            if _pending(host):
                return True
            host.cmd({"cmd": "geo_set_speed", "idx": 2})  # not geo_run: it auto-declines
            return None

        host.wait_for("host brokered the landing prompt", _brokered, timeout=90, interval=0.5)
        # THE assertion: the host asked someone else instead of asking itself.
        assert not _has(host, "ConfirmLandingState"), \
            "host popped its own ConfirmLandingState for a CLIENT-commanded craft"
        client.wait_for("client got the brokered ConfirmLandingState",
                        lambda: _has(client, "ConfirmLandingState") or None,
                        timeout=60, interval=0.5)
        print("PASS route: the host brokered the prompt (none on the host); the "
              "COMMANDING SEAT holds the ConfirmLandingState")

        # ================================================================
        # 2) RE-ASK: reachedDestination re-enters every ~5 game seconds. The
        #    pending guard must stop it re-broadcasting the prompt forever.
        # ================================================================
        host.ok({"cmd": "geo_set_speed", "idx": 2})
        client_dialogs = sum(1 for s in _states(client) if "ConfirmLandingState" in s)
        assert client_dialogs == 1, \
            f"client is stacking brokered dialogs ({client_dialogs}) - the host is re-asking"
        print("PASS re-ask guard: exactly one brokered dialog while the decision is pending")

        # ================================================================
        # 3) DECLINE: the client says NO -> the HOST applies the consequence.
        #    The replica mutated nothing locally; the answer travelled home.
        # ================================================================
        client.ok({"cmd": "decline_landing"})
        host.wait_for("host cleared the pending landing",
                      lambda: (not _pending(host)) or None, timeout=60, interval=0.5)
        host.wait_for("host sent the craft home",
                      lambda: (_craft(host, cid)["destKind"] == "base") or None,
                      timeout=60, interval=0.5)
        assert not _has(client, "ConfirmLandingState"), "client dialog did not close"
        assert not _has(host, "ConfirmLandingState"), "host popped a dialog on decline"
        assert host.cmd({"cmd": "ping"}).get("pong"), "host unresponsive after the decline"
        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive after the decline"
        print("PASS decline: the client's NO reached the host, which returned the "
              "craft to base; no dialog left on either machine")

        # ================================================================
        # 4) HOST-COMMANDED craft: the vanilla path must be untouched for seat 0.
        # ================================================================
        r = host.ok({"cmd": "craft_order", "order": "target", "craft_id": cid,
                     "craft_type": _skyranger(host)["type"], "site_id": site_id})
        assert r.get("ok") or r.get("sent"), f"host craft_order not sent: {r}"
        host.wait_for("host applied its own craft order",
                      lambda: (_craft(host, cid)["destKind"] == "site") or None,
                      timeout=30, interval=0.5)
        _fly_to_site(host, cid, site_id, blon, blat)

        def _host_prompt():
            if _has(host, "ConfirmLandingState"):
                return True
            host.cmd({"cmd": "geo_set_speed", "idx": 2})
            return None

        host.wait_for("host got its OWN landing prompt", _host_prompt, timeout=90, interval=0.5)
        assert not _pending(host), "a host-commanded craft must not be brokered"
        assert not _has(client, "ConfirmLandingState"), \
            "client got a dialog for a HOST-commanded craft"
        host.ok({"cmd": "decline_landing"})
        print("PASS seat-0 craft: the host's own order still pops the host's own "
              "dialog (vanilla path untouched)")

        # ================================================================
        # 5) CONFIRM: the client answers YES. The HOST - still the battle
        #    authority - generates the battle and ships "battlehost"; both
        #    machines enter it. Routing moved the QUESTION, not the authority.
        # ================================================================
        # Two declines have left the craft flying home (and it may be refuelling by
        # now, which the host's launch validator rejects). Reset it to READY on the
        # HOST - the authoritative side the validator reads - so the phase under
        # test is the broker, not the fuel bookkeeping.
        host.ok({"cmd": "craft_force", "craft_id": cid, "status": "STR_READY",
                 "fuel": 999999, "lowFuel": False, "mission": False, "dest": "patrol"})
        r = client.ok({"cmd": "craft_order", "order": "target", "craft_id": cid,
                       "craft_type": _skyranger(client)["type"], "site_id": site_id})
        assert r.get("ok") or r.get("sent"), f"client re-order not sent: {r}"
        host.wait_for("host applied the client's re-order",
                      lambda: (_craft(host, cid)["destKind"] == "site") or None,
                      timeout=30, interval=0.5)
        _fly_to_site(host, cid, site_id, blon, blat)
        host.wait_for("host brokered the second prompt", _brokered, timeout=90, interval=0.5)
        client.wait_for("client got the second brokered dialog",
                        lambda: _has(client, "ConfirmLandingState") or None,
                        timeout=60, interval=0.5)
        assert not _has(host, "ConfirmLandingState"), "host popped its own dialog again"

        client.ok({"cmd": "confirm_landing"})
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} entered the brokered battle",
                        lambda gc=gc: gc.cmd({"cmd": "battle_state"}).get("inBattle") or None,
                        timeout=180, interval=1.0)
        assert not _pending(host), "pending landing not cleared after the confirm"
        print("PASS confirm: the client's YES reached the host, which generated the "
              "authoritative battle - BOTH machines entered it")

        # PRD-J11: the shared final-state assertions (world equality +
        # the replica's zero-disk invariant).
        js.finish()

        print("ALL JOINT LANDING TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
