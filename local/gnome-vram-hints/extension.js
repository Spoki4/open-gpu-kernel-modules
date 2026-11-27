// NVIDIA VRAM window hints for GNOME Shell: tells the patched NVIDIA driver
// which processes the user sees, so it keeps their video memory and moves
// hidden ones to system memory (local/vidmem eviction,
// /proc/driver/nvidia/vidmem_hints; the protocol: local/HINTS.md).
//
// The same rules as the KWin script (kwin-vram-hints/) and vram-hintd: per
// process (pid of the window's owner) f = owns the focused window and it
// shows, v = has a window the user can see, h = all its windows are
// minimized, on another workspace or covered by opaque windows above (less
// than 2 % of it showing); a second word "g" marks a shown fullscreen window.
// An unmanaged (override-redirect) Xwayland window over 90 % of a monitor is
// a fullscreen X11 game: shown, covering what is below, and the focus when
// the focused window does not show. The whole table is written when it
// changes (on focus, stacking, workspace and fullscreen changes at once, the
// rest by a 250 ms poll); a write to the stock driver (no such file) fails
// and is logged once. Disabling the extension (logout, gnome-extensions
// disable) writes an empty table: the driver stops using hints and brings
// back what it moved out. It stays enabled on the lock screen
// ("unlock-dialog"), so locking does not bring everything back.
import GLib from 'gi://GLib';
import Gio from 'gi://Gio';
import Meta from 'gi://Meta';
import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';

// VRAM_HINTS_FILE in gnome-shell's environment: another file (tests).
const HINTS = GLib.getenv('VRAM_HINTS_FILE') || '/proc/driver/nvidia/vidmem_hints';
const MIN_SHARE = 0.02;
const BIG = 0.9;

// Whole pixels: fractional scaling leaves slivers that would count as "not
// covered".
function px(r) {
    const x = Math.round(r.x), y = Math.round(r.y);
    return {x, y, w: Math.round(r.x + r.width) - x, h: Math.round(r.y + r.height) - y};
}

// Rectangle r minus rectangle c: up to four pieces of r outside c.
function subtract(r, c) {
    const x1 = Math.max(r.x, c.x), y1 = Math.max(r.y, c.y);
    const x2 = Math.min(r.x + r.w, c.x + c.w), y2 = Math.min(r.y + r.h, c.y + c.h);
    if (x1 >= x2 || y1 >= y2)
        return [r];
    const out = [];
    if (r.y < y1) out.push({x: r.x, y: r.y, w: r.w, h: y1 - r.y});
    if (y2 < r.y + r.h) out.push({x: r.x, y: y2, w: r.w, h: r.y + r.h - y2});
    if (r.x < x1) out.push({x: r.x, y: y1, w: x1 - r.x, h: y2 - y1});
    if (x2 < r.x + r.w) out.push({x: x2, y: y1, w: r.x + r.w - x2, h: y2 - y1});
    return out;
}

function clip(r, m) {
    const x1 = Math.max(r.x, m.x), y1 = Math.max(r.y, m.y);
    const x2 = Math.min(r.x + r.w, m.x + m.w), y2 = Math.min(r.y + r.h, m.y + m.h);
    return (x1 < x2 && y1 < y2) ? {x: x1, y: y1, w: x2 - x1, h: y2 - y1} : null;
}

// Share (0..1) of window i on a monitor and not under the opaque windows
// above it.
function visibleShare(order, i, monitors) {
    const r = order[i].rect;
    const total = r.w * r.h;
    if (total <= 0)
        return 0;
    let rest = monitors.map(m => clip(r, m)).filter(p => p !== null);
    for (let j = i + 1; j < order.length && rest.length > 0; j++) {
        const o = order[j];
        if (!o.shown || !o.opaque)
            continue;
        let next = [];
        for (const p of rest)
            next = next.concat(subtract(p, o.rect));
        rest = next;
        if (rest.length > 64)       // pathological layouts: stop splitting
            break;
    }
    let left = 0;
    for (const p of rest)
        left += p.w * p.h;
    return Math.min(1, left / total);
}

function overMonitor(r, monitors) {
    for (const m of monitors) {
        const c = clip(r, m);
        if (c && c.w * c.h >= BIG * m.w * m.h)
            return true;
    }
    return false;
}

