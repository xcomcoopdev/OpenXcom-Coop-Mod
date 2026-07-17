"""GAP-4: the desync checksum is widened so store/roster/transfer/production
drift can no longer hide from the auto-repair.

Before GAP-4 the checksum the host stamps onto the geoscape `time` heartbeat
hashed only THREE fields - funds + base count + discovered-tech count. Anything
that left those three untouched (base stores, the soldier roster, in-flight
transfers, active production) could diverge on a replica FOREVER and the
auto-repair would never notice: exactly the pre-battle store drift (GAP-5) and
prod_done overshoot (GAP-6) the design still carries. test_joint_world_equal's
negative control demonstrated the blind spot; this test proves it is closed.

  MISS-THEN-CATCH  perturb ONLY the replica's stores (give_items on the client).
                   The OLD three fields (funds/bases/research) stay identical, so
                   the pre-GAP-4 checksum would see NOTHING - asserted directly.
                   The WIDENED checksum's chkItems now differs between host and
                   replica - also asserted directly.
  AUTO-REPAIR      because the widened checksum differs, the replica notices on
                   its own (no force_resync), asks the host for a fresh world, and
                   the host re-streams it: the injected store drift is healed back
                   to full world-equality, and the replica is released (not stuck
                   in the J09 resume-hold, dialog 68).

The perturbation is give_items on ONE machine - a real, silent store desync,
which is precisely the class GAP-4 set out to detect. (set_funds-on-one-machine
was already caught by the funds field; that path is test_joint_resync.)

Run:  python tools/coop_test/test_joint_checksum.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture

RIFLE = "STR_RIFLE"
DRIFT = 25
COOP_DLG_CLIENT_RESUME_HOLD = 68

# The old, narrow checksum - the three fields GAP-4 was too small at.
OLD_FIELDS = ("chkFunds", "chkBases", "chkResearch")


def _chk(gc):
    return gc.ok({"cmd": "joint_checksum"})


def _resync(gc):
    return gc.ok({"cmd": "joint_resync_stats"})


def _coop(gc):
    return gc.ok({"cmd": "get_coop"})


def _assert_released(gc, tag):
    """The client must not be parked in the resume hold (the J09 deadlock)."""
    c = _coop(gc)
    assert c["coopDialog"] != COOP_DLG_CLIENT_RESUME_HOLD, (
        f"{tag}: client is stuck in COOP_DLG_CLIENT_RESUME_HOLD (68) - the "
        f"restream adopted a world but nothing released the hold. get_coop={c}")


def main():
    js = joint_fixture.bring_up("jchk", (48810, 48811, 48110))
    host, client = js.host, js.client
    try:
        # Baseline: one shared world, and the checksum agrees on every field.
        js.assert_world_equal("bootstrap")
        h0, c0 = _chk(host), _chk(client)
        assert h0 == c0, f"bootstrap checksum differs: host={h0} client={c0}"
        _assert_released(client, "bootstrap")
        print(f"PASS setup: identical world + checksum (chkItems={h0['chkItems']}, "
              f"chkSoldiers={h0['chkSoldiers']}), client released")

        host.ok({"cmd": "joint_reset_resync_stats"})
        client.ok({"cmd": "joint_reset_resync_stats"})

        # ================================================================
        # 1) MISS-THEN-CATCH. Add DRIFT rifles to the REPLICA's stores only.
        #    This is a silent store desync: nothing writes replica stores back
        #    on its own, so only the widened checksum's auto-repair can. Read
        #    both checksums IMMEDIATELY (well inside the 3s resync debounce, so
        #    the repair has not fired yet) to compare old vs widened coverage.
        # ================================================================
        client.ok({"cmd": "give_items", "item": RIFLE, "count": DRIFT})
        hc, cc = _chk(host), _chk(client)

        # (a) the OLD three-field checksum is BLIND to this - it would miss it.
        for f in OLD_FIELDS:
            assert hc[f] == cc[f], (
                f"premise broken: store drift moved an OLD checksum field {f} "
                f"(host={hc[f]} client={cc[f]}); it must not, or this proves nothing")
        print("PASS blind-spot: store drift left funds/bases/research IDENTICAL "
              f"({', '.join(f'{f}={hc[f]}' for f in OLD_FIELDS)}) -> the old "
              "3-field checksum would have missed it")

        # (b) the WIDENED checksum SEES it - chkItems diverges by exactly DRIFT.
        assert cc["chkItems"] == hc["chkItems"] + DRIFT, (
            f"widened checksum did not track the replica store drift: "
            f"host chkItems={hc['chkItems']} client chkItems={cc['chkItems']}")
        print(f"PASS detect-field: widened checksum SEES the drift "
              f"(chkItems host={hc['chkItems']} replica={cc['chkItems']}, "
              f"+{DRIFT}); the other old fields unchanged")

        # ================================================================
        # 2) AUTO-REPAIR. The replica notices the widened mismatch ON ITS OWN
        #    (no force_resync), asks for a fresh world, and the host re-streams.
        # ================================================================
        client.wait_for("replica noticed the widened-checksum mismatch",
                        lambda: (_resync(client)["mismatches"] >= 1) or None,
                        timeout=60, interval=0.5)
        client.wait_for("replica asked the host for a resync",
                        lambda: (_resync(client)["requests"] >= 1) or None,
                        timeout=60, interval=0.5)
        print(f"PASS auto-detect: {_resync(client)['mismatches']} mismatch(es) -> "
              "joint_resync_request sent (checksum, not force_resync)")

        # The host re-streams the authoritative world; the replica adopts it and
        # its injected store drift is gone.
        client.wait_for("replica adopted the re-streamed world (chkItems healed)",
                        lambda: (_chk(client)["chkItems"] == _chk(host)["chkItems"]) or None,
                        timeout=90, interval=0.5)
        hs = _resync(host)
        assert hs["requests"] >= 1, f"host did not serve a resync request: {hs}"
        _assert_released(client, "after auto-resync")
        rs = _resync(client)
        assert not rs["gaveUp"], f"replica gave up on a repairable desync: {rs}"
        print(f"PASS repair: host served {hs['requests']} request(s); replica store "
              "drift healed, client released, guard not given up")

        # The whole shared world is equal again - not just chkItems.
        js.assert_world_equal("after auto-resync heal")

        # PRD-J11 shared final-state assertions (world equality + zero-disk).
        js.finish()

        print("ALL JOINT CHECKSUM TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
