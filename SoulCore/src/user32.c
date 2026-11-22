// SoulCore - user32 emulation: windows, message queue, painting, timers, input, menus, resources.
#include "gui.h"
#include <ctype.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))
#define ARGI(n) ((int32_t)hle_arg(st, (n)))
#define CW_USEDEFAULT ((int32_t)0x80000000)
#define HWND_MESSAGE ((uint64_t)-3)
#define HWND_BROADCAST 0xFFFFull
#define ACCEL_HANDLE_BASE 0x30000
#define ICON_HANDLE_BASE 0x60000

static GuiState g_gui;

GuiState *gui(void) { return &g_gui; }
void gui_lock(void) { pthread_mutex_lock(&g_gui.mu); }
void gui_unlock(void) { pthread_mutex_unlock(&g_gui.mu); }
static void bump(void) { __atomic_add_fetch(&g_gui.generation, 1, __ATOMIC_SEQ_CST); }
static uint32_t cur_tid(void) { SoulThread *t = soul_cur_thread(); return t ? t->tid : 4243; }
static uint32_t tick_ms(void) { return (uint32_t)(soul_now() * 1000.0); }

static const uint32_t k_sys_colors[31] = {
    0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000,
    0xB4B4B4, 0xFCF7F4, 0xABABAB, 0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000,
    0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xFF9933, 0xF0F0F0,
};
uint32_t gui_sys_color(int i) { return i >= 0 && i < 31 ? k_sys_colors[i] : 0; }

// ---- strings -----------------------------------------------------------------------------------

// Guest string argument as malloc'ed UTF-8 (NULL for NULL / integer atoms).
static char *gstr(uint64_t p, int wide) {
    if (p < 0x10000) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
}
// Writes UTF-8 text into a guest buffer of cap units; returns units written (without NUL).
static size_t put_str(uint64_t dst, size_t cap, const char *s, int wide) {
    if (!dst || cap == 0) return 0;
    if (wide) return u2w(s, -1, (uint16_t *)G2H(dst), cap);
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(G2H(dst), s, n);
    ((char *)G2H(dst))[n] = 0;
    return n;
}
static size_t str_units(const char *s, int wide) {
    if (!wide) return strlen(s);
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) == 0x80) continue;
        n += *p >= 0xF0 ? 2 : 1;
    }
    return n;
}

// ---- windows ------------------------------------------------------------------------------------

Win *win_get(uint64_t h) {
    if (h < WIN_HANDLE_BASE) return NULL;
    uint64_t i = (h - WIN_HANDLE_BASE) / 4;
    if (i >= WIN_MAX || (h - WIN_HANDLE_BASE) % 4) return NULL;
    Win *w = &g_gui.wins[i];
    return w->used ? w : NULL;
}
Win *win_top(Win *w) {
    while (w && w->parent) { Win *p = win_get(w->parent); if (!p) break; w = p; }
    return w;
}
void win_client_origin(Win *w, int *ox, int *oy) {
    int x = 0, y = 0;
    while (w && w->parent) { x += w->x; y += w->y; w = win_get(w->parent); }
    *ox = x; *oy = y;
}
static void desktop_origin(Win *w, int *ox, int *oy) {
    Win *top = win_top(w);
    win_client_origin(w, ox, oy);
    if (top) { *ox += top->x; *oy += top->y; }
}
int win_visible_rect(Win *w, int *x0, int *y0, int *x1, int *y1) {
    int ox, oy;
    win_client_origin(w, &ox, &oy);
    int a = ox, b = oy, c = ox + w->w, d = oy + w->h;
    for (Win *it = w; it && it->parent; it = win_get(it->parent)) {
        if (!it->visible) return 0;
        Win *p = win_get(it->parent);
        if (!p) break;
        int px, py;
        win_client_origin(p, &px, &py);
        if (a < px) a = px;
        if (b < py) b = py;
        if (c > px + p->w) c = px + p->w;
        if (d > py + p->h) d = py + p->h;
    }
    Win *top = win_top(w);
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (top && c > top->fb_w) c = top->fb_w;
    if (top && d > top->fb_h) d = top->fb_h;
    *x0 = a; *y0 = b; *x1 = c; *y1 = d;
    return a < c && b < d;
}
static int win_shown(Win *w) { // the window and all its ancestors are visible
    for (; w; w = w->parent ? win_get(w->parent) : NULL) if (!w->visible) return 0;
    return 1;
}
static int win_enabled_chain(Win *w) {
    for (; w; w = w->parent ? win_get(w->parent) : NULL) if (!w->enabled) return 0;
    return 1;
}
void win_touch(Win *w) { if (w) w->version++; }

void win_set_text(Win *w, const char *s) {
    char *n = strdup(s ? s : "");
    gui_lock();
    char *old = w->text;
    w->text = n;
    gui_unlock();
    free(old);
    if (!w->parent) bump();
}

// Children of a window in z-order (creation order). Returns count; *out is malloc'ed.
static int children_of(uint64_t parent, Win ***out) {
    int n = 0;
    Win **list = NULL;
    for (int i = 0; i < WIN_MAX; i++) {
        Win *w = &g_gui.wins[i];
        if (!w->used || w->parent != parent || (parent == 0 && w->message_only)) continue;
        list = realloc(list, sizeof(Win *) * (size_t)(n + 1));
        list[n++] = w;
    }
    for (int a = 1; a < n; a++) for (int b = a; b > 0 && list[b - 1]->seq > list[b]->seq; b--) { Win *t = list[b]; list[b] = list[b - 1]; list[b - 1] = t; }
    *out = list;
    return n;
}
static int is_child_of(uint64_t parent, Win *w) {
    while (w && w->parent) { if (w->parent == parent) return 1; w = win_get(w->parent); }
    return 0;
}

void win_invalidate_rect(Win *w, const int32_t *rc, int erase) {
    int x0 = 0, y0 = 0, x1 = w->w, y1 = w->h;
    if (rc) {
        if (rc[0] > x0) x0 = rc[0];
        if (rc[1] > y0) y0 = rc[1];
        if (rc[2] < x1) x1 = rc[2];
        if (rc[3] < y1) y1 = rc[3];
    }
    if (x0 >= x1 || y0 >= y1) return;
    if (!w->invalid) { w->ix0 = x0; w->iy0 = y0; w->ix1 = x1; w->iy1 = y1; }
    else {
        if (x0 < w->ix0) w->ix0 = x0;
        if (y0 < w->iy0) w->iy0 = y0;
        if (x1 > w->ix1) w->ix1 = x1;
        if (y1 > w->iy1) w->iy1 = y1;
    }
    w->invalid = 1;
    if (erase) w->erase = 1;
    // Soul draws children into their parent's pixels, so a parent repaint covers them.
    Win **kids;
    int n = children_of(w->hwnd, &kids);
    for (int i = 0; i < n; i++) {
        Win *c = kids[i];
        int32_t r[4] = { x0 - c->x, y0 - c->y, x1 - c->x, y1 - c->y };
        if (r[2] > 0 && r[3] > 0 && r[0] < c->w && r[1] < c->h) win_invalidate_rect(c, r, 1);
    }
    free(kids);
    gil_notify();
}

static void fb_alloc(Win *w, int keep) {
    int nw = w->w < 1 ? 1 : (w->w > 4096 ? 4096 : w->w);
    int nh = w->h < 1 ? 1 : (w->h > 4096 ? 4096 : w->h);
    if (w->pixels && nw == w->fb_w && nh == w->fb_h) return;
    uint32_t *px = malloc((size_t)nw * (size_t)nh * 4);
    uint32_t fill = 0xFFFFFFFFu;
    if (w->cls && w->cls->brush) {
        uint64_t b = w->cls->brush;
        if (b < 0x40) fill = rgb_px(gui_sys_color((int)b - 1));
        else { GdiObj *o = gdi_get(b, GO_BRUSH); if (o && o->style != 1) fill = rgb_px(o->color); }
    }
    for (size_t i = 0; i < (size_t)nw * (size_t)nh; i++) px[i] = fill;
    gui_lock();
    if (keep && w->pixels) {
        int cw = nw < w->fb_w ? nw : w->fb_w, ch = nh < w->fb_h ? nh : w->fb_h;
        for (int y = 0; y < ch; y++) memcpy(px + (size_t)y * nw, w->pixels + (size_t)y * w->fb_w, (size_t)cw * 4);
    }
    uint32_t *old = w->pixels;
    w->pixels = px; w->fb_w = nw; w->fb_h = nh;
    w->version++;
    gui_unlock();
    free(old);
    bump();
}

// ---- classes --------------------------------------------------------------------------------------

static WinClass *class_find(uint64_t name, int wide) {
    if (name < 0x10000) {
        for (int i = 0; i < 256; i++) if (g_gui.classes[i].used && g_gui.classes[i].atom == (uint16_t)name) return &g_gui.classes[i];
        return NULL;
    }
    char *s = gstr(name, wide);
    WinClass *found = NULL;
    for (int i = 0; i < 256 && !found; i++)
        if (g_gui.classes[i].used && strcasecmp(g_gui.classes[i].name, s) == 0) found = &g_gui.classes[i];
    free(s);
    return found;
}
static WinClass *class_add(const char *name) {
    for (int i = 0; i < 256; i++) {
        if (!g_gui.classes[i].used) {
            WinClass *c = &g_gui.classes[i];
            memset(c, 0, sizeof *c);
            c->used = 1;
            c->atom = (uint16_t)(0xC000 + i);
            snprintf(c->name, sizeof c->name, "%s", name);
            return c;
        }
    }
    return NULL;
}
static void builtin_class(const char *name, int ctl, uint32_t style, uint64_t brush, int extra) {
    WinClass *c = class_add(name);
    c->builtin = ctl;
    c->style = style;
    c->brush = brush;
    c->wnd_extra = extra;
    c->unicode = 1;
}
int ctl_class_for(const char *name) {
    WinClass *c = NULL;
    for (int i = 0; i < 256 && !c; i++) if (g_gui.classes[i].used && strcasecmp(g_gui.classes[i].name, name) == 0) c = &g_gui.classes[i];
    return c ? c->builtin : 0;
}

static void register_class(X86State *st, int wide, int ex) {
    const uint64_t wc = ARG(0);
    const char *L = ex ? L_WNDCLASSEX : L_WNDCLASS;
    int o = ex ? 1 : 0; // WNDCLASSEX starts with cbSize
    uint64_t name = gs_get(wc, L, 9 + o);
    char *cname = name < 0x10000 ? NULL : gstr(name, wide);
    if (!cname) { RET(0); return; }
    WinClass *c = NULL;
    for (int i = 0; i < 256 && !c; i++) if (g_gui.classes[i].used && !g_gui.classes[i].builtin && strcasecmp(g_gui.classes[i].name, cname) == 0) c = &g_gui.classes[i];
    if (c) { free(cname); set_last_error(st, 1410); RET(0); return; } // ERROR_CLASS_ALREADY_EXISTS
    c = class_add(cname);
    free(cname);
    if (!c) { RET(0); return; }
    c->style = (uint32_t)gs_get(wc, L, o);
    c->wndproc = gs_get(wc, L, 1 + o);
    c->cls_extra = (int32_t)gs_get(wc, L, 2 + o); c->wnd_extra = (int32_t)gs_get(wc, L, 3 + o);
    c->hinst = gs_get(wc, L, 4 + o);
    c->icon = gs_get(wc, L, 5 + o);
    c->cursor = gs_get(wc, L, 6 + o);
    c->brush = gs_get(wc, L, 7 + o);
    c->menu_name = gs_get(wc, L, 8 + o);
    if (c->menu_name >= 0x10000) { char *m = gstr(c->menu_name, wide); uint16_t *wm = u2w_gdup(m); free(m); c->menu_name = H2G(wm); } // keep a private UTF-16 copy (guest memory: pe_resource reads names there)
    c->unicode = wide;
    RET(c->atom);
}
HLE(u_RegisterClassA) { register_class(st, 0, 0); }
HLE(u_RegisterClassW) { register_class(st, 1, 0); }
HLE(u_RegisterClassExA) { register_class(st, 0, 1); }
HLE(u_RegisterClassExW) { register_class(st, 1, 1); }
static void unregister_class(X86State *st, int wide) {
    WinClass *c = class_find(ARG(0), wide);
    if (!c || c->builtin) { RET(0); return; }
    for (int i = 0; i < WIN_MAX; i++) if (g_gui.wins[i].used && g_gui.wins[i].cls == c) { RET(0); return; }
    c->used = 0;
    RET(1);
}
HLE(u_UnregisterClassA) { unregister_class(st, 0); }
HLE(u_UnregisterClassW) { unregister_class(st, 1); }
static void get_class_info(X86State *st, int wide, int ex) {
    WinClass *c = class_find(ARG(1), wide);
    if (!c) { set_last_error(st, 1411); RET(0); return; }
    const uint64_t wc = ARG(2);
    const char *L = ex ? L_WNDCLASSEX : L_WNDCLASS;
    int o = ex ? 1 : 0;
    gs_set(wc, L, o, c->style);
    gs_set(wc, L, 1 + o, c->builtin ? builtin_proc_addr(c->builtin) : c->wndproc);
    gs_set(wc, L, 2 + o, (uint32_t)c->cls_extra); gs_set(wc, L, 3 + o, (uint32_t)c->wnd_extra);
    gs_set(wc, L, 4 + o, c->hinst); gs_set(wc, L, 5 + o, c->icon); gs_set(wc, L, 6 + o, c->cursor);
    gs_set(wc, L, 7 + o, c->brush);
    gs_set(wc, L, 8 + o, 0);
    gs_set(wc, L, 9 + o, ARG(1));
    if (ex) gs_set(wc, L, 11, c->icon);
    RET(c->atom);
}
HLE(u_GetClassInfoA) { get_class_info(st, 0, 0); }
HLE(u_GetClassInfoW) { get_class_info(st, 1, 0); }
HLE(u_GetClassInfoExA) { get_class_info(st, 0, 1); }
HLE(u_GetClassInfoExW) { get_class_info(st, 1, 1); }
static void get_class_name(X86State *st, int wide) {
    Win *w = win_get(ARG(0));
    if (!w || !w->cls) { RET(0); return; }
    RET(put_str(ARG(1), (size_t)ARGI(2), w->cls->name, wide));
}
HLE(u_GetClassNameA) { get_class_name(st, 0); }
HLE(u_GetClassNameW) { get_class_name(st, 1); }
static void class_long(X86State *st, int set) {
    Win *w = win_get(ARG(0));
    if (!w || !w->cls) { RET(0); return; }
    WinClass *c = w->cls;
    uint64_t *field = NULL, v = 0;
    switch (ARGI(1)) {
    case -10: field = &c->brush; break;
    case -12: field = &c->cursor; break;
    case -14: case -34: field = &c->icon; break;
    case -16: field = &c->hinst; break;
    case -24: v = c->builtin ? builtin_proc_addr(c->builtin) : c->wndproc; if (set) c->wndproc = ARG(2); RET(v); return;
    case -26: v = c->style; if (set) c->style = (uint32_t)ARG(2); RET(v); return;
    case -32: RET(c->atom); return;
    case -20: RET(c->cls_extra); return;
    case -18: RET(c->wnd_extra); return;
    default: RET(0); return;
    }
    v = *field;
    if (set) *field = ARG(2);
    RET(v);
}
HLE(u_GetClassLongPtr) { class_long(st, 0); }
HLE(u_SetClassLongPtr) { class_long(st, 1); }

uint64_t menu_from_resource(const uint8_t *res);

// ---- built-in window procedures -----------------------------------------------------------------

static void builtin_thunk(X86State *st) { // CallWindowProc / guest call into a built-in procedure
    SoulProcess *p = g_proc;
    HleThunk *t = &p->thunks[st->thread->cur_thunk];
    int ctl = t->name ? atoi(t->name + strlen("SoulCtlProc")) : 0;
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    if (ctl == CTL_DIALOG) RET(dlg_proc(st, w, (uint32_t)ARG(1), ARG(2), ARG(3), g_gui.cur_unicode));
    else RET(ctl_proc(st, w, ctl, (uint32_t)ARG(1), ARG(2), ARG(3), g_gui.cur_unicode));
}
uint64_t builtin_proc_addr(int ctl) {
    if (ctl <= 0 || ctl >= 32) return 0;
    if (!g_gui.builtin_procs[ctl]) {
        char name[32];
        snprintf(name, sizeof name, "SoulCtlProc%d", ctl);
        uint32_t id = hle_thunk_for(g_proc, builtin_thunk, "user32", name);
        g_proc->thunks[id].argbytes = g_mode32 ? 16 : 0; // a WNDPROC: stdcall (hwnd, msg, wParam, lParam)
        g_gui.builtin_procs[ctl] = g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE;
    }
    return g_gui.builtin_procs[ctl];
}
static int builtin_of(uint64_t proc) {
    for (int i = 1; i < 32; i++) if (g_gui.builtin_procs[i] && g_gui.builtin_procs[i] == proc) return i;
    return 0;
}

// ---- sending messages --------------------------------------------------------------------------

static uint64_t call_proc(X86State *st, Win *w, uint64_t proc, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    int b = builtin_of(proc);
    if (b) {
        int saved = g_gui.cur_unicode;
        g_gui.cur_unicode = unicode;
        uint64_t r = b == CTL_DIALOG ? dlg_proc(st, w, msg, wp, lp, unicode) : ctl_proc(st, w, b, msg, wp, lp, unicode);
        g_gui.cur_unicode = saved;
        return r;
    }
    if (!proc) return win_def_proc(st, w, msg, wp, lp, unicode);
    uint64_t hwnd = w->hwnd;
    int pu = w->unicode;
    void *tmp = NULL;
    uint64_t r;
    if (pu != unicode) { // translate string parameters between ANSI (UTF-8) and UTF-16
        if (msg == WM_SETTEXT && lp) {
            char *s = gstr(lp, unicode);
            if (pu) tmp = u2w_gdup(s); else tmp = g_strdup(s);
            free(s);
            lp = H2G(tmp);
        } else if (msg == WM_GETTEXT && lp && wp) {
            size_t cap = (size_t)wp * 4 + 8;
            tmp = g_calloc(1, cap);
            uint64_t args[4] = { hwnd, msg, pu ? cap / 2 : cap, H2G(tmp) };
            soul_guest_call(st, proc, 4, args);
            char *s = pu ? w2u_dup(tmp) : strdup(tmp);
            r = put_str(lp, (size_t)wp, s, unicode);
            free(s); g_free(tmp);
            return r;
        } else if (msg == WM_CHAR && !pu && wp >= 0x80) {
            char u8[8]; uint16_t c = (uint16_t)wp;
            size_t n = w2u(&c, 1, u8, sizeof u8);
            for (size_t i = 0; i < n; i++) { uint64_t a[4] = { hwnd, msg, (uint8_t)u8[i], lp }; soul_guest_call(st, proc, 4, a); }
            return 0;
        } else if ((msg == WM_CREATE || msg == WM_NCCREATE) && lp) {
            size_t csz = gs_size(L_CREATESTRUCT);
            uint8_t *cs = g_malloc(csz);
            memcpy(cs, G2H(lp), csz);
            uint64_t csg = H2G(cs);
            uint64_t name = gs_get(csg, L_CREATESTRUCT, 9), cls = gs_get(csg, L_CREATESTRUCT, 10);
            void *n2 = NULL, *c2 = NULL;
            if (name >= 0x10000) { char *s = gstr(name, unicode); n2 = pu ? (void *)u2w_gdup(s) : (void *)g_strdup(s); free(s); name = H2G(n2); }
            if (cls >= 0x10000) { char *s = gstr(cls, unicode); c2 = pu ? (void *)u2w_gdup(s) : (void *)g_strdup(s); free(s); cls = H2G(c2); }
            gs_set(csg, L_CREATESTRUCT, 9, name); gs_set(csg, L_CREATESTRUCT, 10, cls);
            uint64_t a[4] = { hwnd, msg, wp, csg };
            r = soul_guest_call(st, proc, 4, a);
            g_free(n2); g_free(c2); g_free(cs);
            return r;
        }
    }
    uint64_t args[4] = { hwnd, msg, wp, lp };
    r = soul_guest_call(st, proc, 4, args);
    g_free(tmp);
    return r;
}
uint64_t win_call_proc(X86State *st, Win *w, uint64_t proc, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    return call_proc(st, w, proc, msg, wp, lp, unicode);
}

uint64_t win_send_ex(X86State *st, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    Win *w = win_get(hwnd);
    if (!w) return 0;
    if (w->ctl == CTL_DIALOG && w->wndproc == builtin_proc_addr(CTL_DIALOG)) return call_proc(st, w, w->wndproc, msg, wp, lp, unicode);
    return call_proc(st, w, w->wndproc, msg, wp, lp, unicode);
}
uint64_t win_send(X86State *st, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) { return win_send_ex(st, hwnd, msg, wp, lp, 1); }

static void queue_add(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, uint32_t tid, int front) {
    GuiState *g = &g_gui;
    if (g->qlen == g->qcap) { g->qcap = g->qcap ? g->qcap * 2 : 64; g->queue = realloc(g->queue, sizeof(QMsg) * (size_t)g->qcap); }
    QMsg m = { hwnd, msg, wp, lp, tick_ms(), g->mouse_x, g->mouse_y, tid };
    if (front) { memmove(g->queue + 1, g->queue, sizeof(QMsg) * (size_t)g->qlen); g->queue[0] = m; }
    else g->queue[g->qlen] = m;
    g->qlen++;
    gil_notify();
}
void win_post(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp) {
    Win *w = win_get(hwnd);
    queue_add(hwnd, msg, wp, lp, w ? w->tid : cur_tid(), 0);
}
void win_notify_parent(X86State *st, Win *w, uint32_t code) {
    if (!w->parent) return;
    win_send(st, w->parent, WM_COMMAND, (uint64_t)(((uint32_t)code << 16) | (uint16_t)w->id), w->hwnd);
}
uint64_t win_ctl_color(X86State *st, Win *w, uint64_t hdc, uint32_t msg) {
    uint64_t target = w->parent ? w->parent : w->hwnd;
    uint64_t br = win_send(st, target, msg, hdc, w->hwnd);
    if (!br) br = win_def_proc(st, w, msg, hdc, w->hwnd, 1);
    return br;
}

// ---- focus / activation ---------------------------------------------------------------------------

static void set_active(X86State *st, uint64_t hwnd);

void win_set_focus(X86State *st, uint64_t hwnd) {
    GuiState *g = &g_gui;
    uint64_t old = g->focus;
    if (old == hwnd) return;
    Win *w = win_get(hwnd);
    if (hwnd && !w) return;
    if (w) { Win *top = win_top(w); if (top && top->hwnd != g->active) set_active(st, top->hwnd); }
    g->focus = hwnd;
    if (old && win_get(old)) win_send(st, old, WM_KILLFOCUS, hwnd, 0);
    if (g->focus == hwnd && w) win_send(st, hwnd, WM_SETFOCUS, old, 0);
    bump();
}

static void set_active(X86State *st, uint64_t hwnd) {
    GuiState *g = &g_gui;
    uint64_t old = g->active;
    if (old == hwnd) return;
    Win *w = win_get(hwnd);
    g->active = hwnd;
    if (w) w->zstamp = ++g->zstamp;
    bump();
    if (old && win_get(old)) {
        win_send(st, old, WM_NCACTIVATE, 0, 0);
        win_send(st, old, WM_ACTIVATE, 0, hwnd);
    }
    if (w) {
        win_send(st, hwnd, WM_NCACTIVATE, 1, 0);
        win_send(st, hwnd, WM_ACTIVATE, 1, old);
    }
}

