"""PRD-DF02 (replicated dogfight control): any player COMMANDS a shared dogfight.

Builds on DF01 (host-sims-all + render-only replicas). A CLIENT-commanded intercept
opens the SAME fight on both machines (client = commanding seat -> both full). The
client now issues stance / weapon / disengage commands on its replica-view window;
each is emitted as a df_cmd on the reliable joint_cmd lane, applied by the host to its
authoritative DogfightState in receive-order, and surfaced back to every replica on the
next df_state frame.

Acceptance (PRD-DF02):
  1. CLIENT dogfight_action{aggressive} -> host _mode changes; next df_state reflects
     aggressive on BOTH machines.
  2. Conflicting host+client stances -> deterministic last-received-wins (the later
     command wins), both converge, no crash.
  3. CLIENT weaponToggle disables a weapon -> host + client both render it disabled
     (the fire gate reads _weaponEnabled, streamed in df_state).
  4. Synced UFO attack-mode marker (ufoStance) renders the SAME posture on both.
  5. CLIENT minimize -> only the client's view minimizes (host window stays full).
  6. CLIENT disengage -> the fight breaks off on BOTH machines.

Run:  python tools/coop_test/test_joint_dogfight_control.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo

# df_state wire enum: 0 standoff, 1 cautious, 2 standard, 3 aggressive, 4 disengage.
STANDOFF, CAUTIOUS, STANDARD, AGGRESSIVE, DISENGAGE = 0, 1, 2, 3, 4


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


def _df_for(gc, craft_id, ufo_id):
    d = gc.ok({"cmd": "dogfight_state"})
    for df in d.get("dogfights", []):
        if df["craftId"] == craft_id and df["ufoId"] == ufo_id:
            return df
    return None


def _pump(host, client, n=1):
    geo.skip_realtime(host, client, n, speed_idx=0, stuck_timeout=None)


def _wait_mode(gc, craft_id, ufo_id, want, label, host=None, client=None, tries=40):
    """Pump until this machine's dogfight reports stance @a want (or fail)."""
    last = None
    for _ in range(tries):
        df = _df_for(gc, craft_id, ufo_id)
        if df is not None:
            last = df["mode"]
            if last == want:
                return df
        if host and client:
            _pump(host, client, 1)
        time.sleep(0.15)
    raise AssertionError(f"{label}: mode never became {want} (last {last})")


