#!/usr/bin/env python3
"""Drive the logged-in desktop session in the test VM guest (Plasma on the
3080 Ti monitor), as a user would: windows (KWin scripts over DBus), mouse
and keyboard (own uinput devices: an absolute pointer, so a click lands
where the screenshot shows the button, no pointer acceleration), and
screenshots (spectacle). Used by scenario.py; also a CLI:

  ./gs.py windows                     JSON list: pid, class, caption, active, fullscreen, geometry
  ./gs.py activate PID|REGEX          raise and focus the first matching window
  ./gs.py shot FILE.png               full screen, physical pixels (3840x2160)
  ./gs.py click X Y [right]           at screenshot pixel X,Y
  ./gs.py move X Y
  ./gs.py key alt+tab                 one combination (names: evdev KEY_* without KEY_)
  ./gs.py type "text"
  ./gs.py wheel CLICKS [X Y]          mouse wheel (positive: down), at X,Y
  ./gs.py cursor                      where KWin has the cursor (logical px)

Runs as the session user in the guest, e.g. over vm.sh ssh.
"""

import json
import os
import re
import subprocess
import sys
import tempfile
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))


def session_env():
    """Environment of the desktop session, from plasmashell (the Wayland
    socket name changes whenever SDDM restarts)."""
    uid = os.getuid()
    pids = subprocess.run(["pgrep", "-u", str(uid), "-x", "plasmashell"], capture_output=True,
                          text=True).stdout.split()
    if not pids:
        raise RuntimeError("no plasmashell: nobody logged in on the desktop")
    env = dict(os.environ)
    with open(f"/proc/{pids[0]}/environ", "rb") as f:
        for item in f.read().split(b"\0"):
            k, _, v = item.decode(errors="replace").partition("=")
            if k in ("DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY", "DBUS_SESSION_BUS_ADDRESS",
                     "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE", "XDG_CURRENT_DESKTOP", "KDE_FULL_SESSION"):
                env[k] = v
    return env


ENV = None


def env():
    global ENV
    if ENV is None:
        ENV = session_env()
    return ENV


def run(cmd, **kw):
    return subprocess.run(cmd, env=env(), capture_output=True, text=True, **kw)


# --------------------------------------------------------------------------
# KWin scripts
# --------------------------------------------------------------------------

def kwin(js, wait=0.4):
    """Runs a KWin JavaScript snippet; returns what it passed to out(x)
    (JSON-encoded), read back from the journal."""
    tag = "GS" + uuid.uuid4().hex[:12]
    body = f'function out(x) {{ print("{tag} " + JSON.stringify(x)); }}\n{js}\n'
    since = time.time() - 1
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write(body)
        path = f.name
    try:
        run(["qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.loadScript", path, tag])
        run(["qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.start"])
        res = []
        deadline = time.time() + max(wait, 0.2) + 2
        while time.time() < deadline:
            time.sleep(wait)
            log = run(["journalctl", "--user", "-o", "cat", "--since", f"@{since:.0f}"]).stdout
            res = [json.loads(line.split(" ", 1)[1]) for line in log.splitlines()
                   if line.startswith(tag + " ")]
            if res:
                break
        return res
    finally:
        run(["qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.unloadScript", tag])
        os.unlink(path)


def windows():
    r = kwin("""
var list = [];
var o = workspace.stackingOrder;
for (var i = 0; i < o.length; i++) {
    var w = o[i];
    if (!w.normalWindow && !w.dialog) continue;
    var g = w.frameGeometry;
    list.push({pid: w.pid, cls: String(w.resourceClass), caption: String(w.caption),
               active: w === workspace.activeWindow, fullscreen: w.fullScreen, minimized: w.minimized,
               x: g.x, y: g.y, w: g.width, h: g.height, stack: i});
}
out(list);
""")
    return r[0] if r else []


def _match_js(match):
    if isinstance(match, int) or str(match).isdigit():
        return f"w.pid == {int(match)}"
    rx = json.dumps(str(match))
    return f"new RegExp({rx}, 'i').test(String(w.caption) + ' ' + String(w.resourceClass))"


def window_action(match, action_js):
    """Runs action_js with `w` = the first normal window matching (pid or
    regex on caption + class); returns its caption or None."""
    r = kwin(f"""
var o = workspace.stackingOrder, done = null;
for (var i = o.length - 1; i >= 0 && done === null; i--) {{
    var w = o[i];
    if (!w.normalWindow) continue;
    if ({_match_js(match)}) {{ {action_js}; done = String(w.caption); }}
}}
out(done);
""")
    return r[0] if r else None


def activate(match):
    return window_action(match, "w.minimized = false; workspace.activeWindow = w")


def minimize(match):
    return window_action(match, "w.minimized = true")


def fullscreen(match, on=True):
    return window_action(match, f"w.fullScreen = {'true' if on else 'false'}")


def active():
    r = kwin("var w = workspace.activeWindow; out(w ? {pid: w.pid, cls: String(w.resourceClass), "
             "caption: String(w.caption)} : null);")
    return r[0] if r else None


def cursor():
    r = kwin("out(workspace.cursorPos);")
    return r[0] if r else None


def screen():
    """Logical geometry of the screens and their scale."""
    r = kwin("var s = workspace.screens, l = []; for (var i = 0; i < s.length; i++) "
             "l.push({name: s[i].name, x: s[i].geometry.x, y: s[i].geometry.y, "
             "w: s[i].geometry.width, h: s[i].geometry.height, scale: s[i].devicePixelRatio}); out(l);")
    return r[0] if r else []


