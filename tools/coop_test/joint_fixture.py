"""PRD-J11: shared bring-up + world-equality assertion for every JOINT test.

Two things every JOINT test needs, in one place:

  bring_up(tag, ports)          - "host creates a JOINT campaign, client joins,
                                  world streamed, both settled on the geoscape".
                                  Returns a JointSession (host/client/dirs).
  assert_world_equal(h, c, tag) - the JOINT promise, asserted: ONE shared world.
                                  Deep-compares an introspection dump of both
                                  machines, modulo the known-volatile fields
                                  below. Wired into every JOINT test's final
                                  state, so any future test gets it for free.

Typical use (matches the existing suite's flat try/finally shape):

    js = joint_fixture.bring_up("jbuy", (48670, 48671, 47970))
    host, client = js.host, js.client
    try:
        ...
        js.finish()          # world equality + zero-disk + a PASS line
    finally:
        js.shutdown()

WHAT EQUALITY COVERS
--------------------
The JOINT guarantee is one host-authoritative ECONOMY world: funds, bases,
facilities, stores, transfers, research, manufacture, crafts and the soldier
roster (with ownership). All of that is compared exactly, base list IN INDEX
ORDER - the base index is the protocol's routing key (PRD-J03), so an ordering
difference is itself a desync.

Note this is deliberately WIDER than the PRD-J04/J10 world checksum, which is
only funds + base count + discovered-tech count. Stores/roster/transfer drift is
invisible to the auto-repair; it is visible here. That is the point of the
helper.

KNOWN-VOLATILE (excluded, with the reason - each is a real, documented property
of the design, not a fudge):

  * clock + monthly tails       - the replica's clock is host-driven and lands a
                                  tick later; tails are asserted by test_joint_sim.
  * craft kinematics            - lon/lat/fuel/damage/lowFuel/inDogfight ride the
                                  position snapshot (async, lossy by design), and
                                  a craft's _dest label lags host-side AUTO-returns
                                  (PRD-J08 known gap). Craft IDENTITY and STATUS
                                  are compared.
  * transfer `hours`            - a replica's sim is frozen (PRD-J04), so its
                                  Transfer objects never advance(); delivery is
                                  forced by the transfer_arrived broadcast. The
                                  transfer's existence/kind/qty IS compared.
  * research `spent`,           - progress rides day_tick, which is daily-granular
    production `timeSpent`,       by design (PRD-J06); it drifts within a day and
    production `produced`         re-converges at the tick.
  * ufos + missionSites         - snapshot-replicated best-effort: replica despawn
                                  is hide-not-delete and secondsRemaining is forced
                                  to a sentinel (PRD-J04/J08 known limits). Shared
                                  UFO/site visibility is asserted by test_joint_sim
                                  and test_joint_craft directly.

Equality is EVENTUAL: assert_world_equal polls, because a joint_apply in flight
is a legitimate transient skew (the same reason PRD-J10's desync detector
debounces for 3s).
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo


# ---- the world dump -------------------------------------------------------

def world_dump(gc):
    """The comparable, volatility-normalised world of one machine.

    Sourced from geo_state (world + per-base economy) + get_soldiers (roster with
    ownerPlayerId). Bases stay in INDEX order on purpose - the index is the JOINT
    command routing key, so a reordering must fail the compare.
    """
    g = gc.ok({"cmd": "geo_state"})
    roster = gc.ok({"cmd": "get_soldiers"})["bases"]

    bases = []
    for i, b in enumerate(g["bases"]):
        soldiers = roster[i]["soldiers"] if i < len(roster) else []
        bases.append({
            "name": b["name"],
            "lon": b["lon"],
            "lat": b["lat"],
            "coopBaseId": b["coopBaseId"],
            "coopBase": b["coopBase"],
            "coopIcon": b["coopIcon"],
            "freeScientists": b["freeScientists"],
            "freeEngineers": b["freeEngineers"],
            "freeLab": b["freeLab"],
            "freeWorkshop": b["freeWorkshop"],
            "items": dict(sorted(b["items"].items())),
            "facilities": sorted(
                (f["type"], f["x"], f["y"], f["buildTime"]) for f in b["facilities"]),
            # `hours` excluded: a frozen replica never advance()s a Transfer.
            "transfers": sorted(
                (t["type"], t["rule"], t["qty"]) for t in b["transfers"]),
            # kinematics excluded (snapshot-driven); identity + status compared.
            "crafts": sorted(
                (c["id"], c["type"], c["status"], c["weapons"]) for c in b["crafts"]),
            # `spent` excluded: progress is daily-granular via day_tick.
            "research": sorted(
                (r["name"], r["assigned"], r["cost"]) for r in b["research"]),
            # `timeSpent`/`produced` excluded: same reason.
            "productions": sorted(
                (p["item"], p["engineers"], p["amount"], p["infinite"], p["sell"])
                for p in b["productions"]),
            "soldiers": sorted(
                (s["id"], s["name"], s["owner"], s["craftId"], s["dead"])
                for s in soldiers),
        })

    return {
        "funds": g["funds"],
        "campaignType": g["campaignType"],
        "monthsPassed": g["monthsPassed"],
        "discoveredResearch": sorted(g["discoveredResearch"]),
        "bases": bases,
    }


def _diff(a, b, path=""):
    """Readable deep diff: a list of 'path: host_value != client_value'."""
    out = []
    if isinstance(a, dict) and isinstance(b, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a:
                out.append(f"{path}.{k}: <absent on host> != {b[k]!r}")
            elif k not in b:
                out.append(f"{path}.{k}: {a[k]!r} != <absent on client>")
            else:
                out.extend(_diff(a[k], b[k], f"{path}.{k}"))
    elif isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append(f"{path}: {len(a)} entries != {len(b)} entries "
                       f"(host={a!r} client={b!r})")
        else:
            for i, (x, y) in enumerate(zip(a, b)):
                out.extend(_diff(x, y, f"{path}[{i}]"))
    elif a != b:
        out.append(f"{path}: {a!r} != {b!r}")
    return out


def world_diff(host, client):
    """[] when the two machines hold the same shared world (see module docstring
    for what is compared). Otherwise the list of differing field paths."""
    return _diff(world_dump(host), world_dump(client))


def world_equal(host, client):
    return not world_diff(host, client)


def assert_world_equal(host, client, tag="", timeout=45, interval=1.0):
    """THE JOINT invariant: host and replica hold one identical shared world.

    Polls until equal (an in-flight joint_apply is a legitimate transient skew -
    the same reason the J10 desync detector debounces) and raises with the field
    paths that never converged.
    """
    label = f"world equality [{tag}]" if tag else "world equality"
    deadline = time.time() + timeout
    diff = None
    while time.time() < deadline:
        diff = world_diff(host, client)
        if not diff:
            print(f"PASS {label}: host and client hold ONE identical shared world")
            return True
        time.sleep(interval)
    shown = "\n  ".join(diff[:25])
    more = f"\n  ... and {len(diff) - 25} more" if len(diff) > 25 else ""
    raise AssertionError(
        f"{label}: worlds DIVERGED after {timeout}s ({len(diff)} field(s)):\n"
        f"  {shown}{more}")


# ---- bring-up -------------------------------------------------------------

class JointSession:
    """A live JOINT campaign: host + client, world streamed, both on geoscape."""

    def __init__(self, tag, ports):
        self.tag = tag
        self.host_port, self.client_port, self.coop_port = ports
        self.host_dir = make_user_dir(f"{tag}_host")
        self.client_dir = make_user_dir(f"{tag}_client")
        self.host = GameClient("host", self.host_port, self.host_dir)
        self.client = GameClient("client", self.client_port, self.client_dir)

    def _start(self, wait_ready, host_base, client_base):
        self.host.spawn()
        self.client.spawn()
        self.host.connect()
        self.client.connect()
        session.new_campaign(self.host, self.client, port=str(self.coop_port),
                             campaign_mode="joint",
                             host_base=host_base, client_base=client_base)
        if wait_ready:
            geo.wait_both_ready(self.host, self.client)

    def assert_world_equal(self, tag="", timeout=45):
        return assert_world_equal(self.host, self.client, tag or self.tag, timeout)

    def finish(self, timeout=45):
        """Standard JOINT end-of-test assertions: the two worlds are one, and the
        replica never touched the disk. Call at the end of every JOINT test."""
        self.assert_world_equal("final state", timeout)
        session.assert_client_zero_disk(self.client_dir)
        print("PASS zero-disk: client (replica) user dir clean")

    def shutdown(self):
        self.host.shutdown()
        self.client.shutdown()

    # context-manager form, for tests that prefer it
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.shutdown()
        return False


def bring_up(tag, ports, wait_ready=True,
             host_base="HostBase", client_base="ClientBase"):
    """Stand up a JOINT campaign: host creates it, client joins, the host streams
    the authoritative world, both settle on the geoscape.

    tag   - short prefix for the two isolated user dirs ("jbuy" -> jbuy_host/...).
    ports - (host_test_port, client_test_port, coop_session_port).

    Cleans up its own processes if bring-up fails, so the caller's try/finally
    only has to cover the body.
    """
    js = JointSession(tag, ports)
    try:
        js._start(wait_ready, host_base, client_base)
    except BaseException:
        js.shutdown()
        raise
    return js