// ---- window creation / destruction ----------------------------------------------------------------

uint64_t win_create(X86State *st, uint32_t exstyle, uint64_t cls_arg, uint64_t name_arg, uint32_t style, int32_t x, int32_t y,
                    int32_t cw, int32_t ch, uint64_t parent, uint64_t menu, uint64_t hinst, uint64_t param, int wide) {
    GuiState *g = &g_gui;
    WinClass *c = class_find(cls_arg, wide);
    if (!c) {
        char *n = gstr(cls_arg, wide);
        if (g_proc->verbose) soul_log("[pencere] Bilinmeyen pencere sınıfı: %s\n", n ? n : "(atom)");
        free(n);
        set_last_error(st, 1407);
        return 0;
    }
    int message_only = parent == HWND_MESSAGE;
    if (message_only) parent = 0;
    Win *pw = parent ? win_get(parent) : NULL;
    if (parent && !pw && parent != DESKTOP_HWND) { set_last_error(st, 1400); return 0; }
    if (parent == DESKTOP_HWND) parent = 0;
    int child = (style & WS_CHILD) && pw;
    Win *w = NULL;
    gui_lock();
    for (int i = 0; i < WIN_MAX; i++) if (!g->wins[i].used) { w = &g->wins[i]; memset(w, 0, sizeof *w); w->hwnd = WIN_HANDLE_BASE + (uint64_t)i * 4; break; }
    if (!w) { gui_unlock(); set_last_error(st, 8); return 0; }
    w->used = 1;
    w->seq = ++g->seq;
    w->cls = c;
    w->wndproc = c->builtin ? builtin_proc_addr(c->builtin) : c->wndproc;
    w->unicode = c->builtin ? wide : c->unicode;
    w->style = style & ~WS_VISIBLE;
    w->exstyle = exstyle;
    w->parent = child ? parent : 0;
    w->owner = child ? 0 : parent;
    w->hinst = hinst;
    w->message_only = message_only;
    w->enabled = !(style & WS_DISABLED);
    w->tid = cur_tid();
    w->ctl = c->builtin;
    w->limit = 30000;
    w->cur_sel = -1;
    w->progress_max = 100;
    if (g->pending_dialog) {
        w->is_dialog = 1;
        w->dlgproc = g->pending_dlgproc;
        w->dlg_unicode = g->pending_dlg_unicode;
        g->pending_dialog = 0;
    }
    char *t = gstr(name_arg, wide);
    w->text = t ? t : strdup("");
    if (child) {
        w->id = menu & 0xFFFFFFFFu;
        w->x = x == CW_USEDEFAULT ? 0 : x; w->y = y == CW_USEDEFAULT ? 0 : y;
        w->w = cw == CW_USEDEFAULT ? 0 : cw; w->h = ch == CW_USEDEFAULT ? 0 : ch;
        if (w->w < 0) w->w = 0;
        if (w->h < 0) w->h = 0;
    } else {
        w->menu = menu;
        if (cw == CW_USEDEFAULT || cw <= 0) { cw = g->screen_w * 3 / 4; ch = g->screen_h * 3 / 4; }
        if (ch == CW_USEDEFAULT || ch <= 0) ch = g->screen_h * 3 / 4;
        if (x == CW_USEDEFAULT) {
            int k = g->cascade++ % 8;
            x = 24 + 28 * k; y = 24 + 28 * k;
            if (x + cw > g->screen_w) x = g->screen_w > cw ? (g->screen_w - cw) / 2 : 0;
            if (y + ch > g->screen_h) y = g->screen_h > ch ? (g->screen_h - ch) / 2 : 0;
        }
        w->x = x; w->y = y; w->w = cw; w->h = ch;
        if (style & WS_MAXIMIZE) { w->x = 0; w->y = 0; w->w = g->screen_w; w->h = g->screen_h; w->maximized = 1; }
    }
    w->extra_size = c->wnd_extra > 0 ? c->wnd_extra : 0;
    if (c->builtin == CTL_DIALOG && w->extra_size < 48) w->extra_size = 48;
    w->extra = calloc(1, (size_t)w->extra_size + 8);
    gui_unlock();
    if (!child && c->menu_name && !menu) {
        const void *res = pe_resource(c->hinst ? c->hinst : hinst, 4, c->menu_name, 1, NULL);
        if (res) { uint64_t hm = menu_from_resource(res); gui_lock(); w->menu = hm; gui_unlock(); }
    }
    if (!child) fb_alloc(w, 0);
    uint64_t hwnd = w->hwnd;

    // CREATESTRUCT
    uint8_t *cs = g_calloc(1, gs_size(L_CREATESTRUCT));
    uint64_t csp = H2G(cs);
    gs_set(csp, L_CREATESTRUCT, 0, param); gs_set(csp, L_CREATESTRUCT, 1, hinst);
    gs_set(csp, L_CREATESTRUCT, 2, menu); gs_set(csp, L_CREATESTRUCT, 3, parent);
    gs_set(csp, L_CREATESTRUCT, 4, (uint32_t)w->h); gs_set(csp, L_CREATESTRUCT, 5, (uint32_t)w->w);
    gs_set(csp, L_CREATESTRUCT, 6, (uint32_t)w->y); gs_set(csp, L_CREATESTRUCT, 7, (uint32_t)w->x);
    gs_set(csp, L_CREATESTRUCT, 8, style);
    gs_set(csp, L_CREATESTRUCT, 9, name_arg); gs_set(csp, L_CREATESTRUCT, 10, cls_arg);
    gs_set(csp, L_CREATESTRUCT, 11, exstyle);
    if (!win_send_ex(st, hwnd, WM_NCCREATE, 0, csp, wide) && win_get(hwnd)) {
        g_free(cs);
        win_destroy(st, win_get(hwnd));
        return 0;
    }
    if (!win_get(hwnd)) { g_free(cs); return 0; }
    int64_t r = (int64_t)win_send_ex(st, hwnd, WM_CREATE, 0, csp, wide);
    g_free(cs);
    w = win_get(hwnd);
    if (!w) return 0;
    if (r == -1) {
        win_destroy(st, w);
        return 0;
    }
    win_send(st, hwnd, WM_SIZE, w->maximized ? 2 : 0, ((uint64_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
    win_send(st, hwnd, WM_MOVE, 0, ((uint64_t)(uint16_t)w->y << 16) | (uint16_t)w->x);
    if (child && pw) win_send(st, parent, 0x0210 /* WM_PARENTNOTIFY */, WM_CREATE | ((uint64_t)(uint16_t)w->id << 16), hwnd);
    if ((style & WS_VISIBLE) && win_get(hwnd)) {
        win_show(st, win_get(hwnd), 5);
    }
    bump();
    return hwnd;
}
static uint64_t create_window(X86State *st, int wide) {
    return win_create(st, (uint32_t)ARG(0), ARG(1), ARG(2), (uint32_t)ARG(3), ARGI(4), ARGI(5), ARGI(6), ARGI(7),
                      hle_arg(st, 8), hle_arg(st, 9), hle_arg(st, 10), hle_arg(st, 11), wide);
}
HLE(u_CreateWindowExA) { RET(create_window(st, 0)); }
HLE(u_CreateWindowExW) { RET(create_window(st, 1)); }

static void kill_window_timers(uint64_t hwnd) {
    for (int i = 0; i < 256; i++) if (g_gui.timers[i].used && g_gui.timers[i].hwnd == hwnd) g_gui.timers[i].used = 0;
}

static void destroy_rec(X86State *st, Win *w) {
    uint64_t hwnd = w->hwnd;
    w->destroying = 1;
    win_send(st, hwnd, WM_DESTROY, 0, 0);
    Win **kids;
    int n = children_of(hwnd, &kids);
    for (int i = 0; i < n; i++) if (kids[i]->used && !kids[i]->destroying) destroy_rec(st, kids[i]);
    free(kids);
    if (!win_get(hwnd)) return;
    win_send(st, hwnd, WM_NCDESTROY, 0, 0);
    GuiState *g = &g_gui;
    kill_window_timers(hwnd);
    int j = 0;
    for (int i = 0; i < g->qlen; i++) if (g->queue[i].hwnd != hwnd) g->queue[j++] = g->queue[i];
    g->qlen = j;
    if (g->focus == hwnd) g->focus = 0;
    if (g->capture == hwnd) g->capture = 0;
    if (g->caret_hwnd == hwnd) g->caret_hwnd = 0;
    if (g->active == hwnd) g->active = 0;
    if (g->popup_hwnd == hwnd) { g->popup_done = 1; g->popup_result = 0; }
    gui_lock();
    uint32_t *px = w->pixels;
    char *text = w->text;
    w->used = 0;
    w->pixels = NULL;
    w->text = NULL;
    gui_unlock();
    free(px);
    free(text);
    free(w->extra);
    for (int i = 0; i < w->item_count; i++) free(w->items[i]);
    free(w->items);
    free(w->item_data);
    if (w->cdata && w->cdata_free) w->cdata_free(w->cdata);
    w->cdata = NULL;
    bump();
}

void win_destroy(X86State *st, Win *w) {
    if (!w || w->destroying) return;
    uint64_t hwnd = w->hwnd, parent = w->parent, owner = w->owner;
    int was_active = g_gui.active == hwnd;
    int x = w->x, y = w->y, ww = w->w, hh = w->h, shown = w->visible;
    // owned top-level windows go first
    for (int i = 0; i < WIN_MAX; i++) {
        Win *o = &g_gui.wins[i];
        if (o->used && !o->parent && o->owner == hwnd && !o->destroying) win_destroy(st, o);
    }
    if (!win_get(hwnd)) return;
    if (g_gui.focus && (g_gui.focus == hwnd || is_child_of(hwnd, win_get(g_gui.focus)))) {
        uint64_t f = g_gui.focus;
        g_gui.focus = 0;
        if (win_get(f)) win_send(st, f, WM_KILLFOCUS, 0, 0);
    }
    destroy_rec(st, w);
    if (parent && shown) { Win *p = win_get(parent); int32_t r[4] = { x, y, x + ww, y + hh }; if (p) win_invalidate_rect(p, r, 1); }
    if (was_active) {
        g_gui.active = 0;
        Win *next = win_get(owner);
        if (!next || !next->visible) {
            Win *best = NULL;
            for (int i = 0; i < WIN_MAX; i++) {
                Win *o = &g_gui.wins[i];
                if (o->used && !o->parent && o->visible && !o->message_only && (!best || o->zstamp > best->zstamp)) best = o;
            }
            next = best;
        }
        if (next) set_active(st, next->hwnd);
    }
}
HLE(u_DestroyWindow) {
    Win *w = win_get(ARG(0));
    if (!w) { set_last_error(st, 1400); RET(0); return; }
    win_destroy(st, w);
    RET(1);
}

void win_show(X86State *st, Win *w, int cmd) {
    GuiState *g = &g_gui;
    uint64_t hwnd = w->hwnd;
    if (cmd == 0) { // SW_HIDE
        if (!w->visible) return;
        win_send(st, hwnd, WM_SHOWWINDOW, 0, 0);
        if (!(w = win_get(hwnd))) return;
        gui_lock(); w->visible = 0; gui_unlock();
        if (w->parent) { Win *p = win_get(w->parent); int32_t r[4] = { w->x, w->y, w->x + w->w, w->y + w->h }; if (p) win_invalidate_rect(p, r, 1); }
        if (g->focus && (g->focus == hwnd || is_child_of(hwnd, win_get(g->focus)))) win_set_focus(st, w->parent);
        if (g->active == hwnd) {
            g->active = 0;
            Win *o = win_get(w->owner);
            if (o && o->visible) set_active(st, o->hwnd);
        }
        bump();
        return;
    }
    if ((cmd == 3 || cmd == 9 || cmd == 1) && !w->parent) {
        int maximize = cmd == 3;
        if (maximize != w->maximized && (maximize || w->maximized)) {
            if (maximize) {
                // remember the normal rectangle in the scroll fields of the last scroll bar slot
                w->sb_min[0] = w->x; w->sb_max[0] = w->y; w->sb_page[0] = w->w; w->sb_pos[0] = w->h;
                w->maximized = 1;
                win_move(st, w, 0, 0, g->screen_w, g->screen_h, 0);
            } else if (w->maximized) {
                w->maximized = 0;
                win_move(st, w, w->sb_min[0], w->sb_max[0], w->sb_page[0], w->sb_pos[0], 0);
            }
            if (!(w = win_get(hwnd))) return;
        }
    }
    if (!w->visible) {
        win_send(st, hwnd, WM_SHOWWINDOW, 1, 0);
        if (!(w = win_get(hwnd))) return;
        gui_lock(); w->visible = 1; gui_unlock();
        win_invalidate_rect(w, NULL, 1);
        bump();
    }
    if (!w->parent && !w->message_only && cmd != 4 && cmd != 7 && cmd != 8 && w->enabled) set_active(st, hwnd);
}
HLE(u_ShowWindow) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    int was = w->visible;
    int cmd = ARGI(1);
    if (cmd == 6 || cmd == 2 || cmd == 7 || cmd == 11) { RET(was); return; } // minimize: ignored
    win_show(st, w, cmd);
    RET(was);
}
HLE(u_ShowWindowAsync) { u_ShowWindow(st); }

void win_move(X86State *st, Win *w, int x, int y, int cw, int ch, int flags) {
    uint64_t hwnd = w->hwnd;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    int moved = x != w->x || y != w->y, sized = cw != w->w || ch != w->h;
    if (!moved && !sized) return;
    int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
    gui_lock();
    w->x = x; w->y = y; w->w = cw; w->h = ch;
    gui_unlock();
    if (!w->parent && sized) fb_alloc(w, 1);
    if (w->parent && w->visible) {
        Win *p = win_get(w->parent);
        int32_t r[4] = { ox, oy, ox + ow, oy + oh };
        if (p) win_invalidate_rect(p, r, 1);
    }
    if (w->visible || !w->parent) win_invalidate_rect(w, NULL, 1);
    uint8_t *wpp = g_calloc(1, gs_size(L_WINDOWPOS));
    uint64_t wpg = H2G(wpp);
    gs_set(wpg, L_WINDOWPOS, 0, hwnd);
    gs_set(wpg, L_WINDOWPOS, 2, (uint32_t)x); gs_set(wpg, L_WINDOWPOS, 3, (uint32_t)y);
    gs_set(wpg, L_WINDOWPOS, 4, (uint32_t)cw); gs_set(wpg, L_WINDOWPOS, 5, (uint32_t)ch);
    gs_set(wpg, L_WINDOWPOS, 6, (uint32_t)flags | (moved ? 0 : 2) | (sized ? 0 : 1));
    win_send(st, hwnd, WM_WINDOWPOSCHANGED, 0, wpg);
    g_free(wpp);
    if (!w->parent) bump();
}
HLE(u_MoveWindow) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    win_move(st, w, ARGI(1), ARGI(2), ARGI(3), ARGI(4), 0);
    RET(1);
}
HLE(u_SetWindowPos) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    uint32_t flags = (uint32_t)hle_arg(st, 6);
    uint64_t after = ARG(1);
    int x = flags & 2 ? w->x : ARGI(2), y = flags & 2 ? w->y : ARGI(3);
    int cw = flags & 1 ? w->w : (int)(int32_t)hle_arg(st, 4), ch = flags & 1 ? w->h : (int)(int32_t)hle_arg(st, 5);
    if (!(flags & 4) && !w->parent && (after == 0 || after == (uint64_t)-1)) { w->zstamp = ++g_gui.zstamp; bump(); }
    win_move(st, w, x, y, cw, ch, (int)flags);
    if ((w = win_get(ARG(0)))) {
        if (flags & 0x40) win_show(st, w, flags & 0x10 ? 8 : 5);
        else if (flags & 0x80) win_show(st, w, 0);
    }
    RET(1);
}
HLE(u_BeginDeferWindowPos) { RET(0x7100); }
HLE(u_DeferWindowPos) {
    // apply immediately: (hdwp, hwnd, after, x, y, cx, cy, flags)
    Win *w = win_get(ARG(1));
    if (w) {
        uint32_t flags = (uint32_t)hle_arg(st, 7);
        int x = flags & 2 ? w->x : (int)(int32_t)ARG(3), y = flags & 2 ? w->y : (int)(int32_t)hle_arg(st, 4);
        int cw = flags & 1 ? w->w : (int)(int32_t)hle_arg(st, 5), ch = flags & 1 ? w->h : (int)(int32_t)hle_arg(st, 6);
        win_move(st, w, x, y, cw, ch, (int)flags);
        if ((w = win_get(ARG(1))) && (flags & 0x40)) win_show(st, w, 8);
        if ((w = win_get(ARG(1))) && (flags & 0x80)) win_show(st, w, 0);
    }
    RET(ARG(0));
}
HLE_STUB_RET(u_EndDeferWindowPos, 1)
HLE(u_BringWindowToTop) { Win *w = win_get(ARG(0)); if (w && !w->parent) { w->zstamp = ++g_gui.zstamp; set_active(st, w->hwnd); bump(); } RET(w != NULL); }
HLE(u_SetForegroundWindow) { Win *w = win_get(ARG(0)); if (w) { Win *t = win_top(w); t->zstamp = ++g_gui.zstamp; set_active(st, t->hwnd); bump(); } RET(w != NULL); }
HLE(u_SetActiveWindow) { uint64_t old = g_gui.active; Win *w = win_get(ARG(0)); if (w) set_active(st, win_top(w)->hwnd); RET(old); }
HLE(u_GetActiveWindow) { RET(g_gui.active); }
HLE(u_GetForegroundWindow) { RET(g_gui.active); }
HLE(u_SetFocus) {
    uint64_t old = g_gui.focus;
    Win *w = win_get(ARG(0));
    if (ARG(0) && !w) { RET(0); return; }
    if (w && !win_enabled_chain(w)) { RET(0); return; }
    win_set_focus(st, ARG(0));
    RET(old);
}
HLE(u_GetFocus) { RET(g_gui.focus); }
HLE(u_SetCapture) { uint64_t old = g_gui.capture; g_gui.capture = ARG(0); RET(old); }
HLE(u_ReleaseCapture) {
    uint64_t old = g_gui.capture;
    g_gui.capture = 0;
    if (old && win_get(old)) win_send(st, old, WM_CAPTURECHANGED, 0, 0);
    RET(1);
}
HLE(u_GetCapture) { RET(g_gui.capture); }
HLE(u_EnableWindow) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    int was_disabled = !w->enabled;
    int en = ARG(1) != 0;
    if (en != w->enabled) {
        gui_lock(); w->enabled = en; gui_unlock();
        if (!en && g_gui.focus && (g_gui.focus == w->hwnd || is_child_of(w->hwnd, win_get(g_gui.focus)))) win_set_focus(st, 0);
        win_send(st, w->hwnd, WM_ENABLE, (uint64_t)en, 0);
        if ((w = win_get(ARG(0)))) win_invalidate_rect(w, NULL, 1);
        bump();
    }
    RET(was_disabled);
}
HLE(u_IsWindowEnabled) { Win *w = win_get(ARG(0)); RET(w && w->enabled); }
HLE(u_IsWindow) { RET(win_get(ARG(0)) != NULL); }
HLE(u_IsWindowVisible) { Win *w = win_get(ARG(0)); RET(w && win_shown(w)); }
HLE(u_IsIconic) { RET(0); }
HLE(u_IsZoomed) { Win *w = win_get(ARG(0)); RET(w && w->maximized); }
HLE(u_IsWindowUnicode) { Win *w = win_get(ARG(0)); RET(w && w->unicode); }
HLE(u_IsChild) { Win *c = win_get(ARG(1)); RET(c && is_child_of(ARG(0), c)); }
HLE(u_GetParent) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    if (w->parent) RET(w->parent);
    else RET((w->style & WS_POPUP) ? w->owner : 0);
}
HLE(u_SetParent) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    uint64_t old = w->parent ? w->parent : DESKTOP_HWND;
    Win *np = win_get(ARG(1));
    gui_lock();
    if (np) { w->parent = np->hwnd; w->style |= WS_CHILD; }
    else w->parent = 0;
    gui_unlock();
    if (!w->parent && !w->pixels) fb_alloc(w, 0);
    win_invalidate_rect(w, NULL, 1);
    bump();
    RET(old);
}
HLE(u_GetAncestor) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    switch (ARGI(1)) {
    case 1: RET(w->parent ? w->parent : DESKTOP_HWND); break;
    case 2: RET(win_top(w)->hwnd); break;
    default: {
        Win *t = win_top(w);
        while (t->owner && win_get(t->owner)) t = win_top(win_get(t->owner));
        RET(t->hwnd);
    }
    }
}
HLE(u_GetWindow) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    int cmd = ARGI(1);
    if (cmd == 4) { RET(w->owner); return; }
    if (cmd == 6) { // GW_ENABLEDPOPUP
        for (int i = 0; i < WIN_MAX; i++) { Win *o = &g_gui.wins[i]; if (o->used && o->owner == w->hwnd && o->visible && o->enabled) { RET(o->hwnd); return; } }
        RET(0); return;
    }
    Win **list;
    int n = children_of(cmd == 5 ? w->hwnd : w->parent, &list);
    uint64_t r = 0;
    if (cmd == 5) r = n ? list[0]->hwnd : 0;
    else if (cmd == 0) r = n ? list[0]->hwnd : 0;
    else if (cmd == 1) r = n ? list[n - 1]->hwnd : 0;
    else for (int i = 0; i < n; i++) if (list[i] == w) { if (cmd == 2 && i + 1 < n) r = list[i + 1]->hwnd; if (cmd == 3 && i > 0) r = list[i - 1]->hwnd; }
    free(list);
    RET(r);
}
HLE(u_GetTopWindow) { Win **list; int n = children_of(ARG(0) == DESKTOP_HWND ? 0 : ARG(0), &list); RET(n ? list[0]->hwnd : 0); free(list); }
HLE(u_GetDesktopWindow) { RET(DESKTOP_HWND); }
HLE(u_GetShellWindow) { RET(DESKTOP_HWND); }
HLE(u_GetLastActivePopup) { RET(ARG(0)); }
HLE(u_GetWindowThreadProcessId) { Win *w = win_get(ARG(0)); if (ARG(1)) mem_wr(ARG(1), 4, 4242); RET(w ? w->tid : 0); }

static void enum_rec(X86State *st, uint64_t parent, uint64_t proc, uint64_t lp, int recurse, int *stop) {
    Win **list;
    int n = children_of(parent, &list);
    for (int i = 0; i < n && !*stop; i++) {
        uint64_t h = list[i]->hwnd;
        if (!win_get(h)) continue;
        uint64_t args[2] = { h, lp };
        if (!(soul_guest_call(st, proc, 2, args) & 0xFFFFFFFF)) { *stop = 1; break; }
        if (recurse && win_get(h)) enum_rec(st, h, proc, lp, 1, stop);
    }
    free(list);
}
HLE(u_EnumWindows) { int stop = 0; enum_rec(st, 0, ARG(0), ARG(1), 0, &stop); RET(1); }
HLE(u_EnumChildWindows) { int stop = 0; if (ARG(0)) enum_rec(st, ARG(0), ARG(1), ARG(2), 1, &stop); else enum_rec(st, 0, ARG(1), ARG(2), 0, &stop); RET(1); }
HLE(u_EnumThreadWindows) { int stop = 0; enum_rec(st, 0, ARG(1), ARG(2), 0, &stop); RET(1); }
static void find_window(X86State *st, int wide, int ex) {
    uint64_t parent = ex ? ARG(0) : 0, after = ex ? ARG(1) : 0;
    uint64_t cls = ARG(ex ? 2 : 0), title = ARG(ex ? 3 : 1);
    WinClass *c = cls ? class_find(cls, wide) : NULL;
    if (cls && !c) { RET(0); return; }
    char *t = gstr(title, wide);
    Win **list;
    int n = children_of(parent == DESKTOP_HWND ? 0 : parent, &list);
    uint64_t r = 0;
    int started = after == 0;
    for (int i = 0; i < n && !r; i++) {
        if (!started) { if (list[i]->hwnd == after) started = 1; continue; }
        if (c && list[i]->cls != c) continue;
        if (t && strcmp(list[i]->text, t) != 0) continue;
        r = list[i]->hwnd;
    }
    free(list); free(t);
    RET(r);
}
HLE(u_FindWindowA) { find_window(st, 0, 0); }
HLE(u_FindWindowW) { find_window(st, 1, 0); }
HLE(u_FindWindowExA) { find_window(st, 0, 1); }
HLE(u_FindWindowExW) { find_window(st, 1, 1); }

