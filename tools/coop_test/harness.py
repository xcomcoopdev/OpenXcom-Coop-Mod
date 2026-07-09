"""Test harness driver for the OpenXcom coop mod.

Talks to the in-game TestServer (src/CoopMod/TestServer.cpp), which is enabled
by setting the OXC_TEST_PORT environment variable on a game instance. Protocol:
newline-delimited JSON over TCP on 127.0.0.1:<port>.

Typical use: spawn two instances (host + client) with isolated -user folders,
drive both through save-load / host / join / lobby, then assert on soldiers.
"""

import json
import os
import shutil
import socket
import subprocess
import time

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
EXE = os.path.join(REPO, "bin", "x64", "Release", "OpenXcom.exe")
TEST_ROOT = os.path.join(os.environ["TEMP"], "oxc-coop-test")

# A known-good land tile for first-base placement (place_first_base rejects
# water). Shared by the fresh-campaign tests so they need no pre-existing save.
LAND_LON, LAND_LAT = 0.7063353365604198, -0.5070346730015731

# Hermetic options.cfg: pin the stock `xcom1` master (no external mods, no
# reading of the machine's real config) with intro/audio/mouse-capture off and
# a small windowed display. OpenXcom defaults every unspecified key and rescans
# the other stock mods as inactive. Data (UFO/TFTD/standard/common) resolves
# from the exe's own dir, so this runs on any machine with a built OpenXcom.exe.
HERMETIC_OPTIONS = """\
mods:
  - active: true
    id: xcom1
options:
  displayWidth: 640
  displayHeight: 400
  fullscreen: false
  borderless: false
  captureMouse: false
  playIntro: false
  musicVolume: 0
  soundVolume: 0
  uiVolume: 0
"""


class GameClient:
    """One running game instance + its command socket."""

    def __init__(self, name, port, user_dir):
        self.name = name
        self.port = port
        self.user_dir = user_dir
        self.proc = None
        self.sock = None
        self.buf = b""

    def spawn(self, extra_args=()):
        env = os.environ.copy()
        env["OXC_TEST_PORT"] = str(self.port)
        # tuck the window into a corner (host left, client right of it)
        env["SDL_VIDEO_WINDOW_POS"] = "0,40" if "host" in self.name else "660,40"
        args = [EXE, "-user", self.user_dir] + list(extra_args)
        # best-effort: ask Windows to start the window without activating it
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 7  # SW_SHOWMINNOACTIVE
        self.proc = subprocess.Popen(args, env=env, cwd=os.path.dirname(EXE), startupinfo=si)

    def connect(self, timeout=60):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc and self.proc.poll() is not None:
                raise RuntimeError(f"{self.name}: game exited early rc={self.proc.returncode}")
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=2)
                self.sock.settimeout(30)
                if self.cmd({"cmd": "ping"}).get("pong"):
                    print(f"[{self.name}] connected on :{self.port}")
                    return
            except (ConnectionRefusedError, socket.timeout, OSError):
                self.sock = None
                time.sleep(1)
        raise TimeoutError(f"{self.name}: test server not reachable on :{self.port}")

    def cmd(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError(f"{self.name}: socket closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def ok(self, obj):
        r = self.cmd(obj)
        if not r.get("ok"):
            raise RuntimeError(f"{self.name}: {obj.get('cmd')} failed: {r.get('error')}")
        return r

    def wait_for(self, desc, predicate, timeout=90, interval=1.0):
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            last = predicate()
            if last:
                return last
            time.sleep(interval)
        raise TimeoutError(f"{self.name}: timed out waiting for {desc} (last={last!r})")

    def shutdown(self):
        try:
            if self.sock:
                self.sock.sendall((json.dumps({"cmd": "quit"}) + "\n").encode())
        except OSError:
            pass
        if self.proc:
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.proc.kill()


def make_user_dir(name, saves=()):
    """Hermetic, isolated user folder: a freshly written options.cfg pinning
    the stock `xcom1` master (see HERMETIC_OPTIONS) with no external mods and
    no dependence on the machine's real config. `saves` are copied into the
    master's save subfolder (xcom1/)."""
    d = os.path.join(TEST_ROOT, name)
    if os.path.exists(d):
        shutil.rmtree(d)
    os.makedirs(os.path.join(d, "xcom1"))
    with open(os.path.join(d, "options.cfg"), "w", encoding="utf-8") as f:
        f.write(HERMETIC_OPTIONS)
    for save in saves:
        shutil.copy(save, os.path.join(d, "xcom1"))
    return d


def find_soldier(soldier_lists, name):
    for base in soldier_lists["bases"]:
        for s in base["soldiers"]:
            if name in s["name"]:
                return base, s
    return None, None
