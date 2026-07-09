"""Validate the geoscape coop-sync fixes (throttle + thread-safe packet queue).

Reproduces the reported field bug ("place bases, hit ready, look at the
geoscape" -> client crash, "TX queue full, dropping packet", ping ~19k ms) in a
controlled 2-instance harness run, and measures the risk areas the throttle
could regress (time / funds / UFO-detection desync).

Signals collected (per run, tagged baseline vs fixed for A/B):
  * ping RTT on host+client   -> main-loop-stall proxy (the "19k ms" symptom).
                                 TestServer answers ping on the main thread, so
                                 if think() floods/serializes, RTT balloons.
  * instance liveness         -> the heap-corruption crash (dead socket / new
                                 crashlog file).
  * host vs client geo_state  -> desync: time / funds / ufo.detected must
                                 converge within one sync interval (<=~500ms),
                                 never drift unbounded.
  * openxcom.log / crashlogs  -> best-effort scrape for "TX queue full,
                                 dropping packet" and crash lines.

Run (once per build; point EXE via the worktree it lives in):
    python tools/coop_test/test_geoscape_sync.py --tag fixed    --idle 45 --advance 120
    python tools/coop_test/test_geoscape_sync.py --tag baseline --idle 45 --advance 120

Writes a JSON summary to  <TEMP>/oxc-coop-test/geosync-<tag>.json  and prints a
human summary. Exit code 0 = ran clean, 2 = a hard failure (crash/desync).
"""

import argparse
import ctypes
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir, TEST_ROOT, LAND_LON, LAND_LAT
from geo import (wait_both_ready, drain_popups, on_geoscape, top_state,
                 TimeWatchdog, game_minutes, StuckDialogError)

HOST_LON, HOST_LAT = 0.35, 0.85
GEO = "GeoscapeState"


def keep_awake():
    # A display sleep kills the SDL video context in the minimized game windows
    # -> std::terminate crash, which would masquerade as our bug. Hold it off.
    ES_CONTINUOUS = 0x80000000
    ES_SYSTEM_REQUIRED = 0x00000001
    ES_DISPLAY_REQUIRED = 0x00000002
    ctypes.windll.kernel32.SetThreadExecutionState(
        ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED)


def states(gc):
    return gc.cmd({"cmd": "get_state"})["states"]


def on_geo(gc):
    st = states(gc)
    return "GeoscapeState" in st[-1]


def bringup(host, client):
    """Full fresh-coop dance to a live geoscape with both bases placed + save
    synced. Lifted from test_transfer_fresh.py (the known-good path)."""
    host.spawn(); client.spawn()
    host.connect(timeout=240); client.connect(timeout=240)
    for gc in (host, client):
        gc.ok({"cmd": "set_option", "name": "HostSaveProgress", "value": True})

    host.ok({"cmd": "open_new_game"})
    host.wait_for("difficulty", lambda: any("NewGameState" in s for s in states(host)) or None)
    host.ok({"cmd": "newgame_ok"})
    host.wait_for("base placement", lambda: any("BuildNewBaseState" in s for s in states(host)) or None)
    r = host.cmd({"cmd": "place_first_base", "lon": HOST_LON, "lat": HOST_LAT, "name": "HostBase"})
    if not r.get("ok"):
        host.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "HostBase"})
    host.wait_for("host on geoscape", lambda: on_geo(host) and len(states(host)) == 1 or None)

    host.ok({"cmd": "host_tcp", "server": "TestSrv", "port": "47900", "player": "HostPlayer"})
    client.ok({"cmd": "join_tcp", "ip": "127.0.0.1", "port": "47900", "player": "ClientPlayer"})
    host.wait_for("client joined", lambda: host.cmd({"cmd": "get_coop"}).get("coopStatic") or None)

    host.wait_for("host profile", lambda: any("Profile" in s for s in states(host)) or None)
    host.ok({"cmd": "profile_ok"})
    client.wait_for("client profile", lambda: any("Profile" in s for s in states(client)) or None)
    client.ok({"cmd": "profile_ok"})

    client.wait_for("difficulty", lambda: any("NewGameState" in s for s in states(client)) or None)
    client.ok({"cmd": "newgame_ok"})
    client.wait_for("base placement", lambda: any("BuildNewBaseState" in s for s in states(client)) or None)
    client.ok({"cmd": "place_first_base", "lon": LAND_LON, "lat": LAND_LAT, "name": "ClientBase"})

    client.wait_for("client lobby", lambda: any("LobbyMenu" in s for s in states(client)) or None)
    client.ok({"cmd": "lobby_ready"})
    host.ok({"cmd": "lobby_ready"})
    client.wait_for("locked", lambda: client.cmd({"cmd": "get_coop"}).get("sessionLocked") or None, timeout=60)
    host.ok({"cmd": "lobby_ready"})
    client.ok({"cmd": "lobby_ready"})
    client.wait_for(
        "lobby closed + save synced",
        lambda: (lambda c: (c.get("lobbyClosed") and c.get("hasSave")) or None)(client.cmd({"cmd": "get_coop"})),
        timeout=120)


