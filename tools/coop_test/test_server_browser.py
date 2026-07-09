"""Menu test: rendezvous-server DisableableComboBox in the Server Browser.

Spawns ONE game instance, bootstraps a SavedGame (the ServerList ctor needs
one), opens the coop Server Browser, waits for the async health probes to
resolve, dumps the combobox state, and saves a PNG for visual inspection.

Scope: this validates the OFFLINE / disabled path only. It points the game at
a throwaway rendezvous config (via the OXC_RENDEZVOUS_CONFIG env override) that
lists two UNREACHABLE servers with placeholder public keys, so nothing real is
committed to the repo and no working server is required. Expected result once
the probes resolve:
    [0] disabled "Test Server A (offline)"
    [1] disabled "Test Server B (offline)"

The happy path (a reachable server showing enabled) needs real server keys, so
it is deliberately NOT covered here; verify it by hand each release.

Run: python tools/coop_test/test_server_browser.py
"""

import base64
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = os.path.join(HERE, "server_browser.png")
SHOT_OPEN = os.path.join(HERE, "server_browser_dropdown.png")
SHOT_FETCH = os.path.join(HERE, "server_browser_fetching.png")

# Placeholder PUBLIC keys (32 zero bytes, base64). Valid shape so the config
# parses; never used because the servers below are unreachable. No secrets.
_DUMMY_KEY = base64.b64encode(b"\x00" * 32).decode()


def write_offline_config(user_dir):
    """Write a rendezvous.json of two UNREACHABLE servers (localhost ports with
    nothing listening) so both resolve as offline/disabled. Returns its path."""
    cfg = {
        "_comment": "Test-only: unreachable servers, placeholder keys. No secrets.",
        "servers": [
            {
                "name": "Test Server A",
                "host": "127.0.0.1", "tcpPort": 5990, "udpPort": 5991,
                "gameVersion": "1.8.4 [v2026-06-28]",
                "serverBoxPublicKey": _DUMMY_KEY,
                "serverSignPublicKey": _DUMMY_KEY,
            },
            {
                "name": "Test Server B",
                "host": "127.0.0.1", "tcpPort": 5992, "udpPort": 5993,
                "gameVersion": "1.8.4 [v2026-06-28]",
                "serverBoxPublicKey": _DUMMY_KEY,
                "serverSignPublicKey": _DUMMY_KEY,
            },
        ],
    }
    path = os.path.join(user_dir, "rendezvous_test.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(cfg, f)
    return path


def main():
    user_dir = make_user_dir("browser-user")
    cfg_path = write_offline_config(user_dir)
    # rendezvous_config search order includes env OXC_RENDEZVOUS_CONFIG first;
    # spawn() inherits os.environ, so set it before spawning.
    os.environ["OXC_RENDEZVOUS_CONFIG"] = cfg_path

    g = GameClient("host", 47901, user_dir)
    try:
        g.spawn()
        g.connect(timeout=240)

        # Bootstrap a SavedGame: new game -> difficulty OK -> place first base.
        g.ok({"cmd": "open_new_game"})
        g.ok({"cmd": "newgame_ok"})
        # Some coop builds show a Profile splash after starting a new game.
        if any("Profile" in s for s in g.cmd({"cmd": "get_state"}).get("states", [])):
            g.ok({"cmd": "profile_ok"})
        g.ok({"cmd": "place_first_base", "lon": 0.7063353365604198, "lat": -0.5070346730015731, "name": "Base 1"})

        # Open the coop Server Browser.
        g.ok({"cmd": "open_server_browser"})

        # Capture the "Fetching server list ..." animation while a probe is
        # still pending.
        time.sleep(0.6)
        g.ok({"cmd": "screenshot", "path": SHOT_FETCH})
        print(f"screenshot -> {SHOT_FETCH}")

        # Wait for the parallel probes (2500ms timeout each) to resolve: no
        # option should still read "(Wait...)".
        def combo_ready():
            r = g.cmd({"cmd": "server_combo"})
            if not r.get("ok"):
                return None
            opts = r.get("options", [])
            if not opts or any("(Wait...)" in o["label"] for o in opts):
                return None
            return r

        r = g.wait_for("combobox probes resolved", combo_ready, timeout=30, interval=1.0)

        print(f"visible={r['visible']} selected={r['selected']}")
        for i, o in enumerate(r["options"]):
            print(f"  [{i}] {'ENABLED ' if o['enabled'] else 'disabled'} {o['label']!r}")

        # Save a screenshot for visual inspection of the widget (closed).
        g.ok({"cmd": "screenshot", "path": SHOT})
        print(f"screenshot -> {SHOT}")

        # Open the dropdown and screenshot it so the greyed/disabled offline rows
        # are visible.
        g.ok({"cmd": "combo_open"})
        time.sleep(0.5)
        g.ok({"cmd": "screenshot", "path": SHOT_OPEN})
        print(f"screenshot -> {SHOT_OPEN}")

        # Assertions: offline/disabled path only.
        labels = [o["label"] for o in r["options"]]
        assert r["visible"], "combobox should be visible with >=2 servers"
        assert len(labels) >= 2, f"expected >=2 servers, got {labels}"
        offline = [o for o in r["options"] if "(offline)" in o["label"]]
        assert offline, f"expected offline servers, got {labels}"
        for o in r["options"]:
            assert not o["enabled"], f"unreachable server must be disabled: {o['label']}"
        print("PASS: combobox visible; all unreachable servers present and disabled")
    finally:
        os.environ.pop("OXC_RENDEZVOUS_CONFIG", None)
        time.sleep(1)
        g.shutdown()


if __name__ == "__main__":
    main()