// ---- text ---------------------------------------------------------------------------------------
static void set_window_text(X86State *st, int wide) {
    if (!win_get(ARG(0))) { RET(0); return; }
    RET(win_send_ex(st, ARG(0), WM_SETTEXT, 0, ARG(1), wide));
}
HLE(u_SetWindowTextA) { set_window_text(st, 0); }
HLE(u_SetWindowTextW) { set_window_text(st, 1); }
static void get_window_text(X86State *st, int wide) {
    if (!win_get(ARG(0)) || ARGI(2) <= 0) { RET(0); return; }
    if (wide) ((uint16_t *)PTR(1))[0] = 0; else ((char *)PTR(1))[0] = 0;
    RET(win_send_ex(st, ARG(0), WM_GETTEXT, (uint64_t)ARGI(2), ARG(1), wide));
}
HLE(u_GetWindowTextA) { get_window_text(st, 0); }
HLE(u_GetWindowTextW) { get_window_text(st, 1); }
HLE(u_InternalGetWindowText) { Win *w = win_get(ARG(0)); RET(w ? put_str(ARG(1), (size_t)ARGI(2), w->text, 1) : 0); }
HLE(u_GetWindowTextLengthA) { RET(win_get(ARG(0)) ? win_send_ex(st, ARG(0), WM_GETTEXTLENGTH, 0, 0, 0) : 0); }
HLE(u_GetWindowTextLengthW) { RET(win_get(ARG(0)) ? win_send_ex(st, ARG(0), WM_GETTEXTLENGTH, 0, 0, 1) : 0); }

// ---- window longs -------------------------------------------------------------------------------
static uint64_t window_long(X86State *st, int wide, int set, int size64) {
    Win *w = win_get(ARG(0));
    if (!w) { set_last_error(st, 1400); return 0; }
    int idx = ARGI(1);
    uint64_t v = ARG(2), old = 0;
    if (!size64) v = (uint64_t)(int64_t)(int32_t)v;
    switch (idx) {
    case -4: // GWLP_WNDPROC
        old = w->wndproc;
        if (set) { w->wndproc = v; if (!builtin_of(v)) w->unicode = wide; }
        return old;
    case -6: old = w->hinst; if (set) w->hinst = v; return old;
    case -8: old = w->parent ? w->parent : w->owner; if (set) { if (w->parent) w->parent = v; else w->owner = v; } return old;
    case -12: old = w->id; if (set) w->id = v; return old;
    case -16: old = w->style | (w->visible ? WS_VISIBLE : 0) | (w->enabled ? 0 : WS_DISABLED); if (set) { w->style = (uint32_t)v & ~WS_VISIBLE; bump(); } return (uint32_t)old;
    case -20: old = w->exstyle; if (set) w->exstyle = (uint32_t)v; return old;
    case -21: old = w->userdata; if (set) w->userdata = v; return old;
    default: break;
    }
    if (w->is_dialog && idx == PSZ) { old = w->dlgproc; if (set) w->dlgproc = v; return old; } // DWLP_DLGPROC
    int bytes = size64 ? 8 : 4;
    if (idx < 0 || idx + bytes > w->extra_size) { set_last_error(st, 1413); return 0; }
    memcpy(&old, w->extra + idx, (size_t)bytes);
    if (!size64) old = (uint64_t)(int64_t)(int32_t)old;
    if (set) memcpy(w->extra + idx, &v, (size_t)bytes);
    return old;
}
HLE(u_GetWindowLongPtrA) { RET(window_long(st, 0, 0, 1)); }
HLE(u_GetWindowLongPtrW) { RET(window_long(st, 1, 0, 1)); }
HLE(u_SetWindowLongPtrA) { RET(window_long(st, 0, 1, 1)); }
HLE(u_SetWindowLongPtrW) { RET(window_long(st, 1, 1, 1)); }
HLE(u_GetWindowLongA) { RET((uint32_t)window_long(st, 0, 0, 0)); }
HLE(u_GetWindowLongW) { RET((uint32_t)window_long(st, 1, 0, 0)); }
HLE(u_SetWindowLongA) { RET((uint32_t)window_long(st, 0, 1, 0)); }
HLE(u_SetWindowLongW) { RET((uint32_t)window_long(st, 1, 1, 0)); }
HLE(u_GetWindowWord) { RET((uint16_t)window_long(st, 1, 0, 0)); }

static void prop_name(X86State *st, uint64_t p, int wide, char *out) {
    (void)st;
    if (p < 0x10000) snprintf(out, 64, "#%u", (unsigned)p);
    else { char *s = gstr(p, wide); snprintf(out, 64, "%s", s); free(s); }
}
static void set_prop(X86State *st, int wide) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    char n[64]; prop_name(st, ARG(1), wide, n);
    for (int i = 0; i < w->prop_count; i++) if (strcmp(w->props[i].name, n) == 0) { w->props[i].value = ARG(2); RET(1); return; }
    if (w->prop_count >= 8) { RET(0); return; }
    snprintf(w->props[w->prop_count].name, 64, "%s", n);
    w->props[w->prop_count++].value = ARG(2);
    RET(1);
}
static void get_prop(X86State *st, int wide, int remove) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    char n[64]; prop_name(st, ARG(1), wide, n);
    for (int i = 0; i < w->prop_count; i++) if (strcmp(w->props[i].name, n) == 0) {
        uint64_t v = w->props[i].value;
        if (remove) { w->props[i] = w->props[--w->prop_count]; }
        RET(v);
        return;
    }
    RET(0);
}
HLE(u_SetPropA) { set_prop(st, 0); }
HLE(u_SetPropW) { set_prop(st, 1); }
HLE(u_GetPropA) { get_prop(st, 0, 0); }
HLE(u_GetPropW) { get_prop(st, 1, 0); }
HLE(u_RemovePropA) { get_prop(st, 0, 1); }
HLE(u_RemovePropW) { get_prop(st, 1, 1); }

// ---- geometry -----------------------------------------------------------------------------------
static void wr_rect(uint64_t p, int a, int b, int c, int d) { int32_t r[4] = { a, b, c, d }; memcpy(G2H(p), r, 16); }
HLE(u_GetClientRect) { Win *w = win_get(ARG(0)); if (!w) { RET(0); return; } wr_rect(ARG(1), 0, 0, w->w, w->h); RET(1); }
HLE(u_GetWindowRect) {
    Win *w = win_get(ARG(0));
    if (ARG(0) == DESKTOP_HWND) { wr_rect(ARG(1), 0, 0, g_gui.screen_w, g_gui.screen_h); RET(1); return; }
    if (!w) { RET(0); return; }
    int ox, oy;
    desktop_origin(w, &ox, &oy);
    wr_rect(ARG(1), ox, oy, ox + w->w, oy + w->h);
    RET(1);
}
static void map_points(uint64_t from, uint64_t to, uint64_t pts, int n, int *dxo, int *dyo) {
    int fx = 0, fy = 0, tx = 0, ty = 0;
    Win *f = win_get(from), *t = win_get(to);
    if (f) desktop_origin(f, &fx, &fy);
    if (t) desktop_origin(t, &tx, &ty);
    int dx = fx - tx, dy = fy - ty;
    for (int i = 0; i < n; i++) {
        int32_t p[2]; memcpy(p, G2H(pts + 8u * (uint64_t)i), 8);
        p[0] += dx; p[1] += dy;
        memcpy(G2H(pts + 8u * (uint64_t)i), p, 8);
    }
    *dxo = dx; *dyo = dy;
}
HLE(u_ClientToScreen) { int dx, dy; if (!win_get(ARG(0))) { RET(0); return; } map_points(ARG(0), 0, ARG(1), 1, &dx, &dy); RET(1); }
HLE(u_ScreenToClient) { int dx, dy; if (!win_get(ARG(0))) { RET(0); return; } map_points(0, ARG(0), ARG(1), 1, &dx, &dy); RET(1); }
HLE(u_MapWindowPoints) { int dx, dy; map_points(ARG(0), ARG(1), ARG(2), ARGI(3), &dx, &dy); RET(((uint32_t)(uint16_t)dy << 16) | (uint16_t)dx); }
HLE(u_AdjustWindowRect) { RET(1); }   // Soul windows have no non-client area
HLE(u_GetWindowPlacement) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    uint8_t *wp = PTR(1);
    uint32_t len; memcpy(&len, wp, 4);
    memset(wp + 4, 0, 40);
    uint32_t show = w->maximized ? 3 : (w->visible ? 1 : 0);
    memcpy(wp + 8, &show, 4);
    int ox, oy; desktop_origin(w, &ox, &oy);
    int32_t r[4] = { ox, oy, ox + w->w, oy + w->h };
    memcpy(wp + 28, r, 16);
    RET(1);
}
HLE(u_SetWindowPlacement) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    const uint8_t *wp = PTR(1);
    int32_t r[4]; memcpy(r, wp + 28, 16);
    uint32_t show; memcpy(&show, wp + 8, 4);
    win_move(st, w, r[0], r[1], r[2] - r[0], r[3] - r[1], 0);
    if ((w = win_get(ARG(0)))) win_show(st, w, (int)show);
    RET(1);
}

static Win *hit_child(Win *w, int x, int y) { // x, y: client coordinates of w
    Win **list;
    int n = children_of(w->hwnd, &list);
    Win *r = w;
    for (int i = 0; i < n; i++) {
        Win *c = list[i];
        if (!c->visible || x < c->x || y < c->y || x >= c->x + c->w || y >= c->y + c->h) continue;
        if (c->ctl == CTL_STATIC && !(c->style & 0x100)) continue;                 // SS_NOTIFY
        if (c->ctl == CTL_BUTTON && (c->style & 0xF) == 7) continue;              // BS_GROUPBOX
        if (comctl_transparent_at(c, x - c->x, y - c->y)) continue;               // tab control pages
        r = hit_child(c, x - c->x, y - c->y);
        break;
    }
    free(list);
    return r;
}
static Win *top_at(int dx, int dy) {
    Win *best = NULL;
    for (int i = 0; i < WIN_MAX; i++) {
        Win *w = &g_gui.wins[i];
        if (!w->used || w->parent || !w->visible || w->message_only) continue;
        if (dx < w->x || dy < w->y || dx >= w->x + w->w || dy >= w->y + w->h) continue;
        if (!best || w->zstamp > best->zstamp) best = w;
    }
    return best;
}
HLE(u_WindowFromPoint) {
    int x = (int32_t)(ARG(0) & 0xFFFFFFFF), y = g_mode32 ? (int32_t)ARG(1) : (int32_t)(ARG(0) >> 32);
    Win *t = top_at(x, y);
    RET(t ? hit_child(t, x - t->x, y - t->y)->hwnd : 0);
}
HLE(u_ChildWindowFromPoint) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    int x = (int32_t)(ARG(1) & 0xFFFFFFFF), y = g_mode32 ? (int32_t)ARG(2) : (int32_t)(ARG(1) >> 32);
    if (x < 0 || y < 0 || x >= w->w || y >= w->h) { RET(0); return; }
    Win **list;
    int n = children_of(w->hwnd, &list);
    uint64_t r = w->hwnd;
    for (int i = 0; i < n; i++) if (x >= list[i]->x && y >= list[i]->y && x < list[i]->x + list[i]->w && y < list[i]->y + list[i]->h) { r = list[i]->hwnd; break; }
    free(list);
    RET(r);
}

// ---- painting -----------------------------------------------------------------------------------
HLE(u_GetDC) {
    if (!ARG(0) || ARG(0) == DESKTOP_HWND) { RET(gdi_create_dc(DC_SCREEN, 0)); return; }
    if (!win_get(ARG(0))) { RET(0); return; }
    RET(gdi_create_dc(DC_WINDOW, ARG(0)));
}
HLE(u_ReleaseDC) { gdi_release_dc(ARG(1)); RET(1); }
HLE(u_BeginPaint) {
    Win *w = win_get(ARG(0));
    const uint64_t psg = ARG(1); // read before WM_ERASEBKGND: guest calls clobber argument registers
    uint8_t *ps = G2H(psg);
    memset(ps, 0, g_mode32 ? 64 : 72);
    if (!w) { RET(0); return; }
    uint64_t hdc = gdi_create_dc(DC_WINDOW, w->hwnd);
    Dc *dc = dc_get(hdc);
    int x0 = 0, y0 = 0, x1 = w->w, y1 = w->h;
    if (w->invalid) { x0 = w->ix0; y0 = w->iy0; x1 = w->ix1; y1 = w->iy1; }
    dc->clip_set = 1; dc->cx0 = x0; dc->cy0 = y0; dc->cx1 = x1; dc->cy1 = y1;
    int erase = w->erase;
    w->invalid = 0;
    w->erase = 0;
    uint32_t ferase = 0;
    if (erase) ferase = win_send(st, w->hwnd, WM_ERASEBKGND, hdc, 0) ? 0 : 1;
    gs_set(psg, L_PAINTSTRUCT, 0, hdc);
    gs_set(psg, L_PAINTSTRUCT, 1, ferase);
    gs_set(psg, L_PAINTSTRUCT, 2, (uint32_t)x0); gs_set(psg, L_PAINTSTRUCT, 3, (uint32_t)y0);
    gs_set(psg, L_PAINTSTRUCT, 4, (uint32_t)x1); gs_set(psg, L_PAINTSTRUCT, 5, (uint32_t)y1);
    RET(hdc);
}
HLE(u_EndPaint) { gdi_release_dc(gs_get(ARG(1), L_PAINTSTRUCT, 0)); RET(1); }
static void invalidate(X86State *st, uint64_t hwnd, const int32_t *rc, int erase) {
    (void)st;
    if (!hwnd || hwnd == DESKTOP_HWND) {
        for (int i = 0; i < WIN_MAX; i++) if (g_gui.wins[i].used && !g_gui.wins[i].parent) win_invalidate_rect(&g_gui.wins[i], NULL, erase);
        return;
    }
    Win *w = win_get(hwnd);
    if (w) win_invalidate_rect(w, rc, erase);
}
HLE(u_InvalidateRect) { int32_t r[4]; if (ARG(1)) memcpy(r, PTR(1), 16); invalidate(st, ARG(0), ARG(1) ? r : NULL, ARG(2) != 0); RET(1); }
HLE(u_InvalidateRgn) {
    GdiObj *rg = gdi_get(ARG(1), GO_REGION);
    int32_t r[4];
    if (rg) { r[0] = rg->rx0; r[1] = rg->ry0; r[2] = rg->rx1; r[3] = rg->ry1; }
    invalidate(st, ARG(0), rg ? r : NULL, ARG(2) != 0);
    RET(1);
}
HLE(u_ValidateRect) { Win *w = win_get(ARG(0)); if (w) { w->invalid = 0; w->erase = 0; } RET(1); }
static void update_now(X86State *st, Win *w) {
    uint64_t h = w->hwnd;
    if (w->invalid && win_shown(w)) win_send(st, h, WM_PAINT, 0, 0);
    if (!win_get(h)) return;
    Win **list;
    int n = children_of(h, &list);
    for (int i = 0; i < n; i++) if (win_get(list[i]->hwnd) == list[i]) update_now(st, list[i]);
    free(list);
}
HLE(u_UpdateWindow) { Win *w = win_get(ARG(0)); if (w) update_now(st, w); RET(w != NULL); }
HLE(u_RedrawWindow) {
    Win *w = win_get(ARG(0));
    uint32_t flags = (uint32_t)ARG(3);
    if (!w) { if (!ARG(0)) invalidate(st, 0, NULL, 1); RET(1); return; }
    int32_t r[4];
    int have = 0;
    if (ARG(1)) { memcpy(r, PTR(1), 16); have = 1; }
    else if (ARG(2)) { GdiObj *rg = gdi_get(ARG(2), GO_REGION); if (rg) { r[0] = rg->rx0; r[1] = rg->ry0; r[2] = rg->rx1; r[3] = rg->ry1; have = 1; } }
    if (flags & 1) win_invalidate_rect(w, have ? r : NULL, (flags & 4) != 0);
    if (flags & 8) { w->invalid = 0; w->erase = 0; }
    if (flags & 0x300) update_now(st, w);
    RET(1);
}
HLE(u_GetUpdateRect) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    if (ARG(1)) { if (w->invalid) wr_rect(ARG(1), w->ix0, w->iy0, w->ix1, w->iy1); else wr_rect(ARG(1), 0, 0, 0, 0); }
    RET(w->invalid);
}
HLE(u_GetUpdateRgn) {
    Win *w = win_get(ARG(0));
    GdiObj *rg = gdi_get(ARG(1), GO_REGION);
    if (!w || !rg) { RET(0); return; }
    if (w->invalid) { rg->rx0 = w->ix0; rg->ry0 = w->iy0; rg->rx1 = w->ix1; rg->ry1 = w->iy1; RET(2); }
    else { rg->rx0 = rg->ry0 = rg->rx1 = rg->ry1 = 0; RET(1); }
}
HLE(u_ScrollWindow) { Win *w = win_get(ARG(0)); if (w) win_invalidate_rect(w, NULL, 1); RET(1); }
HLE(u_ScrollWindowEx) { Win *w = win_get(ARG(0)); if (w) win_invalidate_rect(w, NULL, 1); RET(2); }
HLE_STUB_RET(u_ScrollDC, 1)
HLE_STUB_RET(u_LockWindowUpdate, 1)
HLE(u_FillRect) {
    int32_t r[4]; memcpy(r, PTR(1), 16);
    uint64_t b = ARG(2);
    uint32_t color;
    if (b > 0 && b < 0x40) color = gui_sys_color((int)b - 1);
    else { GdiObj *o = gdi_get(b, GO_BRUSH); if (!o || o->style == 1) { RET(1); return; } color = o->color; }
    gdi_fill_rect_px(ARG(0), r[0], r[1], r[2], r[3], color);
    RET(1);
}
HLE(u_FrameRect) {
    int32_t r[4]; memcpy(r, PTR(1), 16);
    uint64_t b = ARG(2);
    uint32_t color = b > 0 && b < 0x40 ? gui_sys_color((int)b - 1) : (gdi_get(b, GO_BRUSH) ? gdi_get(b, GO_BRUSH)->color : 0);
    gdi_frame_rect_px(ARG(0), r[0], r[1], r[2], r[3], color);
    RET(1);
}
HLE(u_InvertRect) {
    Dc *dc = dc_get(ARG(0));
    int32_t r[4]; memcpy(r, PTR(1), 16);
    if (!dc) { RET(0); return; }
    int old = dc->rop2;
    uint64_t ob = dc->brush, op = dc->pen;
    dc->rop2 = 6; // R2_NOT
    uint64_t b = gdi_make_brush(0);
    dc->brush = b;
    GdiObj *np = NULL; uint64_t pn = gdi_alloc(GO_PEN, &np); np->style = 5; dc->pen = pn;
    gdi_rect_dc(ARG(0), r[0], r[1], r[2], r[3]);
    dc->rop2 = old; dc->brush = ob; dc->pen = op;
    gdi_free(b); gdi_free(pn);
    RET(1);
}
HLE(u_DrawFocusRect) {
    int32_t r[4]; memcpy(r, PTR(1), 16);
    for (int x = r[0]; x < r[2]; x += 2) { gdi_fill_rect_px(ARG(0), x, r[1], x + 1, r[1] + 1, 0x404040); gdi_fill_rect_px(ARG(0), x, r[3] - 1, x + 1, r[3], 0x404040); }
    for (int y = r[1]; y < r[3]; y += 2) { gdi_fill_rect_px(ARG(0), r[0], y, r[0] + 1, y + 1, 0x404040); gdi_fill_rect_px(ARG(0), r[2] - 1, y, r[2], y + 1, 0x404040); }
    RET(1);
}
HLE(u_DrawEdge) {
    uint64_t hdc = ARG(0);
    int32_t *rp = PTR(1), r[4];
    memcpy(r, rp, 16);
    uint32_t edge = (uint32_t)ARG(2), flags = (uint32_t)ARG(3);
    uint32_t light = 0xFFFFFF, mid = 0xE3E3E3, shadow = 0xA0A0A0, dark = 0x696969;
    uint32_t tl_out = (edge & 1) ? mid : (edge & 2) ? shadow : 0xFFFFFFFF, br_out = (edge & 1) ? dark : (edge & 2) ? light : 0xFFFFFFFF;
    uint32_t tl_in = (edge & 4) ? light : (edge & 8) ? dark : 0xFFFFFFFF, br_in = (edge & 4) ? shadow : (edge & 8) ? mid : 0xFFFFFFFF;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t tl = pass ? tl_in : tl_out, br = pass ? br_in : br_out;
        if (tl == 0xFFFFFFFF) continue;
        if (flags & 4) gdi_fill_rect_px(hdc, r[0], r[1], r[2], r[1] + 1, tl);       // BF_TOP
        if (flags & 1) gdi_fill_rect_px(hdc, r[0], r[1], r[0] + 1, r[3], tl);       // BF_LEFT
        if (flags & 8) gdi_fill_rect_px(hdc, r[0], r[3] - 1, r[2], r[3], br);       // BF_BOTTOM
        if (flags & 2) gdi_fill_rect_px(hdc, r[2] - 1, r[1], r[2], r[3], br);       // BF_RIGHT
        r[0]++; r[1]++; r[2]--; r[3]--;
    }
    if (flags & 0x800) gdi_fill_rect_px(hdc, r[0], r[1], r[2], r[3], 0xF0F0F0);    // BF_MIDDLE
    if (flags & 0x2000) memcpy(rp, r, 16);                                          // BF_ADJUST
    RET(1);
}
HLE(u_DrawFrameControl) {
    int32_t r[4]; memcpy(r, PTR(1), 16);
    uint32_t state = (uint32_t)ARG(3);
    gdi_fill_rect_px(ARG(0), r[0], r[1], r[2], r[3], (state & 0x200) ? 0xCCE4F7 : 0xE1E1E1);
    gdi_frame_rect_px(ARG(0), r[0], r[1], r[2], r[3], 0xADADAD);
    RET(1);
}
static void draw_text(X86State *st, int wide, int ex) {
    int n = ARGI(2);
    char *s;
    if (wide) { size_t cap = (n < 0 ? wstrlen(WSTR(1)) : (size_t)n) * 3 + 4; s = malloc(cap); w2u(WSTR(1), n, s, cap); }
    else { size_t len = n < 0 ? strlen(CSTR(1)) : (size_t)n; s = malloc(len + 1); memcpy(s, CSTR(1), len); s[len] = 0; }
    int32_t *rc = PTR(3);
    int h = gdi_draw_text_dc(ARG(0), s, -1, rc, (uint32_t)hle_arg(st, 4));
    (void)ex;
    free(s);
    RET(h ? h : 1);
}
HLE(u_DrawTextA) { draw_text(st, 0, 0); }
HLE(u_DrawTextW) { draw_text(st, 1, 0); }
HLE(u_DrawTextExA) { draw_text(st, 0, 1); }
HLE(u_DrawTextExW) { draw_text(st, 1, 1); }
static void tabbed_text_out_impl(X86State *st, int wide) {
    // (hdc, x, y, str, count, tabs, tabarray, origin)
    int n = (int)(int32_t)hle_arg(st, 4);
    uint64_t sp = ARG(3);
    char *s;
    if (wide) { size_t cap = (size_t)(n > 0 ? n : 0) * 3 + 4; s = malloc(cap); w2u((const uint16_t *)G2H(sp), n, s, cap); }
    else { s = malloc((size_t)(n > 0 ? n : 0) + 1); if (n > 0) memcpy(s, G2H(sp), (size_t)n); s[n > 0 ? n : 0] = 0; }
    gdi_text_out_utf8(ARG(0), ARGI(1), ARGI(2), s, -1);
    Dc *dc = dc_get(ARG(0));
    int h = 0, w = gdi_text_width(dc ? dc->font : gdi_default_font(), s, (int)strlen(s), &h);
    free(s);
    RET(((uint32_t)h << 16) | (uint16_t)w);
}
HLE(u_TabbedTextOutA) { tabbed_text_out_impl(st, 0); }
HLE(u_TabbedTextOutW) { tabbed_text_out_impl(st, 1); }
HLE(u_GetSysColor) { RET(gui_sys_color(ARGI(0))); }
HLE(u_GetSysColorBrush) { int i = ARGI(0); RET(i >= 0 && i < 31 ? g_gui.sys_brush[i] : 0); }
HLE_STUB_RET(u_SetSysColors, 1)
HLE_STUB_RET(u_DrawIcon, 1)
HLE_STUB_RET(u_DrawIconEx, 1)
HLE_STUB_RET(u_DrawStateA, 1)

