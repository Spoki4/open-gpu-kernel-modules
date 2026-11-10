// NVIDIA VRAM window hints: tells the patched NVIDIA driver which processes
// the user sees, so it keeps their video memory and moves hidden ones to
// system memory (local/vidmem eviction, /proc/driver/nvidia/vidmem_hints).
//
// Per process (pid of the window's owner): f = owns the active window,
// v = has a window the user can see, h = all its windows are minimized, on
// another desktop or activity, or covered by opaque windows above (together:
// less than 2% of the window still shows; Steam under several windows,
// 2026-11-02, counted as visible while one window had to cover it whole).
// A second word "g" marks a process with a fullscreen window (the driver
// takes it for a game when it keeps the GPU busy). The driver applies a
// state to the process's children too (browser GPU processes).
//
// The foreground is the active window when it shows (BACKLOG R4: with CoD
// fullscreen on one monitor and the user in Chrome on the other, the game
// used to stay the foreground). An unmanaged (override-redirect) window
// covering a screen is a fullscreen X11 game: shown, covering what is below,
// and the foreground when on top and the active window does not show (it
// cannot be activated itself; 2026-11-04: Superposition's fullscreen window
// was skipped as "not managed", the game counted as hidden and its launcher
// behind it as the foreground, so the driver never made room for the game).
// The whole table is written on every change; a write to the stock driver
// (no such file) simply fails; older drivers ignore the second word.
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
    property string lastDebug: ""
    property bool debugLog: false

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

    // Rectangle r minus rectangle c: up to four pieces of r outside c.
    function subtract(r, c) {
        var x1 = Math.max(r.x, c.x), y1 = Math.max(r.y, c.y);
        var x2 = Math.min(r.x + r.w, c.x + c.width), y2 = Math.min(r.y + r.h, c.y + c.height);
        if (x1 >= x2 || y1 >= y2)
            return [r];
        var out = [];
        if (r.y < y1) out.push({ x: r.x, y: r.y, w: r.w, h: y1 - r.y });                          // above
        if (y2 < r.y + r.h) out.push({ x: r.x, y: y2, w: r.w, h: r.y + r.h - y2 });             // below
        if (r.x < x1) out.push({ x: r.x, y: y1, w: x1 - r.x, h: y2 - y1 });                       // left
        if (x2 < r.x + r.w) out.push({ x: x2, y: y1, w: r.x + r.w - x2, h: y2 - y1 });          // right
        return out;
    }

    // Share of g (0..1) not covered by the covering windows above index i.
    // Whole pixels: fractional scaling gives edges like 1502.8571428571431
    // vs 1502.857142857143, a sliver that would count as "not covered".
    function px(r) {
        var x = Math.round(r.x), y = Math.round(r.y);
        return { x: x, y: y, width: Math.round(r.x + r.width) - x, height: Math.round(r.y + r.height) - y };
    }

    function visibleShare(order, i, g0) {
        var g = px(g0);
        var rest = [{ x: g.x, y: g.y, w: g.width, h: g.height }];
        var total = g.width * g.height;
        if (total <= 0)
            return 0;
        for (var j = i + 1; j < order.length && rest.length > 0; j++) {
            if (!covers(order[j]))
                continue;
            var next = [];
            for (var k = 0; k < rest.length; k++)
                next = next.concat(subtract(rest[k], px(order[j].frameGeometry)));
            rest = next;
            if (rest.length > 64)        // pathological layouts: stop splitting
                break;
        }
        var left = 0;
        for (var m = 0; m < rest.length; m++)
            left += rest[m].w * rest[m].h;
        return left / total;
    }

    // An unmanaged window over at least 90% of a screen (a fullscreen X11
    // game). Small unmanaged ones (menus, tooltips) are ignored altogether.
    function bigUnmanaged(w) {
        if (!w || w.managed || w.deleted)
            return false;
        var g = px(w.frameGeometry);
        var screens = Workspace.screens;
        for (var i = 0; i < screens.length; i++) {
            var s = screens[i].geometry;
            var x1 = Math.max(g.x, s.x), y1 = Math.max(g.y, s.y);
            var x2 = Math.min(g.x + g.width, s.x + s.width), y2 = Math.min(g.y + g.height, s.y + s.height);
            if (x2 > x1 && y2 > y1 && (x2 - x1) * (y2 - y1) >= 0.9 * s.width * s.height)
                return true;
        }
        return false;
    }

    // Shown at all, before looking at what covers it.
    function shown(w) {
        if (bigUnmanaged(w))
            return true;
        return w && w.managed && !w.deleted && !w.minimized && !w.hidden &&
               onCurrentDesktop(w) && onCurrentActivity(w);
    }

    // Can this window hide the ones below it?
    function covers(w) {
        if (bigUnmanaged(w))
            return true;
        return shown(w) && w.opacity >= 1.0 && (w.normalWindow || w.fullScreen) && !w.popupWindow;
    }

    function snapshot() {
        var order = Workspace.stackingOrder;     // bottom to top
        var state = {};                          // pid -> 1 hidden, 2 visible, 3 foreground
        var full = {};                           // pid -> has a fullscreen window
        var active = Workspace.activeWindow;
        var i, j;
        var dbg = "";

        // The topmost fullscreen unmanaged window, if any: the game in front.
        var game = null;
        for (i = order.length - 1; i >= 0 && game === null; i--) {
            if (bigUnmanaged(order[i]))
                game = order[i];
        }

        // The active window, if it shows at all (not covered by the game).
        var activeShows = false;
        for (i = 0; i < order.length; i++) {
            if (active && order[i] === active && shown(active) &&
                visibleShare(order, i, active.frameGeometry) >= 0.02)
                activeShows = true;
        }

        for (i = 0; i < order.length; i++) {
            var w = order[i];
            if (!w || w.deleted || !w.pid || w.pid <= 0)
                continue;
            if (!w.managed && !bigUnmanaged(w))
                continue;                        // menus, tooltips: say nothing
            var s = 1;
            var share = shown(w) ? visibleShare(order, i, w.frameGeometry) : -1;
            if (shown(w))
                s = (share < 0.02) ? 1 : 2;
            dbg += " " + w.pid + ":" + w.resourceClass + "=" + (share < 0 ? "off" : share.toFixed(3));
            // Foreground: the active window if it shows; else the game on top.
            if (activeShows ? (w === active) : (game !== null && w === game))
                s = 3;
            if (!(w.pid in state) || state[w.pid] < s)
                state[w.pid] = s;
            if (s >= 2 && (bigUnmanaged(w) || w.fullScreen))
                full[w.pid] = true;
        }

        lastDebug = dbg;
        var pids = Object.keys(state).map(Number).sort(function (a, b) { return a - b; });
        var text = "";
        for (i = 0; i < pids.length; i++)
            text += pids[i] + " " + "?hvf".charAt(state[pids[i]]) + (full[pids[i]] ? " g" : "") + "\n";
        return text;
    }

    function publish() {
        var text = snapshot();
        if (text === lastSnapshot)
            return;
        var x = new XMLHttpRequest();
        x.open("PUT", hintsUrl);
        x.send(text);
        // Visible share per window of this snapshot (journalctl | grep nvidia-vram-hints).
        if (debugLog)
            console.warn("nvidia-vram-hints:" + lastDebug);
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
