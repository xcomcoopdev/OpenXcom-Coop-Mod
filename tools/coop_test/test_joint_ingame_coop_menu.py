"""Playtest B7: the in-game coop menu must offer RESUME GAME, not only Disconnect.

Opening the coop menu (pause -> co-op) while already connected in a running
campaign lands on the LobbyMenu. Its action button (START/RESUME CAMPAIGN) was
gated to the pre-game lobby and to the host only, so mid-game every player - and
every client always - saw just Disconnect: a dead end with no way back to the
game.

The fix detects that the menu was opened over a live campaign geoscape and shows
a RESUME GAME button for the host and the client alike; clicking it pops the
coop-menu states back down to the running geoscape (the shared world stays live,
no re-handshake, no disconnect).

  MENU    open the coop menu mid-game -> the LobbyMenu shows a visible RESUME GAME
          action button (host AND client), not just Disconnect.
  RESUME  clicking it returns to the running geoscape, still connected.

Run:  python tools/coop_test/test_joint_ingame_coop_menu.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
from geo import top_state, on_geoscape


def _connected(gc):
    return bool(gc.cmd({"cmd": "get_coop"}).get("coopStatic"))


def _check_resume(gc, who):
    # baseline: on the running geoscape, connected.
    assert on_geoscape(gc), f"{who}: not on geoscape at start ({top_state(gc)})"
    assert _connected(gc), f"{who}: not connected at start"

    # open the in-game coop menu.
    assert gc.ok({"cmd": "open_coop_menu"}).get("ok"), f"{who}: open_coop_menu failed"
    gc.wait_for(f"{who} coop menu (LobbyMenu) on top",
                lambda: ("LobbyMenu" in top_state(gc)) or None, timeout=15, interval=0.3)

    # MENU: the action button must be a visible RESUME GAME (not only Disconnect).
    def _resume_btn():
        ls = gc.cmd({"cmd": "lobby_state"})
        if not ls.get("lobbyOpen"):
            return None
        return ls if (ls.get("buttonVisible") and ls.get("buttonText") == "RESUME GAME") else None

    gc.wait_for(f"{who} lobby shows a visible RESUME GAME button",
                _resume_btn, timeout=15, interval=0.3)
    ls = gc.cmd({"cmd": "lobby_state"})
    assert ls["buttonText"] == "RESUME GAME" and ls["buttonVisible"], \
        f"{who}: coop menu is a dead end (button={ls.get('buttonText')!r} " \
        f"visible={ls.get('buttonVisible')}) (B7)"
    print(f"PASS {who} menu: coop menu shows a visible RESUME GAME button")

    # RESUME: clicking it returns to the running geoscape, still connected.
    assert gc.ok({"cmd": "lobby_action"}).get("ok"), f"{who}: lobby_action failed"
    gc.wait_for(f"{who} back on the running geoscape",
                lambda: on_geoscape(gc) or None, timeout=15, interval=0.3)
    assert on_geoscape(gc), f"{who}: did not return to geoscape ({top_state(gc)})"
    assert _connected(gc), f"{who}: resume dropped the connection"
    assert gc.cmd({"cmd": "ping"}).get("pong"), f"{who}: unresponsive after resume"
    print(f"PASS {who} resume: returned to the running game, still connected")


def main():
    js = joint_fixture.bring_up("jcoopmenu", (48820, 48821, 48120))
    host, client = js.host, js.client
    try:
        # The reported case is a CLIENT (always only-Disconnect before the fix)...
        _check_resume(client, "client")
        # ...but the host must get RESUME GAME too.
        _check_resume(host, "host")
        print("ALL JOINT IN-GAME COOP-MENU TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
