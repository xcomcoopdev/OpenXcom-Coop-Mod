"""Playtest B3: soldier renames must synchronize in a JOINT campaign.

Renaming a soldier was local-only (SoldierInfoState::edtSoldierChange just called
Soldier::setName). In one host-authoritative shared world that leaves the two
machines showing different names for the same soldier. The fix routes the rename
through a soldier_rename joint_cmd (host applies + broadcasts), exactly like the
base_rename that J07 already established.

  CLIENT client renames a soldier by id -> both machines show the new name.
  HOST   host renames a (different) soldier -> both machines agree too.

Run:  python tools/coop_test/test_joint_soldier_rename.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo


def _roster(gc):
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        if not b.get("coopBaseFlag") and not b.get("coopIcon"):
            return b["soldiers"]
    raise AssertionError("no real base in get_soldiers")


def _name_of(gc, sid):
    for s in _roster(gc):
        if s["id"] == sid:
            return s["name"]
    return None


def main():
    js = joint_fixture.bring_up("jsrename", (48780, 48781, 48080))
    host, client = js.host, js.client
    try:
        rh = _roster(host)
        rc = _roster(client)
        ids_h = sorted(s["id"] for s in rh)
        assert ids_h == sorted(s["id"] for s in rc), "starting rosters differ"
        assert len(ids_h) >= 2, f"need >=2 soldiers, got {ids_h}"
        for s in rh:
            assert _name_of(client, s["id"]) == s["name"], \
                f"starting names differ for soldier {s['id']}"
        sid_client, sid_host = ids_h[0], ids_h[1]
        print(f"PASS setup: identical rosters, {len(ids_h)} soldiers; "
              f"client renames {sid_client}, host renames {sid_host}")

        # ---- CLIENT renames a soldier ---------------------------------------
        NEW_C = "Ripley"
        assert _name_of(host, sid_client) != NEW_C, "pick a genuinely new name"
        r = client.ok({"cmd": "soldier_rename", "soldierId": sid_client, "name": NEW_C})
        assert r.get("ok"), f"client soldier_rename not accepted: {r}"
        host.wait_for("host sees the client's rename",
                      lambda: (_name_of(host, sid_client) == NEW_C) or None,
                      timeout=30, interval=0.5)
        assert _name_of(client, sid_client) == NEW_C, "client's own rename not shown locally"
        assert _name_of(host, sid_client) == NEW_C, "host did not receive the rename (B3)"
        print(f"PASS client-rename: soldier {sid_client} -> '{NEW_C}' on both")

        # ---- HOST renames a different soldier -------------------------------
        NEW_H = "Vasquez"
        r = host.ok({"cmd": "soldier_rename", "soldierId": sid_host, "name": NEW_H})
        assert r.get("ok"), f"host soldier_rename not accepted: {r}"
        client.wait_for("client sees the host's rename",
                        lambda: (_name_of(client, sid_host) == NEW_H) or None,
                        timeout=30, interval=0.5)
        assert _name_of(host, sid_host) == NEW_H, "host's own rename not shown locally"
        assert _name_of(client, sid_host) == NEW_H, "client did not receive the rename (B3)"
        print(f"PASS host-rename: soldier {sid_host} -> '{NEW_H}' on both")

        # the earlier rename must still hold (no clobber)
        assert _name_of(host, sid_client) == NEW_C and _name_of(client, sid_client) == NEW_C, \
            "the first rename was lost"
        print("ALL JOINT SOLDIER-RENAME TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
