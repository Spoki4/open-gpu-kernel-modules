// vram-hintd: window hints for the patched NVIDIA driver on Hyprland, sway,
// niri and X11 window managers (Xfce, Cinnamon, MATE, i3, Openbox, ...).
// KDE Plasma and GNOME have writers of their own that run inside the
// compositor (kwin-vram-hints/, gnome-vram-hints/); on those this exits.
//
// It tells the driver which processes the user sees, so that it keeps their
// video memory and moves hidden ones to system memory
// (/proc/driver/nvidia/vidmem_hints, the protocol: local/HINTS.md). Per
// process (the pid of a window's owner): f = owns the window in focus,
// v = has a window that shows, h = has windows and none of them shows
// (another workspace, minimized, scratchpad, covered by opaque windows above:
// less than 2 % left); a second word "g" marks a shown fullscreen window. The
// state is decided as by the KWin script: per window, shown on its monitor's
// current workspace, then the share of it on a monitor and not under an
// opaque window above it.
//
// The compositor is asked for its windows on every event it sends and every
// 250 ms (1 s on X11, where events cover more and each query costs more
// round trips); the table is written only when it changed. When the
// compositor goes away, or on SIGTERM/SIGINT/SIGHUP, an empty snapshot turns
// the hints off, so the driver brings back what it moved out.
//
// Wayland compositors need nothing but libc (the IPC is JSON over a Unix
// socket, parsed here); X11 needs libxcb with its res and randr parts, and
// is left out of a build without them (make X11=0).
//
//   vram-hintd                  run for the session (from its autostart)
//   vram-hintd --once           print the table it would write, write nothing
//   vram-hintd --debug          also log each window's visible share
//   vram-hintd --backend NAME   hyprland, sway, niri or x11 instead of guessing
//   VRAM_HINTS_FILE=PATH        write there instead (tests)
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef HAVE_X11
#define HAVE_X11 1
#endif
#if HAVE_X11
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/res.h>
#endif

#define MIN_SHARE 0.02  // below this much of it showing, a window counts as hidden
#define BIG 0.9         // an unmanaged X11 window over 90 % of a monitor: a fullscreen game

static const char *hints = "/proc/driver/nvidia/vidmem_hints";
static volatile sig_atomic_t stop;
static int debug;

static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("vram-hintd: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { logf_("out of memory"); exit(1); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) { logf_("out of memory"); exit(1); }
    return p;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

// --- JSON (what the compositors' IPC sends) ---------------------------------

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct J {
    int t;
    double n;               // J_NUM, J_BOOL (0/1)
    char *s;                // J_STR
    int len;                // J_ARR, J_OBJ
    struct J **v;
    char **k;               // J_OBJ keys
} J;

typedef struct { const char *p; int bad; } JP;

static void jws(JP *jp)
{
    while (*jp->p == ' ' || *jp->p == '\n' || *jp->p == '\r' || *jp->p == '\t')
        jp->p++;
}

static J *jnew(int t)
{
    J *j = xmalloc(sizeof(*j));
    memset(j, 0, sizeof(*j));
    j->t = t;
    return j;
}

static void jput_utf8(char **o, unsigned c)
{
    char *q = *o;
    if (c < 0x80) *q++ = (char)c;
    else if (c < 0x800) { *q++ = (char)(0xC0 | c >> 6); *q++ = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { *q++ = (char)(0xE0 | c >> 12); *q++ = (char)(0x80 | (c >> 6 & 63)); *q++ = (char)(0x80 | (c & 63)); }
    else { *q++ = (char)(0xF0 | c >> 18); *q++ = (char)(0x80 | (c >> 12 & 63)); *q++ = (char)(0x80 | (c >> 6 & 63)); *q++ = (char)(0x80 | (c & 63)); }
    *o = q;
}

static char *jstring(JP *jp)
{
    const char *p = jp->p + 1, *e = p;
    while (*e && *e != '"') {
        if (*e == '\\' && e[1]) e++;
        e++;
    }
    if (*e != '"') { jp->bad = 1; return NULL; }
    char *out = xmalloc((size_t)(e - p) + 1), *o = out;   // escapes only shrink
    while (p < e) {
        if (*p != '\\') { *o++ = *p++; continue; }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; p++; break;
        case 't': *o++ = '\t'; p++; break;
        case 'r': *o++ = '\r'; p++; break;
        case 'b': *o++ = '\b'; p++; break;
        case 'f': *o++ = '\f'; p++; break;
        case 'u': {
            unsigned c = 0;
            int i;
            for (i = 1; i <= 4 && p + i < e; i++) {
                char h = p[i];
                c = c * 16 + (unsigned)(h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
            }
            p += 5;
            if (c >= 0xD800 && c < 0xDC00 && p + 5 < e && p[0] == '\\' && p[1] == 'u') {
                unsigned d = (unsigned)strtoul((char[5]){p[2], p[3], p[4], p[5], 0}, NULL, 16);
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); p += 6; }
            }
            if (c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) c = '?';
            // A 6-byte escape holds at most 4 bytes of UTF-8: fits.
            jput_utf8(&o, c);
            break;
        }
        default: *o++ = *p++; break;
        }
    }
    *o = 0;
    jp->p = e + 1;
    return out;
}

static J *jvalue(JP *jp, int depth)
{
    jws(jp);
    if (depth > 64) { jp->bad = 1; return NULL; }
    char c = *jp->p;
    J *j;
    if (c == '{' || c == '[') {
        int obj = c == '{';
        j = jnew(obj ? J_OBJ : J_ARR);
        int cap = 0;
        jp->p++;
        jws(jp);
        if (*jp->p == (obj ? '}' : ']')) { jp->p++; return j; }
        for (;;) {
            char *key = NULL;
            jws(jp);
            if (obj) {
                if (*jp->p != '"') { jp->bad = 1; return j; }
                key = jstring(jp);
                if (jp->bad) return j;
                jws(jp);
                if (*jp->p != ':') { free(key); jp->bad = 1; return j; }
                jp->p++;
            }
            J *v = jvalue(jp, depth + 1);
            if (j->len == cap) {
                cap = cap ? cap * 2 : 8;
                j->v = xrealloc(j->v, sizeof(*j->v) * (size_t)cap);
                if (obj) j->k = xrealloc(j->k, sizeof(*j->k) * (size_t)cap);
            }
            j->v[j->len] = v;
            if (obj) j->k[j->len] = key;
            j->len++;
            if (jp->bad) return j;
            jws(jp);
            if (*jp->p == ',') { jp->p++; continue; }
            if (*jp->p == (obj ? '}' : ']')) { jp->p++; return j; }
            jp->bad = 1;
            return j;
        }
    }
    if (c == '"') {
        j = jnew(J_STR);
        j->s = jstring(jp);
        return j;
    }
    if (!strncmp(jp->p, "true", 4)) { jp->p += 4; j = jnew(J_BOOL); j->n = 1; return j; }
    if (!strncmp(jp->p, "false", 5)) { jp->p += 5; return jnew(J_BOOL); }
    if (!strncmp(jp->p, "null", 4)) { jp->p += 4; return jnew(J_NULL); }
    char *end;
    double n = strtod(jp->p, &end);
    if (end == jp->p) { jp->bad = 1; return NULL; }
    jp->p = end;
    j = jnew(J_NUM);
    j->n = n;
    return j;
}

