#!/usr/bin/env python3
# A stand-in for Hyprland's two sockets, for vram-hintd's tests: Hyprland
# needs a DRM device and cannot run headless in a container. It answers
# j/monitors, j/clients, j/activewindow and j/getoption from a scene file in
# the JSON Hyprland writes (format 0.57, src/ipc/s1/Commands.cpp, or "old":
# 0.41 and before, workspaces by id, fullscreen a bool), and sends an event
# line on the event socket whenever the scene file changes.
#   hyprland-ipc.py SCENE.json      (HYPRLAND_INSTANCE_SIGNATURE, XDG_RUNTIME_DIR set)
# A scene: {"format": "0.57"|"old", "inactive_opacity": 1.0,
#   "monitors": [{"x","y","width","height","scale","ws","special"}],
#   "clients": [{"pid","ws","at","size","floating","fullscreen","hidden","class"}]
#   (bottom to top), "active": index into clients or null}
import json, os, socket, sys, threading, time

scene_path = sys.argv[1]
base = os.path.join(os.environ["XDG_RUNTIME_DIR"], "hypr", os.environ["HYPRLAND_INSTANCE_SIGNATURE"])
os.makedirs(base, exist_ok=True)


def load():
    try:
        with open(scene_path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"monitors": [], "clients": []}


def ws_obj(name, new):
    if not name:
        return {"address": "", "type": "", "name": ""} if new else {"id": 0, "name": ""}
    special = name.startswith("special:")
    if new:
        o = {"address": ("special:" if special else "") + name.split(":")[-1]}
        if not special and name.isdigit():
            o["id"] = int(name)
        o.update({"type": "special" if special else "regular", "name": name})
        return o
    return {"id": (-98 if special else int(name)) if (special or name.isdigit()) else 1, "name": name}


def answer(cmd):
    s = load()
    new = s.get("format", "0.57") != "old"
    if cmd == "j/monitors":
        return [dict({"id": i, "name": "M%d" % i, "x": m["x"], "y": m["y"], "width": m["width"],
                      "height": m["height"], "scale": m.get("scale", 1.0), "transform": m.get("transform", 0),
                      "activeWorkspace": ws_obj(m["ws"], new),
                      "specialWorkspace": ws_obj(m.get("special", ""), new), "disabled": False})
                for i, m in enumerate(s["monitors"])]
    if cmd == "j/clients":
        out = []
        for i, c in enumerate(s["clients"]):
            o = {"address": "0x%x" % (0x1000 + i), "mapped": True, "hidden": c.get("hidden", False),
                 "at": c["at"], "size": c["size"], "workspace": ws_obj(c["ws"], new),
                 "floating": c.get("floating", False), "class": c.get("class", "app"), "pid": c["pid"],
                 "xwayland": False, "pinned": False}
            fs = c.get("fullscreen", 0)          # 0 none, 1 maximized, 2 fullscreen
            if new:
                o.update({"visible": True, "fullscreen": fs, "fullscreenClient": fs})
            else:
                o.update({"fullscreen": fs != 0, "fullscreenMode": 0 if fs == 2 else 1})
            out.append(o)
        return out
    if cmd == "j/activewindow":
        a = s.get("active")
        return {"address": "0x%x" % (0x1000 + a)} if a is not None else {}
    if cmd.startswith("j/getoption "):
        opt = cmd.split(" ", 1)[1]
        v = s.get("inactive_opacity", 1.0) if opt == "decoration:inactive_opacity" else 1.0
        return {"option": opt, "float": v, "set": True}
    return None


def serve_requests():
    path = os.path.join(base, ".socket.sock")
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(16)
    while True:
        c, _ = srv.accept()
        cmd = c.recv(4096).decode()
        r = answer(cmd)
        c.sendall(("unknown request" if r is None else json.dumps(r, indent=4)).encode())
        c.close()


listeners = []


def serve_events():
    path = os.path.join(base, ".socket2.sock")
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(16)
    while True:
        c, _ = srv.accept()
        listeners.append(c)


threading.Thread(target=serve_requests, daemon=True).start()
threading.Thread(target=serve_events, daemon=True).start()
last = None
while True:
    try:
        m = os.stat(scene_path).st_mtime_ns
    except OSError:
        m = None
    if m != last:
        last = m
        for c in list(listeners):
            try:
                c.sendall(b"activewindowv2>>1000\n")
            except OSError:
                listeners.remove(c)
    if os.path.exists(scene_path + ".quit"):
        for c in listeners:
            c.close()
        sys.exit(0)
    time.sleep(0.05)