def ping_rtt_ms(gc):
    t0 = time.perf_counter()
    r = gc.cmd({"cmd": "ping"})
    dt = (time.perf_counter() - t0) * 1000.0
    return dt if r.get("pong") else None


def geo_snapshot(gc):
    r = gc.cmd({"cmd": "geo_state"})
    if not r.get("ok"):
        return None
    return {
        "sec": r["time"]["hour"] * 3600 + r["time"]["minute"] * 60,
        "day": r["time"]["day"], "month": r["time"]["month"], "year": r["time"]["year"],
        "funds": r["funds"],
        "ufos": {u["id"]: bool(u["detected"]) for u in r.get("ufos", [])},
        # Current membership keyed by the coop cross-instance id (_coop_ufo_id /
        # _coop_mission_id, shared host<->client), NOT the local getId() which
        # differs per instance. These are ALL savegame entities (detected or
        # not), so they measure existence sync rather than per-player radar
        # detection.
        "ufo_ids": {u.get("coopId", u["id"]) for u in r.get("ufos", [])},
        "site_ids": {s.get("coopId", s["id"]) for s in r.get("missionSites", [])},
    }


def abs_days(gc):
    """Absolute in-game day count (31-day months; monotonic enough for deltas)."""
    r = gc.cmd({"cmd": "geo_state"})
    if not r.get("ok") or "time" not in r:
        return None
    t = r["time"]
    return (t["year"] * 12 + t["month"]) * 31 + t["day"]


class Metrics:
    def __init__(self):
        self.rtt = {"host": [], "client": []}
        self.rtt_spikes = []          # (t, name, ms) over threshold
        self.desync_events = []       # (t, kind, detail)
        self.max_funds_gap = 0
        self.crashes = []             # (t, name, detail)
        self.time_advanced = {"host": False, "client": False}
        self._t_ufo_seen = {}         # ufo id -> t first detected on host

    def sample_ping(self, t, host, client):
        for gc in (host, client):
            try:
                ms = ping_rtt_ms(gc)
                if ms is not None:
                    self.rtt[gc.name].append(ms)
                    if ms > 1000:
                        self.rtt_spikes.append((round(t, 1), gc.name, round(ms)))
            except Exception as e:
                self.crashes.append((round(t, 1), gc.name, f"ping: {e!r}"))
                raise

    def sample_desync(self, t, hs, cs, start):
        if hs is None or cs is None:
            return
        # time advance (either player)
        if hs["sec"] != start["h"]["sec"] or hs["day"] != start["h"]["day"]:
            self.time_advanced["host"] = True
        if cs["sec"] != start["c"]["sec"] or cs["day"] != start["c"]["day"]:
            self.time_advanced["client"] = True
        # funds gap: informational only. Coop players have independent
        # economies (funds are never synced peer-to-peer), so a gap is expected
        # and does not count as a desync.
        gap = abs(hs["funds"] - cs["funds"])
        self.max_funds_gap = max(self.max_funds_gap, gap)
        # NB: UFO existence sync is checked in run_month by comparing coop-id
        # sets (shared state). We do NOT compare UFO *detection* here: detection
        # depends on each player's radar/base coverage, so host and client
        # legitimately detect different UFOs even when the UFOs are in sync.

    def summary(self):
        def stats(v):
            if not v:
                return None
            v2 = sorted(v)
            return {"n": len(v), "avg": round(statistics.mean(v), 1),
                    "p95": round(v2[int(len(v2) * 0.95)], 1), "max": round(max(v), 1)}
        return {
            "rtt_ms": {k: stats(v) for k, v in self.rtt.items()},
            "rtt_spikes_over_1000ms": self.rtt_spikes,
            "max_funds_gap": self.max_funds_gap,
            "desync_events": self.desync_events,
            "time_advanced": self.time_advanced,
            "crashes": self.crashes,
        }


