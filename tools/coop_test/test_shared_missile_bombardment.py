"""SHARED: a missile UFO bombarding the shared base must hit BOTH players' worlds.

This is the "partial destruction" path, and it is the only base-attack outcome with NO
battlescape: a UFO whose rules set `missilePower > 0` reaches an X-Com base, and
`Base::damageFacilities()` destroys/replaces facilities while the base SURVIVES.

It runs host-side only (the replica's geoscape sim is frozen) and the roll is RNG-driven
(WeightedOptions::choose), so the replica cannot re-derive it - the host broadcasts the
resulting layout as an absolute end-state (base_damaged). Before the fix, nothing was
broadcast at all (BaseDestroyedState::btnOkClick returns early for a partial destruction,
before it ever reaches the broadcast), so the client's copy of the ONE shared base silently
kept the facilities the host had lost.

`missilePower` defaults to 0 and NOTHING that ships with OXCE sets it, so the path is
unreachable on stock data. Rather than ship a mod just for this, the test GENERATES a
throwaway ruleset into the harness's isolated user dirs and activates it on both machines
(same ruleset on both, or they would diverge for a different reason).

Run:  python tools/coop_test/test_shared_missile_bombardment.py
"""

import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shared_fixture
import geo

# Small on purpose: damageFacility() spends sizeX*sizeY of missile power per facility, so
# a big number would flatten every non-lift facility in the base.
MISSILE_POWER = 2

METADATA = """\
name: "Coop missile-UFO test"
version: 1.0
description: "Test-only: gives the Battleship a missile payload so it bombards a base."
author: coop harness

master: xcom1
"""

RULESET = f"""\
ufos:
  - type: STR_BATTLESHIP
    missilePower: {MISSILE_POWER}
"""


def _make_mod(root):
    """Write the throwaway mod and return its folder path."""
    mod = os.path.join(root, "Coop_Missile_UFO_Test")
    os.makedirs(os.path.join(mod, "Ruleset"))
    with open(os.path.join(mod, "metadata.yml"), "w", encoding="utf-8") as f:
        f.write(METADATA)
    with open(os.path.join(mod, "Ruleset", "missile_ufo.rul"), "w", encoding="utf-8") as f:
        f.write(RULESET)
    return mod


def _base0(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base")


def _facilities(gc):
    return sorted((f["type"], f["x"], f["y"]) for f in _base0(gc)["facilities"])


def _wait(pred, label, timeout=45, interval=0.5):
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = pred()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError(f"{label}: never satisfied (last={last})")


def main():
    tmp = tempfile.mkdtemp(prefix="coop_missile_mod_")
    try:
        mod = _make_mod(tmp)
        js = shared_fixture.bring_up("jmis", (48960, 48961, 48260), mods=[mod])
        host, client = js.host, js.client
        try:
            before = _facilities(host)
            assert before == _facilities(client), "shared base diverged before the attack"
            assert before, "base has no facilities"
            print(f"PASS setup: mod active on both; shared base has {len(before)} "
                  f"facilities, identical on host and client")

            # A Battleship (now missile-armed) assaults the shared base. The starting base
            # has no defence facilities, so there is no turret sequence: the arrival
            # handler goes straight to handleBaseDefense -> the missile branch.
            r = host.ok({"cmd": "spawn_base_assault", "type": "STR_BATTLESHIP"})
            print(f"spawned missile Battleship {r['ufo_id']} against base {r['base']!r}")

            # Let the host's sim reach the arrival handler.
            got = geo.skip_realtime(host, client, 180, speed_idx=2,
                                    interest=geo.popup("BaseDestroyedState"),
                                    stuck_timeout=None)

            host_after = _wait(lambda: (_facilities(host) != before) and _facilities(host),
                               "host lost facilities to the bombardment", timeout=60)
            assert len(host_after) < len(before), \
                f"host facility count did not drop: {len(before)} -> {len(host_after)}"
            print(f"PASS bombardment: host lost {len(before) - len(host_after)} "
                  f"facilities ({len(before)} -> {len(host_after)}), base survived")

            # THE FIX: the replica must adopt the host's exact post-damage layout, and be
            # told with the same "damaged but survived" dialog. Watch for the dialog while
            # waiting for the layout - it can be raised and then covered/closed, and it is
            # not necessarily the TOP state, so scan the whole stack each poll.
            saw_dialog = []

            def _client_caught_up():
                states = client.cmd({"cmd": "get_state"}).get("states", [])
                if any("BaseDestroyedState" in s for s in states):
                    saw_dialog.append(True)
                return _facilities(client) == host_after

            _wait(_client_caught_up,
                  "client adopted the host's post-bombardment base layout", timeout=60)
            print("PASS replication: the client's copy of the SHARED base lost exactly "
                  "the same facilities")

            # The client also gets the "base under attack" alert, and GeoscapeState::popup()
            # only flushes its queue from think() - which does not run while a modal is up.
            # So the damage dialog sits QUEUED behind the alert, exactly as a real player
            # sees it: click through "under attack", then "base damaged". Click through.
            deadline = time.time() + 30
            while time.time() < deadline and not saw_dialog:
                states = client.cmd({"cmd": "get_state"}).get("states", [])
                if any("BaseDestroyedState" in s for s in states):
                    saw_dialog.append(True)
                    break
                if len(states) > 1:            # a dialog is up; dismiss it to reveal the next
                    client.cmd({"cmd": "dismiss_popup"})
                time.sleep(0.4)
            assert saw_dialog, (
                "client never showed the base-damaged dialog; client stack="
                f"{client.cmd({'cmd': 'get_state'}).get('states')} "
                f"host stack={host.cmd({'cmd': 'get_state'}).get('states')}")
            print("PASS alert: client shown the 'base damaged' dialog "
                  "(queued behind the under-attack alert, as a player would see it)")

            for gc in (host, client):
                geo.drain_popups(gc)
            # Facilities are compared field-by-field here, so this is the real proof.
            shared_fixture.assert_world_equal(host, client, "after missile bombardment")

            print("ALL SHARED MISSILE-BOMBARDMENT TESTS PASSED")
        finally:
            js.shutdown()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
