"""Reusable geoscape driving helpers for coop tests.

Built on the TestServer commands get_state, get_coop, geo_state, geo_set_speed
and dismiss_popup. Every test (present or future) can import these to:

  * wait_both_ready(...)   - gate until both players are settled on the geoscape
                             (kills the post-load idle window before driving).
  * skip_realtime(...)     - let N real seconds pass while advancing time and
                             auto-closing any dialogs; optionally stop early on
                             an event of interest.
  * skip_ingame_time(...)  - same, but bounded by in-game minutes instead of
                             wall-clock.

The two skip helpers take an optional `interest` predicate so they stop as soon
as something you care about happens (e.g. a mission site appears) instead of
blindly skipping everything. Build one with popup(...) or geo_when(...), or pass
any callable(GameClient) -> truthy|None.
"""

import time

_GEO = "GeoscapeState"


class StuckDialogError(RuntimeError):
    """Raised when an advance stalls: in-game time stops moving because a dialog
    can't be cleared (or re-pushes itself every frame). Carries the top state on
    each side so the offending dialog can be given explicit handling."""


class TimeWatchdog:
    """Guards a time-advance loop. Call tick(clock) every iteration with a
    monotonic in-game clock value; if it doesn't change for `timeout` real
    seconds, the advance is stuck. On stall it KILLS the given clients (so no
    hung game windows linger) and raises StuckDialogError with diagnostics."""

    def __init__(self, clients, timeout=20.0):
        self.clients = list(clients)
        self.timeout = timeout
        self._last = object()      # sentinel != any real clock value
        self._changed_at = time.time()

    def tick(self, clock):
        now = time.time()
        if clock != self._last:
            self._last = clock
            self._changed_at = now
            return
        if now - self._changed_at > self.timeout:
            diag = {gc.name: top_state(gc) for gc in self.clients}
            for gc in self.clients:
                try:
                    gc.proc.kill()
                except Exception:
                    pass
            raise StuckDialogError(
                f"in-game time stalled >{self.timeout}s at clock={clock!r}; "
                f"killed sessions. Top states: {diag} - add explicit dismiss_popup "
                f"handling for whichever is not a GeoscapeState/WAIT dialog.")


# --- state introspection ---------------------------------------------------

def top_state(gc):
    """Typeid name of the active (top) state, or '' if the stack is empty."""
    st = gc.cmd({"cmd": "get_state"}).get("states", [])
    return st[-1] if st else ""


def on_geoscape(gc):
    """True if the geoscape is the active state (no modal popup on top)."""
    return top_state(gc).endswith(_GEO)


def both_ready(host, client):
    """True if BOTH instances are on a live coop geoscape with no popup on top."""
    for gc in (host, client):
        co = gc.cmd({"cmd": "get_coop"})
        if not (co.get("coopStatic") and co.get("lobbyClosed") and co.get("hasSave")):
            return False
        if not on_geoscape(gc):
            return False
    return True


# --- interest predicates ---------------------------------------------------
# An `interest` is any callable(GameClient) -> truthy|None. It returns a marker
# (anything truthy) when the thing you're watching for has happened, else None.

def popup(*typeid_substrs):
    """Interest predicate: fires when the top state's typeid contains any of the
    given substrings, e.g. popup('MissionDetectedState', 'GeoscapeEventState')."""
    def _p(gc):
        t = top_state(gc)
        return t if any(s in t for s in typeid_substrs) else None
    return _p


def geo_when(pred):
    """Interest predicate over the geo_state snapshot: fires when pred(geo)
    is truthy, e.g. geo_when(lambda g: any(u['detected'] for u in g['ufos']))."""
    def _p(gc):
        g = gc.cmd({"cmd": "geo_state"})
        if not g.get("ok"):
            return None
        return pred(g) or None
    return _p


# --- dialog draining -------------------------------------------------------

def drain_popups(gc, interest=None, limit=25):
    """Close every dismissable popup stacked above the geoscape on `gc` (via the
    dismiss_popup command, which knows Geoscape/Event/MonthlyReport/Mission/
    NextTurn/Abort/Debriefing dialogs). Stops early and returns a hit if
    `interest` fires (the interesting popup is left OPEN). Popups dismiss_popup
    can't close (e.g. a CoopState WAIT dialog, which auto-closes on its own) end
    the drain with no hit so the caller just waits and retries.

    Returns (dismissed: list[str], hit).
    """
    dismissed = []
    for _ in range(limit):
        if interest is not None:
            h = interest(gc)
            if h:
                return dismissed, h
        top = top_state(gc)
        if not top or top.endswith(_GEO):
            return dismissed, None
        r = gc.cmd({"cmd": "dismiss_popup"})
        if not r.get("ok"):
            return dismissed, None  # undismissable (WAIT dialog / unknown) - wait
        dismissed.append(r.get("handled", r.get("type", top)))
    return dismissed, None


# --- readiness gate --------------------------------------------------------

