"""PRD-J01: the JOINT/SEPARATE campaign-type flag end to end.

Host creates a JOINT co-op campaign from the New Game dropdown, a client
joins, and BOTH live saves report campaignType Joint (driven via TestServer,
mirroring test_new_campaign_flow.py). Also verifies the flag round-trips the
save YAML: the host's on-disk save carries `coopCampaignType: 1`.

No JOINT behavior is exercised - with PRD-J01 only the flag/label exist, so
the campaign still *behaves* as SEPARATE. (SEPARATE unchanged is covered by
the rest of the suite, e.g. test_new_campaign_flow.py.)

Run:  python tools/coop_test/test_joint_flag.py
"""

import gzip
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import joint_fixture
import session

SEPARATE, JOINT = 0, 1


def _read_save_text(user_dir, filename):
    """Return the decoded text of a save under user_dir (transparently
    gunzips if the engine wrote a compressed save)."""
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


def main():
    js = joint_fixture.bring_up("joint", (48640, 48641, 47941))
    host, client = js.host, js.client
    host_dir, client_dir = js.host_dir, js.client_dir
    try:

        # AC4: both live saves report campaignType Joint
        hm = host.ok({"cmd": "save_markers"})
        cm = client.ok({"cmd": "save_markers"})
        assert hm["coop"] is True, f"host save must be coop-marked: {hm}"
        assert cm["coop"] is True, f"client save must be coop-marked: {cm}"
        assert hm.get("campaignType") == JOINT, f"host must report JOINT: {hm}"
        assert cm.get("campaignType") == JOINT, f"client must report JOINT: {cm}"
        print("PASS both worlds report campaignType Joint")

        # AC1: the flag round-trips the save YAML (host on-disk save)
        host.ok({"cmd": "save_game", "file": "joint_check.sav"})
        text = _read_save_text(host_dir, "joint_check.sav")
        assert re.search(r"coopCampaignType:\s*1", text), \
            "host save YAML missing 'coopCampaignType: 1'"
        print("PASS host save YAML carries coopCampaignType: 1")

        # PRD-J11: the shared final-state assertions (world equality +
        # the replica's zero-disk invariant).
        js.finish()

        print("ALL JOINT FLAG TESTS PASSED")
    finally:
        js.shutdown()


if __name__ == "__main__":
    main()
