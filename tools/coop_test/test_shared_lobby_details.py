"""Bug: the lobby's "waiting" details line lost the player names on mouse movement.

Resuming a co-op save knows its registered roster, so the lobby says
"Waiting for <names> on port <port>". But the save-list mouse-over/out handlers
unconditionally rewrote the line with the generic "Waiting for players on port <port>",
so simply moving the mouse dropped the names.

Correct behaviour: the NAMED form when resuming a save (the roster is known), the GENERIC
form when starting a new game (there is no roster yet) - and neither may be clobbered by
mouse movement. Both now come from one LobbyMenu::waitingText().

Run:  python tools/coop_test/test_shared_lobby_details.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import session
from harness import GameClient, make_user_dir

SAVE = "shared_lobby_details.sav"
PORT = "47974"


def _details(gc):
    return gc.ok({"cmd": "lobby_state"})["detailsText"]


def main():
    # ---- Build a SHARED save so we have a registered roster to resume. -----------
    js = shared_fixture.bring_up("jlob", (48936, 48937, 48236))
    host_dir = js.host_dir
    try:
        js.host.ok({"cmd": "save_game", "file": SAVE})
    finally:
        js.shutdown()

    # ---- NEW GAME lobby: no roster yet -> GENERIC line. -------------------------
    newhost = GameClient("newhost", 48938, make_user_dir("jlob_new"))
    try:
        newhost.spawn()
        newhost.connect()
        newhost.ok({"cmd": "open_new_game", "mode": "coop_shared"})
        newhost.wait_for("difficulty", lambda: session._has_state(newhost, "NewGameState"))
        newhost.ok({"cmd": "newgame_ok"})
        newhost.wait_for("host window", lambda: session._has_state(newhost, "HostMenu"))
        newhost.ok({"cmd": "host_tcp", "server": "TestSrv", "port": PORT, "player": "HostPlayer"})
        newhost.wait_for("new-game lobby", lambda: session._has_state(newhost, "LobbyMenu"))
        assert newhost.ok({"cmd": "lobby_state"})["lobbyMode"] != 2, "expected a NEW-game lobby"

        txt = _details(newhost)
        assert "Waiting for players on port" in txt, \
            f"new-game lobby should show the generic line, got {txt!r}"
        newhost.ok({"cmd": "lobby_mouse_out"})
        assert _details(newhost) == txt, \
            f"mouse-out changed the new-game line: {txt!r} -> {_details(newhost)!r}"
        print(f"PASS new game: generic line, unchanged by mouse movement ({txt!r})")
    finally:
        newhost.shutdown()

    # ---- RESUME lobby (client NOT joined yet) -> NAMED line, mouse-proof. --------
    rehost = GameClient("rehost", 48940, host_dir)
    try:
        rehost.spawn()
        rehost.connect()
        rehost.ok({"cmd": "load_save_menu", "file": SAVE})
        rehost.wait_for("host window (resume)", lambda: session._has_state(rehost, "HostMenu"))
        rehost.ok({"cmd": "host_tcp", "server": "TestSrv", "port": PORT, "player": "HostPlayer"})
        rehost.wait_for("resume lobby", lambda: session._has_state(rehost, "LobbyMenu"))
        ls = rehost.ok({"cmd": "lobby_state"})
        assert ls["lobbyMode"] == 2, f"expected a RESUME lobby, got {ls}"

        # The client has not joined, so it is a missing registered player: named form.
        txt = rehost.wait_for(
            "named waiting line",
            lambda: (lambda d: d if ("Waiting for" in d and "players on port" not in d) else None)(_details(rehost)),
            timeout=30, interval=0.5)
        assert "on port" in txt, f"resume line lost the port: {txt!r}"
        print(f"PASS resume: names the missing player(s) ({txt!r})")

        # THE BUG: moving the mouse over/off the save list wiped the names.
        rehost.ok({"cmd": "lobby_mouse_out"})
        after = _details(rehost)
        assert after == txt, (
            f"mouse-out rewrote the resume line and lost the player names: "
            f"{txt!r} -> {after!r}")
        print("PASS resume: names survive mouse movement (no generic overwrite)")
        print("ALL SHARED LOBBY-DETAILS TESTS PASSED")
    finally:
        rehost.shutdown()


if __name__ == "__main__":
    main()