static void jfree(J *j)
{
    if (!j) return;
    for (int i = 0; i < j->len; i++) {
        jfree(j->v[i]);
        if (j->k) free(j->k[i]);
    }
    free(j->v);
    free(j->k);
    free(j->s);
    free(j);
}

static J *jparse(const char *s)
{
    JP jp = { s, 0 };
    J *j = jvalue(&jp, 0);
    if (jp.bad) { jfree(j); return NULL; }
    return j;
}

static J *jget(const J *j, const char *key)
{
    if (!j || j->t != J_OBJ) return NULL;
    for (int i = 0; i < j->len; i++)
        if (!strcmp(j->k[i], key)) return j->v[i];
    return NULL;
}

static J *jat(const J *j, int i)
{
    return (j && j->t == J_ARR && i >= 0 && i < j->len) ? j->v[i] : NULL;
}

static int jlen(const J *j) { return (j && j->t == J_ARR) ? j->len : 0; }
static double jnum(const J *j, double def) { return (j && (j->t == J_NUM || j->t == J_BOOL)) ? j->n : def; }
static int jbool(const J *j, int def) { return (j && j->t == J_BOOL) ? (int)j->n : def; }
static const char *jstr(const J *j, const char *def) { return (j && j->t == J_STR) ? j->s : def; }
static int jis(const J *j, int t) { return j && j->t == t; }

// --- windows and the table --------------------------------------------------

typedef struct { int x, y, w, h; } Rect;

// One window in stacking order. rect: in the compositor's layout coordinates.
// shown: mapped, not minimized, on the current workspace of its monitor,
// before looking at what covers it. opaque: hides what is below (a normal or
// fullscreen window with no translucency; menus, docks, translucent windows
// cover nothing). full: fullscreen, or an unmanaged window over a monitor
// (unmanaged). k1..k3: the sort key that puts it in stacking order.
typedef struct {
    long pid;
    Rect r;
    int shown, opaque, full, focus, unmanaged;
    long k1, k2, k3;
    char label[48];
} Win;

typedef struct {
    Win *w;
    int n, cap;
    Rect *m;
    int nm, capm;
} Scene;

static void scene_reset(Scene *s) { s->n = 0; s->nm = 0; }

// Whole pixels: fractional scaling leaves slivers that would count as "not
// covered".
static Rect px(double x, double y, double w, double h)
{
    Rect r;
    r.x = (int)floor(x + 0.5);
    r.y = (int)floor(y + 0.5);
    r.w = (int)floor(x + w + 0.5) - r.x;
    r.h = (int)floor(y + h + 0.5) - r.y;
    return r;
}

static Win *add_win(Scene *s, long pid, Rect r, const char *label)
{
    if (s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 32;
        s->w = xrealloc(s->w, sizeof(*s->w) * (size_t)s->cap);
    }
    Win *w = &s->w[s->n++];
    memset(w, 0, sizeof(*w));
    w->pid = pid;
    w->r = r;
    w->shown = 1;
    w->opaque = 1;
    w->k3 = s->n;
    snprintf(w->label, sizeof(w->label), "%s", label ? label : "");
    return w;
}

static void add_mon(Scene *s, Rect r)
{
    if (s->nm == s->capm) {
        s->capm = s->capm ? s->capm * 2 : 4;
        s->m = xrealloc(s->m, sizeof(*s->m) * (size_t)s->capm);
    }
    s->m[s->nm++] = r;
}

static int cmp_win(const void *a, const void *b)
{
    const Win *x = a, *y = b;
    if (x->k1 != y->k1) return x->k1 < y->k1 ? -1 : 1;
    if (x->k2 != y->k2) return x->k2 < y->k2 ? -1 : 1;
    return (x->k3 > y->k3) - (x->k3 < y->k3);
}

static int clip(Rect r, Rect m, Rect *out)
{
    int x1 = r.x > m.x ? r.x : m.x, y1 = r.y > m.y ? r.y : m.y;
    int x2 = r.x + r.w < m.x + m.w ? r.x + r.w : m.x + m.w;
    int y2 = r.y + r.h < m.y + m.h ? r.y + r.h : m.y + m.h;
    if (x1 >= x2 || y1 >= y2) return 0;
    out->x = x1; out->y = y1; out->w = x2 - x1; out->h = y2 - y1;
    return 1;
}

#define MAX_PIECES 64

// Rectangle r minus rectangle c: up to four pieces of r outside c, appended.
static int subtract(Rect r, Rect c, Rect *out)
{
    Rect i;
    if (!clip(r, c, &i)) { out[0] = r; return 1; }
    int n = 0;
    if (r.y < i.y) out[n++] = (Rect){ r.x, r.y, r.w, i.y - r.y };                         // above
    if (i.y + i.h < r.y + r.h) out[n++] = (Rect){ r.x, i.y + i.h, r.w, r.y + r.h - i.y - i.h }; // below
    if (r.x < i.x) out[n++] = (Rect){ r.x, i.y, i.x - r.x, i.h };                         // left
    if (i.x + i.w < r.x + r.w) out[n++] = (Rect){ i.x + i.w, i.y, r.x + r.w - i.x - i.w, i.h }; // right
    return n;
}

// Share (0..1) of window i that is on a monitor and not under the opaque
// windows above it. Parts off every monitor (a column scrolled away) count as
// not showing.
static double share_of(const Scene *s, int i)
{
    Rect rest[MAX_PIECES * 4 + 4], next[MAX_PIECES * 4 + 4];
    Rect r = s->w[i].r;
    double total = (double)r.w * r.h;
    int n = 0;
    if (total <= 0) return 0;
    if (s->nm == 0) rest[n++] = r;
    for (int m = 0; m < s->nm && n < MAX_PIECES; m++)
        if (clip(r, s->m[m], &rest[n])) n++;
    for (int j = i + 1; j < s->n && n > 0; j++) {
        const Win *o = &s->w[j];
        if (!o->shown || !o->opaque) continue;
        int nn = 0;
        for (int p = 0; p < n; p++)
            nn += subtract(rest[p], o->r, &next[nn]);
        memcpy(rest, next, sizeof(Rect) * (size_t)nn);
        n = nn;
        if (n > MAX_PIECES) break;      // pathological layouts: stop splitting
    }
    double left = 0;
    for (int p = 0; p < n; p++) left += (double)rest[p].w * rest[p].h;
    return left / total > 1 ? 1 : left / total;
}

static int over_monitor(Rect r, const Scene *s)
{
    Rect c;
    for (int m = 0; m < s->nm; m++)
        if (clip(r, s->m[m], &c) && (double)c.w * c.h >= BIG * (double)s->m[m].w * s->m[m].h)
            return 1;
    return 0;
}

