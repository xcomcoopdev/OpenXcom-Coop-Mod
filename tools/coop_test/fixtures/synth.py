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

Notes on shapes (PRD 3.1 detection table, detector v2.3):
  dual          : no `coop` header, no `saveSchema`; a genuine per-object co-op link
                  (soldier coop!=0 / coopbase!=-1 / craft coopItems / coopDestUfoId)
  solo+saveID   : coop_gamemode 0 and saveID != 0 with NO sidecar and no strong
                  marker -> Solo (saveID alone is not a co-op signal)
  solo-coopbuild: coop_gamemode/coopname/coopbaseid/debugCoopMenu + default soldier
                  coop fields (fork solo save) -> Solo
  solo-with-ufo : random per-object coopUfoId/coopMissionId noise, no strong marker
                  -> Solo (the worst false-positive the old detector produced)
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


def _host_body(*, gamemode=1, save_id=None, extra=None, strong=False, extra_soldiers=None):
    """A host geoscape body: one base, two soldiers - the second deliberately
    carries NO coopname and NO ownerplayerid key (the transform must fill/stamp
    them). Per PRD 7.

    strong=True gives the first soldier a genuine cross-instance link: coopbase !=
    -1 on a REAL soldier (coop stays 0). Under detector v2.3 that non-default
    per-object value is the ONLY thing that classifies a save Legacy/Dual -
    coop_gamemode and saveID are no longer co-op signals. NB it uses coopbase (not
    coop:1) so the marker soldier is a real host soldier, NOT a droppable peer
    mirror - the transform must keep it. extra_soldiers injects raw soldier lines
    into the base's soldiers list (e.g. a coop:1 peer mirror for drop tests)."""
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
    ]
    if strong:
        lines += [
            "        coopbase: 4501",  # real soldier staged on a coop craft -> STRONG marker
        ]
    lines += [
        "      - type: STR_SOLDIER",
        "        id: 2",
        "        name: Bravo",     # no coopname key, no ownerplayerid key
    ]
    if extra_soldiers:
        lines += extra_soldiers
    if extra:
        lines += extra
    return "\n".join(lines) + "\n"


def _client_body(*, gamemode=1, save_id=None, extra=None):
    """A standalone client world: own base, own soldier, funds, research."""
    lines = ["difficulty: 0", "monthsPassed: 0"]
    if gamemode is not None:
        lines.append("coop_gamemode: %d" % gamemode)
    if save_id is not None:
        lines.append("saveID: %d" % save_id)
    if extra:
        lines += extra
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
    # A real legacy dual host: a soldier carries a genuine peer link (STRONG marker),
    # so detector v2.3 classifies it Legacy/Dual. The base for the runner e2e.
    return _stream(_header("Dual Coop"), _host_body(gamemode=1, save_id=None, strong=True))


def solo_saveid():
    # A SOLO fork save that happens to carry a non-zero saveID but has NO sidecar and
    # NO strong marker. Under detector v2.3 saveID is only the input to the sidecar
    # lookup, never a co-op signal on its own -> Solo (was the old hard-throw case).
    return _stream(_header("Solo SaveID"), _host_body(gamemode=0, save_id=DUAL_SAVE_ID))


def dual_host_battle():
    # A mid-battle dual host (STRONG marker so it gates). The upgrade KEEPS this
    # battleGame (the host is the single battle authority; the client is rehydrated
    # from it on resume - LoadGameState setBattleGame(0)).
    return _stream(_header("Dual Battle"),
                   _host_body(gamemode=1, save_id=None, strong=True, extra=["battleGame:", "  turn: 3"]))


def dual_client_battle():
    # A mid-battle client world. The upgrade STRIPS this battleGame (redundant; the
    # client's battle comes from the host's battlehost stream on resume).
    return _stream(_header("Dual Battle Client"),
                   _client_body(gamemode=1, extra=["battleGame:", "  turn: 3"]))


def dual_host_mirror():
    # Host with its own soldiers (Alice, Bravo) PLUS a coop:1 MIRROR of the client's
    # soldier Carol (owner 999 -> not a transferred guest). The upgrade must DROP the
    # mirror (Carol's real copy lives in the client world), so: no duplicate-coopname
    # warning, no phantom host Carol. Pair with dual_client.sav (has the real Carol).
    mirror = [
        "      - type: STR_SOLDIER",
        "        id: 99",
        "        name: Carol",
        "        coopname: Carol",
        "        coop: 1",
        "        coopbase: 4501",
    ]
    return _stream(_header("Dual Mirror"),
                   _host_body(gamemode=1, save_id=None, extra_soldiers=mirror))


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


