"""Issue #78 (SHARED geoscape, terror-site lifecycle) - two of the three symptoms:

  "The terror mission only spawned on the client's Geoscape. The host could
   not see it."
  "When I fast-forwarded time to the next month, the terror mission did not
   disappear at all."

One defect, two symptoms: the SHARED snapshot receiver (connectionTCP.cpp,
target_positions handler) CREATES/updates replica mission sites but never
REMOVES one, and pins every replica site to setSecondsRemaining(100000000).
The replica's own sim is frozen (time5Seconds/time30Minutes early-return for
isSharedReplica), so nothing else can age or sweep it. When the host's copy
expires (processMissionSite sweep in time30Minutes), the host's marker
disappears while the client keeps an immortal one - exactly "host cannot see
it" + "never disappears". Contrast UFOs: the same receiver DOES despawn UFOs
absent from the host's authoritative set; sites have no such loop.

Repro: host spawns a terror site with a short 2h fuse (site state is
host-authoritative in SHARED; the snapshot streams it to the client), both
machines show it, then we skip past expiry. The host sweep removes it; the
client must drop it too.

EXPECTED RED until the fix lands (the client keeps the site forever).

Run:  python tools/coop_test/test_shared_site_expiry.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import geo


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _site(gc, sid):
    for s in _geo(gc)["missionSites"]:
        if s["id"] == sid:
            return s
    return None


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
    js = shared_fixture.bring_up("jsitexp", (48980, 48981, 48180))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)

        # Short-fuse terror site on the HOST - the only site authority in
        # SHARED. 2h fuse; the host sweep removes it once secondsRemaining
        # drops under 30 in-game minutes.
        site = host.ok({"cmd": "spawn_mission_site",
                        "mission": "STR_ALIEN_TERROR",
                        "deployment": "STR_TERROR_MISSION",
                        "lon": b0["lon"] + 0.40, "lat": b0["lat"] + 0.10,
                        "race": "STR_SECTOID", "hours": 2})
        sid = site["site_id"]

        # The geoscape snapshot must materialize it on the client too.
        for tag, gc in (("host", host), ("client", client)):
            _wait(lambda gc=gc: _site(gc, sid) is not None,
                  f"{tag} shows terror site {sid}", timeout=40)
        print(f"PASS setup: terror site {sid} present on host AND client")

        # Age it out. 6 in-game hours is a fat margin over the 2h fuse; stop
        # early the moment the host's copy is swept (the interest predicate is
        # polled on both machines, but only the host can lose the site first -
        # it is the only simulating seat).
        res = geo.skip_ingame_time(
            host, client, minutes=6 * 60, speed_idx=5,
            interest=geo.geo_when(
                lambda g: not any(s["id"] == sid for s in g["missionSites"])),
            real_timeout=150)
        _wait(lambda: _site(host, sid) is None,
              "host swept the expired site", timeout=60)
        print(f"PASS: host removed the expired terror site (skip={res['hit']})")

        # Whatever the expiry popped (site-lost alert etc.) - clear both sides
        # so the replication channel is drained and quiet.
        for gc in (host, client):
            geo.drain_popups(gc)

        # THE BUG (issue #78): the client must lose the site as well. Today the
        # replica copy is pinned at secondsRemaining=100000000 and the snapshot
        # receiver has no site-despawn loop, so this never happens.
        try:
            _wait(lambda: _site(client, sid) is None,
                  "client removed the expired site", timeout=30)
        except AssertionError:
            raise AssertionError(
                "BUG issue #78: expired terror site still on the CLIENT "
                "geoscape after the host removed it - client missionSites="
                f"{_geo(client)['missionSites']}")
        print("PASS: client removed the expired terror site")

        js.finish()
        print("ALL SHARED SITE-EXPIRY TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
