"""End-to-end smoke test of the redesigned co-op campaign flow (F2):

New Game -> Co-op -> host window -> lobby (client has no ready button) ->
START CAMPAIGN (host, needs >= 1 client) -> both place bases -> host waits
for every player -> RESUME -> geoscape session.

Also asserts:
  - the save carries the coop marker + locked player list
  - START is refused with an empty lobby
  - a solo campaign cannot be hosted (D1)
  - the client user dir stays free of save data

Run:  python tools/coop_test/test_new_campaign_flow.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session


def main():
    host_dir = make_user_dir("newflow_host")
    client_dir = make_user_dir("newflow_client")
    host = GameClient("host", 48620, host_dir)
    client = GameClient("client", 48621, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        # D1: a solo campaign cannot be hosted
        host.ok({"cmd": "open_new_game", "mode": "solo"})
        host.wait_for("difficulty", lambda: session._has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        r = host.cmd({"cmd": "host_tcp", "server": "X", "port": "48700", "player": "H"})
        assert not r.get("ok") and "solo" in r.get("error", ""), f"solo hosting must be refused, got {r}"
        print("PASS D1: solo campaign hosting refused")

        # restart cleanly for the real flow
        host.shutdown()
        host = GameClient("host", 48622, make_user_dir("newflow_host2"))
        host.spawn()
        host.connect()

        session.new_campaign(host, client)

        # markers: coop flag + locked player list on both worlds
        hm = host.ok({"cmd": "save_markers"})
        cm = client.ok({"cmd": "save_markers"})
        assert hm["coop"] is True, f"host save must be coop-marked: {hm}"
        assert cm["coop"] is True, f"client save must be coop-marked: {cm}"
        assert hm["coopPlayers"] == ["HostPlayer", "ClientPlayer"], f"host player list wrong: {hm}"
        assert cm["coopPlayers"] == ["HostPlayer", "ClientPlayer"], f"client player list wrong: {cm}"
        print("PASS markers: coop + locked player list on both worlds")

        # both on geoscape with their own base
        hs = host.ok({"cmd": "get_soldiers"})
        cs = client.ok({"cmd": "get_soldiers"})
        assert any(b["soldiers"] for b in hs["bases"]), "host has no manned base"
        assert any(b["soldiers"] for b in cs["bases"]), "client has no manned base"
        print("PASS worlds: both players landed on the geoscape with a base")

        session.assert_client_zero_disk(client_dir)
        print("PASS zero-disk: client user dir clean")

        print("ALL NEW-CAMPAIGN FLOW TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
