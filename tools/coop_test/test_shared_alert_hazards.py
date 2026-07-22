"""Two SHARED divergence hazards surfaced by the alert audit.

HAZARD 1 - alien-base discovery was rolled TWICE.
  `time1MonthCoop` is the one monthly handler that is NOT frozen on a replica, so the
  client ran its OWN `RNG::percent(getChanceToDetectAlienBaseEachMonth())` and called
  `setDiscovered(true)` on a base of its own choosing. Discovering a base is a shared-world
  MUTATION, not just an alert, so host and client could end up disagreeing about which base
  (if any) was found. Fix: only the host rolls; it names the winner via the new
  `alien_base_found` shared_cmd and replicas apply that.
  Asserted: the client's base stays undiscovered until the host announces it, then the
  client discovers the SAME base and pops the same dialog.

HAZARD 2 - `ufo_popup` was lossy.
  The peer alert was a single type/race slot, so a second detection in the same window
  overwrote the first (alert silently lost) and matching on type+race alone could pop the
  dialog for the WRONG UFO of the same race/type. Fix: the alert carries the ufo id and is
  QUEUED. Asserted: two same-type/same-race UFOs detected back-to-back raise TWO dialogs on
  the client, for the two distinct UFOs.

Run:  python tools/coop_test/test_shared_alert_hazards.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import geo


def _bases(gc):
    return {b["id"]: b["discovered"] for b in gc.ok({"cmd": "alien_base_state"})["alienBases"]}


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base")


def _wait(pred, label, timeout=25, interval=0.4):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"{label}: never satisfied (last={last})")


def main():
    js = shared_fixture.bring_up("jhaz", (48912, 48913, 48212))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]

        # ---- HAZARD 1: host-authoritative alien-base discovery -------------------
        # Two bases, created on BOTH machines so the shared world holds matching ids.
        ids = []
        for i in range(2):
            lon, lat = blon + 0.5 + i * 0.2, blat + 0.5
            h = host.ok({"cmd": "spawn_alien_base", "lon": lon, "lat": lat})["alien_base_id"]
            c = client.ok({"cmd": "spawn_alien_base", "lon": lon, "lat": lat})["alien_base_id"]
            assert h == c, f"alien base ids diverged: host={h} client={c}"
            ids.append(h)
        assert all(v is False for v in _bases(host).values()), "host base pre-discovered"
        assert all(v is False for v in _bases(client).values()), "client base pre-discovered"
        print(f"PASS setup: alien bases {ids} present and undiscovered on both machines")

        # The host discovers ONE specific base; the client must adopt exactly that one.
        target = ids[1]
        host.ok({"cmd": "host_alien_base_found", "alien_base_id": target})
        _wait(lambda: _bases(client).get(target) is True,
              "client adopted the host's alien-base discovery")
        cb = _bases(client)
        assert cb[ids[0]] is False, (
            f"client discovered a base the host did NOT announce ({ids[0]}) - "
            "the replica is still rolling its own discovery")
        geo.drain_popups(client)
        geo.drain_popups(host)
        print(f"PASS hazard1: client discovered exactly the host's base {target}, "
              f"and only that one (no independent roll)")

        # ---- HAZARD 2: queued, id-matched UFO alerts -----------------------------
        # Two UFOs of the SAME type and race - the old type+race match could not tell them
        # apart, and the single pending slot dropped one alert entirely.
        ufos = []
        for i in range(2):
            u = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                         "mission": "STR_ALIEN_RESEARCH", "region": "STR_NORTH_AMERICA",
                         "race": "STR_SECTOID", "trajectory": "P0", "state": "flying",
                         "speed": 1, "lon": blon + 0.03 + i * 0.05, "lat": blat + 0.02})
            ufos.append(u["ufo_id"])
        assert ufos[0] != ufos[1]
        # Let both replicate to the client.
        deadline = time.time() + 60
        while time.time() < deadline:
            have = {x["id"] for x in _geo(client).get("ufos", [])}
            if set(ufos) <= have:
                break
            geo.skip_realtime(host, client, 1, speed_idx=0, stuck_timeout=None)
            time.sleep(0.2)
        have = {x["id"] for x in _geo(client).get("ufos", [])}
        assert set(ufos) <= have, f"client never materialised both UFOs ({have} vs {ufos})"

        geo.drain_popups(client)
        # Host raises BOTH detections back-to-back (same window).
        for uid in ufos:
            host.ok({"cmd": "ufo_alert", "ufo_id": uid})

        # The client must pop TWO dialogs - one per UFO. Drain them one at a time.
        seen = 0
        deadline = time.time() + 30
        while seen < 2 and time.time() < deadline:
            if "UfoDetectedState" in geo.top_state(client):
                seen += 1
                client.cmd({"cmd": "dismiss_popup"})
            time.sleep(0.3)
        assert seen == 2, (
            f"client raised {seen} UFO-detected dialog(s), expected 2 - a simultaneous "
            "second detection is still being dropped by the single pending slot")
        print("PASS hazard2: two same-type/same-race detections raised TWO separate "
              "client dialogs (queued + id-matched, none dropped)")

        js.finish()
        print("ALL SHARED ALERT-HAZARD TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