typedef struct { long pid; int st, g; } Entry;

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = a, *y = b;
    return (x->pid > y->pid) - (x->pid < y->pid);
}

// The snapshot for windows in stacking order (bottom to top): text to write
// (malloc'd), and with --debug one line of each window's share.
static char *table(const Scene *s)
{
    double *share = xmalloc(sizeof(double) * (size_t)(s->n + 1));
    for (int i = 0; i < s->n; i++)
        share[i] = s->w[i].shown ? share_of(s, i) : -1.0;
    // The focus: the focused window if it shows. If it does not (a fullscreen
    // game on top took the screen without the focus, as an override-redirect
    // X11 window does), the topmost shown fullscreen window.
    int fg = -1;
    for (int i = 0; i < s->n; i++)
        if (s->w[i].focus && share[i] >= MIN_SHARE) fg = i;
    for (int i = s->n - 1; fg < 0 && i >= 0; i--) {
        const Win *w = &s->w[i];
        if (share[i] >= MIN_SHARE && (w->unmanaged || (w->full && over_monitor(w->r, s))))
            fg = i;
    }
    Entry *e = xmalloc(sizeof(Entry) * (size_t)(s->n + 1));
    int ne = 0;
    long me = getpid();
    size_t dcap = 256, dlen = 0;
    char *dbg = debug ? xmalloc(dcap) : NULL;
    if (dbg) dbg[0] = 0;
    for (int i = 0; i < s->n; i++) {
        const Win *w = &s->w[i];
        if (w->pid <= 0 || w->pid == me) continue;
        int st = share[i] >= MIN_SHARE ? 2 : 1, k;
        if (i == fg) st = 3;
        for (k = 0; k < ne && e[k].pid != w->pid; k++) {}
        if (k == ne) { e[ne].pid = w->pid; e[ne].st = 0; e[ne].g = 0; ne++; }
        if (e[k].st < st) e[k].st = st;
        if (st >= 2 && w->full) e[k].g = 1;
        if (dbg) {
            char one[96];
            int l = share[i] < 0 ? snprintf(one, sizeof(one), " %ld:%s=off", w->pid, w->label)
                                 : snprintf(one, sizeof(one), " %ld:%s=%.3f", w->pid, w->label, share[i]);
            if (dlen + (size_t)l + 1 > dcap) { dcap = (dcap + (size_t)l) * 2; dbg = xrealloc(dbg, dcap); }
            memcpy(dbg + dlen, one, (size_t)l + 1);
            dlen += (size_t)l;
        }
    }
    if (ne > 1) qsort(e, (size_t)ne, sizeof(Entry), cmp_entry);
    char *text = xmalloc((size_t)ne * 32 + 1), *t = text;
    *t = 0;
    for (int k = 0; k < ne; k++)
        t += sprintf(t, "%ld %c%s\n", e[k].pid, "?hvf"[e[k].st], e[k].g ? " g" : "");
    if (dbg) logf_("%s", dlen ? dbg + 1 : "(no windows)");
    free(dbg);
    free(e);
    free(share);
    return text;
}

static int last_errno = -1;

// One open, one write: the driver takes what one open writes as the whole
// table. A write without lines turns the hints off.
static int publish(const char *text)
{
    const char *data = (text && *text) ? text : "\n";
    int flags = O_WRONLY | O_TRUNC | O_CLOEXEC;
    if (strncmp(hints, "/proc/", 6)) flags |= O_CREAT;
    int fd = open(hints, flags, 0644), err = 0;
    if (fd < 0) err = errno;
    else {
        size_t n = strlen(data);
        if (write(fd, data, n) != (ssize_t)n) err = errno ? errno : EIO;
        close(fd);
    }
    if (err) {
        if (err != last_errno)
            logf_("cannot write %s: %s%s", hints, strerror(err),
                  err == ENOENT ? " (the patched driver is not loaded?)" : "");
        last_errno = err;
        return 0;
    }
    if (last_errno >= 0) logf_("writing %s again", hints);
    last_errno = -1;
    return 1;
}

// --- sockets ----------------------------------------------------------------

static int unix_connect(const char *path)
{
    struct sockaddr_un a;
    if (strlen(path) >= sizeof(a.sun_path)) { errno = ENAMETOOLONG; return -1; }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { int e = errno; close(fd); errno = e; return -1; }
    struct timeval tv = { 2, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR && !stop) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

// Read until EOF (or, with line, until a newline); malloc'd, NUL-terminated.
static char *read_reply(int fd, int line)
{
    size_t cap = 65536, len = 0;
    char *b = xmalloc(cap + 1);
    for (;;) {
        if (len == cap) { cap *= 2; b = xrealloc(b, cap + 1); }
        ssize_t r = read(fd, b + len, cap - len);
        if (r < 0 && errno == EINTR && !stop) continue;
        if (r < 0) { free(b); return NULL; }
        if (r == 0) break;
        len += (size_t)r;
        if (line && memchr(b + len - (size_t)r, '\n', (size_t)r)) break;
    }
    b[len] = 0;
    return b;
}

static int read_exact(int fd, void *buf, size_t n)
{
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR && !stop) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

// Read and drop what is waiting on a non-blocking event socket; -1 on EOF.
static int drain_fd(int fd)
{
    char buf[65536];
    for (;;) {
        ssize_t r = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r > 0) continue;
        if (r == 0) return -1;
        return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : -1;
    }
}

// --- backends ---------------------------------------------------------------

typedef struct Backend {
    const char *name;
    double interval;            // poll every this many seconds
    double gap;                 // at least this long between two queries after events
    int (*scene)(struct Backend *, Scene *);    // -1: the compositor went away
    int (*drain)(struct Backend *);             // -1: the compositor went away
    int fd;                     // the event fd
    char path[256], path2[256];
    double alpha[3], alpha_at;  // Hyprland: active, inactive, fullscreen opacity
#if HAVE_X11
    xcb_connection_t *c;
    xcb_window_t root;
    xcb_atom_t A[16];
    int res, randr;
    uint32_t *pid_w; long *pid_p; int npid, cappid;
    uint32_t *watched; int nwatched, capwatched;
#endif
} Backend;

static void transform_logical(double *w, double *h, double scale, int transform)
{
    if (transform & 1) { double t = *w; *w = *h; *h = t; }
    if (scale <= 0) scale = 1;
    *w /= scale;
    *h /= scale;
}

// Hyprland: hyprctl's socket (j/monitors, j/clients, j/activewindow) and the
// event socket (.socket2.sock: any line means something changed). Formats of
// 0.4x to 0.57: workspaces are told apart by address (0.57), else id, else
// name; fullscreen is a bool with fullscreenMode (before 0.42) or the
// internal mode (2: fullscreen, 1: maximized).