// ---- DefWindowProc ------------------------------------------------------------------------------

uint64_t win_def_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    if (!w) return 0;
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_NCCALCSIZE: return 0;
    case WM_NCHITTEST: return 1; // HTCLIENT
    case WM_MOUSEACTIVATE: return 1;
    case WM_PAINT: {
        uint8_t *ps = calloc(1, 72);
        uint64_t args_hwnd = w->hwnd;
        // BeginPaint/EndPaint without a guest round trip
        Win *ww = win_get(args_hwnd);
        if (ww) {
            uint64_t hdc = gdi_create_dc(DC_WINDOW, ww->hwnd);
            int erase = ww->erase;
            Dc *dc = dc_get(hdc);
            if (ww->invalid) { dc->clip_set = 1; dc->cx0 = ww->ix0; dc->cy0 = ww->iy0; dc->cx1 = ww->ix1; dc->cy1 = ww->iy1; }
            ww->invalid = 0; ww->erase = 0;
            if (erase) win_send(st, args_hwnd, WM_ERASEBKGND, hdc, 0);
            gdi_release_dc(hdc);
        }
        free(ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        uint64_t b = w->cls ? w->cls->brush : 0;
        if (!b) return 0;
        uint32_t color;
        if (b < 0x40) color = gui_sys_color((int)b - 1);
        else { GdiObj *o = gdi_get(b, GO_BRUSH); if (!o || o->style == 1) return 0; color = o->color; }
        gdi_fill_rect_px(wp, 0, 0, w->w, w->h, color);
        return 1;
    }
    case WM_CLOSE: win_destroy(st, w); return 0;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0) {
        case 0xF060: win_send(st, w->hwnd, WM_CLOSE, 0, 0); break;
        case 0xF030: win_show(st, w, 3); break;
        case 0xF120: win_show(st, w, 9); break;
        default: break;
        }
        return 0;
    case WM_SETTEXT: {
        char *s = lp ? gstr(lp, unicode) : strdup("");
        win_set_text(w, s ? s : "");
        free(s);
        return 1;
    }
    case WM_GETTEXT: {
        if (!wp || !lp) return 0;
        return put_str(lp, (size_t)wp, w->text, unicode);
    }
    case WM_GETTEXTLENGTH: return str_units(w->text, unicode);
    case WM_SETFONT: w->font = wp; if (lp) win_invalidate_rect(w, NULL, 1); return 0;
    case WM_GETFONT: return w->font;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: case WM_CTLCOLORDLG: case WM_CTLCOLORMSGBOX: case WM_CTLCOLORSCROLLBAR: {
        Dc *dc = dc_get(wp);
        if (dc) { dc->text_color = gui_sys_color(COLOR_WINDOWTEXT); dc->bk_color = gui_sys_color(COLOR_BTNFACE); }
        return g_gui.sys_brush[COLOR_BTNFACE];
    }
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        Dc *dc = dc_get(wp);
        if (dc) { dc->text_color = gui_sys_color(COLOR_WINDOWTEXT); dc->bk_color = gui_sys_color(COLOR_WINDOW); }
        return g_gui.sys_brush[COLOR_WINDOW];
    }
    case WM_MOUSEWHEEL:
        if (w->parent) return win_send(st, w->parent, msg, wp, lp);
        if (w->style & WS_VSCROLL) {
            int delta = (int16_t)(wp >> 16);
            int steps = delta / 40;
            for (int i = 0; i < (steps < 0 ? -steps : steps) && i < 10; i++) win_send(st, w->hwnd, WM_VSCROLL, steps > 0 ? 0 : 1, 0);
        }
        return 0;
    case 0x007B: // WM_CONTEXTMENU
        if (w->parent) return win_send(st, w->parent, msg, wp, lp);
        return 0;
    case WM_ACTIVATE:
        if ((wp & 0xFFFF) && !w->parent) {
            if (!g_gui.focus || !(g_gui.focus == w->hwnd || is_child_of(w->hwnd, win_get(g_gui.focus)))) win_set_focus(st, w->hwnd);
        }
        return 0;
    case WM_WINDOWPOSCHANGED: {
        uint32_t flags = lp ? (uint32_t)gs_get(lp, L_WINDOWPOS, 6) : 0;
        if (!(flags & 2)) win_send(st, w->hwnd, WM_MOVE, 0, ((uint64_t)(uint16_t)w->y << 16) | (uint16_t)w->x);
        if (!(flags & 1)) win_send(st, w->hwnd, WM_SIZE, w->maximized ? 2 : 0, ((uint64_t)(uint16_t)w->h << 16) | (uint16_t)w->w);
        return 0;
    }
    case WM_GETMINMAXINFO: case WM_WINDOWPOSCHANGING: case WM_SHOWWINDOW: case WM_SETCURSOR: return 0;
    case WM_NCACTIVATE: return 1;
    case 0x0011: return 1; // WM_QUERYENDSESSION
    case WM_GETICON: return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: return 0;
    case WM_SYSKEYDOWN:
        if (wp == 0x73 && (g_gui.keys[VK_MENU] & 0x80)) win_send(st, win_top(w)->hwnd, WM_SYSCOMMAND, 0xF060, 0); // Alt+F4
        return 0;
    default: return 0;
    }
}
HLE(u_DefWindowProcA) { RET(win_def_proc(st, win_get(ARG(0)), (uint32_t)ARG(1), ARG(2), ARG(3), 0)); }
HLE(u_DefWindowProcW) { RET(win_def_proc(st, win_get(ARG(0)), (uint32_t)ARG(1), ARG(2), ARG(3), 1)); }
static void call_window_proc(X86State *st, int wide) {
    Win *w = win_get(ARG(1));
    uint64_t proc = ARG(0);
    if (!w) { if (proc) { uint64_t a[4] = { ARG(1), ARG(2), ARG(3), hle_arg(st, 4) }; RET(soul_guest_call(st, proc, 4, a)); } else RET(0); return; }
    int b = builtin_of(proc);
    if (b || !proc) { RET(call_proc(st, w, proc, (uint32_t)ARG(2), ARG(3), hle_arg(st, 4), wide)); return; }
    uint64_t a[4] = { ARG(1), ARG(2), ARG(3), hle_arg(st, 4) };
    RET(soul_guest_call(st, proc, 4, a));
}
HLE(u_CallWindowProcA) { call_window_proc(st, 0); }
HLE(u_CallWindowProcW) { call_window_proc(st, 1); }
static void send_message(X86State *st, int wide) {
    uint64_t h = ARG(0);
    if (h == HWND_BROADCAST) {
        for (int i = 0; i < WIN_MAX; i++) if (g_gui.wins[i].used && !g_gui.wins[i].parent) win_send_ex(st, g_gui.wins[i].hwnd, (uint32_t)ARG(1), ARG(2), ARG(3), wide);
        RET(0);
        return;
    }
    RET(win_send_ex(st, h, (uint32_t)ARG(1), ARG(2), ARG(3), wide));
}
HLE(u_SendMessageA) { send_message(st, 0); }
HLE(u_SendMessageW) { send_message(st, 1); }
HLE(u_SendMessageTimeoutA) { uint64_t r = win_send_ex(st, ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3), 0); if (hle_arg(st, 6)) wr_ptr(hle_arg(st, 6), r); RET(1); }
HLE(u_SendMessageTimeoutW) { uint64_t r = win_send_ex(st, ARG(0), (uint32_t)ARG(1), ARG(2), ARG(3), 1); if (hle_arg(st, 6)) wr_ptr(hle_arg(st, 6), r); RET(1); }
HLE(u_PostMessage) {
    uint64_t h = ARG(0);
    if (h == HWND_BROADCAST) {
        for (int i = 0; i < WIN_MAX; i++) if (g_gui.wins[i].used && !g_gui.wins[i].parent) win_post(g_gui.wins[i].hwnd, (uint32_t)ARG(1), ARG(2), ARG(3));
        RET(1);
        return;
    }
    if (h && !win_get(h)) { set_last_error(st, 1400); RET(0); return; }
    if (h) win_post(h, (uint32_t)ARG(1), ARG(2), ARG(3));
    else queue_add(0, (uint32_t)ARG(1), ARG(2), ARG(3), cur_tid(), 0);
    RET(1);
}
HLE(u_PostThreadMessage) { queue_add(0, (uint32_t)ARG(1), ARG(2), ARG(3), (uint32_t)ARG(0), 0); RET(1); }
HLE(u_PostQuitMessage) { g_gui.quit_posted = 1; g_gui.quit_code = ARGI(0); g_gui.quit_tid = cur_tid(); gil_notify(); RET(0); }
HLE(u_InSendMessage) { RET(0); }
HLE(u_ReplyMessage) { RET(0); }
static uint32_t g_wm_next = 0xC100;
static struct { char name[64]; uint32_t id; } g_wm_names[128];
static int g_wm_count;
static void register_window_message(X86State *st, int wide) {
    char *s = gstr(ARG(0), wide);
    if (!s) { RET(0); return; }
    for (int i = 0; i < g_wm_count; i++) if (strcasecmp(g_wm_names[i].name, s) == 0) { free(s); RET(g_wm_names[i].id); return; }
    uint32_t id = g_wm_next++;
    if (g_wm_count < 128) { snprintf(g_wm_names[g_wm_count].name, 64, "%s", s); g_wm_names[g_wm_count++].id = id; }
    free(s);
    RET(id);
}
HLE(u_RegisterWindowMessageA) { register_window_message(st, 0); }
HLE(u_RegisterWindowMessageW) { register_window_message(st, 1); }

// ---- input from the UI ----------------------------------------------------------------------------

static uint32_t mk_flags(void) {
    GuiState *g = &g_gui;
    return (g->keys[VK_LBUTTON] & 0x80 ? 1 : 0) | (g->keys[VK_RBUTTON] & 0x80 ? 2 : 0) | (g->keys[VK_SHIFT] & 0x80 ? 4 : 0) |
           (g->keys[VK_CONTROL] & 0x80 ? 8 : 0) | (g->keys[4] & 0x80 ? 0x10 : 0);
}
static Win *modal_blocked(Win *top) { // returns the enabled owned popup if top is disabled by a modal dialog
    if (!top || top->enabled) return NULL;
    for (int i = 0; i < WIN_MAX; i++) { Win *o = &g_gui.wins[i]; if (o->used && !o->parent && o->owner == top->hwnd && o->visible) return o; }
    return NULL;
}
static void post_to(Win *w, uint32_t msg, uint64_t wp, uint64_t lp) { queue_add(w->hwnd, msg, wp, lp, w->tid, 0); }

static void set_mod(int mods) {
    GuiState *g = &g_gui;
    g->keys[VK_SHIFT] = (mods & 1) ? 0x80 : 0;
    g->keys[VK_CONTROL] = (mods & 2) ? 0x80 : 0;
    g->keys[VK_MENU] = (mods & 4) ? 0x80 : 0;
    g->keys[0xA0] = g->keys[VK_SHIFT]; g->keys[0xA2] = g->keys[VK_CONTROL]; g->keys[0xA4] = g->keys[VK_MENU];
}
static Win *key_target(uint64_t hwnd) {
    Win *f = win_get(g_gui.focus);
    if (f && win_shown(f)) return f;
    Win *a = win_get(g_gui.active);
    if (a) return a;
    return win_get(hwnd);
}
static uint32_t key_char(int vk) { // character a key-down produces with the current modifier state
    GuiState *g = &g_gui;
    int shift = g->keys[VK_SHIFT] & 0x80, ctrl = g->keys[VK_CONTROL] & 0x80;
    if (vk >= 'A' && vk <= 'Z') return ctrl ? (uint32_t)(vk - 'A' + 1) : (shift ? (uint32_t)vk : (uint32_t)(vk + 32));
    if (ctrl) return 0;
    if (vk >= '0' && vk <= '9' && !shift) return (uint32_t)vk;
    if (vk >= 0x60 && vk <= 0x69) return (uint32_t)('0' + vk - 0x60);
    switch (vk) {
    case VK_SPACE: return ' ';
    case VK_RETURN: return '\r';
    case VK_BACK: return 8;
    case VK_TAB: return '\t';
    case VK_ESCAPE: return 27;
    default: return 0;
    }
}
static void post_key(Win *t, int vk, int down, uint32_t ch) {
    GuiState *g = &g_gui;
    if (down && !ch) ch = key_char(vk);
    int alt = (g->keys[VK_MENU] & 0x80) && !(g->keys[VK_CONTROL] & 0x80);
    uint32_t msg = down ? (alt ? WM_SYSKEYDOWN : WM_KEYDOWN) : (alt ? WM_SYSKEYUP : WM_KEYUP);
    uint32_t prev = g->keys[vk] & 0x80;
    if (down) g->keys[vk] = (uint8_t)(0x80 | ((g->keys[vk] & 1) ^ 1));
    else g->keys[vk] &= 1;
    static const uint8_t scan[256] = { ['A'] = 0x1E, ['B'] = 0x30, ['C'] = 0x2E, ['D'] = 0x20, ['E'] = 0x12, ['F'] = 0x21, ['G'] = 0x22, ['H'] = 0x23,
        ['I'] = 0x17, ['J'] = 0x24, ['K'] = 0x25, ['L'] = 0x26, ['M'] = 0x32, ['N'] = 0x31, ['O'] = 0x18, ['P'] = 0x19, ['Q'] = 0x10, ['R'] = 0x13,
        ['S'] = 0x1F, ['T'] = 0x14, ['U'] = 0x16, ['V'] = 0x2F, ['W'] = 0x11, ['X'] = 0x2D, ['Y'] = 0x15, ['Z'] = 0x2C, [VK_RETURN] = 0x1C,
        [VK_ESCAPE] = 0x01, [VK_SPACE] = 0x39, [VK_BACK] = 0x0E, [VK_TAB] = 0x0F, [VK_LEFT] = 0x4B, [VK_RIGHT] = 0x4D, [VK_UP] = 0x48, [VK_DOWN] = 0x50 };
    uint32_t lp = 1u | ((uint32_t)scan[vk & 255] << 16) | (alt ? (1u << 29) : 0) | (down ? (prev ? (1u << 30) : 0) : 0xC0000000u);
    if (vk >= VK_PRIOR && vk <= VK_DOWN) lp |= 1u << 24; // extended key
    QMsg *q;
    queue_add(t->hwnd, msg, (uint64_t)vk, lp, t->tid, 0);
    q = &g->queue[g->qlen - 1];
    q->x = (int)ch; // carried into MSG.lPrivate for TranslateMessage
}
static int vk_for_char(uint32_t c, int *shift) {
    *shift = 0;
    if (c >= 'a' && c <= 'z') return (int)(c - 32);
    if (c >= 'A' && c <= 'Z') { *shift = 1; return (int)c; }
    if (c >= '0' && c <= '9') return (int)c;
    if (c == ' ') return VK_SPACE;
    if (c == '\r' || c == '\n') return VK_RETURN;
    if (c == '\t') return VK_TAB;
    if (c == 8) return VK_BACK;
    if (c == 27) return VK_ESCAPE;
    return 0xE7; // VK_PACKET
}

static void process_input(X86State *st, GuiInput *ev) {
    GuiState *g = &g_gui;
    Win *top = win_get(ev->hwnd);
    switch (ev->type) {
    case 1: case 2: case 3: case 4: { // mouse down / up / move / wheel
        if (!top) return;
        top = win_top(top);
        if (modal_blocked(top)) return;
        int dx = top->x + ev->x, dy = top->y + ev->y;
        g->mouse_x = dx; g->mouse_y = dy;
        Win *t = win_get(g->capture);
        if (!t || win_top(t) != top) t = hit_child(top, ev->x, ev->y);
        if (!t) return;
        if (getenv("SOUL_DEBUG_INPUT") && ev->type != 3) soul_log("[input] %d @%d,%d -> %llx cls=%s id=%d en=%d cap=%llx\n", ev->type, ev->x, ev->y, (unsigned long long)t->hwnd, t->cls ? t->cls->name : "?", (int)t->id, win_enabled_chain(t), (unsigned long long)g->capture);
        if (!win_enabled_chain(t)) {
            if (ev->type == 1 && top->hwnd != g->active) set_active(st, top->hwnd);
            return;
        }
        int ox, oy;
        desktop_origin(t, &ox, &oy);
        uint64_t lp = ((uint64_t)(uint16_t)(dy - oy) << 16) | (uint16_t)(dx - ox);
        int button = ev->data;
        if (ev->type == 4) {
            Win *f = t;
            post_to(f, (ev->data & 0x10000) ? WM_MOUSEHWHEEL : WM_MOUSEWHEEL,
                    ((uint64_t)(uint16_t)(int16_t)(ev->data & 0xFFFF) << 16) | mk_flags(), ((uint64_t)(uint16_t)dy << 16) | (uint16_t)dx);
            return;
        }
        if (ev->type == 3) {
            // coalesce consecutive moves
            if (g->qlen && g->queue[g->qlen - 1].msg == WM_MOUSEMOVE && g->queue[g->qlen - 1].hwnd == t->hwnd) {
                g->queue[g->qlen - 1].lp = lp;
                g->queue[g->qlen - 1].x = dx; g->queue[g->qlen - 1].y = dy;
                return;
            }
            post_to(t, WM_MOUSEMOVE, mk_flags(), lp);
            return;
        }
        int vk = button == 1 ? VK_RBUTTON : button == 2 ? 4 : VK_LBUTTON;
        uint32_t base = button == 1 ? WM_RBUTTONDOWN : button == 2 ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
        if (ev->type == 1) {
            if (top->hwnd != g->active) set_active(st, top->hwnd);
            post_to(t, WM_MOUSEMOVE, mk_flags(), lp);
            g->keys[vk] = 0x80;
            double now = soul_now();
            uint32_t msg = base;
            if (t->cls && (t->cls->style & 8) && g->last_click_hwnd == t->hwnd && g->last_click_button == button &&
                now - g->last_click_time < 0.5 && abs(dx - g->last_click_x) < 8 && abs(dy - g->last_click_y) < 8) {
                msg = base + 2; // WM_xBUTTONDBLCLK
                g->last_click_time = 0;
            } else {
                g->last_click_time = now; g->last_click_hwnd = t->hwnd; g->last_click_button = button;
                g->last_click_x = dx; g->last_click_y = dy;
            }
            post_to(t, msg, mk_flags(), lp);
        } else {
            g->keys[vk] = 0;
            post_to(t, base + 1, mk_flags(), lp);
            if (button == 1) post_to(t, 0x007B /* WM_CONTEXTMENU */, t->hwnd, ((uint64_t)(uint16_t)dy << 16) | (uint16_t)dx);
        }
        return;
    }
    case 5: { // key: data = vk, x = down, y = modifiers, ch = character
        set_mod(ev->y);
        Win *t = key_target(ev->hwnd);
        if (!t || modal_blocked(win_top(t))) return;
        post_key(t, ev->data & 0xFF, ev->x != 0, ev->ch);
        return;
    }
    case 6: { // text typed on the on-screen keyboard (no modifier keys are held)
        set_mod(0);
        Win *t = key_target(ev->hwnd);
        if (!t || !ev->text || modal_blocked(win_top(t))) return;
        const char *p = ev->text;
        while (*p) {
            uint16_t w16[2];
            size_t n = u2w(p, (long)(((*p & 0xF8) == 0xF0) ? 4 : ((*p & 0xF0) == 0xE0) ? 3 : ((*p & 0xE0) == 0xC0) ? 2 : 1), w16, 2);
            p += ((*p & 0xF8) == 0xF0) ? 4 : ((*p & 0xF0) == 0xE0) ? 3 : ((*p & 0xE0) == 0xC0) ? 2 : 1;
            uint32_t c = n ? w16[0] : '?';
            if (c == '\n') c = '\r';
            int shift;
            int vk = vk_for_char(c, &shift);
            uint8_t saved = g->keys[VK_SHIFT];
            if (shift) g->keys[VK_SHIFT] = 0x80;
            post_key(t, vk, 1, c);
            post_key(t, vk, 0, 0);
            g->keys[VK_SHIFT] = saved;
        }
        return;
    }
    case 7: // close button
        if (top && !modal_blocked(top) && top->enabled) post_to(top, WM_SYSCOMMAND, 0xF060, 0);
        return;
    case 8: // menu command
        if (top && top->enabled) post_to(top, WM_COMMAND, (uint16_t)ev->data, 0);
        return;
    case 9: // resize from the UI
        if (top && !top->parent && ev->x > 0 && ev->y > 0) win_move(st, top, top->x, top->y, ev->x, ev->y, 0);
        return;
    case 10: { // popup answered
        if (g->popup_active && g->popup_kind == 2) {
            Win *c = win_get(g->popup_hwnd);
            g->popup_active = 0;
            bump();
            if (c && ev->data > 0) combo_select_from_popup(st, c, ev->data - 1);
        } else {
            g->popup_done = 1;
            g->popup_result = (uint32_t)ev->data;
        }
        return;
    }
    case 11: // a menu of the menu bar is about to be shown
        if (top && top->menu) {
            win_send(st, top->hwnd, WM_INITMENU, top->menu, 0);
            Menu *m = menu_get(top->menu);
            for (int i = 0; m && i < m->count; i++) if (m->items[i].submenu) win_send(st, top->hwnd, WM_INITMENUPOPUP, m->items[i].submenu, (uint64_t)i);
            bump();
        }
        return;
    case 12: // activate
        if (top && !top->parent) { top = modal_blocked(top) ? modal_blocked(top) : top; top->zstamp = ++g->zstamp; set_active(st, top->hwnd); bump(); }
        return;
    default: return;
    }
}

