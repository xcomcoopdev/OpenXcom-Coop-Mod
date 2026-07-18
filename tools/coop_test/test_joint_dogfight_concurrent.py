"""PRD-DF03 (shared/replicated dogfights): up to 4 concurrent interceptions hold
the SAME membership set on both machines, and an HK interrupt-all-and-restart
reshuffle converges both to the new set ATOMICALLY - the membership epoch advances
in lock-step, no stale window survives, and no old-epoch df_state is applied after
the reshuffle.

Membership + the epoch guard (README locked decision #4): every membership change
emits a full-set df_open with a monotonically increasing epoch; df_state frames
carry that epoch and a replica drops any frame that predates its current epoch. That
guard exists precisely for the HK reshuffle: when a hunter-killer engages, the
geoscape (GeoscapeState::time5Seconds) INTERRUPTS every running interception -
Collections::deleteAll(_dogfights) - and restarts the fight around the HK (main
target + escorts in range). The old full-set df_state frames are now stale; without
the epoch guard a late old-epoch frame could re-render a window the reshuffle just
closed.

Sequence:
  * 4 armed Avengers + 4 slow scouts clustered near the base (within HK escort
    range); the CLIENT commands each Avenger to intercept a distinct scout -> 4
    concurrent regular dogfights, one membership set on both machines (epoch E1);
  * per-(craft,ufo) command targeting (the DF03 harness addition): drive ONE fight
    aggressive by (craft,ufo) and confirm only that pair changes on both machines;
  * seed an HK scout on the host at craft #1 and make it HUNT craft #1
    (set_ufo_hunt) -> the interrupt-all-and-restart fires; the 4 scout fights are
    torn down and the fight is rebuilt around the HK;
  * both machines converge to the NEW set, epoch E2 > E1, host epoch == client epoch
    (lock-step), an HK fight (ufoIsAttacking) is open on both, and the set stays
    stable + identical across samples (no stale window, no old-epoch frame applied).

Ends by downing the HK to resolve every fight, then world-equality + replica
zero-disk.

Run:  python tools/coop_test/test_joint_dogfight_concurrent.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import geo

STANDOFF, CAUTIOUS, STANDARD, AGGRESSIVE, DISENGAGE = 0, 1, 2, 3, 4
CRASHED = 2
N = 4  # concurrent interceptions (fills all 4 dogfight slots)


def _geo(gc):
    return gc.ok({"cmd": "geo_state"})


def _base0(gc):
    for b in _geo(gc)["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _craft(gc, craft_id, craft_type="STR_AVENGER"):
    # craft ids are per-TYPE (a Skyranger and an Avenger can both be id 1), so match
    # both id and type - matching id alone returns the wrong craft.
    for b in _geo(gc)["bases"]:
        for c in b.get("crafts", []):
            if c["id"] == craft_id and c["type"] == craft_type and not c["coop"]:
                return c
    return None


def _ufo(gc, ufo_id):
    for u in _geo(gc).get("ufos", []):
        if u["id"] == ufo_id:
            return u
    return None


def _dfstate(gc):
    """(membership set of (craftId,ufoId), epoch, {(c,u): df})."""
    d = gc.ok({"cmd": "dogfight_state"})
    dfs = d.get("dogfights", [])
    by = {(df["craftId"], df["ufoId"]): df for df in dfs}
    return frozenset(by), d.get("epoch", -1), by


def _df_for(gc, craft_id, ufo_id):
    return _dfstate(gc)[2].get((craft_id, ufo_id))


def _pump(host, client, n=1):
    geo.skip_realtime(host, client, n, speed_idx=0, stuck_timeout=None)


def _has_hk(by):
    return any(df.get("ufoIsAttacking") for df in by.values())


def _minimize_open(gc):
    """Minimize every non-minimized STARTED fight on this machine. A non-minimized
    dogfight PAUSES the geoscape clock (vanilla), so to let successive crafts arrive
    and open their own fights we keep the host's open fights minimized - then the
    host clock runs (all-minimized) and, via df_state.hostAnyOpen, the client's too."""
    for df in gc.ok({"cmd": "dogfight_state"}).get("dogfights", []):
        if not df["minimized"]:
            gc.cmd({"cmd": "dogfight_action", "action": "minimize",
                    "craft_id": df["craftId"], "ufo_id": df["ufoId"]})


