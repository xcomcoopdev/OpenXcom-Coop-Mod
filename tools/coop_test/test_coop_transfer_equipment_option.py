"""Regression test: the "Alternate craft equipment management" option
(Options::oxceAlternateCraftEquipmentManagement) drives whether a soldier's
equipment travels with it when it is vanilla-transferred to a co-op base.

  OFF (default): a soldier's gear is not carried between bases. The transferred
    soldier is stripped - it arrives at the peer base with an empty layout.
  ON: the equipment layout travels with the soldier; it stays equipped at the
    peer base.

This test only checks the guest's *layout* at the peer base (stripped vs kept).
The matching item-count movement (ON: sender -N / receiver +N) is covered by
test_coop_transfer_equipment_counts.py.

Only a base-resident (non-craft) soldier is individually transferable, and a
fresh base has exactly one, so each mode runs in its own fresh session.

Run:  python tools/coop_test/test_coop_transfer_equipment_option.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session

ROCKET = "STR_SMALL_ROCKET"
OPT = "oxceAlternateCraftEquipmentManagement"


def own_base(gc):
    s = gc.ok({"cmd": "get_soldiers"})
    return next(b for b in s["bases"]
               if not b["coopBaseFlag"] and not b["coopIcon"] and b["soldiers"])


def run_mode(alt_on, label):
    host = GameClient("host", 47811, make_user_dir("host-user"))
    client = GameClient("client", 47812, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client)

        hb = own_base(host); cb = own_base(client)

        # the lone base-resident soldier, uniquely named + a rocket in the off-hand
        soldier = next(s for s in hb["soldiers"] if not s.get("craft"))["name"]
        host.ok({"cmd": "rename_soldier", "name": soldier, "newName": "Zzz Xfer"})
        host.ok({"cmd": "give_layout", "item": ROCKET, "slot": "left", "name": "Zzz", "count": 1})

        # both machines share the option value
        host.ok({"cmd": "set_option", "name": OPT, "value": alt_on})
        client.ok({"cmd": "set_option", "name": OPT, "value": alt_on})

        r = host.ok({"cmd": "transfer_to_coop_base", "name": "Zzz", "toBase": cb["name"]})
        assert r.get("transferred"), f"{label}: transfer failed: {r}"

        host.ok({"cmd": "visit_coop_base", "base": cb["name"]})
        host.wait_for("inside peer base",
                      lambda: host.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None, timeout=60)
        rep = host.ok({"cmd": "base_report", "coop": True})
        guest = next((s for s in rep["soldiers"] if "Zzz" in s["name"]), None)
        assert guest is not None, f"{label}: transferred soldier not at the visited base"
        layout = guest["layout"]
        print(f"{label}: visited guest '{guest['name']}' layout={layout}")

        host.ok({"cmd": "leave_base"})
        host.wait_for("back home",
                      lambda: (not host.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None, timeout=60)
        return layout
    finally:
        host.shutdown(); client.shutdown()


def main():
    off_layout = run_mode(False, "OFF")
    assert off_layout == [], f"OFF: expected the transferred soldier stripped (empty layout), got {off_layout}"
    print("PASS OFF: soldier stripped on transfer (empty layout at peer base)")

    on_layout = run_mode(True, "ON")
    assert ROCKET in on_layout, f"ON: expected the transferred soldier to keep its {ROCKET}, got {on_layout}"
    print("PASS ON: soldier kept its loadout on transfer (equipped at peer base)")

    print("TEST PASSED")


if __name__ == "__main__":
    main()
