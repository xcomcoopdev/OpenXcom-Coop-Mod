"""PRD-J09 AC1 - JOINT mixed-owner squad assembly (pre-battle).

In a JOINT campaign there is ONE shared base / craft / roster. This test proves:

  1. Both players see the FULL roster at the shared base (the SEPARATE
     guest-filter is fenced off in JOINT).
  2. Craft assignment is a shared-world mutation: the CLIENT assigns BOTH a
     host-owned (seat 0) and its own (seat 1) soldier to the shared craft via
     the real CraftSoldiersState path (-> craft_assign command); the host
     validates capacity and broadcasts, and BOTH machines agree on the result.
  3. Un-assigning likewise rides the protocol and agrees on both sides.

Ownership (Soldier::ownerPlayerId / seat) is stamped deterministically on both
machines (set_soldier_owner) to build the mixed squad - it stands in for a real
cross-seat hire, which is exercised by the economy suite.

Run:  python tools/coop_test/test_joint_deploy.py
Exit 0 = pass; 2 = failure.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo


def _roster(gc):
    """Every soldier at the shared base(s), flattened; JOINT has one world."""
    out = []
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        out.extend(b["soldiers"])
    return out


def _skyranger(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if b.get("coopBase") or b.get("coopIcon"):
            continue
        for c in b.get("crafts", []):
            if "SKYRANGER" in c["type"]:
                return c
    return None


def _sol(gc, sid):
    for s in _roster(gc):
        if s["id"] == sid:
            return s
    return None


def _on_craft(gc, sid, cid):
    s = _sol(gc, sid)
    return s is not None and s["craftId"] == cid


def main():
    host = GameClient("host", 48760, make_user_dir("jdep_host"))
    client = GameClient("client", 48761, make_user_dir("jdep_client"))
    fail = []
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client, port="48060", campaign_mode="joint")
        geo.wait_both_ready(host, client)

        # ---- 1) full roster visible to both players ----------------------
        rh = sorted(s["id"] for s in _roster(host))
        rc = sorted(s["id"] for s in _roster(client))
        assert rh == rc, f"rosters differ: host={rh} client={rc}"
        assert len(rh) >= 4, f"need >=4 starting soldiers for a mixed squad, got {rh}"
        print(f"PASS full-roster: both players see all {len(rh)} soldiers {rh}")

        craft = _skyranger(host)
        craft_c = _skyranger(client)
        assert craft and craft_c, "no Skyranger on both sides"
        assert craft["id"] == craft_c["id"], \
            f"craft id differs: host={craft['id']} client={craft_c['id']}"
        cid = craft["id"]

        # ---- 2) build a MIXED squad: A owned by host (0), B by client (1) --
        a, b = rh[0], rh[1]
        for gc in (host, client):
            gc.ok({"cmd": "set_soldier_owner", "soldier_id": a, "owner": 0})
            gc.ok({"cmd": "set_soldier_owner", "soldier_id": b, "owner": 1})
        assert _sol(host, a)["owner"] == _sol(client, a)["owner"] == 0
        assert _sol(host, b)["owner"] == _sol(client, b)["owner"] == 1
        print(f"PASS mixed-owner: soldier {a} -> seat 0 (host), {b} -> seat 1 (client)")

        # empty the shared craft (starting soldiers fill it) so the assignment
        # below is capacity-independent; also proves the CLIENT can de-assign any
        # soldier from the shared craft.
        for sid in rh:
            client.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": False})
        host.wait_for("host craft empty",
                      lambda: all(not _on_craft(host, s, cid) for s in rh) or None, timeout=30, interval=0.5)
        client.wait_for("client craft empty",
                        lambda: all(not _on_craft(client, s, cid) for s in rh) or None, timeout=30, interval=0.5)
        print(f"PASS deassign-all: client cleared shared craft {cid}; both agree it is empty")

        # ---- CLIENT assigns BOTH the host-owned and its own soldier -------
        for sid in (a, b):
            client.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": sid, "on": True})

        for label, sid in (("host-owned", a), ("client-owned", b)):
            host.wait_for(f"host sees {label} {sid} aboard",
                          lambda sid=sid: _on_craft(host, sid, cid) or None, timeout=30, interval=0.5)
            client.wait_for(f"client sees {label} {sid} aboard",
                            lambda sid=sid: _on_craft(client, sid, cid) or None, timeout=30, interval=0.5)
        print(f"PASS assign: client put host-owned {a} + own {b} on shared craft {cid}; both agree")

        # ---- 3) un-assign the host-owned soldier, both agree --------------
        client.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": a, "on": False})
        host.wait_for("host sees A removed",
                      lambda: (not _on_craft(host, a, cid)) or None, timeout=30, interval=0.5)
        client.wait_for("client sees A removed",
                        lambda: (not _on_craft(client, a, cid)) or None, timeout=30, interval=0.5)
        # B still aboard on both
        assert _on_craft(host, b, cid) and _on_craft(client, b, cid), \
            "client-owned B should remain aboard after removing A"
        print(f"PASS unassign: host-owned {a} removed on both; client-owned {b} still aboard")

        print("TEST PASSED")
    except Exception as e:
        print(f"[FAIL] {e}")
        fail.append(str(e))
        try:
            for tag, gc in (("host", host), ("client", client)):
                rs = [(s["id"], s["owner"], s["craftId"]) for s in _roster(gc)]
                print(f"  DBG {tag} (id,owner,craftId): {rs}")
                print(f"  DBG {tag} joint_stats: {gc.cmd({'cmd': 'joint_stats'})}")
        except Exception as e2:
            print(f"  DBG dump failed: {e2}")
    finally:
        host.shutdown(); client.shutdown()
    sys.exit(2 if fail else 0)


if __name__ == "__main__":
    main()