static J *hypr_call(Backend *b, const char *cmd, int *gone)
{
    int fd = unix_connect(b->path);
    if (fd < 0) { *gone = 1; return NULL; }
    char *r = NULL;
    if (write_all(fd, cmd, strlen(cmd)) == 0) r = read_reply(fd, 0);
    close(fd);
    if (!r) { *gone = 1; return NULL; }
    J *j = jparse(r);
    free(r);
    return j;
}

static double hypr_opt(Backend *b, const char *name, int *gone)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "j/getoption %s", name);
    J *j = hypr_call(b, cmd, gone);
    double v = 1.0;
    if (jis(jget(j, "float"), J_NUM)) v = jnum(jget(j, "float"), 1.0);
    else if (jis(jget(j, "int"), J_NUM)) v = jnum(jget(j, "int"), 1.0);
    jfree(j);
    return v;
}

// A workspace's identity: "a:<address>", else "i:<id>", else "n:<name>";
// empty when there is none (no special workspace shown).
static void hypr_ws(const J *w, char *out, size_t n)
{
    out[0] = 0;
    if (!jis(w, J_OBJ)) return;
    const char *a = jstr(jget(w, "address"), "");
    double id = jnum(jget(w, "id"), 0);
    const char *name = jstr(jget(w, "name"), "");
    if (*a) snprintf(out, n, "a:%s", a);
    else if (jis(jget(w, "id"), J_NUM) && id != 0) snprintf(out, n, "i:%ld", (long)id);
    else if (*name) snprintf(out, n, "n:%s", name);
}

#define MAX_WS 64

static int in_set(char set[][160], int n, const char *k)
{
    if (!*k) return 0;
    for (int i = 0; i < n; i++)
        if (!strcmp(set[i], k)) return 1;
    return 0;
}

static int hypr_scene(Backend *b, Scene *s)
{
    int gone = 0;
    double t = now_s();
    if (t - b->alpha_at > 5.0) {        // the opacity settings, now and then
        b->alpha[0] = hypr_opt(b, "decoration:active_opacity", &gone);
        b->alpha[1] = hypr_opt(b, "decoration:inactive_opacity", &gone);
        b->alpha[2] = hypr_opt(b, "decoration:fullscreen_opacity", &gone);
        b->alpha_at = t;
    }
    J *mons = hypr_call(b, "j/monitors", &gone);
    J *clients = hypr_call(b, "j/clients", &gone);
    J *active = hypr_call(b, "j/activewindow", &gone);
    if (gone) { jfree(mons); jfree(clients); jfree(active); return -1; }
    const char *act = jstr(jget(active, "address"), NULL);
    static char visible[MAX_WS][160], special[MAX_WS][160];
    int nv = 0, ns = 0;
    for (int i = 0; i < jlen(mons); i++) {
        J *m = jat(mons, i);
        if (jbool(jget(m, "disabled"), 0)) continue;
        double w = jnum(jget(m, "width"), 0), h = jnum(jget(m, "height"), 0);
        transform_logical(&w, &h, jnum(jget(m, "scale"), 1.0), (int)jnum(jget(m, "transform"), 0));
        add_mon(s, px(jnum(jget(m, "x"), 0), jnum(jget(m, "y"), 0), w, h));
        if (nv < MAX_WS) { hypr_ws(jget(m, "activeWorkspace"), visible[nv], 160); if (*visible[nv]) nv++; }
        J *sp = jget(m, "specialWorkspace");
        if (nv < MAX_WS && ns < MAX_WS && *jstr(jget(sp, "name"), "")) {
            hypr_ws(sp, special[ns], 160);
            if (*special[ns]) { strcpy(visible[nv++], special[ns]); ns++; }
        }
    }
    for (int i = 0; i < jlen(clients); i++) {
        J *c = jat(clients, i);
        if (!jbool(jget(c, "mapped"), 1)) continue;
        char ws[160];
        hypr_ws(jget(c, "workspace"), ws, sizeof(ws));
        J *fs = jget(c, "fullscreen");
        int full = jis(fs, J_BOOL) ? (fs->n != 0 && jnum(jget(c, "fullscreenMode"), 0) == 0)   // before 0.42
                                   : (((long)jnum(fs, 0) & 2) != 0);
        const char *addr = jstr(jget(c, "address"), "");
        int focus = act && !strcmp(addr, act);
        double a = full ? b->alpha[2] : focus ? b->alpha[0] : b->alpha[1];
        J *at = jget(c, "at"), *sz = jget(c, "size");
        Win *w = add_win(s, (long)jnum(jget(c, "pid"), -1),
                         px(jnum(jat(at, 0), 0), jnum(jat(at, 1), 0), jnum(jat(sz, 0), 0), jnum(jat(sz, 1), 0)),
                         jstr(jget(c, "class"), ""));
        // Not "visible" (0.57): it is also false for a window that takes no input.
        w->shown = !jbool(jget(c, "hidden"), 0) && in_set(visible, nv, ws);
        w->opaque = a >= 1.0;
        w->full = full;
        w->focus = focus;
        // Stacking: tiled, then floating, then fullscreen; a special
        // workspace over the regular one; in each, the list's order (the
        // compositor moves a raised window to its end).
        w->k1 = in_set(special, ns, ws);
        w->k2 = full ? 2 : jbool(jget(c, "floating"), 0) ? 1 : 0;
        w->k3 = i;
    }
    jfree(mons);
    jfree(clients);
    jfree(active);
    if (s->n > 1) qsort(s->w, (size_t)s->n, sizeof(Win), cmp_win);
    return 0;
}

static int sock_drain(Backend *b) { return drain_fd(b->fd); }

static int hypr_open(Backend *b)
{
    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE"), *run = getenv("XDG_RUNTIME_DIR");
    char base[200], ev[256];
    snprintf(base, sizeof(base), "%s/hypr/%s", run ? run : "/tmp", sig);
    if (access(base, F_OK)) snprintf(base, sizeof(base), "/tmp/hypr/%s", sig);
    snprintf(b->path, sizeof(b->path), "%s/.socket.sock", base);
    snprintf(ev, sizeof(ev), "%s/.socket2.sock", base);
    b->fd = unix_connect(ev);
    if (b->fd < 0) { logf_("no Hyprland event socket in %s: %s", base, strerror(errno)); return -1; }
    b->name = "hyprland";
    b->interval = 0.25;
    b->gap = 0.02;
    b->scene = hypr_scene;
    b->drain = sock_drain;
    b->alpha[0] = b->alpha[1] = b->alpha[2] = 1.0;
    b->alpha_at = -1e9;
    return 0;
}

// sway: GET_TREE over the i3 IPC on every window/workspace/output event and
// every 250 ms. A view's "visible" is already false on hidden workspaces, in
// the scratchpad, for unseen tabs of tabbed and stacked containers and
// behind a fullscreen view; only floating windows need the covering test.

