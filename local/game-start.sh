#!/bin/bash
# game-start.sh [APPID=3132990]: in the test VM guest, over ssh: start Steam
# with a game in the logged-in Plasma session and wait until the game holds
# vidmem (3132990: Black Myth Wukong Benchmark Tool, sits in its menu with
# ~2.2 GB). Prints the game's pid. Steam must be logged in already (the user
# does that).
cd "$(dirname "$0")" || exit 1
app=${1:-3132990}
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus WAYLAND_DISPLAY=wayland-1 DISPLAY=:0
XAUTHORITY=$(ls "$XDG_RUNTIME_DIR"/xauth_* 2>/dev/null | head -1); export XAUTHORITY
[[ -S $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY ]] || { echo "no Plasma session" >&2; exit 1; }
(setsid nohup steam -applaunch "$app" >/tmp/steam.log 2>&1 &)
for _ in $(seq 90); do
    pid=$(sudo -n ./evict.sh status | awk '$2 == "GameThread" && $3 >= 1000 {print $1; exit}')
    [[ -n $pid ]] && { sleep 10; echo "$pid"; exit 0; }
    sleep 2
done
echo "the game did not start in 180 s" >&2; exit 1
