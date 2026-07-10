"""Autonomous test: brand-new hosted campaign, connect a client, transfer the
first host soldier, verify the client sees them at the host's base.

Run:  python tools/coop_test/test_transfer_fresh.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT

HOST_LON, HOST_LAT = 0.35, 0.85  # different land spot for the host base


def main():
    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        for gc in (host, client):
            gc.ok({"cmd": "set_option", "name": "HostSaveProgress", "value": True})

        # --- host: brand-new campaign ---
        host.ok({"cmd": "open_new_game"})
        host.wait_for("difficulty", lambda: any("NewGameState" in s for s in host.cmd({"cmd": "get_state"})["states"]) or None)
        host.ok({"cmd": "newgame_ok"})
        host.wait_for("base placement", lambda: any("BuildNewBaseState" in s for s in host.cmd({"cmd": "get_state"})["states"]) or None)
        r = host.cmd({"cmd": "place_first_base", "lon": HOST_LON, "lat": HOST_LAT, "name": "HostBase"})
        if not r.get("ok"):
            # fall back to the known-good land spot
            host.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "HostBase"})
        host.wait_for("host on geoscape", lambda: (lambda st: "GeoscapeState" in st[0] and len(st) == 1 or None)([s for s in host.cmd({"cmd": "get_state"})["states"]]))

        # --- host up, client join, same lobby dance as legacy test ---
        r = host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47900", "player": "HostPlayer"})
        assert r["campaign"], "fresh campaign should count as campaign"

        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47900", "player": "ClientPlayer"})
        host.wait_for("client joined", lambda: host.cmd({"cmd": "get_coop"}).get("coopStatic") or None)

        host.wait_for("host profile", lambda: any("Profile" in s for s in host.cmd({"cmd": "get_state"})["states"]) or None)
        host.ok({"cmd": "profile_ok"})
        client.wait_for("client profile", lambda: any("Profile" in s for s in client.cmd({"cmd": "get_state"})["states"]) or None)
        client.ok({"cmd": "profile_ok"})

        client.wait_for("difficulty", lambda: any("NewGameState" in s for s in client.cmd({"cmd": "get_state"})["states"]) or None)
        client.ok({"cmd": "newgame_ok"})
        client.wait_for("base placement", lambda: any("BuildNewBaseState" in s for s in client.cmd({"cmd": "get_state"})["states"]) or None)
        client.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "ClientBase"})

        client.wait_for("client lobby", lambda: any("LobbyMenu" in s for s in client.cmd({"cmd": "get_state"})["states"]) or None)
        client.ok({"cmd": "lobby_ready"})
        host.ok({"cmd": "lobby_ready"})
        client.wait_for("locked", lambda: client.cmd({"cmd": "get_coop"}).get("sessionLocked") or None, timeout=60)
        host.ok({"cmd": "lobby_ready"})
        client.ok({"cmd": "lobby_ready"})
        client.wait_for(
            "lobby closed + save synced",
            lambda: (lambda c: (c.get("lobbyClosed") and c.get("hasSave")) or None)(client.cmd({"cmd": "get_coop"})),
            timeout=120,
        )
        print("fresh coop session established")

        # --- pick host's first soldier, transfer, verify ---
        hs = host.ok({"cmd": "get_soldiers"})
        hbase = next(b for b in hs["bases"] if not b["coopBaseFlag"] and b["soldiers"])
        soldier = hbase["soldiers"][0]
        host_base_id = hbase["coopBaseId"]
        print(f"transferring {soldier['name']} from '{hbase['name']}' (coopBaseId={host_base_id})")

        host.ok({"cmd": "transfer", "name": soldier["name"], "owner": 1})

        def client_sees():
            r = client.cmd({"cmd": "get_mirror_soldiers", "coopBaseId": host_base_id})
            if r.get("ok"):
                for s in r["soldiers"]:
                    if s["name"] == soldier["name"] and s["owner"] == 1:
                        return s
            return None

        s = client.wait_for("soldier at host base on client", client_sees, timeout=60)
        print(f"PASS: client sees {s['name']} owner={s['owner']} coopBase={s['coopBase']}")

        hs2 = host.ok({"cmd": "get_soldiers"})
        for b in hs2["bases"]:
            assert all(x["name"] != soldier["name"] for x in b["soldiers"]), "host still has the soldier"
        print("PASS: gone from host rosters")
        print("TEST PASSED")
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