def embed_no_blob():
    # A stray coopClientSaveKey with NO coopClientSaveBlob and no strong marker: not
    # a recoverable embed. The detector keys Embed on the BLOB (what ingest reads),
    # so this must classify Solo, not dead-end demanding a client name (F2).
    extra = ["coopClientSaveKey: host_%d_%s.data" % (EMBED_SAVE_ID, EMBED_CLIENT_NAME)]
    return _stream(_header("Embed No Blob"),
                   _host_body(gamemode=1, save_id=0, extra=extra))


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


# ---- detector v2.3 fixtures (strong / solo-coopbuild / vanilla) ---------------
# Shaped after the real-world fork co-op save (see PRD 3.1). The fork serializer
# wrote every co-op key unconditionally, so DEFAULT values (coop 0, coopbase -1,
# coopcraft -1, coopcrafttype '') mean nothing; only NON-DEFAULT per-object link
# values are STRONG markers. The always-written keys (coop_gamemode, per-soldier
# coopname, a random base coopbaseid, a random per-object coopUfoId/coopMissionId,
# the debugCoopMenu option) appear in pure SOLO saves too and are NOT co-op signals:
# a save carrying only those must classify Solo and load untouched.

# A living soldier exactly as the era wrote it: all coop keys present but DEFAULT,
# coopname non-empty. On its own this is a WEAK marker only.
def _living_soldier(sid, name):
    return [
        "      - type: STR_SOLDIER",
        "        id: %d" % sid,
        "        name: %s" % name,
        "        coopbase: -1",
        "        coop: 0",
        "        coopcraft: -1",
        "        coopcrafttype: ''",
        "        coopname: %s" % name,
    ]


# A dead soldier carrying real cross-instance links (STRONG markers), sitting in
# the top-level deadSoldiers[] list - proving the detector/transform must scan the
# WHOLE body, not just bases[].soldiers[].
def _dead_soldier_strong(sid, name, coopbase):
    return [
        "  - type: STR_SOLDIER",
        "    id: %d" % sid,
        "    name: %s" % name,
        "    coopbase: %d" % coopbase,
        "    coop: 1",
        "    coopcraft: 1",
        "    coopcrafttype: STR_SKYRANGER",
        "    coopname: %s" % name,
        "    ownerplayerid: 999",
    ]


def _solo_coopbuild_body(base_id=2032):
    """A pure SOLO save written by the co-op-capable fork build: coop_gamemode
    present (=0), non-empty coopname on every soldier, a non-zero (random) base
    coopbaseid, and a debugCoopMenu option - every soldier coop link at its default.
    None of these is a co-op signal under detector v2.3 -> Solo, loads untouched."""
    lines = ["difficulty: 0", "monthsPassed: 0", "coop_gamemode: 0", "funds:", "  - 1000000",
             "bases:", "  - name: HostBase", "    lon: 0.1", "    lat: 0.2",
             "    coopbaseid: %d" % base_id, "    soldiers:"]
    lines += _living_soldier(1, "Alice")
    lines += _living_soldier(2, "Bravo")
    lines += ["options:", "  debugCoopMenu: false"]
    return "\n".join(lines) + "\n"


def _strong_host_body(base_id=2032):
    """A host body that ALSO carries strong markers: a dead soldier bound to a peer
    base/craft, a craft with a coopItems peer-item cache + a coopDestUfoId, and a
    ufo with a coopUfoId. -> Legacy/Dual. The transform must reset every one of
    these (and leave coopname + the living soldiers' ownerplayerid stamps)."""
    lines = ["difficulty: 0", "monthsPassed: 0", "coop_gamemode: 0", "funds:", "  - 1000000",
             "bases:", "  - name: HostBase", "    lon: 0.1", "    lat: 0.2",
             "    coopbaseid: %d" % base_id, "    soldiers:"]
    lines += _living_soldier(1, "Alice")
    lines += _living_soldier(2, "Bravo")
    lines += [
        "    crafts:",
        "      - type: STR_SKYRANGER",
        "        id: 1",
        "        fuel: 1375",
        "        coopDestUfoId: 5001",
        "        coopItems:",
        "          - {id: 2, name: STR_RIFLE, owner: true}",
        "          - {id: 3, name: STR_RIFLE, owner: true}",
    ]
    lines += ["deadSoldiers:"]
    lines += _dead_soldier_strong(49, "Elliott Kay", base_id)
    lines += [
        "ufos:",
        "  - type: STR_SMALL_SCOUT",
        "    id: 7",
        "    coopUfoId: 9001",
    ]
    lines += ["options:", "  debugCoopMenu: false"]
    return "\n".join(lines) + "\n"


