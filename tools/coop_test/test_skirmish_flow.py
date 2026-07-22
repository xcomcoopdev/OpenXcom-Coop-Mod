"""Skirmish (NEW BATTLE > COOP) lobby flow, the "player joined" popup, and the
"Connecting..." dialog lifecycle.

The skirmish lobby is session.lobbyMode 0, reachable ONLY from
Main Menu > NEW BATTLE > COOP (the New Game dropdown always yields a campaign
lobby). Desired flow:

  1. host: NEW BATTLE > COOP > Host > START HOST            -> lobby
  2. client joins
  3. BOTH machines show the "player joined" popup ON TOP of the lobby
  4. host's action button reads BATTLE SETTINGS; the client has no button
  5. host presses BATTLE SETTINGS -> host returns to the NEW BATTLE setup
     screen to finalise craft/equipment/enemies; the CLIENT STAYS in the lobby
  6. the setup screen's COOP button re-opens the lobby, which still offers
     BATTLE SETTINGS, so the host can bounce between the two
  7. host presses OK on the setup screen -> the client leaves the lobby and
     both machines reach the battlescape

What used to be wrong:
  - the popup was suppressed entirely for campaign lobbies (a lobbyMode gate),
    and on the skirmish client it was pushed BEFORE the lobby, so the lobby
    drew over it;
  - the skirmish lobby ran a mutual READY dance with a 30s countdown that
    neither player controlled, and the client had a live READY button, and the
    host still had to walk back to the setup screen by hand;
  - a "Connecting..." wait dialog (CoopState 15) was only retired on one client
    success path, so a failed join sat on it forever and a refused join stacked
    its error popup on top of it.

Run:  python tools/coop_test/test_skirmish_flow.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT
import session

CONNECTING = 15  # CoopState code for the "Connecting..." wait dialog


def states(gc):
    return [s.replace("class OpenXcom::", "") for s in session.states(gc)]


def top_state(gc):
    return states(gc)[-1]


def connecting_anywhere(gc):
    """True if a "Connecting..." dialog sits ANYWHERE in the stack, not just on
    top - the bug being guarded is precisely one left lurking underneath."""
    return gc.cmd({"cmd": "coop_connecting_dialogs"}).get("count", 0) > 0


def lobby(gc):
    return gc.cmd({"cmd": "lobby_state"})


def skirmish_host(host, port, player="HostPlayer", password=None):
    """Host a skirmish through the real UI: NEW BATTLE > COOP > Host > START HOST."""
    host.ok({"cmd": "open_new_battle"})
    host.wait_for("host new battle", lambda: session.has_state(host, "NewBattleState"))
    host.ok({"cmd": "newbattle_coop"})
    host.wait_for("host browser", lambda: session.has_state(host, "ServerList"))
    host.ok({"cmd": "server_list_host"})
    host.wait_for("host window", lambda: session.has_state(host, "HostMenu"))
    req = {"cmd": "host_menu_host", "visibility": 0, "server": "TestSrv",
           "port": port, "player": player}
    if password:
        req["password"] = password
    host.ok(req)
    host.wait_for("host lobby", lambda: session.has_state(host, "LobbyMenu"))


def skirmish_client_at_browser(client):
    """Client sitting in the server browser off the NEW BATTLE screen."""
    client.ok({"cmd": "open_new_battle"})
    client.wait_for("client new battle", lambda: session.has_state(client, "NewBattleState"))
    client.ok({"cmd": "newbattle_coop"})
    client.wait_for("client browser", lambda: session.has_state(client, "ServerList"))


def test_skirmish_full_flow():
    """Steps 1-7 end to end, asserting popup placement, button roles, the
    host's round trip to the setup screen, and that the client only leaves the
    lobby when the host presses OK."""
    port = "47970"
    host = GameClient("host", 48760, make_user_dir("skirm_flow_host"))
    client = GameClient("client", 48761, make_user_dir("skirm_flow_client"))
    try:
        host.spawn(); host.connect()
        client.spawn(); client.connect()

        # 1. host opens the skirmish lobby
        skirmish_host(host, port)
        st = lobby(host)
        assert st["lobbyMode"] == 0, f"expected the skirmish lobby (mode 0): {st}"
        assert st["buttonText"] == "BATTLE SETTINGS", \
            f"host button should read BATTLE SETTINGS, got {st['buttonText']!r}"
        assert not st["buttonVisible"], \
            "BATTLE SETTINGS must stay hidden until a peer is actually in the lobby"

        # 2. client joins
        skirmish_client_at_browser(client)
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": port, "player": "ClientPlayer"})

        # 3. BOTH machines show the popup, and it is ON TOP of the lobby
        host.wait_for("host popup", lambda: session.has_state(host, "Profile"))
        client.wait_for("client popup", lambda: session.has_state(client, "Profile"))

        assert top_state(host) == "Profile", \
            f"host: join popup must be the top state, stack={states(host)}"
        assert top_state(client) == "Profile", \
            f"client: join popup must be the top state (it used to be buried " \
            f"under the lobby), stack={states(client)}"
        # and the lobby is underneath it on both, so it renders behind the popup
        assert "LobbyMenu" in states(host), f"host lost its lobby: {states(host)}"
        assert "LobbyMenu" in states(client), f"client has no lobby: {states(client)}"
        print("PASS step 3: join popup on top of the lobby on BOTH machines")

        # the connect attempt resolved: no "Connecting..." left anywhere
        assert not connecting_anywhere(client), \
            f"client left a Connecting... dialog in the stack: {states(client)}"
        print("PASS step 3: no lingering Connecting... dialog after a successful join")

        # 4. dismiss the popups; host owns the only action button
        host.ok({"cmd": "profile_ok"})
        client.ok({"cmd": "profile_ok"})
        host.wait_for("start offered", lambda: lobby(host).get("buttonVisible") or None)

        hs, cs = lobby(host), lobby(client)
        assert hs["buttonText"] == "BATTLE SETTINGS", \
            f"host button should read BATTLE SETTINGS, got {hs['buttonText']!r}"
        assert hs["buttonVisible"], "host's BATTLE SETTINGS must be visible once the peer is in"
        assert not cs["buttonVisible"], \
            f"client must have NO action button, got {cs['buttonText']!r} visible"
        print("PASS step 4: host BATTLE SETTINGS visible, client button hidden")

        # no ready dance survives: neither roster row carries a READY marker
        for who, snap in (("host", hs), ("client", cs)):
            for name in snap.get("players") or []:
                assert "READY" not in name, f"{who}: stale ready marker in roster: {name!r}"
        print("PASS step 4: READY/NOT READY dance is gone")

        # 5. host steps out to the battle settings; the CLIENT STAYS in the lobby
        host.ok({"cmd": "lobby_action"})
        host.wait_for("host at battle settings",
                      lambda: (not session.has_state(host, "LobbyMenu")) or None)
        assert states(host)[-1] == "NewBattleState", \
            f"host should land on the NEW BATTLE setup screen, stack={states(host)}"
        time.sleep(2)
        assert session.has_state(client, "LobbyMenu"), \
            f"client must STAY in the lobby while the host configures: {states(client)}"
        assert not lobby(client)["sessionLocked"], \
            "nothing has started yet, so the session must not be locked"
        print("PASS step 5: host at BATTLE SETTINGS, client still waiting in the lobby")

        # 6. the COOP button re-opens the lobby, still offering BATTLE SETTINGS
        host.ok({"cmd": "newbattle_coop"})
        host.wait_for("host lobby again", lambda: session.has_state(host, "LobbyMenu"))
        host.wait_for("button offered again",
                      lambda: lobby(host).get("buttonVisible") or None)
        again = lobby(host)
        assert again["buttonText"] == "BATTLE SETTINGS",             f"re-opened lobby should still offer BATTLE SETTINGS, got {again['buttonText']!r}"
        print("PASS step 6: COOP re-opens the lobby, BATTLE SETTINGS still offered")

        # back out to the settings screen again, then start for real
        host.ok({"cmd": "lobby_action"})
        host.wait_for("host at battle settings again",
                      lambda: (not session.has_state(host, "LobbyMenu")) or None)

        # 7. OK on the setup screen starts the battle for BOTH machines
        host.ok({"cmd": "newbattle_ok"})

        def in_battle(gc):
            st = states(gc)
            return ("BriefingState" in st or "BattlescapeState" in st
                    or "InventoryState" in st or "NextTurnState" in st) or None

        host.wait_for("host in battle", lambda: in_battle(host), timeout=120)
        client.wait_for("client in battle", lambda: in_battle(client), timeout=120)
        assert "LobbyMenu" not in states(host), f"host still in the lobby: {states(host)}"
        assert "LobbyMenu" not in states(client), \
            f"client left sitting on a dead lobby it cannot dismiss: {states(client)}"
        print(f"PASS step 7: both reached the battle (host={states(host)}, client={states(client)})")
    finally:
        host.shutdown(); client.shutdown()


def test_campaign_join_popup_over_lobby():
    """The campaign lobby shows the popup too (the lobbyMode gate is gone), and
    the campaign button labels are UNCHANGED."""
    port = "47971"
    host = GameClient("host", 48762, make_user_dir("skirm_camp_host"))
    client = GameClient("client", 48763, make_user_dir("skirm_camp_client"))
    try:
        host.spawn(); host.connect()
        client.spawn(); client.connect()

        host.ok({"cmd": "open_new_game", "mode": "coop"})
        host.wait_for("difficulty", lambda: session.has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("host window", lambda: session.has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": port, "player": "HostPlayer"})
        host.wait_for("host lobby", lambda: session.has_state(host, "LobbyMenu"))

        assert lobby(host)["lobbyMode"] == 1, "expected a NEW campaign lobby (mode 1)"

        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": port, "player": "ClientPlayer"})
        host.wait_for("host popup", lambda: session.has_state(host, "Profile"))
        client.wait_for("client popup", lambda: session.has_state(client, "Profile"))

        assert top_state(host) == "Profile", f"host stack={states(host)}"
        assert top_state(client) == "Profile", f"client stack={states(client)}"
        assert "LobbyMenu" in states(client), f"client lobby missing: {states(client)}"
        print("PASS campaign: join popup on top of the campaign lobby on both machines")

        assert not connecting_anywhere(client), \
            f"client left a Connecting... dialog: {states(client)}"

        # REGRESSION GUARD: campaign labels must not have become START BATTLE
        host.ok({"cmd": "profile_ok"})
        client.ok({"cmd": "profile_ok"})
        host.wait_for("start eligible", lambda: lobby(host).get("startEligible") or None)
        hs = lobby(host)
        assert hs["buttonText"] == "START CAMPAIGN", \
            f"campaign label regressed to {hs['buttonText']!r}"
        assert not lobby(client)["buttonVisible"], "campaign client must have no button"
        print("PASS campaign: label still START CAMPAIGN, client still buttonless")

        # dismissing the popup over a lobby must NOT kick off a world request
        # (that path belongs to the classic bare join only)
        assert not connecting_anywhere(client)
        assert lobby(client)["lobbyOpen"], "client fell out of the lobby after the popup"
        print("PASS campaign: popup dismissal did not trigger a world load")
    finally:
        host.shutdown(); client.shutdown()


def test_failed_join_clears_connecting():
    """A join to a port nobody is listening on must not leave the player staring
    at a "Connecting..." dialog forever."""
    client = GameClient("client", 48764, make_user_dir("skirm_fail_client"))
    try:
        client.spawn(); client.connect()
        skirmish_client_at_browser(client)

        # nothing is listening on this port
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47899", "player": "ClientPlayer"})

        # the attempt must resolve and retire the wait dialog
        client.wait_for(
            "connect attempt resolved",
            lambda: (not connecting_anywhere(client)) or None,
            timeout=120,
        )
        assert not connecting_anywhere(client), \
            f"Connecting... dialog still lurking: {states(client)}"
        print(f"PASS failed join: Connecting... retired, stack={states(client)}")
    finally:
        client.shutdown()


def main():
    test_skirmish_full_flow()
    test_campaign_join_popup_over_lobby()
    test_failed_join_clears_connecting()
    print("ALL SKIRMISH FLOW TESTS PASSED")


if __name__ == "__main__":
    main()
