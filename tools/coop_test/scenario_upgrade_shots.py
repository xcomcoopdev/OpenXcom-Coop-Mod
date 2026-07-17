"""Capture a screenshot of every Save-Upgrader UI state for visual QA.

Stages the synthetic legacy fixtures into one hermetic instance, then shoots:
  01 gate dialog     - via the REAL load path (load_save_menu -> schema gate)
  01b ambiguous gate - via the REAL load path on a weak-only save (3-way choice)
  02 client input    - upgrade_show state=client (candidate picker + name fields)
  03 info message    - upgrade_show state=info (blocking-refusal, single OK)
  04 confirm message - upgrade_show state=confirm (warnings, Continue anyway/Cancel)
  05 summary         - upgrade_show state=summary (REAL runner report + backup name)

The battlescape-themed variant (origin=OPT_BATTLESCAPE, applyBattlescapeTheme) is
NOT shot here: applyBattlescapeTheme swaps the window to TAC00.SCR, which only
renders correctly under the battlescape palette a LIVE battle installs. Pushed over
the menu (geoscape palette) it comes out as palette garbage - misleading, not a
real defect. The `upgrade_show ... origin=battlescape` command still supports it for
capture from within an actual battle; the harness has no cheap path to one.

The synthetic pushes use the same constructor args the real flow passes
(LoadGameState gate / SaveUpgradeUI); the data is real (a detected schema from a
staged fixture, the engine's own warning strings, and for the summary the actual
UpgradeRunner::execute() report over the dual fixture pair).

Usage: python tools/coop_test/scenario_upgrade_shots.py <outdir>
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures"))

from harness import GameClient, make_user_dir
import session
import synth

PORT = 48760
SETTLE = 1.2  # let the POPUP_BOTH open animation finish before the screenshot


def main(outdir):
    os.makedirs(outdir, exist_ok=True)
    d = make_user_dir("upgrade_shots")
    xcom1 = os.path.join(d, "xcom1")
    synth.write_all(xcom1)

    gc = GameClient("host", PORT, d)
    try:
        gc.spawn()
        gc.connect()

        def shot(name):
            time.sleep(SETTLE)
            path = os.path.join(outdir, name)
            gc.ok({"cmd": "screenshot", "path": path})
            print("saved", path)

        def reset():
            gc.ok({"cmd": "pop_state"})
            time.sleep(0.5)

        # 01 gate dialog via the REAL load gate (LoadGameState schema classifier)
        gc.ok({"cmd": "load_save_menu", "file": "dual_host.sav"})
        gc.wait_for("gate dialog", lambda: session.has_state(gc, "SaveUpgradeDialogState"))
        shot("01_gate_dialog.png")
        reset()

        # 01b ambiguous-build 3-way choice, via the REAL gate on a weak-only save
        gc.ok({"cmd": "load_save_menu", "file": "weak_only.sav"})
        gc.wait_for("ambiguous dialog", lambda: session.has_state(gc, "SaveUpgradeAmbiguousState"))
        shot("01b_ambiguous_gate.png")
        reset()

        # 02 client input picker (dense: list + two name fields + 4 buttons).
        # The names are prefilled: an empty TextEdit draws nothing, so without them
        # the shot cannot show the fields' placement.
        gc.ok({"cmd": "upgrade_show", "state": "client", "host": "dual_host.sav",
               "clientName": "Player1", "hostName": "Player2"})
        gc.wait_for("client state", lambda: session.has_state(gc, "SaveUpgradeClientState"))
        shot("02_client_input.png")
        reset()

        # 03 info message (blocking refusal, single OK)
        gc.ok({"cmd": "upgrade_show", "state": "info"})
        gc.wait_for("info state", lambda: session.has_state(gc, "SaveUpgradeMessageState"))
        shot("03_msg_info.png")
        reset()

        # 04 confirm message (non-blocking warnings, Continue anyway / Cancel)
        gc.ok({"cmd": "upgrade_show", "state": "confirm", "host": "dual_host.sav"})
        gc.wait_for("confirm state", lambda: session.has_state(gc, "SaveUpgradeMessageState"))
        shot("04_msg_confirm.png")
        reset()

        # 05 summary: run the REAL runner over the dual pair. execute() overwrites
        #    dual_host.sav + writes a backup, so restage a fresh legacy fixture first.
        synth.write_all(xcom1)
        gc.ok({"cmd": "upgrade_show", "state": "summary", "host": "dual_host.sav",
               "client": "dual_client.sav", "clientName": "ClientPlayer", "hostName": "HostPlayer"})
        gc.wait_for("summary state", lambda: session.has_state(gc, "SaveUpgradeSummaryState"))
        shot("05_summary.png")
        reset()

        # 06 battlescape theme: skipped - see module docstring (needs a live battle
        # for the battlescape palette; over the menu it renders as palette garbage).
        print("skip 06_gate_dialog_battlescape: faithful capture needs a live battle palette")

        print("ALL SHOTS SAVED to", outdir)
    finally:
        try:
            gc.shutdown()
        except Exception:
            pass


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "upgrade_shots"
    main(out)