static J *i3_ask(int fd, uint32_t type, const char *payload)
{
    char hdr[14];
    uint32_t len = payload ? (uint32_t)strlen(payload) : 0;
    memcpy(hdr, "i3-ipc", 6);
    memcpy(hdr + 6, &len, 4);
    memcpy(hdr + 10, &type, 4);
    if (write_all(fd, hdr, 14) || (len && write_all(fd, payload, len))) return NULL;
    if (read_exact(fd, hdr, 14) || memcmp(hdr, "i3-ipc", 6)) return NULL;
    memcpy(&len, hdr + 6, 4);
    char *body = xmalloc((size_t)len + 1);
    if (read_exact(fd, body, len)) { free(body); return NULL; }
    body[len] = 0;
    J *j = jparse(body);
    free(body);
    return j ? j : jnew(J_NULL);
}

static void sway_walk(Scene *s, const J *n, int floats)
{
    const char *type = jstr(jget(n, "type"), "");
    if (jnum(jget(n, "pid"), 0) > 0 && (!strcmp(type, "con") || !strcmp(type, "floating_con")) &&
        jlen(jget(n, "nodes")) == 0) {
        // The view with its title bar (deco_rect, above rect).
        J *r = jget(n, "rect");
        double dh = jnum(jget(jget(n, "deco_rect"), "height"), 0);
        J *op = jget(n, "opacity");
        const char *label = jstr(jget(n, "app_id"), NULL);
        if (!label) label = jstr(jget(jget(n, "window_properties"), "class"), "");
        Win *w = add_win(s, (long)jnum(jget(n, "pid"), -1),
                         px(jnum(jget(r, "x"), 0), jnum(jget(r, "y"), 0) - dh,
                            jnum(jget(r, "width"), 0), jnum(jget(r, "height"), 0) + dh), label);
        w->shown = jbool(jget(n, "visible"), 0);
        w->opaque = !jis(op, J_NUM) || op->n >= 1.0;
        w->full = jnum(jget(n, "fullscreen_mode"), 0) != 0;
        w->focus = jbool(jget(n, "focused"), 0);
        w->k1 = w->full ? 2 : floats ? 1 : 0;   // tiled, floating, fullscreen
        return;
    }
    J *c = jget(n, "nodes");
    for (int i = 0; i < jlen(c); i++) sway_walk(s, jat(c, i), floats);
    c = jget(n, "floating_nodes");
    for (int i = 0; i < jlen(c); i++) sway_walk(s, jat(c, i), 1);
}

static int sway_scene(Backend *b, Scene *s)
{
    int fd = unix_connect(b->path);
    if (fd < 0) return -1;
    J *tree = i3_ask(fd, 4, NULL);                   // GET_TREE
    close(fd);
    if (!tree) return -1;
    J *outs = jget(tree, "nodes");
    for (int i = 0; i < jlen(outs); i++) {
        J *o = jat(outs, i), *r = jget(o, "rect");
        if (strcmp(jstr(jget(o, "name"), ""), "__i3"))  // __i3: the scratchpad
            add_mon(s, px(jnum(jget(r, "x"), 0), jnum(jget(r, "y"), 0),
                          jnum(jget(r, "width"), 0), jnum(jget(r, "height"), 0)));
        sway_walk(s, o, 0);
    }
    jfree(tree);
    if (s->n > 1) qsort(s->w, (size_t)s->n, sizeof(Win), cmp_win);
    return 0;
}

static int sway_open(Backend *b, const char *path)
{
    snprintf(b->path, sizeof(b->path), "%s", path);
    b->fd = unix_connect(path);
    if (b->fd < 0) { logf_("sway socket %s: %s", path, strerror(errno)); return -1; }
    J *r = i3_ask(b->fd, 2, "[\"window\",\"workspace\",\"output\"]");    // SUBSCRIBE
    if (!r) { logf_("sway: cannot subscribe to events"); return -1; }
    jfree(r);
    b->name = "sway";
    b->interval = 0.25;
    b->gap = 0.02;
    b->scene = sway_scene;
    b->drain = sock_drain;
    return 0;
}

// niri: its IPC ($NIRI_SOCKET, one JSON request per line). It gives no
// position for tiled windows (tile_pos_in_workspace_view is only set for
// floating ones), so a tiled window on an active workspace counts as shown
// even when its column is scrolled out of view: a hidden window kept is
// memory not freed, a shown one moved out would be the user waiting. A tiled
// window the size of its output is fullscreen and, when it is its workspace's
// active window, hides the other tiled ones.

static J *niri_call(Backend *b, const char *req, J **whole)
{
    *whole = NULL;
    int fd = unix_connect(b->path);
    if (fd < 0) return NULL;
    char line[64];
    snprintf(line, sizeof(line), "\"%s\"\n", req);
    char *r = write_all(fd, line, strlen(line)) ? NULL : read_reply(fd, 1);
    close(fd);
    if (!r) return NULL;
    *whole = jparse(r);
    free(r);
    J *v = jget(jget(*whole, "Ok"), req);
    if (!v) { jfree(*whole); *whole = jnew(J_NULL); }
    return v;
}

typedef struct { long id; int active; long active_win; char out[64]; } NiriWs;

static int niri_scene(Backend *b, Scene *s)
{
    J *wo, *ww, *wwin;
    J *outputs = niri_call(b, "Outputs", &wo);
    J *wss = niri_call(b, "Workspaces", &ww);
    J *wins = niri_call(b, "Windows", &wwin);
    if (!wo || !ww || !wwin) { jfree(wo); jfree(ww); jfree(wwin); return -1; }
    int no = jis(outputs, J_OBJ) ? outputs->len : 0;
    Rect *orect = xmalloc(sizeof(Rect) * (size_t)(no + 1));
    for (int i = 0; i < no; i++) {
        J *lg = jget(outputs->v[i], "logical");
        orect[i] = px(jnum(jget(lg, "x"), 0), jnum(jget(lg, "y"), 0), jnum(jget(lg, "width"), 0), jnum(jget(lg, "height"), 0));
        if (lg) add_mon(s, orect[i]);
    }
    int nw = jlen(wss);
    NiriWs *ws = xmalloc(sizeof(NiriWs) * (size_t)(nw + 1));
    for (int i = 0; i < nw; i++) {
        J *w = jat(wss, i);
        ws[i].id = (long)jnum(jget(w, "id"), -1);
        ws[i].active = jbool(jget(w, "is_active"), 0);
        ws[i].active_win = (long)jnum(jget(w, "active_window_id"), -1);
        snprintf(ws[i].out, sizeof(ws[i].out), "%s", jstr(jget(w, "output"), ""));
    }
    for (int i = 0; i < jlen(wins); i++) {
        J *w = jat(wins, i), *lay = jget(w, "layout");
        long wid = (long)jnum(jget(w, "workspace_id"), -1);
        NiriWs *k = NULL;
        Rect *o = NULL;
        for (int j = 0; j < nw; j++) if (ws[j].id == wid) k = &ws[j];
        for (int j = 0; k && j < no; j++)
            if (!strcmp(outputs->k[j], k->out) && jget(outputs->v[j], "logical")) o = &orect[j];
        J *size = jget(lay, "window_size");
        if (!size) size = jget(lay, "tile_size");
        double sw = jnum(jat(size, 0), 0), sh = jnum(jat(size, 1), 0);
        const char *label = jstr(jget(w, "app_id"), "");
        long pid = (long)jnum(jget(w, "pid"), -1);
        int shown = k && k->active && o, focus = jbool(jget(w, "is_focused"), 0);
        Win *v;
        if (jbool(jget(w, "is_floating"), 0)) {
            J *pos = jget(lay, "tile_pos_in_workspace_view");
            Rect r = (o && jlen(pos) == 2) ? px(o->x + jnum(jat(pos, 0), 0), o->y + jnum(jat(pos, 1), 0), sw, sh)
                                           : o ? *o : px(0, 0, sw, sh);
            v = add_win(s, pid, r, label);
            // Floating windows above the tiled ones, the most recently focused on top.
            J *ts = jget(w, "focus_timestamp");
            v->k1 = 2;
            v->k2 = (long)jnum(jget(ts, "secs"), 0) * 1000000000L + (long)jnum(jget(ts, "nanos"), 0);
        } else if (o && sw >= o->w && sh >= o->h && k->active_win == (long)jnum(jget(w, "id"), -2)) {
            v = add_win(s, pid, *o, label);
            v->full = 1;
            v->k1 = 1;
        } else {
            // Position unknown: on its output, covering nothing.
            v = add_win(s, pid, o ? *o : px(0, 0, sw, sh), label);
            v->opaque = 0;
            v->k1 = 0;
        }
        v->shown = shown;
        v->focus = focus;
    }
    free(ws);
    free(orect);
    jfree(wo);
    jfree(ww);
    jfree(wwin);
    if (s->n > 1) qsort(s->w, (size_t)s->n, sizeof(Win), cmp_win);
    return 0;
}

