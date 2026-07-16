"""Synthetic legacy-save fixtures for the in-game Save Upgrader tests.

PROVENANCE: these saves are SYNTHESIZED to the documented legacy shapes, not
captured from a historical build. The exact mod v1.7.0 cut is a Discord release
with no mapped commit, and the schema-1 dual/embed/sidecar shapes are
structurally identical for the detector + framework's purposes (see
.agents/prds/save-upgrader.md sections 1, 3 and 9). Each fixture is a minimal but
well-formed two-document save ("header\\n---\\nbody"): enough real structure
(bases, soldiers, funds, gamemode, saveID) for the SchemaDetector and the raw-YAML
1->2 transform to exercise, without depending on a full SavedGame::load (the
upgrade framework never builds a SavedGame - see PRD 6.1). The full-flow test that
actually LOADS an upgraded save uses a real harness-generated campaign save
instead (see test_save_upgrade_flow.py); these are for detector + runner units.

Notes on shapes (PRD 3 detection table):
  dual          : no `coop` header, no `saveSchema`; body coop_gamemode != 0
  dual+saveID   : same, but coop_gamemode 0 and saveID != 0 with no sidecar
                  (the case the old build hard-threw on at SavedGame.cpp:820)
  embed         : body has coopClientSaveKey + base64 coopClientSaveBlob
  sidecar       : body saveID != 0, no embed, + a host_<saveID>_<name>.data file
  solo          : no co-op trace at all -> loads normally, stamped on next save
  current       : header saveSchema: 2 (+ coop: true)
  unknown-future: header saveSchema: 99 (> SAVE_SCHEMA_CURRENT)
  malformed     : not a two-document stream
"""

import base64
import os

# Fixed saveIDs so detection is deterministic and the sidecar/dual+saveID
# fixtures never collide (the detector scans the whole folder for
# host_<saveID>_*.data). Keep them distinct.
EMBED_SAVE_ID = 12340000
SIDECAR_SAVE_ID = 12340042
DUAL_SAVE_ID = 12349999  # dual variant carrying a saveID but no sidecar

SIDECAR_CLIENT_NAME = "Carol"
EMBED_CLIENT_NAME = "Bob"


def _header(name, *, schema=None, coop=False):
    lines = [
        "name: %s" % name,
        "version: Extended 8.4.2",
        "engine: Extended",
        "build: 8.4.2.0",
    ]
    if coop:
        lines.append("coop: true")
    if schema is not None:
        lines.append("saveSchema: %d" % schema)
    lines += [
        "time:",
        "  year: 1999",
        "  month: 1",
        "  day: 1",
        "  hour: 12",
        "  minute: 0",
        "  second: 0",
        "  weekday: 6",
        "mods:",
        "  - xcom1 ver: 1.0",
    ]
    return "\n".join(lines) + "\n"


def _host_body(*, gamemode=1, save_id=None, extra=None):
    """A host geoscape body: one base, two soldiers - the second deliberately
    carries NO coopname and NO ownerplayerid key (the transform must fill/stamp
    them). Per PRD 7."""
    lines = ["difficulty: 0", "monthsPassed: 0"]
    if gamemode is not None:
        lines.append("coop_gamemode: %d" % gamemode)
    if save_id is not None:
        lines.append("saveID: %d" % save_id)
    lines += [
        "funds:",
        "  - 1000000",
        "bases:",
        "  - name: HostBase",
        "    lon: 0.1",
        "    lat: 0.2",
        "    soldiers:",
        "      - type: STR_SOLDIER",
        "        id: 1",
        "        name: Alice",
        '        coopname: ""',   # present-but-empty coopname -> filled with name
        "      - type: STR_SOLDIER",
        "        id: 2",
        "        name: Bravo",     # no coopname key, no ownerplayerid key
    ]
    if extra:
        lines += extra
    return "\n".join(lines) + "\n"


def _client_body(*, gamemode=1, save_id=None):
    """A standalone client world: own base, own soldier, funds, research."""
    lines = ["difficulty: 0", "monthsPassed: 0"]
    if gamemode is not None:
        lines.append("coop_gamemode: %d" % gamemode)
    if save_id is not None:
        lines.append("saveID: %d" % save_id)
    lines += [
        "funds:",
        "  - 500000",
        "research:",
        "  - name: STR_MEDI_KIT",
        "    discovered: true",
        "bases:",
        "  - name: ClientBase",
        "    lon: 0.5",
        "    lat: 0.6",
        "    soldiers:",
        "      - type: STR_SOLDIER",
        "        id: 10",
        "        name: Carol",
    ]
    return "\n".join(lines) + "\n"