static void pump_input(X86State *st) {
    GuiState *g = &g_gui;
    if (!g->input_len) return;
    gui_lock();
    GuiInput *list = g->input;
    int n = g->input_len;
    g->input = NULL; g->input_len = g->input_cap = 0;
    gui_unlock();
    for (int i = 0; i < n; i++) { process_input(st, &list[i]); free(list[i].text); }
    free(list);
}

// ---- message retrieval ---------------------------------------------------------------------------

static int msg_matches(uint64_t filter, uint32_t mn, uint32_t mx, uint64_t hwnd, uint32_t msg) {
    if (filter == (uint64_t)-1) { if (hwnd) return 0; }
    else if (filter) { if (hwnd != filter && !is_child_of(filter, win_get(hwnd))) return 0; }
    if (mn || mx) { if (msg < mn || msg > mx) return 0; }
    return 1;
}
#define KEYCHAR_RING 16
static struct { uint32_t time, vk, ch; } g_keychars[KEYCHAR_RING];
static int g_keychar_pos;
static void write_msg(uint64_t p, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, uint32_t time, int x, int y, uint32_t priv) {
    memset(G2H(p), 0, gs_size(L_MSG));
    gs_set(p, L_MSG, 0, hwnd); gs_set(p, L_MSG, 1, msg); gs_set(p, L_MSG, 2, wp); gs_set(p, L_MSG, 3, lp);
    gs_set(p, L_MSG, 4, time); gs_set(p, L_MSG, 5, (uint32_t)x); gs_set(p, L_MSG, 6, (uint32_t)y);
    if (!g_mode32) mem_wr(p + 44, 4, priv);
    else if (priv && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        g_keychars[g_keychar_pos].time = time; g_keychars[g_keychar_pos].vk = (uint32_t)wp; g_keychars[g_keychar_pos].ch = priv;
        g_keychar_pos = (g_keychar_pos + 1) % KEYCHAR_RING;
    }
}
static uint32_t msg_private(uint64_t m, uint32_t vk) {
    if (!g_mode32) return (uint32_t)mem_rd(m + 44, 4);
    uint32_t time = (uint32_t)gs_get(m, L_MSG, 4);
    // newest first: GetMessage records the key just before the program translates it
    for (int k = 1; k <= KEYCHAR_RING; k++) {
        int i = (g_keychar_pos - k + KEYCHAR_RING) % KEYCHAR_RING;
        if (g_keychars[i].ch && g_keychars[i].time == time && g_keychars[i].vk == vk) { uint32_t c = g_keychars[i].ch; g_keychars[i].ch = 0; return c; }
    }
    return 0;
}

static Win *find_paint(uint32_t tid, uint64_t filter) {
    for (int i = 0; i < WIN_MAX; i++) {
        Win *w = &g_gui.wins[i];
        if (!w->used || !w->invalid || w->tid != tid || !win_shown(w)) continue;
        int ancestor_invalid = 0;
        for (Win *a = win_get(w->parent); a; a = win_get(a->parent)) if (a->invalid) { ancestor_invalid = 1; break; }
        if (ancestor_invalid) continue;
        if (filter && filter != (uint64_t)-1 && w->hwnd != filter && !is_child_of(filter, w)) continue;
        return w;
    }
    return NULL;
}

// Returns 1 for a message, 2 for WM_QUIT, 0 for none.
static int peek(X86State *st, uint64_t out, uint64_t filter, uint32_t mn, uint32_t mx, uint32_t flags) {
    GuiState *g = &g_gui;
    uint32_t tid = cur_tid();
    int remove = flags & 1;
    (void)st;
    for (int i = 0; i < g->qlen; i++) {
        QMsg *q = &g->queue[i];
        if (q->tid != tid || !msg_matches(filter, mn, mx, q->hwnd, q->msg)) continue;
        uint32_t priv = (q->msg == WM_KEYDOWN || q->msg == WM_SYSKEYDOWN) ? (uint32_t)q->x : 0;
        int px = (q->msg == WM_KEYDOWN || q->msg == WM_SYSKEYDOWN || q->msg == WM_KEYUP || q->msg == WM_SYSKEYUP) ? g->mouse_x : q->x;
        int py = (q->msg == WM_KEYDOWN || q->msg == WM_SYSKEYDOWN || q->msg == WM_KEYUP || q->msg == WM_SYSKEYUP) ? g->mouse_y : q->y;
        write_msg(out, q->hwnd, q->msg, q->wp, q->lp, q->time, px, py, priv);
        g->msg_time = q->time; g->msg_x = px; g->msg_y = py;
        if (remove) { memmove(q, q + 1, sizeof(QMsg) * (size_t)(g->qlen - i - 1)); g->qlen--; }
        return 1;
    }
    if (g->quit_posted && g->quit_tid == tid && (filter == 0 || filter == (uint64_t)-1) && (!mn && !mx ? 1 : (WM_QUIT >= mn && WM_QUIT <= mx))) {
        write_msg(out, 0, WM_QUIT, (uint64_t)(int64_t)g->quit_code, 0, tick_ms(), g->mouse_x, g->mouse_y, 0);
        if (remove) g->quit_posted = 0;
        return 2;
    }
    if (!(mn || mx) || (WM_PAINT >= mn && WM_PAINT <= mx)) {
        Win *w = find_paint(tid, filter);
        if (w) { write_msg(out, w->hwnd, WM_PAINT, 0, 0, tick_ms(), g->mouse_x, g->mouse_y, 0); return 1; }
    }
    if (!(mn || mx) || (WM_TIMER >= mn && WM_TIMER <= mx)) {
        double now = soul_now();
        GuiTimer *best = NULL;
        for (int i = 0; i < 256; i++) {
            GuiTimer *t = &g->timers[i];
            if (!t->used || t->tid != tid || t->due > now) continue;
            if (filter && filter != (uint64_t)-1 && t->hwnd != filter && !is_child_of(filter, win_get(t->hwnd))) continue;
            if (filter == (uint64_t)-1 && t->hwnd) continue;
            if (!best || t->due < best->due) best = t;
        }
        if (best) {
            write_msg(out, best->hwnd, WM_TIMER, best->id, best->proc, tick_ms(), g->mouse_x, g->mouse_y, 0);
            if (remove) { best->due += best->interval / 1000.0; if (best->due < now) best->due = now + best->interval / 1000.0; }
            return 1;
        }
    }
    return 0;
}
static int64_t next_timer_ms(void) {
    uint32_t tid = cur_tid();
    double now = soul_now(), best = -1;
    for (int i = 0; i < 256; i++) {
        GuiTimer *t = &g_gui.timers[i];
        if (!t->used || t->tid != tid) continue;
        double d = t->due - now;
        if (best < 0 || d < best) best = d;
    }
    if (best < 0) return -1;
    return (int64_t)(best * 1000.0) + 1;
}
static int has_messages(uint32_t tid) {
    for (int i = 0; i < g_gui.qlen; i++) if (g_gui.queue[i].tid == tid) return 1;
    return g_gui.input_len > 0 || (g_gui.quit_posted && g_gui.quit_tid == tid) || find_paint(tid, 0) != NULL;
}
// Blocks until something may have arrived. Returns 1 if the process is ending.
static int wait_for_message(int64_t max_ms) {
    if (g_proc->terminate) return 1;
    int64_t t = next_timer_ms();
    if (t < 0 || (max_ms >= 0 && t > max_ms)) t = max_ms;
    if (t == 0) return 0;
    gil_wait(t < 0 ? 1000 : t);
    return g_proc->terminate;
}
static int get_message(X86State *st, uint64_t out, uint64_t filter, uint32_t mn, uint32_t mx) {
    for (;;) {
        pump_input(st);
        int r = peek(st, out, filter, mn, mx, 1);
        if (r) return r == 2 ? 0 : 1;
        if (wait_for_message(-1)) { write_msg(out, 0, WM_QUIT, 0, 0, tick_ms(), 0, 0, 0); return 0; }
    }
}
HLE(u_GetMessageA) { RET(get_message(st, ARG(0), ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3))); }
HLE(u_GetMessageW) { RET(get_message(st, ARG(0), ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3))); }
HLE(u_PeekMessage) {
    pump_input(st);
    int r = peek(st, ARG(0), ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3), (uint32_t)hle_arg(st, 4));
    if (!r) soul_yield();
    RET(r != 0);
}
HLE(u_WaitMessage) {
    pump_input(st);
    if (!has_messages(cur_tid())) wait_for_message(-1);
    RET(1);
}
HLE(u_MsgWaitForMultipleObjects) {
    // (count, handles, waitAll, ms, wakeMask) / Ex: (count, handles, ms, wakeMask, flags)
    int ex = strcmp(g_proc->thunks[st->thread->cur_thunk].name, "MsgWaitForMultipleObjectsEx") == 0;
    uint32_t n = (uint32_t)ARG(0);
    uint32_t ms = (uint32_t)(ex ? ARG(2) : ARG(3));
    int all = ex ? (int)((hle_arg(st, 4) & 1)) : ARG(2) != 0;
    uint64_t hs_[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs_[i] = rd_ptr(ARG(1) + (uint64_t)PSZ * i);
    double deadline = ms == 0xFFFFFFFF ? -1 : soul_now() + ms / 1000.0;
    for (;;) {
        pump_input(st);
        if (n) { uint32_t r = k32_wait_objects(st, n, hs_, all, 0); if (r != 0x102) { RET(r); return; } }
        if (has_messages(cur_tid())) { RET(n); return; }
        if (next_timer_ms() == 0 || (next_timer_ms() > 0 && next_timer_ms() <= 1)) { RET(n); return; }
        double now = soul_now();
        if (deadline >= 0 && now >= deadline) { RET(0x102); return; }
        int64_t wait = deadline < 0 ? 50 : (int64_t)((deadline - now) * 1000) + 1;
        if (wait > 50) wait = 50;
        if (wait_for_message(wait)) { RET(0xFFFFFFFF); return; }
    }
}
static int translate_message(uint64_t m) {
    uint64_t hwnd = gs_get(m, L_MSG, 0), wp = gs_get(m, L_MSG, 2), lp = gs_get(m, L_MSG, 3);
    uint32_t msg = (uint32_t)gs_get(m, L_MSG, 1);
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN) return 0;
    uint32_t priv = msg_private(m, (uint32_t)wp);
    uint32_t ch = priv ? priv : key_char((int)wp); // keys from the UI carry their character
    if (!ch) return 0;
    if (ch == '\n') ch = '\r';
    Win *w = win_get(hwnd);
    queue_add(hwnd, msg == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR, ch, lp, w ? w->tid : cur_tid(), 1);
    return 1;
}
HLE(u_TranslateMessage) { RET(translate_message(ARG(0))); }
static uint64_t dispatch_message_at(X86State *st, uint64_t m, int wide) {
    uint64_t hwnd = gs_get(m, L_MSG, 0), wp = gs_get(m, L_MSG, 2), lp = gs_get(m, L_MSG, 3);
    uint32_t msg = (uint32_t)gs_get(m, L_MSG, 1);
    if (msg == WM_TIMER && lp) {
        uint64_t a[4] = { hwnd, WM_TIMER, wp, tick_ms() };
        return soul_guest_call(st, lp, 4, a);
    }
    if (!hwnd) return 0;
    return win_send_ex(st, hwnd, msg, wp, lp, wide);
}
static void dispatch_message(X86State *st, int wide) { RET(dispatch_message_at(st, ARG(0), wide)); }
// helpers for modal loops (dialogs); msg is a guest-memory MSG buffer
int gui_get_message(X86State *st, uint8_t *msg) { return get_message(st, H2G(msg), 0, 0, 0); }
void gui_translate_dispatch(X86State *st, uint8_t *msg) {
    translate_message(H2G(msg));
    dispatch_message_at(st, H2G(msg), 1);
}
void gui_repost_quit(int code) { g_gui.quit_posted = 1; g_gui.quit_code = code; g_gui.quit_tid = cur_tid(); }
HLE(u_DispatchMessageA) { dispatch_message(st, 0); }
HLE(u_DispatchMessageW) { dispatch_message(st, 1); }
HLE(u_GetMessageTime) { RET(g_gui.msg_time); }
HLE(u_GetMessagePos) { RET(((uint32_t)(uint16_t)g_gui.msg_y << 16) | (uint16_t)g_gui.msg_x); }
HLE(u_GetMessageExtraInfo) { RET(0); }
HLE(u_GetQueueStatus) { pump_input(st); RET(has_messages(cur_tid()) ? 0x00FF00FFu : 0); }
HLE(u_GetInputState) { pump_input(st); RET(has_messages(cur_tid())); }

// ---- timers -------------------------------------------------------------------------------------
HLE(u_SetTimer) {
    GuiState *g = &g_gui;
    uint64_t hwnd = ARG(0), id = ARG(1);
    uint32_t ms = (uint32_t)ARG(2);
    if (ms < 10) ms = 10;
    if (hwnd && !win_get(hwnd)) { RET(0); return; }
    GuiTimer *t = NULL;
    if (hwnd) for (int i = 0; i < 256; i++) if (g->timers[i].used && g->timers[i].hwnd == hwnd && g->timers[i].id == id) t = &g->timers[i];
    if (!hwnd && id) for (int i = 0; i < 256; i++) if (g->timers[i].used && !g->timers[i].hwnd && g->timers[i].id == id) t = &g->timers[i];
    if (!t) for (int i = 0; i < 256 && !t; i++) if (!g->timers[i].used) t = &g->timers[i];
    if (!t) { RET(0); return; }
    if (!hwnd && !id) { static uint64_t next = 0x7F00; id = next++; }
    t->used = 1; t->hwnd = hwnd; t->id = id; t->proc = ARG(3); t->interval = ms;
    t->due = soul_now() + ms / 1000.0;
    Win *w = win_get(hwnd);
    t->tid = w ? w->tid : cur_tid();
    gil_notify();
    RET(hwnd ? (id ? id : 1) : id);
}
HLE(u_KillTimer) {
    for (int i = 0; i < 256; i++) {
        GuiTimer *t = &g_gui.timers[i];
        if (t->used && t->hwnd == ARG(0) && t->id == ARG(1)) {
            t->used = 0;
            // drop WM_TIMER already waiting (none are queued: timers are generated on demand)
            RET(1);
            return;
        }
    }
    RET(0);
}

// ---- keyboard / mouse state ------------------------------------------------------------------------
HLE(u_GetKeyState) { uint8_t k = g_gui.keys[ARG(0) & 0xFF]; RET((uint64_t)(int64_t)(int16_t)(((k & 0x80) ? 0x8000 : 0) | (k & 1))); }
HLE(u_GetAsyncKeyState) { uint8_t k = g_gui.keys[ARG(0) & 0xFF]; RET((uint64_t)(int64_t)(int16_t)((k & 0x80) ? 0x8000 : 0)); }
HLE(u_GetKeyboardState) { memcpy(PTR(0), g_gui.keys, 256); RET(1); }
HLE(u_SetKeyboardState) { memcpy(g_gui.keys, PTR(0), 256); RET(1); }
HLE(u_MapVirtualKey) {
    uint32_t code = (uint32_t)ARG(0), type = (uint32_t)ARG(1);
    if (type == 2) { RET(code >= 'A' && code <= 'Z' ? code : (code >= '0' && code <= '9' ? code : (code == VK_SPACE ? ' ' : 0))); return; }
    RET(code & 0xFF);
}
HLE(u_VkKeyScanA) { int shift; int vk = vk_for_char((uint32_t)(ARG(0) & 0xFF), &shift); RET(vk == 0xE7 ? (uint64_t)-1 : (uint64_t)(vk | (shift << 8))); }
HLE(u_VkKeyScanW) { int shift; int vk = vk_for_char((uint32_t)(ARG(0) & 0xFFFF), &shift); RET(vk == 0xE7 ? (uint64_t)-1 : (uint64_t)(vk | (shift << 8))); }
static uint32_t char_for_vk(int vk, const uint8_t *keys) {
    int shift = keys && (keys[VK_SHIFT] & 0x80);
    if (vk >= 'A' && vk <= 'Z') return shift ? (uint32_t)vk : (uint32_t)(vk + 32);
    if (vk >= '0' && vk <= '9') return (uint32_t)vk;
    if (vk == VK_SPACE) return ' ';
    if (vk == VK_RETURN) return '\r';
    if (vk == VK_BACK) return 8;
    if (vk == VK_TAB) return '\t';
    return 0;
}
HLE(u_ToAscii) { uint32_t c = char_for_vk((int)ARG(0), PTR(2)); if (c && ARG(3)) mem_wr(ARG(3), 2, c); RET(c ? 1 : 0); }
HLE(u_ToUnicode) { uint32_t c = char_for_vk((int)ARG(0), PTR(2)); if (c && ARG(3) && ARGI(4) > 0) mem_wr(ARG(3), 2, c); RET(c ? 1 : 0); }
HLE(u_GetKeyNameTextW) { RET(put_str(ARG(1), (size_t)ARGI(2), "Key", 1)); }
HLE(u_GetKeyNameTextA) { RET(put_str(ARG(1), (size_t)ARGI(2), "Key", 0)); }
HLE(u_GetKeyboardLayout) { RET(0x041F041Full); }
HLE_STUB_RET(u_GetKeyboardType, 4)
HLE(u_GetCursorPos) { if (ARG(0)) { int32_t p[2] = { g_gui.mouse_x, g_gui.mouse_y }; memcpy(PTR(0), p, 8); } RET(1); }
HLE(u_SetCursorPos) { g_gui.mouse_x = ARGI(0); g_gui.mouse_y = ARGI(1); RET(1); }
HLE(u_GetPhysicalCursorPos) { u_GetCursorPos(st); }
HLE_STUB_RET(u_ClipCursor, 1)
HLE(u_GetClipCursor) { wr_rect(ARG(0), 0, 0, g_gui.screen_w, g_gui.screen_h); RET(1); }
static int g_cursor_count;
static uint64_t g_cursor = ICON_HANDLE_BASE + 0x100;
HLE(u_ShowCursor) { g_cursor_count += ARG(0) ? 1 : -1; RET((uint32_t)g_cursor_count); }
HLE(u_SetCursor) { uint64_t o = g_cursor; g_cursor = ARG(0); RET(o); }
HLE(u_GetCursor) { RET(g_cursor); }
HLE(u_LoadCursor) { RET(ICON_HANDLE_BASE + 0x100 + (ARG(1) & 0xFF)); }
HLE(u_LoadIcon) { RET(ICON_HANDLE_BASE + 0x200 + (ARG(1) & 0xFF)); }
HLE(u_CreateIconIndirect) { RET(ICON_HANDLE_BASE + 0x300); }
HLE_STUB_RET(u_DestroyIcon, 1)
HLE_STUB_RET(u_DestroyCursor, 1)
HLE(u_CopyIcon) { RET(ARG(0)); }
HLE(u_GetIconInfo) { memset(PTR(1), 0, g_mode32 ? 20 : 32); RET(1); }
HLE_STUB_RET(u_TrackMouseEvent, 1)
HLE(u_GetDoubleClickTime) { RET(500); }
HLE_STUB_RET(u_SwapMouseButton, 0)
HLE_STUB_RET(u_SendInput, 0)
HLE_STUB_RET(u_keybd_event, 0)
HLE_STUB_RET(u_mouse_event, 0)
HLE_STUB_RET(u_RegisterHotKey, 1)
HLE_STUB_RET(u_UnregisterHotKey, 1)
HLE_STUB_RET(u_RegisterRawInputDevices, 1)
HLE_STUB_RET(u_GetRawInputData, 0)
HLE_STUB_RET(u_GetRawInputDeviceList, 0)
HLE(u_SetWindowsHookEx) { RET(++g_gui.hook_id + 0x8000); }
HLE_STUB_RET(u_UnhookWindowsHookEx, 1)
HLE_STUB_RET(u_CallNextHookEx, 0)
HLE_STUB_RET(u_FlashWindow, 1)
HLE_STUB_RET(u_FlashWindowEx, 1)

// caret (drawn by the UI)
void gui_caret_set(uint64_t hwnd, int x, int y, int w, int h, int visible) {
    gui_lock();
    g_gui.caret_hwnd = hwnd; g_gui.caret_x = x; g_gui.caret_y = y; g_gui.caret_w = w; g_gui.caret_h = h; g_gui.caret_visible = visible;
    gui_unlock();
}
HLE(u_CreateCaret) { int w = ARGI(2), h = ARGI(3); gui_caret_set(ARG(0), 0, 0, w > 0 ? w : 2, h > 0 ? h : 16, 0); RET(1); }
HLE(u_DestroyCaret) { gui_caret_set(0, 0, 0, 0, 0, 0); RET(1); }
HLE(u_ShowCaret) { gui_lock(); if (g_gui.caret_hwnd) g_gui.caret_visible = 1; gui_unlock(); RET(1); }
HLE(u_HideCaret) { gui_lock(); g_gui.caret_visible = 0; gui_unlock(); RET(1); }
HLE(u_SetCaretPos) { gui_lock(); g_gui.caret_x = ARGI(0); g_gui.caret_y = ARGI(1); gui_unlock(); RET(1); }
HLE(u_GetCaretPos) { int32_t p[2] = { g_gui.caret_x, g_gui.caret_y }; memcpy(PTR(0), p, 8); RET(1); }
HLE(u_GetCaretBlinkTime) { RET(530); }
HLE_STUB_RET(u_SetCaretBlinkTime, 1)

// ---- scroll bars (state only; the UI turns two-finger pans into wheel messages) ---------------------
static int sb_index(int bar) { return bar == 1 ? 1 : 0; } // SB_VERT = 1
HLE(u_SetScrollInfo) {
    Win *w = win_get(ARG(0));
    const uint8_t *si = PTR(2);
    if (!w || ARGI(1) > 1) { RET(0); return; }
    int b = sb_index(ARGI(1));
    uint32_t mask; memcpy(&mask, si + 4, 4);
    int32_t v[5]; memcpy(v, si + 8, 20); // min, max, page, pos, trackpos
    if (mask & 1) { w->sb_min[b] = v[0]; w->sb_max[b] = v[1]; }
    if (mask & 2) w->sb_page[b] = v[2];
    if (mask & 4) w->sb_pos[b] = v[3];
    int maxpos = w->sb_max[b] - (w->sb_page[b] > 0 ? w->sb_page[b] - 1 : 0);
    if (w->sb_pos[b] > maxpos) w->sb_pos[b] = maxpos;
    if (w->sb_pos[b] < w->sb_min[b]) w->sb_pos[b] = w->sb_min[b];
    RET(w->sb_pos[b]);
}
HLE(u_GetScrollInfo) {
    Win *w = win_get(ARG(0));
    uint8_t *si = PTR(2);
    if (!w || ARGI(1) > 1) { RET(0); return; }
    int b = sb_index(ARGI(1));
    int32_t v[5] = { w->sb_min[b], w->sb_max[b], w->sb_page[b], w->sb_pos[b], w->sb_pos[b] };
    memcpy(si + 8, v, 20);
    RET(1);
}
HLE(u_SetScrollPos) { Win *w = win_get(ARG(0)); if (!w || ARGI(1) > 1) { RET(0); return; } int b = sb_index(ARGI(1)); int o = w->sb_pos[b]; w->sb_pos[b] = ARGI(2); RET(o); }
HLE(u_GetScrollPos) { Win *w = win_get(ARG(0)); RET(w && ARGI(1) <= 1 ? w->sb_pos[sb_index(ARGI(1))] : 0); }
HLE(u_SetScrollRange) { Win *w = win_get(ARG(0)); if (w && ARGI(1) <= 1) { int b = sb_index(ARGI(1)); w->sb_min[b] = ARGI(2); w->sb_max[b] = ARGI(3); } RET(1); }
HLE(u_GetScrollRange) { Win *w = win_get(ARG(0)); int b = sb_index(ARGI(1)); mem_wr(ARG(2), 4, w ? (uint32_t)w->sb_min[b] : 0); mem_wr(ARG(3), 4, w ? (uint32_t)w->sb_max[b] : 0); RET(1); }
HLE_STUB_RET(u_ShowScrollBar, 1)
HLE_STUB_RET(u_EnableScrollBar, 1)

