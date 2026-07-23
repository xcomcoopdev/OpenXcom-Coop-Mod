"""In-game Save Upgrader: load-gate + full upgrade->load->rejoin flow.

This is the e2e that needs a REAL, loadable save (unlike test_save_upgrade.py,
which unit-tests the raw-YAML engine on synthetic fixtures). Strategy mirrors the
retired test_legacy_migration.py: run a real co-op campaign to get a modern
host save with an embedded client world, then fabricate a pre-schema legacy DUAL
pair from it (strip the modern co-op fields; decode the embedded client blob into
a standalone client .sav). Then:

  Gate  : load the un-upgraded DUAL host through the real LoadGameState. The
          schema gate (PRD 4) must intercept it - the SaveUpgradeDialogState is
          pushed and it must NOT load as a solo geoscape. Asserted via the
          TestServer state-stack dump (get_state); the real button-driven dialog
          interaction is not scripted from the harness (see report / un-verified).

  Flow  : run the upgrade headless (UpgradeRunner via upgrade_run), then LOAD the
          upgraded save through the real menu path -> host window -> resume lobby;
          a second client instance rejoins with the exact client name and gets its
          world streamed back (roster + base intact), zero-disk.

Run:  python tools/coop_test/test_save_upgrade_flow.py
"""

import base64
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import yaml

from harness import GameClient, make_user_dir
import session

HOST_NAME = "HostPlayer"
CLIENT_NAME = "ClientPlayer"
DUAL_HOST = "dual_host.sav"
DUAL_CLIENT = "dual_client.sav"

# top-level keys to drop so a modern save looks pre-schema legacy-dual:
#  header: the schema stamp + co-op markers   body: the embed + host-authority tags
# kept: saveID + coop_gamemode (the dual fingerprint), all real geoscape state.
STRIP_TOP_KEYS = ("coop:", "coopPlayers:", "saveSchema:",
                  "coopClientSaves:", "coop_save_owner_player_id:", "no_bases:")


def strip_modern_coop_fields(text):
    """Remove whole top-level blocks/lines named in STRIP_TOP_KEYS from a
    two-document stream, preserving every other byte (line-based, like the old
    v1.8.4 migration test) so the real geoscape body still loads."""
    out, in_block = [], False
    for line in text.splitlines(keepends=True):
        stripped = line.rstrip("\n").rstrip("\r")
        is_top = bool(stripped) and stripped != "---" and not stripped[0].isspace()
        if in_block:
            if is_top or stripped == "---":
                in_block = False  # block ended; fall through
            else:
                continue  # still inside a dropped block
        if is_top and any(stripped.startswith(k) for k in STRIP_TOP_KEYS):
            # a block key opens a nested block if nothing follows the colon
            rest = stripped.split(":", 1)[1].strip()
            if rest == "":
                in_block = True
            continue
        out.append(line)
    return "".join(out)


def derive_legacy_dual(modern_text):
    """From a modern host save produce (host_dual_text, client_dual_text).

    Strip every modern co-op marker so the host is NOT classified Current, then
    inject a single genuine cross-instance link (force the first soldier's coopbase
    to a peer id). Under detector v2.3 that non-default per-object value is the ONLY
    positive signal that gates a save - coop_gamemode/saveID and the random per-object
    ids are all treated as noise now. The 1->2 transform resets that link, so the
    upgraded save still loads clean."""
    hdr, body = list(yaml.safe_load_all(modern_text))
    ccs = body.get("coopClientSaves") or []
    assert ccs, "modern source save has no embedded client world to derive from"
    client_modern = base64.b64decode(ccs[0]["blob"]).decode("utf-8")
    host_dual = strip_modern_coop_fields(modern_text)
    client_dual = strip_modern_coop_fields(client_modern)
    # inject a STRONG marker: force the first soldier's coopbase to a peer link
    host_dual = re.sub(r"(?m)^(\s+)coopbase: .*$", r"\g<1>coopbase: 4501", host_dual, count=1)
    assert "coopbase: 4501" in host_dual, "failed to inject a strong co-op marker into the host"
    # sanity: the stripped host must no longer carry the modern markers
    h2, b2 = list(yaml.safe_load_all(host_dual))
    assert "coop" not in h2 and "saveSchema" not in h2, "strip left modern header markers"
    assert "coopClientSaves" not in b2, "strip left the embed block"
    return host_dual, client_dual