def shot(path):
    r = run(["spectacle", "-b", "-n", "-f", "-o", path], timeout=30)
    if not os.path.exists(path):
        raise RuntimeError(f"spectacle failed: {r.stderr.strip()[-200:]}")
    return path


# --------------------------------------------------------------------------
# input (uinput)
# --------------------------------------------------------------------------

class Input:
    """An absolute pointer (like QEMU's usb-tablet: libinput maps its range
    onto the screen) and a keyboard. Coordinates: physical pixels of a
    full-screen screenshot."""

    def __init__(self, width=None, height=None):
        from evdev import UInput, AbsInfo, ecodes as e
        self.e = e
        if width is None:
            s = screen()[0]
            width, height = round(s["w"] * s["scale"]), round(s["h"] * s["scale"])
        self.w, self.h = width, height
        cap = {
            e.EV_KEY: [e.BTN_LEFT, e.BTN_RIGHT, e.BTN_MIDDLE],
            e.EV_ABS: [(e.ABS_X, AbsInfo(0, 0, width - 1, 0, 0, 0)),
                       (e.ABS_Y, AbsInfo(0, 0, height - 1, 0, 0, 0))],
            e.EV_REL: [e.REL_WHEEL],
        }
        self.ptr = UInput(cap, name="gs-scenario-pointer", input_props=[e.INPUT_PROP_DIRECT])
        keys = [v for k, v in e.ecodes.items() if k.startswith("KEY_") and v < 0x200]
        self.kbd = UInput({e.EV_KEY: keys}, name="gs-scenario-keyboard")
        time.sleep(1.0)     # libinput/KWin pick the new devices up

    def close(self):
        self.ptr.close()
        self.kbd.close()

    def move(self, x, y):
        e = self.e
        self.ptr.write(e.EV_ABS, e.ABS_X, int(x))
        self.ptr.write(e.EV_ABS, e.ABS_Y, int(y))
        self.ptr.syn()
        time.sleep(0.05)

    def click(self, x, y, button="left"):
        e = self.e
        b = {"left": e.BTN_LEFT, "right": e.BTN_RIGHT, "middle": e.BTN_MIDDLE}[button]
        self.move(x, y)
        self.ptr.write(e.EV_KEY, b, 1)
        self.ptr.syn()
        time.sleep(0.06)
        self.ptr.write(e.EV_KEY, b, 0)
        self.ptr.syn()
        time.sleep(0.1)

    def wheel(self, clicks):
        e = self.e
        for _ in range(abs(clicks)):
            self.ptr.write(e.EV_REL, e.REL_WHEEL, -1 if clicks > 0 else 1)   # positive = down
            self.ptr.syn()
            time.sleep(0.03)

    def _code(self, k):
        return getattr(self.e, "KEY_" + {"alt": "LEFTALT", "ctrl": "LEFTCTRL", "shift": "LEFTSHIFT",
                                         "super": "LEFTMETA", "meta": "LEFTMETA", "win": "LEFTMETA",
                                         "esc": "ESC", "enter": "ENTER", "return": "ENTER"}
                       .get(k.lower(), k.upper()))

    def key_down(self, k):
        self.kbd.write(self.e.EV_KEY, self._code(k), 1)
        self.kbd.syn()
        time.sleep(0.03)

    def key_up(self, k):
        self.kbd.write(self.e.EV_KEY, self._code(k), 0)
        self.kbd.syn()
        time.sleep(0.03)

    def key(self, combo, hold=0.05):
        e = self.e
        codes = [self._code(k) for k in combo.split("+")]
        for c in codes:
            self.kbd.write(e.EV_KEY, c, 1)
            self.kbd.syn()
            time.sleep(0.02)
        time.sleep(hold)
        for c in reversed(codes):
            self.kbd.write(e.EV_KEY, c, 0)
            self.kbd.syn()
            time.sleep(0.02)
        time.sleep(0.05)

    def type(self, text):
        shifted = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8',
                   '(': '9', ')': '0', '_': 'minus', '+': 'equal', '{': 'leftbrace', '}': 'rightbrace',
                   ':': 'semicolon', '"': 'apostrophe', '<': 'comma', '>': 'dot', '?': 'slash',
                   '|': 'backslash', '~': 'grave'}
        plain = {' ': 'space', '-': 'minus', '=': 'equal', '[': 'leftbrace', ']': 'rightbrace',
                 ';': 'semicolon', "'": 'apostrophe', ',': 'comma', '.': 'dot', '/': 'slash',
                 '\\': 'backslash', '`': 'grave', '\n': 'enter'}
        for ch in text:
            if ch.isupper():
                self.key("shift+" + ch.lower())
            elif ch in shifted:
                self.key("shift+" + shifted[ch])
            else:
                self.key(plain.get(ch, ch))


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    cmd = a[0]
    if cmd == "windows":
        print(json.dumps(windows(), indent=1, ensure_ascii=False))
    elif cmd == "activate":
        print(activate(a[1]))
    elif cmd == "shot":
        print(shot(a[1]))
    elif cmd == "cursor":
        print(cursor())
    elif cmd == "screen":
        print(screen())
    elif cmd in ("click", "move", "key", "type", "wheel"):
        inp = Input()
        try:
            if cmd == "click":
                inp.click(float(a[1]), float(a[2]), a[3] if len(a) > 3 else "left")
            elif cmd == "move":
                inp.move(float(a[1]), float(a[2]))
            elif cmd == "key":
                inp.key(a[1])
            elif cmd == "wheel":         # wheel CLICKS [X Y]: positive scrolls down
                if len(a) > 3:
                    inp.move(float(a[2]), float(a[3]))
                inp.wheel(int(a[1]))
            else:
                inp.type(a[1])
            time.sleep(0.2)
        finally:
            inp.close()
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
