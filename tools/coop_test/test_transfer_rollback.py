"""Autonomous test: host save = single source of truth for soldier transfers.

Exact user repro:
  1. new game, host + client connected
  2. host transfers unit A to client
  3. host saves game
  4. host transfers unit B to client
  5. host abandons (in-session: statics survive, like returning to main menu)
  6. host loads the save, client re-fetches its world from the host
  7. EXPECTED: host has B (not A), client has A (not B), no bogus notices

Also runs the reverse direction (client gives, host's save still authority)
and the stacked-notice color check.

Run:  python tools/coop_test/test_transfer_rollback.py
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
from test_bug_fixes import bootstrap_fresh_session, own_base


def soldier_count(gc, name, owner=None):
    n = 0
    r = gc.ok({"cmd": "get_soldiers"})
    for b in r["bases"]:
        for s in b["soldiers"]:
            if s["name"] == name and (owner is None or s["owner"] == owner):
                n += 1
    return n


def dismiss_all_notices(gc):
    while gc.cmd({"cmd": "dismiss_notice"}).get("ok"):
        pass


def no_notices(gc):
    return not any("TransferNoticeState" in s for s in gc.cmd({"cmd": "get_state"})["states"])


def wait_blob_fresh(host, client):
    """Wait until the host holds the client's pushed world blob."""
    save_id = client.cmd({"cmd": "get_coop"})["saveID"]
    key = f"host_{save_id}_ClientPlayer.data"
    host.wait_for(f"client blob {key} on host",
                  lambda: host.cmd({"cmd": "has_coop_file", "key": key}).get("present") or None,
                  timeout=30)
    time.sleep(3)  # existence check can't tell a fresh push from an old one


def transfer_and_wait(giver, receiver, name, receiver_owner_id):
    giver.ok({"cmd": "transfer", "name": name, "owner": receiver_owner_id})
    receiver.wait_for(f"{name} arrives",
                      lambda: (soldier_count(receiver, name, owner=receiver_owner_id) == 1) or None, timeout=30)
    dismiss_all_notices(receiver)


def run_repro(host, client, giver, receiver, receiver_owner_id, tag):
    # unique names (fresh saves can roll identical rosters)
    roster = own_base(giver)["soldiers"]
    unit_a, unit_b = f"Unit A {tag}", f"Unit B {tag}"
    giver.ok({"cmd": "rename_soldier", "name": roster[0]["name"], "newName": unit_a})
    giver.ok({"cmd": "rename_soldier", "name": roster[1]["name"], "newName": unit_b})

    # 2. transfer A
    transfer_and_wait(giver, receiver, unit_a, receiver_owner_id)
    wait_blob_fresh(host, client)

    # 3. HOST saves (the authority snapshot: A traded, B not)
    host.ok({"cmd": "save_game", "file": f"authority_{tag}.sav"})

    # 4. transfer B
    transfer_and_wait(giver, receiver, unit_b, receiver_owner_id)
    wait_blob_fresh(host, client)

    # 5+6. host "abandons" and loads the save; client re-fetches its world
    host.ok({"cmd": "load_save", "file": f"authority_{tag}.sav"})
    client.ok({"cmd": "client_reload_progress"})

    if receiver is client:
        # client should end with A (traded pre-save) and without B
        expected = lambda: (soldier_count(client, unit_a, owner=receiver_owner_id) == 1
                            and soldier_count(client, unit_b, owner=receiver_owner_id) == 0) or None
    else:
        # client was the giver: A gone (given pre-save), B back (post-save trade rolled back)
        expected = lambda: (soldier_count(client, unit_a) == 0
                            and soldier_count(client, unit_b) == 1) or None
    client.wait_for("client world reloaded from host save", expected, timeout=90)

    # 7. assertions: the save is the authority
    assert soldier_count(giver, unit_a) == 0, f"{tag}: giver must NOT have {unit_a} (traded before save)"
    assert soldier_count(giver, unit_b) == 1, f"{tag}: giver must have {unit_b} (trade B was after the save)"
    assert soldier_count(receiver, unit_a, owner=receiver_owner_id) == 1, f"{tag}: receiver must have {unit_a}"
    assert soldier_count(receiver, unit_b, owner=receiver_owner_id) == 0, f"{tag}: receiver must NOT have {unit_b}"
    assert no_notices(host) and no_notices(client), f"{tag}: no bogus 'returned' notices allowed"
    print(f"PASS {tag}: save is authority - A stays traded, B rolled back, no notices")


def test_stacked_notices(gc):
    for i in range(3):
        gc.ok({"cmd": "show_notice", "message": f"stacked notice {i + 1}"})
    cats = gc.ok({"cmd": "get_notices"})["categories"]
    assert cats == ["geoManufactureComplete"] * 3, f"wrong categories: {cats}"
    for _ in range(3):
        gc.ok({"cmd": "dismiss_notice"})
    print("PASS stacked notices: all 3 use geoscape popup colors")


def main():
    host = GameClient("host", 47801, make_user_dir("host-user"))
    client = GameClient("client", 47802, make_user_dir("client-user"))
    try:
        host.spawn(); client.spawn()
        host.connect(); client.connect()
        bootstrap_fresh_session(host, client)

        test_stacked_notices(host)

        run_repro(host, client, giver=host, receiver=client, receiver_owner_id=1, tag="h2c")
        run_repro(host, client, giver=client, receiver=host, receiver_owner_id=0, tag="c2h")

        print("TEST PASSED")
    finally:
        host.shutdown(); client.shutdown()


if __name__ == "__main__":
    main()
