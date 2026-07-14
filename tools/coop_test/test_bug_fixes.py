"""Autonomous regression tests for the three gift bugs:

1. Gift while the receiving player has a soldier list / the peer-base
   view open must not lose the soldier.
2. The gift dialog must adopt its parent state's palette (no flicker).
3. Owner resolution: a fresh soldier's gift target list must show the
   OTHER player (bug: client saw their own name first, click was a no-op).

Run:  python tools/coop_test/test_bug_fixes.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, LAND_LON, LAND_LAT
import session

HOST_LON, HOST_LAT = 0.35, 0.85


def bootstrap_fresh_session(host, client):
    """Fresh co-op campaign via the redesigned flow (shared session dance)."""
    session.new_campaign(host, client)


def own_base(gc):
    s = gc.ok({"cmd": "get_soldiers"})
    return next(b for b in s["bases"] if not b["coopBaseFlag"] and not b["coopIcon"] and b["soldiers"])


def test_bug3_owner_resolution(host, client):
    hbase = own_base(host)
    cbase = own_base(client)

    r = client.ok({"cmd": "gift_targets", "name": cbase["soldiers"][0]["name"]})
    assert r["currentOwner"] == 1, f"client's fresh soldier should resolve to owner 1, got {r['currentOwner']}"
    assert [t["id"] for t in r["targets"]] == [0], f"client targets wrong: {r['targets']}"
    assert r["targets"][0]["name"] == "HostPlayer", f"client should see host's name, got {r['targets'][0]['name']}"

    r = host.ok({"cmd": "gift_targets", "name": hbase["soldiers"][0]["name"]})
    assert r["currentOwner"] == 0, f"host's fresh soldier should resolve to owner 0, got {r['currentOwner']}"
    assert [t["id"] for t in r["targets"]] == [1], f"host targets wrong: {r['targets']}"
    assert r["targets"][0]["name"] == "ClientPlayer", f"host should see client's name, got {r['targets'][0]['name']}"
    print("PASS bug3: owner resolution + target names correct on both machines")


def test_bug2_palette(host):
    hbase = own_base(host)
    host.ok({"cmd": "open_soldiers", "base": hbase["name"]})
    host.wait_for("soldiers screen", lambda: any("SoldiersState" in s for s in host.cmd({"cmd": "get_state"})["states"]) or None)
    host.ok({"cmd": "open_gift_dialog", "name": hbase["soldiers"][0]["name"]})
    host.wait_for("dialog", lambda: any("GiftSoldierMenu" in s for s in host.cmd({"cmd": "get_state"})["states"]) or None)

    pals = host.ok({"cmd": "get_palettes"})["states"]
    parent = next(e for e in pals if "SoldiersState" in e["state"])
    dialog = next(e for e in pals if "GiftSoldierMenu" in e["state"])
    assert dialog["colors"] == parent["colors"], "dialog palette differs from parent (flicker)"

    host.ok({"cmd": "cancel_dialog"})
    host.ok({"cmd": "soldiers_ok"})
    print("PASS bug2: dialog adopts parent palette (no palette swap = no flicker)")


def test_bug1a_list_open(host, client):
    hbase = own_base(host)
    soldier = hbase["soldiers"][0]["name"]
    host_base_id = hbase["coopBaseId"]

    cbase = own_base(client)
    client.ok({"cmd": "open_soldiers", "base": cbase["name"]})
    client.wait_for("client list open", lambda: any("SoldiersState" in s for s in client.cmd({"cmd": "get_state"})["states"]) or None)

    host.ok({"cmd": "gift", "name": soldier, "owner": 1})

    # notification must pop on the receiver
    client.wait_for("gift notice", lambda: any("GiftNoticeState" in s for s in client.cmd({"cmd": "get_state"})["states"]) or None, timeout=30)
    client.ok({"cmd": "dismiss_notice"})
    client.ok({"cmd": "soldiers_ok"})

    def present():
        r = client.cmd({"cmd": "get_mirror_soldiers", "coopBaseId": host_base_id})
        if r.get("ok"):
            for s in r["soldiers"]:
                if s["name"] == soldier and s["owner"] == 1:
                    return s
        return None

    client.wait_for("soldier survived list-open gift", present, timeout=30)
    print(f"PASS bug1a: '{soldier}' survived gift with client's own list open (+notice shown)")


def test_bug1b_mirror_open(host, client):
    hbase = own_base(host)
    soldier = hbase["soldiers"][0]["name"]  # next soldier (previous one left)
    host_base_id = hbase["coopBaseId"]

    client.ok({"cmd": "visit_coop_base", "base": hbase["name"]})
    client.wait_for("inside peer base", lambda: client.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None, timeout=60)

    host.ok({"cmd": "gift", "name": soldier, "owner": 1})

    # notice pops immediately, and the visited base shows the soldier NOW
    client.wait_for("gift notice while visiting", lambda: any("GiftNoticeState" in s for s in client.cmd({"cmd": "get_state"})["states"]) or None, timeout=30)

    def visible_in_visited_world():
        r = client.cmd({"cmd": "get_soldiers"})
        if not r.get("ok"):
            return None
        for b in r["bases"]:
            if b["coopBaseId"] == host_base_id:
                for s in b["soldiers"]:
                    if s["name"] == soldier and s["owner"] == 1:
                        return s
        return None

    client.wait_for("soldier visible inside visited base", visible_in_visited_world, timeout=15)
    client.ok({"cmd": "dismiss_notice"})

    client.ok({"cmd": "leave_base"})
    client.wait_for("back in own world", lambda: (not client.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None, timeout=60)

    def present():
        r = client.cmd({"cmd": "get_mirror_soldiers", "coopBaseId": host_base_id})
        if not r.get("ok"):
            return None
        hits = [s for s in r["soldiers"] if s["name"] == soldier and s["owner"] == 1]
        return hits[0] if len(hits) == 1 else None  # exactly one - no dup

    client.wait_for("exactly one durable copy after visit", present, timeout=30)
    print(f"PASS bug1b: '{soldier}' visible during visit, notice shown, exactly one copy after leaving")


def main():
    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        bootstrap_fresh_session(host, client)

        test_bug3_owner_resolution(host, client)
        test_bug2_palette(host)
        test_bug1a_list_open(host, client)
        test_bug1b_mirror_open(host, client)
        print("ALL BUG TESTS PASSED")
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
