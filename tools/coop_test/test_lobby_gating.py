"""Regression tests for two lobby/base-building gating bugs:

1. START CAMPAIGN must be ineligible while the host is ALONE in the lobby
   (was: onConnect==1 as soon as the host listens, so a lone host could
   start).
2. The player who places their base FIRST must hold in a
   "Waiting for all players to place their bases..." dialog with frozen
   time until the host resumes (was: the client landed on a live, ticking
   geoscape while the host was still placing).

Run:  python tools/coop_test/test_lobby_gating.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT
import session
import geo  # PRD-13 S7: geo.top_state (safe on empty stack)


def client_clock(gc):
    r = gc.cmd({"cmd": "geo_state"})
    if not r.get("ok"):
        return None
    t = r["time"]
    return (t["year"], t["month"], t["day"], t["hour"], t["minute"])


def main():
    host = GameClient("host", 48650, make_user_dir("gating_host"))
    client = GameClient("client", 48651, make_user_dir("gating_client"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        # --- Bug 1: lone host must not be able to START ---
        host.ok({"cmd": "open_new_game", "mode": "coop"})
        host.wait_for("difficulty", lambda: session._has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("host window", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47900", "player": "HostPlayer"})
        host.wait_for("host lobby", lambda: session._has_state(host, "LobbyMenu"))

        # give the host thread time to settle in its (empty) listening state
        time.sleep(3)
        ls = host.ok({"cmd": "lobby_state"})
        assert not ls["startEligible"], f"BUG1: lone host is start-eligible: {ls}"
        r = host.cmd({"cmd": "lobby_start_campaign"})
        assert not r.get("ok"), f"BUG1: lone host could START CAMPAIGN: {r}"
        print("PASS bug1: lone host cannot start the campaign")

        # --- Bug 2: first placer waits with frozen time ---
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47900", "player": "ClientPlayer"})
        client.wait_for("client lobby", lambda: session._has_state(client, "LobbyMenu"))
        host.wait_for(
            "start eligible with client",
            lambda: host.cmd({"cmd": "lobby_state"}).get("startEligible") or None,
        )
        host.ok({"cmd": "lobby_start_campaign"})

        # CLIENT places first, before the host
        client.wait_for("client base placement", lambda: session._has_state(client, "BuildNewBaseState"))
        client.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "ClientBase"})

        # the client must be held in a waiting dialog, not on a live geoscape
        client.wait_for(
            "client waiting dialog",
            lambda: ("CoopState" in geo.top_state(client)) or None,
            timeout=30,
        )
        print("PASS bug2a: first placer holds in the waiting dialog")

        # and its clock must not advance while waiting
        t0 = client_clock(client)
        time.sleep(5)
        t1 = client_clock(client)
        if t0 != t1:
            print("DEBUG client states:", client.cmd({"cmd": "get_state"})["states"])
            print("DEBUG host   states:", host.cmd({"cmd": "get_state"})["states"])
        assert t0 == t1, f"BUG2: client clock advanced while waiting: {t0} -> {t1}"
        print("PASS bug2b: client clock frozen while waiting")

        # host places, waits for the blob, resumes
        host.wait_for("host base placement", lambda: session._has_state(host, "BuildNewBaseState"))
        r = host.cmd({"cmd": "place_first_base", "lon": 0.35, "lat": 0.85, "name": "HostBase"})
        if not r.get("ok"):
            host.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "HostBase"})
        host.wait_for(
            "all players placed",
            lambda: host.cmd({"cmd": "has_coop_file",
                              "key": f"host_{host.cmd({'cmd': 'save_markers'})['saveID']}_ClientPlayer.data"}).get("present") or None,
            timeout=120,
        )
        host.ok({"cmd": "coop_dialog_back"})

        # the client's waiting dialog must clear and the session comes up
        client.wait_for(
            "client released to geoscape",
            lambda: ("GeoscapeState" in geo.top_state(client)) or None,
            timeout=60,
        )
        client.wait_for(
            "session up",
            lambda: (lambda c: (c.get("lobbyClosed") and c.get("hasSave")) or None)(client.cmd({"cmd": "get_coop"})),
            timeout=120,
        )
        print("PASS bug2c: RESUME releases the waiting player, session up")

        session.assert_client_zero_disk(client.user_dir)

        # --- C4 (PRD-10): client drop while the confirm dialog is open ---
        # A client leaving after the host opens the START CAMPAIGN confirm
        # dialog must (a) auto-dismiss the dialog and (b) never start the
        # campaign, even if the host clicks OK. Drives the REAL confirm dialog.
        host.shutdown(); client.shutdown()
        c4_host_dir = make_user_dir("c4_host")
        host = GameClient("host", 48652, c4_host_dir)
        client = GameClient("client", 48653, make_user_dir("c4_client"))
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        host.ok({"cmd": "open_new_game", "mode": "coop"})
        host.wait_for("difficulty", lambda: session._has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("host window", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47901", "player": "HostPlayer"})
        host.wait_for("host lobby", lambda: session._has_state(host, "LobbyMenu"))
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47901", "player": "ClientPlayer"})
        client.wait_for("client lobby", lambda: session._has_state(client, "LobbyMenu"))
        host.wait_for(
            "start eligible with client",
            lambda: host.cmd({"cmd": "lobby_state"}).get("startEligible") or None,
        )

        # open the REAL confirm dialog (not the startCampaign bypass)
        host.ok({"cmd": "lobby_start_campaign", "confirm": "dialog"})
        host.wait_for(
            "confirm dialog up",
            lambda: ("ConfirmStartCampaignState" in geo.top_state(host)) or None,
            timeout=15,
        )

        # hard-kill the lone client (abrupt drop, no graceful quit)
        client.proc.kill()

        # (a) the dialog auto-dismisses once the host notices the drop
        host.wait_for(
            "confirm dialog auto-dismissed",
            lambda: ("ConfirmStartCampaignState" not in geo.top_state(host)) or None,
            timeout=30,
        )
        assert not host.cmd({"cmd": "lobby_state"}).get("startEligible"), \
            "C4: host still start-eligible after the lone client left"
        print("PASS C4a: confirm dialog auto-dismissed on client drop")

        # (b) clicking OK now must not start the campaign (dialog gone -> no-op;
        # and even had it lingered, btnOkClick re-checks eligibility)
        host.ok({"cmd": "lobby_confirm_ok"})
        time.sleep(2)
        ls = host.ok({"cmd": "lobby_state"})
        assert ls.get("lobbyOpen") and ls.get("lobbyMode") == 1 and not ls.get("sessionLocked"), \
            f"C4: campaign started after client left (lobby_state={ls})"
        markers = host.cmd({"cmd": "save_markers"})
        assert not markers.get("coopPlayers"), \
            f"C4: roster locked with a departed player: {markers}"
        assert not os.path.exists(os.path.join(c4_host_dir, "xcom1", "_autogeo_.asav")), \
            "C4: _autogeo_.asav written despite the aborted start"
        print("PASS C4b: client drop before OK does not start the campaign")

        print("ALL LOBBY GATING TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
