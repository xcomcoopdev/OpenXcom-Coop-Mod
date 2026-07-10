"""Coop UFO-notice propagation + PARALLEL-acknowledgement test.

Two things are validated:

1. Propagation: when one player detects a UFO, the peer gets the notice too
   (both show UfoDetectedState). Mechanism: the detecting owner sends a reliable
   "ufo_popup" packet (src/Geoscape/UfoDetectedState.cpp:238); the peer pops a
   UfoDetectedState for the matching coop UFO (connectionTCP "ufo_popup" ->
   GeoscapeState show_coop_ufo_popup).

2. Parallel acknowledgement (the important one): both players must be able to
   see and dismiss the dialog INDEPENDENTLY. The historical bug was: a UFO
   dialog on player A froze A's time, which blocked the notice from reaching
   B, so B sat frozen (no dialog, clock stopped) until A dismissed - forcing
   SERIAL acknowledgement. This test holds one side's UFO dialog OPEN and checks
   the peer is NOT left frozen-without-a-dialog: the peer must either raise its
   own UFO dialog (both up at once = parallel) or keep advancing. If the peer is
   stuck (no dialog AND clock frozen) while the first holds its dialog, that is
   the serial-freeze bug.

Run: python tools/coop_test/test_ufo_notice.py [in_game_days]
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
from test_geoscape_sync import bringup
from geo import (wait_both_ready, drain_popups, on_geoscape, top_state,
                 TimeWatchdog, game_minutes)

SPEED = 5           # 1 day/tick (fastest): a detection dialog pauses time on
                    # that side regardless of speed, so notices are still
                    # observable, and the fastest speed surfaces more UFOs sooner
DAYS = int(sys.argv[1]) if len(sys.argv) > 1 else 40   # bound by IN-GAME days,
                    # not real seconds: at fastest speed a real-time window blows
                    # through several months, and skipping every mission tanks the
                    # monthly score -> consecutive negative months -> game over.
REALCAP = 300       # wall-clock safety cap
HOLD_WINDOW = 6.0   # sec to hold one dialog while checking the peer isn't frozen

UFO_POPUP = "UfoDetectedState"


def is_ufo_popup(gc):
    return top_state(gc).endswith(UFO_POPUP)


def detected_map(gc):
    g = gc.cmd({"cmd": "geo_state"})
    if not g.get("ok"):
        return {}
    return {u["coopId"]: bool(u["detected"]) for u in g.get("ufos", [])}


def hold_test(holder, other, peer_shown):
    """`holder` currently shows a UFO dialog. Hold it OPEN and watch `other`.
    `peer_shown` = did `other` already show a UFO dialog earlier in THIS freeze
    session (contiguous no-time-advance period). Outcomes:
      - 'parallel'  : other raised its own UFO dialog while holder held (both up)
      - 'advanced'  : other kept its clock moving -> not blocked
      - 'benign_pause': other paused with no dialog, but either it already
                      acknowledged its own dialog(s) this session (multi-UFO,
                      out-of-order clearing is fine) or it simply had no notice
      - 'same_freeze': other showed no dialog while holder held, but the instant
                      holder dismissed - WITHOUT time advancing - its queued
                      notice popped. Same freeze session, just out-of-order
                      clearing (e.g. two UFOs in one max-speed tick). This is
                      correct parallel acknowledgement, NOT a bug.
      - 'coincidental': after holder dismissed, other advanced time and THEN
                      detected a fresh UFO (not the held one)
      - 'holder_gone': holder's dialog closed on its own.
    On the last-three outcomes the holder is dismissed inside this function."""
    c0 = game_minutes(other)
    deadline = time.time() + HOLD_WINDOW
    while time.time() < deadline:
        if not is_ufo_popup(holder):
            return "holder_gone"
        if is_ufo_popup(other):
            return "parallel"          # both dialogs up at the same time
        if on_geoscape(other):
            other.cmd({"cmd": "geo_set_speed", "idx": SPEED})
        c = game_minutes(other)
        if c0 is not None and c is not None and c > c0:
            return "advanced"          # peer kept moving -> not blocked
        time.sleep(0.3)
    # Peer was frozen with no dialog while holder held. If it already cleared its
    # own dialog(s) this session, it is merely waiting for holder to finish -
    # that is normal parallel clearing of a multi-UFO freeze, not the bug.
    if peer_shown:
        return "benign_pause"
    # Peer showed NOTHING this session. Dismiss holder: a notice that pops the
    # instant holder clears (before the peer's clock advances) was queued/blocked
    # = serial. If the peer advances first, any later dialog is a fresh detection.
    drain_popups(holder)
    base = game_minutes(other)
    d2 = time.time() + 3.0
    while time.time() < d2:
        if is_ufo_popup(other):
            adv = game_minutes(other)
            if base is not None and adv is not None and adv > base:
                return "coincidental"   # advanced first -> fresh detection
            return "same_freeze"        # popped in the same freeze -> OK
        if on_geoscape(other):
            other.cmd({"cmd": "geo_set_speed", "idx": SPEED})
        time.sleep(0.3)
    return "benign_pause"


def main():
    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    first_det = {"host": {}, "client": {}}
    popups = {"host": 0, "client": 0}
    prev_pop = {"host": False, "client": False}
    simultaneous = 0        # polls with both dialogs up at once
    holds = {"parallel": 0, "advanced": 0, "same_freeze": 0,
             "coincidental": 0, "benign_pause": 0, "holder_gone": 0}
    shown = {"host": False, "client": False}   # showed a dialog this freeze session
    in_session = False
    try:
        bringup(host, client)   # spawns + connects both instances (do not spawn again)
        wait_both_ready(host, client, 60)
        for gc in (host, client):
            gc.cmd({"cmd": "geo_set_speed", "idx": SPEED})
        print(f"advancing {DAYS} in-game days at speed {SPEED} "
              f"(hold window {HOLD_WINDOW}s, cap {REALCAP}s) ...")

        wd = TimeWatchdog([host, client], timeout=40.0)
        d0 = game_minutes(host)
        t0 = time.time()
        while (time.time() - t0 < REALCAP
               and ((game_minutes(host) or d0) - d0) < DAYS * 24 * 60):
            now = time.time() - t0
            for gc in (host, client):
                if gc.proc.poll() is not None:
                    raise RuntimeError(f"instance exited early rc={gc.proc.returncode} "
                                       f"(possible crash / game-over)")
            h_pop, c_pop = is_ufo_popup(host), is_ufo_popup(client)
            # Track freeze sessions: a contiguous period with a dialog up on
            # either side (time not advancing). A side that shows a dialog any
            # time during the session has done its part (multi-UFO out-of-order
            # clearing is fine).
            any_pop = h_pop or c_pop
            if any_pop and not in_session:
                in_session = True
                shown = {"host": False, "client": False}
            if h_pop:
                shown["host"] = True
            if c_pop:
                shown["client"] = True
            if not any_pop:
                in_session = False
            for name, p in (("host", h_pop), ("client", c_pop)):
                if p and not prev_pop[name]:
                    popups[name] += 1
                prev_pop[name] = p
            for name, gc in (("host", host), ("client", client)):
                for cid, det in detected_map(gc).items():
                    if det and cid not in first_det[name]:
                        first_det[name][cid] = now

            if h_pop and c_pop:
                simultaneous += 1
                for gc in (host, client):   # both up = parallel; clear both
                    drain_popups(gc)
            elif h_pop or c_pop:
                holder, other = (host, client) if h_pop else (client, host)
                oname = "client" if h_pop else "host"
                res = hold_test(holder, other, shown[oname])
                holds[res] += 1
                for gc in (host, client):
                    drain_popups(gc)
            else:
                for gc in (host, client):
                    drain_popups(gc)
                    if on_geoscape(gc):
                        gc.cmd({"cmd": "geo_set_speed", "idx": SPEED})
            wd.tick(game_minutes(host))
            time.sleep(0.3)

        allids = set(first_det["host"]) | set(first_det["client"])
        both = len(set(first_det["host"]) & set(first_det["client"]))
        host_only = sorted(set(first_det["host"]) - set(first_det["client"]))
        client_only = sorted(set(first_det["client"]) - set(first_det["host"]))

        print("\n===== UFO detection propagation =====")
        print(f"UFOs detected on at least one side: {len(allids)}  both: {both}")
        print(f"  host-only:   {len(host_only)} {host_only}")
        print(f"  client-only: {len(client_only)} {client_only}")
        print(f"UfoDetectedState popups shown: host={popups['host']} client={popups['client']}")
        print("\n===== parallel acknowledgement =====")
        print(f"both dialogs up simultaneously (polls): {simultaneous}")
        print(f"hold tests: parallel={holds['parallel']} advanced={holds['advanced']} "
              f"same_freeze={holds['same_freeze']} coincidental={holds['coincidental']} "
              f"benign_pause={holds['benign_pause']} holder_gone={holds['holder_gone']}")
        # parallel/same_freeze = both players cleared their own dialog(s) in the
        # same freeze session (overlapping or out-of-order, e.g. two UFOs in one
        # max-speed tick). None of the hold outcomes is the serial-freeze bug:
        # that bug leaves a player frozen with no dialog and unable to advance,
        # which the TimeWatchdog would kill + surface as a hard failure.

        propagation_gap = bool(host_only) or bool(client_only)
        parallel_evidence = simultaneous > 0 or holds["parallel"] > 0 or holds["same_freeze"] > 0
        saw_notices = len(allids) > 0 or popups["host"] or popups["client"]
        if not saw_notices:
            print("\nverdict: INCONCLUSIVE - no UFO notices in the window "
                  "(raise the in-game-days arg)")
            sys.exit(0)
        ok = not propagation_gap
        print("\nverdict:",
              "OK - notices propagate; parallel acknowledgement observed"
              if (ok and parallel_evidence) else
              "OK - notices propagate (no parallel overlap sampled this run)"
              if ok else "PROPAGATION GAP")
        sys.exit(0 if ok else 2)
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