// ---- menus ----------------------------------------------------------------------------------------
Menu *menu_get(uint64_t h) {
    if (h < MENU_HANDLE_BASE) return NULL;
    uint64_t i = (h - MENU_HANDLE_BASE) / 4;
    if (i >= MENU_MAX || (h - MENU_HANDLE_BASE) % 4) return NULL;
    return g_gui.menus[i].used ? &g_gui.menus[i] : NULL;
}
static uint64_t menu_new(void) {
    gui_lock();
    for (int i = 1; i < MENU_MAX; i++) {
        if (!g_gui.menus[i].used) {
            memset(&g_gui.menus[i], 0, sizeof g_gui.menus[i]);
            g_gui.menus[i].used = 1;
            gui_unlock();
            return MENU_HANDLE_BASE + (uint64_t)i * 4;
        }
    }
    gui_unlock();
    return 0;
}
static void menu_insert(Menu *m, int pos, uint32_t flags, uint64_t id_or_sub, const char *text) {
    gui_lock();
    if (m->count == m->cap) { m->cap = m->cap ? m->cap * 2 : 8; m->items = realloc(m->items, sizeof(MenuItem) * (size_t)m->cap); }
    if (pos < 0 || pos > m->count) pos = m->count;
    memmove(m->items + pos + 1, m->items + pos, sizeof(MenuItem) * (size_t)(m->count - pos));
    MenuItem *it = &m->items[pos];
    memset(it, 0, sizeof *it);
    it->flags = flags & (0x1 | 0x2 | 0x8 | 0x10 | 0x200 | 0x800 | 0x1000 | 0x20 | 0x40 | 0x4000);
    if (flags & 0x10) it->submenu = id_or_sub; else it->id = (uint32_t)id_or_sub;
    if (text && !(flags & 0x800)) snprintf(it->text, sizeof it->text, "%s", text);
    m->count++;
    gui_unlock();
    bump();
}
static MenuItem *menu_find(uint64_t hmenu, uint32_t item, int by_pos, Menu **owner) {
    Menu *m = menu_get(hmenu);
    if (!m) return NULL;
    if (by_pos) { if (item < (uint32_t)m->count) { if (owner) *owner = m; return &m->items[item]; } return NULL; }
    for (int i = 0; i < m->count; i++) {
        if (!(m->items[i].flags & 0x10) && m->items[i].id == item) { if (owner) *owner = m; return &m->items[i]; }
        if (m->items[i].submenu) { MenuItem *r = menu_find(m->items[i].submenu, item, 0, owner); if (r) return r; }
    }
    return NULL;
}
static void menu_destroy(uint64_t h) {
    Menu *m = menu_get(h);
    if (!m) return;
    for (int i = 0; i < m->count; i++) if (m->items[i].submenu) menu_destroy(m->items[i].submenu);
    gui_lock();
    free(m->items);
    m->items = NULL; m->count = m->cap = 0; m->used = 0;
    gui_unlock();
}
HLE(u_CreateMenu) { RET(menu_new()); }
HLE(u_CreatePopupMenu) { RET(menu_new()); }
HLE(u_DestroyMenu) { menu_destroy(ARG(0)); bump(); RET(1); }
static void append_menu(X86State *st, int wide, int insert) {
    // Append: (hmenu, flags, id, text)  Insert: (hmenu, pos, flags, id, text)
    Menu *m = menu_get(ARG(0));
    if (!m) { RET(0); return; }
    uint32_t flags = (uint32_t)ARG(insert ? 2 : 1);
    uint64_t id = ARG(insert ? 3 : 2), tp = hle_arg(st, insert ? 4 : 3);
    char *text = (flags & (0x4 | 0x100 | 0x800)) ? NULL : gstr(tp, wide);
    int pos = -1;
    if (insert) {
        uint32_t where = (uint32_t)ARG(1);
        if (flags & 0x400) pos = where == 0xFFFFFFFF ? -1 : (int)where;
        else { for (int i = 0; i < m->count; i++) if (m->items[i].id == where && !(m->items[i].flags & 0x10)) pos = i; }
    }
    menu_insert(m, pos, flags, id, text);
    free(text);
    RET(1);
}
HLE(u_AppendMenuA) { append_menu(st, 0, 0); }
HLE(u_AppendMenuW) { append_menu(st, 1, 0); }
HLE(u_InsertMenuA) { append_menu(st, 0, 1); }
HLE(u_InsertMenuW) { append_menu(st, 1, 1); }
static void modify_menu(X86State *st, int wide) {
    // (hmenu, item, flags, id, text)
    uint32_t flags = (uint32_t)ARG(2);
    MenuItem *it = menu_find(ARG(0), (uint32_t)ARG(1), (flags & 0x400) != 0, NULL);
    if (!it) { RET(0); return; }
    char *text = (flags & (0x4 | 0x100 | 0x800)) ? NULL : gstr(hle_arg(st, 4), wide);
    gui_lock();
    it->flags = flags & (0x1 | 0x2 | 0x8 | 0x10 | 0x200 | 0x800 | 0x1000);
    if (flags & 0x10) it->submenu = ARG(3); else { it->id = (uint32_t)ARG(3); it->submenu = 0; }
    snprintf(it->text, sizeof it->text, "%s", text ? text : "");
    gui_unlock();
    free(text);
    bump();
    RET(1);
}
HLE(u_ModifyMenuA) { modify_menu(st, 0); }
HLE(u_ModifyMenuW) { modify_menu(st, 1); }
static void remove_menu(X86State *st, int destroy) {
    Menu *m = NULL;
    MenuItem *it = menu_find(ARG(0), (uint32_t)ARG(1), (ARG(2) & 0x400) != 0, &m);
    if (!it) { RET(0); return; }
    if (destroy && it->submenu) menu_destroy(it->submenu);
    gui_lock();
    int i = (int)(it - m->items);
    memmove(m->items + i, m->items + i + 1, sizeof(MenuItem) * (size_t)(m->count - i - 1));
    m->count--;
    gui_unlock();
    bump();
    RET(1);
}
HLE(u_DeleteMenu) { remove_menu(st, 1); }
HLE(u_RemoveMenu) { remove_menu(st, 0); }
HLE(u_SetMenu) {
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    gui_lock(); w->menu = ARG(1); gui_unlock();
    bump();
    RET(1);
}
HLE(u_GetMenu) { Win *w = win_get(ARG(0)); RET(w ? w->menu : 0); }
HLE(u_GetSubMenu) { Menu *m = menu_get(ARG(0)); int p = ARGI(1); RET(m && p >= 0 && p < m->count ? m->items[p].submenu : 0); }
HLE(u_GetMenuItemCount) { Menu *m = menu_get(ARG(0)); RET(m ? (uint64_t)m->count : (uint64_t)-1); }
HLE(u_GetMenuItemID) { Menu *m = menu_get(ARG(0)); int p = ARGI(1); RET(m && p >= 0 && p < m->count ? (m->items[p].submenu ? 0xFFFFFFFFu : m->items[p].id) : 0xFFFFFFFFu); }
HLE(u_DrawMenuBar) { bump(); RET(1); }
HLE(u_CheckMenuItem) {
    MenuItem *it = menu_find(ARG(0), (uint32_t)ARG(1), (ARG(2) & 0x400) != 0, NULL);
    if (!it) { RET(0xFFFFFFFF); return; }
    uint32_t old = it->flags & 8;
    gui_lock();
    if (ARG(2) & 8) it->flags |= 8; else it->flags &= ~8u;
    gui_unlock();
    bump();
    RET(old);
}
HLE(u_EnableMenuItem) {
    MenuItem *it = menu_find(ARG(0), (uint32_t)ARG(1), (ARG(2) & 0x400) != 0, NULL);
    if (!it) { RET(0xFFFFFFFF); return; }
    uint32_t old = it->flags & 3;
    gui_lock();
    it->flags = (it->flags & ~3u) | ((uint32_t)ARG(2) & 3);
    gui_unlock();
    bump();
    RET(old);
}
HLE(u_CheckMenuRadioItem) {
    // (hmenu, first, last, check, flags)
    int by_pos = (hle_arg(st, 4) & 0x400) != 0;
    uint32_t first = (uint32_t)ARG(1), last = (uint32_t)ARG(2), check = (uint32_t)ARG(3);
    Menu *m = NULL;
    MenuItem *f = menu_find(ARG(0), first, by_pos, &m);
    if (!f || !m) { RET(0); return; }
    gui_lock();
    for (int i = 0; i < m->count; i++) {
        MenuItem *it = &m->items[i];
        uint32_t key = by_pos ? (uint32_t)i : it->id;
        if (key < first || key > last) continue;
        if (key == check) it->flags |= 8 | 0x200; else it->flags &= ~8u;
    }
    gui_unlock();
    bump();
    RET(1);
}
HLE(u_GetMenuState) {
    MenuItem *it = menu_find(ARG(0), (uint32_t)ARG(1), (ARG(2) & 0x400) != 0, NULL);
    if (!it) { RET(0xFFFFFFFF); return; }
    uint32_t r = it->flags & (0x1 | 0x2 | 0x8 | 0x800 | 0x1000);
    if (it->submenu) { Menu *s = menu_get(it->submenu); r |= 0x10 | ((uint32_t)(s ? s->count : 0) << 8); }
    RET(r);
}
static void get_menu_string(X86State *st, int wide) {
    MenuItem *it = menu_find(ARG(0), (uint32_t)ARG(1), (hle_arg(st, 4) & 0x400) != 0, NULL);
    if (!it) { RET(0); return; }
    if (!ARG(2)) { RET(str_units(it->text, wide)); return; }
    RET(put_str(ARG(2), (size_t)ARGI(3), it->text, wide));
}
HLE(u_GetMenuStringA) { get_menu_string(st, 0); }
HLE(u_GetMenuStringW) { get_menu_string(st, 1); }
static void menu_item_info(X86State *st, int wide, int mode) { // mode 0 get, 1 set, 2 insert
    // (hmenu, item, byPosition, MENUITEMINFO*)
    const uint64_t mi = ARG(3);
    const char *L = L_MENUITEMINFO;
    uint32_t mask = (uint32_t)gs_get(mi, L, 1), type = (uint32_t)gs_get(mi, L, 2), state = (uint32_t)gs_get(mi, L, 3), id = (uint32_t)gs_get(mi, L, 4);
    uint64_t sub = gs_get(mi, L, 5), data = gs_get(mi, L, 8), tp = gs_get(mi, L, 9);
    uint32_t cch = (uint32_t)gs_get(mi, L, 10);
    int by_pos = ARG(2) != 0;
    if (mode == 2) {
        Menu *m = menu_get(ARG(0));
        if (!m) { RET(0); return; }
        int pos = -1;
        if (by_pos) pos = (int)(uint32_t)ARG(1) == -1 ? -1 : (int)ARG(1);
        else for (int i = 0; i < m->count; i++) if (m->items[i].id == (uint32_t)ARG(1)) pos = i;
        uint32_t flags = ((mask & 0x100) || (mask & 0x10) ? (type & (0x800 | 0x200)) : 0) | ((mask & 1) ? (state & (3 | 8 | 0x1000)) : 0);
        if ((mask & 4) && sub) flags |= 0x10;
        char *text = ((mask & 0x40) || ((mask & 0x10) && !(type & 0x800))) ? gstr(tp, wide) : NULL;
        menu_insert(m, pos, flags, (flags & 0x10) ? sub : ((mask & 2) ? id : 0), text);
        if (flags & 0x10) { MenuItem *it = &m->items[pos < 0 ? m->count - 1 : pos]; it->id = (mask & 2) ? id : 0; }
        if (mask & 0x20) m->items[pos < 0 ? m->count - 1 : pos].data = data;
        free(text);
        RET(1);
        return;
    }
    MenuItem *it = menu_find(ARG(0), (uint32_t)ARG(1), by_pos, NULL);
    if (!it) { RET(0); return; }
    if (mode == 1) {
        gui_lock();
        if (mask & 1) it->flags = (it->flags & ~(3u | 8u | 0x1000u)) | (state & (3 | 8 | 0x1000));
        if (mask & 2) it->id = id;
        if (mask & 4) { it->submenu = sub; if (sub) it->flags |= 0x10; else it->flags &= ~0x10u; }
        if ((mask & 0x100) || (mask & 0x10)) it->flags = (it->flags & ~(0x800u | 0x200u)) | (type & (0x800 | 0x200));
        if (mask & 0x20) it->data = data;
        if ((mask & 0x40) || ((mask & 0x10) && !(type & 0x800))) { char *t = gstr(tp, wide); snprintf(it->text, sizeof it->text, "%s", t ? t : ""); free(t); }
        gui_unlock();
        bump();
        RET(1);
        return;
    }
    if (mask & 1) gs_set(mi, L, 3, it->flags & (3 | 8 | 0x1000));
    if (mask & 2) gs_set(mi, L, 4, it->id);
    if (mask & 4) gs_set(mi, L, 5, it->submenu);
    if ((mask & 0x100) || (mask & 0x10)) gs_set(mi, L, 2, it->flags & (0x800 | 0x200));
    if (mask & 0x20) gs_set(mi, L, 8, it->data);
    if ((mask & 0x40) || (mask & 0x10)) {
        uint32_t n = (uint32_t)str_units(it->text, wide);
        if (tp && cch) put_str(tp, cch, it->text, wide);
        gs_set(mi, L, 10, n);
    }
    RET(1);
}
HLE(u_GetMenuItemInfoA) { menu_item_info(st, 0, 0); }
HLE(u_GetMenuItemInfoW) { menu_item_info(st, 1, 0); }
HLE(u_SetMenuItemInfoA) { menu_item_info(st, 0, 1); }
HLE(u_SetMenuItemInfoW) { menu_item_info(st, 1, 1); }
HLE(u_InsertMenuItemA) { menu_item_info(st, 0, 2); }
HLE(u_InsertMenuItemW) { menu_item_info(st, 1, 2); }
HLE_STUB_RET(u_SetMenuDefaultItem, 1)
HLE_STUB_RET(u_SetMenuInfo, 1)
HLE_STUB_RET(u_GetMenuInfo, 1)
HLE_STUB_RET(u_SetMenuItemBitmaps, 1)
HLE(u_GetSystemMenu) { static uint64_t sys; if (!sys || !menu_get(sys)) sys = menu_new(); RET(sys); }
HLE(u_IsMenu) { RET(menu_get(ARG(0)) != NULL); }

// RT_MENU resources (MENUHEADER / MENUEX_TEMPLATE_HEADER)
static const uint16_t *parse_menu_items(uint64_t hmenu, const uint16_t *p) {
    Menu *m = menu_get(hmenu);
    for (;;) {
        uint16_t flags = *p++;
        uint16_t id = 0;
        if (!(flags & 0x10)) id = *p++;
        const uint16_t *text = p;
        while (*p) p++;
        p++;
        char t[128];
        w2u(text, -1, t, sizeof t);
        if (flags & 0x10) {
            uint64_t sub = menu_new();
            p = parse_menu_items(sub, p);
            menu_insert(m, -1, (flags & (0x1 | 0x2 | 0x8)) | 0x10, sub, t);
        } else if (!id && !t[0]) {
            menu_insert(m, -1, 0x800, 0, NULL);
        } else {
            menu_insert(m, -1, flags & (0x1 | 0x2 | 0x8 | 0x800), id, t);
        }
        if (flags & 0x80) break;
    }
    return p;
}
static const uint8_t *parse_menuex_items(uint64_t hmenu, const uint8_t *p) {
    Menu *m = menu_get(hmenu);
    for (;;) {
        uint32_t type, state, id; uint16_t res;
        memcpy(&type, p, 4); memcpy(&state, p + 4, 4); memcpy(&id, p + 8, 4); memcpy(&res, p + 12, 2);
        p += 14;
        const uint16_t *text = (const uint16_t *)(const void *)p;
        size_t n = wstrlen(text);
        char t[128];
        w2u(text, -1, t, sizeof t);
        p += (n + 1) * 2;
        p = (const uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
        uint32_t flags = (type & (0x800 | 0x200)) | (state & (3 | 8 | 0x1000));
        if (res & 1) {
            p += 4; // dwHelpId
            uint64_t sub = menu_new();
            p = parse_menuex_items(sub, p);
            menu_insert(m, -1, flags | 0x10, sub, t);
            m->items[m->count - 1].id = id;
        } else {
            menu_insert(m, -1, flags, id, t);
        }
        if (res & 0x80) break;
    }
    return p;
}
uint64_t menu_from_resource(const uint8_t *res) {
    uint16_t ver, off;
    memcpy(&ver, res, 2); memcpy(&off, res + 2, 2);
    uint64_t h = menu_new();
    if (ver == 1) parse_menuex_items(h, res + 4 + off);
    else parse_menu_items(h, (const uint16_t *)(const void *)(res + 4 + off));
    return h;
}
HLE(u_LoadMenuA) { const void *r = pe_resource(ARG(0), 4, ARG(1), 0, NULL); RET(r ? menu_from_resource(r) : 0); }
HLE(u_LoadMenuW) { const void *r = pe_resource(ARG(0), 4, ARG(1), 1, NULL); RET(r ? menu_from_resource(r) : 0); }
HLE(u_LoadMenuIndirect) { RET(menu_from_resource(PTR(0))); }

static uint32_t track_popup(X86State *st, uint64_t hmenu, uint32_t flags, int x, int y, uint64_t hwnd) {
    GuiState *g = &g_gui;
    if (!menu_get(hmenu) || !g_proc->opts.gui_enabled) return 0;
    win_send(st, hwnd, WM_INITMENUPOPUP, hmenu, 0);
    gui_lock();
    g->popup_active = 1; g->popup_kind = 1; g->popup_done = 0; g->popup_result = 0;
    g->popup_hwnd = hwnd; g->popup_menu = hmenu; g->popup_x = x; g->popup_y = y;
    gui_unlock();
    bump();
    while (!g->popup_done && !g_proc->terminate) {
        pump_input(st);
        if (g->popup_done) break;
        gil_wait(50);
    }
    gui_lock();
    g->popup_active = 0;
    gui_unlock();
    bump();
    uint32_t r = g->popup_result;
    if (r && !(flags & 0x100) && win_get(hwnd)) win_post(hwnd, WM_COMMAND, r, 0); // !TPM_RETURNCMD
    return (flags & 0x100) ? r : (r != 0);
}
HLE(u_TrackPopupMenu) { RET(track_popup(st, ARG(0), (uint32_t)ARG(1), ARGI(2), ARGI(3), hle_arg(st, 5))); }
HLE(u_TrackPopupMenuEx) { RET(track_popup(st, ARG(0), (uint32_t)ARG(1), ARGI(2), ARGI(3), hle_arg(st, 4))); }
HLE_STUB_RET(u_EndMenu, 1)

// ---- accelerators -----------------------------------------------------------------------------------
static uint64_t accel_new(void) {
    for (int i = 0; i < 32; i++) if (!g_gui.accel[i].used) { memset(&g_gui.accel[i], 0, sizeof g_gui.accel[i]); g_gui.accel[i].used = 1; return ACCEL_HANDLE_BASE + (uint64_t)i; }
    return 0;
}
static GuiAccel *accel_get(uint64_t h) {
    if (h < ACCEL_HANDLE_BASE || h >= ACCEL_HANDLE_BASE + 32) return NULL;
    GuiAccel *a = &g_gui.accel[h - ACCEL_HANDLE_BASE];
    return a->used ? a : NULL;
}
static void load_accel(X86State *st, int wide) {
    const uint8_t *r = pe_resource(ARG(0), 9, ARG(1), wide, NULL);
    if (!r) { RET(0); return; }
    uint64_t h = accel_new();
    GuiAccel *a = accel_get(h);
    if (!a) { RET(0); return; }
    for (int i = 0; i < 256; i++) {
        uint16_t e[4]; memcpy(e, r + 8 * i, 8);
        a->fvirt[i] = (uint8_t)e[0]; a->key[i] = e[1]; a->cmd[i] = e[2];
        a->count++;
        if (e[0] & 0x80) break;
    }
    RET(h);
}
HLE(u_LoadAcceleratorsA) { load_accel(st, 0); }
HLE(u_LoadAcceleratorsW) { load_accel(st, 1); }
HLE(u_CreateAcceleratorTable) {
    uint64_t h = accel_new();
    GuiAccel *a = accel_get(h);
    if (!a) { RET(0); return; }
    int n = ARGI(1);
    const uint8_t *p = PTR(0);
    for (int i = 0; i < n && i < 256; i++) { a->fvirt[i] = p[6 * i]; memcpy(&a->key[i], p + 6 * i + 2, 2); memcpy(&a->cmd[i], p + 6 * i + 4, 2); a->count++; }
    RET(h);
}
HLE(u_DestroyAcceleratorTable) { GuiAccel *a = accel_get(ARG(0)); if (a) a->used = 0; RET(1); }
HLE(u_TranslateAccelerator) {
    GuiAccel *a = accel_get(ARG(1));
    uint32_t msg = (uint32_t)gs_get(ARG(2), L_MSG, 1);
    uint64_t wp = gs_get(ARG(2), L_MSG, 2);
    if (!a || (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN && msg != WM_CHAR)) { RET(0); return; }
    int shift = (g_gui.keys[VK_SHIFT] & 0x80) != 0, ctrl = (g_gui.keys[VK_CONTROL] & 0x80) != 0, alt = (g_gui.keys[VK_MENU] & 0x80) != 0;
    for (int i = 0; i < a->count; i++) {
        uint8_t f = a->fvirt[i];
        if (f & 1) { // FVIRTKEY
            if (msg == WM_CHAR || a->key[i] != (uint16_t)wp) continue;
            if (((f & 4) != 0) != shift || ((f & 8) != 0) != ctrl || ((f & 0x10) != 0) != alt) continue;
        } else {
            if (msg != WM_CHAR || a->key[i] != (uint16_t)wp) continue;
        }
        win_send(st, ARG(0), WM_COMMAND, 0x10000u | a->cmd[i], 0);
        RET(1);
        return;
    }
    RET(0);
}
HLE(u_CopyAcceleratorTableW) { GuiAccel *a = accel_get(ARG(0)); RET(a ? (uint64_t)a->count : 0); }

// ---- resources --------------------------------------------------------------------------------------
static void load_string(X86State *st, int wide) {
    uint32_t id = (uint32_t)ARG(1);
    uint32_t sz;
    const uint16_t *blk = pe_resource(ARG(0), 6, (id >> 4) + 1, 1, &sz);
    int cap = ARGI(3);
    if (!blk) { if (ARG(2) && cap > 0) { if (wide) ((uint16_t *)PTR(2))[0] = 0; else ((char *)PTR(2))[0] = 0; } RET(0); return; }
    for (uint32_t i = 0; i < (id & 15); i++) blk += 1 + blk[0];
    uint16_t len = blk[0];
    if (wide && cap == 0) { wr_ptr(ARG(2), H2G(blk + 1)); RET(len); return; }
    if (!ARG(2) || cap <= 0) { RET(0); return; }
    if (wide) {
        int n = len < cap - 1 ? len : cap - 1;
        memcpy(PTR(2), blk + 1, (size_t)n * 2);
        ((uint16_t *)PTR(2))[n] = 0;
        RET(n);
    } else {
        char *u = malloc((size_t)len * 3 + 1);
        w2u(blk + 1, len, u, (size_t)len * 3 + 1);
        RET(put_str(ARG(2), (size_t)cap, u, 0));
        free(u);
    }
}
HLE(u_LoadStringA) { load_string(st, 0); }
HLE(u_LoadStringW) { load_string(st, 1); }
static void load_bitmap(X86State *st, int wide) {
    const uint8_t *r = pe_resource(ARG(0), 2, ARG(1), wide, NULL);
    RET(r ? gdi_bitmap_from_dib(r, NULL) : 0);
}
HLE(u_LoadBitmapA) { load_bitmap(st, 0); }
HLE(u_LoadBitmapW) { load_bitmap(st, 1); }
static uint64_t bitmap_from_file(const char *win_path) {
    char host[1024];
    if (path_w2h(win_path, host, sizeof host) != 0) return 0;
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 54 || n > (64 << 20)) { fclose(f); return 0; }
    uint8_t *buf = malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    uint64_t h = 0;
    if (got == (size_t)n && buf[0] == 'B' && buf[1] == 'M') {
        uint32_t off; memcpy(&off, buf + 10, 4);
        if (off < (uint32_t)n) h = gdi_bitmap_from_dib(buf + 14, buf + off);
    }
    free(buf);
    return h;
}
static void load_image(X86State *st, int wide) {
    // (hinst, name, type, cx, cy, flags)
    uint32_t type = (uint32_t)ARG(2), flags = (uint32_t)hle_arg(st, 5);
    if (type == 0) { // IMAGE_BITMAP
        if (flags & 0x10) { char *p = gstr(ARG(1), wide); RET(p ? bitmap_from_file(p) : 0); free(p); return; }
        const uint8_t *r = pe_resource(ARG(0), 2, ARG(1), wide, NULL);
        RET(r ? gdi_bitmap_from_dib(r, NULL) : 0);
        return;
    }
    RET(ICON_HANDLE_BASE + 0x400 + type);
}
HLE(u_LoadImageA) { load_image(st, 0); }
HLE(u_LoadImageW) { load_image(st, 1); }
HLE(u_CopyImage) { RET(ARG(0)); }

