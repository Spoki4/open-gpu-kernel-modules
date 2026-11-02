// NVIDIA VRAM window hints: tells the patched NVIDIA driver which processes
// the user sees, so it keeps their video memory and moves hidden ones to
// system memory (local/vidmem eviction, /proc/driver/nvidia/vidmem_hints).
//
// Per process (pid of the window's owner): f = owns the active window,
// v = has a window the user can see, h = all its windows are minimized, on
// another desktop or activity, or fully covered by an opaque window above.
// The driver applies a state to the process's children too (browser GPU
// processes). The whole table is written on every change; a write to the
// stock driver (no such file) simply fails.
//
// Needs QML_XHR_ALLOW_FILE_WRITE=1 in KWin's environment
// (~/.config/plasma-workspace/env/nvidia-vram-hints.sh, see install.sh).
import QtQuick
import org.kde.kwin

Item {
    id: root

    readonly property string hintsUrl: "file:///proc/driver/nvidia/vidmem_hints"
    property string lastSnapshot: ""
    property int writes: 0

    function onCurrentDesktop(w) {
        if (w.onAllDesktops)
            return true;
        var ds = w.desktops;
        for (var i = 0; i < ds.length; i++) {
            if (ds[i] === Workspace.currentDesktop)
                return true;
        }
        return false;
    }

    function onCurrentActivity(w) {
        var as = w.activities;
        if (!as || as.length === 0)
            return true;
        for (var i = 0; i < as.length; i++) {
            if (as[i] === Workspace.currentActivity)
                return true;
        }
        return false;
    }

    function contains(a, b) {
        return a.x <= b.x && a.y <= b.y &&
               a.x + a.width >= b.x + b.width && a.y + a.height >= b.y + b.height;
    }

    // Shown at all, before looking at what covers it.
    function shown(w) {
        return w && w.managed && !w.deleted && !w.minimized && !w.hidden &&
               onCurrentDesktop(w) && onCurrentActivity(w);
    }

    // Can this window hide the ones below it?
    function covers(w) {
        return shown(w) && w.opacity >= 1.0 && (w.normalWindow || w.fullScreen) && !w.popupWindow;
    }

    function snapshot() {
        var order = Workspace.stackingOrder;     // bottom to top
        var state = {};                          // pid -> 1 hidden, 2 visible, 3 foreground
        var active = Workspace.activeWindow;
        var i, j;

        for (i = 0; i < order.length; i++) {
            var w = order[i];
            if (!w || w.deleted || !w.pid || w.pid <= 0)
                continue;
            var s = 1;
            if (shown(w)) {
                s = 2;
                var g = w.frameGeometry;
                for (j = i + 1; j < order.length; j++) {
                    if (covers(order[j]) && contains(order[j].frameGeometry, g)) {
                        s = 1;
                        break;
                    }
                }
            }
            if (w === active && shown(w))
                s = 3;
            if (!(w.pid in state) || state[w.pid] < s)
                state[w.pid] = s;
        }

        var pids = Object.keys(state).map(Number).sort(function (a, b) { return a - b; });
        var text = "";
        for (i = 0; i < pids.length; i++)
            text += pids[i] + " " + "?hvf".charAt(state[pids[i]]) + "\n";
        return text;
    }

    function publish() {
        var text = snapshot();
        if (text === lastSnapshot)
            return;
        var x = new XMLHttpRequest();
        x.open("PUT", hintsUrl);
        x.send(text);
        lastSnapshot = text;
        writes++;
    }

    // Changes that matter right away (alt-tab, minimize via activation,
    // desktop switch) publish at once; everything else (geometry, minimize
    // of an inactive window, opacity) is caught by the poll.
    Connections {
        target: Workspace
        function onWindowActivated(w) { root.publish(); }
        function onWindowAdded(w) { root.publish(); }
        function onWindowRemoved(w) { root.publish(); }
        function onCurrentDesktopChanged(previous) { root.publish(); }
        function onCurrentActivityChanged(id) { root.publish(); }
    }

    Timer {
        interval: 250
        running: true
        repeat: true
        onTriggered: root.publish()
    }

    Component.onCompleted: publish()
}
