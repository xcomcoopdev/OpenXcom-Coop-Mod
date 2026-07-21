"""Regression test: the base that HOUSES a soldier is the base that pays for it.

A SEPARATE co-op transfer moves a soldier to the peer's base but deliberately
does NOT change its owner (that is what a gift does), so:

  * the sender KEEPS the Soldier object - TransferItemsState::completeTransfer
    skips the erase when the destination is a co-op base, and just tags the
    soldier with getCoopBase() = that base's id;
  * the receiver gets NOTHING - Base::syncTrade drops an incoming
    TRANSFER_SOLDIER outright because `soldier_rule` is never written by either
    transfer path.

So before this was fixed the guest occupied NOBODY's living quarters: the sender
went on paying for a soldier that had left, and the receiver was charged nothing.
Two halves close it:

  sender   Base::getUsedQuarters() no longer counts soldiers with
           getCoopBase() != -1 (real own bases only - while VISITING a peer base
           the world is swapped and those guests really are its residents);
  receiver connectionTCP::sendGuestCensus() reports the per-peer-base headcount,
           the peer stores it in Base::coop_guests, and getUsedQuarters() adds it.

Both directions are checked (the bug is symmetric), and THREE soldiers are moved
each way, not one: a single transfer would not have caught that the roster was
being silently pruned between moves.

Run:  python tools/coop_test/test_coop_quarters_accounting.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import test_coop_transferred_equipment as T


def quarters(gc, base_name):
    return gc.ok({"cmd": "base_report", "base": base_name})["usedQuarters"]


def free_soldier(gc, base_name):
    """A base-resident soldier, taking one off the craft if they are all seated
    (only getCraft()==0 soldiers are transferable rows)."""
    r = gc.ok({"cmd": "base_report", "base": base_name})
    free = [s for s in r["soldiers"] if s["craft"] == -1 and s["coopBase"] == -1]
    if free:
        return free[0]
    craft = next(c for c in r["crafts"] if c["type"] == "STR_SKYRANGER")["id"]
    seated = next(s for s in r["soldiers"] if s["craft"] == craft and s["coopBase"] == -1)
    gc.ok({"cmd": "craft_assign", "soldier_id": seated["id"],
           "craft_id": craft, "base": base_name, "on": False})
    return seated


def run_direction(sender, receiver, send_base, recv_base, label, moves=3):
    send0 = quarters(sender, send_base)
    recv0 = quarters(receiver, recv_base)
    print(f"{label}: before - {send_base}={send0} (sender's view), "
          f"{recv_base}={recv0} (receiver's view)")

    for n in range(1, moves + 1):
        soldier = free_soldier(sender, send_base)
        tr = sender.ok({"cmd": "transfer_to_coop_base",
                        "name": soldier["name"], "toBase": recv_base})
        assert tr.get("transferred"), f"{label}: transfer {n} failed: {tr}"

        sender.wait_for(
            f"{label}: sender stopped paying for {n} guest(s)",
            lambda n=n: (quarters(sender, send_base) == send0 - n) or None, timeout=30)
        receiver.wait_for(
            f"{label}: receiver charged for {n} guest(s)",
            lambda n=n: (quarters(receiver, recv_base) == recv0 + n) or None, timeout=45)
        print(f"{label}: after {n} transfer(s) - {send_base}={quarters(sender, send_base)}, "
              f"{recv_base}={quarters(receiver, recv_base)}")

    # the world total is conserved: nobody is housed twice, nobody vanishes
    assert quarters(sender, send_base) + quarters(receiver, recv_base) == send0 + recv0, (
        f"{label}: living quarters not conserved - "
        f"{send0}+{recv0} -> {quarters(sender, send_base)}+{quarters(receiver, recv_base)}")
    print(f"PASS {label}: {moves} guest(s) moved from the sender's quarters to the "
          f"receiver's, world total conserved")


def visiting_view_is_not_double_counted(visitor, peer_base, label):
    """While visiting, the guests ARE that base's residents - the correction must
    not be applied a second time there."""
    visitor.ok({"cmd": "visit_coop_base", "base": peer_base})
    visitor.wait_for("inside peer base",
                     lambda: visitor.cmd({"cmd": "get_coop"}).get("insideCoopBase") or None,
                     timeout=60)
    rep = visitor.ok({"cmd": "base_report", "coop": True})
    guests = [s for s in rep["soldiers"] if s["coopBase"] != -1]
    assert rep["usedQuarters"] >= len(guests), (
        f"{label}: the visited base reports {rep['usedQuarters']} used quarters but "
        f"houses {len(guests)} of my guests - the sender-side correction was applied "
        f"to a visited base too")
    print(f"PASS {label}: visited base counts its {len(guests)} guest(s) "
          f"(usedQuarters={rep['usedQuarters']})")
    visitor.ok({"cmd": "leave_base"})
    visitor.wait_for("back home",
                     lambda: (not visitor.cmd({"cmd": "get_coop"}).get("insideCoopBase")) or None,
                     timeout=60)


def main():
    host = GameClient("host", 47811, make_user_dir("host-user"))
    client = GameClient("client", 47812, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        session.new_campaign(host, client)
        hb = T.own_base(host)["name"]
        cb = T.own_base(client)["name"]

        run_direction(host, client, hb, cb, "host -> client")
        visiting_view_is_not_double_counted(host, cb, "host visiting client base")

        run_direction(client, host, cb, hb, "client -> host")
        visiting_view_is_not_double_counted(client, hb, "client visiting host base")

        print("TEST PASSED")
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
