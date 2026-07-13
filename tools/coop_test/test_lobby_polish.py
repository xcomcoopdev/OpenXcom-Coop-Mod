"""Regression tests for four lobby/flow polish bugs:

2. HostMenu CANCEL during a co-op campaign must return to the MAIN MENU,
   not drop the host onto a live solo geoscape.
3. Host disconnect from a resume lobby + re-host must land back in a
   RESUME lobby (was: lobbyMode reset -> legacy READY-dance lobby).
4. Dismissing "You are not a player in this campaign." must also clear the
   "Connecting..." dialog left underneath.
5. A refused (wrong-name) join attempt must be invisible to the host: no
   roster entry, no "... has left the server" popup.

Run:  python tools/coop_test/test_lobby_polish.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo  # PRD-13 S7: geo.top_state (safe on empty stack)

SAVE = "polish_e2e.sav"


def main():
    host_dir = make_user_dir("polish_host")
    host = GameClient("host", 48660, host_dir)
    client = GameClient("client", 48661, make_user_dir("polish_client"))
    imposter = None
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        # --- Bug 2: HostMenu cancel -> main menu ---
        host.ok({"cmd": "open_new_game", "mode": "coop"})
        host.wait_for("difficulty", lambda: session._has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("host window", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_menu_cancel"})
        host.wait_for("main menu after cancel", lambda: ("MainMenuState" in geo.top_state(host)) or None, timeout=30)
        assert not session._has_state(host, "GeoscapeState"), \
            f"BUG2: host still has a live world after cancel: {host.cmd({'cmd': 'get_state'})['states']}"
        print("PASS bug2: host-window cancel returns to the main menu")

        # --- set up a campaign + save for the remaining bugs ---
        session.new_campaign(host, client)
        host.ok({"cmd": "save_game_ui", "type": "quick"})
        host.wait_for(
            "host quicksave",
            lambda: os.path.exists(os.path.join(host_dir, "xcom1", "_quick_.asav")) or None,
            timeout=60,
        )
        host.ok({"cmd": "save_game", "file": SAVE})
        host.shutdown(); client.shutdown()

        host = GameClient("host", 48662, host_dir)
        host.spawn(); host.connect()
        host.ok({"cmd": "load_save_menu", "file": SAVE})
        host.wait_for("host window (resume)", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47900", "player": "HostPlayer"})
        host.wait_for("resume lobby", lambda: session._has_state(host, "LobbyMenu"))
        assert host.ok({"cmd": "lobby_state"})["lobbyMode"] == 2

        # --- Bug 3: disconnect + re-host must stay a RESUME lobby, no READY ---
        host.ok({"cmd": "lobby_disconnect"})
        host.wait_for("back at host window", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47901", "player": "HostPlayer"})
        host.wait_for("second lobby", lambda: session._has_state(host, "LobbyMenu"))
        ls = host.ok({"cmd": "lobby_state"})
        assert ls["lobbyMode"] == 2, f"BUG3: re-hosted lobby lost resume mode: {ls}"
        assert "READY" not in ls.get("buttonText", ""), f"BUG3: READY button in campaign lobby: {ls}"
        print("PASS bug3: re-hosted lobby stays in resume mode, no READY button")

        # --- Bug 2b: disconnect -> host window -> CANCEL must also reach the
        # main menu (the disconnect cleanup used to wipe the mode the cancel
        # routing keyed on) ---
        host.ok({"cmd": "lobby_disconnect"})
        host.wait_for("host window after disconnect", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_menu_cancel"})
        host.wait_for("main menu after disconnect+cancel", lambda: ("MainMenuState" in geo.top_state(host)) or None, timeout=30)
        assert not session._has_state(host, "GeoscapeState"), \
            f"BUG2b: host on a live world after disconnect+cancel: {host.cmd({'cmd': 'get_state'})['states']}"
        print("PASS bug2b: disconnect + cancel also returns to the main menu")

        # bring the lobby back for the remaining scenarios (fresh port - the
        # previous one may still be in TIME_WAIT)
        host.ok({"cmd": "load_save_menu", "file": SAVE})
        host.wait_for("host window again", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47902", "player": "HostPlayer"})
        host.wait_for("lobby again", lambda: session._has_state(host, "LobbyMenu"))

        # --- Bugs 4+5: refused joiner ---
        imposter = GameClient("client", 48663, make_user_dir("polish_imposter"))
        imposter.spawn(); imposter.connect()
        imposter.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47902", "player": "Imposter"})
        # (port 47902 is the re-hosted lobby from the bug-2b recovery above)
        imposter.wait_for(
            "refusal dialog",
            lambda: ("CoopState" in geo.top_state(imposter)) or None,
            timeout=60,
        )

        # Bug 5: watch the host for ~6s - no roster entry, no popup
        deadline = time.time() + 6
        while time.time() < deadline:
            ls = host.ok({"cmd": "lobby_state"})
            players = ls.get("players", [])
            assert not any("Imposter" in p for p in players), f"BUG5: imposter in roster: {players}"
            t = geo.top_state(host)
            assert "LobbyMenu" in t, f"BUG5: popup appeared on host after refused join: {t}"
            time.sleep(0.5)
        print("PASS bug5: refused join invisible to the host (no roster entry, no popup)")

        # Bug 4: dismissing the refusal dialog must clear everything behind it
        imposter.ok({"cmd": "coop_dialog_back"})
        time.sleep(2)
        t = geo.top_state(imposter)
        assert "CoopState" not in t, f"BUG4: dialog left behind after dismissing refusal: {t}"
        print("PASS bug4: refusal dismissal leaves no stray dialog")

        print("ALL LOBBY POLISH TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()
        if imposter:
            imposter.shutdown()


if __name__ == "__main__":
    main()
