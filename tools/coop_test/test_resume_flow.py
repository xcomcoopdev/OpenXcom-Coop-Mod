"""End-to-end test of the co-op RESUME flow (F3):

1. Bring up a fresh campaign (redesigned flow), host saves, both quit.
2. Host reloads the save from the menu -> host window -> resume lobby.
3. A WRONG-named client is refused at the door (roster gate).
4. The registered client joins, RESUME serves its world, session is up and
   the client's roster survived the round trip.
5. Client user dir stays free of save data throughout.

Run:  python tools/coop_test/test_resume_flow.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session

SAVE = "resume_e2e.sav"


def main():
    host_dir = make_user_dir("resume_host")
    client_dir = make_user_dir("resume_client")
    host = GameClient("host", 48630, host_dir)
    client = GameClient("client", 48631, client_dir)
    imposter = None
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client)

        # remember the client's soldiers to verify the resume round trip
        before = client.ok({"cmd": "get_soldiers"})
        own_before = sorted(s["name"] for b in before["bases"] if not b["coopBaseFlag"] for s in b["soldiers"])
        assert own_before, "client should own soldiers before the save"

        # host saves (real funnel: pulls fresh client progress + embeds it)
        host.ok({"cmd": "save_game_ui", "type": "quick"})
        host.wait_for(
            "host quicksave on disk",
            lambda: os.path.exists(os.path.join(host_dir, "xcom1", "_quick_.asav")) or None,
            timeout=60,
        )
        host.ok({"cmd": "save_game", "file": SAVE})

        # both sessions down
        host.shutdown(); client.shutdown()

        # host reloads the save through the menu (fresh process, same dir)
        host = GameClient("host", 48632, host_dir)
        host.spawn(); host.connect()

        host.ok({"cmd": "load_save_menu", "file": SAVE})
        host.wait_for("host window (resume)", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47900", "player": "HostPlayer"})
        host.wait_for("resume lobby", lambda: session._has_state(host, "LobbyMenu"))
        ls = host.ok({"cmd": "lobby_state"})
        assert ls["lobbyMode"] == 2, f"expected resume lobby, got {ls}"

        # wrong name refused at the door
        imposter = GameClient("client", 48633, make_user_dir("resume_imposter"))
        imposter.spawn(); imposter.connect()
        imposter.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47900", "player": "Imposter"})
        imposter.wait_for(
            "imposter refused",
            lambda: any("CoopState" in s for s in imposter.cmd({"cmd": "get_state"})["states"]) or None,
            timeout=60,
        )
        r = imposter.cmd({"cmd": "get_coop"})
        assert not r.get("coopStatic"), f"imposter must not enter the session: {r}"
        print("PASS roster gate: wrong name refused")
        imposter.shutdown(); imposter = None

        # registered client rejoins (fresh process, EMPTY user dir)
        client = GameClient("client", 48634, make_user_dir("resume_client2"))
        client.spawn(); client.connect()
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47900", "player": "ClientPlayer"})
        client.wait_for("client resume lobby", lambda: session._has_state(client, "LobbyMenu"))

        host.wait_for(
            "resume accepted",
            lambda: (lambda rr: rr.get("ok") is True or None)(host.cmd({"cmd": "lobby_resume_campaign"})),
            timeout=60, interval=2.0,
        )
        host.wait_for(
            "client world ack",
            lambda: host.cmd({"cmd": "get_coop"}).get("resumeAck") or None,
            timeout=120,
        )
        host.ok({"cmd": "coop_dialog_back"})

        client.wait_for(
            "resume session up",
            lambda: (lambda c: (c.get("hasSave") and not session._has_state(client, "LobbyMenu")) or None)(client.cmd({"cmd": "get_coop"})),
            timeout=120,
        )

        after = client.ok({"cmd": "get_soldiers"})
        own_after = sorted(s["name"] for b in after["bases"] if not b["coopBaseFlag"] for s in b["soldiers"])
        assert own_after == own_before, f"client roster changed across resume: {own_before} -> {own_after}"
        print("PASS resume: client world restored from the host save (roster intact)")

        cm = client.ok({"cmd": "save_markers"})
        assert cm["coop"] is True and cm["coopPlayers"] == ["HostPlayer", "ClientPlayer"], f"markers lost: {cm}"
        print("PASS markers survived the round trip")

        session.assert_client_zero_disk(client.user_dir)
        print("PASS zero-disk: resumed client user dir clean")

        print("ALL RESUME FLOW TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()
        if imposter:
            imposter.shutdown()


if __name__ == "__main__":
    main()
