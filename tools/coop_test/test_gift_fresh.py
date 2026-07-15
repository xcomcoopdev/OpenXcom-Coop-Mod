"""Autonomous test: brand-new hosted campaign, connect a client, gift the
first host soldier, verify the client sees them at the host's base.

Run:  python tools/coop_test/test_gift_fresh.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT
import session


def main():
    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        session.new_campaign(host, client)
        print("fresh coop session established")

        # --- pick host's first soldier, gift, verify ---
        hs = host.ok({"cmd": "get_soldiers"})
        hbase = next(b for b in hs["bases"] if not b["coopBaseFlag"] and b["soldiers"])
        soldier = hbase["soldiers"][0]
        host_base_id = hbase["coopBaseId"]
        print(f"gifting {soldier['name']} from '{hbase['name']}' (coopBaseId={host_base_id})")

        host.ok({"cmd": "gift", "name": soldier["name"], "owner": 1})

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