def soak(host, client, metrics, seconds, label, start, advancing=False, speed_idx=None):
    print(f"  [{label}] soak {seconds}s ...")
    t_end = time.time() + seconds
    t0 = time.time()
    next_ping = 0.0
    next_geo = 0.0
    next_drive = 0.0
    while time.time() < t_end:
        now = time.time() - t0
        # liveness: a dead process = crash
        for gc in (host, client):
            if gc.proc.poll() is not None:
                metrics.crashes.append((round(now, 1), gc.name, f"process exited rc={gc.proc.returncode}"))
                raise RuntimeError(f"{gc.name} died during {label} (rc={gc.proc.returncode})")
        # keep time flowing while advancing: close any popup (UFO/event/monthly
        # report) that would otherwise pause the clock, and re-assert the speed
        # (a dismissed dialog can reset the coop speed selection).
        if advancing and now >= next_drive:
            for gc in (host, client):
                drain_popups(gc)
                if speed_idx is not None and on_geoscape(gc):
                    gc.cmd({"cmd": "geo_set_speed", "idx": speed_idx})
            next_drive = now + 1.0
        if now >= next_ping:
            metrics.sample_ping(now, host, client)
            next_ping = now + 0.25
        if now >= next_geo:
            try:
                hs, cs = geo_snapshot(host), geo_snapshot(client)
                metrics.sample_desync(now, hs, cs, start)
            except Exception as e:
                metrics.crashes.append((round(now, 1), "geo", f"{e!r}"))
                raise
            next_geo = now + 0.5
        time.sleep(0.02)


EXIST_LAG_TOL = 4.0  # seconds a UFO/site may legitimately lag between host<->client


