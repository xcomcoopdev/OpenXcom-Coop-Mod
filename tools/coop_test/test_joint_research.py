"""PRD-J06: JOINT research start / allocate / cancel via joint_cmd.

A JOINT campaign is one host-authoritative world. The research screens mutate
NOTHING locally: starting a project, (re)allocating scientists, and cancelling a
project each ride the J03 joint_cmd protocol. The host validates against the live
world, applies, and broadcasts joint_apply; replicas apply ONLY from joint_apply.
Completion stays host-driven (J04 research_done).

  START    client opens ResearchInfoState and starts a project + 5 scientists
           (REAL screen -> res_start + res_alloc) -> host AND client show the
           same project with the same allocation, pools reduced equally.
  OVER     res_alloc more scientists than are free -> joint_fail, pools unchanged.
  RACE     both players res_alloc the SAME project "simultaneously" -> the final
           _assigned equals ONE of the two absolute values on BOTH machines
           (no compounding), scientist pools consistent.
  CANCEL   client cancels a second project -> both sides free the scientists.
  DONE     host advances days -> the project completes -> both sides discovered +
           scientists freed (J04 research_done path).

Run:  python tools/coop_test/test_joint_research.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo


def _base0(gc):
    """The first real (non-mirror) base from the geoscape snapshot."""
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _free_sci(gc):
    return _base0(gc)["freeScientists"]


def _proj(gc, topic):
    """The running research project dict for <topic>, or None."""
    for r in _base0(gc)["research"]:
        if r["name"] == topic:
            return r
    return None


def _assigned(gc, topic):
    p = _proj(gc, topic)
    return p["assigned"] if p else None


def _stats(gc):
    return gc.ok({"cmd": "joint_stats"})


def main():
    host_dir = make_user_dir("jres_host")
    client_dir = make_user_dir("jres_client")
    host = GameClient("host", 48720, host_dir)
    client = GameClient("client", 48721, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        session.new_campaign(host, client, port="48020", campaign_mode="joint")
        geo.wait_both_ready(host, client)

        # available research is a property of the shared world -> identical on both.
        topics_h = host.ok({"cmd": "available_research"})["topics"]
        topics_c = client.ok({"cmd": "available_research"})["topics"]
        assert topics_h == topics_c, f"available research differs: {topics_h} vs {topics_c}"
        assert len(topics_h) >= 2, f"need >=2 startable topics, got {topics_h}"
        T, T2 = topics_h[0], topics_h[1]
        print(f"PASS setup: {len(topics_h)} startable topics; using {T} + {T2}")

        sci0 = _free_sci(host)
        assert sci0 == _free_sci(client), "starting scientist pool differs"
        assert sci0 >= 8, f"expected the vanilla starting lab (>=8 sci), got {sci0}"

        # ================================================================
        # 1) START: client drives the REAL ResearchInfoState -> res_start +
        #    res_alloc(5). Host + client converge to the same project + alloc.
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        r = client.ok({"cmd": "research_start", "topic": T, "scientists": 5})
        assert r.get("sent"), f"client research_start not sent: {r}"
        host.wait_for("host started project",
                      lambda: (_assigned(host, T) == 5) or None, timeout=30, interval=0.5)
        client.wait_for("client started project",
                        lambda: (_assigned(client, T) == 5) or None, timeout=30, interval=0.5)
        assert _free_sci(host) == _free_sci(client) == sci0 - 5, \
            f"scientist pool mismatch after start: host={_free_sci(host)} client={_free_sci(client)}"
        print(f"PASS start: {T} @ 5 scientists on both; free scientists {sci0} -> {sci0 - 5}")

        # ================================================================
        # 2) OVER-ALLOCATION: ask for more scientists than are free -> reject,
        #    pools unchanged on both sides.
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        pre_sci_h, pre_sci_c = _free_sci(host), _free_sci(client)
        pre_asg = _assigned(host, T)
        # absolute target 5 + (free+50) is unreachable -> host rejects.
        r = client.ok({"cmd": "joint_cmd", "jcmd": "res_alloc", "baseId": 0,
                        "payload": {"project": T, "assigned": pre_asg + pre_sci_h + 50}})
        client.wait_for("client saw the rejection",
                        lambda: (_stats(client)["failCount"] >= 1) or None, timeout=30, interval=0.5)
        assert _assigned(host, T) == pre_asg and _assigned(client, T) == pre_asg, \
            "allocation changed on a rejected res_alloc"
        assert _free_sci(host) == pre_sci_h and _free_sci(client) == pre_sci_c, \
            "scientist pool changed on a rejected res_alloc"
        print(f"PASS over-alloc: rejected '{_stats(client)['lastFail']}', pools unchanged (asg {pre_asg}, free {pre_sci_h})")
        # dismiss the initiator's failure popup so it can't block later.
        try:
            client.ok({"cmd": "coop_dialog_back"})
        except Exception:
            pass

        # ================================================================
        # 3) RACE: both players res_alloc the SAME project within one pump.
        #    Final _assigned must equal ONE of the two absolutes on BOTH sides
        #    (absolute/last-write-wins, no compounding); pools consistent.
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        A, B = 7, 3
        # fire both as close together as the harness allows -> same/adjacent pump.
        host.ok({"cmd": "joint_cmd", "jcmd": "res_alloc", "baseId": 0,
                 "payload": {"project": T, "assigned": A}})
        client.ok({"cmd": "joint_cmd", "jcmd": "res_alloc", "baseId": 0,
                   "payload": {"project": T, "assigned": B}})

        def _converged():
            ah, ac = _assigned(host, T), _assigned(client, T)
            if ah is None or ac is None or ah != ac:
                return None
            if ah not in (A, B):
                return None
            if _free_sci(host) != _free_sci(client):
                return None
            if _free_sci(host) != sci0 - ah:
                return None
            return True

        host.wait_for("race converged", _converged, timeout=30, interval=0.5)
        final = _assigned(host, T)
        assert _converged(), (
            f"race did not converge: host asg={_assigned(host, T)} free={_free_sci(host)}; "
            f"client asg={_assigned(client, T)} free={_free_sci(client)}")
        print(f"PASS race: both settled on _assigned={final} (one of {A}/{B}), "
              f"free scientists {_free_sci(host)} on both")

        # ================================================================
        # 4) CANCEL: start a second project, then client cancels it -> both
        #    sides free the scientists and drop the project.
        # ================================================================
        r = client.ok({"cmd": "research_start", "topic": T2, "scientists": 2})
        assert r.get("sent"), f"client second research_start not sent: {r}"
        host.wait_for("host started 2nd project",
                      lambda: (_assigned(host, T2) == 2) or None, timeout=30, interval=0.5)
        client.wait_for("client started 2nd project",
                        lambda: (_assigned(client, T2) == 2) or None, timeout=30, interval=0.5)
        sci_before_cancel = _free_sci(host)
        assert sci_before_cancel == _free_sci(client), "pool differs before cancel"

        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_cmd", "jcmd": "res_cancel", "baseId": 0,
                   "payload": {"project": T2}})
        host.wait_for("host cancelled 2nd project",
                      lambda: (_proj(host, T2) is None) or None, timeout=30, interval=0.5)
        client.wait_for("client cancelled 2nd project",
                        lambda: (_proj(client, T2) is None) or None, timeout=30, interval=0.5)
        assert _free_sci(host) == _free_sci(client) == sci_before_cancel + 2, \
            f"cancel did not free 2 scientists equally: host={_free_sci(host)} client={_free_sci(client)}"
        print(f"PASS cancel: {T2} gone on both; free scientists {sci_before_cancel} -> {_free_sci(host)}")

        # ================================================================
        # 5) COMPLETION: force a low cost on the host, advance days -> the host
        #    completes T and broadcasts research_done -> both discover it and
        #    free the assigned scientists (J04 path, on a JOINT-started project).
        # ================================================================
        assert not host.ok({"cmd": "is_researched", "topic": T})["researched"], "T already researched?!"
        r = host.ok({"cmd": "set_research_cost", "topic": T, "cost": 3})
        assert r.get("found"), f"could not set research cost on host: {r}"
        geo.skip_ingame_time(host, client, minutes=60 * 24 * 3, speed_idx=5, real_timeout=150)
        host.wait_for("host discovered T",
                      lambda: host.ok({"cmd": "is_researched", "topic": T})["researched"] or None,
                      timeout=30, interval=0.5)
        client.wait_for("client discovered T",
                        lambda: client.ok({"cmd": "is_researched", "topic": T})["researched"] or None,
                        timeout=30, interval=0.5)
        assert _proj(host, T) is None and _proj(client, T) is None, \
            "completed project still present in the research list"
        assert _free_sci(host) == _free_sci(client) == sci0, \
            f"scientists not freed on completion: host={_free_sci(host)} client={_free_sci(client)} (want {sci0})"
        print(f"PASS completion: {T} discovered on both; scientists freed back to {sci0}")

        # standing invariant: the JOINT replica never wrote save data to disk.
        session.assert_client_zero_disk(client_dir)
        print("PASS zero-disk: client (replica) user dir clean")

        print("ALL JOINT RESEARCH TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