def main():
    js = joint_fixture.bring_up("jdfc", (48784, 48785, 48084))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]

        sc_h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                        "weapon": "STR_CANNON_UC"})
        sc_c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER",
                          "weapon": "STR_CANNON_UC"})
        assert sc_h["craft_id"] == sc_c["craft_id"], "avenger ids diverged"
        avenger_id = sc_h["craft_id"]

        # A full-hull slow scout near the base: the fight persists (a non-HK intercept
        # opens in STANDOFF and does not auto-fire) so we can drive it through commands.
        ufo = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                       "mission": "STR_ALIEN_RESEARCH",
                       "region": "STR_NORTH_AMERICA", "race": "STR_SECTOID",
                       "trajectory": "P0", "state": "flying", "speed": 1,
                       "lon": blon + 0.03, "lat": blat})
        ufo_id = ufo["ufo_id"]
        print(f"setup: avenger id {avenger_id}; scout id {ufo_id}")

        # Materialise the UFO on the client, then the CLIENT commands the intercept
        # (client = commanding seat -> full window on both machines).
        deadline = time.time() + 45
        while time.time() < deadline and _ufo(client, ufo_id) is None:
            _pump(host, client, 1)
            time.sleep(0.2)
        assert _ufo(client, ufo_id) is not None, "client never materialised the UFO"
        client.ok({"cmd": "craft_order", "order": "target",
                   "craft_id": avenger_id, "craft_type": "STR_AVENGER",
                   "ufo_id": ufo_id})
        print("client commanded the intercept")

        # Both machines open the fight for the same pair.
        deadline = time.time() + 120
        while time.time() < deadline:
            _pump(host, client, 1)
            if _df_for(host, avenger_id, ufo_id) and _df_for(client, avenger_id, ufo_id):
                break
            time.sleep(0.2)
        hd = _df_for(host, avenger_id, ufo_id)
        cd = _df_for(client, avenger_id, ufo_id)
        assert hd and cd, "the intercept did not open on BOTH machines"
        # Presentation policy: a client-commanded craft -> the client IS the commanding
        # seat -> its window is FULL (not minimized); the host is always full.
        assert not cd["minimized"], "client (commanding seat) should open FULL"
        assert cd["replica"] and not hd["replica"], "replica/host role flags wrong"
        print("PASS setup: both opened; client is the commanding seat (full window)")

        # --- AC1: CLIENT issues aggressive -> host _mode changes; both converge. ------
        # Optimistic echo: the client's own window flips immediately.
        client.ok({"cmd": "dogfight_action", "action": "aggressive"})
        cd = _df_for(client, avenger_id, ufo_id)
        assert cd["mode"] == AGGRESSIVE, \
            f"optimistic echo failed: client mode {cd['mode']} != aggressive"
        # The host applies the df_cmd (receive-order) and streams it back.
        _wait_mode(host, avenger_id, ufo_id, AGGRESSIVE,
                   "AC1 host aggressive", host, client)
        _wait_mode(client, avenger_id, ufo_id, AGGRESSIVE,
                   "AC1 client confirm aggressive", host, client)
        print("PASS AC1: client aggressive -> host _mode aggressive, both converged")

        # --- AC2: conflicting stances -> last-received-wins. -------------------------
        # Host sets standoff (applied locally, immediately). Then the client sets
        # cautious (df_cmd, received by the host AFTER). The later command wins.
        host.ok({"cmd": "dogfight_action", "action": "standoff"})
        client.ok({"cmd": "dogfight_action", "action": "cautious"})
        _wait_mode(host, avenger_id, ufo_id, CAUTIOUS,
                   "AC2 host last-received-wins (cautious)", host, client)
        _wait_mode(client, avenger_id, ufo_id, CAUTIOUS,
                   "AC2 client converge (cautious)", host, client)
        print("PASS AC2: conflicting stances resolved last-received-wins (cautious), "
              "both converged")

        # --- AC4: synced UFO attack-mode marker parity (check while the fight lives). -
        hd = _df_for(host, avenger_id, ufo_id)
        cd = _df_for(client, avenger_id, ufo_id)
        assert hd["ufoStance"] == cd["ufoStance"], (
            f"PRD-DF02 FAILED: ufoStance marker diverged host={hd['ufoStance']} "
            f"client={cd['ufoStance']}")
        print(f"PASS AC4: synced UFO stance marker parity (ufoStance="
              f"{hd['ufoStance']} on both)")

        # --- AC3: CLIENT weaponToggle -> weapon disabled on host + client. -----------
        hd = _df_for(host, avenger_id, ufo_id)
        assert hd["weaponEnabled"] and hd["weaponEnabled"][0], \
            f"weapon 0 not initially enabled on host: {hd['weaponEnabled']}"
        client.ok({"cmd": "dogfight_action", "action": "weaponToggle", "arg": 0})
        # optimistic echo on the client
        cd = _df_for(client, avenger_id, ufo_id)
        assert cd["weaponEnabled"] and cd["weaponEnabled"][0] is False, \
            f"optimistic weapon echo failed on client: {cd['weaponEnabled']}"
        # host applies + streams; both settle disabled
        ok = False
        for _ in range(40):
            _pump(host, client, 1)
            hd = _df_for(host, avenger_id, ufo_id)
            cd = _df_for(client, avenger_id, ufo_id)
            if hd and cd and hd["weaponEnabled"][0] is False and cd["weaponEnabled"][0] is False:
                ok = True
                break
            time.sleep(0.15)
        assert ok, (f"PRD-DF02 FAILED: weaponToggle did not disable weapon 0 on both "
                    f"(host={hd['weaponEnabled']} client={cd['weaponEnabled']})")
        print("PASS AC3: client weaponToggle disabled weapon 0 on host AND client "
              "(host fire gate reads _weaponEnabled)")

        # --- AC5: CLIENT minimize is view-only (host window stays full). -------------
        client.ok({"cmd": "dogfight_action", "action": "minimize"})
        _pump(host, client, 2)
        cd = _df_for(client, avenger_id, ufo_id)
        hd = _df_for(host, avenger_id, ufo_id)
        assert cd and cd["minimized"], "client view did not minimize"
        assert hd and not hd["minimized"], (
            "PRD-DF02 FAILED: host window minimized from a client-local minimize "
            "(minimize must be per-machine VIEW state)")
        print("PASS AC5: client minimize is local-only; host window stays full "
              "(world clock stays host-gated)")

        # --- AC6: CLIENT disengage -> the fight breaks off on BOTH. ------------------
        client.ok({"cmd": "dogfight_action", "action": "disengage"})
        host_diseng = client_diseng = False
        for _ in range(40):
            _pump(host, client, 1)
            hd = _df_for(host, avenger_id, ufo_id)
            cd = _df_for(client, avenger_id, ufo_id)
            if hd and hd["disengaging"]:
                host_diseng = True
            if cd and cd["disengaging"]:
                client_diseng = True
            # once the fight closes on both, the break-off is complete
            if host_diseng and client_diseng:
                break
            if hd is None and cd is None and host_diseng:
                client_diseng = True  # both windows already closed after break-off
                break
            time.sleep(0.15)
        assert host_diseng, "PRD-DF02 FAILED: host did not break off on client disengage"
        assert client_diseng, "PRD-DF02 FAILED: client did not reflect disengage"
        print("PASS AC6: client disengage broke the fight off on BOTH machines")

        js.finish()
        print("ALL JOINT DOGFIGHT CONTROL TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
