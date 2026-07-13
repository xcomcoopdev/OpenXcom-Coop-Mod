"""v1.8.4 legacy-save migration test.

Older versions kept the client's world as a sidecar `host_<id>_<name>.data`
file on the host's disk and carried no `coop` marker / roster / embed in the
.sav. The migration path (SavedGame::load) must upgrade such a save in place:
load it, import the sidecar into the served store, synthesize the roster, and
make the NEXT save a fully modern one (embed = single authority).

Fixture strategy: run a real campaign to get a modern save + a real client
blob, then strip the modern coop fields from the .sav and dump the blob as a
legacy sidecar via the `dump_coop_file` test command.

1. Fresh campaign; host saves `legacy_e2e.sav` (modern embed present).
2. Fabricate the legacy shape: sidecar on disk + stripped .sav.
3. Fresh host process loads the stripped save: no refusal, sidecar imported,
   roster synthesized ("" host slot until re-host locks it).
4. Host re-hosts (slot 0 locked to profile name), resume lobby comes up.
5. Host re-saves; sidecar deleted; fresh process reloads: blob still served -
   the new embed is now the authority (marker round trip proven).

Run:  python tools/coop_test/test_legacy_migration.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session

SAVE = "legacy_e2e.sav"


def strip_modern_coop_fields(sav_path):
    """Remove `coop:`, `coopPlayers:` and the `coopClientSaves:` block so the
    save looks like a pre-redesign (v1.8.4) one. `saveID` stays - legacy saves
    carried it, and the migration keys off it."""
    with open(sav_path, "r", encoding="utf-8") as f:
        lines = f.readlines()
    out = []
    in_block = False
    for line in lines:
        stripped = line.rstrip("\n")
        is_top_level = stripped and stripped != "---" and not stripped[0].isspace()
        if in_block:
            if is_top_level or stripped == "---":
                in_block = False  # block over; fall through to normal handling
            else:
                continue  # still inside the dropped block
        if is_top_level and (stripped.startswith("coop:")
                             or stripped.startswith("coopPlayers:")):
            continue
        if is_top_level and stripped.startswith("coopClientSaves:"):
            in_block = True
            continue
        out.append(line)
    with open(sav_path, "w", encoding="utf-8") as f:
        f.writelines(out)


def main():
    host_dir = make_user_dir("legacy_host")
    client_dir = make_user_dir("legacy_client")
    host = GameClient("host", 48660, host_dir)
    client = GameClient("client", 48661, client_dir)
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client)

        save_id = host.cmd({"cmd": "save_markers"})["saveID"]
        blob_key = f"host_{save_id}_ClientPlayer.data"

        # modern save + the client blob dumped as a legacy sidecar
        host.ok({"cmd": "save_game", "file": SAVE})
        host.ok({"cmd": "dump_coop_file", "key": blob_key})

        host.shutdown(); client.shutdown(); client = None

        sav_path = os.path.join(host_dir, "xcom1", SAVE)
        sidecar_path = os.path.join(host_dir, "xcom1", blob_key)
        assert os.path.exists(sav_path), f"missing fixture save {sav_path}"
        assert os.path.exists(sidecar_path), f"missing fixture sidecar {sidecar_path}"
        strip_modern_coop_fields(sav_path)
        with open(sav_path, encoding="utf-8") as f:
            body = f.read()
        assert "coopClientSaves" not in body and "coopPlayers" not in body, \
            "strip failed - modern fields still present"
        print("fixture ready: stripped save + legacy sidecar")

        # fresh process: the stripped save must load and migrate
        host = GameClient("host", 48662, host_dir)
        host.spawn(); host.connect()
        host.ok({"cmd": "load_save_menu", "file": SAVE})
        host.wait_for("host window after legacy load",
                      lambda: session.has_state(host, "HostMenu"))

        markers = host.ok({"cmd": "save_markers"})
        assert markers["coop"] is True, f"migrated save must carry the coop marker: {markers}"
        players = markers.get("coopPlayers") or []
        assert "ClientPlayer" in players, f"roster must contain the imported client: {markers}"
        print("PASS legacy load: migrated, roster synthesized")

        # re-host: the empty legacy host slot is claimed, resume lobby mode.
        # (has_coop_file resolves against the HOST map only once hosting.)
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47910", "player": "HostPlayer"})
        host.wait_for("resume lobby", lambda: session.has_state(host, "LobbyMenu"))
        ls = host.ok({"cmd": "lobby_state"})
        assert ls["lobbyMode"] == 2, f"expected resume lobby for a migrated save: {ls}"
        markers = host.ok({"cmd": "save_markers"})
        assert markers["coopPlayers"][0] == "HostPlayer", \
            f"host slot must be claimed by the re-hosting player: {markers}"
        r = host.ok({"cmd": "has_coop_file", "key": blob_key})
        assert r["present"], "legacy sidecar was not imported into the served store"
        print("PASS re-host: host slot claimed, blob imported, resume lobby gated")

        # the next save is fully modern: sidecar no longer needed
        host.ok({"cmd": "save_game", "file": SAVE})
        host.shutdown()
        os.remove(sidecar_path)

        host = GameClient("host", 48663, host_dir)
        host.spawn(); host.connect()
        host.ok({"cmd": "load_save_menu", "file": SAVE})
        host.wait_for("host window after modern reload",
                      lambda: session.has_state(host, "HostMenu"))
        host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47911", "player": "HostPlayer"})
        host.wait_for("resume lobby (modern reload)",
                      lambda: session.has_state(host, "LobbyMenu"))
        r = host.ok({"cmd": "has_coop_file", "key": blob_key})
        assert r["present"], "re-saved (migrated) save must embed the client blob itself"
        print("PASS round trip: embed is the authority, sidecar not needed")

        print("ALL PASS test_legacy_migration")
    finally:
        for gc in (host, client):
            try:
                if gc:
                    gc.shutdown()
            except Exception:
                pass


if __name__ == "__main__":
    main()