def run_month(host, client, metrics, days, speed, real_timeout, start):
    """Advance `days` in-game days at time speed `speed` (5 = 1 day = fastest),
    auto-closing every dialog. Two cross-checks:
    - EXISTENCE sync: each poll compare the coop-id sets of all savegame UFOs /
      mission sites on both instances. An entity present on only one side for
      longer than EXIST_LAG_TOL seconds is a real desync (brief lag = normal
      sync latency). This measures shared geoscape state, NOT per-player radar
      detection (which legitimately differs by base coverage).
    - the end-of-month report (score + per-country funding), captured via the
      monthsPassed transition.
    Existence desyncs are appended to metrics.desync_events. Returns
    reports:{name:dict|None}."""
    clients = {"host": host, "client": client}
    reports = {"host": None, "client": None}
    diverged = {}   # (kind, coopId) -> real-time first seen on one side only
    flagged = set()
    print(f"  [month] advancing {days} in-game days at speed {speed} "
          f"(cap {real_timeout}s) ...")
    d0 = abs_days(host)
    t0 = time.time()
    real_end = t0 + real_timeout
    next_ping = 0.0
    # Kill + fail fast if the clock stalls on a dialog we can't clear, instead
    # of spinning until the real-time cap.
    wd = TimeWatchdog([host, client], timeout=25.0)
    while time.time() < real_end:
        now = time.time() - t0
        for name, gc in clients.items():
            if gc.proc.poll() is not None:
                metrics.crashes.append((round(now, 1), name, f"exited rc={gc.proc.returncode}"))
                raise RuntimeError(f"{name} died during month advance (rc={gc.proc.returncode})")
            # Capture the end-of-month report by the monthsPassed transition
            # (robust: a UFO dialog can stack on top of MonthlyReportState, so
            # keying on "MonthlyReportState is top" misses it). month_report reads
            # the SavedGame, valid whether or not the popup is showing.
            if reports[name] is None:
                r = gc.cmd({"cmd": "month_report"})
                if r.get("ok") and r["monthsPassed"] >= 1:
                    reports[name] = r
                    print(f"    [{name}] month-end report: monthsPassed={r['monthsPassed']} "
                          f"score={r['score']} funds={r['funds']}")
            drain_popups(gc)
            if on_geoscape(gc):
                gc.cmd({"cmd": "geo_set_speed", "idx": speed})
        # existence-sync check: compare current coop-id sets (all savegame UFOs
        # and mission sites), tolerating brief lag.
        hs, cs = geo_snapshot(host), geo_snapshot(client)
        if hs and cs:
            current = {}
            for kind, key in (("ufo", "ufo_ids"), ("site", "site_ids")):
                for cid in hs[key] - cs[key]:
                    current[(kind, cid)] = "host"
                for cid in cs[key] - hs[key]:
                    current[(kind, cid)] = "client"
            tnow = time.time()
            for k, side in current.items():
                diverged.setdefault(k, tnow)
                if tnow - diverged[k] > EXIST_LAG_TOL and k not in flagged:
                    flagged.add(k)
                    metrics.desync_events.append(
                        (round(now, 1), "entity_only_one_side",
                         f"{k[0]} coopId={k[1]} on {side} only for >{EXIST_LAG_TOL}s"))
            for k in [k for k in diverged if k not in current]:
                del diverged[k]   # converged (or gone from both)
        if now >= next_ping:
            metrics.sample_ping(now, host, client)
            next_ping = now + 0.25
        wd.tick(game_minutes(host))    # watchdog: stalls -> kill + StuckDialogError
        d = abs_days(host)
        if d is not None and d0 is not None and (d - d0) >= days:
            print(f"  [month] reached +{d - d0} in-game days in {round(now, 1)}s")
            return reports
        time.sleep(0.3)
    print(f"  [month] real-time cap hit after {round(time.time() - t0, 1)}s "
          f"(+{(abs_days(host) or d0) - d0} days)")
    return reports


