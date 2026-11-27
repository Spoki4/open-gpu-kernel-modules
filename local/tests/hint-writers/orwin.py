#!/usr/bin/env python3
# An override-redirect window over the whole screen, as a fullscreen X11 game
# makes (it bypasses the window manager): stays SECONDS.
#   orwin.py [SECONDS]
import os, sys, time
from Xlib import X, display
d = display.Display()
s = d.screen()
w = s.root.create_window(0, 0, s.width_in_pixels, s.height_in_pixels, 0, s.root_depth, X.InputOutput,
                         X.CopyFromParent, override_redirect=True, background_pixel=s.black_pixel)
w.change_property(d.intern_atom("_NET_WM_PID"), d.intern_atom("CARDINAL"), 32, [os.getpid()])
w.map()
d.sync()
time.sleep(float(sys.argv[1]) if len(sys.argv) > 1 else 3)
