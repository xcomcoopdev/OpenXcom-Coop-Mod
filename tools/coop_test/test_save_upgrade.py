"""In-game Save Upgrader: detector + runner unit/e2e coverage.

Drives the real Phase A engine (SchemaDetector + UpgradeRunner) through the
TestServer hooks `upgrade_detect`, `upgrade_run`, `upgrade_selftest` - no UI, no
campaign bring-up. Fixtures are synthesized to the documented legacy shapes
(tools/coop_test/fixtures/synth.py); the framework works on raw YAML and never
builds a SavedGame, so these minimal-but-well-formed saves exercise the whole
detect -> preflight -> transform -> emit -> post-flight path.

The in-game LOAD of an upgraded save (which does need a real, loadable save) and
the load-gate dialog are covered by test_save_upgrade_flow.py.

Covers (PRD save-upgrader.md 9):
- detector classification: dual / dual+saveID / embed / sidecar / solo / current
  / unknown-future / malformed
- disk-less self-test (runSelfTest)
- runner e2e on the DUAL pair: original left untouched, upgraded written to a new file
  (header coop/coopPlayers/saveSchema:2; body saveID/owner 0/coopClientSaves[1]
  whose blob decodes to owner 1 + client base; soldiers stamped both sides)
- embed + sidecar recovery (ports test_legacy_migration.py's intent to the new flow)
- negatives: mid-battle refused, gamemode mismatch refused, skip-client warns + 0 saves

Run:  python tools/coop_test/test_save_upgrade.py
"""

import base64
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures"))

import yaml

from harness import GameClient, make_user_dir
import synth

PORT = 48710


def two_docs(path):
    with open(path, "r", encoding="utf-8") as f:
        docs = list(yaml.safe_load_all(f.read()))
    assert len(docs) == 2, f"{path}: expected 2 YAML docs, got {len(docs)}"
    return docs[0], docs[1]