static int niri_open(Backend *b, const char *path)
{
    snprintf(b->path, sizeof(b->path), "%s", path);
    b->fd = unix_connect(path);
    if (b->fd < 0 || write_all(b->fd, "\"EventStream\"\n", 14)) {
        logf_("niri socket %s: %s", path, strerror(errno));
        return -1;
    }
    struct timeval tv = { 0, 0 };       // the event stream stays open: no timeout
    setsockopt(b->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    b->name = "niri";
    b->interval = 0.25;
    b->gap = 0.02;
    b->scene = niri_scene;
    b->drain = sock_drain;
    return 0;
}

#if HAVE_X11
// X11: any EWMH window manager. The stacking order is the X server's (the
// root's children, bottom to top), so override-redirect windows sit where
// they are drawn. Shown: the client window is viewable (window managers unmap
// minimized windows and those of other desktops), not _NET_WM_STATE_HIDDEN,
// on the current desktop or all of them. Covering: normal and dialog windows
// without _NET_WM_WINDOW_OPACITY below 1, and fullscreen ones. Pids from
// _NET_WM_PID, else the X-Resource extension. An override-redirect window
// over 90 % of a monitor is a fullscreen game (not in the client list).

enum { A_CLIENT_LIST, A_ACTIVE, A_CUR_DESK, A_WM_DESK, A_PID, A_STATE, A_HIDDEN, A_FULLSCREEN,
       A_TYPE, A_NORMAL, A_DIALOG, A_OPACITY, A_N };
static const char *atom_names[A_N] = {
    "_NET_CLIENT_LIST", "_NET_ACTIVE_WINDOW", "_NET_CURRENT_DESKTOP", "_NET_WM_DESKTOP",
    "_NET_WM_PID", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_FULLSCREEN",
    "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL", "_NET_WM_WINDOW_TYPE_DIALOG",
    "_NET_WM_WINDOW_OPACITY",
};

static const char *x11_compositors[] = {
    "xfwm4", "picom", "compton", "compiz", "marco", "cinnamon", "muffin", "budgie-wm", "openbox",
    "i3", "awesome", "bspwm", "xmonad", "fluxbox", "icewm", "herbstluftwm", "qtile", "dwm",
    "mutter", "gnome-shell", "kwin_x11", "Xorg", "Xwayland", NULL,
};

static void comm_of(long pid, char *out, size_t n)
{
    char p[64];
    out[0] = 0;
    snprintf(p, sizeof(p), "/proc/%ld/comm", pid);
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}

// A property as 32-bit values: count, or -1 when missing. *v malloc'd.
static int xprop32(Backend *b, xcb_window_t w, xcb_atom_t a, uint32_t **v)
{
    *v = NULL;
    xcb_get_property_reply_t *r = xcb_get_property_reply(
        b->c, xcb_get_property(b->c, 0, w, a, XCB_GET_PROPERTY_TYPE_ANY, 0, 4096), NULL);
    if (!r) return -1;
    if (r->type == XCB_NONE || r->format != 32) { free(r); return -1; }
    int n = xcb_get_property_value_length(r) / 4;
    *v = xmalloc(sizeof(uint32_t) * (size_t)(n + 1));
    memcpy(*v, xcb_get_property_value(r), sizeof(uint32_t) * (size_t)n);
    free(r);
    return n;
}

static long x11_pid(Backend *b, xcb_window_t w)
{
    for (int i = 0; i < b->npid; i++)
        if (b->pid_w[i] == w) return b->pid_p[i];
    uint32_t *v;
    long pid = -1;
    if (xprop32(b, w, b->A[A_PID], &v) > 0) pid = v[0];
    free(v);
    if (pid <= 0 && b->res) {
        xcb_res_client_id_spec_t spec = { w, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID };
        xcb_res_query_client_ids_reply_t *r = xcb_res_query_client_ids_reply(
            b->c, xcb_res_query_client_ids(b->c, 1, &spec), NULL);
        if (r) {
            xcb_res_client_id_value_iterator_t it = xcb_res_query_client_ids_ids_iterator(r);
            for (; it.rem; xcb_res_client_id_value_next(&it))
                if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) &&
                    xcb_res_client_id_value_value_length(it.data) > 0)
                    pid = *xcb_res_client_id_value_value(it.data);
            free(r);
        }
    }
    if (pid > 0) {                      // stays for the window's life
        if (b->npid == b->cappid) {
            b->cappid = b->cappid ? b->cappid * 2 : 64;
            b->pid_w = xrealloc(b->pid_w, sizeof(uint32_t) * (size_t)b->cappid);
            b->pid_p = xrealloc(b->pid_p, sizeof(long) * (size_t)b->cappid);
        }
        b->pid_w[b->npid] = w;
        b->pid_p[b->npid++] = pid;
    }
    return pid;
}

static int has(const uint32_t *v, int n, uint32_t x)
{
    for (int i = 0; i < n; i++) if (v[i] == x) return 1;
    return 0;
}

