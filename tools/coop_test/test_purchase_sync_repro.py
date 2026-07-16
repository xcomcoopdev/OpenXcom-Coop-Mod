"""Repro harness for the two concerns raised reviewing branch `purchase-sync-fixes`.

Both tests assert the bug does NOT happen, so they FAIL while the bug is present.
They exercise the REAL modified functions (no re-implementation):

  #1 craft-with-crew transfer UAF
     TransferItemsState::createPendingTransfers builds a craft transfer + one
     transfer per crew soldier on the destination base. When the sender receives
     "transfer_completed", Base::removePendingTransfers removes the craft from the
     source base via removeCraft(craft, false) (which does NOT delete it) and KEEPS
     the crew soldiers in the source base. The destination transfers are then
     deleted, and Transfer::~Transfer (with _delivered == false) does `delete _craft`
     -> the craft is freed while the kept crew soldiers still point at it
     (dangling Craft*, use-after-free).

  #2 receiver ACKs before validating the target base exists
     connectionTCP onTCPMessage("transfer"/"purchase") accepts a packet whenever its
     "items" array is non-empty and immediately sends "*_completed" back, WITHOUT
     checking that a base with base_to_id exists locally. updateCoopTask can then
     never apply it (no base matches) and silently re-queues it forever, while the
     sender - told it succeeded - removes the goods from its own base. Net: goods
     lost, which is the exact class of corruption the commit claims to fix.

Run:  python tools/coop_test/test_purchase_sync_repro.py

Note: #2 needs getCoopStatic() == true (updateCoopTask only drains the trade queue
in a live coop session), so both repros run on a connected two-instance session.
#1 is purely local to the sender but rides the same session for convenience.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
from test_bug_fixes import bootstrap_fresh_session


def repro_craft_uaf(host):
    r = host.ok({"cmd": "repro_craft_uaf"})
    print(f"  repro_craft_uaf -> {r}")
    assert r.get("removeOk") is True, f"removePendingTransfers did not run cleanly: {r}"
    assert not r.get("crewCraftDangling"), (
        f"BUG #1 REPRODUCED: crew '{r.get('crewName')}' was kept in the source base "
        f"but its Craft* points at a craft that was removed and freed "
        f"(dangling pointer / use-after-free)"
    )
    print("PASS #1: crew's craft pointer is not left dangling")


def repro_receiver_ack_gap(host):
    r = host.ok({"cmd": "repro_receiver_ack_gap"})
    print(f"  repro_receiver_ack_gap -> {r}")
    assert not r.get("appliedToAnyBaseWhenNoMatch"), (
        f"setup issue: transfer applied to a base despite a non-matching id: {r}"
    )
    assert not r.get("receiverAcceptedAndQueued"), (
        f"BUG #2 REPRODUCED: receiver accepted + queued (and would ACK "
        f"'transfer_completed' for) a transfer targeting base id {r.get('bogusBaseId')} "
        f"that exists on no base; the sender would then remove goods that never arrive"
    )
    print("PASS #2: receiver did not accept a transfer for a non-existent base")


def main():
    host = GameClient("host", 46110, make_user_dir("psf-repro-host"))
    client = GameClient("client", 46111, make_user_dir("psf-repro-client"))
    failures = []
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()
        bootstrap_fresh_session(host, client)

        for fn in (repro_craft_uaf, repro_receiver_ack_gap):
            try:
                fn(host)
            except AssertionError as e:
                print(f"REPRO (expected to fire while the bug is present):\n  {e}")
                failures.append(str(e))
    finally:
        host.shutdown()
        client.shutdown()

    if failures:
        print(f"\n{len(failures)} concern(s) reproduced -- see assertions above.")
        sys.exit(1)
    print("\nNeither concern reproduced.")


if __name__ == "__main__":
    main()
