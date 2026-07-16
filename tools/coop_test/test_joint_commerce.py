"""PRD-J05: JOINT economy UI - sell, hire (with ownership), transfer, containment.

A JOINT campaign is one host-authoritative world. Every inventory/funds-mutating
commerce screen routes through the J03 joint_cmd protocol: the client sends a
command, the host re-validates + re-prices against the live world, applies, and
broadcasts joint_apply (carrying the new authoritative funds). Replicas apply
ONLY from joint_apply. This test drives the REAL Basescape screens
(PurchaseState / SellState / ManageAlienContainmentState / TransferItemsState).

  SELL      client sells 10 rifles -> host and client show IDENTICAL stores +
            funds; funds credited.
  SELL-FAIL host then tries to sell MORE than remain -> joint_fail (atomic, no
            partial), world unchanged on both sides.
  HIRE      client hires 2 soldiers -> after the transfer time (host advances the
            clock) they ARRIVE on both machines with ownerPlayerId == the client
            seat and BYTE-IDENTICAL names + stats (host generates + serializes;
            replica reconstructs, never re-rolls).
  TRANSFER  client transfers items base A->B -> after travel time the destination
            stores are EQUAL on both machines.
  CONTAIN   client sells live aliens from containment -> both sides show the same
            funds credit + containment count.

Run:  python tools/coop_test/test_joint_commerce.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo

RIFLE = "STR_RIFLE"
PISTOL = "STR_PISTOL"
SOLDIER = "STR_SOLDIER"
SECTOID = "STR_SECTOID_SOLDIER"
CLIENT_SEAT = 1  # 2-player: host = seat 0, client = seat 1

BASE_B = "Base B"
BASE_B_LON = 1.1
BASE_B_LAT = 0.35


def _funds(gc):
    return gc.ok({"cmd": "geo_state"})["funds"]


def _report(gc, base=None):
    req = {"cmd": "base_report"}
    if base:
        req["base"] = base
    return gc.ok(req)


def _storage(gc, item, base=None):
    return _report(gc, base)["storage"].get(item, 0)


def _soldiers(gc, base=None):
    return _report(gc, base)["soldiers"]


def _incoming(gc, base=None):
    req = {"cmd": "incoming_transfers"}
    if base:
        req["base"] = base
    return gc.ok(req)


def _stats(gc):
    return gc.ok({"cmd": "joint_stats"})


def _give_both(host, client, item, count, base=None):
    """Add <count> of <item> to the same base on BOTH machines (deterministic;
    keeps the shared world equal for a sell/containment setup)."""
    for gc in (host, client):
        req = {"cmd": "give_items", "item": item, "count": count}
        if base:
            req["base"] = base
        gc.ok(req)


def main():
    host_dir = make_user_dir("jcom_host")
    client_dir = make_user_dir("jcom_client")
    host = GameClient("host", 48700, host_dir)
    client = GameClient("client", 48701, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        session.new_campaign(host, client, port="48000", campaign_mode="joint")
        geo.wait_both_ready(host, client)

        # bootstrap invariant: identical funds.
        assert _funds(host) == _funds(client), "bootstrap funds differ"
        print(f"PASS bootstrap: identical funds {_funds(host)}")

        # ================================================================
        # 1) SELL: client sells 10 rifles -> both sides equal stores + funds
        # ================================================================
        _give_both(host, client, RIFLE, 20)
        r0_h, r0_c = _storage(host, RIFLE), _storage(client, RIFLE)
        assert r0_h == r0_c, f"rifle stock differs after give: {r0_h} vs {r0_c}"
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        f0_h, f0_c = _funds(host), _funds(client)
        assert f0_h == f0_c, "pre-sell funds differ"

        r = client.ok({"cmd": "sell", "item": RIFLE, "count": 10})
        assert r.get("sent"), f"client sell not sent: {r}"
        host.wait_for("host applied sell",
                      lambda: (_storage(host, RIFLE) == r0_h - 10) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client applied sell",
                        lambda: (_storage(client, RIFLE) == r0_c - 10) or None,
                        timeout=30, interval=0.5)
        rh, rc = _storage(host, RIFLE), _storage(client, RIFLE)
        fh, fc = _funds(host), _funds(client)
        assert rh == rc == r0_h - 10, f"post-sell stores mismatch: host={rh} client={rc}"
        assert fh == fc, f"post-sell funds differ: host={fh} client={fc}"
        assert fh > f0_h, f"sell did not credit funds: {f0_h} -> {fh}"
        credit = fh - f0_h
        print(f"PASS sell: both stores {rh} rifles, both funds {fh} (+{credit})")

        # ================================================================
        # 2) SELL overlapping fail: sell more rifles than remain -> reject
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        avail = _storage(host, RIFLE)          # what actually remains
        attempt = avail + 5                    # ask for MORE than exist
        pre_fh, pre_fc = _funds(host), _funds(client)
        pre_rh, pre_rc = _storage(host, RIFLE), _storage(client, RIFLE)

        r = host.ok({"cmd": "sell", "item": RIFLE, "count": attempt})
        assert r.get("sent"), f"host overlapping sell not sent: {r}"
        host.wait_for("host self-rejected overlapping sell",
                      lambda: (_stats(host)["failCount"] >= 1) or None,
                      timeout=30, interval=0.5)
        hs = _stats(host)
        assert hs["lastFail"] == "STR_NOT_ENOUGH_ITEMS_TO_SELL", \
            f"unexpected fail reason: {hs}"
        # world UNCHANGED on both sides (atomic).
        assert _storage(host, RIFLE) == pre_rh and _storage(client, RIFLE) == pre_rc, \
            "stores changed on a rejected sell"
        assert _funds(host) == pre_fh and _funds(client) == pre_fc, \
            "funds changed on a rejected sell"
        print(f"PASS sell-fail: sell {attempt}/{avail} rejected '{hs['lastFail']}', world unchanged")
        # dismiss the host's failure popup so it doesn't block the clock advance.
        try:
            host.ok({"cmd": "coop_dialog_back"})
        except Exception:
            pass

        # ================================================================
        # 3) HIRE: client hires 2 soldiers -> arrive owned by client seat,
        #    byte-identical names + stats on both machines
        # ================================================================
        h_ids0 = {s["id"] for s in _soldiers(host)}
        c_ids0 = {s["id"] for s in _soldiers(client)}
        assert h_ids0 == c_ids0, "starting soldier ids differ between host and client"
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})

        r = client.ok({"cmd": "buy", "item": SOLDIER, "count": 2, "kind": "soldier"})
        assert r.get("sent"), f"client hire not sent: {r}"
        host.wait_for("host hire in transit",
                      lambda: (_incoming(host)["soldiers"] == 2) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client hire in transit",
                        lambda: (_incoming(client)["soldiers"] == 2) or None,
                        timeout=30, interval=0.5)
        print("PASS hire: 2 soldiers en route on both machines")

        # advance past the personnel transfer time (~72h); host completes the
        # transfer + broadcasts transfer_arrived; the replica delivers.
        geo.skip_ingame_time(host, client, minutes=60 * 24 * 4, speed_idx=5, real_timeout=180)
        host.wait_for("host hires arrived",
                      lambda: (len(_soldiers(host)) == len(h_ids0) + 2) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client hires arrived",
                        lambda: (len(_soldiers(client)) == len(c_ids0) + 2) or None,
                        timeout=30, interval=0.5)

        new_h = sorted([s for s in _soldiers(host) if s["id"] not in h_ids0],
                       key=lambda s: s["id"])
        new_c = sorted([s for s in _soldiers(client) if s["id"] not in c_ids0],
                       key=lambda s: s["id"])
        assert len(new_h) == 2 and len(new_c) == 2, \
            f"expected 2 new soldiers each: host={len(new_h)} client={len(new_c)}"
        for s in new_h + new_c:
            assert s["owner"] == CLIENT_SEAT, \
                f"hired soldier owner {s['owner']} != client seat {CLIENT_SEAT}: {s['name']}"
        for sh, sc in zip(new_h, new_c):
            assert sh["id"] == sc["id"], f"soldier id mismatch: {sh['id']} vs {sc['id']}"
            assert sh["name"] == sc["name"], f"soldier name mismatch: {sh['name']} vs {sc['name']}"
            assert sh["stats"] == sc["stats"], \
                f"soldier stats mismatch for {sh['name']}: {sh['stats']} vs {sc['stats']}"
        names = ", ".join(s["name"] for s in new_h)
        print(f"PASS hire arrival: 2 soldiers owned by seat {CLIENT_SEAT}, "
              f"identical names+stats both sides ({names})")

        # ================================================================
        # 4) TRANSFER: client transfers items base A -> B, equal on both
        # ================================================================
        # second shared base (same coords on both -> baseId=1 resolves equally).
        for gc in (host, client):
            gc.ok({"cmd": "add_base", "name": BASE_B, "lon": BASE_B_LON, "lat": BASE_B_LAT})
        _give_both(host, client, PISTOL, 10)   # into source base (base 0)
        p0_h = _storage(host, PISTOL)
        assert p0_h == _storage(client, PISTOL), "source pistol stock differs"
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        f_pre_h, f_pre_c = _funds(host), _funds(client)

        r = client.ok({"cmd": "joint_transfer", "item": PISTOL, "count": 6,
                        "fromBase": None, "toBase": BASE_B})
        assert r.get("sent"), f"client transfer not sent: {r}"
        # source debited on both; destination has the incoming transfer on both.
        host.wait_for("host transfer applied (source debited)",
                      lambda: (_storage(host, PISTOL) == p0_h - 6) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client transfer applied (source debited)",
                        lambda: (_storage(client, PISTOL) == p0_h - 6) or None,
                        timeout=30, interval=0.5)
        assert _incoming(host, BASE_B)["items"].get(PISTOL, 0) == 6, "host dest not en route"
        assert _incoming(client, BASE_B)["items"].get(PISTOL, 0) == 6, "client dest not en route"
        assert _funds(host) == _funds(client), "transfer funds differ (host vs client)"
        assert _funds(host) < f_pre_h, "transfer did not debit the fee"
        print(f"PASS transfer sent: 6 pistols A->B en route both sides, "
              f"fee {f_pre_h - _funds(host)}, funds equal")

        # advance a few hours; the transfer lands at Base B on both machines.
        geo.skip_ingame_time(host, client, minutes=60 * 24, speed_idx=5, real_timeout=120)
        host.wait_for("host dest received",
                      lambda: (_storage(host, PISTOL, BASE_B) == 6) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client dest received",
                        lambda: (_storage(client, PISTOL, BASE_B) == 6) or None,
                        timeout=30, interval=0.5)
        bh, bc = _storage(host, PISTOL, BASE_B), _storage(client, PISTOL, BASE_B)
        assert bh == bc == 6, f"dest stores mismatch: host={bh} client={bc}"
        print(f"PASS transfer arrival: Base B has {bh} pistols on both machines")

        # ================================================================
        # 5) CONTAINMENT: client sells live aliens -> equal funds + count
        # ================================================================
        _give_both(host, client, SECTOID, 3)
        s0_h = _storage(host, SECTOID)
        assert s0_h == _storage(client, SECTOID), "sectoid stock differs"
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        cf0_h, cf0_c = _funds(host), _funds(client)

        r = client.ok({"cmd": "containment", "item": SECTOID, "count": 2, "sell": True})
        assert r.get("sent"), f"client containment not sent: {r}"
        host.wait_for("host containment applied",
                      lambda: (_storage(host, SECTOID) == s0_h - 2) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client containment applied",
                        lambda: (_storage(client, SECTOID) == s0_h - 2) or None,
                        timeout=30, interval=0.5)
        sh2, sc2 = _storage(host, SECTOID), _storage(client, SECTOID)
        cfh, cfc = _funds(host), _funds(client)
        assert sh2 == sc2 == s0_h - 2, f"containment count mismatch: host={sh2} client={sc2}"
        assert cfh == cfc, f"containment funds differ: host={cfh} client={cfc}"
        assert cfh > cf0_h, "containment sell did not credit funds"
        print(f"PASS containment: both {sh2} sectoids left, both funds {cfh} (+{cfh - cf0_h})")

        # standing invariant: the JOINT replica never wrote save data to disk.
        session.assert_client_zero_disk(client_dir)
        print("PASS zero-disk: client (replica) user dir clean")

        print("ALL JOINT COMMERCE TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
