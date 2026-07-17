"""PRD-J10: open JOINT screens react to the other player's joint_apply.

OXCE base screens snapshot the world in their CONSTRUCTORS (PurchaseState
precomputes rows/prices/stock/funds, CraftSoldiersState precomputes the crew
list). In a JOINT campaign a peer's joint_apply lands underneath an open screen
and leaves it showing a world that no longer exists. J10 binds each JOINT screen
to a single apply listener and rebuilds it.

The assertions read CONSTRUCTOR-TIME caches (the FUNDS header text, the row's
snapshotted base stock, the SPACE USED header) precisely because those cannot
change without a rebuild - a live world read would pass even if refresh were
broken.

  PURCHASE  client sits in PurchaseState; host SELLS rifles (funds up, stock
            down in one apply) -> the client's screen shows the new funds AND
            the new stock, and is still a PurchaseState (rebuilt, not crashed).
  STALE     a purchase order built against the pre-apply world, submitted on the
            raw joint_cmd lane (i.e. bypassing the rebuild that would have
            discarded it), is re-validated and REJECTED by the host - the host
            validator, not the screen, is the correctness guard.
  SQUAD     client sits in CraftSoldiersState while the host de-assigns a
            soldier from the same craft -> the crew header refreshes live.
  FUNDS     host sits in PurchaseState while the CLIENT buys -> the host's own
            open screen refreshes too (both roles notify).

Run:  python tools/coop_test/test_joint_refresh.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir
import session
import geo

RIFLE = "STR_RIFLE"


def _funds(gc):
    return gc.ok({"cmd": "geo_state"})["funds"]


def _base0(gc):
    for b in gc.ok({"cmd": "geo_state"})["bases"]:
        if not b.get("coopBase") and not b.get("coopIcon"):
            return b
    raise AssertionError("no real base in geo_state")


def _stock(gc, item=RIFLE):
    """Live base storage (base_report) - as opposed to a screen's cached snapshot."""
    return gc.ok({"cmd": "base_report"})["storage"].get(item, 0)


def _roster(gc):
    for b in gc.ok({"cmd": "get_soldiers"})["bases"]:
        if not b.get("coopBaseFlag") and not b.get("coopIcon"):
            return b["soldiers"]
    raise AssertionError("no real base in get_soldiers")


def _digits(text):
    """The number out of a formatted funds label. Unicode::formatFunding uses a
    non-ASCII thousands separator, so strip everything that is not a digit."""
    return "".join(c for c in text if c.isdigit())


def _screen(gc, item=None):
    req = {"cmd": "screen_state"}
    if item:
        req["item"] = item
    return gc.ok(req)


def _stats(gc):
    return gc.ok({"cmd": "joint_stats"})


def _first_craft(gc):
    crafts = _base0(gc)["crafts"]
    assert crafts, "no craft at the starting base"
    return crafts[0]


