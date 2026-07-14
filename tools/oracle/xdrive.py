#!/usr/bin/env python3
"""xdrive.py — minimal X11 GUI driver for headless (Xvfb) automation.

Injects synthetic keyboard/mouse events via the XTest extension and inspects
the window tree. Built to drive Dolphin's GUI-only FIFO recorder from scripts
(tools/oracle/record_fifo.sh) on a virtual display; generic enough for any Qt app.

Usage (mini-script; commands run in order, DISPLAY from env):
  xdrive.py key alt+t            # press+release a chord (keysym names, '+'-joined)
  xdrive.py type "hello.dff"     # type literal text
  xdrive.py click 100 200        # left-click at root coords
  xdrive.py click 100 200 3      # button 3
  xdrive.py move 100 200         # move pointer only
  xdrive.py sleep 0.5
  xdrive.py windows              # print visible top-level windows (id, geom, name)
  xdrive.py activate <name-substr>   # raise+focus first window whose name matches
Commands can be chained:  xdrive.py key alt+t sleep 0.3 key Down key Return

Fails loudly: unknown keysym, no matching window, or missing XTest → exit 1.
"""
import sys
import time

from Xlib import X, XK, display as xdisplay
from Xlib.ext import xtest
from Xlib.protocol import event as xevent


def fail(msg: str):
    print(f"[xdrive] FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


class Driver:
    def __init__(self):
        self.d = xdisplay.Display()
        if not self.d.has_extension("XTEST"):
            fail("X server lacks XTEST extension")
        self.root = self.d.screen().root

    # -- keyboard -------------------------------------------------------
    KEYSYM_ALIASES = {
        "alt": "Alt_L", "ctrl": "Control_L", "shift": "Shift_L",
        "super": "Super_L", "enter": "Return", "esc": "Escape",
        "tab": "Tab", "space": "space", "menu": "Menu",
    }

    def _keycode(self, name: str) -> int:
        name = self.KEYSYM_ALIASES.get(name.lower(), name)
        ks = XK.string_to_keysym(name)
        if ks == 0:
            fail(f"unknown keysym '{name}'")
        kc = self.d.keysym_to_keycode(ks)
        if kc == 0:
            fail(f"keysym '{name}' has no keycode on this display")
        return kc

    def key(self, chord: str):
        parts = chord.split("+")
        codes = [self._keycode(p) for p in parts]
        for kc in codes:
            xtest.fake_input(self.d, X.KeyPress, kc)
        for kc in reversed(codes):
            xtest.fake_input(self.d, X.KeyRelease, kc)
        self.d.sync()

    def type_text(self, text: str):
        # Literal typing via per-char keysym lookup; shifted chars handled by
        # checking whether the keysym sits in the shifted column.
        shift = self._keycode("Shift_L")
        for ch in text:
            ks = XK.string_to_keysym(ch)
            if ks == 0:
                # names for common punctuation python-xlib can't map from the char
                named = {" ": "space", "/": "slash", ".": "period", "-": "minus",
                         "_": "underscore", ":": "colon", "~": "asciitilde"}.get(ch)
                if not named:
                    fail(f"cannot type character {ch!r}")
                ks = XK.string_to_keysym(named)
            kc = self.d.keysym_to_keycode(ks)
            if kc == 0:
                fail(f"no keycode for character {ch!r}")
            needs_shift = ch.isupper() or ch in "_:~<>?\"{}|+!@#$%^&*()"
            if needs_shift:
                xtest.fake_input(self.d, X.KeyPress, shift)
            xtest.fake_input(self.d, X.KeyPress, kc)
            xtest.fake_input(self.d, X.KeyRelease, kc)
            if needs_shift:
                xtest.fake_input(self.d, X.KeyRelease, shift)
            self.d.sync()
            time.sleep(0.01)

    # -- mouse ----------------------------------------------------------
    def move(self, x: int, y: int):
        xtest.fake_input(self.d, X.MotionNotify, x=x, y=y)
        self.d.sync()

    def click(self, x: int, y: int, button: int = 1):
        self.move(x, y)
        time.sleep(0.05)
        xtest.fake_input(self.d, X.ButtonPress, button)
        self.d.sync()
        time.sleep(0.05)
        xtest.fake_input(self.d, X.ButtonRelease, button)
        self.d.sync()

    # -- windows --------------------------------------------------------
    def _window_name(self, w) -> str:
        try:
            net = w.get_full_property(self.d.intern_atom("_NET_WM_NAME"), 0)
            if net and net.value:
                v = net.value
                return v.decode("utf-8", "replace") if isinstance(v, bytes) else str(v)
            nm = w.get_wm_name()
            return nm or ""
        except Exception:
            return ""

    def _iter_windows(self):
        for w in self.root.query_tree().children:
            try:
                attrs = w.get_attributes()
                if attrs.map_state != X.IsViewable:
                    continue
                geo = w.get_geometry()
                yield w, geo, self._window_name(w)
            except Exception:
                continue

    def windows(self):
        for w, geo, name in self._iter_windows():
            print(f"0x{w.id:08x}  {geo.x:5d},{geo.y:5d} {geo.width:5d}x{geo.height:<5d}  {name!r}")

    def activate(self, substr: str):
        for w, geo, name in self._iter_windows():
            if substr.lower() in name.lower():
                w.configure(stack_mode=X.Above)
                w.set_input_focus(X.RevertToParent, X.CurrentTime)
                self.d.sync()
                print(f"[xdrive] activated 0x{w.id:08x} {name!r}")
                return
        fail(f"no visible window matching {substr!r}")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    drv = Driver()
    i = 1
    while i < len(argv):
        cmd = argv[i]
        if cmd == "key":
            drv.key(argv[i + 1]); i += 2
        elif cmd == "type":
            drv.type_text(argv[i + 1]); i += 2
        elif cmd == "click":
            if i + 3 < len(argv) and argv[i + 3].isdigit():
                drv.click(int(argv[i + 1]), int(argv[i + 2]), int(argv[i + 3])); i += 4
            else:
                drv.click(int(argv[i + 1]), int(argv[i + 2])); i += 3
        elif cmd == "move":
            drv.move(int(argv[i + 1]), int(argv[i + 2])); i += 3
        elif cmd == "sleep":
            time.sleep(float(argv[i + 1])); i += 2
        elif cmd == "windows":
            drv.windows(); i += 1
        elif cmd == "activate":
            drv.activate(argv[i + 1]); i += 2
        else:
            fail(f"unknown command '{cmd}'")
        time.sleep(0.05)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
