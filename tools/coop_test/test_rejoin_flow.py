"""Mid-session disconnect + rejoin (F4/D5):

1. Fresh campaign up (redesigned flow).
2. Client process is HARD-KILLED mid-geoscape.
3. Host freezes in a 'Waiting for <name> to reconnect...' dialog.
4. A fresh client process (empty user dir) reconnects with the registered
   name, is served its world directly (no lobby), acks, host clicks RESUME.
5. Session is live again; client roster survived; zero-disk holds.

Run:  python tools/coop_test/test_rejoin_flow.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session


def main():
    host_dir = make_user_dir("rejoin_host")
    host = GameClient("host", 48640, host_dir)
    client = GameClient("client", 48641, make_user_dir("rejoin_client"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client)

        before = client.ok({"cmd": "get_soldiers"})
        own_before = sorted(s["name"] for b in before["bases"] if not b["coopBaseFlag"] for s in b["soldiers"])

        # hard-kill the client mid-session (no clean quit)
        client.proc.kill()
        client.proc.wait(timeout=10)

        # host must freeze in the reconnect dialog
        host.wait_for(
            "host freeze dialog",
            lambda: (lambda st: ("CoopState" in st[-1]) or None)(host.cmd({"cmd": "get_state"})["states"]),
            timeout=60,
        )
        print("PASS freeze: host waiting for the dropped player")

        # registered client rejoins from a fresh process + empty dir
        client = GameClient("client", 48642, make_user_dir("rejoin_client2"))
        client.spawn(); client.connect()
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47900", "player": "ClientPlayer"})

        # host: world served, ack arrives, RESUME
        host.wait_for(
            "rejoined client world ack",
            lambda: host.cmd({"cmd": "get_coop"}).get("resumeAck") or None,
            timeout=120,
        )
        # a rejoin now announces itself on the host with the "<player> has joined
        # the game" popup, stacked over the freeze dialog; dismiss it first so the
        # freeze dialog is the top state again (that gap - a mid-session rejoin
        # telling you nothing about who came back - is exactly what it fixes)
        if session.has_state(host, "Profile"):
            host.ok({"cmd": "profile_ok"})
            time.sleep(0.5)
        host.ok({"cmd": "coop_dialog_back"})

        client.wait_for(
            "client back on geoscape",
            lambda: (lambda c: (c.get("hasSave") and c.get("coopStatic")) or None)(client.cmd({"cmd": "get_coop"})),
            timeout=120,
        )

        after = client.ok({"cmd": "get_soldiers"})
        own_after = sorted(s["name"] for b in after["bases"] if not b["coopBaseFlag"] for s in b["soldiers"])
        assert own_after == own_before, f"client roster changed across rejoin: {own_before} -> {own_after}"
        print("PASS rejoin: world served directly, roster intact")

        session.assert_client_zero_disk(client.user_dir)
        print("PASS zero-disk: rejoined client user dir clean")

        print("ALL REJOIN FLOW TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
