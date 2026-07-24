"""Issue #78 follow-up sweep: every site-capable (mission, deployment) rule
pair must materialize identically on the HOST and the CLIENT in SHARED.

Phase 1 - type/field parity: enumerates the loaded ruleset YAML
(bin/standard/xcom1/*.rul): alienMissions with objective 3 (site-spawning
missions) x alienDeployments carrying a markerName (minus the alien-base
marker, which belongs to AlienBase objects, not MissionSites). Spawns every
combo on the HOST at distinct coords with varied race/city, then asserts the
client materializes ALL of them with identical
id/type/deployment/mission/race/city/lon/lat/detected - and nothing extra.
Vanilla xcom1 yields one combo (STR_ALIEN_TERROR x STR_TERROR_MISSION); with
a mod list (e.g. XCF) the same test sweeps every modded site type unchanged.

Why field parity matters mechanically: the snapshot receiver
(connectionTCP.cpp, missions loop) does `if (!srule || !dep) continue;` - a
site whose mission/deployment rule fails to resolve on the client is dropped
SILENTLY. Any rule that reaches the client wrong or not at all shows up here.

Phase 2 - detection parity (EXPECTED RED): the host snapshot serializer sends
every site with NO getDetected() filter, and the replica creation path forces
setDetected(true). So the client renders a site the host has not detected
yet - the inverse of issue #78's "only one player can see it". Spawns an
UNDETECTED site on the host and requires the client to mirror the host's
detected=false.

Run:  python tools/coop_test/test_shared_site_types.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import geo

try:
    import yaml
except ImportError:
    print("SKIP: pyyaml not installed (pip install pyyaml)")
    sys.exit(0)

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RULESET_DIR = os.path.join(REPO, "bin", "standard", "xcom1")

# Marker used by AlienBase objects; a deployment carrying it is the alien-base
# assault, never spawned as a MissionSite.
ALIEN_BASE_MARKER = "STR_ALIEN_BASE"

RACES = ["STR_SECTOID", "STR_FLOATER", "STR_SNAKEMAN", "STR_MUTON"]

# Fields that must be byte-identical on both machines for every site.
# secondsRemaining is deliberately excluded (the replica pin is defect A,
# covered red by test_shared_site_expiry); coopId is machine-local random.
PARITY_FIELDS = ["id", "type", "deployment", "mission", "race", "city",
                 "lon", "lat", "detected", "coop"]


def _load_rul(name, key):
    path = os.path.join(RULESET_DIR, name)
    with open(path, "r", encoding="utf-8") as f:
        doc = yaml.safe_load(f)
    return (doc or {}).get(key, []) or []


def enumerate_site_combos():
    """All (mission, deployment) pairs that can form a MissionSite in the
    loaded ruleset. Site missions = objective 3; site deployments = any
    deployment with a markerName that is not the alien-base marker."""
    missions = [m["type"] for m in _load_rul("alienMissions.rul", "alienMissions")
                if isinstance(m, dict) and m.get("objective") == 3]
    deployments = [d["type"] for d in _load_rul("alienDeployments.rul", "alienDeployments")
                   if isinstance(d, dict) and d.get("markerName")
                   and d.get("markerName") != ALIEN_BASE_MARKER]
    assert missions, "no objective-3 (site) alien missions in the ruleset"
    assert deployments, "no marker-carrying site deployments in the ruleset"
    return [(m, d) for m in missions for d in deployments]


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _sites(gc):
    return {s["id"]: s for s in _geo(gc)["missionSites"]}


def _wait(pred, label, timeout=30, interval=0.4):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"{label}: never satisfied (last={last})")


def main():
    combos = enumerate_site_combos()
    print(f"ruleset sweep: {len(combos)} site combo(s): {combos}")

    js = shared_fixture.bring_up("jsitetype", (48988, 48989, 48188))
    host, client = js.host, js.client
    try:
        # ------------------------------------------------------------------
        # phase 1: spawn every combo on the HOST, varied fields, and demand
        # exact materialization on the client.
        # ------------------------------------------------------------------
        spawned = []
        for i, (mission, deployment) in enumerate(combos):
            req = {"cmd": "spawn_mission_site", "mission": mission,
                   "deployment": deployment, "race": RACES[i % len(RACES)],
                   "lon": 0.30 + 0.15 * i, "lat": -0.40 + 0.10 * i,
                   "hours": 200 + i}
            if i == 0:
                req["city"] = "STR_LONDON"   # city fidelity on at least one
            r = host.ok(req)
            spawned.append(r["site_id"])
        print(f"spawned {len(spawned)} site(s) on the host: ids {spawned}")

        _wait(lambda: set(spawned) <= set(_sites(host)), "all sites on host",
              timeout=30)
        try:
            _wait(lambda: set(spawned) <= set(_sites(client)),
                  "all sites materialized on client", timeout=40)
        except AssertionError:
            missing = sorted(set(spawned) - set(_sites(client)))
            details = [(sid, _sites(host)[sid]["mission"],
                        _sites(host)[sid]["deployment"]) for sid in missing]
            raise AssertionError(
                "BUG: site type(s) never appeared on the CLIENT (silent "
                f"rules/deployment drop?) - missing {details}")

        hs, cs = _sites(host), _sites(client)
        assert set(hs) == set(cs), \
            f"site id sets differ: host={sorted(hs)} client={sorted(cs)}"
        for sid in spawned:
            for f in PARITY_FIELDS:
                assert hs[sid].get(f) == cs[sid].get(f), (
                    f"BUG: site {sid} field '{f}' differs: "
                    f"host={hs[sid].get(f)!r} client={cs[sid].get(f)!r} "
                    f"(host site={hs[sid]} client site={cs[sid]})")
        print(f"PASS phase 1: {len(spawned)} site type(s) identical on both "
              f"machines across {PARITY_FIELDS}")

        # ------------------------------------------------------------------
        # phase 2: detection parity. Host spawns an UNDETECTED site; the
        # client must mirror detected=false until the host detects it.
        # EXPECTED RED: the replica creation path forces setDetected(true).
        # ------------------------------------------------------------------
        mission, deployment = combos[0]
        r = host.ok({"cmd": "spawn_mission_site", "mission": mission,
                     "deployment": deployment, "race": RACES[0],
                     "lon": -0.90, "lat": 0.55, "hours": 100,
                     "detected": False})
        sid = r["site_id"]
        assert _wait(lambda: _sites(host).get(sid) or None,
                     "undetected site on host")["detected"] is False, \
            "host site unexpectedly already detected"
        csite = _wait(lambda: _sites(client).get(sid) or None,
                      "undetected site replicated to client", timeout=40)
        if csite["detected"] and not _sites(host)[sid]["detected"]:
            raise AssertionError(
                "BUG: detection asymmetry - client shows site "
                f"{sid} detected={csite['detected']} while the host has "
                f"detected={_sites(host)[sid]['detected']} (replica forces "
                "setDetected(true); host snapshot sends undetected sites)")
        print("PASS phase 2: client mirrors the host's detection state")

        print("ALL SHARED SITE-TYPE SWEEP TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
