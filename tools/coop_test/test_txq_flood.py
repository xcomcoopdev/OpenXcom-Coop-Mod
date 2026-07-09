"""Deterministic repro for the "TX queue full, dropping packet" / 19k-ping bug.

Player report (v1.8.4, TCP coop): two players start a campaign, place bases,
hit Ready; one player gets `TX queue full, dropping packet` and ping spikes to
~19,000 ms, sync stuck at "Requesting map data". See docs/handoff.md for the
full root-cause writeup.

Root cause (recap): GeoscapeState::think() serializes the whole geoscape to
JSON and enqueues it EVERY rendered frame while on the coop geoscape. At high
FPS that push rate outruns the single blocking send thread; the 1024-slot
g_txQ fills and enqueueTx() silently drops packets. On a constrained link
(the players used a VPN relay) the drain is slow enough that this happens the
moment both peers sit on the geoscape after Ready.

Why the dev couldn't repro: on fast LAN/localhost the send thread always
outruns any FPS, so the queue never fills; and with two windows on one machine
the unfocused one throttles to FPSInactive, killing its heartbeat rate.

This harness makes it deterministic on loopback with two instances by:
  * OXC_REPRO_TX_BPS         - pace the send thread to a slow-link byte/sec
                               rate (emulates the VPN relay; no clumsy/netem).
  * OXC_REPRO_FULLFPS_UNFOCUSED - keep the background window flooding at the
                               full FPS cap instead of FPSInactive.
  * a high fixed FPS in options.cfg so the per-frame heartbeat rate is high
    and constant.

It then brings up a fresh 2-player campaign (place bases + Ready via
bootstrap_fresh_session), sits on the geoscape, and polls the new coop_stats
TestServer command until txDropCount > 0 on either instance -> bug reproduced.

Run:  python tools/coop_test/test_txq_flood.py
Exit 0 + "REPRO CONFIRMED" on success; non-zero if it did not reproduce.
"""

import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
from test_bug_fixes import bootstrap_fresh_session

# --- repro knobs ---------------------------------------------------------
TX_BPS = int(os.environ.get("REPRO_TX_BPS", "16000"))   # emulated link ~16 KB/s
FPS = int(os.environ.get("REPRO_FPS", "120"))           # per-frame heartbeat rate
WATCH_SECS = int(os.environ.get("REPRO_WATCH_SECS", "60"))


def patch_options(user_dir):
    """Force the FPS / vsync / time-sync knobs the repro depends on."""
    cfg_path = os.path.join(user_dir, "options.cfg")
    with open(cfg_path, encoding="utf-8") as f:
        cfg = f.read()

    def setopt(key, val, text):
        nonlocal cfg
        new, n = re.subn(rf"(?m)^(\s*){key}: .*$", rf"\g<1>{key}: {val}", cfg)
        if n:
            cfg = new
        else:  # key absent -> append
            cfg = cfg.rstrip() + f"\n    {key}: {val}\n"

    setopt("FPS", FPS, "high fixed heartbeat rate")
    setopt("FPSInactive", FPS, "background window keeps flooding")
    setopt("vSyncForOpenGL", "false", "no vsync frame cap")
    setopt("EnableTimeSync", "true", "geoscape heartbeat ON (the flood source)")
    # NB: do NOT enable debugMode+logInfoToFile here. DebugLog() -> CrashHandler::log()
    # opens a fresh crash_<ts>.log PER message, so tens of thousands of drops would
    # spawn tens of thousands of files. The coop_stats.txDropCount counter is the
    # deterministic signal instead; the emitted string lives at connectionTCP.cpp:281.

    with open(cfg_path, "w", encoding="utf-8") as f:
        f.write(cfg)


def stats(gc):
    r = gc.cmd({"cmd": "coop_stats"})
    return r.get("txDropCount", 0), r.get("ping", "0")


def main():
    # env inherited by both spawned instances (harness spawn copies os.environ)
    os.environ["OXC_REPRO_TX_BPS"] = str(TX_BPS)
    os.environ["OXC_REPRO_FULLFPS_UNFOCUSED"] = "1"

    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    for gc in (host, client):
        patch_options(gc.user_dir)

    print(f"[cfg] TX_BPS={TX_BPS} B/s  FPS={FPS}  watch={WATCH_SECS}s")
    reproduced = False
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()

        # Place bases + Ready (the exact "start campaign -> Ready" flow). No
        # geoscape heartbeat flood yet (both sit in menus/lobby), so the initial
        # save sync still completes despite the throttle.
        bootstrap_fresh_session(host, client)
        print("[repro] both on coop geoscape; heartbeat flood now active")

        # Sit on the geoscape and watch the queue overflow.
        deadline = time.time() + WATCH_SECS
        while time.time() < deadline:
            hd, hp = stats(host)
            cd, cp = stats(client)
            print(f"  t+{WATCH_SECS-int(deadline-time.time()):02d}s "
                  f"host[drop={hd} ping={hp}ms]  client[drop={cd} ping={cp}ms]")
            if hd > 0 or cd > 0:
                reproduced = True
                who = "host" if hd > 0 else "client"
                drops = hd if hd > 0 else cd
                ping = hp if hd > 0 else cp
                print(f"\n*** REPRO CONFIRMED *** '{who}' logged "
                      f"'TX queue full, dropping packet' x{drops}, ping={ping}ms")
                break
            time.sleep(2)

        if not reproduced:
            print("\n[FAIL] no TX-queue drops in the watch window; "
                  "lower REPRO_TX_BPS or raise REPRO_FPS and retry")
    finally:
        host.shutdown(); client.shutdown()

    return 0 if reproduced else 1


if __name__ == "__main__":
    sys.exit(main())
