#!/bin/bash
# Prints export lines for the logged-in desktop session (the test VM guest's
# own Plasma on the 3080 Ti monitor), taken from plasmashell's environment:
#   eval "$(./session-env.sh)"; spectacle -b -n -f -o /tmp/shot.png
# The Wayland socket name changes whenever SDDM restarts (vm.sh reload).
p=$(pgrep -u "$(id -u)" -x plasmashell | head -1)
[[ -n $p ]] || { echo "session-env.sh: no plasmashell for $(id -un): nobody logged in?" >&2; exit 1; }
tr '\0' '\n' < "/proc/$p/environ" |
    grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|DBUS_SESSION_BUS_ADDRESS|XDG_RUNTIME_DIR|XDG_SESSION_TYPE|XDG_CURRENT_DESKTOP|KDE_FULL_SESSION)=' |
    sed 's/^/export /'
