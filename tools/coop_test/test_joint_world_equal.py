"""PRD-J11: the shared world-equality helper itself, and what it can see.

The helper (joint_fixture.assert_world_equal) is wired into every JOINT test's
final state, so it has to be trustworthy in BOTH directions: it must pass on an
equal world, and - the part that actually matters - it must FAIL on an unequal
one. A helper that only ever passes proves nothing.

  EQUAL     a freshly bootstrapped JOINT world compares equal, and stays equal
            across a real mutation (a client buy) once the round-trip lands.
  SENSITIVE force a divergence the auto-repair CANNOT see and prove the helper
            catches it. Base stores are deliberately NOT part of the J04/J10
            world checksum (funds + base count + discovered-tech count), so
            giving items to ONE machine is a permanent, silent divergence: no
            mismatch, no auto-resync, nothing in the logs. Exactly the class of
            drift this helper exists to catch.
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

        # ---- SENSITIVE: drift the checksum cannot see ---------------------
        # give_items is per-machine scaffolding; giving to the HOST only diverges
        # base stores, which are NOT a checksum field.
        chk_before_h = host.ok({"cmd": "joint_checksum"})
        host.ok({"cmd": "give_items", "item": RIFLE, "count": 7})
        chk_after_h = host.ok({"cmd": "joint_checksum"})
        chk_c = client.ok({"cmd": "joint_checksum"})
        assert (chk_after_h["chkFunds"], chk_after_h["chkBases"], chk_after_h["chkResearch"]) == \
               (chk_c["chkFunds"], chk_c["chkBases"], chk_c["chkResearch"]), \
            (f"store drift moved the world checksum - this test's premise is stale: "
             f"host={chk_after_h} client={chk_c}")
        assert chk_before_h == chk_after_h, "giving items moved the host checksum"
        print("PASS blind-spot: 7 rifles added to the HOST only; the world checksum "
              f"is IDENTICAL on both (funds/bases/tech) -> no auto-resync will fire")

        diff = joint_fixture.world_diff(host, client)
        assert diff, ("the equality helper did NOT catch a 7-rifle store divergence - "
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
