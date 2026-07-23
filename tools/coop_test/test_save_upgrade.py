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
- runner e2e on the DUAL pair: backup naming + original preserved, upgraded shape
  (header coop/coopPlayers/saveSchema:2; body saveID/owner 0/coopClientSaves[1]
  whose blob decodes to owner 1 + client base; soldiers stamped both sides)
- embed + sidecar recovery (ports test_legacy_migration.py's intent to the new flow)
- negatives: mid-battle refused, gamemode mismatch refused, skip-client warns + 0 saves

Run:  python tools/coop_test/test_save_upgrade.py
"""

import base64
import os
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
        synth.write_all(xcom1)

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
            "dual_host.sav":        ("legacy", "dual", True),
            "dual_host_saveid.sav": ("legacy", "dual", True),
            "dual_host_strong.sav": ("legacy", "dual", True),   # detector v2: STRONG deep-scan markers
            "weak_only.sav":        ("ambiguous_build", "none", False),  # WEAK-only -> ask the player
            "vanilla_solo.sav":     ("solo", "none", False),    # pure OXCE shape, zero coop keys
            "embed_host.sav":       ("legacy", "embed", True),
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
        # backup: <stem>_bak_v1.sav, and the original is preserved inside it
        backup = os.path.join(xcom1, "dual_host_bak_v1.sav")
        assert os.path.exists(backup), "backup <stem>_bak_v1.sav not written"
        assert r["backupPath"].replace("\\", "/").endswith("dual_host_bak_v1.sav")
        bh, bb = two_docs(backup)
        assert bh["name"].endswith("(pre-upgrade backup)"), f"backup name not suffixed: {bh['name']}"
        assert "coop" not in bh and "saveSchema" not in bh, "backup body must be the untouched legacy original"
        assert bb["coop_gamemode"] == 1 and "coopClientSaves" not in bb, "backup body must be byte-identical legacy"
        # a re-detect of the backup still classifies it as the legacy original (restore path)
        assert gc.ok({"cmd": "upgrade_detect", "file": "dual_host_bak_v1.sav"})["kind"] == "legacy"

        # upgraded host file shape
        uh, ub = two_docs(os.path.join(xcom1, "dual_host.sav"))
        assert uh["coop"] is True, "upgraded header coop != true"
        assert uh["coopPlayers"] == ["HostGuy", "Carol"], uh.get("coopPlayers")
        assert uh["saveSchema"] == 2, uh.get("saveSchema")
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
        assert gc.ok({"cmd": "upgrade_detect", "file": "dual_host.sav"})["kind"] == "current"
        print("PASS dual e2e: backup preserved original, upgraded shape + embedded client world correct")

        # ---- 4. embed + sidecar recovery (test_legacy_migration.py intent) ---
        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "embed_host.sav"})
        assert r["success"] is True, f"embed upgrade failed: {r}"
        eh, eb = two_docs(os.path.join(xcom1, "embed_host.sav"))
        assert eb["coop_save_owner_player_id"] == 0 and len(eb["coopClientSaves"]) == 1
        assert "coopClientSaveKey" not in eb and "coopClientSaveBlob" not in eb, "embed leftovers not removed"
        ec = list(yaml.safe_load_all(base64.b64decode(eb["coopClientSaves"][0]["blob"]).decode()))[1]
        assert ec["coop_save_owner_player_id"] == 1, "embed client not tagged owner 1"
        assert eb["coopClientSaves"][0]["key"].endswith("_Bob.data"), eb["coopClientSaves"][0]["key"]
        print("PASS embed recovery: client world de-embedded + re-embedded as coopClientSaves")

        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "sidecar_host.sav"})
        assert r["success"] is True, f"sidecar upgrade failed: {r}"
        sh, sb = two_docs(os.path.join(xcom1, "sidecar_host.sav"))
        assert len(sb["coopClientSaves"]) == 1, "sidecar client not imported"
        assert sb["coopClientSaves"][0]["key"].endswith("_Carol.data"), sb["coopClientSaves"][0]["key"]
        sc = list(yaml.safe_load_all(base64.b64decode(sb["coopClientSaves"][0]["blob"]).decode()))[1]
        assert sc["bases"][0]["name"] == "ClientBase", "sidecar client world not embedded"
        # the read-only sidecar .data is never modified/deleted (PRD 5)
        assert os.path.exists(os.path.join(xcom1, synth.sidecar_data_name())), "sidecar .data must be left intact"
        print("PASS sidecar recovery: host_<id>_<name>.data imported, source file left intact")

        # ---- 5. negatives ---------------------------------------------------
        reset_fixtures()
        r = gc.cmd({"cmd": "upgrade_run", "host": "dual_host_battle.sav",
                    "client": "dual_client.sav", "clientName": "Carol"})
        assert r["success"] is False and any("mid-battle" in e for e in r["errors"]), r
        # nothing written: no backup, original still legacy
        assert not os.path.exists(os.path.join(xcom1, "dual_host_battle_bak_v1.sav")), "refused upgrade must not write a backup"
        assert gc.ok({"cmd": "upgrade_detect", "file": "dual_host_battle.sav"})["kind"] == "legacy"
        print("PASS negative: mid-battle save refused, nothing written")

        reset_fixtures()
        r = gc.cmd({"cmd": "upgrade_run", "host": "dual_host.sav",
                    "client": "dual_client_mode2.sav", "clientName": "Carol"})
        assert r["success"] is False and any("game mode" in e for e in r["errors"]), r
        print("PASS negative: gamemode mismatch refused")

        reset_fixtures()
        r = gc.ok({"cmd": "upgrade_run", "host": "dual_host.sav", "skip": True})
        assert r["success"] is True, f"skip-client upgrade failed: {r}"
        assert any("restart fresh" in w for w in r["warnings"]), r["warnings"]
        _, kb = two_docs(os.path.join(xcom1, "dual_host.sav"))
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
        uh, ub = two_docs(os.path.join(xcom1, "dual_host_strong.sav"))
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
        assert gc.ok({"cmd": "upgrade_detect", "file": "dual_host_strong.sav"})["kind"] == "current"
        print("PASS strong e2e: strong-marker detection + host/client link resets + coopname preserved")

        print("ALL PASS test_save_upgrade")
    finally:
        gc.shutdown()


if __name__ == "__main__":
    main()