def _remove_indented_block(text, key):
    """Drop a block-style mapping key (e.g. 'coopItems:') and every line indented
    deeper than it, at any nesting depth. Preserves everything else byte-for-byte."""
    lines = text.splitlines(keepends=True)
    out, i = [], 0
    while i < len(lines):
        m = re.match(r"^(\s*)" + re.escape(key) + r":\s*$", lines[i].rstrip("\r\n"))
        if m:
            indent = len(m.group(1))
            i += 1
            while i < len(lines):
                nxt = lines[i]
                if nxt.strip() == "":
                    i += 1
                    continue
                if len(nxt) - len(nxt.lstrip()) > indent:
                    i += 1
                else:
                    break
            continue
        out.append(lines[i])
        i += 1
    return "".join(out)


def derive_solo(modern_text):
    """From the same modern host save produce a loadable SOLO save carrying the fork
    build's always-written coop keys but NO genuine cross-instance link: strip the
    modern markers, neutralize every strong soldier/craft link back to its default,
    and inject a random per-object coopUfoId onto the first ufo (if any). Detector
    v2.3 must classify this Solo and the real load path must load it straight to the
    geoscape without gating - the end-to-end proof of the false-positive fix."""
    t = strip_modern_coop_fields(modern_text)
    # neutralize any STRONG soldier/vehicle link values back to their defaults
    t = re.sub(r"(?m)^(\s+)coop: [1-9]\d*(\s*)$", r"\g<1>coop: 0\g<2>", t)
    t = re.sub(r"(?m)^(\s+)coopbase: (?!-1\b)-?\d+(\s*)$", r"\g<1>coopbase: -1\g<2>", t)
    t = re.sub(r"(?m)^(\s+)coopcraft: (?!-1\b)-?\d+(\s*)$", r"\g<1>coopcraft: -1\g<2>", t)
    t = re.sub(r"(?m)^(\s+)coopcrafttype: (?!''\s*$).*$", r"\g<1>coopcrafttype: ''", t)
    # drop strong craft dest-id lines + the coopItems peer-item block
    t = re.sub(r"(?m)^\s+coopDest(?:Ufo|Mission)Id: .*\r?\n", "", t)
    t = _remove_indented_block(t, "coopItems")
    return t


def generate_fixtures():
    """Bring up a real co-op campaign, save it modern, and return the derived legacy
    DUAL (host_text, client_text) plus a SOLO host text - all from the same real,
    loadable geoscape save."""
    host_dir = make_user_dir("upg_gen_host")
    client_dir = make_user_dir("upg_gen_client")
    host = GameClient("host", 48730, host_dir)
    client = GameClient("client", 48731, client_dir)
    try:
        host.spawn(); client.spawn(); host.connect(); client.connect()
        session.new_campaign(host, client, host_name=HOST_NAME, client_name=CLIENT_NAME)
        host.ok({"cmd": "save_game", "file": "modern_src.sav"})
        modern = open(os.path.join(host_dir, "xcom1", "modern_src.sav"), encoding="utf-8").read()
        host_dual, client_dual = derive_legacy_dual(modern)
        solo_text = derive_solo(modern)
        return host_dual, client_dual, solo_text
    finally:
        for gc in (host, client):
            try: gc.shutdown()
            except Exception: pass


def test_gate(host_text):
    """The un-upgraded DUAL save must be intercepted by the load gate, not
    loaded as a solo geoscape."""
    d = make_user_dir("upg_gate")
    with open(os.path.join(d, "xcom1", DUAL_HOST), "w", encoding="utf-8", newline="\n") as f:
        f.write(host_text)
    host = GameClient("host", 48732, d)
    try:
        host.spawn(); host.connect()
        # sanity: detector agrees this is a legacy dual before we drive the menu
        det = host.ok({"cmd": "upgrade_detect", "file": DUAL_HOST})
        assert det["kind"] == "legacy" and det["variant"] == "dual", det
        host.ok({"cmd": "load_save_menu", "file": DUAL_HOST})
        # the gate pushes SaveUpgradeDialogState; assert it appears
        host.wait_for("upgrade dialog", lambda: session.has_state(host, "SaveUpgradeDialogState"))
        st = host.cmd({"cmd": "get_state"})["states"]
        assert not any("GeoscapeState" in s for s in st), f"save loaded as solo geoscape despite gate: {st}"
        assert not any("HostMenu" in s for s in st), f"save reached the host window without upgrading: {st}"
        print("PASS gate: legacy dual intercepted -> SaveUpgradeDialogState (did not load as solo)")
    finally:
        try: host.shutdown()
        except Exception: pass


