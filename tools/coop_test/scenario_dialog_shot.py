"""Drive the game to the 'waiting for players to place bases' dialog and
screenshot it (visual iteration helper for the compact dialog, not a test).

Usage: python tools/coop_test/scenario_dialog_shot.py <out.png>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT
import session


def main(out_png):
    host = GameClient("host", 48670, make_user_dir("shot_host"))
    client = GameClient("client", 48671, make_user_dir("shot_client"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        host.ok({"cmd": "open_new_game", "mode": "coop"})
        host.wait_for("difficulty", lambda: session._has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("host window", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47900", "player": "HostPlayer"})
        host.wait_for("host lobby", lambda: session._has_state(host, "LobbyMenu"))
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47900", "player": "ClientPlayer"})
        client.wait_for("client lobby", lambda: session._has_state(client, "LobbyMenu"))
        host.wait_for("eligible", lambda: host.cmd({"cmd": "lobby_state"}).get("startEligible") or None)
        host.ok({"cmd": "lobby_start_campaign"})

        client.wait_for("client base placement", lambda: session._has_state(client, "BuildNewBaseState"))
        client.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "ClientBase"})
        client.wait_for(
            "client waiting dialog",
            lambda: ("CoopState" in client.cmd({"cmd": "get_state"})["states"][-1]) or None,
        )
        time.sleep(1)  # let the popup animation finish
        client.ok({"cmd": "screenshot", "path": out_png})
        print("saved:", out_png)
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main(sys.argv[1])
