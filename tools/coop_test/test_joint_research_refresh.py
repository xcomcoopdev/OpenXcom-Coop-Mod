"""Playtest B2: the "NEW RESEARCH PROJECTS" list must live-sync.

Two players must not both start the same project (the host validator rejects the
loser with STR_RESEARCH_NOT_AVAILABLE). Worse, a client that just started a topic
still saw it in the startable list - the list is built in the constructor / init
and never rebuilt under an open screen, so the started topic stayed clickable and
players re-started it into the error.

The fix binds NewResearchListState to the joint_apply stream: a res_start apply
(a peer's, or this player's own once it round-trips) rebuilds the list and the
now-unavailable topic disappears.

  OPEN   client opens the NewResearchListState; record the startable topics.
  START  host starts topic T (res_start joint_cmd -> apply on both).
  LIVE   T disappears from the client's still-open list (no exit/reopen), the
         screen is still a NewResearchListState (rebuilt, not crashed), and a
         SECOND start of T is impossible because it is no longer offered.

Run:  python tools/coop_test/test_joint_research_refresh.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session
import geo


def _screen(gc):
    return gc.ok({"cmd": "screen_state"})


def _base0(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _running(gc, topic):
    for r in _base0(gc)["research"]:
        if r["name"] == topic:
            return True
    return False


def main():
    js = joint_fixture.bring_up("jresref", (48770, 48771, 48070))
    host, client = js.host, js.client
    try:
        topics = host.ok({"cmd": "available_research"})["topics"]
        assert topics == client.ok({"cmd": "available_research"})["topics"], \
            "available research differs across machines"
        assert len(topics) >= 2, f"need >=2 startable topics, got {topics}"
        T = topics[0]
        print(f"PASS setup: {len(topics)} startable topics; targeting {T}")

        # ---- OPEN: client sits in the NEW RESEARCH PROJECTS list -------------
        r = client.ok({"cmd": "open_screen", "screen": "new_research"})
        assert r.get("ok"), f"client could not open new_research: {r}"
        s0 = _screen(client)
        assert s0["top"] == "new_research", f"client top is {s0['top']}, want new_research"
        assert T in s0["projects"], f"target {T} not offered in list: {s0['projects']}"
        print(f"PASS open: client list offers {len(s0['projects'])} topics incl. {T}")

        # ---- START: host starts T (res_start -> joint_apply on both) ---------
        r = host.ok({"cmd": "research_start", "topic": T, "scientists": 5})
        assert r.get("ok"), f"host research_start not accepted: {r}"

        # world settles first: T is a running project on both machines
        client.wait_for("client's shared world shows T running",
                        lambda: _running(client, T) or None,
                        timeout=30, interval=0.5)
        assert _running(host, T), "host does not show T running"

        # ---- LIVE: T must drop from the client's still-open list -------------
        def _dropped():
            s = _screen(client)
            if s["top"] != "new_research":
                return None
            if T in s["projects"]:
                return None
            return s

        client.wait_for("started topic T dropped from the open list live",
                        _dropped, timeout=30, interval=0.5)
        s1 = _screen(client)
        assert s1["top"] == "new_research", f"list gone/crashed: top={s1['top']}"
        assert T not in s1["projects"], \
            f"started topic {T} STILL offered in the open list (B2): {s1['projects']}"
        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive after refresh"
        print(f"PASS live: {T} dropped from the open list (now {len(s1['projects'])} topics), no crash")

        client.ok({"cmd": "close_screens"})
        print("ALL JOINT RESEARCH-REFRESH TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
