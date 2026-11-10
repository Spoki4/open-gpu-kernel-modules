#!/bin/bash
# Records a gaming session for later study: driver events, kernel and system
# journal, VRAM/RAM/PSI/power samples, MangoHud frame times, and a Perfetto
# flight recorder cloned on every mark (hotkey). See gamerec/gamerec.py.
#
#   sudo ./gamerec.sh install       once: the root service (again after changing gamerec.py)
#   ./gamerec.sh record [--hud]      by hand, Ctrl-C to stop
#   ./gamerec.sh auto start|end      from GameMode's hook: records while GameMode is on
#   ./gamerec.sh mark [TEXT]         mark a problem (hotkey Meta+Ctrl+M)
#   ./gamerec.sh hud [on|off]        show / hide MangoHud (hotkey Meta+Ctrl+H)
#   ./gamerec.sh hotkey | steam | stop | status | report [DIR]
exec python3 "$(dirname -- "$(readlink -f -- "${BASH_SOURCE[0]}")")/gamerec/gamerec.py" "$@"
