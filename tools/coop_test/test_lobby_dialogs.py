"""Regression tests for five co-op dialog bugs:

1. Host new game -> client joins lobby -> client disconnects -> the client must
   NOT be left staring at a lingering "Connecting..." wait dialog (CoopState 15)
   that was buried under the lobby and never popped.
   Password variant (issue #46 family): a password-protected join detours
   through PasswordCheckMenu and pushes a SECOND "Connecting..." dialog, so the
   stack under the lobby is [Connecting, PasswordCheckMenu, Connecting]; leaving
   the lobby must not resurface the buried password menu or either dialog.
2. New game, all bases placed: the host dialog must read "All bases placed." with
   a "BEGIN" button (was "All players connected" / "RESUME").
3. The host's "waiting for bases" dialog must use the SAME message and compact
   scaling as the client's hold dialog (was a big, poorly-scaled window).
4. A rejoining client must hold with "Waiting for host to resume the game." (was
   the fresh-placement message "Waiting for all players to place their bases...").
5. The "Waiting for <player> to reconnect" freeze dialog must be compact (~30% of
   the old height), not a huge window.

Dialog introspection uses the coop_dialog_info harness command (code / title /
back-button text+visibility / window height).

Run:  python tools/coop_test/test_lobby_dialogs.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT
import session

HOST_LON, HOST_LAT = 0.35, 0.85

WAIT_BASES_MSG = "Waiting for all players\nto place their bases..."
RESUME_HOLD_MSG = "Waiting for host to resume the game."


def dlg(gc):
    return gc.cmd({"cmd": "coop_dialog_info"})


def dismiss_join_popup(gc):
    """A successful join now ends on the "You have joined <host>'s game" popup,
    stacked over the lobby. Dismiss it the way a player would, so the lobby is
    the top state again for the assertions below."""
    if session.has_state(gc, "Profile"):
        gc.ok({"cmd": "profile_ok"})
        time.sleep(0.5)


def _host_lobby(host, port, password=None):
    host.ok({"cmd": "open_new_game", "mode": "coop"})
    host.wait_for("difficulty", lambda: session.has_state(host, "NewGameState"))
    host.ok({"cmd": "newgame_ok"})
    host.wait_for("host window", lambda: session.has_state(host, "HostMenu"))
    req = {"cmd": "host_tcp", "server": "TestSrv", "port": port, "player": "HostPlayer"}
    if password:
        req["password"] = password
    host.ok(req)
    host.wait_for("host lobby", lambda: session.has_state(host, "LobbyMenu"))


def _client_at_server_browser(client):
    """Client with its own solo world sitting in the server browser, the state
    a real player joins from (a ServerList ends up under the join dialogs)."""
    client.ok({"cmd": "open_new_game", "mode": "solo"})
    client.wait_for("client difficulty", lambda: session.has_state(client, "NewGameState"))
    client.ok({"cmd": "newgame_ok"})
    client.wait_for("client base", lambda: session.has_state(client, "BuildNewBaseState"))
    client.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "SoloBase"})
    client.ok({"cmd": "open_server_browser"})
    client.wait_for("client browser", lambda: session.has_state(client, "ServerList"))


def _campaign_lobby(host, client, port):
    """Host opens a co-op lobby; the client joins it (plain campaign join)."""
    _host_lobby(host, port)
    client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": port, "player": "ClientPlayer"})
    client.wait_for("client lobby", lambda: session.has_state(client, "LobbyMenu"))
    dismiss_join_popup(client)


# ---------------------------------------------------------------- bug 1
def test_connecting_dialog_cleared():
    host = GameClient("host", 48720, make_user_dir("dlg1_host"))
    client = GameClient("client", 48721, make_user_dir("dlg1_client"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        # The real join flow: the client has its own world and browses servers,
        # so a ServerList sits under the "Connecting..." dialog and the lobby.
        _client_at_server_browser(client)

        _host_lobby(host, "47910")

        # mirror ServerList/DirectConnect: push the "Connecting..." dialog, join
        client.ok({"cmd": "coop_push_connecting"})
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47910", "player": "ClientPlayer"})
        client.wait_for("client lobby", lambda: session.has_state(client, "LobbyMenu"))
        dismiss_join_popup(client)

        # client leaves the lobby -> must not resurface a buried "Connecting..."
        client.ok({"cmd": "lobby_disconnect"})
        time.sleep(3)  # let the client's think()/teardown settle

        d = dlg(client)
        assert not (d.get("present") and d.get("code") == 15), \
            f"BUG1: stale Connecting... dialog after lobby disconnect: {d}"
        assert "Connecting" not in (d.get("title") or ""), \
            f"BUG1: Connecting text still showing: {d}"
        print("PASS bug1: no lingering Connecting... after lobby disconnect")
    finally:
        host.shutdown(); client.shutdown()


# ------------------------------------------------- bug 1, password variant
def test_connecting_dialog_cleared_password():
    """Issue #46 family: the same leave-the-lobby scenario, but the host
    requires a password. The join then detours through PasswordCheckMenu and
    the JOIN click pushes a SECOND "Connecting..." dialog, so the stack under
    the lobby is [Connecting, PasswordCheckMenu, Connecting].

    The client deliberately has NO world of its own (the F2 fresh-join flow):
    a campaign client's disconnect happens to rebuild the whole state stack,
    but a fresh client keeps its stack, so the buried dialogs would resurface
    one by one as the player backs out. Leaving the lobby must resurface none
    of them."""
    host = GameClient("host", 48727, make_user_dir("dlg1pw_host"))
    client = GameClient("client", 48728, make_user_dir("dlg1pw_client"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        _host_lobby(host, "47913", password="s3cret")

        # mirror ServerList: push the "Connecting..." dialog, then join; the
        # passwordless hello bounces into the password prompt
        client.ok({"cmd": "coop_push_connecting"})
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47913", "player": "ClientPlayer"})
        client.wait_for("password prompt",
                        lambda: session.has_state(client, "PasswordCheckMenu"))

        # answering the challenge pushes the second Connecting... and reconnects
        client.ok({"cmd": "password_join", "password": "s3cret"})
        client.wait_for("client lobby", lambda: session.has_state(client, "LobbyMenu"))
        dismiss_join_popup(client)

        # client leaves the lobby -> no stale password menu, no Connecting...
        client.ok({"cmd": "lobby_disconnect"})
        time.sleep(3)  # let the client's think()/teardown settle

        d = dlg(client)
        assert not (d.get("present") and d.get("code") == 15), \
            f"BUG1pw: stale Connecting... dialog after lobby disconnect: {d}"
        assert "Connecting" not in (d.get("title") or ""), \
            f"BUG1pw: Connecting text still showing: {d}"
        assert not session.has_state(client, "PasswordCheckMenu"), \
            f"BUG1pw: stale PasswordCheckMenu after lobby disconnect: {session.states(client)}"
        print("PASS bug1pw: no stale password/Connecting dialogs after lobby disconnect")
    finally:
        host.shutdown(); client.shutdown()


# ---------------------------------------------------------------- bugs 2 + 3
def test_wait_bases_dialog():
    host = GameClient("host", 48722, make_user_dir("dlg2_host"))
    client = GameClient("client", 48723, make_user_dir("dlg2_client"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        _campaign_lobby(host, client, "47911")

        host.wait_for("start eligible",
                      lambda: host.cmd({"cmd": "lobby_state"}).get("startEligible") or None)
        host.ok({"cmd": "lobby_start_campaign"})

        # host places first, then sits in WAIT_BASES waiting for the client
        host.wait_for("host base placement", lambda: session.has_state(host, "BuildNewBaseState"))
        r = host.cmd({"cmd": "place_first_base", "lon": HOST_LON, "lat": HOST_LAT, "name": "HostBase"})
        if not r.get("ok"):
            host.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "HostBase"})

        # bug 3: while waiting, same message + compact window as the client hold
        host.wait_for("host wait-bases dialog",
                      lambda: (dlg(host).get("code") == 60) or None,
                      timeout=60)
        hd = dlg(host)
        assert hd["code"] == 60, f"BUG3: expected WAIT_BASES(60): {hd}"
        assert hd["title"] == WAIT_BASES_MSG, f"BUG3: host message mismatch: {hd!r}"
        assert not hd["backVisible"], f"BUG3: button visible while still waiting: {hd}"
        assert hd["windowHeight"] <= 80, f"BUG3: host wait dialog not compact: {hd}"
        print(f"PASS bug3: host wait-bases matches client message, compact (h={hd['windowHeight']})")

        # the client, having placed, holds with the SAME message (proves reuse)
        client.wait_for("client base placement", lambda: session.has_state(client, "BuildNewBaseState"))
        client.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "ClientBase"})
        client.wait_for("client hold", lambda: (dlg(client).get("code") == 65) or None, timeout=60)
        cd = dlg(client)
        assert cd["title"] == WAIT_BASES_MSG, f"BUG3: client hold message drifted: {cd!r}"

        # bug 2: once all bases arrive, host flips to "All bases placed." / BEGIN
        host.wait_for("all bases placed", lambda: (dlg(host).get("backVisible")) or None, timeout=120)
        hd = dlg(host)
        assert hd["title"] == "All bases placed.", f"BUG2: title wrong: {hd!r}"
        assert hd["backText"] == "BEGIN", f"BUG2: button wrong: {hd!r}"
        print("PASS bug2: all-bases-placed reads 'All bases placed.' / BEGIN")
    finally:
        host.shutdown(); client.shutdown()


# ---------------------------------------------------------------- bugs 4 + 5
def test_rejoin_hold_and_freeze():
    host = GameClient("host", 48724, make_user_dir("dlg4_host"))
    client = GameClient("client", 48725, make_user_dir("dlg4_client"))
    client2 = None
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client, port="47912")

        # hard-kill the client mid-session
        client.proc.kill(); client.proc.wait(timeout=10)

        # bug 5: host freezes in a compact "waiting to reconnect" dialog
        host.wait_for("host freeze dialog", lambda: (dlg(host).get("code") == 64) or None, timeout=60)
        fd = dlg(host)
        assert fd["code"] == 64, f"BUG5: expected FREEZE(64): {fd}"
        assert "to reconnect" in fd["title"], f"BUG5: wrong freeze text: {fd!r}"
        assert fd["windowHeight"] <= 60, f"BUG5: freeze dialog too tall (h={fd['windowHeight']}): {fd}"
        print(f"PASS bug5: freeze dialog compact (h={fd['windowHeight']})")

        # registered client rejoins from a fresh process + empty dir
        client2 = GameClient("client", 48726, make_user_dir("dlg4_client2"))
        client2.spawn(); client2.connect()
        client2.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47912", "player": "ClientPlayer"})

        # bug 4: after loading its world, the client holds with the resume message
        client2.wait_for("client resume hold",
                         lambda: (dlg(client2).get("code") == 68) or None, timeout=120)
        cd = dlg(client2)
        assert cd["title"] == RESUME_HOLD_MSG, f"BUG4: wrong rejoin hold message: {cd!r}"
        print("PASS bug4: rejoin hold reads 'Waiting for host to resume the game.'")
    finally:
        host.shutdown(); client.shutdown()
        if client2:
            client2.shutdown()


def main():
    test_connecting_dialog_cleared()
    test_connecting_dialog_cleared_password()
    test_wait_bases_dialog()
    test_rejoin_hold_and_freeze()
    print("ALL LOBBY DIALOG TESTS PASSED")


if __name__ == "__main__":
    main()
