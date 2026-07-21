"""Bug: clients get no sound effects in a shared dogfight.

In the SHARED model the host simulates every dogfight and clients open render-only replica
windows driven by df_state frames. Every in-combat SFX (weapon fire, hits, explosions) is
raised from the sim body, which a replica never runs (DogfightState::update() returns early
for `_isReplicaView`) - so the client watched the whole fight in silence.

Fix: sounds raised by the host's sim are recorded and carried in the df_state frame; the
replica plays them in applyFrame. Best-effort by design - df_state rides the conflation
slot, so a dropped frame drops its sounds, which is fine for SFX and never used for state.

Asserted here: with a live fight exchanging fire, the CLIENT actually raises dogfight SFX
(pre-fix its count stayed pinned at 0 while the host's climbed).

`soundsPlayed` counts the decision to play, not audio output, because the harness runs
muted on the SDL dummy driver.

Run:  python tools/coop_test/test_shared_dogfight_sfx.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import geo

AGGRESSIVE = 3


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base")


def _ufo(gc, ufo_id):
    for u in _geo(gc).get("ufos", []):
        if u["id"] == ufo_id:
            return u
    return None


def _df(gc, craft_id, ufo_id):
    for df in gc.ok({"cmd": "dogfight_state"}).get("dogfights", []):
        if df["craftId"] == craft_id and df["ufoId"] == ufo_id:
            return df
    return None


def _pump(host, client, n=1):
    geo.skip_realtime(host, client, n, speed_idx=0, stuck_timeout=None)


def main():
    js = shared_fixture.bring_up("jsfx", (48968, 48969, 48268))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]

        sc_h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER", "weapon": "STR_CANNON_UC"})
        sc_c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER", "weapon": "STR_CANNON_UC"})
        assert sc_h["craft_id"] == sc_c["craft_id"], "avenger ids diverged"
        avenger = sc_h["craft_id"]

        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH", "region": "STR_NORTH_AMERICA",
                       "race": "STR_SECTOID", "trajectory": "P0", "state": "flying",
                       "speed": 1, "lon": blon + 0.03, "lat": blat})
        ufo_id = ufo["ufo_id"]

        deadline = time.time() + 45
        while time.time() < deadline and _ufo(client, ufo_id) is None:
            _pump(host, client, 1)
            time.sleep(0.2)
        assert _ufo(client, ufo_id) is not None, "client never materialised the UFO"

        # Client commands the intercept, so both machines hold a live window.
        client.ok({"cmd": "craft_order", "order": "target", "craft_id": avenger,
                   "craft_type": "STR_AVENGER", "ufo_id": ufo_id})
        deadline = time.time() + 120
        while time.time() < deadline:
            _pump(host, client, 1)
            if _df(host, avenger, ufo_id) and _df(client, avenger, ufo_id):
                break
            time.sleep(0.2)
        assert _df(host, avenger, ufo_id) and _df(client, avenger, ufo_id), \
            "the intercept did not open on BOTH machines"
        print("setup: fight open on host and client")

        # Close to weapons range so shots are actually exchanged.
        client.ok({"cmd": "dogfight_action", "action": "aggressive"})

        # Run the fight until the HOST has raised SFX, then require the CLIENT to have too.
        host_snd = client_snd = 0
        deadline = time.time() + 150
        while time.time() < deadline:
            _pump(host, client, 1)
            hd, cd = _df(host, avenger, ufo_id), _df(client, avenger, ufo_id)
            if hd:
                host_snd = max(host_snd, hd.get("soundsPlayed", 0))
            if cd:
                client_snd = max(client_snd, cd.get("soundsPlayed", 0))
            if host_snd > 0 and client_snd > 0:
                break
            if hd is None and cd is None:
                break  # fight ended
            time.sleep(0.15)

        assert host_snd > 0, \
            f"the host never raised any dogfight SFX ({host_snd}) - test setup never fired"
        assert client_snd > 0, (
            f"CLIENT IS SILENT: host raised {host_snd} dogfight SFX, client raised "
            f"{client_snd}. Sim-raised sounds are not reaching the replica's df_state.")
        print(f"PASS sfx: host raised {host_snd} dogfight SFX and the client raised "
              f"{client_snd} - the replica hears the fight")

        print("ALL SHARED DOGFIGHT-SFX TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
