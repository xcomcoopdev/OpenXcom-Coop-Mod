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
    }


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
        # funds gap (snapshots are last-write-wins; a transient gap is fine,
        # a large persistent one is desync)
        gap = abs(hs["funds"] - cs["funds"])
        self.max_funds_gap = max(self.max_funds_gap, gap)
        # ufo detection: once host reports a ufo detected, client should agree
        # within the sync window. Record a lag > 1.5s as a desync event.
        for uid, det in hs["ufos"].items():
            if det and uid not in self._t_ufo_seen:
                self._t_ufo_seen[uid] = t
            if det and uid in self._t_ufo_seen:
                if not cs["ufos"].get(uid, False) and (t - self._t_ufo_seen[uid]) > 1.5:
                    self.desync_events.append((round(t, 1), "ufo_detect_lag",
                                               f"ufo {uid} detected on host, not client after "
                                               f"{round(t - self._t_ufo_seen[uid], 1)}s"))

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


def soak(host, client, metrics, seconds, label, start, advancing=False):
    print(f"  [{label}] soak {seconds}s ...")
    t_end = time.time() + seconds
    t0 = time.time()
    next_ping = 0.0
    next_geo = 0.0
    while time.time() < t_end:
        now = time.time() - t0
        # liveness: a dead process = crash
        for gc in (host, client):
            if gc.proc.poll() is not None:
                metrics.crashes.append((round(now, 1), gc.name, f"process exited rc={gc.proc.returncode}"))
                raise RuntimeError(f"{gc.name} died during {label} (rc={gc.proc.returncode})")
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
    ap.add_argument("--idle", type=int, default=45, help="idle-geoscape soak seconds")
    ap.add_argument("--advance", type=int, default=120, help="time-advancing soak seconds")
    ap.add_argument("--speed", type=int, default=2, help="geo_set_speed idx (2=5min)")
    args = ap.parse_args()
    keep_awake()

    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    user_dirs = {"host": host.user_dir, "client": client.user_dir}
    metrics = Metrics()
    hard_fail = None
    try:
        print(f"[{args.tag}] bringing up fresh coop session ...")
        bringup(host, client)
        assert on_geo(host) and on_geo(client), "both must be on geoscape"
        print(f"[{args.tag}] session live on geoscape")

        start = {"h": geo_snapshot(host), "c": geo_snapshot(client)}

        # Phase A: idle geoscape (flood from per-frame think() with static state)
        soak(host, client, metrics, args.idle, "idle", start)

        # Phase B: advance time in lockstep (state actually changes each tick ->
        # target_positions differs every send -> hardest flood + real sync work)
        for gc in (host, client):
            if on_geo(gc):
                gc.cmd({"cmd": "geo_set_speed", "idx": args.speed})
        soak(host, client, metrics, args.advance, "advance", start, advancing=True)

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
    out = os.path.join(TEST_ROOT, f"geosync-{args.tag}.json")
    with open(out, "w", encoding="utf-8") as f:
        json.dump(summ, f, indent=2)

    print("\n===== SUMMARY (%s) =====" % args.tag)
    print(json.dumps(summ, indent=2))
    print("written:", out)

    # verdict
    crashed = bool(summ["crashes"]) or hard_fail is not None
    desynced = bool(summ["desync_events"]) or summ["max_funds_gap"] > 100000
    tx_full = bool(logs["tx_queue_full"])
    print("\nverdict:",
          "CRASH " if crashed else "",
          "DESYNC " if desynced else "",
          "TXFULL " if tx_full else "",
          "(clean)" if not (crashed or desynced) else "")
    sys.exit(2 if (crashed or desynced) else 0)


if __name__ == "__main__":
    main()
