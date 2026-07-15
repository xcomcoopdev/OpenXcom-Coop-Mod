"""Validates that a soldier's equipped items move with it when it is vanilla-
transferred to a co-op base, driven by the "Alternate craft equipment
management" option:

  ON  - the gear's item counts move AND arrive IMMEDIATELY (a 0-hour transfer,
        matching the instant soldier move): the sending base's stored count
        drops, the receiving base's stored count rises by the same amount, with
        no pending timed transfer and no in-game time elapsing.
  OFF - nothing moves.

Immediacy is proven definitively: both instances sit paused after
new_campaign, so a travel-timed transfer could NEVER arrive (its clock never
moves). The receiving base's stored count going up while the in-game clock is
unchanged can only mean the items landed instantly.

Several distinct items are equipped (a rocket in one hand, a pistol in the
other) so multi-item transfer is covered too.

Only a base-resident (non-craft) soldier is individually transferable and a
fresh base has one, so each mode runs in its own fresh session.

Run:  python tools/coop_test/test_coop_transfer_equipment_counts.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo

# item -> inventory slot; both are present in a fresh starting base.
GEAR = {"STR_SMALL_ROCKET": "left", "STR_PISTOL": "right"}
OPT = "oxceAlternateCraftEquipmentManagement"


def own_base(gc):
    s = gc.ok({"cmd": "get_soldiers"})
    return next(b for b in s["bases"]
               if not b["coopBaseFlag"] and not b["coopIcon"] and b["soldiers"])


def stored(gc, base_name, item):
    return gc.ok({"cmd": "base_report", "base": base_name})["storage"].get(item, 0)


def incoming(gc, base_name, item):
    return gc.ok({"cmd": "incoming_transfers", "base": base_name})["items"].get(item, 0)


def run(alt_on):
    host = GameClient("host", 47811, make_user_dir("host-user"))
    client = GameClient("client", 47812, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client)

        hb = own_base(host); cb = own_base(client)
        host.ok({"cmd": "set_option", "name": OPT, "value": alt_on})
        client.ok({"cmd": "set_option", "name": OPT, "value": alt_on})

        # the lone base-resident soldier, uniquely named + several equipped items
        soldier = next(s for s in hb["soldiers"] if not s.get("craft"))["name"]
        host.ok({"cmd": "rename_soldier", "name": soldier, "newName": "Zzz Xfer"})
        for item, slot in GEAR.items():
            host.ok({"cmd": "give_layout", "item": item, "slot": slot, "name": "Zzz", "count": 1})

        send0 = {i: stored(host, hb["name"], i) for i in GEAR}
        recv0 = {i: stored(client, cb["name"], i) for i in GEAR}
        for i, q in send0.items():
            assert q >= 1, f"sending base needs a {i} to reason about (has {q})"
        clock0 = geo.game_minutes(client)

        host.ok({"cmd": "transfer_to_coop_base", "name": "Zzz", "toBase": cb["name"]})

        if alt_on:
            # every reserved item leaves the sender immediately...
            for i in GEAR:
                assert stored(host, hb["name"], i) == send0[i] - 1, \
                    f"ON: sender {i} {send0[i]} -> {stored(host, hb['name'], i)}, expected -1"
            # ...and lands in the receiver's STORAGE immediately (no time advance).
            client.wait_for(
                "all gear stored at receiving base immediately",
                lambda: all(stored(client, cb["name"], i) == recv0[i] + 1 for i in GEAR) or None,
                timeout=20)
            clock1 = geo.game_minutes(client)
            for i in GEAR:
                assert incoming(client, cb["name"], i) == 0, f"ON: {i} should land in storage, not queue"
                assert (stored(host, hb["name"], i) + stored(client, cb["name"], i)) \
                    == (send0[i] + recv0[i]), f"ON: world {i} total changed"
            # definitive immediacy: the receiver's clock never advanced.
            assert clock1 == clock0, \
                f"ON: gear arrived only after in-game time passed ({clock0} -> {clock1}); not immediate"
            print(f"PASS ON: {list(GEAR)} each moved sender -1 / receiver +1, "
                  f"immediately (clock frozen at {clock0}), world conserved")
        else:
            for _ in range(4):
                host.cmd({"cmd": "ping"}); client.cmd({"cmd": "ping"}); time.sleep(1)
            for i in GEAR:
                assert stored(host, hb["name"], i) == send0[i], f"OFF: sender {i} changed"
                assert stored(client, cb["name"], i) == recv0[i], f"OFF: receiver {i} changed"
                assert incoming(client, cb["name"], i) == 0, f"OFF: receiver got {i} incoming"
            print(f"PASS OFF: nothing moved (sender & receiver unchanged for {list(GEAR)})")
    finally:
        host.shutdown(); client.shutdown()


def main():
    run(alt_on=True)
    run(alt_on=False)
    print("TEST PASSED")


if __name__ == "__main__":
    main()
