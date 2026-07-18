"""GAP-9: the replica's income/expenditure GRAPH series must match the host's.

A JOINT campaign is one host-authoritative world. Funds ride every joint_apply
(the packet carries the post-mutation getFunds()), so the CURRENT funds stay
exact and are checksum-verified. But the Graphs->Finance screen also plots the
12-month rolling income and expenditure vectors (SavedGame::_incomes /
_expenditures, .back() = the in-progress month), and those DRIFTED on the replica.

ROOT CAUSE (SavedGame::setFunds, SavedGame.cpp:~1964): setFunds infers income vs
expenditure from the NET delta -
    _funds.back() > funds ? _expenditures.back() += d : _incomes.back() += d
The host reaches a month total through MANY gross events; a running SELL-flagged
production is the canonical one - Production::step SELLS the finished unit (income
line 198) AND starts the next (expenditure, manufactureCost line 352) in the SAME
step, folded into ONE prod_done broadcast. The host books +sell income and +8000
expenditure (gross); the replica moved funds ONLY via setFunds(net) once, so it
booked the single NET direction. Funds stayed exact; the income/expenditure
decomposition drifted 8000+/event.

  MIX      run a SELL-flagged laser production and drive several host-side
           completions. Each completion: host sells (income) + starts the next
           unit (expenditure 8000), one prod_done carrying the net funds.

RED  (pre-fix): host incomeTail/expenditureTail != replica's (the folded gross
     vs net), while funds are equal.
GREEN (post-fix): the joint_apply carries the host's authoritative income/
     expenditure tails and the replica adopts them verbatim (setFundsRaw + copy),
     so all three (funds, income, expenditure) are equal on both, world-equality
     (which now compares the series) + zero-disk pass, and no spurious resync.

Run:  python tools/coop_test/test_joint_graphs.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo

LASER = "STR_LASER_PISTOL"          # manufacture: time 300, cost 8000, sells its output
LASER_TIME = 300
COMPLETIONS = 4                     # host-side sell+restart events to drive


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _prod(gc, item):
    for p in _base0(gc)["productions"]:
        if p["item"] == item:
            return p
    return None


def _tails(gc):
    g = _geo(gc)
    return g["funds"], g["incomeTail"], g["expenditureTail"]


def _resync_requests(gc):
    return gc.ok({"cmd": "joint_resync_stats"})["requests"]


def main():
    js = joint_fixture.bring_up("jgraph", (48770, 48771, 48070))
    host, client = js.host, js.client
    try:
        # Unlock the laser manufacture on BOTH machines (deterministic).
        for gc in (host, client):
            gc.ok({"cmd": "discover_research", "topic": LASER})

        # Start an INFINITE SELL-flagged laser production (client-originated -> the
        # applier sets it on host AND replica). Infinite so every completed unit both
        # sells (income) and starts the next (expenditure) -> the mixed fold.
        client.ok({"cmd": "joint_cmd", "jcmd": "man_start", "baseId": 0,
                   "payload": {"item": LASER, "engineers": 1, "qty": 1,
                               "infinite": True, "sell": True, "fallback": False}})
        for gc, tag in ((host, "host"), (client, "client")):
            gc.wait_for(f"{tag} sell production running",
                        lambda gc=gc: (_prod(gc, LASER) and _prod(gc, LASER)["sell"]) or None,
                        timeout=30, interval=0.5)
        assert _prod(host, LASER)["sell"] and _prod(client, LASER)["sell"], \
            "sell flag did not replicate to both productions"
        print(f"PASS setup: infinite SELL-flagged {LASER} running on both @ 1 engineer")

        # Reset resync counters: only a completion-driven drift may move anything now.
        host.ok({"cmd": "joint_reset_resync_stats"})
        client.ok({"cmd": "joint_reset_resync_stats"})

        # Drive COMPLETIONS host-side sell+restart events. Seed _timeSpent one short
        # on the host only (replica sim is frozen) and advance one hour; the host
        # completes exactly one unit, sells it, starts the next, broadcasts prod_done.
        for i in range(COMPLETIONS):
            r = host.ok({"cmd": "set_production_progress", "item": LASER,
                         "timeSpent": LASER_TIME - 1, "engineers": 1})
            assert r.get("found"), f"could not seed production progress on host: {r}"
            geo.skip_ingame_time(host, client, minutes=90, speed_idx=5, real_timeout=60)
            host.wait_for("host still running (infinite prod survived a completion)",
                          lambda: (_prod(host, LASER) is not None) or None,
                          timeout=20, interval=0.5)

        # Let the last prod_done land, then poll briefly for the series to converge
        # (an in-flight joint_apply is a legitimate transient skew - same reason the
        # world-equality helper polls). Pre-fix they NEVER converge (net-inference
        # drift); post-fix they land equal within a tick.
        deadline = time.time() + 12
        fh = ih = eh = fc = ic = ec = None
        while time.time() < deadline:
            fh, ih, eh = _tails(host)
            fc, ic, ec = _tails(client)
            if fh == fc and ih == ic and eh == ec:
                break
            time.sleep(1)
        print(f"HOST   funds={fh}  income={ih}  expenditure={eh}")
        print(f"CLIENT funds={fc}  income={ic}  expenditure={ec}")
        print(f"DELTA  funds={fh - fc}  income={ih - ic}  expenditure={eh - ec}")

        # Funds are the number in the checksum - they must be exact regardless.
        assert fh == fc, f"funds diverged (a real regression): host={fh} client={fc}"

        # GAP-9 GREEN: the graph series are host-authoritative and equal on both.
        assert ih == ic, \
            f"GAP-9 income series drift: host incomeTail={ih} client={ic} (delta {ih - ic})"
        assert eh == ec, \
            f"GAP-9 expenditure series drift: host expenditureTail={eh} client={ec} (delta {eh - ec})"
        print(f"PASS series: income {ih} and expenditure {eh} equal on host and replica")

        # No spurious resync: funds never mismatched, so the checksum never fired.
        assert _resync_requests(client) == 0, \
            f"the graph fix spuriously triggered {_resync_requests(client)} resync(s)"
        print("PASS no-resync: replica requested 0 world restreams")

        # PRD-J11 shared final-state assertions (world equality now INCLUDES the
        # income/expenditure series - the GAP-9 exclusion was removed) + zero-disk.
        js.finish()

        print("ALL JOINT GRAPH TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
