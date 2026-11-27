#!/bin/bash
# The window hint writers for desktops other than KDE (vram-hintd, the GNOME
# extension) against the real compositors, run headless in a container. Per
# desktop, windows are opened, moved between workspaces, minimized, covered,
# made fullscreen and closed, and the table is checked after each step
# (tests/hint-writers/run.sh); the writer must turn the hints off when it
# stops and when the compositor goes away. The image (Arch with sway, niri,
# GNOME Shell, Openbox, xfwm4, i3; about 2 GB) is built on the first run and
# kept, named after its Containerfile. Ends with HINTWRITERS PASS|FAIL.
#
# Without --gpu: on the host, rootless podman, no GPU, no driver: the table
# goes to a file (VRAM_HINTS_FILE). Hyprland needs a DRM device: its IPC comes
# from a stand-in that speaks the JSON of 0.57 and of 0.41
# (hint-writers/hyprland-ipc.py).
#
# With --gpu: as root on a machine with the patched driver loaded and no
# desktop on the GPU (the test VM, a rented machine). The writers write
# /proc/driver/nvidia/vidmem_hints, which takes only pids of the init pid
# namespace (the container shares the host's) owned by the writer's user.
# The image adds the real Hyprland (on the GPU's DRM device, a headless
# output) and NVIDIA's userspace of the loaded release (its .run, fetched
# into ~ if missing). Each desktop also runs the memory scenario: a hidden
# window's process (nvt-idle-a) gives its vidmem to the focused one
# (nvt-game), gets it back when shown, and reads back what it wrote. The
# memory each process has where comes from evict.sh status, sampled here
# every second. hyprland-old needs the stand-in: not run with --gpu. The
# container shares the host's cgroup namespace too: with the host's pids but
# a cgroup root of its own, libsystemd finds no unit for gnome-shell, which
# then does not start Xwayland. podman or, where there is none, docker.
#   ./hint-writers-test.sh [--gpu] [sway niri openbox xfwm4 i3 gnome hyprland hyprland-old]
cd "$(dirname "$0")" || exit 1
gpu=
[[ ${1:-} == --gpu ]] && { gpu=1; shift; }
if [[ -n $gpu ]]; then
    desks=${*:-sway niri openbox xfwm4 i3 gnome hyprland}
else
    desks=${*:-sway niri openbox xfwm4 i3 gnome hyprland hyprland-old}
fi
E=podman rm=(rm -f -t 0)
command -v podman >/dev/null || E=docker rm=(rm -f)
IMG=localhost/vram-hint-writers:$(sha1sum hint-writers/Containerfile | cut -c1-12)
C=vram-hint-writers-$$
if ! $E image inspect "$IMG" >/dev/null 2>&1; then
    echo "building $IMG"
    $E build -q -f hint-writers/Containerfile -t "$IMG" hint-writers >/dev/null || { echo "HINTWRITERS FAIL (image)"; exit 1; }
fi
opts=(-v "$PWD/..:/src:ro")
if [[ -n $gpu ]]; then
    H=/proc/driver/nvidia/vidmem_hints
    [[ $(id -u) == 0 ]] || { echo "--gpu runs as root"; exit 2; }
    [[ -e $H ]] || { echo "HINTWRITERS FAIL (no $H: the patched driver is not loaded)"; exit 1; }
    ver=$(sed -n 's/^NVIDIA_VERSION *= *//p' ../../version.mk)
    grep -q " $ver " /proc/driver/nvidia/version ||
        { echo "HINTWRITERS FAIL (the loaded driver is not $ver: $(head -1 /proc/driver/nvidia/version))"; exit 1; }
    run=$HOME/NVIDIA-Linux-x86_64-$ver.run
    [[ -f $run ]] || curl -fsSL -o "$run" "https://us.download.nvidia.com/XFree86/Linux-x86_64/$ver/NVIDIA-Linux-x86_64-$ver.run" ||
        { echo "HINTWRITERS FAIL (no $run)"; exit 1; }
    GIMG=localhost/vram-hint-writers-gpu:$(cat hint-writers/Containerfile hint-writers/Containerfile.gpu | sha1sum | cut -c1-8)-$ver
    if ! $E image inspect "$GIMG" >/dev/null 2>&1; then
        echo "building $GIMG"
        ctx=$(mktemp -d)
        cp hint-writers/Containerfile.gpu "$ctx/Containerfile"
        cp "$run" "$ctx/NVIDIA.run"
        $E build -q -f "$ctx/Containerfile" --build-arg BASE="$IMG" -t "$GIMG" "$ctx" >/dev/null
        rc=$?
        rm -rf "$ctx"
        (( rc == 0 )) || { echo "HINTWRITERS FAIL (gpu image)"; exit 1; }
    fi
    IMG=$GIMG
    # What evict.sh status says, for run.sh: written whole, every second.
    S=$(mktemp -d /tmp/hint-writers-gpu.XXXX)
    chmod 755 "$S"
    ( while sleep 1; do ../evict.sh status --raw >"$S/status.tmp" 2>&1 && mv "$S/status.tmp" "$S/status"; done ) &
    sampler=$!
    # The DRM device for Hyprland; nothing else uses the GPU here.
    chmod 666 /dev/dri/card* /dev/dri/renderD* 2>/dev/null
    echo >"$H"
    T0=$(date '+%F %T')
    opts+=(--privileged --pid=host --cgroupns=host -v /dev:/dev -v "$S:/gpu:ro" -e GPU=1)
fi
$E run -d --name "$C" "${opts[@]}" "$IMG" sleep infinity >/dev/null || exit 1
trap '$E "${rm[@]}" "$C" >/dev/null; [[ -n $gpu ]] && { kill $sampler; echo >"$H"; rm -rf "$S"; }' EXIT
$E exec "$C" /src/tests/hint-writers/setup-root.sh || { echo "HINTWRITERS FAIL (setup)"; exit 1; }
out=$(mktemp)
for d in $desks; do
    timeout 400 $E exec -u t -e HOME=/home/t -e XDG_RUNTIME_DIR=/run/t "$C" \
        dbus-run-session -- /src/tests/hint-writers/run.sh "$d" 2>/dev/null | tee -a "$out"
    grep -q "^RUN $d " "$out" || echo "RUN $d FAIL (no result)" | tee -a "$out"
    if [[ -n $gpu ]]; then
        # Nothing of this desktop may stay behind: the next one starts clean.
        pkill -u 1000 -x nvt-game 2>/dev/null
        grep -q ' 0 entries' "$H" || { echo "RUN $d FAIL (the table stayed: $(head -1 $H))" | tee -a "$out"; echo >"$H"; }
    fi
done
if [[ -n $gpu ]]; then
    journalctl -k --since "$T0" --no-pager | grep -E 'Xid|BUG:|Oops|general protection|Assertion failed' | tail -5 | sed 's/^/  kernel: /' | tee -a "$out" |
        grep -q . && echo "RUN kernel-log FAIL" | tee -a "$out"
fi
fails=$(grep -c '^RUN .* FAIL' "$out")
rm -f "$out"
(( fails == 0 )) && echo "HINTWRITERS PASS" || echo "HINTWRITERS FAIL $fails"
