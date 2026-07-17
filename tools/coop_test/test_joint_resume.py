"""PRD-J02: JOINT resume - the host re-streams the authoritative world.

1. Bring up a JOINT campaign (client adopts a streamed replica), remember the
   host world (base name/coords + funds), host saves, both quit.
2. Host reloads the JOINT save from the menu -> resume lobby.
3. The registered client rejoins with an EMPTY user dir; RESUME makes the host
   serialize its CURRENT world fresh and stream it. The client replica must
   match the host world again (AC2).
4. Client user dir stays free of save data throughout.

Run:  python tools/coop_test/test_joint_resume.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import joint_fixture
import session

SAVE = "joint_resume_e2e.sav"
JOINT = 1


def _real_bases(geo):
    return [b for b in geo["bases"] if not b.get("coopBase")]


def _world_signature(geo):
    """A comparable snapshot: funds + the sorted real-base (name, lon, lat)."""
    bases = sorted(
        (b["name"], round(b["lon"], 6), round(b["lat"], 6)) for b in _real_bases(geo)
    )
    return geo["funds"], bases


def main():
    # NOTE: this test outlives its fixture - it tears the pair down and brings a
    # DIFFERENT pair up (host reloads from disk, client rejoins with a clean dir),
    # so it owns the second pair by hand and only the first comes from bring_up.
    js = joint_fixture.bring_up("jres", (48670, 48671, 47970))
    host, client = js.host, js.client
    host_dir, client_dir = js.host_dir, js.client_dir
    try:
        # the pre-save world is already one shared world
        joint_fixture.assert_world_equal(host, client, "before save")

        # the authoritative world at save time
        before = _world_signature(host.ok({"cmd": "geo_state"}))
        assert before[1], "host must have at least one real base"

        # host saves the JOINT world to disk (host is the single authority)
        host.ok({"cmd": "save_game", "file": SAVE})
        session.assert_client_zero_disk(client_dir)

        # both sessions down
        js.shutdown()

        # host reloads the JOINT save (fresh process, same dir); client rejoins
        # with a clean dir. resume_campaign drives the standard resume handshake;
        # in JOINT the host serializes the CURRENT world fresh and streams it.
        host = GameClient("host", 48672, host_dir)
        host.spawn()
        host.connect()

        client = GameClient("client", 48673, make_user_dir("jres_client2"))
        client.spawn()
        client.connect()

        session.resume_campaign(host, client, SAVE, port="47971")

        # the reloaded host still reports JOINT
        hm = host.ok({"cmd": "save_markers"})
        assert hm.get("campaignType") == JOINT, f"reloaded host not JOINT: {hm}"

        # AC2: the client replica matches the host world again.
        host_after = _world_signature(host.ok({"cmd": "geo_state"}))
        client_after = _world_signature(client.ok({"cmd": "geo_state"}))
        assert host_after == before, \
            f"host world changed across save/reload: {before} -> {host_after}"
        assert client_after == host_after, \
            f"client replica != host world after resume: {client_after} vs {host_after}"
        print(f"PASS resume: client replica matches host world {client_after}")

        # replica reports JOINT + the intact roster, no mirror bases
        cm = client.ok({"cmd": "save_markers"})
        assert cm.get("campaignType") == JOINT, f"client replica not JOINT: {cm}"
        assert cm["coopPlayers"] == hm["coopPlayers"], \
            f"roster mismatch after resume: {cm['coopPlayers']} vs {hm['coopPlayers']}"
        cgeo = client.ok({"cmd": "geo_state"})
        assert all(not b.get("coopBase") and not b.get("coopIcon") for b in cgeo["bases"]), \
            f"client has mirror bases after resume: {cgeo['bases']}"
        print("PASS replica reports JOINT, intact roster, no mirror bases")

        # PRD-J11: the resumed pair is one shared world, whole - not just the
        # funds/base signature above (stores, roster + ownership, facilities,
        # transfers, research and craft identity all survived the round trip).
        joint_fixture.assert_world_equal(host, client, "after resume")

        session.assert_client_zero_disk(client.user_dir)
        print("PASS zero-disk: resumed client (replica) user dir clean")

        print("ALL JOINT RESUME TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