// ---- clipboard ----------------------------------------------------------------------------------
HLE(u_OpenClipboard) { g_gui.clip_open = 1; RET(1); }
HLE(u_CloseClipboard) { g_gui.clip_open = 0; RET(1); }
HLE(u_EmptyClipboard) { free(g_gui.clip_text); g_gui.clip_text = NULL; RET(1); }
HLE(u_SetClipboardData) {
    uint32_t fmt = (uint32_t)ARG(0);
    if (!ARG(1)) { RET(0); return; }
    char *s = NULL;
    if (fmt == 13) s = w2u_dup(WSTR(1));
    else if (fmt == 1 || fmt == 7) s = strdup(CSTR(1));
    if (s) { free(g_gui.clip_text); g_gui.clip_text = s; }
    RET(ARG(1));
}
HLE(u_GetClipboardData) {
    uint32_t fmt = (uint32_t)ARG(0);
    if (!g_gui.clip_text) { RET(0); return; }
    static void *last;
    free(last);
    if (fmt == 13) last = u2w_dup(g_gui.clip_text);
    else if (fmt == 1 || fmt == 7) last = strdup(g_gui.clip_text);
    else last = NULL;
    RET(last);
}
HLE(u_IsClipboardFormatAvailable) { uint32_t f = (uint32_t)ARG(0); RET(g_gui.clip_text && (f == 1 || f == 7 || f == 13)); }
HLE(u_CountClipboardFormats) { RET(g_gui.clip_text ? 2 : 0); }
HLE(u_EnumClipboardFormats) { uint32_t f = (uint32_t)ARG(0); RET(!g_gui.clip_text ? 0 : f == 0 ? 13 : f == 13 ? 1 : 0); }
HLE(u_RegisterClipboardFormat) { static uint32_t next = 0xC800; RET(next++); }
HLE(u_GetClipboardOwner) { RET(0); }

// ---- system information ---------------------------------------------------------------------------
HLE(u_GetSystemMetrics) {
    GuiState *g = &g_gui;
    switch (ARGI(0)) {
    case 0: case 16: case 78: case 61: RET(g->screen_w); break;
    case 1: case 79: case 62: RET(g->screen_h); break;
    case 17: RET(g->screen_h - 23); break;
    case 2: case 3: case 9: case 10: case 20: case 21: RET(17); break;
    case 4: RET(23); break;
    case 5: case 6: case 83: case 84: RET(1); break;
    case 7: case 8: RET(3); break;
    case 11: case 12: case 13: case 14: RET(32); break;
    case 15: RET(20); break;
    case 19: case 75: RET(1); break;
    case 28: case 34: RET(136); break;
    case 29: case 35: RET(39); break;
    case 30: RET(36); break;
    case 31: RET(22); break;
    case 32: case 33: RET(4); break;
    case 36: case 37: RET(4); break;
    case 38: case 39: RET(75); break;
    case 43: RET(3); break;
    case 45: case 46: RET(2); break;
    case 49: case 50: RET(16); break;
    case 80: case 81: RET(1); break;
    default: RET(0); break;
    }
}
static void fill_logfont(uint8_t *lf, int wide) {
    memset(lf, 0, wide ? 92 : 60);
    int32_t h = -12, wt = 400;
    memcpy(lf, &h, 4); memcpy(lf + 16, &wt, 4);
    lf[23] = 1; // DEFAULT_CHARSET
    if (wide) u2w("Segoe UI", -1, (uint16_t *)(void *)(lf + 28), 32); else snprintf((char *)lf + 28, 32, "Segoe UI");
}
static void system_parameters(X86State *st, int wide) {
    uint32_t action = (uint32_t)ARG(0);
    uint8_t *pv = PTR(2);
    switch (action) {
    case 0x30: wr_rect(ARG(2), 0, 0, g_gui.screen_w, g_gui.screen_h); RET(1); return; // SPI_GETWORKAREA
    case 0x29: { // SPI_GETNONCLIENTMETRICS
        uint32_t cb; memcpy(&cb, pv, 4);
        memset(pv + 4, 0, cb > 4 ? cb - 4 : 0);
        int lfs = wide ? 92 : 60;
        int32_t m5[5] = { 1, 17, 17, 23, 23 };
        memcpy(pv + 4, m5, 20);
        fill_logfont(pv + 24, wide);
        int o = 24 + lfs;
        int32_t sm[2] = { 17, 17 }; memcpy(pv + o, sm, 8); o += 8;
        fill_logfont(pv + o, wide); o += lfs;
        int32_t mm[2] = { 19, 19 }; memcpy(pv + o, mm, 8); o += 8;
        fill_logfont(pv + o, wide); o += lfs;
        fill_logfont(pv + o, wide); o += lfs;
        fill_logfont(pv + o, wide);
        RET(1);
        return;
    }
    case 0x1F: fill_logfont(pv, wide); RET(1); return; // SPI_GETICONTITLELOGFONT
    case 0x0E: case 0x66: case 0x68: case 0x1016: if (pv) mem_wr(ARG(2), 4, 0); RET(1); return;
    case 0x6A: case 0x64: if (pv) mem_wr(ARG(2), 4, 400); RET(1); return; // hover time / width
    case 0x0068 + 0x100: RET(1); return;
    default: if (pv && ARG(1) == 0 && action < 0x2000 && (action & 1) == 0) mem_wr(ARG(2), 4, 0); RET(1); return;
    }
}
HLE(u_SystemParametersInfoA) { system_parameters(st, 0); }
HLE(u_SystemParametersInfoW) { system_parameters(st, 1); }
HLE(u_MonitorFromWindow) { RET(0x7000); }
static void monitor_info(X86State *st, int wide) {
    uint8_t *mi = PTR(1);
    uint32_t cb; memcpy(&cb, mi, 4);
    int32_t r[4] = { 0, 0, g_gui.screen_w, g_gui.screen_h };
    memcpy(mi + 4, r, 16); memcpy(mi + 20, r, 16);
    uint32_t f = 1; memcpy(mi + 36, &f, 4);
    if (cb >= (wide ? 104u : 72u)) put_str(H2G(mi + 40), 32, "\\\\.\\DISPLAY1", wide);
    RET(1);
}
HLE(u_GetMonitorInfoA) { monitor_info(st, 0); }
HLE(u_GetMonitorInfoW) { monitor_info(st, 1); }
HLE(u_EnumDisplayMonitors) {
    int32_t *r = g_malloc(16);
    r[0] = 0; r[1] = 0; r[2] = g_gui.screen_w; r[3] = g_gui.screen_h;
    uint64_t a[4] = { 0x7000, ARG(0), H2G(r), ARG(3) };
    if (ARG(2)) soul_guest_call(st, ARG(2), 4, a);
    g_free(r);
    RET(1);
}
static void enum_display_settings(X86State *st, int wide) {
    uint8_t *dm = PTR(2);
    uint32_t mode = (uint32_t)ARG(1);
    if (mode != 0xFFFFFFFF && mode != 0xFFFFFFFE && mode > 0) { RET(0); return; }
    int o = wide ? 32 : 0;
    uint16_t size; memcpy(&size, dm + 36 + o, 2);
    uint32_t fields = 0x40000 | 0x80000 | 0x100000 | 0x400000;
    memcpy(dm + 40 + o, &fields, 4);
    int fo = wide ? 64 : 32; // dmFormName grows too
    uint32_t bpp = 32, w = (uint32_t)g_gui.screen_w, h = (uint32_t)g_gui.screen_h, hz = 60;
    memcpy(dm + 104 + o + (fo - 32), &bpp, 4);
    memcpy(dm + 108 + o + (fo - 32), &w, 4);
    memcpy(dm + 112 + o + (fo - 32), &h, 4);
    memcpy(dm + 120 + o + (fo - 32), &hz, 4);
    RET(1);
}
HLE(u_EnumDisplaySettingsA) { enum_display_settings(st, 0); }
HLE(u_EnumDisplaySettingsW) { enum_display_settings(st, 1); }
HLE_STUB_RET(u_ChangeDisplaySettings, 0)
HLE(u_EnumDisplayDevices) { RET(ARG(1) == 0 ? 1 : 0); }
HLE(u_GetDpiForWindow) { RET(96); }
HLE_STUB_RET(u_EnableNonClientDpiScaling, 1)
HLE(u_SetThreadDpiAwarenessContext) { RET((uint64_t)-4); }
HLE(u_GetThreadDpiAwarenessContext) { RET((uint64_t)-4); }
HLE_STUB_RET(u_IsProcessDPIAware, 1)
HLE_STUB_RET(u_SetLayeredWindowAttributes, 1)
HLE_STUB_RET(u_UpdateLayeredWindow, 1)
HLE_STUB_RET(u_SetWindowRgn, 1)
HLE_STUB_RET(u_GetWindowRgn, 0)
HLE_STUB_RET(u_AllowSetForegroundWindow, 1)
HLE_STUB_RET(u_LockSetForegroundWindow, 1)
HLE_STUB_RET(u_ChangeWindowMessageFilter, 1)
HLE_STUB_RET(u_ChangeWindowMessageFilterEx, 1)
HLE(u_RegisterPowerSettingNotification) { RET(0x7200); }
HLE_STUB_RET(u_UnregisterPowerSettingNotification, 1)
HLE(u_RegisterDeviceNotification) { RET(0x7300); }
HLE_STUB_RET(u_UnregisterDeviceNotification, 1)
HLE_STUB_RET(u_GetGUIThreadInfo, 0)
HLE_STUB_RET(u_InitCommonControls, 1)
HLE_STUB_RET(u_SetWindowTheme, 0)
HLE_STUB_RET(u_DwmSetWindowAttribute, 0)
HLE(u_DwmGetWindowAttribute) { RET(0x80004001); }
HLE_STUB_RET(u_DwmExtendFrameIntoClientArea, 0)
HLE(u_DwmIsCompositionEnabled) { if (ARG(0)) mem_wr(ARG(0), 4, 1); RET(0); }
HLE_STUB_RET(u_DwmFlush, 0)
HLE_STUB_RET(u_CommDlgExtendedError, 0)
HLE_STUB_RET(u_ChooseColor, 0)
HLE_STUB_RET(u_ChooseFont, 0)
HLE_STUB_RET(u_ImmGetContext, 0)
HLE_STUB_RET(u_ImmReleaseContext, 1)
HLE_STUB_RET(u_ImmAssociateContext, 0)
HLE_STUB_RET(u_ImmSetCompositionWindow, 1)
HLE_STUB_RET(u_SetProcessDpiAwareness, 0)
HLE(u_GetDpiForMonitor) { if (ARG(2)) mem_wr(ARG(2), 4, 96); if (ARG(3)) mem_wr(ARG(3), 4, 96); RET(0); }
HLE(u_GetScaleFactorForMonitor) { if (ARG(1)) mem_wr(ARG(1), 4, 100); RET(0); }

// ---- lifecycle ------------------------------------------------------------------------------------
void gui_reset(void) {
    GuiState *g = &g_gui;
    if (!g->mu_init) { pthread_mutex_init(&g->mu, NULL); g->mu_init = 1; }
    gui_shutdown();
    gdi_reset_objects();
    g->screen_w = g->screen_w > 0 ? g->screen_w : 1280;
    g->screen_h = g->screen_h > 0 ? g->screen_h : 800;
    for (int i = 0; i < 31; i++) g->sys_brush[i] = gdi_make_brush(k_sys_colors[i]);
    for (int i = 0; i < 31; i++) gdi_get(g->sys_brush[i], GO_BRUSH)->stock = 1;
    builtin_class("Button", CTL_BUTTON, 0x4008 | 3, 0, 0);
    builtin_class("Static", CTL_STATIC, 0x4008 | 3, 0, 0);
    builtin_class("Edit", CTL_EDIT, 0x4008 | 3, 0, 0);
    builtin_class("ListBox", CTL_LISTBOX, 0x4008 | 3, 0, 0);
    builtin_class("ComboBox", CTL_COMBOBOX, 0x4008 | 3, 0, 0);
    builtin_class("ComboLBox", CTL_LISTBOX, 0x4008 | 3, 0, 0);
    builtin_class("msctls_progress32", CTL_PROGRESS, 3, 0, 0);
    builtin_class("msctls_statusbar32", CTL_STATUS, 0x4008 | 3, 0, 0);
    builtin_class("msctls_trackbar32", CTL_TRACKBAR, 0x4008 | 3, 0, 0);
    builtin_class("tooltips_class32", CTL_TOOLTIP, 0, 0, 0);
    builtin_class("RichEdit20W", CTL_EDIT, 0x4008 | 3, 0, 0);
    builtin_class("RichEdit20A", CTL_EDIT, 0x4008 | 3, 0, 0);
    builtin_class("RICHEDIT50W", CTL_EDIT, 0x4008 | 3, 0, 0);
    builtin_class("RichEdit", CTL_EDIT, 0x4008 | 3, 0, 0);
    builtin_class("SysListView32", CTL_LISTVIEW, 0x4008 | 3, 0, 0);
    builtin_class("SysTreeView32", CTL_TREEVIEW, 0x4008 | 3, 0, 0);
    builtin_class("SysTabControl32", CTL_TAB, 0x4008 | 3, 0, 0);
    builtin_class("ToolbarWindow32", CTL_TOOLBAR, 0x4008 | 3, 0, 0);
    builtin_class("msctls_updown32", CTL_UPDOWN, 0x4008 | 3, 0, 0);
    builtin_class("SysHeader32", CTL_HEADER, 0x4008 | 3, 0, 0);
    builtin_class("SysLink", CTL_SYSLINK, 0x4008 | 3, 0, 0);
    static const char *generic[] = { "ReBarWindow32", "SysDateTimePick32", "SysMonthCal32", "msctls_hotkey32",
                                     "SysIPAddress32", "SysAnimate32", "ScrollBar", "SysPager", "NativeFontCtl", NULL };
    for (int i = 0; generic[i]; i++) builtin_class(generic[i], CTL_GENERIC, 0x4008 | 3, 0, 0);
    builtin_class("#32770", CTL_DIALOG, 0x0008, COLOR_BTNFACE + 1, 48);
    g_wm_count = 0;
    g_wm_next = 0xC100;
}

void gui_shutdown(void) {
    GuiState *g = &g_gui;
    if (!g->mu_init) return;
    gdi_release_guest_bits();
    gui_lock();
    for (int i = 0; i < WIN_MAX; i++) {
        Win *w = &g->wins[i];
        if (!w->used) continue;
        free(w->pixels); free(w->text); free(w->extra);
        for (int k = 0; k < w->item_count; k++) free(w->items[k]);
        free(w->items); free(w->item_data);
        if (w->cdata && w->cdata_free) w->cdata_free(w->cdata);
    }
    for (int i = 0; i < MENU_MAX; i++) free(g->menus[i].items);
    for (int i = 0; i < g->input_len; i++) free(g->input[i].text);
    for (int i = 0; i < 256; i++) if (g->classes[i].used && g->classes[i].menu_name >= 0x10000) g_free(G2H(g->classes[i].menu_name));
    free(g->input);
    free(g->queue);
    free(g->clip_text);
    int sw = g->screen_w, sh = g->screen_h;
    pthread_mutex_t mu = g->mu;
    memset(g, 0, sizeof *g);
    g->mu = mu;
    g->mu_init = 1;
    g->screen_w = sw; g->screen_h = sh;
    pthread_mutex_unlock(&g->mu);
}

// ---- public API for the iPad UI ---------------------------------------------------------------------

static void push_input(GuiInput ev) {
    GuiState *g = &g_gui;
    gui_lock();
    if (g->input_len == g->input_cap) { g->input_cap = g->input_cap ? g->input_cap * 2 : 64; g->input = realloc(g->input, sizeof(GuiInput) * (size_t)g->input_cap); }
    g->input[g->input_len++] = ev;
    gui_unlock();
    if (g_proc) {
        pthread_mutex_lock(&g_proc->ev_mu);
        g_proc->ev_gen++;
        pthread_cond_broadcast(&g_proc->ev_cv);
        pthread_mutex_unlock(&g_proc->ev_mu);
    }
}

void soul_gui_event(SoulProcess *p, uint64_t hwnd, int type, int x, int y, int data, uint32_t ch, const char *text) {
    if (!p || p != g_proc || !g_gui.mu_init) return;
    GuiInput ev = { hwnd, type, x, y, data, ch, text ? strdup(text) : NULL };
    push_input(ev);
}

int soul_gui_windows(SoulProcess *p, SoulWindowInfo *out, int max) {
    if (!p || p != g_proc || !g_gui.mu_init) return 0;
    GuiState *g = &g_gui;
    int n = 0;
    gui_lock();
    Win *list[WIN_MAX];
    int count = 0;
    for (int i = 0; i < WIN_MAX; i++) {
        Win *w = &g->wins[i];
        if (w->used && !w->parent && w->visible && !w->message_only && w->pixels && w->ctl != CTL_TOOLTIP) list[count++] = w;
    }
    for (int a = 1; a < count; a++) for (int b = a; b > 0; b--) {
        int swap = list[b - 1]->zstamp > list[b]->zstamp ||
                   (list[b - 1]->zstamp == list[b]->zstamp && list[b - 1]->seq > list[b]->seq);
        if ((list[b]->exstyle & WS_EX_TOPMOST) && !(list[b - 1]->exstyle & WS_EX_TOPMOST)) swap = 0;
        if (!swap) break;
        Win *t = list[b]; list[b] = list[b - 1]; list[b - 1] = t;
    }
    Win *focus = win_get(g->focus);
    for (int i = 0; i < count && n < max; i++) {
        Win *w = list[i];
        SoulWindowInfo *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->hwnd = w->hwnd;
        o->owner = w->owner;
        o->x = w->x; o->y = w->y; o->width = w->fb_w; o->height = w->fb_h;
        unsigned f = 0;
        if ((w->style & WS_CAPTION) == WS_CAPTION || (!(w->style & WS_POPUP))) f |= SOUL_WIN_CAPTION;
        if (w->style & WS_THICKFRAME) f |= SOUL_WIN_RESIZABLE;
        if (w->is_dialog) f |= SOUL_WIN_DIALOG;
        if (g->active == w->hwnd) f |= SOUL_WIN_ACTIVE;
        if (!w->enabled) f |= SOUL_WIN_DISABLED;
        if (focus && win_top(focus) == w && focus->ctl == CTL_EDIT && !focus->readonly) f |= SOUL_WIN_WANTS_TEXT;
        if (w->menu && menu_get(w->menu) && menu_get(w->menu)->count) f |= SOUL_WIN_HAS_MENU;
        if (w->maximized) f |= SOUL_WIN_MAXIMIZED;
        if (w->style & WS_SYSMENU) f |= SOUL_WIN_CLOSABLE;
        o->flags = f;
        o->version = w->version;
        Win *cw = win_get(g->caret_hwnd);
        if (cw && g->caret_visible && win_top(cw) == w && win_shown(cw)) {
            int ox, oy;
            win_client_origin(cw, &ox, &oy);
            o->caret_visible = 1;
            o->caret_x = ox + g->caret_x; o->caret_y = oy + g->caret_y; o->caret_w = g->caret_w; o->caret_h = g->caret_h;
        }
        snprintf(o->title, sizeof o->title, "%s", w->text ? w->text : "");
    }
    gui_unlock();
    return n;
}

int soul_gui_copy(SoulProcess *p, uint64_t hwnd, void *dst, size_t stride, int width, int height, uint64_t *version) {
    if (!p || p != g_proc || !g_gui.mu_init) return 0;
    int copied = 0;
    gui_lock();
    Win *w = win_get(hwnd);
    if (w && w->pixels && (!version || *version != w->version)) {
        int cw = width < w->fb_w ? width : w->fb_w, ch = height < w->fb_h ? height : w->fb_h;
        for (int y = 0; y < ch; y++) memcpy((uint8_t *)dst + (size_t)y * stride, w->pixels + (size_t)y * w->fb_w, (size_t)cw * 4);
        if (version) *version = w->version;
        copied = 1;
    }
    gui_unlock();
    return copied;
}

