"""Playtest: the shared dogfight window must work for every player.

Requirements (user):
  1. The dogfight window opens FULL for ALL players when the interceptor reaches
     the UFO - regardless of who last commanded the craft (scrap the
     commandingSeat/last-commander minimize).
  2. The stance button is ALWAYS synced to whatever the HOST shows as the active
     mode (host-authoritative; a client click never optimistically moves the
     button and then reverts).
  3. Any player's command applies in the order the host receives it, and sticks.

This test is deliberately extensive (the user asked not to have to revisit it).

The client window is a render-only REPLICA (the host sims every dogfight). We read
`dogfight_state`:
  - `minimized`  : must be False on the client (window opens full).
  - `mode`       : the _mode group pointer (the active stance, 0..4).
  - `highlight`  : which button is actually drawn inverted (0..4). The invariant
                   highlight == mode must hold on the client after EVERY change -
                   a divergence is the "buttons desync / multiple active / revert".

Two full scenarios: HOST-commanded craft (the case that used to open minimized on
the client) and CLIENT-commanded craft.

Run:  python tools/coop_test/test_shared_dogfight_shared.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import session
import geo

STANDOFF, CAUTIOUS, STANDARD, AGGRESSIVE, DISENGAGE = 0, 1, 2, 3, 4
NAMES = {0: "standoff", 1: "cautious", 2: "standard", 3: "aggressive", 4: "disengage"}


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


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


def _wait(gc, craft_id, ufo_id, pred, label, host, client, tries=60):
    last = None
    for _ in range(tries):
        df = _df(gc, craft_id, ufo_id)
        if df is not None:
            last = df
            if pred(df):
                return df
        _pump(host, client, 1)
        time.sleep(0.12)
    raise AssertionError(f"{label}: never satisfied (last={last})")


def _invariant(gc, craft_id, ufo_id, who):
    """The client's highlighted button must equal its active mode (one button lit)."""
    df = _df(gc, craft_id, ufo_id)
    assert df is not None, f"{who}: dogfight vanished"
    assert df["highlight"] == df["mode"], \
        f"{who}: highlight {df['highlight']} != mode {df['mode']} (button desync)"
    return df


def _open_fight(host, client, commander, tag):
    """Spawn craft+UFO, `commander` (host/client) directs the intercept, both open."""
    b0 = _base0(host)
    blon, blat = b0["lon"], b0["lat"]
    sc_h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER", "weapon": "STR_CANNON_UC"})
    sc_c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER", "weapon": "STR_CANNON_UC"})
    assert sc_h["craft_id"] == sc_c["craft_id"], "avenger ids diverged"
    craft_id = sc_h["craft_id"]
    ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT", "mission": "STR_ALIEN_RESEARCH",
                   "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID", "trajectory": "P0",
                   "state": "flying", "speed": 1, "lon": blon + 0.03, "lat": blat})
    ufo_id = ufo["ufo_id"]

    deadline = time.time() + 45
    while time.time() < deadline and _ufo(client, ufo_id) is None:
        _pump(host, client, 1); time.sleep(0.2)
    assert _ufo(client, ufo_id) is not None, f"{tag}: client never materialised the UFO"

    commander.ok({"cmd": "craft_order", "order": "target", "craft_id": craft_id,
                  "craft_type": "STR_AVENGER", "ufo_id": ufo_id})

    deadline = time.time() + 120
    while time.time() < deadline:
        _pump(host, client, 1)
        if _df(host, craft_id, ufo_id) and _df(client, craft_id, ufo_id):
            break
        time.sleep(0.2)
    hd, cd = _df(host, craft_id, ufo_id), _df(client, craft_id, ufo_id)
    assert hd and cd, f"{tag}: intercept did not open on BOTH machines"
    return craft_id, ufo_id, hd, cd


def _scenario(host, client, commander, cname, tag):
    print(f"--- scenario {tag}: {cname}-commanded ---")
    craft_id, ufo_id, hd, cd = _open_fight(host, client, commander, tag)

    # (1) FULL for all: the client window must NOT be minimized, whoever commanded.
    assert cd["replica"] and not hd["replica"], f"{tag}: replica/host role flags wrong"
    assert not cd["minimized"], \
        f"{tag}: client window opened MINIMIZED ({cname}-commanded) - must be full"
    assert not hd["minimized"], f"{tag}: host window minimized"
    _invariant(client, craft_id, ufo_id, f"{tag} baseline")
    print(f"PASS {tag} full: both windows open FULL ({cname} commanded), baseline lit")

    # (2) HOST changes stance -> client button (mode AND highlight) follows.
    for want in (AGGRESSIVE, CAUTIOUS, STANDARD, STANDOFF):
        host.ok({"cmd": "dogfight_action", "action": NAMES[want]})
        _wait(client, craft_id, ufo_id, lambda d: d["mode"] == want and d["highlight"] == want,
              f"{tag} client follows host {NAMES[want]}", host, client)
        _invariant(client, craft_id, ufo_id, f"{tag} host->{NAMES[want]}")
    print(f"PASS {tag} host-sync: client button tracked every host stance (highlight==mode)")

    # (3) CLIENT commands a stance -> host applies (receive order) -> it STICKS on
    #     both, and the client button never settles on a wrong value (no revert).
    for want in (AGGRESSIVE, STANDOFF, CAUTIOUS):
        client.ok({"cmd": "dogfight_action", "action": NAMES[want]})
        _wait(host, craft_id, ufo_id, lambda d: d["mode"] == want,
              f"{tag} host applies client {NAMES[want]}", host, client)
        _wait(client, craft_id, ufo_id, lambda d: d["mode"] == want and d["highlight"] == want,
              f"{tag} client confirms {NAMES[want]}", host, client)
        # stability: pump more and confirm it did NOT revert.
        _pump(host, client, 4)
        df = _invariant(client, craft_id, ufo_id, f"{tag} client->{NAMES[want]} stable")
        assert df["mode"] == want, f"{tag}: client {NAMES[want]} reverted to {df['mode']}"
    print(f"PASS {tag} client-cmd: client stances applied in host order and stuck (no revert)")

    # (4) Conflicting host+client -> last-received-wins, both converge, invariant holds.
    host.ok({"cmd": "dogfight_action", "action": "standoff"})
    client.ok({"cmd": "dogfight_action", "action": "aggressive"})
    _wait(host, craft_id, ufo_id, lambda d: d["mode"] == AGGRESSIVE,
          f"{tag} conflict last-wins host", host, client)
    _wait(client, craft_id, ufo_id, lambda d: d["mode"] == AGGRESSIVE and d["highlight"] == AGGRESSIVE,
          f"{tag} conflict last-wins client", host, client)
    _invariant(client, craft_id, ufo_id, f"{tag} conflict")
    print(f"PASS {tag} conflict: last-received-wins (aggressive), highlight==mode")

    # tidy: disengage to end the fight so the next scenario starts clean.
    client.ok({"cmd": "dogfight_action", "action": "disengage"})
    _pump(host, client, 6)


def main():
    js = shared_fixture.bring_up("jdfshared", (48830, 48831, 48130))
    host, client = js.host, js.client
    try:
        _scenario(host, client, host, "host", "S1")
        _scenario(host, client, client, "client", "S2")
        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive"
        assert host.cmd({"cmd": "ping"}).get("pong"), "host unresponsive"
        print("ALL SHARED-DOGFIGHT TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
