"""Coop UFO-notice propagation test.

Question: when one player detects a UFO, does the peer get the notice too?

Mechanism under test (src/Geoscape/UfoDetectedState.cpp:238): the detecting
OWNER of a UFO (coop==false) sends a reliable "ufo_popup" packet; the peer
(connectionTCP "ufo_popup" handler -> GeoscapeState show_coop_ufo_popup) pops a
UfoDetectedState for the matching coop UFO. So host-owned detections should
reach the client. The client-first direction is unverified (the client's own
UfoDetectedState is coop==true, which does NOT send back).

This drives a fresh 2-player session at a moderate speed and, per UFO (by the
shared coop id), records when `detected` first turns true on each side, and
counts the UfoDetectedState popups each side actually shows. Reports both
directions + latency.

Run: python tools/coop_test/test_ufo_notice.py [observe_seconds]
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
from test_geoscape_sync import bringup
from geo import wait_both_ready, drain_popups, on_geoscape, top_state, TimeWatchdog, game_minutes

SPEED = 3  # 30min/tick: UFOs persist and detections are observable
OBSERVE_S = int(sys.argv[1]) if len(sys.argv) > 1 else 150


def detected_map(gc):
    g = gc.cmd({"cmd": "geo_state"})
    if not g.get("ok"):
        return {}
    return {u["coopId"]: bool(u["detected"]) for u in g.get("ufos", [])}


def main():
    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    first_det = {"host": {}, "client": {}}   # coopId -> t (first detected==true)
    popups = {"host": 0, "client": 0}
    prev_ufopopup = {"host": False, "client": False}
    try:
        host.spawn(); client.spawn(); host.connect(); client.connect()
        bringup(host, client)
        wait_both_ready(host, client, 60)
        for gc in (host, client):
            gc.cmd({"cmd": "geo_set_speed", "idx": SPEED})
        print(f"observing UFO detection/notice for {OBSERVE_S}s at speed {SPEED} ...")

        wd = TimeWatchdog([host, client], timeout=30.0)
        t0 = time.time()
        while time.time() - t0 < OBSERVE_S:
            now = time.time() - t0
            for name, gc in (("host", host), ("client", client)):
                top = top_state(gc)
                is_pop = top.endswith("UfoDetectedState")
                if is_pop and not prev_ufopopup[name]:
                    popups[name] += 1     # rising edge = one new notice shown
                prev_ufopopup[name] = is_pop
                for cid, det in detected_map(gc).items():
                    if det and cid not in first_det[name]:
                        first_det[name][cid] = now
                drain_popups(gc)          # close the notice so time keeps moving
                if on_geoscape(gc):
                    gc.cmd({"cmd": "geo_set_speed", "idx": SPEED})
            wd.tick(game_minutes(host))
            time.sleep(0.4)

        allids = set(first_det["host"]) | set(first_det["client"])
        both = 0
        host_only, client_only = [], []
        lat_h2c, lat_c2h = [], []
        for cid in allids:
            th = first_det["host"].get(cid)
            tc = first_det["client"].get(cid)
            if th is not None and tc is not None:
                both += 1
                (lat_h2c if th <= tc else lat_c2h).append(abs(tc - th))
            elif th is not None:
                host_only.append(cid)
            else:
                client_only.append(cid)

        def stat(l):
            return f"n={len(l)} avg={sum(l) / len(l):.1f}s max={max(l):.1f}s" if l else "n=0"

        print("\n===== UFO detection propagation =====")
        print(f"UFOs detected on at least one side: {len(allids)}")
        print(f"  detected on BOTH:                 {both}")
        print(f"  host-only (client never got it):  {len(host_only)} {host_only}")
        print(f"  client-only (host never got it):  {len(client_only)} {client_only}")
        print(f"  host->client latency: {stat(lat_h2c)}")
        print(f"  client->host latency: {stat(lat_c2h)}")
        print(f"UfoDetectedState popups shown: host={popups['host']} client={popups['client']}")

        # Verdict. A UFO detected on one side must reach the other. (Radar
        # coverage can overlap, so some pairs may be independent detections
        # rather than the ufo_popup notify - but either way both must see it.)
        if not allids:
            print("\nverdict: INCONCLUSIVE - no UFOs detected in the window "
                  "(raise observe_seconds)")
            sys.exit(0)
        ok = (not host_only and not client_only)
        print("\nverdict:", "OK - notices propagate both ways" if ok else
              "GAP - some detections did not reach the peer")
        sys.exit(0 if ok else 2)
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