static int flatten_menu(uint64_t h, int depth, SoulMenuEntry *out, int n, int max) {
    Menu *m = menu_get(h);
    if (!m || depth > 6) return n;
    for (int i = 0; i < m->count && n < max; i++) {
        MenuItem *it = &m->items[i];
        SoulMenuEntry *e = &out[n++];
        memset(e, 0, sizeof *e);
        e->depth = depth;
        e->id = it->id;
        unsigned f = 0;
        if (it->submenu) f |= SOUL_MENU_POPUP;
        if (it->flags & 0x800) f |= SOUL_MENU_SEPARATOR;
        if (it->flags & 8) f |= SOUL_MENU_CHECKED;
        if (it->flags & 3) f |= SOUL_MENU_DISABLED;
        if (it->flags & 0x200) f |= SOUL_MENU_RADIO;
        e->flags = f;
        snprintf(e->text, sizeof e->text, "%s", it->text);
        if (it->submenu) n = flatten_menu(it->submenu, depth + 1, out, n, max);
    }
    return n;
}
int soul_gui_menu(SoulProcess *p, uint64_t hwnd, SoulMenuEntry *out, int max) {
    if (!p || p != g_proc || !g_gui.mu_init) return 0;
    gui_lock();
    Win *w = win_get(hwnd);
    int n = w && w->menu ? flatten_menu(w->menu, 0, out, 0, max) : 0;
    gui_unlock();
    return n;
}
int soul_gui_popup(SoulProcess *p, SoulPopupInfo *info, SoulMenuEntry *out, int max) {
    memset(info, 0, sizeof *info);
    if (!p || p != g_proc || !g_gui.mu_init) return 0;
    GuiState *g = &g_gui;
    int n = 0;
    gui_lock();
    if (g->popup_active) {
        info->active = 1;
        info->kind = g->popup_kind;
        info->hwnd = g->popup_hwnd;
        info->x = g->popup_x; info->y = g->popup_y;
        Win *w = win_get(g->popup_hwnd);
        if (g->popup_kind == 1) {
            n = flatten_menu(g->popup_menu, 0, out, 0, max);
            if (w) { Win *t = win_top(w); info->hwnd = t->hwnd; info->x -= t->x; info->y -= t->y; }
        } else if (w) {
            for (int i = 0; i < w->item_count && n < max; i++) {
                SoulMenuEntry *e = &out[n++];
                memset(e, 0, sizeof *e);
                e->id = (uint32_t)i + 1;
                e->flags = i == w->cur_sel ? SOUL_MENU_CHECKED : 0;
                snprintf(e->text, sizeof e->text, "%s", w->items[i]);
            }
            Win *t = win_top(w);
            int ox, oy;
            win_client_origin(w, &ox, &oy);
            info->hwnd = t->hwnd; info->x = ox; info->y = oy + w->h;
        }
    }
    gui_unlock();
    return n;
}
int soul_gui_popup_active(SoulProcess *p) { return p && p == g_proc && g_gui.popup_active; }
int soul_gui_idle(SoulProcess *p) {
    if (!p || p != g_proc || !g_gui.mu_init) return 1;
    GuiState *g = &g_gui;
    if (g->input_len || g->qlen) return 0;
    for (int i = 0; i < WIN_MAX; i++) if (g->wins[i].used && g->wins[i].invalid && win_shown(&g->wins[i])) return 0;
    return 1;
}
uint64_t soul_gui_generation(SoulProcess *p) { return p && p == g_proc ? g_gui.generation : 0; }
void soul_gui_set_screen(SoulProcess *p, int w, int h) {
    (void)p;
    if (w >= 320 && h >= 240) { g_gui.screen_w = w; g_gui.screen_h = h; }
}
int soul_gui_has_windows(SoulProcess *p) {
    if (!p || p != g_proc || !g_gui.mu_init) return 0;
    for (int i = 0; i < WIN_MAX; i++) if (g_gui.wins[i].used && !g_gui.wins[i].parent && !g_gui.wins[i].message_only) return 1;
    return 0;
}

// ---- table ----------------------------------------------------------------------------------------
#define E(n, f) { n, f, NULL }
const HleEntry hle_user32_table[] = {
    E("RegisterClassA", u_RegisterClassA), E("RegisterClassW", u_RegisterClassW), E("RegisterClassExA", u_RegisterClassExA),
    E("RegisterClassExW", u_RegisterClassExW), E("UnregisterClassA", u_UnregisterClassA), E("UnregisterClassW", u_UnregisterClassW),
    E("GetClassInfoA", u_GetClassInfoA), E("GetClassInfoW", u_GetClassInfoW), E("GetClassInfoExA", u_GetClassInfoExA), E("GetClassInfoExW", u_GetClassInfoExW),
    E("GetClassNameA", u_GetClassNameA), E("GetClassNameW", u_GetClassNameW), E("RealGetWindowClassA", u_GetClassNameA), E("RealGetWindowClassW", u_GetClassNameW),
    E("GetClassLongPtrA", u_GetClassLongPtr), E("GetClassLongPtrW", u_GetClassLongPtr), E("GetClassLongA", u_GetClassLongPtr), E("GetClassLongW", u_GetClassLongPtr),
    E("SetClassLongPtrA", u_SetClassLongPtr), E("SetClassLongPtrW", u_SetClassLongPtr), E("SetClassLongA", u_SetClassLongPtr), E("SetClassLongW", u_SetClassLongPtr),
    E("CreateWindowExA", u_CreateWindowExA), E("CreateWindowExW", u_CreateWindowExW), E("DestroyWindow", u_DestroyWindow),
    E("ShowWindow", u_ShowWindow), E("ShowWindowAsync", u_ShowWindowAsync), E("MoveWindow", u_MoveWindow), E("SetWindowPos", u_SetWindowPos),
    E("BeginDeferWindowPos", u_BeginDeferWindowPos), E("DeferWindowPos", u_DeferWindowPos), E("EndDeferWindowPos", u_EndDeferWindowPos),
    E("BringWindowToTop", u_BringWindowToTop), E("SetForegroundWindow", u_SetForegroundWindow), E("SetActiveWindow", u_SetActiveWindow),
    E("GetActiveWindow", u_GetActiveWindow), E("GetForegroundWindow", u_GetForegroundWindow), E("SetFocus", u_SetFocus), E("GetFocus", u_GetFocus),
    E("SetCapture", u_SetCapture), E("ReleaseCapture", u_ReleaseCapture), E("GetCapture", u_GetCapture), E("EnableWindow", u_EnableWindow),
    E("IsWindowEnabled", u_IsWindowEnabled), E("IsWindow", u_IsWindow), E("IsWindowVisible", u_IsWindowVisible), E("IsIconic", u_IsIconic),
    E("IsZoomed", u_IsZoomed), E("IsWindowUnicode", u_IsWindowUnicode), E("IsChild", u_IsChild), E("GetParent", u_GetParent), E("SetParent", u_SetParent),
    E("GetAncestor", u_GetAncestor), E("GetWindow", u_GetWindow), E("GetTopWindow", u_GetTopWindow), E("GetDesktopWindow", u_GetDesktopWindow),
    E("GetShellWindow", u_GetShellWindow), E("GetLastActivePopup", u_GetLastActivePopup), E("GetWindowThreadProcessId", u_GetWindowThreadProcessId),
    E("EnumWindows", u_EnumWindows), E("EnumChildWindows", u_EnumChildWindows), E("EnumThreadWindows", u_EnumThreadWindows),
    E("FindWindowA", u_FindWindowA), E("FindWindowW", u_FindWindowW), E("FindWindowExA", u_FindWindowExA), E("FindWindowExW", u_FindWindowExW),
    E("SetWindowTextA", u_SetWindowTextA), E("SetWindowTextW", u_SetWindowTextW), E("GetWindowTextA", u_GetWindowTextA), E("GetWindowTextW", u_GetWindowTextW),
    E("InternalGetWindowText", u_InternalGetWindowText), E("GetWindowTextLengthA", u_GetWindowTextLengthA), E("GetWindowTextLengthW", u_GetWindowTextLengthW),
    E("GetWindowLongPtrA", u_GetWindowLongPtrA), E("GetWindowLongPtrW", u_GetWindowLongPtrW), E("SetWindowLongPtrA", u_SetWindowLongPtrA),
    E("SetWindowLongPtrW", u_SetWindowLongPtrW), E("GetWindowLongA", u_GetWindowLongA), E("GetWindowLongW", u_GetWindowLongW),
    E("SetWindowLongA", u_SetWindowLongA), E("SetWindowLongW", u_SetWindowLongW), E("GetWindowWord", u_GetWindowWord),
    E("SetPropA", u_SetPropA), E("SetPropW", u_SetPropW), E("GetPropA", u_GetPropA), E("GetPropW", u_GetPropW), E("RemovePropA", u_RemovePropA), E("RemovePropW", u_RemovePropW),
    E("GetClientRect", u_GetClientRect), E("GetWindowRect", u_GetWindowRect), E("ClientToScreen", u_ClientToScreen), E("ScreenToClient", u_ScreenToClient),
    E("MapWindowPoints", u_MapWindowPoints), E("AdjustWindowRect", u_AdjustWindowRect), E("AdjustWindowRectEx", u_AdjustWindowRect),
    E("AdjustWindowRectExForDpi", u_AdjustWindowRect), E("GetWindowPlacement", u_GetWindowPlacement), E("SetWindowPlacement", u_SetWindowPlacement),
    E("WindowFromPoint", u_WindowFromPoint), E("ChildWindowFromPoint", u_ChildWindowFromPoint), E("ChildWindowFromPointEx", u_ChildWindowFromPoint),
    E("RealChildWindowFromPoint", u_ChildWindowFromPoint),
    E("GetDC", u_GetDC), E("GetDCEx", u_GetDC), E("GetWindowDC", u_GetDC), E("ReleaseDC", u_ReleaseDC), E("BeginPaint", u_BeginPaint), E("EndPaint", u_EndPaint),
    E("InvalidateRect", u_InvalidateRect), E("InvalidateRgn", u_InvalidateRgn), E("ValidateRect", u_ValidateRect), E("ValidateRgn", u_ValidateRect),
    E("UpdateWindow", u_UpdateWindow), E("RedrawWindow", u_RedrawWindow), E("GetUpdateRect", u_GetUpdateRect), E("GetUpdateRgn", u_GetUpdateRgn),
    E("ScrollWindow", u_ScrollWindow), E("ScrollWindowEx", u_ScrollWindowEx), E("ScrollDC", u_ScrollDC), E("LockWindowUpdate", u_LockWindowUpdate),
    E("FillRect", u_FillRect), E("FrameRect", u_FrameRect), E("InvertRect", u_InvertRect), E("DrawFocusRect", u_DrawFocusRect), E("DrawEdge", u_DrawEdge),
    E("DrawFrameControl", u_DrawFrameControl), E("DrawTextA", u_DrawTextA), E("DrawTextW", u_DrawTextW), E("DrawTextExA", u_DrawTextExA),
    E("DrawTextExW", u_DrawTextExW), E("TabbedTextOutA", u_TabbedTextOutA), E("TabbedTextOutW", u_TabbedTextOutW), E("GetSysColor", u_GetSysColor),
    E("GetSysColorBrush", u_GetSysColorBrush), E("SetSysColors", u_SetSysColors), E("DrawIcon", u_DrawIcon), E("DrawIconEx", u_DrawIconEx),
    E("DrawStateA", u_DrawStateA), E("DrawStateW", u_DrawStateA),
    E("DefWindowProcA", u_DefWindowProcA), E("DefWindowProcW", u_DefWindowProcW), E("CallWindowProcA", u_CallWindowProcA), E("CallWindowProcW", u_CallWindowProcW),
    E("SendMessageA", u_SendMessageA), E("SendMessageW", u_SendMessageW), E("SendMessageTimeoutA", u_SendMessageTimeoutA), E("SendMessageTimeoutW", u_SendMessageTimeoutW),
    E("SendNotifyMessageA", u_SendMessageA), E("SendNotifyMessageW", u_SendMessageW), E("PostMessageA", u_PostMessage), E("PostMessageW", u_PostMessage),
    E("PostThreadMessageA", u_PostThreadMessage), E("PostThreadMessageW", u_PostThreadMessage), E("PostQuitMessage", u_PostQuitMessage),
    E("InSendMessage", u_InSendMessage), E("ReplyMessage", u_ReplyMessage), E("RegisterWindowMessageA", u_RegisterWindowMessageA),
    E("RegisterWindowMessageW", u_RegisterWindowMessageW), E("GetMessageA", u_GetMessageA), E("GetMessageW", u_GetMessageW),
    E("PeekMessageA", u_PeekMessage), E("PeekMessageW", u_PeekMessage), E("WaitMessage", u_WaitMessage),
    E("MsgWaitForMultipleObjects", u_MsgWaitForMultipleObjects), E("MsgWaitForMultipleObjectsEx", u_MsgWaitForMultipleObjects),
    E("TranslateMessage", u_TranslateMessage), E("DispatchMessageA", u_DispatchMessageA), E("DispatchMessageW", u_DispatchMessageW),
    E("GetMessageTime", u_GetMessageTime), E("GetMessagePos", u_GetMessagePos), E("GetMessageExtraInfo", u_GetMessageExtraInfo),
    E("GetQueueStatus", u_GetQueueStatus), E("GetInputState", u_GetInputState), E("SetTimer", u_SetTimer), E("KillTimer", u_KillTimer),
    E("GetKeyState", u_GetKeyState), E("GetAsyncKeyState", u_GetAsyncKeyState), E("GetKeyboardState", u_GetKeyboardState),
    E("SetKeyboardState", u_SetKeyboardState), E("MapVirtualKeyA", u_MapVirtualKey), E("MapVirtualKeyW", u_MapVirtualKey),
    E("MapVirtualKeyExA", u_MapVirtualKey), E("MapVirtualKeyExW", u_MapVirtualKey), E("VkKeyScanA", u_VkKeyScanA), E("VkKeyScanW", u_VkKeyScanW),
    E("ToAscii", u_ToAscii), E("ToUnicode", u_ToUnicode), E("ToUnicodeEx", u_ToUnicode), E("GetKeyNameTextA", u_GetKeyNameTextA), E("GetKeyNameTextW", u_GetKeyNameTextW),
    E("GetKeyboardLayout", u_GetKeyboardLayout), E("GetKeyboardType", u_GetKeyboardType), E("GetCursorPos", u_GetCursorPos), E("SetCursorPos", u_SetCursorPos),
    E("GetPhysicalCursorPos", u_GetPhysicalCursorPos), E("ClipCursor", u_ClipCursor), E("GetClipCursor", u_GetClipCursor), E("ShowCursor", u_ShowCursor),
    E("SetCursor", u_SetCursor), E("GetCursor", u_GetCursor), E("LoadCursorA", u_LoadCursor), E("LoadCursorW", u_LoadCursor),
    E("LoadIconA", u_LoadIcon), E("LoadIconW", u_LoadIcon), E("CreateIconIndirect", u_CreateIconIndirect), E("DestroyIcon", u_DestroyIcon),
    E("DestroyCursor", u_DestroyCursor), E("CopyIcon", u_CopyIcon), E("GetIconInfo", u_GetIconInfo), E("TrackMouseEvent", u_TrackMouseEvent),
    E("_TrackMouseEvent", u_TrackMouseEvent), E("GetDoubleClickTime", u_GetDoubleClickTime), E("SwapMouseButton", u_SwapMouseButton),
    E("SendInput", u_SendInput), E("keybd_event", u_keybd_event), E("mouse_event", u_mouse_event), E("RegisterHotKey", u_RegisterHotKey),
    E("UnregisterHotKey", u_UnregisterHotKey), E("RegisterRawInputDevices", u_RegisterRawInputDevices), E("GetRawInputData", u_GetRawInputData),
    E("GetRawInputDeviceList", u_GetRawInputDeviceList), E("SetWindowsHookExA", u_SetWindowsHookEx), E("SetWindowsHookExW", u_SetWindowsHookEx),
    E("UnhookWindowsHookEx", u_UnhookWindowsHookEx), E("CallNextHookEx", u_CallNextHookEx), E("FlashWindow", u_FlashWindow), E("FlashWindowEx", u_FlashWindowEx),
    E("CreateCaret", u_CreateCaret), E("DestroyCaret", u_DestroyCaret), E("ShowCaret", u_ShowCaret), E("HideCaret", u_HideCaret),
    E("SetCaretPos", u_SetCaretPos), E("GetCaretPos", u_GetCaretPos), E("GetCaretBlinkTime", u_GetCaretBlinkTime), E("SetCaretBlinkTime", u_SetCaretBlinkTime),
    E("SetScrollInfo", u_SetScrollInfo), E("GetScrollInfo", u_GetScrollInfo), E("SetScrollPos", u_SetScrollPos), E("GetScrollPos", u_GetScrollPos),
    E("SetScrollRange", u_SetScrollRange), E("GetScrollRange", u_GetScrollRange), E("ShowScrollBar", u_ShowScrollBar), E("EnableScrollBar", u_EnableScrollBar),
    E("CreateMenu", u_CreateMenu), E("CreatePopupMenu", u_CreatePopupMenu), E("DestroyMenu", u_DestroyMenu), E("AppendMenuA", u_AppendMenuA),
    E("AppendMenuW", u_AppendMenuW), E("InsertMenuA", u_InsertMenuA), E("InsertMenuW", u_InsertMenuW), E("ModifyMenuA", u_ModifyMenuA),
    E("ModifyMenuW", u_ModifyMenuW), E("DeleteMenu", u_DeleteMenu), E("RemoveMenu", u_RemoveMenu), E("SetMenu", u_SetMenu), E("GetMenu", u_GetMenu),
    E("GetSubMenu", u_GetSubMenu), E("GetMenuItemCount", u_GetMenuItemCount), E("GetMenuItemID", u_GetMenuItemID), E("DrawMenuBar", u_DrawMenuBar),
    E("CheckMenuItem", u_CheckMenuItem), E("EnableMenuItem", u_EnableMenuItem), E("CheckMenuRadioItem", u_CheckMenuRadioItem),
    E("GetMenuState", u_GetMenuState), E("GetMenuStringA", u_GetMenuStringA), E("GetMenuStringW", u_GetMenuStringW),
    E("GetMenuItemInfoA", u_GetMenuItemInfoA), E("GetMenuItemInfoW", u_GetMenuItemInfoW), E("SetMenuItemInfoA", u_SetMenuItemInfoA),
    E("SetMenuItemInfoW", u_SetMenuItemInfoW), E("InsertMenuItemA", u_InsertMenuItemA), E("InsertMenuItemW", u_InsertMenuItemW),
    E("SetMenuDefaultItem", u_SetMenuDefaultItem), E("SetMenuInfo", u_SetMenuInfo), E("GetMenuInfo", u_GetMenuInfo), E("SetMenuItemBitmaps", u_SetMenuItemBitmaps),
    E("GetSystemMenu", u_GetSystemMenu), E("IsMenu", u_IsMenu), E("LoadMenuA", u_LoadMenuA), E("LoadMenuW", u_LoadMenuW),
    E("LoadMenuIndirectA", u_LoadMenuIndirect), E("LoadMenuIndirectW", u_LoadMenuIndirect), E("TrackPopupMenu", u_TrackPopupMenu),
    E("TrackPopupMenuEx", u_TrackPopupMenuEx), E("EndMenu", u_EndMenu),
    E("LoadAcceleratorsA", u_LoadAcceleratorsA), E("LoadAcceleratorsW", u_LoadAcceleratorsW), E("CreateAcceleratorTableA", u_CreateAcceleratorTable),
    E("CreateAcceleratorTableW", u_CreateAcceleratorTable), E("DestroyAcceleratorTable", u_DestroyAcceleratorTable),
    E("TranslateAcceleratorA", u_TranslateAccelerator), E("TranslateAcceleratorW", u_TranslateAccelerator), E("CopyAcceleratorTableW", u_CopyAcceleratorTableW),
    E("LoadStringA", u_LoadStringA), E("LoadStringW", u_LoadStringW), E("LoadBitmapA", u_LoadBitmapA), E("LoadBitmapW", u_LoadBitmapW),
    E("LoadImageA", u_LoadImageA), E("LoadImageW", u_LoadImageW), E("CopyImage", u_CopyImage),
    E("OpenClipboard", u_OpenClipboard), E("CloseClipboard", u_CloseClipboard), E("EmptyClipboard", u_EmptyClipboard),
    E("SetClipboardData", u_SetClipboardData), E("GetClipboardData", u_GetClipboardData), E("IsClipboardFormatAvailable", u_IsClipboardFormatAvailable),
    E("CountClipboardFormats", u_CountClipboardFormats), E("EnumClipboardFormats", u_EnumClipboardFormats),
    E("RegisterClipboardFormatA", u_RegisterClipboardFormat), E("RegisterClipboardFormatW", u_RegisterClipboardFormat), E("GetClipboardOwner", u_GetClipboardOwner),
    E("GetSystemMetrics", u_GetSystemMetrics), E("GetSystemMetricsForDpi", u_GetSystemMetrics), E("SystemParametersInfoA", u_SystemParametersInfoA),
    E("SystemParametersInfoW", u_SystemParametersInfoW), E("SystemParametersInfoForDpi", u_SystemParametersInfoW), E("MonitorFromWindow", u_MonitorFromWindow),
    E("MonitorFromPoint", u_MonitorFromWindow), E("MonitorFromRect", u_MonitorFromWindow), E("GetMonitorInfoA", u_GetMonitorInfoA),
    E("GetMonitorInfoW", u_GetMonitorInfoW), E("EnumDisplayMonitors", u_EnumDisplayMonitors), E("EnumDisplaySettingsA", u_EnumDisplaySettingsA),
    E("EnumDisplaySettingsW", u_EnumDisplaySettingsW), E("EnumDisplaySettingsExA", u_EnumDisplaySettingsA), E("EnumDisplaySettingsExW", u_EnumDisplaySettingsW),
    E("ChangeDisplaySettingsA", u_ChangeDisplaySettings), E("ChangeDisplaySettingsW", u_ChangeDisplaySettings),
    E("ChangeDisplaySettingsExA", u_ChangeDisplaySettings), E("ChangeDisplaySettingsExW", u_ChangeDisplaySettings),
    E("EnumDisplayDevicesA", u_EnumDisplayDevices), E("EnumDisplayDevicesW", u_EnumDisplayDevices), E("GetDpiForWindow", u_GetDpiForWindow),
    E("EnableNonClientDpiScaling", u_EnableNonClientDpiScaling), E("SetThreadDpiAwarenessContext", u_SetThreadDpiAwarenessContext),
    E("GetThreadDpiAwarenessContext", u_GetThreadDpiAwarenessContext), E("IsProcessDPIAware", u_IsProcessDPIAware),
    E("SetLayeredWindowAttributes", u_SetLayeredWindowAttributes), E("UpdateLayeredWindow", u_UpdateLayeredWindow), E("SetWindowRgn", u_SetWindowRgn),
    E("GetWindowRgn", u_GetWindowRgn), E("AllowSetForegroundWindow", u_AllowSetForegroundWindow), E("LockSetForegroundWindow", u_LockSetForegroundWindow),
    E("ChangeWindowMessageFilter", u_ChangeWindowMessageFilter), E("ChangeWindowMessageFilterEx", u_ChangeWindowMessageFilterEx),
    E("RegisterPowerSettingNotification", u_RegisterPowerSettingNotification), E("UnregisterPowerSettingNotification", u_UnregisterPowerSettingNotification),
    E("RegisterDeviceNotificationA", u_RegisterDeviceNotification), E("RegisterDeviceNotificationW", u_RegisterDeviceNotification),
    E("UnregisterDeviceNotification", u_UnregisterDeviceNotification), E("GetGUIThreadInfo", u_GetGUIThreadInfo),
    E("InitCommonControls", u_InitCommonControls), E("InitCommonControlsEx", u_InitCommonControls), E("SetWindowTheme", u_SetWindowTheme),
    E("DwmSetWindowAttribute", u_DwmSetWindowAttribute), E("DwmGetWindowAttribute", u_DwmGetWindowAttribute),
    E("DwmExtendFrameIntoClientArea", u_DwmExtendFrameIntoClientArea), E("DwmIsCompositionEnabled", u_DwmIsCompositionEnabled), E("DwmFlush", u_DwmFlush),
    E("CommDlgExtendedError", u_CommDlgExtendedError), E("ChooseColorA", u_ChooseColor), E("ChooseColorW", u_ChooseColor),
    E("ChooseFontA", u_ChooseFont), E("ChooseFontW", u_ChooseFont), E("ImmGetContext", u_ImmGetContext), E("ImmReleaseContext", u_ImmReleaseContext),
    E("ImmAssociateContext", u_ImmAssociateContext), E("ImmSetCompositionWindow", u_ImmSetCompositionWindow),
    E("SetProcessDpiAwareness", u_SetProcessDpiAwareness), E("GetDpiForMonitor", u_GetDpiForMonitor), E("GetScaleFactorForMonitor", u_GetScaleFactorForMonitor),
    { NULL, NULL, NULL }
};
