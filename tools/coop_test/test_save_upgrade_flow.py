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
WEAK_ONLY = "weak_only.sav"

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


def derive_weak_only(modern_text):
    """From a modern host save produce a WEAK-ONLY legacy save: strip every modern
    co-op marker AND every STRONG deep-body marker, but keep the WEAK ones the era
    build always wrote (coop_gamemode, per-soldier coopname, base coopbaseid). The
    detector must classify this AmbiguousBuild, and it must still load as a solo
    geoscape when the player chooses "load as solo"."""
    t = strip_modern_coop_fields(modern_text)            # drop coop:true/saveSchema/coopClientSaves/...
    t = re.sub(r"(?m)^saveID: .*\r?\n", "", t)            # no saveID -> the saveID!=0 rule must not fire
    # coop_gamemode present but 0 (a WEAK marker, not the strong gamemode!=0 rule)
    if re.search(r"(?m)^coop_gamemode:", t):
        t = re.sub(r"(?m)^coop_gamemode: .*$", "coop_gamemode: 0", t)
    else:
        t = t.replace("---\n", "---\ncoop_gamemode: 0\n", 1)
    # neutralise any STRONG soldier/vehicle link values back to their defaults
    t = re.sub(r"(?m)^(\s+)coop: [1-9]\d*(\s*)$", r"\1coop: 0\2", t)
    t = re.sub(r"(?m)^(\s+)coopbase: (?!-1\b)-?\d+(\s*)$", r"\1coopbase: -1\2", t)
    t = re.sub(r"(?m)^(\s+)coopcraft: (?!-1\b)-?\d+(\s*)$", r"\1coopcraft: -1\2", t)
    t = re.sub(r"(?m)^(\s+)coopcrafttype: (?!''\s*$).*$", r"\1coopcrafttype: ''", t)
    # drop strong id lines + the coopItems peer-item block
    t = re.sub(r"(?m)^\s+coop(?:Dest)?(?:Ufo|Mission)Id: .*\r?\n", "", t)
    t = _remove_indented_block(t, "coopItems")
    return t


def derive_legacy_dual(modern_text):
    """From a modern host save produce (host_dual_text, client_dual_text)."""
    hdr, body = list(yaml.safe_load_all(modern_text))
    ccs = body.get("coopClientSaves") or []
    assert ccs, "modern source save has no embedded client world to derive from"
    client_modern = base64.b64decode(ccs[0]["blob"]).decode("utf-8")
    host_dual = strip_modern_coop_fields(modern_text)
    client_dual = strip_modern_coop_fields(client_modern)
    # sanity: the stripped host must no longer carry the modern markers
    h2, b2 = list(yaml.safe_load_all(host_dual))
    assert "coop" not in h2 and "saveSchema" not in h2, "strip left modern header markers"
    assert "coopClientSaves" not in b2, "strip left the embed block"
    assert b2.get("coop_gamemode") or b2.get("saveID"), "stripped host lost its dual fingerprint"
    return host_dual, client_dual


def generate_fixtures():
    """Bring up a real co-op campaign, save it modern, and return the derived
    legacy DUAL (host_text, client_text) plus a WEAK-ONLY host text - all from the
    same real, loadable geoscape save."""
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
        weak = derive_weak_only(modern)
        # sanity: the weak-only derivation is well-formed and carries no strong marker
        wh, wb = list(yaml.safe_load_all(weak))
        assert "coop" not in wh and "saveSchema" not in wh, "weak derivation left modern header markers"
        assert "saveID" not in wb, "weak derivation left a saveID (would trip the dual rule)"
        return host_dual, client_dual, weak
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


def test_ambiguous_gate(weak_text):
    """A weak-only save must be intercepted by the load gate as AmbiguousBuild ->
    the three-way SaveUpgradeAmbiguousState (NOT a silent solo load, NOT the legacy
    upgrade dialog). Then "load as solo" must reach a normal loaded geoscape."""
    d = make_user_dir("upg_ambiguous")
    with open(os.path.join(d, "xcom1", WEAK_ONLY), "w", encoding="utf-8", newline="\n") as f:
        f.write(weak_text)
    host = GameClient("host", 48735, d)
    try:
        host.spawn(); host.connect()
        # sanity: the detector agrees this is ambiguous before we drive the menu
        det = host.ok({"cmd": "upgrade_detect", "file": WEAK_ONLY})
        assert det["kind"] == "ambiguous_build", det
        assert det["needsUpgrade"] is False, det

        # the gate must push the ambiguous choice dialog, not load solo / not the
        # legacy upgrade dialog
        host.ok({"cmd": "load_save_menu", "file": WEAK_ONLY})
        host.wait_for("ambiguous dialog", lambda: session.has_state(host, "SaveUpgradeAmbiguousState"))
        st = host.cmd({"cmd": "get_state"})["states"]
        assert not any("GeoscapeState" in s for s in st), f"weak save loaded as solo despite gate: {st}"
        assert not any("SaveUpgradeDialogState" in s for s in st), f"weak save routed to the legacy upgrade dialog: {st}"
        print("PASS ambiguous gate: weak-only intercepted -> SaveUpgradeAmbiguousState")

        # "load as solo campaign": drive the real dialog button headless; it must
        # bypass the gate and reach a normal loaded geoscape (no coop, no host window)
        host.ok({"cmd": "upgrade_ambiguous", "choice": "solo"})
        host.wait_for("solo geoscape", lambda: session.has_state(host, "GeoscapeState"))
        st = host.cmd({"cmd": "get_state"})["states"]
        assert not any("HostMenu" in s for s in st), f"weak save loaded as co-op, not solo: {st}"
        markers = host.ok({"cmd": "save_markers"})
        assert markers["coop"] is False, f"weak save must load as solo (coop=false): {markers}"
        print("PASS ambiguous solo: load-as-solo bypassed the gate -> normal solo geoscape")
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
    host_text, client_text, weak_text = generate_fixtures()
    print("derived legacy DUAL pair + weak-only save from a real campaign save")
    test_gate(host_text)
    test_ambiguous_gate(weak_text)
    test_upgrade_and_rejoin(host_text, client_text)
    print("ALL PASS test_save_upgrade_flow")


if __name__ == "__main__":
    main()