export default class VramHintsExtension extends Extension {
    enable() {
        this._last = null;
        this._wrote = false;
        this._error = null;
        this._idle = 0;
        this._signals = [];
        this._myPid = new Gio.Credentials().get_unix_pid();
        const on = (obj, name) => this._signals.push([obj, obj.connect(name, () => this._queue())]);
        on(global.display, 'notify::focus-window');
        on(global.display, 'window-created');
        on(global.display, 'restacked');
        on(global.display, 'in-fullscreen-changed');
        on(global.workspace_manager, 'active-workspace-changed');
        on(global.window_manager, 'minimize');
        on(global.window_manager, 'unminimize');
        on(global.window_manager, 'destroy');
        // The shell exits (logout, SIGTERM) without disabling extensions:
        // turn the hints off on its shutdown signal.
        try {
            this._signals.push([global, global.connect('shutdown', () => {
                if (this._wrote)
                    this._write('\n');
                this._wrote = false;
            })]);
        } catch (e) {
            console.warn(`nvidia-vram-hints: no shutdown signal (${e.message}): the table stays after the shell exits`);
        }
        this._timer = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 250, () => {
            this._publish();
            return GLib.SOURCE_CONTINUE;
        });
        this._publish();
    }

    disable() {
        for (const [obj, id] of this._signals)
            obj.disconnect(id);
        this._signals = [];
        if (this._timer)
            GLib.source_remove(this._timer);
        this._timer = 0;
        if (this._idle)
            GLib.source_remove(this._idle);
        this._idle = 0;
        // A stale table is worse than none: the driver would keep acting on
        // windows nobody reports.
        if (this._wrote)
            this._write('\n');
        this._wrote = false;
        this._last = null;
    }

    _queue() {
        if (this._idle)
            return;
        this._idle = GLib.idle_add(GLib.PRIORITY_DEFAULT_IDLE, () => {
            this._idle = 0;
            this._publish();
            return GLib.SOURCE_REMOVE;
        });
    }

    // Windows bottom to top and the monitors.
    _scene() {
        const display = global.display;
        const ws = global.workspace_manager.get_active_workspace();
        const focus = display.focus_window;
        const monitors = [];
        for (let i = 0; i < display.get_n_monitors(); i++)
            monitors.push(px(display.get_monitor_geometry(i)));
        const order = [];
        for (const actor of global.get_window_actors()) {
            const w = actor.get_meta_window();
            if (!w)
                continue;
            const rect = px(w.get_frame_rect());
            const label = w.get_wm_class() || '';
            if (w.is_override_redirect()) {
                // Menus and tooltips say nothing; one over a monitor is a game.
                if (actor.visible && overMonitor(rect, monitors))
                    order.push({pid: w.get_pid(), rect, shown: true, opaque: true, full: true,
                                focus: false, unmanaged: true, label: `or:${label}`});
                continue;
            }
            const type = w.get_window_type();
            const normal = type === Meta.WindowType.NORMAL || type === Meta.WindowType.DIALOG ||
                           type === Meta.WindowType.MODAL_DIALOG;
            const full = w.is_fullscreen();
            const shown = !w.minimized && w.showing_on_its_workspace() && w.located_on_workspace(ws);
            order.push({pid: w.get_pid(), rect, shown, opaque: (normal || full) && actor.opacity === 255,
                        full, focus: w === focus, unmanaged: false, label});
        }
        return [order, monitors];
    }

    _table() {
        const [order, monitors] = this._scene();
        const share = order.map((w, i) => w.shown ? visibleShare(order, i, monitors) : -1);
        // The focus: the focused window if it shows, else the topmost shown
        // fullscreen window (an override-redirect game takes the screen
        // without the focus).
        let fg = null;
        order.forEach((w, i) => {
            if (w.focus && share[i] >= MIN_SHARE)
                fg = w;
        });
        for (let i = order.length - 1; fg === null && i >= 0; i--) {
            const w = order[i];
            if (share[i] >= MIN_SHARE && (w.unmanaged || (w.full && overMonitor(w.rect, monitors))))
                fg = w;
        }
        const state = new Map(), game = new Set();
        order.forEach((w, i) => {
            if (!(w.pid > 0) || w.pid === this._myPid)
                return;
            let s = share[i] >= MIN_SHARE ? 2 : 1;
            if (w === fg)
                s = 3;
            if ((state.get(w.pid) ?? 0) < s)
                state.set(w.pid, s);
            if (s >= 2 && w.full)
                game.add(w.pid);
        });
        return [...state.keys()].sort((a, b) => a - b)
            .map(p => `${p} ${'?hvf'[state.get(p)]}${game.has(p) ? ' g' : ''}\n`).join('');
    }

    _publish() {
        let text;
        try {
            text = this._table();
        } catch (e) {
            console.error(`nvidia-vram-hints: ${e}`);
            return;
        }
        if (text === this._last)
            return;
        if (this._write(text)) {
            this._last = text;
            this._wrote = true;
        }
    }

    // One open, one write: the driver takes what one open writes as the
    // whole table, and a write without lines turns the hints off. Not
    // replace_contents() on /proc: it writes a temporary file and renames it.
    _write(text) {
        const bytes = new TextEncoder().encode(text || '\n');
        try {
            const file = Gio.File.new_for_path(HINTS);
            if (HINTS.startsWith('/proc/')) {
                const out = file.append_to(Gio.FileCreateFlags.NONE, null);
                out.write_all(bytes, null);
                out.close(null);
            } else {
                file.replace_contents(bytes, null, false, Gio.FileCreateFlags.NONE, null);
            }
        } catch (e) {
            if (this._error !== e.message)
                console.warn(`nvidia-vram-hints: cannot write ${HINTS}: ${e.message}`);
            this._error = e.message;
            return false;
        }
        if (this._error !== null)
            console.warn(`nvidia-vram-hints: writing ${HINTS} again`);
        this._error = null;
        return true;
    }
}
