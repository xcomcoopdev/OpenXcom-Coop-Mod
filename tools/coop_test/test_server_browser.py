"""Menu test: rendezvous-server DisableableComboBox in the Server Browser.

Spawns ONE game instance, bootstraps a SavedGame (the ServerList ctor needs
one), opens the coop Server Browser, waits for the async health probes to
resolve, dumps the combobox state, and saves a PNG for visual inspection.

Expects bin/x64/Release/rendezvous.json to define >=2 servers so the combobox
is visible. With "Official" (reachable) + "Local Test" (127.0.0.1, nothing
listening) the expected result is:
    [0] enabled  "Official"
    [1] disabled "Local Test (offline)"

Run: python tools/coop_test/test_server_browser.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir

HERE = os.path.dirname(os.path.abspath(__file__))
SHOT = os.path.join(HERE, "server_browser.png")
SHOT_OPEN = os.path.join(HERE, "server_browser_dropdown.png")
SHOT_FETCH = os.path.join(HERE, "server_browser_fetching.png")


def main():
    g = GameClient("host", 47901, make_user_dir("browser-user"))
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

    # Capture the "Fetching server list ..." animation while the selected
    # server's probe is still pending.
    time.sleep(0.6)
    g.ok({"cmd": "screenshot", "path": SHOT_FETCH})
    print(f"screenshot -> {SHOT_FETCH}")

    # Wait for the parallel probes (2500ms timeout each) to resolve: no option
    # should still read "(Wait...)".
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

    # Open the dropdown and screenshot it so the greyed/disabled offline row is
    # visible.
    g.ok({"cmd": "combo_open"})
    time.sleep(0.5)
    g.ok({"cmd": "screenshot", "path": SHOT_OPEN})
    print(f"screenshot -> {SHOT_OPEN}")

    # Basic assertions.
    labels = [o["label"] for o in r["options"]]
    assert r["visible"], "combobox should be visible with >=2 servers"
    assert len(labels) >= 2, f"expected >=2 servers, got {labels}"
    offline = [o for o in r["options"] if "(offline)" in o["label"]]
    assert offline, f"expected at least one (offline) server, got {labels}"
    for o in offline:
        assert not o["enabled"], f"offline server must be disabled: {o['label']}"
    print("PASS: combobox visible, offline server present and disabled")

    time.sleep(1)
    g.shutdown()


if __name__ == "__main__":
    main()
