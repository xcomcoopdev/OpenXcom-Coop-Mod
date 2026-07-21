"""Playtest bug: the HOST dogfight window desyncs when a CLIENT clicks a stance
command - several stance buttons show lit at once.

DF02's control test asserts the scalar `mode` converges, but the reported bug is in the
BUTTON HIGHLIGHT (a persistent ImageButton invert()). A client stance arrives on the host
through hostApplyStance(), NOT through ImageButton::mousePress, so the old radio-group
highlight was never moved: the old button stayed lit AND, because the new _mode was left
un-inverted, the next real host click double-inverted - several buttons lit simultaneously.

This test drives that exact sequence and asserts the GROUND-TRUTH lit-button count
(dogfight_state.litStances = how many stance buttons are actually drawn inverted) stays
EXACTLY 1 on the host after every client- and host-issued stance change.

Run:  python tools/coop_test/test_shared_dogfight_button_desync.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
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
    raise AssertionError(f"{label}: condition never met (last {last})")


def _assert_one_lit(gc, craft_id, ufo_id, tag, host, client):
    """The host's radio group must have EXACTLY ONE stance button lit."""
    df = _wait(gc, craft_id, ufo_id, lambda d: d.get("litStances", -1) == 1,
               f"{tag}: exactly one stance lit", host, client)
    # And the lit button must be the active mode (highlight tracks _mode).
    assert df["highlight"] == df["mode"], \
        f"{tag}: highlight {df['highlight']} != mode {df['mode']} (radio desync)"
    return df


def main():
    js = shared_fixture.bring_up("jdfbtn", (48796, 48797, 48096))
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

        # CLIENT commands the intercept (commanding seat -> full window on both).
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
        assert _df_for(host, avenger_id, ufo_id) and _df_for(client, avenger_id, ufo_id), \
            "the intercept did not open on BOTH machines"
        # Baseline: fresh fight opens in STANDOFF with exactly one lit button on the host.
        _assert_one_lit(host, avenger_id, ufo_id, "open", host, client)
        print("PASS open: host opens with exactly one stance button lit")

        # --- The bug: a sequence of CLIENT stance commands. After EACH, the host must
        #     still show exactly one lit button (the old highlight must have moved). ----
        for want, name in [(AGGRESSIVE, "aggressive"), (CAUTIOUS, "cautious"),
                           (STANDARD, "standard"), (STANDOFF, "standoff")]:
            client.ok({"cmd": "dogfight_action", "action": name})
            _wait(host, avenger_id, ufo_id, lambda d: d["mode"] == want,
                  f"host mode -> {name}", host, client)
            df = _assert_one_lit(host, avenger_id, ufo_id, f"after client {name}", host, client)
            assert df["mode"] == want, f"host mode {df['mode']} != {want} ({name})"
            print(f"PASS client {name}: host shows exactly one lit button (mode {want})")

        # --- The compound case the bug amplified: a client command THEN a real host
        #     click. Pre-fix the un-inverted _mode double-inverted here -> 2+ lit. ------
        client.ok({"cmd": "dogfight_action", "action": "aggressive"})
        _wait(host, avenger_id, ufo_id, lambda d: d["mode"] == AGGRESSIVE,
              "host mode -> aggressive (compound)", host, client)
        host.ok({"cmd": "dogfight_action", "action": "cautious"})  # real host mousePress
        df = _assert_one_lit(host, avenger_id, ufo_id, "client-then-host click", host, client)
        assert df["mode"] == CAUTIOUS, f"host mode {df['mode']} != cautious after host click"
        print("PASS compound: client stance then host click -> still exactly one lit button")

        js.finish()
        print("ALL SHARED DOGFIGHT BUTTON-DESYNC TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
