"""Shared session bring-up for the redesigned co-op campaign flow.

One implementation of the dance, imported by every test (no more copied
bootstrap code):

  new_campaign(host, client)   - Solo/Co-op dropdown -> difficulty -> host
                                 window -> lobby -> START CAMPAIGN -> both
                                 players place bases -> host RESUME ->
                                 session up on the geoscape.
  assert_client_zero_disk(dir) - the standing invariant: a co-op client never
                                 writes save data to disk. Call in teardown.

Ports/base coordinates match the old bootstrap defaults so migrated tests
behave identically.
"""

import os
import time

from harness import LAND_LON, LAND_LAT

HOST_LON, HOST_LAT = 0.35, 0.85

SAVE_EXTS = (".sav", ".asav", ".data")


def states(gc):
    return gc.cmd({"cmd": "get_state"})["states"]


def has_state(gc, name):
    return any(name in s for s in states(gc)) or None


# PRD-13 S7: public names; the underscore forms are kept as aliases for any
# straggler caller and for this module's own internal use below.
_states = states
_has_state = has_state


def new_campaign(host, client, port="47900",
                 host_name="HostPlayer", client_name="ClientPlayer",
                 host_base="HostBase", client_base="ClientBase",
                 campaign_mode="coop"):
    """Bring up a fresh co-op campaign through the redesigned flow.

    campaign_mode selects the New Game dropdown choice: "coop" (SEPARATE,
    the default, unchanged) or "joint" (PRD-J01 JOINT economy).
    """

    # host: New Game -> Co-op -> difficulty OK (world created, HostMenu opens)
    host.ok({"cmd": "open_new_game", "mode": campaign_mode})
    host.wait_for("difficulty", lambda: _has_state(host, "NewGameState"))
    host.ok({"cmd": "newgame_ok"})
    host.wait_for("host window", lambda: _has_state(host, "HostMenu"))

    # host window -> lobby
    host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": port, "player": host_name})
    host.wait_for("host lobby", lambda: _has_state(host, "LobbyMenu"))

    # client joins and lands in the lobby (no ready button, no Profile)
    client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": port, "player": client_name})
    client.wait_for("client lobby", lambda: _has_state(client, "LobbyMenu"))

    # START CAMPAIGN enabled once the client is in
    host.wait_for(
        "start eligible",
        lambda: host.cmd({"cmd": "lobby_state"}).get("startEligible") or None,
    )
    host.ok({"cmd": "lobby_start_campaign"})

    # the host always places its own first base
    host.wait_for("host base placement", lambda: _has_state(host, "BuildNewBaseState"))
    r = host.cmd({"cmd": "place_first_base", "lon": HOST_LON, "lat": HOST_LAT, "name": host_base})
    if not r.get("ok"):
        host.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": host_base})

    if campaign_mode == "joint":
        # PRD-J02: a JOINT client never builds its own world - it waits for the
        # host to stream the authoritative world after the host's base is placed.
        # The host holds in COOP_DLG_RESUME_ACK_WAIT until the client acks the
        # streamed world loaded, then BEGIN releases both.
        host.wait_for(
            "client world ack",
            lambda: host.cmd({"cmd": "get_coop"}).get("resumeAck") or None,
            timeout=120,
        )
        host.ok({"cmd": "coop_dialog_back"})
    else:
        # SEPARATE: the client places its own base and pushes its world blob;
        # the host waits for that blob, then clicks BEGIN.
        client.wait_for("client base placement", lambda: _has_state(client, "BuildNewBaseState"))
        client.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": client_base})

        host.wait_for(
            "all players placed bases",
            lambda: host.cmd({"cmd": "has_coop_file",
                              "key": f"host_{host.cmd({'cmd': 'save_markers'})['saveID']}_{client_name}.data"}).get("present") or None,
            timeout=120,
        )
        host.ok({"cmd": "coop_dialog_back"})

    # session up: both synced (client holds the streamed / synced world)
    try:
        client.wait_for(
            "session up",
            lambda: (lambda c: (c.get("hasSave") and not _has_state(client, "LobbyMenu")) or None)(client.cmd({"cmd": "get_coop"})),
            timeout=120,
        )
    except TimeoutError:
        print("DEBUG host  get_coop:", host.cmd({"cmd": "get_coop"}))
        print("DEBUG host  states:  ", host.cmd({"cmd": "get_state"})["states"])
        print("DEBUG client get_coop:", client.cmd({"cmd": "get_coop"}))
        print("DEBUG client states: ", client.cmd({"cmd": "get_state"})["states"])
        raise
    print("session up (redesigned flow)")


def resume_campaign(host, client, save_file, port="47900",
                    host_name="HostPlayer", client_name="ClientPlayer"):
    """Resume a co-op campaign save through the redesigned flow: menu load ->
    host window -> resume lobby (gated on the registered roster) -> RESUME ->
    world served -> session up. `host` must be freshly at the main menu with
    the save in its user dir; `client` freshly at the main menu."""

    host.ok({"cmd": "load_save_menu", "file": save_file})
    host.wait_for("host window (resume)", lambda: _has_state(host, "HostMenu"))

    host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": port, "player": host_name})
    host.wait_for("resume lobby", lambda: _has_state(host, "LobbyMenu"))
    ls = host.ok({"cmd": "lobby_state"})
    assert ls["lobbyMode"] == 2, f"expected resume lobby, got {ls}"

    client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": port, "player": client_name})
    client.wait_for("client resume lobby", lambda: _has_state(client, "LobbyMenu"))

    host.wait_for(
        "all registered players joined",
        lambda: (lambda r: r.get("ok") is True or None)(host.cmd({"cmd": "lobby_resume_campaign"})),
        timeout=60,
        interval=2.0,
    )

    # host holds in the loading dialog until the client acks, then RESUME
    host.wait_for(
        "client world ack",
        lambda: host.cmd({"cmd": "get_coop"}).get("resumeAck") or None,
        timeout=120,
    )
    host.ok({"cmd": "coop_dialog_back"})

    client.wait_for(
        "resume session up",
        lambda: (lambda c: (c.get("hasSave") and not _has_state(client, "LobbyMenu")) or None)(client.cmd({"cmd": "get_coop"})),
        timeout=120,
    )
    print("session resumed (redesigned flow)")


def save_files(user_dir):
    found = []
    for root, _dirs, files in os.walk(user_dir):
        for f in files:
            if f.lower().endswith(SAVE_EXTS):
                found.append(os.path.relpath(os.path.join(root, f), user_dir))
    return sorted(found)


def assert_client_zero_disk(client_dir):
    files = save_files(client_dir)
    assert files == [], f"CLIENT WROTE SAVE DATA TO DISK: {files}"