def main():
    d = make_user_dir("save_upgrade")
    xcom1 = os.path.join(d, "xcom1")

    def reset_fixtures():
        # Clean slate: remove any prior run's output (the upgrade writes a NEW
        # "<stem>_upgraded.sav" and never clobbers, so stale outputs would otherwise
        # accumulate as _upgraded-2.sav and shadow this section's file).
        if os.path.isdir(xcom1):
            shutil.rmtree(xcom1)
        synth.write_all(xcom1)

    # New file model: the upgrade never touches the original; it writes the upgraded
    # content to "<stem>_upgraded.sav". These map a host fixture name -> that file.
    def upgname(host):
        return host[:-4] + "_upgraded.sav"

    def outp(host):
        return os.path.join(xcom1, upgname(host))

    reset_fixtures()
    gc = GameClient("host", PORT, d)
    try:
        gc.spawn()
        gc.connect()

        # ---- 1. disk-less self-test -----------------------------------------
        st = gc.ok({"cmd": "upgrade_selftest"})
        assert st["pass"] is True, f"self-test failed: {st.get('log')}"
        print("PASS self-test:", " | ".join(st["log"]))

        # ---- 2. detector classification -------------------------------------
        expect = {
            "dual_host.sav":        ("legacy", "dual", True),   # genuine soldier peer-link (STRONG)
            "solo_saveid.sav":      ("solo", "none", False),    # saveID but no sidecar/marker -> Solo
            "dual_host_strong.sav": ("legacy", "dual", True),   # detector v2.3: STRONG deep-scan markers
            "solo_coopbuild.sav":   ("solo", "none", False),    # fork solo save (coop keys, no genuine marker)
            "solo_with_ufo.sav":    ("solo", "none", False),    # random coopUfoId/coopMissionId noise -> Solo
            "vanilla_solo.sav":     ("solo", "none", False),    # pure OXCE shape, zero coop keys
            "embed_host.sav":       ("legacy", "embed", True),
            "embed_no_blob.sav":    ("solo", "none", False),    # F2: key but no blob -> not a recoverable embed -> Solo
            "sidecar_host.sav":     ("legacy", "sidecar", True),
            "solo.sav":             ("solo", "none", False),
            "current.sav":          ("current", "none", False),
            "future.sav":           ("unknown_future", "none", False),
            "malformed.sav":        ("malformed", "none", False),
        }
        for fname, (kind, variant, needs) in expect.items():
            r = gc.ok({"cmd": "upgrade_detect", "file": fname})
            assert r["kind"] == kind, f"{fname}: kind {r['kind']} != {kind}"
            assert r["variant"] == variant, f"{fname}: variant {r['variant']} != {variant}"
            assert r["needsUpgrade"] == needs, f"{fname}: needsUpgrade {r['needsUpgrade']} != {needs}"
        assert gc.ok({"cmd": "upgrade_detect", "file": "future.sav"})["schema"] == 99
        print("PASS detector table:", ", ".join(expect))

        # ---- 3. runner e2e on the DUAL pair ---------------------------------
        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "dual_host.sav", "client": "dual_client.sav",
                   "clientName": "Carol", "hostName": "HostGuy"})
        assert r["success"] is True, f"dual upgrade failed: {r}"
        assert r["errors"] == [], r["errors"]
        # new model: the upgraded content is a NEW file; the ORIGINAL is untouched.
        upgraded = outp("dual_host.sav")
        assert os.path.exists(upgraded), "dual_host_upgraded.sav not written"
        assert r["upgradedPath"].replace("\\", "/").endswith("dual_host_upgraded.sav")
        # the original is left byte-identical (never written) and still detects legacy
        assert open(os.path.join(xcom1, "dual_host.sav"), encoding="utf-8", newline="").read() == synth.dual_host(), \
            "the original save must be left byte-identical"
        assert gc.ok({"cmd": "upgrade_detect", "file": "dual_host.sav"})["kind"] == "legacy"

        # upgraded host file shape
        uh, ub = two_docs(upgraded)
        assert uh["coop"] is True, "upgraded header coop != true"
        assert uh["coopPlayers"] == ["HostGuy", "Carol"], uh.get("coopPlayers")
        assert uh["saveSchema"] == 2, uh.get("saveSchema")
        assert uh["name"].endswith("(upgraded)"), f"upgraded name not suffixed: {uh['name']}"
        # upgraded saves are always SEPARATE - the shared-world feature postdates them
        assert uh["coopCampaignType"] == 0, uh.get("coopCampaignType")
        assert ub["coop_save_owner_player_id"] == 0, ub.get("coop_save_owner_player_id")
        assert ub["saveID"] != 0 and "saveID" in ub, "upgraded body missing saveID"
        assert len(ub["coopClientSaves"]) == 1, ub.get("coopClientSaves")
        # host soldiers stamped owner 0; empty/missing coopname filled from name
        hs = ub["bases"][0]["soldiers"]
        assert all(s["ownerplayerid"] == 0 for s in hs), [s.get("ownerplayerid") for s in hs]
        assert {s["coopname"] for s in hs} == {"Alice", "Bravo"}, [s.get("coopname") for s in hs]
        # embedded client blob decodes to a 2-doc stream: owner 1 + the client base
        entry = ub["coopClientSaves"][0]
        assert entry["key"] == f"host_{ub['saveID']}_Carol.data", entry["key"]
        cdocs = list(yaml.safe_load_all(base64.b64decode(entry["blob"]).decode("utf-8")))
        assert len(cdocs) == 2, "client blob is not a 2-doc stream"
        ch, cb = cdocs
        assert ch["coop"] is True and ch["saveSchema"] == 2, "client header not stamped"
        assert ch["coopCampaignType"] == 0, ch.get("coopCampaignType")
        assert cb["coop_save_owner_player_id"] == 1, cb.get("coop_save_owner_player_id")
        assert cb["saveID"] == ub["saveID"], "client saveID must match host"
        assert cb["bases"][0]["name"] == "ClientBase", "client world/base not embedded"
        assert all(s["ownerplayerid"] == 1 for s in cb["bases"][0]["soldiers"]), "client soldiers not stamped owner 1"
        # re-detecting the upgraded file now reports current
        assert gc.ok({"cmd": "upgrade_detect", "file": upgname("dual_host.sav")})["kind"] == "current"
        print("PASS dual e2e: original untouched, upgraded shape + embedded client world correct")

        # ---- 4. embed + sidecar recovery (test_legacy_migration.py intent) ---
        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "embed_host.sav"})
        assert r["success"] is True, f"embed upgrade failed: {r}"
        eh, eb = two_docs(outp("embed_host.sav"))
        assert eb["coop_save_owner_player_id"] == 0 and len(eb["coopClientSaves"]) == 1
        assert "coopClientSaveKey" not in eb and "coopClientSaveBlob" not in eb, "embed leftovers not removed"
        ec = list(yaml.safe_load_all(base64.b64decode(eb["coopClientSaves"][0]["blob"]).decode()))[1]
        assert ec["coop_save_owner_player_id"] == 1, "embed client not tagged owner 1"
        assert eb["coopClientSaves"][0]["key"].endswith("_Bob.data"), eb["coopClientSaves"][0]["key"]
        print("PASS embed recovery: client world de-embedded + re-embedded as coopClientSaves")

        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "sidecar_host.sav"})
        assert r["success"] is True, f"sidecar upgrade failed: {r}"
        sh, sb = two_docs(outp("sidecar_host.sav"))
        assert len(sb["coopClientSaves"]) == 1, "sidecar client not imported"
        assert sb["coopClientSaves"][0]["key"].endswith("_Carol.data"), sb["coopClientSaves"][0]["key"]
        sc = list(yaml.safe_load_all(base64.b64decode(sb["coopClientSaves"][0]["blob"]).decode()))[1]
        assert sc["bases"][0]["name"] == "ClientBase", "sidecar client world not embedded"
        # the read-only sidecar .data is never modified/deleted (PRD 5)
        assert os.path.exists(os.path.join(xcom1, synth.sidecar_data_name())), "sidecar .data must be left intact"
        print("PASS sidecar recovery: host_<id>_<name>.data imported, source file left intact")

        # ---- 5a. mid-battle upgrade: keep host battle, drop client's ---------
        # The battle format is unchanged since 1.8.4 and the current build rehydrates
        # the client from the host's battleGame on resume (LoadGameState setBattleGame(0)),
        # so the upgrade keeps the host's battleGame and strips the client's redundant one.
        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "dual_host_battle.sav",
                   "client": "dual_client_battle.sav", "clientName": "Carol", "hostName": "HostGuy"})
        assert r["success"] is True, f"mid-battle upgrade should succeed: {r}"
        assert os.path.exists(outp("dual_host_battle.sav")), "dual_host_battle_upgraded.sav must be written"
        uh, ub = two_docs(outp("dual_host_battle.sav"))
        assert "battleGame" in ub, "host battleGame (the single battle authority) must be kept"
        entry = ub["coopClientSaves"][0]
        cb2 = list(yaml.safe_load_all(base64.b64decode(entry["blob"]).decode("utf-8")))[1]
        assert "battleGame" not in cb2, "client battleGame must be stripped (rehydrated from host)"
        assert any("host battle kept" in l for l in r["report"]), r["report"]
        assert any("client battle snapshot" in l for l in r["report"]), r["report"]
        assert gc.ok({"cmd": "upgrade_detect", "file": upgname("dual_host_battle.sav")})["kind"] == "current"
        print("PASS mid-battle: host battle kept, client battle dropped, loads as current")

        # ---- 5b. negatives --------------------------------------------------

        reset_fixtures()
        r = gc.cmd({"cmd": "upgrade_run", "host": "dual_host.sav",
                    "client": "dual_client_mode2.sav", "clientName": "Carol"})
        assert r["success"] is False and any("game mode" in e for e in r["errors"]), r
        print("PASS negative: gamemode mismatch refused")

        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "dual_host.sav", "skip": True})
        assert r["success"] is True, f"skip-client upgrade failed: {r}"
        assert any("restart fresh" in w for w in r["warnings"]), r["warnings"]
        _, kb = two_docs(outp("dual_host.sav"))
        assert "coopClientSaves" not in kb or kb["coopClientSaves"] in (None, []), kb.get("coopClientSaves")
        print("PASS skip-client: warns + upgraded host carries 0 client worlds")

        # ---- 6. detector v2 STRONG-marker save + transform hardening ---------
        # The strong pair is shaped like the real-world 1.7.0 save: no saveID,
        # coop_gamemode 0, real cross-instance links buried in the body (dead
        # soldier, craft coopItems + coopDestUfoId, ufo coopUfoId). The transform
        # must reset every stale link on BOTH sides while preserving coopname.
        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "dual_host_strong.sav",
                   "client": "dual_client_strong.sav", "clientName": "Carol", "hostName": "HostGuy"})
        assert r["success"] is True, f"strong upgrade failed: {r}"
        assert r["errors"] == [], r["errors"]
        uh, ub = two_docs(outp("dual_host_strong.sav"))
        # host living soldiers: stamped owner 0, coopname preserved (defaults untouched)
        hs = ub["bases"][0]["soldiers"]
        assert all(s["ownerplayerid"] == 0 for s in hs), [s.get("ownerplayerid") for s in hs]
        assert {s["coopname"] for s in hs} == {"Alice", "Bravo"}, [s.get("coopname") for s in hs]
        # dead soldier's strong cross-instance links reset; coopname kept
        dead = ub["deadSoldiers"][0]
        assert dead["coop"] == 0 and dead["coopbase"] == -1 and dead["coopcraft"] == -1, dead
        assert dead["coopcrafttype"] in ("", None), dead.get("coopcrafttype")
        assert dead["coopname"] == "Elliott Kay", "coopname must be preserved through the reset"
        # craft: peer-item cache removed, cross-instance destination zeroed
        craft = ub["bases"][0]["crafts"][0]
        assert "coopItems" not in craft, "host craft coopItems must be removed"
        assert craft.get("coopDestUfoId", 0) == 0, craft.get("coopDestUfoId")
        # ufo cross-instance id zeroed
        assert ub["ufos"][0].get("coopUfoId", 0) == 0, ub["ufos"][0].get("coopUfoId")
        # embedded client world: soldier links reset (owner 1), craft coopItems gone
        entry = ub["coopClientSaves"][0]
        cb = list(yaml.safe_load_all(base64.b64decode(entry["blob"]).decode("utf-8")))[1]
        cs = cb["bases"][0]["soldiers"][0]
        assert cs["ownerplayerid"] == 1, cs.get("ownerplayerid")
        assert cs["coop"] == 0 and cs["coopbase"] == -1 and cs["coopcraft"] == -1, cs
        assert cs["coopcrafttype"] in ("", None), cs.get("coopcrafttype")
        assert cs["coopname"] == "Carol", "client coopname must be preserved"
        assert "coopItems" not in cb["bases"][0]["crafts"][0], "client craft coopItems must be removed"
        # the report calls out the resets with counts
        assert any("Reset stale co-op links" in l for l in r["report"]), r["report"]
        # and the upgraded strong save now reads as current
        assert gc.ok({"cmd": "upgrade_detect", "file": upgname("dual_host_strong.sav")})["kind"] == "current"
        print("PASS strong e2e: strong-marker detection + host/client link resets + coopname preserved")

        # ---- 6b. peer-mirror soldiers dropped, not duplicated ---------------
        # The host holds a coop:1 MIRROR of the client's soldier Carol; the client
        # holds the real Carol. The upgrade must DROP the mirror (real copy lives in
        # the client world) so there is no duplicate-coopname warning and no phantom
        # host Carol - the engine rebuilds the mirror from the client blob on resume.
        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "dual_host_mirror.sav",
                   "client": "dual_client.sav", "clientName": "Carol", "hostName": "HostGuy"})
        assert r["success"] is True, f"mirror upgrade should succeed: {r}"
        assert not any("Duplicate soldier co-op name" in w for w in r["warnings"]), \
            f"mirror must not raise a duplicate-coopname warning: {r['warnings']}"
        assert any("peer-mirror soldier" in l for l in r["report"]), r["report"]
        uh, ub = two_docs(outp("dual_host_mirror.sav"))
        hnames = {s.get("coopname") or s.get("name") for s in ub["bases"][0]["soldiers"]}
        assert hnames == {"Alice", "Bravo"}, f"peer mirror must be dropped from host: {hnames}"
        # the client's real Carol is preserved in the embedded world
        cb = list(yaml.safe_load_all(base64.b64decode(ub["coopClientSaves"][0]["blob"]).decode("utf-8")))[1]
        cnames = {s.get("coopname") or s.get("name") for base in cb["bases"] for s in base.get("soldiers", [])}
        assert "Carol" in cnames, f"client's real Carol must be preserved: {cnames}"
        print("PASS mirror-drop: peer mirror removed from host, real copy kept in client, no dup warning")

        # ---- 6c. mid-battle mirror KEPT (crash regression) ------------------
        # A mid-battle host's coop:1 mirror is a live battle unit (battleGame references
        # it by soldier id, SavedBattleGame.cpp:268). It must be KEPT (not dropped -> the
        # load would crash) and left pristine (coop=1, owner 999), NOT laundered.
        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "dual_host_battle_mirror.sav",
                   "client": "dual_client.sav", "clientName": "Carol", "hostName": "HostGuy"})
        assert r["success"] is True, f"mid-battle mirror upgrade should succeed: {r}"
        assert not any("Duplicate soldier co-op name" in w for w in r["warnings"]), r["warnings"]
        assert not any("Dropped" in l and "peer-mirror" in l for l in r["report"]), \
            f"a mid-battle mirror must be KEPT, not dropped: {r['report']}"
        uh, ub = two_docs(outp("dual_host_battle_mirror.sav"))
        assert "battleGame" in ub, "host battleGame must be kept"
        carol = [s for s in ub["bases"][0]["soldiers"] if (s.get("coopname") or s.get("name")) == "Carol"]
        assert carol, "mid-battle mirror must remain in the host roster (its BattleUnit links to it)"
        assert carol[0].get("coop") == 1, "kept mirror must stay coop=1 (not laundered to 0)"
        assert carol[0].get("ownerplayerid", 999) == 999, "kept mirror must not be tagged owner 0"
        print("PASS mid-battle mirror: kept (coop=1, owner 999), not dropped, not laundered")

        # ---- 7. F6: SavedGame::load defensive schema throw -------------------
        # A legacy save handed straight to SavedGame::load (bypassing the gate) must
        # throw the clear "older version" refusal - the safety net for a legacy save
        # that reaches load without the LoadGameState gate.
        reset_fixtures()
        r = gc.ok({"cmd": "load_raw", "file": "dual_host_strong.sav"})
        assert r["threw"] is True, "legacy save must throw in SavedGame::load"
        assert "older version" in r.get("message", ""), r.get("message")
        print("PASS defensive throw: legacy save refused by SavedGame::load")

        print("ALL PASS test_save_upgrade")
    finally:
        gc.shutdown()


if __name__ == "__main__":
    main()
