"""PRD-J07: JOINT facility build / dismantle + rename + sack via joint_cmd.

A JOINT campaign is one host-authoritative world. The Basescape facility screens
mutate NOTHING locally: placing a facility, dismantling one, renaming a base and
sacking a soldier each ride the J03 joint_cmd protocol. The host validates
against the live world (placement rules exactly as PlaceFacilityState), applies,
and broadcasts joint_apply; replicas apply ONLY from joint_apply.

  BUILD     client drives the REAL PlaceFacilityState at a free tile -> fac_build
            -> facility (in construction) present on BOTH sides, funds debited
            equally once.
  CONFLICT  both players build on the SAME tile in one pump -> host order
            decides: ONE succeeds, ONE gets joint_fail, worlds identical, funds
            debited exactly once (the vanilla validity re-check IS the guard).
  DISMANTLE client dismantles the first facility via the REAL
            DismantleFacilityState -> gone on both sides, refund applied
            identically (vanilla refund of an in-progress build is 0).
  RENAME    client renames the base via the REAL BasescapeState name box ->
            base_rename -> both sides agree (replaces SEPARATE changeBaseName).
  SACK      client sacks a soldier by id -> sack -> removed on both sides (any
            player may sack any soldier).

Run:  python tools/coop_test/test_joint_facilities.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo

FACILITY = "STR_LIVING_QUARTERS"
FACILITY_COST = 400000  # vanilla build cost
BASE_SIZE = 6


def _base0(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _funds(gc):
    return gc.ok({"cmd": "geo_state"})["funds"]


def _fac_at(gc, x, y):
    """The facility dict whose ORIGIN is (x,y), or None."""
    for f in _base0(gc)["facilities"]:
        if f["x"] == x and f["y"] == y:
            return f
    return None


def _occupancy(base):
    """Set of occupied (x,y) tiles + set of tiles of BUILT facilities."""
    occupied, built = set(), set()
    for f in base["facilities"]:
        for dx in range(f.get("sizeX", 1)):
            for dy in range(f.get("sizeY", 1)):
                t = (f["x"] + dx, f["y"] + dy)
                occupied.add(t)
                if f["buildTime"] == 0:
                    built.add(t)
    return occupied, built


def _free_tiles_next_to_built(base, count):
    """Deterministic list of free tiles orthogonally adjacent to a BUILT tile."""
    occupied, built = _occupancy(base)
    out = []
    for y in range(BASE_SIZE):
        for x in range(BASE_SIZE):
            if (x, y) in occupied:
                continue
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if (nx, ny) in built:
                    out.append((x, y))
                    break
            if len(out) == count:
                return out
    raise AssertionError(f"could not find {count} buildable tiles (got {out})")


def _stats(gc):
    return gc.ok({"cmd": "joint_stats"})


def _dismiss(gc):
    try:
        gc.ok({"cmd": "coop_dialog_back"})
    except Exception:
        pass


def main():
    js = joint_fixture.bring_up("jfac", (48750, 48751, 48050))
    host, client = js.host, js.client
    try:
        b0h, b0c = _base0(host), _base0(client)
        assert b0h["name"] == b0c["name"], "starting base differs"
        (ax, ay), (bx, by) = _free_tiles_next_to_built(b0h, 2)
        funds0 = _funds(host)
        assert funds0 == _funds(client), "starting funds differ"
        print(f"PASS setup: tiles A=({ax},{ay}) B=({bx},{by}); funds {funds0}")

        # ================================================================
        # 1) BUILD: client drives the REAL PlaceFacilityState -> fac_build.
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "fac_build", "facility": FACILITY, "x": ax, "y": ay})
        host.wait_for("host got facility A",
                      lambda: (_fac_at(host, ax, ay) is not None) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client got facility A",
                        lambda: (_fac_at(client, ax, ay) is not None) or None,
                        timeout=30, interval=0.5)
        fh, fc = _fac_at(host, ax, ay), _fac_at(client, ax, ay)
        assert fh["type"] == fc["type"] == FACILITY, f"wrong facility type: {fh} vs {fc}"
        assert fh["buildTime"] == fc["buildTime"] > 0, \
            f"buildTime mismatch: host={fh['buildTime']} client={fc['buildTime']}"
        assert _funds(host) == _funds(client) == funds0 - FACILITY_COST, \
            f"funds after build: host={_funds(host)} client={_funds(client)} want {funds0 - FACILITY_COST}"
        print(f"PASS build: {FACILITY} at ({ax},{ay}) in construction on both "
              f"({fh['buildTime']} days); funds -> {_funds(host)}")

        # ================================================================
        # 2) CONFLICT: both players target tile B in one pump. Host order
        #    decides; loser gets joint_fail; worlds identical; one debit.
        #    The client rides the REAL screen; the host's competing command is
        #    submitted RAW (no local pre-check) so the loser is rejected by the
        #    HOST VALIDATOR re-check - the guard the PRD names - regardless of
        #    which of the two wins the FIFO race.
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        funds1 = _funds(host)
        client.ok({"cmd": "fac_build", "facility": FACILITY, "x": bx, "y": by})
        host.ok({"cmd": "joint_cmd", "jcmd": "fac_build", "baseId": 0,
                 "payload": {"facilityType": FACILITY, "x": bx, "y": by}})

        def _settled():
            fh2, fc2 = _fac_at(host, bx, by), _fac_at(client, bx, by)
            if fh2 is None or fc2 is None:
                return None
            if _funds(host) != funds1 - FACILITY_COST:
                return None
            if _funds(client) != funds1 - FACILITY_COST:
                return None
            # the loser's rejection must have surfaced on exactly one machine
            if _stats(host)["failCount"] + _stats(client)["failCount"] < 1:
                return None
            return True

        host.wait_for("conflict settled", _settled, timeout=30, interval=0.5)
        n_host = len(_base0(host)["facilities"])
        n_client = len(_base0(client)["facilities"])
        assert n_host == n_client, f"facility count differs: {n_host} vs {n_client}"
        fails = (_stats(host)["failCount"], _stats(client)["failCount"])
        last = _stats(host)["lastFail"] or _stats(client)["lastFail"]
        assert sum(fails) >= 1, "no joint_fail surfaced for the losing build"
        # dismiss the loser's failure popup (host or client) so time can advance
        _dismiss(host)
        _dismiss(client)
        print(f"PASS conflict: one build won tile ({bx},{by}), loser rejected "
              f"('{last}', fails host/client={fails}); single debit -> {_funds(host)}")

        # ================================================================
        # 3) DISMANTLE: client removes facility A via the REAL dialog.
        #    Vanilla refund of an in-progress build is 0 -> funds unchanged.
        # ================================================================
        funds2 = _funds(host)
        client.ok({"cmd": "fac_dismantle", "x": ax, "y": ay})
        host.wait_for("host facility A gone",
                      lambda: (_fac_at(host, ax, ay) is None) or None,
                      timeout=30, interval=0.5)
        client.wait_for("client facility A gone",
                        lambda: (_fac_at(client, ax, ay) is None) or None,
                        timeout=30, interval=0.5)
        assert _funds(host) == _funds(client) == funds2, \
            f"dismantle refund mismatch: host={_funds(host)} client={_funds(client)} want {funds2}"
        assert len(_base0(host)["facilities"]) == len(_base0(client)["facilities"]), \
            "facility lists diverged after dismantle"
        print(f"PASS dismantle: ({ax},{ay}) gone on both; funds unchanged at {_funds(host)}")

        # ================================================================
        # 4) RENAME: client renames the shared base via the REAL name box.
        # ================================================================
        old_name = _base0(host)["name"]
        client.ok({"cmd": "base_rename", "base": old_name, "name": "Joint HQ"})
        host.wait_for("host sees rename",
                      lambda: (_base0(host)["name"] == "Joint HQ") or None,
                      timeout=30, interval=0.5)
        client.wait_for("client sees rename",
                        lambda: (_base0(client)["name"] == "Joint HQ") or None,
                        timeout=30, interval=0.5)
        print(f"PASS rename: '{old_name}' -> 'Joint HQ' on both")

        # ================================================================
        # 5) SACK: client sacks a soldier by id -> removed on both sides.
        # ================================================================
        rep_h = host.ok({"cmd": "base_report", "base": "Joint HQ"})
        rep_c = client.ok({"cmd": "base_report", "base": "Joint HQ"})
        ids_h = sorted(s["id"] for s in rep_h["soldiers"])
        ids_c = sorted(s["id"] for s in rep_c["soldiers"])
        assert ids_h == ids_c and len(ids_h) > 0, f"soldier rosters differ: {ids_h} vs {ids_c}"
        victim = ids_h[0]
        n0 = len(ids_h)
        client.ok({"cmd": "sack", "soldierId": victim, "base": "Joint HQ"})

        def _sacked(gc):
            rep = gc.ok({"cmd": "base_report", "base": "Joint HQ"})
            ids = [s["id"] for s in rep["soldiers"]]
            return (victim not in ids and len(ids) == n0 - 1) or None

        host.wait_for("host sacked", lambda: _sacked(host), timeout=30, interval=0.5)
        client.wait_for("client sacked", lambda: _sacked(client), timeout=30, interval=0.5)
        print(f"PASS sack: soldier {victim} removed on both ({n0} -> {n0 - 1})")

        # PRD-J11: the shared final-state assertions (world equality +
        # the replica's zero-disk invariant).
        js.finish()

        print("ALL JOINT FACILITIES TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