def _strong_client_body():
    """A standalone client world (gamemode 0 to match the strong host) whose LIVING
    soldier is the client's OWN soldier staged on the shared coop craft: coop=0
    (NOT a mirror, so it survives) with stale coopbase/coopcraft/coopcrafttype links
    the transform must reset, and a craft coopItems cache the transform must strip -
    matching the real-world client save's shape."""
    lines = ["difficulty: 0", "monthsPassed: 0", "coop_gamemode: 0", "funds:", "  - 500000",
             "bases:", "  - name: ClientBase", "    lon: 0.5", "    lat: 0.6",
             "    coopbaseid: 3050", "    soldiers:",
             "      - type: STR_SOLDIER",
             "        id: 10",
             "        name: Carol",
             "        coopbase: 2032",
             "        coop: 0",
             "        coopcraft: 1",
             "        coopcrafttype: STR_SKYRANGER",
             "        coopname: Carol",
             "    crafts:",
             "      - type: STR_INTERCEPTOR",
             "        id: 2",
             "        coopItems:",
             "          - {id: 20, name: STR_LASER, owner: false}"]
    return "\n".join(lines) + "\n"


def dual_host_strong():
    # no saveID, coop_gamemode 0: the OLD fingerprints do not fire; only the deep
    # STRONG-marker scan classifies this Legacy/Dual (the real-world bug case).
    return _stream(_header("Strong Coop"), _strong_host_body())


def dual_client_strong():
    return _stream(_header("Strong Client"), _strong_client_body())


def solo_coopbuild():
    # a solo save from the co-op-capable fork build (coop keys present but all at
    # default / random-noise values) -> Solo, must keep loading untouched.
    return _stream(_header("Solo Coop-Build"), _solo_coopbuild_body())


def solo_with_ufo():
    """The WORST-BUG regression (detector v2.3): a fork SOLO save with a UFO and a
    mission site on the geoscape. The fork build stamped a RANDOM non-zero coopUfoId
    on every ufo and coopMissionId on every mission site unconditionally, and the old
    detector treated those as STRONG markers - so any solo save with a UFO in flight
    was forced into the co-op upgrade dialog. No genuine cross-instance link anywhere
    -> MUST classify Solo and load untouched."""
    lines = ["difficulty: 1", "monthsPassed: 2", "coop_gamemode: 0", "funds:", "  - 1500000",
             "bases:", "  - name: SoloBase", "    lon: 0.1", "    lat: 0.2",
             "    coopbaseid: 8123", "    soldiers:"]
    lines += _living_soldier(1, "Alice")   # default coop link fields -> no strong marker
    lines += _living_soldier(2, "Bravo")
    lines += [
        "ufos:",
        "  - type: STR_SMALL_SCOUT",
        "    id: 3",
        "    coopUfoId: 74213",       # RANDOM per-object noise, NOT a co-op signal
        "  - type: STR_MEDIUM_SCOUT",
        "    id: 4",
        "    coopUfoId: 9981",
        "missionSites:",
        "  - type: STR_ALIEN_TERROR",
        "    id: 5",
        "    coopMissionId: 55207",   # RANDOM per-object noise, NOT a co-op signal
    ]
    lines += ["options:", "  debugCoopMenu: false"]
    return _stream(_header("Solo With UFO"), "\n".join(lines) + "\n")


def vanilla_solo():
    # pure OXCE shape: not a single coop key -> Solo, must keep loading untouched.
    body = "\n".join([
        "difficulty: 2",
        "monthsPassed: 3",
        "funds:",
        "  - 2000000",
        "bases:",
        "  - name: Vanilla Base",
        "    lon: 0.3",
        "    lat: 0.4",
        "    soldiers:",
        "      - type: STR_SOLDIER",
        "        id: 1",
        "        name: Jane Doe",
        "      - type: STR_SOLDIER",
        "        id: 2",
        "        name: John Roe",
        "    crafts:",
        "      - type: STR_SKYRANGER",
        "        id: 1",
        "        fuel: 1375",
    ]) + "\n"
    return _stream(_header("Vanilla Solo"), body)


# name -> builder. .sav files plus the one sidecar .data.
FILES = {
    "dual_host.sav": dual_host,
    "solo_saveid.sav": solo_saveid,
    "dual_host_battle.sav": dual_host_battle,
    "dual_host_strong.sav": dual_host_strong,
    "dual_client.sav": dual_client,
    "dual_client_mode2.sav": dual_client_mode2,
    "dual_client_strong.sav": dual_client_strong,
    "dual_client_battle.sav": dual_client_battle,
    "dual_host_mirror.sav": dual_host_mirror,
    "solo_coopbuild.sav": solo_coopbuild,
    "solo_with_ufo.sav": solo_with_ufo,
    "vanilla_solo.sav": vanilla_solo,
    "embed_host.sav": embed_host,
    "embed_no_blob.sav": embed_no_blob,
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