static int x11_scene(Backend *b, Scene *s)
{
    if (xcb_connection_has_error(b->c)) { logf_("the X server closed the connection"); return -1; }
    xcb_randr_get_monitors_reply_t *mr = b->randr ?
        xcb_randr_get_monitors_reply(b->c, xcb_randr_get_monitors(b->c, b->root, 1), NULL) : NULL;
    if (mr) {
        xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(mr);
        for (; it.rem; xcb_randr_monitor_info_next(&it))
            add_mon(s, (Rect){ it.data->x, it.data->y, it.data->width, it.data->height });
        free(mr);
    }
    if (s->nm == 0) {
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(b->c, xcb_get_geometry(b->c, b->root), NULL);
        if (g) { add_mon(s, (Rect){ 0, 0, g->width, g->height }); free(g); }
    }
    xcb_query_tree_reply_t *tr = xcb_query_tree_reply(b->c, xcb_query_tree(b->c, b->root), NULL);
    if (!tr) { logf_("the X server closed the connection"); return -1; }
    xcb_window_t *stack = xcb_query_tree_children(tr);          // bottom to top
    int nstack = xcb_query_tree_children_length(tr);
    char *frame = xmalloc((size_t)nstack + 1);                  // the root's children that hold a client
    memset(frame, 0, (size_t)nstack + 1);
    uint32_t *clients, *v;
    int nc = xprop32(b, b->root, b->A[A_CLIENT_LIST], &clients);
    if (nc < 0) nc = 0;
    xcb_window_t active = 0;
    long cur = -1;
    if (xprop32(b, b->root, b->A[A_ACTIVE], &v) > 0) active = v[0];
    free(v);
    if (xprop32(b, b->root, b->A[A_CUR_DESK], &v) > 0) cur = v[0];
    free(v);
    for (int ci = 0; ci < nc; ci++) {
        xcb_window_t cw = clients[ci], top = cw;
        if (!has(b->watched, b->nwatched, cw)) {
            uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
            xcb_change_window_attributes(b->c, cw, XCB_CW_EVENT_MASK, &mask);
            if (b->nwatched == b->capwatched) {
                b->capwatched = b->capwatched ? b->capwatched * 2 : 64;
                b->watched = xrealloc(b->watched, sizeof(uint32_t) * (size_t)b->capwatched);
            }
            b->watched[b->nwatched++] = cw;
        }
        // The root's child that holds the client (its frame).
        for (int d = 0; d < 16; d++) {
            xcb_query_tree_reply_t *t = xcb_query_tree_reply(b->c, xcb_query_tree(b->c, top), NULL);
            if (!t) { top = 0; break; }
            xcb_window_t parent = t->parent;
            free(t);
            if (parent == b->root || parent == XCB_NONE) break;
            top = parent;
        }
        if (!top) continue;                                     // gone meanwhile
        int idx = -1;
        for (int i = 0; i < nstack; i++) if (stack[i] == top) { idx = i; frame[i] = 1; break; }
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(b->c, xcb_get_geometry(b->c, top), NULL);
        xcb_get_window_attributes_reply_t *at = xcb_get_window_attributes_reply(
            b->c, xcb_get_window_attributes(b->c, cw), NULL);
        if (!g || !at) { free(g); free(at); continue; }
        uint32_t *state, *types, *op;
        int nst = xprop32(b, cw, b->A[A_STATE], &state);
        int nty = xprop32(b, cw, b->A[A_TYPE], &types);
        int nop = xprop32(b, cw, b->A[A_OPACITY], &op);
        if (nop < 0 && top != cw) { free(op); nop = xprop32(b, top, b->A[A_OPACITY], &op); }
        long desk = -1;
        if (xprop32(b, cw, b->A[A_WM_DESK], &v) > 0) desk = v[0];
        free(v);
        int full = has(state, nst, b->A[A_FULLSCREEN]);
        int normal = nty <= 0 || types[0] == b->A[A_NORMAL] || types[0] == b->A[A_DIALOG];
        char label[24];
        snprintf(label, sizeof(label), "0x%x", cw);
        Win *w = add_win(s, x11_pid(b, cw), (Rect){ g->x, g->y, g->width + 2 * g->border_width,
                                                       g->height + 2 * g->border_width }, label);
        w->shown = at->map_state == XCB_MAP_STATE_VIEWABLE && !has(state, nst, b->A[A_HIDDEN]) &&
                   (desk < 0 || cur < 0 || desk == cur || desk == 0xFFFFFFFFL);
        w->opaque = (full || normal) && (nop <= 0 || op[0] == 0xFFFFFFFFu);
        w->full = full;
        w->focus = cw == active;
        w->k1 = idx;
        free(state); free(types); free(op); free(g); free(at);
    }
    // Unmanaged windows over a monitor: fullscreen games that bypass the
    // window manager. Small ones (menus, tooltips) say nothing.
    for (int i = 0; i < nstack; i++) {
        if (frame[i]) continue;
        xcb_get_window_attributes_reply_t *at = xcb_get_window_attributes_reply(
            b->c, xcb_get_window_attributes(b->c, stack[i]), NULL);
        if (!at) continue;
        int cand = at->override_redirect && at->map_state == XCB_MAP_STATE_VIEWABLE &&
                   at->_class == XCB_WINDOW_CLASS_INPUT_OUTPUT;
        free(at);
        if (!cand) continue;
        xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(b->c, xcb_get_geometry(b->c, stack[i]), NULL);
        if (!g) continue;
        Rect r = { g->x, g->y, g->width, g->height };
        free(g);
        if (!over_monitor(r, s)) continue;
        long pid = x11_pid(b, stack[i]);
        char comm[32];
        if (pid <= 0) continue;
        comm_of(pid, comm, sizeof(comm));
        int wm = 0;
        for (const char **c = x11_compositors; *c; c++) if (!strcmp(comm, *c)) wm = 1;
        if (wm) continue;                       // the compositor's own overlay
        char label[24];
        snprintf(label, sizeof(label), "or:0x%x", stack[i]);
        Win *w = add_win(s, pid, r, label);
        w->full = w->unmanaged = 1;
        w->k1 = i;
    }
    // Forget windows that are gone.
    int k = 0;
    for (int i = 0; i < b->nwatched; i++)
        if (has(clients, nc, b->watched[i])) b->watched[k++] = b->watched[i];
    b->nwatched = k;
    if (b->npid > 4 * (nc + 64)) {
        k = 0;
        for (int i = 0; i < b->npid; i++)
            if (has(clients, nc, b->pid_w[i])) { b->pid_w[k] = b->pid_w[i]; b->pid_p[k++] = b->pid_p[i]; }
        b->npid = k;
    }
    free(clients);
    free(frame);
    free(tr);
    if (s->n > 1) qsort(s->w, (size_t)s->n, sizeof(Win), cmp_win);
    return 0;
}

static int x11_drain(Backend *b)
{
    xcb_generic_event_t *e;
    while ((e = xcb_poll_for_event(b->c))) free(e);
    if (xcb_connection_has_error(b->c)) { logf_("the X server closed the connection"); return -1; }
    return 0;
}

