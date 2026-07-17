"""PRD-J03: the JOINT economy command protocol (joint_cmd / joint_apply).

A JOINT campaign is one host-authoritative world. Every economy mutation is a
command to the host; the host validates against the authoritative world, applies,
debits funds, and broadcasts joint_apply (carrying the new authoritative funds)
to all players. Replicas apply ONLY from joint_apply. This test exercises the
reference command "buy" through the real PurchaseState OK path:

  AC1a CLIENT buys 5 rifles from the shared base -> host and client show
       IDENTICAL funds and an IDENTICAL incoming transfer (5 rifles).
  AC1b HOST buys 5 rifles -> same (both sides identical, 10 rifles en route).
  AC1c Insufficient funds -> joint_fail reason surfaces on the initiating
       client (popup + reason), world UNCHANGED on both sides.
  AC2  joint_cmd for an unknown cmd -> joint_fail, no crash (both instances
       stay alive and responsive).

Run:  python tools/coop_test/test_joint_purchase.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session

RIFLE = "STR_RIFLE"


def _funds(gc):
    return gc.ok({"cmd": "geo_state"})["funds"]


def _rifles(gc):
    # incoming transfers on the (single) real, non-mirror base
    return gc.ok({"cmd": "incoming_transfers"})["items"].get(RIFLE, 0)


def _stats(gc):
    return gc.ok({"cmd": "joint_stats"})


def main():
    js = joint_fixture.bring_up("jbuy", (48670, 48671, 47970))
    host, client = js.host, js.client
    try:
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})

        # bootstrap invariant: identical funds, no rifles en route yet.
        f0_h, f0_c = _funds(host), _funds(client)
        assert f0_h == f0_c, f"bootstrap funds differ: host={f0_h} client={f0_c}"
        assert _rifles(host) == 0 and _rifles(client) == 0, "unexpected pre-existing rifles"
        print(f"PASS bootstrap: identical funds {f0_h}, no transfers")

        # ---- AC1a: CLIENT buys 5 rifles ------------------------------------
        r = client.ok({"cmd": "buy", "item": RIFLE, "count": 5})
        assert r.get("sent"), f"client buy not sent: {r}"
        # the mutation only lands after the joint_cmd -> joint_apply round-trip.
        host.wait_for("host applied client buy",
                      lambda: _rifles(host) == 5 or None, timeout=30, interval=0.5)
        client.wait_for("client applied joint_apply",
                        lambda: _rifles(client) == 5 or None, timeout=30, interval=0.5)
        f1_h, f1_c = _funds(host), _funds(client)
        assert f1_h == f1_c, f"AC1a funds differ: host={f1_h} client={f1_c}"
        assert f1_h < f0_h, f"AC1a funds not debited: {f0_h} -> {f1_h}"
        assert _rifles(host) == 5 and _rifles(client) == 5, "AC1a rifle count mismatch"
        cost = f0_h - f1_h
        print(f"PASS AC1a client buy: both funds {f1_h} (-{cost}), 5 rifles en route both sides")

        # ---- AC1b: HOST buys 5 rifles --------------------------------------
        r = host.ok({"cmd": "buy", "item": RIFLE, "count": 5})
        assert r.get("sent"), f"host buy not sent: {r}"
        host.wait_for("host applied own buy",
                      lambda: _rifles(host) == 10 or None, timeout=30, interval=0.5)
        client.wait_for("client applied host buy",
                        lambda: _rifles(client) == 10 or None, timeout=30, interval=0.5)
        f2_h, f2_c = _funds(host), _funds(client)
        assert f2_h == f2_c, f"AC1b funds differ: host={f2_h} client={f2_c}"
        assert f2_h == f1_h - cost, f"AC1b funds not debited by same cost: {f1_h}->{f2_h} (cost {cost})"
        assert _rifles(host) == 10 and _rifles(client) == 10, "AC1b rifle count mismatch"
        print(f"PASS AC1b host buy: both funds {f2_h}, 10 rifles en route both sides")

        # both machines applied both mutations exactly.
        sh, sc = _stats(host), _stats(client)
        assert sh["applyCount"] >= 2, f"host applyCount low: {sh}"
        assert sc["applyCount"] >= 2, f"client applyCount low: {sc}"
        print(f"PASS protocol counters: host apply={sh['applyCount']} client apply={sc['applyCount']}")

        # ---- AC1c: insufficient funds (client-originated) ------------------
        # Drop the funds below the cost of 5 rifles WITHOUT tripping storage
        # limits, then have the client try to buy.
        # set_funds is per-machine scaffolding, so it must be applied to BOTH (the
        # give_items idiom): funds are part of the PRD-J04 world checksum, and
        # since PRD-J10 a host-only set_funds is a genuine desync that the replica
        # now auto-repairs by re-adopting the host's world - which would overwrite
        # the very "replica funds unchanged" invariant this step asserts.
        for gc in (host, client):
            gc.ok({"cmd": "set_funds", "value": 1000})
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        host_funds_pre = _funds(host)          # 1000
        client_funds_pre = _funds(client)      # unchanged replica value
        rifles_pre_h, rifles_pre_c = _rifles(host), _rifles(client)  # 10 / 10

        r = client.ok({"cmd": "buy", "item": RIFLE, "count": 5})
        assert r.get("sent"), f"client (insufficient) buy not sent: {r}"
        client.wait_for("client received joint_fail",
                        lambda: _stats(client)["failCount"] >= 1 or None, timeout=30, interval=0.5)
        cs = _stats(client)
        assert cs["lastFail"] == "STR_NOT_ENOUGH_MONEY", f"unexpected fail reason: {cs}"
        assert session._has_state(client, "CoopState"), "no failure popup surfaced on the client"
        # world unchanged on BOTH sides
        assert _funds(host) == host_funds_pre, "host funds changed on a rejected buy"
        assert _funds(client) == client_funds_pre, "client funds changed on a rejected buy"
        assert _rifles(host) == rifles_pre_h and _rifles(client) == rifles_pre_c, \
            "transfers changed on a rejected buy"
        print(f"PASS AC1c insufficient funds: joint_fail '{cs['lastFail']}' surfaced, world unchanged")
        try:
            client.ok({"cmd": "coop_dialog_back"})  # dismiss the popup
        except Exception:
            pass

        # ---- AC2: unknown command -> joint_fail, no crash ------------------
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_cmd", "jcmd": "bogus_frobnicate", "baseId": 0})
        client.wait_for("client received joint_fail for unknown cmd",
                        lambda: _stats(client)["failCount"] >= 1 or None, timeout=30, interval=0.5)
        cs = _stats(client)
        assert "unknown command" in cs["lastFail"], f"unexpected unknown-cmd reason: {cs}"
        hs = _stats(host)
        assert hs["unknownCount"] >= 1, f"host did not count the unknown cmd: {hs}"
        # both instances are still alive and responsive (no crash).
        assert host.cmd({"cmd": "ping"}).get("pong"), "host unresponsive after unknown cmd"
        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive after unknown cmd"
        print(f"PASS AC2 unknown cmd: joint_fail '{cs['lastFail']}', both instances alive")
        try:
            client.ok({"cmd": "coop_dialog_back"})
        except Exception:
            pass

        # PRD-J11: the shared final-state assertions (world equality +
        # the replica's zero-disk invariant).
        js.finish()

        print("ALL JOINT PURCHASE TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
