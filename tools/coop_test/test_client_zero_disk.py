"""Client-zero-disk guarantee: in a coop campaign the CLIENT machine must never
write save data to disk. The host .sav (with embedded client blobs) is the
single authority.

Asserts that after a full session -- bring-up, geoscape days passing with
autosave enabled, an explicit host save through the real SaveGameState funnel,
and a client save attempt through the same funnel -- the client's user folder
contains zero *.sav / *.asav / *.data files.

Run:  python tools/coop_test/test_client_zero_disk.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
from test_bug_fixes import bootstrap_fresh_session
import geo
import session

# PRD-13 S7: SAVE_EXTS / save_files / the zero-disk assert live in session.py -
# this test guards the branch's core invariant, so it must use the one true rule
# (if SAVE_EXTS grows, this test must check the new rule too).


def main():
    host_dir = make_user_dir("zerodisk_host")
    client_dir = make_user_dir("zerodisk_client")
    host = GameClient("host", 48610, host_dir)
    client = GameClient("client", 48611, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        bootstrap_fresh_session(host, client)
        geo.wait_both_ready(host, client)

        # Aggressive autosave settings on BOTH sides: every in-game day on the
        # geoscape. Today this leaks _autogeo_.asav onto the client; after the
        # migration the client-side gate must swallow it.
        for gc in (host, client):
            gc.ok({"cmd": "set_option", "name": "autosave", "value": True})
            gc.ok({"cmd": "set_option", "name": "oxceGeoAutosaveFrequency", "value": 1})

        # Let two in-game days pass so the daily autosave trigger fires.
        geo.skip_ingame_time(host, client, minutes=2 * 24 * 60)

        # Host quicksaves through the real save funnel (SaveGameState): this
        # is the authoritative save path - it regenerates the saveID, runs the
        # client-progress pull cycle, and embeds the client world blob.
        host.ok({"cmd": "save_game_ui", "type": "quick"})
        host.wait_for(
            "host quicksave file",
            lambda: os.path.exists(os.path.join(host_dir, "xcom1", "_quick_.asav")) or None,
            timeout=60,
        )

        # Client tries the same funnel; this must be a silent no-op
        # (no file, no crash).
        client.ok({"cmd": "save_game_ui", "type": "quick"})
        client.ok({"cmd": "save_game_ui", "type": "auto_geoscape"})
        time.sleep(5)  # give a would-be write time to land

        # No geoscape-settle gate here: a UFO detected during the time skip
        # re-pushes its popup while paused, and the assertions below only
        # look at the filesystem anyway.
        host_saves = session.save_files(host_dir)
        print(f"host save files:   {host_saves}")
        print(f"client save files: {session.save_files(client_dir)}")

        assert any("_quick_.asav" in f for f in host_saves), \
            f"host must still save normally, got {host_saves}"
        session.assert_client_zero_disk(client_dir)

        print("PASS client-zero-disk: host saves normally, client user dir clean")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
