"""Regression tests for six session-hardening bugs:

1. Repeated host/disconnect (UDP-public path) could leave a HostMenu with
   every control hidden (blank window with just CANCEL).
2. The joining client could not see the host in the lobby roster.
3. After a disconnect the client's lobby think() stacked a SECOND server
   browser on top of the old one (CANCEL then acted on the stale copy).
4. A different player name must never host a campaign save (host identity
   is locked to coopPlayers[0]).
5. The resume lobby's waiting text must merge names + port:
   "Waiting for <names> on port <X>".
6. A joiner may not take a name already in use (the host's own name passes
   the roster check - both players ended up as the same identity).

Run:  python tools/coop_test/test_session_hardening.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT
import session
import geo  # PRD-13 S7: geo.top_state (safe on empty stack) + session.states

SAVE = "hardening_e2e.sav"


def main():
    host_dir = make_user_dir("hard_host")
    host = GameClient("host", 48680, host_dir)
    client = GameClient("client", 48681, make_user_dir("hard_client"))
    joiner = None
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        # ---------- Bug 2: client sees the host in the lobby ----------
        host.ok({"cmd": "open_new_game", "mode": "coop"})
        host.wait_for("difficulty", lambda: session._has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("host window", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47900", "player": "HostPlayer"})
        host.wait_for("host lobby", lambda: session._has_state(host, "LobbyMenu"))
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47900", "player": "ClientPlayer"})
        client.wait_for("client lobby", lambda: session._has_state(client, "LobbyMenu"))

        client.wait_for(
            "host visible in client roster",
            lambda: any("HostPlayer" in p for p in client.ok({"cmd": "lobby_state"}).get("players", [])) or None,
            timeout=30,
        )
        print("PASS bug2: client sees the host in the lobby roster")

        # ---------- Bug 3: no stacked server browser after disconnect ----------
        # Fresh client WITH its own solo world (ServerList needs one), browser
        # under the lobby like the real join flow.
        host.shutdown(); client.shutdown()
        host = GameClient("host", 48686, make_user_dir("hard_host3"))
        client = GameClient("client", 48687, make_user_dir("hard_client3"))
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        client.ok({"cmd": "open_new_game", "mode": "solo"})
        client.wait_for("client difficulty", lambda: session._has_state(client, "NewGameState"))
        client.ok({"cmd": "newgame_ok"})
        client.wait_for("client base placement", lambda: session._has_state(client, "BuildNewBaseState"))
        client.ok({"cmd": "place_first_base", "lon": 0.7063353365604198, "lat": -0.5070346730015731, "name": "SoloBase"})
        client.ok({"cmd": "open_server_browser"})

        host.ok({"cmd": "open_new_game", "mode": "coop"})
        host.wait_for("difficulty", lambda: session._has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("host window", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47901", "player": "HostPlayer"})
        host.wait_for("host lobby", lambda: session._has_state(host, "LobbyMenu"))

        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47901", "player": "ClientPlayer"})
        client.wait_for("client lobby over browser", lambda: session._has_state(client, "LobbyMenu"))

        # host tears the session down while the client sits in the lobby
        host.ok({"cmd": "lobby_disconnect"})
        time.sleep(4)  # give the client's lobby think() time to misbehave
        st = session.states(client)
        assert sum(1 for s in st if "ServerList" in s) <= 1, f"BUG3: stacked server browsers: {st}"
        if "CoopState" in geo.top_state(client):
            client.ok({"cmd": "coop_dialog_back"})  # dismiss "connection lost"
            time.sleep(1)
        st = session.states(client)
        assert "CoopState" not in geo.top_state(client), f"BUG3: stray dialog persists: {st}"
        assert sum(1 for s in st if "ServerList" in s) <= 1, f"BUG3: stacked server browsers: {st}"
        print("PASS bug3a: host-initiated drop leaves a clean stack")

        # client-initiated disconnect: leave via the lobby button, then make
        # sure exactly one browser (no stacking, no reconnect dialog)
        host.wait_for("host window after drop", lambda: session._has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47902", "player": "HostPlayer"})
        host.wait_for("host lobby again", lambda: session._has_state(host, "LobbyMenu"))
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47902", "player": "ClientPlayer"})
        client.wait_for("client lobby again", lambda: session._has_state(client, "LobbyMenu"))
        client.ok({"cmd": "lobby_disconnect"})
        time.sleep(4)
        st = session.states(client)
        assert sum(1 for s in st if "ServerList" in s) <= 1, f"BUG3: stacked browsers after client disconnect: {st}"
        if "CoopState" in geo.top_state(client):
            client.ok({"cmd": "coop_dialog_back"})
            time.sleep(1)
        st = session.states(client)
        assert "CoopState" not in geo.top_state(client), f"BUG3: stray dialog after client disconnect: {st}"
        assert sum(1 for s in st if "ServerList" in s) <= 1, f"BUG3: stacked browsers after dismiss: {st}"
        print("PASS bug3b: client-initiated disconnect leaves a clean stack")

        # reset both instances for the campaign scenarios
        host.shutdown(); client.shutdown()
        host = GameClient("host", 48682, host_dir)
        client = GameClient("client", 48683, make_user_dir("hard_client2"))
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client)
        host.ok({"cmd": "save_game_ui", "type": "quick"})
        host.wait_for(
            "host quicksave",
            lambda: os.path.exists(os.path.join(host_dir, "xcom1", "_quick_.asav")) or None,
            timeout=60,
        )
        host.ok({"cmd": "save_game", "file": SAVE})
        host.shutdown(); client.shutdown()

        # ---------- Bug 4: wrong host name cannot host the save ----------
        host = GameClient("host", 48684, host_dir)
        host.spawn(); host.connect()
        host.ok({"cmd": "load_save_menu", "file": SAVE})
        host.wait_for("host window (resume)", lambda: session._has_state(host, "HostMenu"))
        r = host.cmd({"cmd": "host_tcp", "server": "TestSrv", "port": "47903", "player": "WrongHost"})
        assert not r.get("ok"), f"BUG4: a different name hosted the campaign save: {r}"
        print("PASS bug4: campaign save refuses a different host name")

        # correct name hosts fine
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47903", "player": "HostPlayer"})
        host.wait_for("resume lobby", lambda: session._has_state(host, "LobbyMenu"))

        # ---------- Bug 5: waiting text merges names + port ----------
        # detailsText is refreshed on the lobby's ~1s think cadence; the first
        # frame still shows the ctor's generic "Waiting for players on port X".
        # Poll for the merged form rather than reading that transient default.
        details = host.wait_for(
            "resume waiting text merged (names + port)",
            lambda: (lambda d: d if ("ClientPlayer" in d and "47903" in d) else None)(
                host.cmd({"cmd": "lobby_state"}).get("detailsText", "")),
            timeout=15,
        )
        print(f"PASS bug5: waiting text merged: {details!r}")

        # ---------- Bug 6: the host's own name is refused ----------
        joiner = GameClient("client", 48685, make_user_dir("hard_joiner"))
        joiner.spawn(); joiner.connect()
        joiner.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47903", "player": "HostPlayer"})
        joiner.wait_for(
            "duplicate name refused",
            lambda: ("CoopState" in geo.top_state(joiner)) or None,
            timeout=60,
        )
        r = joiner.cmd({"cmd": "get_coop"})
        assert not r.get("coopStatic"), f"BUG6: duplicate name entered the session: {r}"
        ls = host.ok({"cmd": "lobby_state"})
        assert ls.get("lobbyMode") == 2 and not ls.get("startEligible"), \
            f"BUG6: host lobby thinks the duplicate joined: {ls}"
        print("PASS bug6: duplicate player name refused")
        joiner.shutdown(); joiner = None

        # ---------- Bug 1: UDP-public host attempt never strands a blank window ----------
        # (offline: the rendezvous registration cannot succeed; whatever happens,
        # any HostMenu still on screen must keep its controls usable)
        host.ok({"cmd": "lobby_disconnect"})
        host.wait_for("host window again", lambda: session._has_state(host, "HostMenu"))
        for cycle in range(3):
            host.ok({"cmd": "host_menu_host", "visibility": 2})  # UDP public
            time.sleep(3)
            hs = host.ok({"cmd": "host_menu_state"})
            if hs["open"]:
                assert hs["controlsVisible"], \
                    f"BUG1: blank host window (cycle {cycle}): {session.states(host)}"
            # tear down whatever came up (dialogs, lobby, in any order) until
            # a host window surfaces again
            for _ in range(8):
                t = geo.top_state(host)
                if "HostMenu" in t:
                    break
                if "CoopState" in t:
                    host.cmd({"cmd": "coop_dialog_back"})
                elif "LobbyMenu" in t:
                    host.cmd({"cmd": "lobby_disconnect"})
                time.sleep(1)
            host.wait_for(
                "host window for next cycle",
                lambda: ("HostMenu" in geo.top_state(host)) or None,
                timeout=30,
            )
        hs = host.ok({"cmd": "host_menu_state"})
        assert hs["open"] and hs["controlsVisible"], f"BUG1: final host window unusable: {session.states(host)}"
        print("PASS bug1: UDP host/disconnect cycles never strand a blank host window")

        # ---------- C1 (PRD-04): a solo save written after a coop session loads ----------
        # A coop session leaves connectionTCP::saveID nonzero in-process. Before
        # PRD-04, a later solo save carried that nonzero saveID with no coop
        # marker, so loading it threw "made on an earlier version". Verify the
        # solo save loads and no foreign host_* blob survives.
        host.shutdown(); client.shutdown()
        host = GameClient("host", 48690, make_user_dir("c1_host"))
        client = GameClient("client", 48691, make_user_dir("c1_client"))
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client, port="47910")
        id_a = host.ok({"cmd": "save_markers"})["saveID"]
        assert id_a != 0, "C1 setup: campaign saveID should be nonzero"
        key_a = f"host_{id_a}_ClientPlayer.data"
        assert host.ok({"cmd": "has_coop_file", "key": key_a}).get("present"), \
            "C1 setup: client world blob should be present after the campaign"

        # disconnect both to the main menu (the ex-host stays the same process)
        host.ok({"cmd": "disconnect_to_menu"})
        client.ok({"cmd": "disconnect_to_menu"})
        time.sleep(3)

        # ex-host builds a SOLO game in the SAME process
        host.ok({"cmd": "open_new_game", "mode": "solo"})
        host.wait_for("solo difficulty", lambda: session._has_state(host, "NewGameState"))
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("solo base placement", lambda: session._has_state(host, "BuildNewBaseState"))
        r = host.cmd({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "SoloBase"})
        assert r.get("ok"), f"C1 setup: solo base placement failed: {r}"
        host.ok({"cmd": "save_game", "file": "solo_after_coop.sav"})

        r = host.cmd({"cmd": "load_save", "file": "solo_after_coop.sav"})
        assert r.get("ok"), f"C1: solo save written after a coop session is unloadable: {r}"
        assert not host.ok({"cmd": "has_coop_file", "key": key_a}).get("present"), \
            "C1: a stale host_* blob survived the solo load"
        print("PASS C1: solo save after coop loads cleanly, no stale blob")

        # ---------- C2 (PRD-04): a second campaign mints a fresh saveID ----------
        # Before PRD-04 nothing regenerated saveID or cleared the blob maps, so a
        # second campaign in the same process reused the first's ID and served its
        # stale client world.
        host.shutdown(); client.shutdown()
        host = GameClient("host", 48692, make_user_dir("c2_host"))
        client = GameClient("client", 48693, make_user_dir("c2_client"))
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client, port="47911")
        id_1 = host.ok({"cmd": "save_markers"})["saveID"]
        assert id_1 != 0, "C2 setup: first campaign saveID should be nonzero"
        key_1 = f"host_{id_1}_ClientPlayer.data"
        assert host.ok({"cmd": "has_coop_file", "key": key_1}).get("present"), \
            "C2 setup: first campaign blob should be present"

        # tear the session down to the main menu in-process (resetSession fires):
        # a pristine identity means the first campaign's blob is gone.
        host.ok({"cmd": "disconnect_to_menu"})
        client.ok({"cmd": "disconnect_to_menu"})
        time.sleep(3)
        assert not host.ok({"cmd": "has_coop_file", "key": key_1}).get("present"), \
            "C2: resetSession did not clear the first campaign's blob maps"

        # second campaign on the SAME instances
        session.new_campaign(host, client, port="47912")
        id_2 = host.ok({"cmd": "save_markers"})["saveID"]
        assert id_2 != 0 and id_2 != id_1, \
            f"C2: second campaign reused the first saveID (id1={id_1}, id2={id_2})"
        assert not host.ok({"cmd": "has_coop_file", "key": key_1}).get("present"), \
            "C2: the first campaign's stale blob is still being served"
        print(f"PASS C2: second campaign minted a fresh saveID ({id_1} -> {id_2})")

        # ---------- C7 (PRD-08): host mid-session local load is refused ----------
        # A live coop session forbids local loads for everyone, host included -
        # loading mid-session forks the served world silently (the client keeps
        # its live/now-future world and its next progress push clobbers the
        # rolled-back one). After the session ends, loads are allowed again.
        host.shutdown(); client.shutdown()
        c7_host_dir = make_user_dir("c7_host")
        host = GameClient("host", 48694, c7_host_dir)
        client = GameClient("client", 48695, make_user_dir("c7_client"))
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client, port="47913")
        # host writes a real save it will then try to reload mid-session
        host.ok({"cmd": "save_game", "file": SAVE})
        saveid_live = host.ok({"cmd": "get_coop"}).get("saveID")

        host.cmd({"cmd": "load_save_menu", "file": SAVE})
        time.sleep(3)
        gc = host.ok({"cmd": "get_coop"})
        assert gc.get("coopSession"), \
            f"C7: host mid-session load tore down the live session: {gc}"
        assert gc.get("saveID") == saveid_live, \
            f"C7: host mid-session load switched worlds (saveID {saveid_live} -> {gc.get('saveID')})"
        assert "LoadGameState" not in geo.top_state(host), \
            f"C7: LoadGameState lingering after refusal: {geo.top_state(host)}"
        print("PASS C7: host mid-session local load refused")

        # end the session -> solo -> a local load is allowed again and routes to
        # the host's resume window (coop save -> HostMenu / resume lobby)
        host.ok({"cmd": "disconnect_to_menu"})
        client.ok({"cmd": "disconnect_to_menu"})
        time.sleep(3)
        host.ok({"cmd": "load_save_menu", "file": SAVE})
        host.wait_for(
            "post-session load routed",
            lambda: (session._has_state(host, "HostMenu")
                     or session._has_state(host, "GeoscapeState")
                     or session._has_state(host, "LobbyMenu")) or None,
            timeout=30,
        )
        print("PASS C7b: local load allowed after the session ends")

        print("ALL SESSION HARDENING TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()
        if joiner:
            joiner.shutdown()


if __name__ == "__main__":
    main()
