"""PRD-J04: host simulation authority + result broadcasts.

In a JOINT campaign only the HOST runs world simulation (daily/monthly ticks,
research/production/facility progress, funding). Replicas are simulation-frozen
(their timeXxx handlers early-return) and receive results as host broadcasts. A
replica's clock still advances (host `time` packet) and its globe still draws.

  AC1a research: host+client start the SAME cheap project; the host ticks it to
       completion; the replica shows the SAME discovered tech, the project
       REMOVED, its scientists FREED, and identical funds (research_done).
  AC1b month-end: jump the host to month-end and roll it; the replica's funds +
       maintenance tail equal the host's (extended monthly_report packet).
  AC2  facility freeze: a facility's buildTime decrements on the HOST every day
       but NEVER on the replica -- until the host completes it and broadcasts
       fac_done, which drives it to 0 on the replica.
  AC3  positions: a craft launched on the host MOVES on the replica via the
       JOINT position snapshot (the replica never simulates it).

Run:  python tools/coop_test/test_joint_sim.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(g):
    for b in g["bases"]:
        if not b["coopBase"] and not b["coopIcon"]:
            return b
    return g["bases"][0]


def _research_names(gc):
    return [r["name"] for r in _base0(_geo(gc))["research"]]


def _facility(gc, x, y):
    for f in _base0(_geo(gc))["facilities"]:
        if f["x"] == x and f["y"] == y:
            return f
    return None


def _craft_pos(gc, cid):
    for b in _geo(gc)["bases"]:
        for c in b["crafts"]:
            if c["id"] == cid:
                return (c["lon"], c["lat"], c["status"])
    return None


def main():
    js = joint_fixture.bring_up("jsim", (48690, 48691, 47990))
    host, client = js.host, js.client
    try:
        # ---- AC1a: research completion mirrors to the replica --------------
        # bootstrap makes the worlds byte-identical, so free-scientist counts match.
        c_free0 = _base0(_geo(client))["freeScientists"]
        hr = host.ok({"cmd": "start_research", "cost": 1, "scientists": 2})
        assert hr.get("ok"), f"host start_research failed: {hr}"
        topic = hr["topic"]
        cr = client.ok({"cmd": "start_research", "topic": topic, "cost": 1, "scientists": 2})
        assert cr.get("ok"), f"client start_research failed: {cr}"
        assert topic in _research_names(client), "client did not start the project"
        c_free_started = _base0(_geo(client))["freeScientists"]
        assert c_free_started < c_free0, "client scientists not assigned"

        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        # advance ~2 in-game days; the host completes it and broadcasts research_done.
        geo.skip_ingame_time(host, client, minutes=60 * 24 * 2, speed_idx=5, real_timeout=120)
        client.wait_for("replica research_done applied",
                        lambda: (topic not in _research_names(client)) or None,
                        timeout=30, interval=0.5)

        assert host.ok({"cmd": "is_researched", "topic": topic})["researched"], "host tech missing"
        assert client.ok({"cmd": "is_researched", "topic": topic})["researched"], "replica tech missing"
        assert topic not in _research_names(client), "replica project not removed"
        c_free_after = _base0(_geo(client))["freeScientists"]
        assert c_free_after == c_free0, f"replica scientists not freed: {c_free_after} != {c_free0}"
        fh, fc = _geo(host)["funds"], _geo(client)["funds"]
        assert fh == fc, f"funds differ after research: host={fh} client={fc}"
        print(f"PASS AC1a research: replica has {topic}, project removed, "
              f"scientists freed ({c_free_after}), funds {fc}")

        # ---- AC1b: month-end funds/maintenance sync ------------------------
        host.ok({"cmd": "set_geo_day", "day": 28, "hour": 12})
        mp0 = _geo(host)["monthsPassed"]
        t0 = time.time()
        while _geo(host)["monthsPassed"] <= mp0 and time.time() - t0 < 150:
            geo.skip_ingame_time(host, client, minutes=60 * 24 * 2, speed_idx=5, real_timeout=60)
        assert _geo(host)["monthsPassed"] > mp0, "host did not roll the month"
        client.wait_for("replica monthly_report applied",
                        lambda: (_geo(client)["monthsPassed"] > mp0) or None,
                        timeout=30, interval=0.5)
        gh, gc = _geo(host), _geo(client)
        assert gh["funds"] == gc["funds"], \
            f"month funds differ: host={gh['funds']} client={gc['funds']}"
        assert gh["maintenanceTail"] == gc["maintenanceTail"], \
            f"maintenance tail differ: host={gh['maintenanceTail']} client={gc['maintenanceTail']}"
        print(f"PASS AC1b month-end: funds {gc['funds']}, "
              f"maintenance tail {gc['maintenanceTail']} match (monthsPassed {gc['monthsPassed']})")

        # ---- AC2: facility construction freeze + fac_done ------------------
        facs = _base0(_geo(host))["facilities"]
        idx = None
        for i, f in enumerate(facs):
            if "LIFT" not in f["type"]:
                idx = i  # last non-lift facility
        assert idx is not None, "no non-lift facility to test"
        BIG = 90
        sfh = host.ok({"cmd": "set_facility_build_time", "baseId": 0, "index": idx, "time": BIG})
        sfc = client.ok({"cmd": "set_facility_build_time", "baseId": 0, "index": idx, "time": BIG})
        fx, fy = sfh["x"], sfh["y"]
        assert (sfc["x"], sfc["y"]) == (fx, fy), "facility position mismatch host/client"

        geo.skip_ingame_time(host, client, minutes=60 * 24 * 4, speed_idx=5, real_timeout=150)
        hf = _facility(host, fx, fy)
        cf = _facility(client, fx, fy)
        assert hf["buildTime"] < BIG, f"host facility did not build: {hf['buildTime']}"
        assert cf["buildTime"] == BIG, \
            f"replica facility ticked locally: {cf['buildTime']} (should stay {BIG})"
        print(f"PASS AC2 freeze: host facility {hf['buildTime']} < {BIG}, replica frozen at {cf['buildTime']}")

        # finish it on the HOST only -> fac_done -> the replica facility goes to 0.
        host.ok({"cmd": "set_facility_build_time", "baseId": 0, "index": idx, "time": 1})
        geo.skip_ingame_time(host, client, minutes=60 * 24 * 2, speed_idx=5, real_timeout=90)
        client.wait_for("replica fac_done applied",
                        lambda: (_facility(client, fx, fy)["buildTime"] == 0) or None,
                        timeout=30, interval=0.5)
        assert _facility(host, fx, fy)["buildTime"] == 0, "host facility not complete"
        assert _facility(client, fx, fy)["buildTime"] == 0, "replica facility not set 0 by fac_done"
        print("PASS AC2 fac_done: host completion drove the replica facility to 0")

        # ---- AC3: craft positions move on the replica ----------------------
        fcr = host.ok({"cmd": "fly_craft"})
        assert fcr.get("ok"), f"fly_craft failed: {fcr}"
        cid = fcr["craftId"]
        blon, blat = fcr["baseLon"], fcr["baseLat"]

        geo.skip_realtime(host, client, 6, speed_idx=1)
        p1 = _craft_pos(client, cid)
        assert p1 is not None, "replica lost the craft"
        assert p1[2] == "STR_OUT", f"replica craft not airborne: {p1[2]}"
        left_base = abs(p1[0] - blon) > 1e-6 or abs(p1[1] - blat) > 1e-6
        assert left_base, f"replica craft did not leave base: {p1} base=({blon},{blat})"

        geo.skip_realtime(host, client, 5, speed_idx=1)
        p2 = _craft_pos(client, cid)
        hp = _craft_pos(host, cid)
        moved = abs(p2[0] - p1[0]) > 1e-6 or abs(p2[1] - p1[1]) > 1e-6
        synced = hp is not None and abs(hp[0] - p2[0]) < 0.5 and abs(hp[1] - p2[1]) < 0.5
        assert moved or synced, f"replica craft not tracking host: c1={p1} c2={p2} host={hp}"
        print(f"PASS AC3 positions: replica craft {p1[:2]} -> {p2[:2]} tracks host {hp[:2] if hp else None}")

        # PRD-J11: the shared final-state assertions (world equality +
        # the replica's zero-disk invariant).
        js.finish()

        print("ALL JOINT SIM TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