def test_solo_never_gates(solo_text):
    """A real, loadable SOLO save that still carries the fork build's always-written
    coop keys (coop_gamemode, per-soldier coopname, default coop link fields) must
    NEVER gate: the detector classifies it Solo and the real load path loads it
    straight to the geoscape with NO upgrade dialog. This is the end-to-end proof of
    the false-positive fix (detector v2.3)."""
    d = make_user_dir("upg_solo_never")
    solo_name = "solo_src.sav"
    with open(os.path.join(d, "xcom1", solo_name), "w", encoding="utf-8", newline="\n") as f:
        f.write(solo_text)
    host = GameClient("host", 48735, d)
    try:
        host.spawn(); host.connect()
        det = host.ok({"cmd": "upgrade_detect", "file": solo_name})
        assert det["kind"] == "solo", det
        assert det["needsUpgrade"] is False, det
        # the real load path must reach a normal geoscape - no upgrade dialog at all
        host.ok({"cmd": "load_save_menu", "file": solo_name})
        host.wait_for("solo geoscape", lambda: session.has_state(host, "GeoscapeState"))
        st = host.cmd({"cmd": "get_state"})["states"]
        assert not any("SaveUpgrade" in s for s in st), f"solo save was gated by the upgrader: {st}"
        assert not any("HostMenu" in s for s in st), f"solo save loaded as co-op: {st}"
        markers = host.ok({"cmd": "save_markers"})
        assert markers["coop"] is False, f"solo save must load as solo (coop=false): {markers}"
        print("PASS solo-never-gates: fork-era solo save loaded straight to geoscape, no gate")
    finally:
        try: host.shutdown()
        except Exception: pass


def test_upgrade_and_rejoin(host_text, client_text):
    """Upgrade the DUAL pair headless, then LOAD the upgraded save through the
    real menu flow and have a fresh client rejoin and receive its world."""
    host_dir = make_user_dir("upg_flow_host")
    client_dir = make_user_dir("upg_flow_client")
    with open(os.path.join(host_dir, "xcom1", DUAL_HOST), "w", encoding="utf-8", newline="\n") as f:
        f.write(host_text)
    with open(os.path.join(host_dir, "xcom1", DUAL_CLIENT), "w", encoding="utf-8", newline="\n") as f:
        f.write(client_text)

    host = GameClient("host", 48733, host_dir)
    client = GameClient("client", 48734, client_dir)
    try:
        host.spawn(); client.spawn(); host.connect(); client.connect()

        # 1. upgrade headless (backup + chained migration + atomic write)
        r = host.ok({"cmd": "upgrade_run", "host": DUAL_HOST, "client": DUAL_CLIENT,
                     "clientName": CLIENT_NAME, "hostName": HOST_NAME})
        assert r["success"] is True, f"upgrade failed: {r}"
        assert os.path.exists(os.path.join(host_dir, "xcom1", "dual_host_bak_v1.sav")), "no backup written"
        assert host.ok({"cmd": "upgrade_detect", "file": DUAL_HOST})["kind"] == "current", "not current after upgrade"
        print("PASS upgrade: backup written, save now schema-current")

        # 2. LOAD the upgraded save through the real menu -> host window -> resume
        #    lobby, then a fresh client rejoins with the exact name and is served
        #    its world (this is the proven resume flow, driven on an UPGRADED save).
        session.resume_campaign(host, client, DUAL_HOST, port="47950",
                                host_name=HOST_NAME, client_name=CLIENT_NAME)

        # 3. spot-check the resumed world on both sides
        markers = host.ok({"cmd": "save_markers"})
        assert markers["coop"] is True, markers
        assert markers["coopPlayers"] == [HOST_NAME, CLIENT_NAME], markers["coopPlayers"]
        cc = client.cmd({"cmd": "get_coop"})
        assert cc.get("hasSave"), f"client did not receive a world after rejoin: {cc}"
        # client received its world from the host embed (zero-disk invariant holds)
        session.assert_client_zero_disk(client_dir)
        print("PASS rejoin: upgraded save loaded, client streamed its world, roster intact, zero-disk")
    finally:
        for gc in (host, client):
            try: gc.shutdown()
            except Exception: pass


def main():
    host_text, client_text, solo_text = generate_fixtures()
    print("derived legacy DUAL pair + a solo save from a real campaign save")
    test_gate(host_text)
    test_solo_never_gates(solo_text)
    test_upgrade_and_rejoin(host_text, client_text)
    print("ALL PASS test_save_upgrade_flow")


if __name__ == "__main__":
    main()