def _stream(header, body):
    return header + "---\n" + body


def client_stream(name="Carol", gamemode=1, save_id=None):
    """A full two-document client save stream (used raw for a sidecar .data
    file and base64-embedded for the embed variant)."""
    return _stream(_header(name + "-client"), _client_body(gamemode=gamemode, save_id=save_id))


# ---- individual fixtures (name -> two-document text) --------------------------

def dual_host():
    return _stream(_header("Dual Coop"), _host_body(gamemode=1, save_id=None))


def dual_host_saveid():
    # coop_gamemode 0 so ONLY the non-zero saveID (with no sidecar) drives the
    # dual classification - the previously-hard-throw case (PRD 3).
    return _stream(_header("Dual SaveID"), _host_body(gamemode=0, save_id=DUAL_SAVE_ID))


def dual_host_battle():
    # A mid-battle dual host: preflight must refuse it (PRD 7).
    return _stream(_header("Dual Battle"),
                   _host_body(gamemode=1, save_id=None, extra=["battleGame:", "  turn: 3"]))


def dual_client(gamemode=1):
    return _stream(_header("Dual Client"), _client_body(gamemode=gamemode))


def dual_client_mode2():
    # gamemode mismatch vs a mode-1 host -> blocking error.
    return _stream(_header("Dual Client M2"), _client_body(gamemode=2))


def embed_host():
    blob = base64.b64encode(client_stream(EMBED_CLIENT_NAME, gamemode=1).encode("utf-8")).decode("ascii")
    extra = [
        "coopClientSaveKey: host_%d_%s.data" % (EMBED_SAVE_ID, EMBED_CLIENT_NAME),
        "coopClientSaveBlob: %s" % blob,
    ]
    return _stream(_header("Embed Coop"),
                   _host_body(gamemode=1, save_id=EMBED_SAVE_ID, extra=extra))


def sidecar_host():
    return _stream(_header("Sidecar Coop"), _host_body(gamemode=1, save_id=SIDECAR_SAVE_ID))


def sidecar_data():
    # Raw two-document client stream (sidecar .data is read raw, NOT base64).
    return client_stream(SIDECAR_CLIENT_NAME, gamemode=1)


def sidecar_data_name():
    return "host_%d_%s.data" % (SIDECAR_SAVE_ID, SIDECAR_CLIENT_NAME)


def current():
    body = _host_body(gamemode=1, save_id=20260716120000)
    body += "coop_save_owner_player_id: 0\nno_bases: false\n"
    return _stream(_header("Current Coop", schema=2, coop=True), body)


def future():
    return _stream(_header("Future Save", schema=99), _host_body(gamemode=1, save_id=None))


def malformed():
    # A single document with no '---' separator: the detector cannot get a body.
    return "this: is\nnot: a\ntwo: document\nsave: file\n"


def solo():
    body = "\n".join([
        "difficulty: 0",
        "monthsPassed: 0",
        "funds:",
        "  - 1000000",
        "bases:",
        "  - name: SoloBase",
        "    soldiers:",
        "      - type: STR_SOLDIER",
        "        id: 1",
        "        name: Solo Guy",
    ]) + "\n"
    return _stream(_header("Solo Game"), body)


# name -> builder. .sav files plus the one sidecar .data.
FILES = {
    "dual_host.sav": dual_host,
    "dual_host_saveid.sav": dual_host_saveid,
    "dual_host_battle.sav": dual_host_battle,
    "dual_client.sav": dual_client,
    "dual_client_mode2.sav": dual_client_mode2,
    "embed_host.sav": embed_host,
    "sidecar_host.sav": sidecar_host,
    "solo.sav": solo,
    "current.sav": current,
    "future.sav": future,
    "malformed.sav": malformed,
}


def write_all(dest_dir):
    """Write every fixture (plus the sidecar .data) into dest_dir. Returns the
    list of filenames written."""
    os.makedirs(dest_dir, exist_ok=True)
    written = []
    for fname, builder in FILES.items():
        with open(os.path.join(dest_dir, fname), "w", encoding="utf-8", newline="\n") as f:
            f.write(builder())
        written.append(fname)
    # the sidecar's client world lives beside sidecar_host.sav
    with open(os.path.join(dest_dir, sidecar_data_name()), "w", encoding="utf-8", newline="\n") as f:
        f.write(sidecar_data())
    written.append(sidecar_data_name())
    return written


if __name__ == "__main__":
    # Regenerate the committed snapshot next to this module.
    here = os.path.dirname(os.path.abspath(__file__))
    names = write_all(here)
    print("wrote %d fixtures into %s" % (len(names), here))
    for n in names:
        print("  ", n)