def scan_logs(user_dirs, tag):
    hits = {"tx_queue_full": [], "crash_files": []}
    for name, d in user_dirs.items():
        log = os.path.join(d, "openxcom.log")
        if os.path.isfile(log):
            try:
                with open(log, encoding="utf-8", errors="replace") as f:
                    for ln in f:
                        if "TX queue full" in ln or "dropping packet" in ln:
                            hits["tx_queue_full"].append(f"{name}: {ln.strip()}")
            except OSError:
                pass
    # crashlogs live next to the exe
    from harness import EXE
    cl = os.path.join(os.path.dirname(EXE), "crashlogs")
    if os.path.isdir(cl):
        for fn in sorted(os.listdir(cl)):
            hits["crash_files"].append(fn)
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default="run")
    ap.add_argument("--idle", type=int, default=5, help="idle-geoscape soak seconds")
    ap.add_argument("--days", type=int, default=40, help="in-game days to advance in phase B")
    ap.add_argument("--speed", type=int, default=5, help="geo_set_speed idx (5=1day=fastest)")
    ap.add_argument("--realcap", type=int, default=300, help="wall-clock cap for the day advance")
    args = ap.parse_args()
    keep_awake()

    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    user_dirs = {"host": host.user_dir, "client": client.user_dir}
    metrics = Metrics()
    hard_fail = None
    reports = {"host": None, "client": None}
    score_mismatch = None
    funding_mismatch = []
    got_reports = False
    try:
        print(f"[{args.tag}] bringing up fresh coop session ...")
        bringup(host, client)
        # Gate on both players being settled on the geoscape (session live, no
        # CoopState WAIT / map-download dialog on top) before driving, so we
        # don't burn the soak sitting idle right after load.
        settle = wait_both_ready(host, client, timeout=60)
        print(f"[{args.tag}] both ready on geoscape after {settle}s")

        start = {"h": geo_snapshot(host), "c": geo_snapshot(client)}

        # Phase A: brief idle-geoscape baseline. The per-frame think() heartbeat
        # is FPS-bound (not game-speed bound) and the send thread outruns it on
        # loopback, so a few seconds is enough to sample RTT and confirm no
        # backlog forms; a longer idle only adds RTT samples.
        soak(host, client, metrics, args.idle, "idle", start)

        # Phase B: advance a full month-plus (default 40 in-game days) at the
        # fastest speed, closing every dialog. Cross-validate the shared outcome:
        # UFO/mission existence stays in sync (checked in run_month via coop-id
        # sets -> metrics.desync_events), and host+client agree on the end-of-
        # month report (score + per-country funding changes) even though each
        # player's absolute funds are independent.
        reports = run_month(host, client, metrics, args.days, args.speed,
                            args.realcap, start)

        # end-of-month report agreement (only if both captured one).
        got_reports = reports["host"] is not None and reports["client"] is not None
        if got_reports:
            hr, cr = reports["host"], reports["client"]
            if hr["score"] != cr["score"]:
                score_mismatch = {"host": hr["score"], "client": cr["score"]}
            hc = {c["name"]: c["fundingChange"] for c in hr["countries"]}
            cc = {c["name"]: c["fundingChange"] for c in cr["countries"]}
            for name in sorted(set(hc) | set(cc)):
                if hc.get(name) != cc.get(name):
                    funding_mismatch.append({"country": name,
                                             "host": hc.get(name), "client": cc.get(name)})

    except Exception as e:
        hard_fail = repr(e)
        print(f"[{args.tag}] HARD FAILURE: {hard_fail}")
    finally:
        logs = scan_logs(user_dirs, args.tag)
        host.shutdown(); client.shutdown()

    summ = metrics.summary()
    summ["tag"] = args.tag
    summ["hard_fail"] = hard_fail
    summ["logs"] = logs
    summ["month_report_captured"] = got_reports
    summ["score"] = {"host": reports["host"]["score"] if reports["host"] else None,
                     "client": reports["client"]["score"] if reports["client"] else None}
    summ["score_mismatch"] = score_mismatch
    summ["funding_mismatch"] = funding_mismatch
    out = os.path.join(TEST_ROOT, f"geosync-{args.tag}.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(summ, f, indent=2)

    print("\n===== SUMMARY (%s) =====" % args.tag)
    print(json.dumps(summ, indent=2))
    print("written:", out)

    # verdict
    # NB: funds are NOT synced in coop - each player runs an independent economy
    # (connectionTCP coopFunds = own funds, playersFunds = peer's, shown only in
    # the lobby; setFunds is never driven from a peer value). So a funds gap is
    # expected, not a desync. max_funds_gap stays in the summary as info only.
    crashed = bool(summ["crashes"]) or hard_fail is not None
    # desync = a UFO/mission that stayed present on only one side past the lag
    # tolerance (metrics.desync_events, set in run_month), OR host/client
    # disagree on the end-of-month report (score / per-country funding change),
    # OR no month report was captured (never reached the boundary).
    desynced = (bool(summ["desync_events"]) or score_mismatch is not None
                or bool(funding_mismatch) or not got_reports)
    tx_full = bool(logs["tx_queue_full"])
    print("\nverdict:",
          "CRASH " if crashed else "",
          "DESYNC " if desynced else "",
          "TXFULL " if tx_full else "",
          "(clean)" if not (crashed or desynced) else "")
    if desynced:
        print("  desync detail:",
              f"desync_events={summ['desync_events'][:5]}" if summ["desync_events"] else "",
              f"score_mismatch={score_mismatch}" if score_mismatch else "",
              f"funding_mismatch={funding_mismatch}" if funding_mismatch else "",
              "no_month_report" if not got_reports else "")
    sys.exit(2 if (crashed or desynced) else 0)


if __name__ == "__main__":
    main()
