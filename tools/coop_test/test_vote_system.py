"""Regression coverage for the host-authoritative multiplayer VoteMenu.

Covers two layers:

1. The production VoteSession rules directly through TestServer:
   - a 3-player vote passes with 2 YES votes;
   - a 4-player vote needs 3 YES votes and a 2-2 split fails;
   - the starter is automatically YES and cannot vote twice.
2. A real host/client session:
   - a client can request the vote;
   - the host sends the locked roster names in seat order;
   - both VoteMenus render the real player names instead of PLAYER 1/2;
   - the host's second YES resolves the vote on both machines.

Run:  python tools/coop_test/test_vote_system.py
Exit 0 = pass; an assertion/exception = failure.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session


def _wait_vote(gc, description, predicate, timeout=30):
    return gc.wait_for(
        description,
        lambda: (lambda state: state if predicate(state) else None)(
            gc.ok({"cmd": "vote_state"})
        ),
        timeout=timeout,
        interval=0.25,
    )


def _assert_rule_probes(host):
    three = host.ok({
        "cmd": "vote_session_probe",
        "players": 3,
        "starter": 0,
        "names": ["Alice", "Bob", "Carol"],
        "casts": [{"seat": 1, "yes": True}],
    })
    assert three["requiredYes"] == 2, three
    assert three["decision"] == "passed", three
    assert three["votes"] == [1, 1, -1], three
    assert three["playerNames"] == ["Alice", "Bob", "Carol"], three
    print("PASS 3-player strict majority: 2 YES votes")

    four_pass = host.ok({
        "cmd": "vote_session_probe",
        "players": 4,
        "starter": 0,
        "names": ["Alice", "Bob", "Carol", "Dave"],
        "casts": [
            {"seat": 1, "yes": True},
            {"seat": 2, "yes": True},
        ],
    })
    assert four_pass["requiredYes"] == 3, four_pass
    assert four_pass["decision"] == "passed", four_pass
    print("PASS 4-player strict majority: 3 YES votes")

    four_tie = host.ok({
        "cmd": "vote_session_probe",
        "players": 4,
        "starter": 0,
        "casts": [
            {"seat": 1, "yes": True},
            {"seat": 2, "yes": False},
            {"seat": 3, "yes": False},
        ],
    })
    assert four_tie["decision"] == "failed", four_tie
    assert four_tie["yesVotes"] == 2 and four_tie["noVotes"] == 2, four_tie
    print("PASS 4-player 2-2 tie fails")

    duplicate = host.ok({
        "cmd": "vote_session_probe",
        "players": 3,
        "starter": 1,
        "casts": [
            {"seat": 1, "yes": False},  # starter already auto-voted YES
            {"seat": 2, "yes": False},
        ],
    })
    assert duplicate["accepted"] == [False, True], duplicate
    assert duplicate["votes"] == [-1, 1, 0], duplicate
    print("PASS duplicate seat vote rejected")


def main():
    host_dir = make_user_dir("vote_system_host")
    client_dir = make_user_dir("vote_system_client")
    host = GameClient("host", 48940, host_dir)
    client = GameClient("client", 48941, client_dir)

    host_name = "AliceHost"
    client_name = "BobClient"
    expected_names = [host_name, client_name]

    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        _assert_rule_probes(host)

        session.new_campaign(
            host,
            client,
            port="47995",
            host_name=host_name,
            client_name=client_name,
            host_base="Alice Base",
            client_base="Bob Base",
        )

        # Start from the CLIENT. Its request is itself seat 1's YES vote; the
        # host creates the authoritative session and sends vote_start + names.
        client.ok({
            "cmd": "vote_request",
            "action": "test_vote",
            "title": "PLAYER NAME TEST",
            "question": "Do both machines show the real names?",
        })

        host_vote = _wait_vote(
            host,
            "host VoteMenu",
            lambda s: s.get("active") and s.get("menuOpen"),
        )
        client_vote = _wait_vote(
            client,
            "client VoteMenu",
            lambda s: s.get("active") and s.get("menuOpen"),
        )

        for label, state in (("host", host_vote), ("client", client_vote)):
            assert state["playerNames"] == expected_names, f"{label}: {state}"
            assert state["menuPlayerNames"] == expected_names, f"{label}: {state}"
            assert host_name in state["menuRows"], f"{label}: {state}"
            assert client_name in state["menuRows"], f"{label}: {state}"
            assert "PLAYER 1" not in state["menuRows"], f"{label}: {state}"
            assert "PLAYER 2" not in state["menuRows"], f"{label}: {state}"
            assert state["requiredYes"] == 2, f"{label}: {state}"
            assert state["starterSeat"] == 1, f"{label}: {state}"
            assert state["votes"] == [-1, 1], f"{label}: {state}"
        print("PASS both VoteMenus render the locked roster names")

        cast = host.ok({"cmd": "vote_cast", "yes": True})
        assert cast["accepted"] is True, cast

        host_done = _wait_vote(
            host,
            "host vote result",
            lambda s: s.get("finished") and s.get("passed"),
        )
        client_done = _wait_vote(
            client,
            "client vote result",
            lambda s: s.get("finished") and s.get("passed"),
        )
        assert host_done["votes"] == [1, 1], host_done
        assert client_done["votes"] == [1, 1], client_done
        assert host_done["menuFinished"] is True, host_done
        assert client_done["menuFinished"] is True, client_done
        print("PASS host-authoritative result reached both machines")

        host.ok({"cmd": "vote_close"})
        client.ok({"cmd": "vote_close"})
        session.assert_client_zero_disk(client_dir)

        print("ALL VOTE SYSTEM TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