def main():
    host_dir = make_user_dir("jref_host")
    client_dir = make_user_dir("jref_client")
    host = GameClient("host", 48740, host_dir)
    client = GameClient("client", 48741, client_dir)
    try:
        host.spawn()
        client.spawn()
        host.connect()
        client.connect()

        session.new_campaign(host, client, port="48040", campaign_mode="joint")
        geo.wait_both_ready(host, client)

        # Seed an equal shared world: give_items mutates each machine directly
        # (deterministic, no protocol), the established J05 scaffolding idiom.
        # The starting base already ships a couple of rifles, so work relative to
        # whatever is there rather than assuming an empty store.
        for gc in (host, client):
            gc.ok({"cmd": "give_items", "item": RIFLE, "count": 20})

        f0 = _funds(host)
        assert f0 == _funds(client), "bootstrap funds differ"
        stock0 = _stock(host)
        assert stock0 == _stock(client) and stock0 >= 20,             f"seeded stores differ or too small: host={stock0} client={_stock(client)}"
        print(f"PASS setup: identical funds {f0}, {stock0} rifles in stores on both")

        # ================================================================
        # 1) PURCHASE: the client sits in PurchaseState while the host sells.
        #    One apply moves BOTH things this screen cached: funds and stock.
        # ================================================================
        r = client.ok({"cmd": "open_screen", "screen": "purchase"})
        assert r.get("ok"), f"client could not open PurchaseState: {r}"
        s0 = _screen(client, RIFLE)
        assert s0["top"] == "purchase", f"client top state is {s0['top']}, want purchase"
        assert s0["stock"] == stock0,             f"client PurchaseState cached stock {s0['stock']}, want {stock0}"
        funds_text0 = s0["funds"]
        print(f"PASS open: client in PurchaseState showing '{funds_text0}', stock {stock0}")

        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        r = host.ok({"cmd": "sell", "item": RIFLE, "count": 10})
        assert r.get("sent"), f"host sell not sent: {r}"

        def _refreshed():
            s = _screen(client, RIFLE)
            if s["top"] != "purchase":
                return None
            if s["stock"] != stock0 - 10:
                return None
            if s["funds"] == funds_text0:
                return None
            return s

        client.wait_for("client PurchaseState rebuilt after the host's sell",
                        _refreshed, timeout=30, interval=0.5)
        s1 = _screen(client, RIFLE)
        assert s1["top"] == "purchase", "client screen was not a PurchaseState after refresh"
        assert s1["stock"] == stock0 - 10, f"stale stock after refresh: {s1['stock']}"
        assert s1["funds"] != funds_text0, "funds header still showing the pre-apply value"
        # the rebuilt header must agree with the authoritative funds.
        f1 = _funds(client)
        assert f1 == _funds(host), f"funds diverged: client={f1} host={_funds(host)}"
        assert f1 > f0, f"sell did not credit funds: {f0} -> {f1}"
        assert _digits(s1["funds"]) == str(f1), \
            f"funds header '{s1['funds']}' does not reflect the new funds {f1}"
        assert client.cmd({"cmd": "ping"}).get("pong"), "client unresponsive after refresh"
        print(f"PASS refresh: client screen rebuilt -> stock {stock0}->{stock0 - 10}, "
              f"'{funds_text0}' -> '{s1['funds']}' (funds {f0} -> {f1}), no crash")

        # ================================================================
        # 2) STALE ORDER: the refresh discards an in-progress order, so to prove
        #    the HOST re-validates a stale one we must submit it past the screen.
        #    Raw joint_cmd lane = the J07 tile-conflict idiom: it is the host
        #    validator, not the UI, that is the correctness guard.
        # ================================================================
        host.ok({"cmd": "joint_reset_stats"})
        client.ok({"cmd": "joint_reset_stats"})
        # Both machines: set_funds is per-machine scaffolding and funds are part of
        # the world checksum, so a host-only value is a real desync that the J10
        # auto-repair would fix by re-streaming the world - which replaces the
        # client's whole state stack and would take the open screens with it.
        for gc in (host, client):
            gc.ok({"cmd": "set_funds", "value": 1000})  # authoritative funds now tiny
        stock_pre_h = _stock(host)

        # An order priced against the OLD (rich) world, submitted now.
        client.ok({"cmd": "joint_cmd", "jcmd": "buy", "baseId": 0,
                   "payload": {"items": [{"type": 0, "rule": RIFLE, "qty": 5}],
                               "total": 1}})  # type 0 = TRANSFER_ITEM
        client.wait_for("host rejected the stale order",
                        lambda: (_stats(client)["failCount"] >= 1) or None,
                        timeout=30, interval=0.5)
        cs = _stats(client)
        assert cs["lastFail"] == "STR_NOT_ENOUGH_MONEY", \
            f"stale order rejected for the wrong reason: {cs}"
        assert _funds(host) == 1000, "host funds moved on a rejected stale order"
        assert _stock(host) == stock_pre_h, \
            "host stores moved on a rejected stale order"
        print(f"PASS stale order: host re-validated and rejected it "
              f"('{cs['lastFail']}'), world unchanged")
        # the client's failure dialog is modal - dismiss it before driving on.
        try:
            client.ok({"cmd": "coop_dialog_back"})
        except Exception:
            pass
        for gc in (host, client):
            gc.ok({"cmd": "set_funds", "value": 5000000})

        # ================================================================
        # 3) SQUAD: CraftSoldiersState is the screen two players most plausibly
        #    edit at once (one shared squad). Client watches; host de-assigns.
        # ================================================================
        craft = _first_craft(host)
        cid = craft["id"]
        # find a soldier currently aboard that craft
        aboard = [s for s in _roster(host) if s.get("craftId") == cid]
        assert aboard, f"no soldier aboard craft {cid} to de-assign"
        victim = aboard[0]["id"]

        r = client.ok({"cmd": "open_screen", "screen": "craft_soldiers", "craft_id": cid})
        assert r.get("ok"), f"client could not open CraftSoldiersState: {r}"
        sq0 = _screen(client)
        assert sq0["top"] == "craft_soldiers", f"client top is {sq0['top']}"
        used0 = sq0["used"]
        print(f"PASS squad open: client in CraftSoldiersState showing '{used0}'")

        host.ok({"cmd": "craft_assign", "craft_id": cid, "soldier_id": victim, "on": False})

        def _squad_refreshed():
            s = _screen(client)
            if s["top"] != "craft_soldiers":
                return None
            return s if s["used"] != used0 else None

        client.wait_for("client CraftSoldiersState refreshed after the host's craft_assign",
                        _squad_refreshed, timeout=30, interval=0.5)
        sq1 = _screen(client)
        assert sq1["top"] == "craft_soldiers", "squad screen did not survive the refresh"
        print(f"PASS squad refresh: '{used0}' -> '{sq1['used']}' live (no re-entry)")

        # ================================================================
        # 4) FUNDS both ways: the HOST's open screen is as stale as a replica's
        #    when the CLIENT spends shared funds. Both roles fire the listener.
        # ================================================================
        r = host.ok({"cmd": "open_screen", "screen": "purchase"})
        assert r.get("ok"), f"host could not open PurchaseState: {r}"
        h0 = _screen(host, RIFLE)
        assert h0["top"] == "purchase", f"host top is {h0['top']}"
        host_funds_text0 = h0["funds"]

        client.ok({"cmd": "buy", "item": RIFLE, "count": 5})

        def _host_refreshed():
            s = _screen(host, RIFLE)
            if s["top"] != "purchase":
                return None
            return s if s["funds"] != host_funds_text0 else None

        host.wait_for("host PurchaseState rebuilt after the client's buy",
                      _host_refreshed, timeout=30, interval=0.5)
        h1 = _screen(host, RIFLE)
        assert h1["top"] == "purchase", "host screen was not a PurchaseState after refresh"
        print(f"PASS host-side refresh: '{host_funds_text0}' -> '{h1['funds']}' "
              f"after the client's buy")

        # standing invariant: the JOINT replica never wrote save data to disk.
        session.assert_client_zero_disk(client_dir)
        print("PASS zero-disk: client (replica) user dir clean")

        print("ALL JOINT REFRESH TESTS PASSED")
    finally:
        host.shutdown()
        client.shutdown()


if __name__ == "__main__":
    main()