def wait_both_ready(host, client, timeout=90, interval=0.5):
    """Block until both players are settled on the coop geoscape (session live,
    no CoopState WAIT / map-download dialog on top). Call this right after the
    session comes up so a test starts driving immediately instead of sitting
    idle. Returns elapsed seconds; raises TimeoutError with the blocking state."""
    t0 = time.time()
    deadline = t0 + timeout
    while time.time() < deadline:
        if both_ready(host, client):
            return round(time.time() - t0, 1)
        time.sleep(interval)
    blocking = {gc.name: top_state(gc) for gc in (host, client)}
    raise TimeoutError(f"players not ready on geoscape after {timeout}s: {blocking}")


# --- time skipping ---------------------------------------------------------

def _apply_speed(host, client, speed_idx):
    # Coop only advances fast when BOTH players pick the SAME speed, so (re)apply
    # it to both every step (a dismissed dialog can reset the selection).
    for gc in (host, client):
        if on_geoscape(gc):
            gc.cmd({"cmd": "geo_set_speed", "idx": speed_idx})


def _step_dialogs(host, client, interest, do_dismiss, dismissed):
    """One pass of interest-check + popup drain across both instances. Returns
    the (name, marker) hit or None."""
    for gc in (host, client):
        if interest is not None:
            h = interest(gc)
            if h:
                return (gc.name, h)
        if do_dismiss:
            d, h = drain_popups(gc, interest)
            dismissed[gc.name] += d
            if h:
                return (gc.name, h)
    return None


def _abs_minutes(ts):
    # Monotonic-enough minute counter for deltas. Uses 31-day months, so a delta
    # that crosses a month boundary is approximate; within a month it is exact.
    return ((((ts["year"] * 12 + ts["month"]) * 31 + ts["day"]) * 24
             + ts["hour"]) * 60 + ts["minute"])


def game_minutes(gc):
    """The instance's current in-game clock as an absolute minute count, or None."""
    g = gc.cmd({"cmd": "geo_state"})
    if not g.get("ok") or "time" not in g:
        return None
    return _abs_minutes(g["time"])


def skip_realtime(host, client, seconds, speed_idx=5, interest=None,
                  dismiss=True, poll=0.5, stuck_timeout=20.0):
    """Let `seconds` of real time pass while both instances advance on the
    geoscape at time speed `speed_idx` (0=5s .. 5=1day; default 5) and any
    dialogs that pop are auto-closed. If `interest` is given, stop the moment it
    fires on either side (leaving the triggering popup open) and return.

    A watchdog kills the sessions and raises StuckDialogError if the host clock
    stalls for `stuck_timeout` real seconds (a dialog that can't be cleared);
    pass stuck_timeout=None to disable.

    Returns {'elapsed', 'dismissed': {name: [...]}, 'hit': (name, marker)|None}.
    """
    dismissed = {host.name: [], client.name: []}
    hit = None
    wd = TimeWatchdog([host, client], stuck_timeout) if stuck_timeout else None
    t0 = time.time()
    t_end = t0 + seconds
    while time.time() < t_end:
        hit = _step_dialogs(host, client, interest, dismiss, dismissed)
        if hit:
            break
        _apply_speed(host, client, speed_idx)
        if wd:
            wd.tick(game_minutes(host))
        time.sleep(poll)
    return {"elapsed": round(time.time() - t0, 1), "dismissed": dismissed, "hit": hit}


def skip_ingame_time(host, client, minutes, speed_idx=5, interest=None,
                     dismiss=True, poll=0.5, real_timeout=None, stuck_timeout=20.0):
    """Advance until the host geoscape clock moves forward `minutes` in-game
    minutes (or `interest` fires), auto-closing dialogs on both. Same speed/coop
    rules as skip_realtime. `real_timeout` bounds wall-clock seconds (default
    max(30, minutes*3)) as a safety net; a watchdog kills the sessions and raises
    StuckDialogError if the clock stalls for `stuck_timeout` real seconds.

    Returns {'game_minutes', 'target', 'dismissed', 'hit', 'timed_out'}.
    """
    dismissed = {host.name: [], client.name: []}
    hit = None
    wd = TimeWatchdog([host, client], stuck_timeout) if stuck_timeout else None
    start = _abs_minutes(host.cmd({"cmd": "geo_state"})["time"])
    now = start
    target = start + minutes
    real_deadline = time.time() + (real_timeout if real_timeout else max(30, minutes * 3))
    while now < target and time.time() < real_deadline:
        hit = _step_dialogs(host, client, interest, dismiss, dismissed)
        if hit:
            break
        _apply_speed(host, client, speed_idx)
        g = host.cmd({"cmd": "geo_state"})
        if g.get("ok") and "time" in g:
            now = _abs_minutes(g["time"])
        if wd:
            wd.tick(now)
        time.sleep(poll)
    return {"game_minutes": now - start, "target": minutes, "dismissed": dismissed,
            "hit": hit, "timed_out": (now < target and hit is None)}