def main():
    js = joint_fixture.bring_up("jdfcc", (48814, 48815, 48114))
    host, client = js.host, js.client
    try:
        b0 = _base0(host)
        blon, blat = b0["lon"], b0["lat"]

        # 4 armed Avengers on BOTH machines (ids lock-step).
        avengers = []
        for _ in range(N):
            h = host.ok({"cmd": "spawn_craft", "type": "STR_AVENGER", "weapon": "STR_CANNON_UC"})
            c = client.ok({"cmd": "spawn_craft", "type": "STR_AVENGER", "weapon": "STR_CANNON_UC"})
            assert h["craft_id"] == c["craft_id"], "avenger ids diverged"
            avengers.append(h["craft_id"])

        # 4 slow full-hull scouts near the base, spaced enough to intercept 1:1.
        scouts = []
        for i in range(N):
            u = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                         "mission": "STR_ALIEN_RESEARCH", "region": "STR_NORTH_AMERICA",
                         "race": "STR_SECTOID", "trajectory": "P0", "state": "flying",
                         "speed": 1, "lon": blon + 0.03 + i * 0.006, "lat": blat})
            scouts.append(u["ufo_id"])
        print(f"setup: avengers={avengers} scouts={scouts}")

        # Materialise all scouts on the client, then the CLIENT intercepts 1:1.
        deadline = time.time() + 60
        while time.time() < deadline and any(_ufo(client, s) is None for s in scouts):
            _pump(host, client, 1)
            time.sleep(0.2)
        assert all(_ufo(client, s) is not None for s in scouts), "client missing some scouts"
        for cid, uid in zip(avengers, scouts):
            client.ok({"cmd": "craft_order", "order": "target", "craft_id": cid,
                       "craft_type": "STR_AVENGER", "ufo_id": uid})

        # Open all 4: minimize each fight as it opens so the geoscape clock keeps
        # running and the remaining crafts can arrive (a non-minimized fight pauses
        # the world - vanilla). Once all 4 are open (minimized) the sets match.
        want = frozenset(zip(avengers, scouts))
        deadline = time.time() + 200
        while time.time() < deadline:
            _pump(host, client, 1)
            _minimize_open(host)
            hs, he, _ = _dfstate(host)
            cs, ce, _ = _dfstate(client)
            if hs == want and cs == want and he == ce:
                break
            time.sleep(0.2)
        hs, he, _ = _dfstate(host)
        cs, ce, _ = _dfstate(client)
        assert hs == want, f"host did not open all {N} intercepts: {sorted(hs)}"
        assert cs == want, f"client membership set != host: client={sorted(cs)}"
        assert he == ce, f"epoch not lock-step at phase A: host={he} client={ce}"
        e1 = he
        print(f"PASS phase A: {N} concurrent intercepts; SAME membership set on both; "
              f"epoch lock-step E1={e1}")

        # --- per-(craft,ufo) targeting: drive ONE fight aggressive, only it changes. -
        tc, tu = avengers[1], scouts[1]
        client.ok({"cmd": "dogfight_action", "action": "aggressive",
                   "craft_id": tc, "ufo_id": tu})
        deadline = time.time() + 30
        while time.time() < deadline:
            _pump(host, client, 1)
            hd = _df_for(host, tc, tu)
            cd = _df_for(client, tc, tu)
            if hd and cd and hd["mode"] == AGGRESSIVE and cd["mode"] == AGGRESSIVE:
                break
            time.sleep(0.15)
        hd, cd = _df_for(host, tc, tu), _df_for(client, tc, tu)
        assert hd and hd["mode"] == AGGRESSIVE and cd and cd["mode"] == AGGRESSIVE, (
            f"per-(craft,ufo) targeting failed: host={hd and hd['mode']} client={cd and cd['mode']}")
        # a DIFFERENT fight is untouched (still standoff) on both machines
        oc, ou = avengers[0], scouts[0]
        ho, co = _df_for(host, oc, ou), _df_for(client, oc, ou)
        assert ho and ho["mode"] == STANDOFF and co and co["mode"] == STANDOFF, (
            f"targeting bled into another fight: host={ho and ho['mode']} client={co and co['mode']}")
        print(f"PASS per-(craft,ufo) targeting: only ({tc},{tu}) went aggressive on "
              f"both; ({oc},{ou}) untouched")
        # revert the driven fight to standoff so it does not down its scout (which would
        # change membership) before the HK reshuffle; keep every fight minimized so the
        # world clock keeps running toward the HK.
        client.ok({"cmd": "dogfight_action", "action": "standoff", "craft_id": tc, "ufo_id": tu})
        _minimize_open(host)

        # --- HK interrupt-all-and-restart reshuffle --------------------------------
        c1 = _craft(host, avengers[0])
        hk = host.ok({"cmd": "spawn_ufo", "type": "STR_MEDIUM_SCOUT",
                      "mission": "STR_ALIEN_RESEARCH", "region": "STR_NORTH_AMERICA",
                      "race": "STR_SECTOID", "trajectory": "P0", "state": "flying",
                      "speed": 1, "lon": c1["lon"], "lat": c1["lat"]})
        hk_id = hk["ufo_id"]
        r = host.ok({"cmd": "set_ufo_hunt", "ufo_id": hk_id,
                     "craft_id": avengers[0], "craft_type": "STR_AVENGER"})
        assert r.get("isHunterKiller") and r.get("isHunting"), f"HK not set: {r}"
        print(f"seeded HK scout id {hk_id} hunting craft {avengers[0]}")

        # Poll for the ATOMIC-CONVERGENCE snapshot: the interrupt tore down the 4 scout
        # fights and rebuilt the fight around the HK, so at the first CONSISTENT snapshot
        # (host set == client set) every fight targets the HK (no scout pair survived),
        # the epoch is lock-step (host == client) and advanced past E1. The HK fight
        # actively resolves, so we capture the moment rather than freezing the set. At
        # every consistent sample the epochs must match - the epoch guard drops any
        # stale old-epoch df_state, so no closed scout window is ever re-rendered.
        deadline = time.time() + 150
        landed = None
        prev_he = prev_ce = e1
        while time.time() < deadline:
            _pump(host, client, 1)
            hs, he, hby = _dfstate(host)
            cs, ce, cby = _dfstate(client)
            # epochs are monotonic on each machine, and the client (which only ADOPTS
            # host df_open epochs) can never run ahead of the host's membership version.
            if he >= 0:
                assert he >= prev_he, f"host epoch went backwards {prev_he}->{he}"
                prev_he = he
            if ce >= 0:
                assert ce >= prev_ce, f"client epoch went backwards {prev_ce}->{ce}"
                assert ce <= he, f"client epoch {ce} ran AHEAD of host {he} (old/foreign epoch applied)"
                prev_ce = ce
            # the first CONSISTENT snapshot after the interrupt: both hold the SAME new
            # all-HK set at the SAME advanced epoch - atomic convergence.
            if (_has_hk(hby) and _has_hk(cby) and hs == cs and he == ce
                    and he > e1 and hs != want and hs):
                landed = (frozenset(hs), he)
                break
            time.sleep(0.2)
        assert landed, (
            "PRD-DF03 FAILED: the HK reshuffle did not converge on both machines "
            f"(last host set={sorted(hs)} epoch={he}; client set={sorted(cs)} epoch={ce}; E1={e1})")
        lset, e2 = landed
        assert all(u == hk_id for (_, u) in lset), \
            f"a pre-reshuffle scout fight survived the reshuffle: {sorted(lset)}"
        assert (avengers[0], hk_id) in lset, \
            f"the HK main-target fight (craft1, hk) was not in the converged set {sorted(lset)}"
        print(f"PASS HK reshuffle CONVERGED atomically: SAME set on both {sorted(lset)}; "
              f"epoch lock-step E2={e2} > E1={e1}; every fight is the HK (no stale scout "
              f"window survived, no old-epoch df_state applied)")

        # --- resolve everything: down every UFO so all fights end + crafts return. ---
        for uid in scouts + [hk_id]:
            probe = host.cmd({"cmd": "set_ufo_damage", "ufo_id": uid, "damage": 0})
            if probe.get("ok"):
                host.cmd({"cmd": "set_ufo_damage", "ufo_id": uid, "damage": probe["damageMax"] // 2})
        deadline = time.time() + 150
        while time.time() < deadline:
            for (cid, uid) in list(_dfstate(host)[0]):
                host.cmd({"cmd": "dogfight_action", "action": "aggressive",
                          "craft_id": cid, "ufo_id": uid})
            _pump(host, client, 1)
            if not _dfstate(host)[0] and not _dfstate(client)[0]:
                break
            # a downed HK ends the fight; make sure no craft lingers re-engaging.
            time.sleep(0.2)
        # send any still-out craft home, then let both settle for world-equality.
        for cid in avengers:
            ch = _craft(host, cid)
            if ch and ch["status"] == "STR_OUT":
                host.cmd({"cmd": "craft_order", "order": "return", "craft_id": cid,
                          "craft_type": "STR_AVENGER"})
        for _ in range(10):
            _pump(host, client, 1)
            time.sleep(0.3)

        js.finish(timeout=90)
        print("ALL JOINT DOGFIGHT CONCURRENT + HK-RESHUFFLE TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
