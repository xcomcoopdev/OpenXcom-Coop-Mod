"""Playtest B5: every player must get a new-UFO alert, not just the host.

A JOINT campaign runs ONE host-authoritative geoscape sim, so UFO detection only
ever happens on the host. The host broadcasts a reliable "ufo_popup" packet
(UfoDetectedState ctor) and the peer is supposed to raise its own alert for the
matching shared UFO. But the client-side matcher required ufo->getCoop()==true -
the SEPARATE "mirror of the peer's UFO" flag. A JOINT client's UFO is a
byte-faithful replica of the host's own (getCoop()==false), so the match failed
and the alert was swallowed on every non-host player.

The fix accepts the shared replica in JOINT.

  ALERT  a detected UFO exists on both machines; the host raises the alert
         (broadcasts ufo_popup) -> the CLIENT raises its own UfoDetectedState.

Run:  python tools/coop_test/test_joint_ufo_alert.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
from geo import top_state

UFO = dict(type="STR_MEDIUM_SCOUT", mission="STR_ALIEN_RESEARCH",
           region="STR_NORTH_AMERICA", race="STR_SECTOID",
           trajectory="P0", state="flying", speed=1, lon=0.4, lat=0.3)


def main():
    js = joint_fixture.bring_up("jufo", (48800, 48801, 48100))
    host, client = js.host, js.client
    try:
        # A detected UFO of the same race+type on BOTH machines (spawn_ufo mutates
        # the local world; the matcher keys on race+type+detected, not id).
        rh = host.ok({"cmd": "spawn_ufo", **UFO})
        rc = client.ok({"cmd": "spawn_ufo", **UFO})
        assert rh.get("ok") and rc.get("ok"), f"spawn failed: host={rh} client={rc}"
        print(f"PASS setup: detected {UFO['type']} ({UFO['race']}) on both machines")

        # The client must NOT already be showing a UFO alert (clean baseline).
        assert "UfoDetectedState" not in top_state(client), "client already alerting"

        # HOST detects it -> ufo_popup broadcast to the client.
        r = host.ok({"cmd": "ufo_alert"})
        assert r.get("ok"), f"host ufo_alert failed: {r}"
        print(f"PASS host-detect: broadcast ufo_popup type={r['type']} race={r['race']}")

        # ---- ALERT: the CLIENT raises its own UfoDetectedState --------------
        client.wait_for("client raises its own UFO alert",
                        lambda: ("UfoDetectedState" in top_state(client)) or None,
                        timeout=30, interval=0.5)
        assert "UfoDetectedState" in top_state(client), \
            f"client never raised the UFO alert (B5); top={top_state(client)}"
        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive"
        print("PASS alert: client raised its own UfoDetectedState (all players alerted)")

        # acknowledge + tidy up
        client.ok({"cmd": "dismiss_popup"})
        print("ALL JOINT UFO-ALERT TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
