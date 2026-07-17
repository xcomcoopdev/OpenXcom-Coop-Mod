"""PRD-J11: the shared world-equality helper itself, and what it can see.

The helper (joint_fixture.assert_world_equal) is wired into every JOINT test's
final state, so it has to be trustworthy in BOTH directions: it must pass on an
equal world, and - the part that actually matters - it must FAIL on an unequal
one. A helper that only ever passes proves nothing.

  EQUAL     a freshly bootstrapped JOINT world compares equal, and stays equal
            across a real mutation (a client buy) once the round-trip lands.
  SENSITIVE force a divergence the auto-repair CANNOT see and prove the helper
            catches it. The GAP-4 world checksum now sums total item COUNT (so a
            plain give_items-to-one-machine WOULD trip it - that path is covered
            by test_joint_checksum), but it is only COUNTS: swap 7 rifles on the
            host for 7 pistols on the client and every checksum field - including
            the widened item/soldier/transfer/production counts - stays identical,
            so no mismatch, no auto-resync, nothing in the logs. The world-diff
            helper compares exact CONTENTS, so it still catches it: this is the
            honest negative control for the widened checksum.
  REPAIR    force_resync re-streams the authoritative world and equality returns
            - i.e. the repair primitive fixes drift BEYOND the checksum's reach,
            even though nothing would have triggered it automatically.

Run:  python tools/coop_test/test_joint_world_equal.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture

RIFLE = "STR_RIFLE"
PISTOL = "STR_PISTOL"


def _rifles(gc):
    return gc.ok({"cmd": "incoming_transfers"})["items"].get(RIFLE, 0)


def main():
    js = joint_fixture.bring_up("jweq", (48780, 48781, 48080))
    host, client = js.host, js.client
    try:
        # ---- EQUAL: the bootstrap world ----------------------------------
        js.assert_world_equal("bootstrap")

        # the dump is not vacuous: it must actually carry the shared world.
        d = joint_fixture.world_dump(host)
        assert d["campaignType"] == 1, f"fixture dump says not JOINT: {d['campaignType']}"
        assert len(d["bases"]) == 1, f"expected the one shared base, got {len(d['bases'])}"
        b = d["bases"][0]
        assert b["soldiers"], "dump carries no soldier roster"
        assert b["items"], "dump carries no base stores"
        assert b["facilities"], "dump carries no facility grid"
        assert b["crafts"], "dump carries no craft list"
        print(f"PASS dump: funds={d['funds']} bases=1 soldiers={len(b['soldiers'])} "
              f"items={len(b['items'])} facilities={len(b['facilities'])} "
              f"crafts={len(b['crafts'])}")

        # ---- EQUAL: across a real mutation --------------------------------
        r = client.ok({"cmd": "buy", "item": RIFLE, "count": 4})
        assert r.get("sent"), f"client buy not sent: {r}"
        host.wait_for("host applied the client buy",
                      lambda: _rifles(host) == 4 or None, timeout=30, interval=0.5)
        js.assert_world_equal("after a client buy")

        # ---- SENSITIVE: drift the (widened) checksum cannot see ------------
        # GAP-4 made total item COUNT a checksum field, so a plain give_items to
        # one machine now trips it (test_joint_checksum covers that). To exercise
        # the world-diff helper's blind spot we need a COUNT-preserving CONTENT
        # drift: +7 rifles on the host, +7 pistols on the client. Every checksum
        # count is identical (+7 items on both), so nothing auto-resyncs; only the
        # exact contents differ, which only world_diff sees.
        chk_before = host.ok({"cmd": "joint_checksum"})
        host.ok({"cmd": "give_items", "item": RIFLE, "count": 7})
        client.ok({"cmd": "give_items", "item": PISTOL, "count": 7})
        chk_h = host.ok({"cmd": "joint_checksum"})
        chk_c = client.ok({"cmd": "joint_checksum"})
        assert chk_h == chk_c, \
            (f"count-preserving content drift moved the world checksum - this "
             f"test's premise is stale: host={chk_h} client={chk_c}")
        # ...and the GAP-4 fix really is live: the widened checksum DID track the
        # added stock (item count went up by 7 on both), it just can't tell rifles
        # from pistols.
        assert chk_h["chkItems"] == chk_before["chkItems"] + 7, \
            f"widened checksum did not track the added items: {chk_before} -> {chk_h}"
        print("PASS blind-spot: 7 rifles on the host vs 7 pistols on the client; every "
              "checksum count IDENTICAL (incl. chkItems) -> no auto-resync will fire")

        diff = joint_fixture.world_diff(host, client)
        assert diff, ("the equality helper did NOT catch a content divergence - "
                      "it is not sensitive enough to be worth asserting")
        assert any("items" in d for d in diff), \
            f"caught a divergence, but not the stores one: {diff}"
        print(f"PASS sensitive: helper caught the divergence ({len(diff)} field(s)): "
              f"{diff[0]}")

        # and it is a REAL failure, not just a truthy diff: the assert must raise.
        try:
            joint_fixture.assert_world_equal(host, client, "negative control", timeout=5)
        except AssertionError as e:
            assert "DIVERGED" in str(e), f"unexpected assertion text: {e}"
            print("PASS negative-control: assert_world_equal RAISES on an unequal world")
        else:
            raise AssertionError(
                "assert_world_equal PASSED on a knowingly divergent world - every "
                "'PASS world equality' line in this suite would be worthless")

        # ---- REPAIR: force_resync heals past the checksum's reach ---------
        host.ok({"cmd": "force_resync"})
        joint_fixture.assert_world_equal(host, client, "after force_resync", timeout=90)
        print("PASS repair: force_resync re-streamed the authoritative world; the "
              "store drift the checksum could not see is gone")

        js.finish()
        print("ALL JOINT WORLD-EQUALITY TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
