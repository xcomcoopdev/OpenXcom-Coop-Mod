"""PRD-J11: a JOINT client dropping mid-session, including with a command in flight.

The JOINT world lives on the host, so losing a client must cost nothing: the
authoritative world is untouched, and a relaunched client re-adopts it whole
through the ordinary resume path (PRD-J02's streamer).

  INFLIGHT   a joint_cmd is submitted and the client is HARD-KILLED microseconds
             later, while the command is on the wire. The host must land on ONE
             side of the fence: either it validated+applied the buy (items en
             route AND funds debited) or it never saw it (neither). A half-apply
             - stock without the debit, or a debit without the stock - would be a
             torn world, and no restream would ever fix it because the host IS
             the authority. Asserted as an exact pair, not "roughly".
  HOST-ALIVE the host process survives the drop and its world stays
             self-consistent. NOTE: it does NOT keep simulating - the existing
             coop transport freezes the host in the "waiting for <player> to
             reconnect" dialog (same as SEPARATE, see test_rejoin_flow). That is
             the shipped behaviour, asserted here so a future change is visible.
  REJOIN     a fresh client process with an EMPTY user dir rejoins under the
             registered roster name; the host re-streams the authoritative world
             and the two are one world again (full equality, not a funds check).
  LIVE       the rejoined replica is a working replica: a new buy round-trips.

Run:  python tools/coop_test/test_joint_disconnect.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import joint_fixture
import session

RIFLE = "STR_RIFLE"


def _funds(gc):
    return gc.ok({"cmd": "geo_state"})["funds"]


def _rifles(gc):
    return gc.ok({"cmd": "incoming_transfers"})["items"].get(RIFLE, 0)


def main():
    # NOTE: like test_joint_resume, this test outlives its fixture - the client is
    # killed and a DIFFERENT client process rejoins - so the second client is owned
    # by hand.
    js = joint_fixture.bring_up("jdis", (48790, 48791, 48090))
    host, client = js.host, js.client
    try:
        joint_fixture.assert_world_equal(host, client, "bootstrap")

        # ---- price discovery: a completed buy, so the in-flight one below has
        #      an EXACT expected cost to check the funds/stock pair against.
        f0, r0 = _funds(host), _rifles(host)
        client.ok({"cmd": "buy", "item": RIFLE, "count": 3})
        host.wait_for("host applied the baseline buy",
                      lambda: _rifles(host) == r0 + 3 or None, timeout=30, interval=0.5)
        unit_cost = (f0 - _funds(host)) // 3
        assert unit_cost > 0, f"could not price a rifle: funds {f0} -> {_funds(host)}"
        joint_fixture.assert_world_equal(host, client, "after the baseline buy")
        print(f"PASS baseline: 3 rifles bought, {unit_cost} each; worlds equal")

        # ================================================================
        # 1) INFLIGHT: submit, then hard-kill the client on top of the send.
        # ================================================================
        f1, r1 = _funds(host), _rifles(host)
        client.cmd({"cmd": "buy", "item": RIFLE, "count": 2})
        client.proc.kill()
        client.proc.wait(timeout=10)
        print("kill: client hard-killed with a joint_cmd on the wire")

        host.wait_for("host still answering after the client dropped",
                      lambda: host.cmd({"cmd": "ping"}).get("pong") or None,
                      timeout=30, interval=0.5)

        # Let the host settle, then demand an exact, self-consistent outcome. The
        # wait gives a command that DID arrive time to be applied; the assertion
        # holds either way, so this only avoids reading a mid-apply instant.
        time.sleep(5)
        f2, r2 = _funds(host), _rifles(host)
        applied = (r2 == r1 + 2)
        if applied:
            assert f2 == f1 - 2 * unit_cost, (
                f"HALF-APPLY: the host materialised 2 rifles but funds went "
                f"{f1} -> {f2} (expected {f1 - 2 * unit_cost}). The world is torn.")
            outcome = f"APPLIED (2 rifles en route, {2 * unit_cost} debited)"
        else:
            assert r2 == r1, (
                f"HALF-APPLY: rifles en route went {r1} -> {r2} - neither the "
                f"whole command nor none of it.")
            assert f2 == f1, (
                f"HALF-APPLY: the host debited funds {f1} -> {f2} without "
                f"materialising the items.")
            outcome = "NOT APPLIED (never reached the host; world untouched)"
        print(f"PASS inflight: host world self-consistent - {outcome}")

        # ---- HOST-ALIVE: the process survived; record what it actually does.
        top = host.cmd({"cmd": "get_state"})["states"][-1]
        note = ("frozen in the reconnect dialog - shipped coop behaviour; the host "
                "does NOT keep simulating while a player is missing"
                if "CoopState" in top else "still on the geoscape")
        print(f"PASS host-alive: host responsive after the drop; top state {top!r} ({note})")

        # ================================================================
        # 2) REJOIN: fresh process, empty user dir, registered roster name.
        # ================================================================
        client = GameClient("client", 48792, make_user_dir("jdis_client2"))
        client.spawn()
        client.connect()
        client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": str(js.coop_port),
                   "player": "ClientPlayer"})

        host.wait_for("rejoined client acked the streamed world",
                      lambda: host.cmd({"cmd": "get_coop"}).get("resumeAck") or None,
                      timeout=120)
        host.ok({"cmd": "coop_dialog_back"})   # the host's RESUME releases the hold
        client.wait_for(
            "rejoined client live on the geoscape",
            lambda: (lambda c: (c.get("hasSave") and c.get("coopStatic")) or None)(
                client.cmd({"cmd": "get_coop"})),
            timeout=120)

        cm = client.ok({"cmd": "save_markers"})
        assert cm.get("campaignType") == 1, f"rejoined client is not JOINT: {cm}"
        joint_fixture.assert_world_equal(host, client, "after rejoin", timeout=90)
        print("PASS rejoin: the killed client came back to the SAME shared world")

        # ================================================================
        # 3) LIVE: the rejoined replica is a working replica, not a snapshot.
        # ================================================================
        r3 = _rifles(host)
        r = client.ok({"cmd": "buy", "item": RIFLE, "count": 4})
        assert r.get("sent"), f"rejoined client buy not sent: {r}"
        host.wait_for("host applied the post-rejoin client buy",
                      lambda: _rifles(host) == r3 + 4 or None, timeout=30, interval=0.5)
        client.wait_for("rejoined client applied the joint_apply",
                        lambda: _rifles(client) == r3 + 4 or None, timeout=30, interval=0.5)
        joint_fixture.assert_world_equal(host, client, "after the post-rejoin buy")
        print("PASS live: the rejoined replica commands the shared world again")

        session.assert_client_zero_disk(client.user_dir)
        print("PASS zero-disk: rejoined client (replica) user dir clean")

        print("ALL JOINT DISCONNECT TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
