"""Boot one instance and confirm the active mod set loads to the main menu
without fatal errors. Not a coop test - just an install/smoke check. Runs
against whatever master mod the isolated options.cfg selects (bare OXCE by
default); needs no pre-existing save."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness import GameClient, make_user_dir


def main():
    d = make_user_dir("bootcheck")
    g = GameClient("bootcheck-host", 45999, d)
    g.spawn()
    try:
        g.connect(timeout=180)  # large mod sets (e.g. XCF) parse slowly on first boot
        print("PING OK")
        st = g.wait_for("main menu (mods fully parsed)",
                        lambda: (lambda s: s if s and s[0] != "class OpenXcom::StartState" else None)(
                            g.cmd({"cmd": "get_state"}).get("states")),
                        timeout=180, interval=2)
        print("STATE:", st)
        co = g.cmd({"cmd": "get_coop"})
        print("COOP:", co)
    finally:
        time.sleep(1)
        g.shutdown()
        log = os.path.join(d, "openxcom.log")
        if os.path.exists(log):
            with open(log, encoding="utf-8", errors="replace") as f:
                lines = f.readlines()
            print("=== last 40 log lines ===")
            print("".join(lines[-40:]))
            errs = [l for l in lines if "ERROR" in l or "FATAL" in l or "error" in l.lower()]
            print("=== error-ish lines: %d ===" % len(errs))
            print("".join(errs[-20:]))


if __name__ == "__main__":
    main()