static int x11_open(Backend *b)
{
    int scr;
    b->c = xcb_connect(NULL, &scr);
    if (xcb_connection_has_error(b->c)) { logf_("cannot open the X display"); return -1; }
    const xcb_setup_t *setup = xcb_get_setup(b->c);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < scr; i++) xcb_screen_next(&it);
    b->root = it.data->root;
    xcb_intern_atom_cookie_t ck[A_N];
    for (int i = 0; i < A_N; i++) ck[i] = xcb_intern_atom(b->c, 0, (uint16_t)strlen(atom_names[i]), atom_names[i]);
    for (int i = 0; i < A_N; i++) {
        xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(b->c, ck[i], NULL);
        b->A[i] = r ? r->atom : XCB_NONE;
        free(r);
    }
    const xcb_query_extension_reply_t *q = xcb_get_extension_data(b->c, &xcb_res_id);
    if (q && q->present) {
        xcb_res_query_version_reply_t *r = xcb_res_query_version_reply(b->c, xcb_res_query_version(b->c, 1, 2), NULL);
        b->res = r && (r->server_major > 1 || (r->server_major == 1 && r->server_minor >= 2));
        free(r);
    }
    q = xcb_get_extension_data(b->c, &xcb_randr_id);
    if (q && q->present) {
        xcb_randr_query_version_reply_t *r = xcb_randr_query_version_reply(b->c, xcb_randr_query_version(b->c, 1, 5), NULL);
        b->randr = r && (r->major_version > 1 || (r->major_version == 1 && r->minor_version >= 5));
        free(r);
    }
    uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
    xcb_change_window_attributes(b->c, b->root, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(b->c);
    b->fd = xcb_get_file_descriptor(b->c);
    b->name = "x11";
    b->interval = 1.0;
    b->gap = 0.1;           // a window dragged sends a stream of ConfigureNotify
    b->scene = x11_scene;
    b->drain = x11_drain;
    return 0;
}
#endif

// --- main -------------------------------------------------------------------

// 0: ready; 1: nothing to do here (message given); -1: error.
static int pick(Backend *b, const char *name)
{
    const char *why = NULL;
    if (!name || !strcmp(name, "auto")) {
        const char *desk = getenv("XDG_CURRENT_DESKTOP");
        char d[256];
        snprintf(d, sizeof(d), ":%s:", desk ? desk : "");
        for (char *p = d; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (getenv("HYPRLAND_INSTANCE_SIGNATURE")) name = "hyprland";
        else if (getenv("SWAYSOCK")) name = "sway";
        else if (getenv("NIRI_SOCKET")) name = "niri";
        else if (strstr(d, ":KDE:")) why = "KDE Plasma: the KWin script (kwin-vram-hints/) reports here";
        else if (strstr(d, ":GNOME:")) why = "GNOME: the Shell extension (gnome-vram-hints/) reports here";
        else if (getenv("WAYLAND_DISPLAY")) why = "this Wayland compositor gives no window list with pids (local/HINTS.md)";
        else if (getenv("DISPLAY")) name = "x11";
        else why = "no Hyprland, sway, niri or X11 display in the environment";
        if (why) { logf_("%s; nothing to do", why); return 1; }
    }
    if (!strcmp(name, "hyprland")) {
        if (!getenv("HYPRLAND_INSTANCE_SIGNATURE")) { logf_("HYPRLAND_INSTANCE_SIGNATURE is not set"); return -1; }
        return hypr_open(b);
    }
    if (!strcmp(name, "sway")) {
        const char *p = getenv("SWAYSOCK");
        if (!p) { logf_("SWAYSOCK is not set"); return -1; }
        return sway_open(b, p);
    }
    if (!strcmp(name, "niri")) {
        const char *p = getenv("NIRI_SOCKET");
        if (!p) { logf_("NIRI_SOCKET is not set"); return -1; }
        return niri_open(b, p);
    }
    if (!strcmp(name, "x11")) {
#if HAVE_X11
        return x11_open(b);
#else
        logf_("built without X11 (libxcb, xcb-res, xcb-randr)");
        return -1;
#endif
    }
    logf_("unknown backend %s", name);
    return -1;
}

static void on_signal(int sig) { (void)sig; stop = 1; }

static void usage(void)
{
    fputs("usage: vram-hintd [--once] [--debug] [--backend auto|hyprland|sway|niri|x11]\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *bname = "auto";
    int once = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--once")) once = 1;
        else if (!strcmp(argv[i], "--debug")) debug = 1;
        else if (!strcmp(argv[i], "--backend") && i + 1 < argc) bname = argv[++i];
        else if (!strncmp(argv[i], "--backend=", 10)) bname = argv[i] + 10;
        else usage();
    }
    if (getenv("VRAM_HINTS_FILE") && *getenv("VRAM_HINTS_FILE")) hints = getenv("VRAM_HINTS_FILE");
    signal(SIGPIPE, SIG_IGN);

    Backend b;
    memset(&b, 0, sizeof(b));
    b.fd = -1;
    int r = pick(&b, bname);
    if (r) return r > 0 ? 0 : 1;

    Scene s;
    memset(&s, 0, sizeof(s));
    if (once) {
        if (b.scene(&b, &s)) return 1;
        char *t = table(&s);
        fputs(t, stdout);
        free(t);
        return 0;
    }
    // One writer per user: two would overwrite each other's tables.
    const char *run = getenv("XDG_RUNTIME_DIR");
    char lp[300];
    snprintf(lp, sizeof(lp), "%s/vram-hintd.lock", run && *run ? run : "/tmp");
    int lock = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
        logf_("another vram-hintd runs for this user");
        return 0;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;          // no SA_RESTART: select returns at once
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    logf_("%s: writing %s", b.name, hints);

    char *last = NULL;
    int wrote = 0;
    while (!stop) {
        scene_reset(&s);
        if (b.scene(&b, &s)) { logf_("%s went away", b.name); break; }
        char *t = table(&s);
        if (!last || strcmp(t, last)) {
            if (publish(t)) { free(last); last = t; t = NULL; wrote = 1; }
        }
        free(t);
        if (stop) break;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(b.fd, &fds);
        double t0 = now_s();
        struct timeval tv = { (time_t)b.interval, (suseconds_t)((b.interval - (long)b.interval) * 1e6) };
        int n = select(b.fd + 1, &fds, NULL, NULL, &tv);
        if (n > 0) {
            // Let a burst of events arrive, then one query.
            double wait = b.gap - (now_s() - t0);
            if (wait < 0.02) wait = 0.02;
            struct timespec ts = { 0, (long)(wait * 1e9) };
            nanosleep(&ts, NULL);
        }
        // Drained after a timeout too: xcb may have read events into its
        // queue already, and would not wake select for them.
        if (b.drain(&b)) { logf_("%s went away", b.name); break; }
    }
    // A stale table is worse than none: the driver would keep acting on
    // windows nobody reports.
    if (wrote) publish("\n");
    free(last);
    return 0;
}
