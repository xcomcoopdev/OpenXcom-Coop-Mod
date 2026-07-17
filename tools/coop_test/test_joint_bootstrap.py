"""PRD-J02: JOINT world bootstrap - the client receives a streamed replica.

Host creates a JOINT campaign and places its first base. The client never
builds its own world: it waits for the host to stream the authoritative world
and adopts it as a replica. This test asserts:

  AC1  the client replica holds the SAME real base (name + coordinates) and
       the SAME funds as the host - NOT a _coopBase/_coopIcon mirror
       (coopBase == false on every base on both sides).
  AC3  a JOINT replica's manual save attempt is refused with a popup and
       writes nothing to disk.
  AC4  the host's on-disk JOINT .sav carries `coopCampaignType: 1` and NO
       `coopClientSaves` sequence (the JOINT world is the single authority).

Run:  python tools/coop_test/test_joint_bootstrap.py
"""

import gzip
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session

JOINT = 1


def _read_save_text(user_dir, filename):
    """Decoded text of a save under user_dir (gunzips a compressed save)."""
    path = None
    for root, _dirs, files in os.walk(user_dir):
        if filename in files:
            path = os.path.join(root, filename)
            break
    assert path is not None, f"save {filename!r} not found under {user_dir}"
    with open(path, "rb") as fh:
        raw = fh.read()
    if raw[:2] == b"\x1f\x8b":  # gzip magic
        raw = gzip.decompress(raw)
    return raw.decode("latin-1")


def _real_bases(geo):
    """Non-mirror bases from a geo_state snapshot."""
    return [b for b in geo["bases"] if not b.get("coopBase")]


def main():
    js = joint_fixture.bring_up("jboot", (48660, 48661, 47960))
    host, client = js.host, js.client
    host_dir, client_dir = js.host_dir, js.client_dir
    try:

        # both live saves must report JOINT
        hm = host.ok({"cmd": "save_markers"})
        cm = client.ok({"cmd": "save_markers"})
        assert hm.get("campaignType") == JOINT, f"host not JOINT: {hm}"
        assert cm.get("campaignType") == JOINT, f"client not JOINT: {cm}"
        assert cm["coopPlayers"] == hm["coopPlayers"], \
            f"roster mismatch host={hm['coopPlayers']} client={cm['coopPlayers']}"
        print("PASS both worlds report campaignType Joint, same roster")

        # AC1: the replica holds the SAME real base + funds, no mirror bases.
        hgeo = host.ok({"cmd": "geo_state"})
        cgeo = client.ok({"cmd": "geo_state"})

        assert hgeo["funds"] == cgeo["funds"], \
            f"funds differ: host={hgeo['funds']} client={cgeo['funds']}"

        hbases = _real_bases(hgeo)
        cbases = _real_bases(cgeo)
        assert len(hbases) == 1, f"host should have exactly one base: {hgeo['bases']}"
        assert len(cbases) == len(hbases), \
            f"client base count != host: {cgeo['bases']} vs {hgeo['bases']}"

        hb, cb = hbases[0], cbases[0]
        assert hb["name"] == cb["name"], f"base name differs: {hb['name']} vs {cb['name']}"
        assert abs(hb["lon"] - cb["lon"]) < 1e-6 and abs(hb["lat"] - cb["lat"]) < 1e-6, \
            f"base coords differ: host=({hb['lon']},{hb['lat']}) client=({cb['lon']},{cb['lat']})"
        print(f"PASS replica base matches host: {cb['name']!r} @ ({cb['lon']:.4f},{cb['lat']:.4f}), "
              f"funds={cgeo['funds']}")

        # no mirror bases anywhere on either side
        assert all(not b.get("coopBase") and not b.get("coopIcon") for b in hgeo["bases"]), \
            f"host has mirror bases: {hgeo['bases']}"
        assert all(not b.get("coopBase") and not b.get("coopIcon") for b in cgeo["bases"]), \
            f"client has mirror bases: {cgeo['bases']}"
        print("PASS no _coopBase/_coopIcon mirror bases on host or client")

        # AC3: a JOINT replica's manual save is refused (popup + no disk write).
        client.ok({"cmd": "save_game_ui", "type": "quick"})
        client.wait_for(
            "replica save refused (popup)",
            lambda: session._has_state(client, "CoopState"),
            timeout=30,
        )
        print("PASS JOINT replica manual save refused with a popup")
        # dismiss the popup so it does not leak into later interactions
        try:
            client.ok({"cmd": "coop_dialog_back"})
        except Exception:
            pass

        # AC4: host on-disk JOINT save has coopCampaignType:1 and NO coopClientSaves.
        host.ok({"cmd": "save_game", "file": "jboot_check.sav"})
        text = _read_save_text(host_dir, "jboot_check.sav")
        assert re.search(r"coopCampaignType:\s*1", text), \
            "host JOINT save missing 'coopCampaignType: 1'"
        assert "coopClientSaves" not in text, \
            "host JOINT save must NOT embed a coopClientSaves sequence"
        print("PASS host JOINT save: coopCampaignType 1, no coopClientSaves embed")

        # PRD-J11: the shared final-state assertions (world equality +
        # the replica's zero-disk invariant).
        js.finish()

        print("ALL JOINT BOOTSTRAP TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
