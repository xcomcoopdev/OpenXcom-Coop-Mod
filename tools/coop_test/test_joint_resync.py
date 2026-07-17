"""PRD-J10: desync detection is upgraded from log-only (J04) to automatic repair.

The host stamps a world checksum (funds + base count + discovered tech) onto the
periodic geoscape `time` heartbeat. J04 logged a mismatch and moved on. J10 makes
the replica ASK for a fresh world (joint_resync_request); the host re-streams the
authoritative world down the J02 bootstrap lane AND releases the client's resume
hold, and the replica adopts it whole.

  DETECT   corrupt the replica's funds directly (set_funds on the client only) ->
           the next checksum mismatches, the replica requests a resync, the host
           re-streams, and the replica's funds match the host's again.
  RELEASE  the repaired client must NOT be parked in COOP_DLG_CLIENT_RESUME_HOLD
           (68). LoadGameState holds EVERY client that adopts a streamed world
           until a campaign_begun arrives, and mid-session nobody clicks BEGIN -
           this is the deadlock PRD-J09 hit after battles. Asserted explicitly.
  LIVE     after the repair the session still works: a client buy still round-
           trips through the protocol into both worlds.
  FORCE    the TestServer force_resync hook (the PRD's debug trigger) re-streams
           on demand, past the throttle, and leaves the client released again.

Run:  python tools/coop_test/test_joint_resync.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo

RIFLE = "STR_RIFLE"
COOP_DLG_CLIENT_RESUME_HOLD = 68


def _funds(gc):
    return gc.ok({"cmd": "geo_state"})["funds"]


def _resync(gc):
    return gc.ok({"cmd": "joint_resync_stats"})


def _coop(gc):
    return gc.ok({"cmd": "get_coop"})


def _rifles(gc):
    return gc.ok({"cmd": "incoming_transfers"})["items"].get(RIFLE, 0)


def _assert_released(gc, tag):
    """The client must not be parked in the resume hold (the J09 deadlock)."""
    c = _coop(gc)
    assert c["coopDialog"] != COOP_DLG_CLIENT_RESUME_HOLD, (
        f"{tag}: client is stuck in COOP_DLG_CLIENT_RESUME_HOLD (68) - the "
        f"restream adopted a world but nothing released the hold. get_coop={c}")


def main():
    host_dir = make_user_dir("jrsy_host")
    client_dir = make_user_dir("jrsy_client")
    host = GameClient("host", 48740, host_dir)
    client = GameClient("client", 48741, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        session.new_campaign(host, client, port="48040", campaign_mode="joint")
        geo.wait_both_ready(host, client)

        f0 = _funds(host)
        assert f0 == _funds(client), f"bootstrap funds differ: {f0} vs {_funds(client)}"
        _assert_released(client, "bootstrap")
        print(f"PASS setup: identical funds {f0}, client released")

        # ================================================================
        # 1) DETECT + REPAIR. Corrupting the REPLICA's funds is the cleanest
        #    forced mismatch: it changes exactly one checksum field, and only
        #    the repair can put it back (nothing else writes replica funds
        #    while no commands are running).
        # ================================================================
        host.ok({"cmd": "joint_reset_resync_stats"})
        client.ok({"cmd": "joint_reset_resync_stats"})

        BOGUS = 12345
        client.ok({"cmd": "set_funds", "value": BOGUS})
        assert _funds(client) == BOGUS, "replica funds not corrupted"
        assert _funds(host) == f0, "host funds must be untouched"
        print(f"PASS corrupt: replica funds forced to {BOGUS} (host still {f0})")

        client.wait_for("replica noticed the checksum mismatch",
                        lambda: (_resync(client)["mismatches"] >= 1) or None,
                        timeout=60, interval=0.5)
        client.wait_for("replica asked the host for a resync",
                        lambda: (_resync(client)["requests"] >= 1) or None,
                        timeout=60, interval=0.5)
        print(f"PASS detect: {_resync(client)['mismatches']} mismatch(es) -> "
              f"joint_resync_request sent")

        # The host serves it by re-streaming the authoritative world; the replica
        # adopts it through the same bootstrap path (CoopState 555 -> LoadGameState).
        client.wait_for("replica adopted the re-streamed world",
                        lambda: (_funds(client) == _funds(host)) or None,
                        timeout=90, interval=0.5)
        f1_h, f1_c = _funds(host), _funds(client)
        assert f1_c == f1_h, f"resync did not converge: host={f1_h} client={f1_c}"
        assert f1_c != BOGUS, "replica still holds the corrupted funds"
        hs = _resync(host)
        assert hs["requests"] >= 1, f"host did not serve a resync request: {hs}"
        print(f"PASS repair: replica funds {BOGUS} -> {f1_c} == host {f1_h} "
              f"(host served {hs['requests']} request(s))")

        # ================================================================
        # 2) RELEASE. THE regression guard for the J09 deadlock: adopting a
        #    streamed world always pushes the resume hold, and only the host's
        #    campaign_begun clears it.
        # ================================================================
        client.wait_for("client released from the resume hold",
                        lambda: (_coop(client)["coopDialog"] != COOP_DLG_CLIENT_RESUME_HOLD) or None,
                        timeout=60, interval=0.5)
        _assert_released(client, "after auto-resync")
        rs = _resync(client)
        assert not rs["pending"], f"resync guard never cleared: {rs}"
        assert not rs["gaveUp"], f"replica gave up on a repairable desync: {rs}"
        print(f"PASS release: client not held (dialog="
              f"{_coop(client)['coopDialog']}), resync guard cleared")

        # ================================================================
        # 3) LIVE. The repaired replica is a working replica, not just a
        #    correct one: the protocol still round-trips both ways.
        # ================================================================
        rifles_pre = _rifles(host)
        r = client.ok({"cmd": "buy", "item": RIFLE, "count": 3})
        assert r.get("sent"), f"client buy not sent after resync: {r}"
        host.wait_for("host applied the post-resync client buy",
                      lambda: (_rifles(host) == rifles_pre + 3) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client applied the post-resync joint_apply",
                        lambda: (_rifles(client) == rifles_pre + 3) or None,
                        timeout=30, interval=0.5)
        assert _funds(host) == _funds(client), "funds diverged after the post-resync buy"
        print(f"PASS live: post-resync buy round-tripped, funds equal "
              f"({_funds(host)}) on both")

        # ================================================================
        # 4) FORCE. The PRD's manual/debug trigger, past the throttle.
        # ================================================================
        client.ok({"cmd": "joint_reset_resync_stats"})
        host.ok({"cmd": "joint_reset_resync_stats"})
        r = client.ok({"cmd": "force_resync"})
        assert r.get("role") == "replica" and r.get("sent"), f"force_resync not sent: {r}"
        host.wait_for("host served the forced resync",
                      lambda: (_resync(host)["requests"] >= 1) or None,
                      timeout=60, interval=0.5)
        client.wait_for("client adopted the forced re-stream",
                        lambda: (not _resync(client)["pending"]) or None,
                        timeout=90, interval=0.5)
        _assert_released(client, "after force_resync")
        assert _funds(host) == _funds(client), "forced resync left the worlds unequal"
        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive after force_resync"
        assert host.cmd({"cmd": "ping"}).get("pong"), "host unresponsive after force_resync"
        print("PASS force: force_resync re-streamed on demand, client released, "
              "worlds equal, both alive")

        # standing invariant: the JOINT replica never wrote save data to disk -
        # a resync adopts a STREAMED world, it does not load one from disk.
        session.assert_client_zero_disk(client_dir)
        print("PASS zero-disk: client (replica) user dir clean")

        print("ALL JOINT RESYNC TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
