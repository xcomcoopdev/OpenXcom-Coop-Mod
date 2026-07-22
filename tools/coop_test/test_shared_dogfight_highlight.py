"""Playtest B6: shared-dogfight stance BUTTONS must auto-sync (highlight).

The stance buttons are an ImageButton radio group; the pressed look is a
persistent invert() that mousePress moves on a real click. A replica adopts the
host's stance through df_state (applyFrame), which bypassed mousePress and only
reassigned the _mode group pointer - so the highlight stayed on the old button:
two buttons looked lit, and the next real click double-inverted. The reported
"buttons not auto-synced; only synced when the other player presses, which
desyncs everything (multiple active)".

The df_state frame already carries the stance (modeIndex), so the _mode POINTER
synced (dogfight_state.mode) even before the fix - which is exactly why the bug
hid. This test reads the INVERTED button (dogfight_state.highlight): post-fix it
must equal .mode; pre-fix it stays stuck on the initial button.

  AUTOSYNC  host changes stance with the client just watching -> the client's
            highlighted button follows (highlight == mode == the new stance),
            i.e. exactly one button lit, no press required on the client.

Run:  python tools/coop_test/test_shared_dogfight_highlight.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import session
import geo

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
    for df in gc.ok({"cmd": "dogfight_state"}).get("dogfights", []):
        if df["craftId"] == craft_id and df["ufoId"] == ufo_id:
            return df
    return None


def _pump(host, client, n=1):
    geo.skip_realtime(host, client, n, speed_idx=0, stuck_timeout=None)


def _wait(gc, craft_id, ufo_id, pred, label, host, client, tries=40):
    last = None
    for _ in range(tries):
        df = _df_for(gc, craft_id, ufo_id)
        if df is not None:
            last = df
            if pred(df):
                return df
        _pump(host, client, 1)
        time.sleep(0.15)
    raise AssertionError(f"{label}: never satisfied (last {last})")


def _assert_synced_stance(client, craft_id, ufo_id, want, host):
    """The client's replica must show highlight == mode == want (one button lit)."""
    df = _wait(client, craft_id, ufo_id, lambda d: d["mode"] == want,
               f"client mode -> {want}", host, client)
    # mode (the _mode pointer) synced via df_state; the point of B6 is the HIGHLIGHT.
    df = _wait(client, craft_id, ufo_id, lambda d: d["highlight"] == want,
               f"client HIGHLIGHT -> {want}", host, client)
    assert df["mode"] == want and df["highlight"] == want, \
        f"stance {want}: mode={df['mode']} highlight={df['highlight']} (B6 desync)"


def main():
    js = shared_fixture.bring_up("jdfhl", (48810, 48811, 48110))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]

        sc_h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER", "weapon": "STR_CANNON_UC"})
        sc_c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER", "weapon": "STR_CANNON_UC"})
        assert sc_h["craft_id"] == sc_c["craft_id"], "avenger ids diverged"
        avenger_id = sc_h["craft_id"]

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
        client.ok({"cmd": "craft_order", "order": "target", "craft_id": avenger_id,
                   "craft_type": "STR_AVENGER", "ufo_id": ufo_id})

        deadline = time.time() + 120
        while time.time() < deadline:
            _pump(host, client, 1)
            if _df_for(host, avenger_id, ufo_id) and _df_for(client, avenger_id, ufo_id):
                break
            time.sleep(0.2)
        hd = _df_for(host, avenger_id, ufo_id)
        cd = _df_for(client, avenger_id, ufo_id)
        assert hd and cd, "the intercept did not open on BOTH machines"
        assert cd["replica"] and not hd["replica"], "replica/host role flags wrong"
        # baseline invariant already holds on both.
        assert cd["highlight"] == cd["mode"], \
            f"baseline highlight!=mode on client: {cd['highlight']} vs {cd['mode']}"
        print(f"PASS setup: both opened; client is a replica; baseline stance {cd['mode']} lit")

        # ---- AUTOSYNC: the HOST changes stance; the client only watches. -----
        for want, name in ((AGGRESSIVE, "aggressive"), (CAUTIOUS, "cautious"),
                           (STANDARD, "standard"), (STANDOFF, "standoff")):
            host.ok({"cmd": "dogfight_action", "action": name})
            _assert_synced_stance(client, avenger_id, ufo_id, want, host)
            print(f"PASS autosync: host -> {name}; client highlight followed (one button lit)")

        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive"
        print("ALL SHARED DOGFIGHT-HIGHLIGHT TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
