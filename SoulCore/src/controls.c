// SoulCore - built-in Win32 controls (Button, Static, Edit, ListBox, ComboBox, progress bar) and dialogs.
#include "gui.h"
#include <ctype.h>
#include <strings.h>

#define ARGI(n) ((int32_t)hle_arg(st, (n)))
#define LOWORD(v) ((uint32_t)(v) & 0xFFFF)
#define HIWORD(v) (((uint32_t)(v) >> 16) & 0xFFFF)

static char *gstr(uint64_t p, int wide) {
    if (p < 0x10000) return NULL;
    return wide ? w2u_dup((const uint16_t *)(uintptr_t)p) : strdup((const char *)(uintptr_t)p);
}
static size_t put_str(uint64_t dst, size_t cap, const char *s, int wide) {
    if (!dst || cap == 0) return 0;
    if (wide) return u2w(s, -1, (uint16_t *)(uintptr_t)dst, cap);
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy((void *)(uintptr_t)dst, s, n);
    ((char *)(uintptr_t)dst)[n] = 0;
    return n;
}
static size_t units(const char *s, int wide) {
    if (!wide) return strlen(s);
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if ((*p & 0xC0) != 0x80) n += *p >= 0xF0 ? 2 : 1;
    return n;
}

static uint64_t font_of(Win *w) { return w->font ? w->font : gdi_default_font(); }
static int line_height(Win *w) { int h; gdi_text_width(font_of(w), "Ay", 2, &h); return h; }

// colour of a brush returned by WM_CTLCOLOR* (or a (COLOR_x + 1) value); 0 = hollow
static int brush_color(uint64_t b, uint32_t *color) {
    if (b > 0 && b < 0x40) { *color = gui_sys_color((int)b - 1); return 1; }
    GdiObj *o = gdi_get(b, GO_BRUSH);
    if (!o || o->style == 1) return 0;
    *color = o->color;
    return 1;
}

typedef struct Paint { uint64_t hdc; uint32_t bg, fg; int has_bg; } Paint;

static Paint ctl_begin(X86State *st, Win *w, uint32_t ctlcolor_msg) {
    Paint p;
    p.hdc = gdi_create_dc(DC_WINDOW, w->hwnd);
    Dc *dc = dc_get(p.hdc);
    dc->font = font_of(w);
    w->invalid = 0;
    w->erase = 0;
    uint64_t br = ctlcolor_msg ? win_ctl_color(st, w, p.hdc, ctlcolor_msg) : 0;
    p.has_bg = brush_color(br, &p.bg);
    if (!p.has_bg) p.bg = gui_sys_color(COLOR_BTNFACE);
    p.fg = dc->text_color;
    dc->bk_mode = 1; // TRANSPARENT
    return p;
}
static void ctl_end(Paint *p) { gdi_release_dc(p->hdc); }
static void set_text_color(uint64_t hdc, uint32_t c) { Dc *dc = dc_get(hdc); if (dc) dc->text_color = c; }

static void text_in_rect(uint64_t hdc, const char *s, int x0, int y0, int x1, int y1, uint32_t fmt) {
    int32_t r[4] = { x0, y0, x1, y1 };
    gdi_draw_text_dc(hdc, s, -1, r, fmt);
}

// ---- Button ------------------------------------------------------------------------------------

#define BS_TYPE(w) ((w)->style & 0xF)
enum { BS_PUSH = 0, BS_DEFPUSH = 1, BS_CHECK = 2, BS_AUTOCHECK = 3, BS_RADIO = 4, BS_3STATE = 5, BS_AUTO3STATE = 6, BS_GROUP = 7,
       BS_AUTORADIO = 9, BS_OWNERDRAW = 0xB, BS_SPLIT = 0xC, BS_DEFSPLIT = 0xD, BS_CMDLINK = 0xE };

static int is_push(Win *w) { int t = BS_TYPE(w); return t == BS_PUSH || t == BS_DEFPUSH || t == BS_SPLIT || t == BS_DEFSPLIT || t == BS_CMDLINK || (w->style & 0x1000); }

static void owner_draw(X86State *st, Win *w, uint64_t hdc, uint32_t type, uint32_t item, uint32_t state, int x0, int y0, int x1, int y1, uint64_t data) {
    uint8_t *ds = calloc(1, 64);
    uint32_t v[5] = { type, (uint32_t)w->id, item, 1 /* ODA_DRAWENTIRE */, state };
    memcpy(ds, v, 20);
    memcpy(ds + 24, &w->hwnd, 8);
    memcpy(ds + 32, &hdc, 8);
    int32_t r[4] = { x0, y0, x1, y1 };
    memcpy(ds + 40, r, 16);
    memcpy(ds + 56, &data, 8);
    win_send(st, w->parent, WM_DRAWITEM, w->id, (uint64_t)(uintptr_t)ds);
    free(ds);
}

static void button_paint(X86State *st, Win *w) {
    GuiState *g = gui();
    int focused = g->focus == w->hwnd;
    int disabled = !w->enabled;
    if (BS_TYPE(w) == BS_OWNERDRAW) {
        uint64_t hdc = gdi_create_dc(DC_WINDOW, w->hwnd);
        w->invalid = 0; w->erase = 0;
        owner_draw(st, w, hdc, 4, 0, (w->pressed ? 1 : 0) | (focused ? 0x10 : 0) | (disabled ? 4 : 0), 0, 0, w->w, w->h, 0);
        gdi_release_dc(hdc);
        return;
    }
    if (is_push(w)) {
        Paint p = ctl_begin(st, w, WM_CTLCOLORBTN);
        int def = BS_TYPE(w) == BS_DEFPUSH || (w->parent && win_get(w->parent) && win_get(w->parent)->def_id == w->id && w->id);
        int down = w->pressed || ((w->style & 0x1000) && w->checked);
        uint32_t fill = disabled ? 0xCCCCCC : down ? 0xF7E4CC : w->hot ? 0xFBF1E5 : 0xE1E1E1;
        uint32_t border = disabled ? 0xBFBFBF : down ? 0x995400 : (def || focused) ? 0xD77800 : 0xADADAD;
        gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, fill);
        gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, border);
        if ((def || focused) && !disabled && !down) gdi_frame_rect_px(p.hdc, 1, 1, w->w - 1, w->h - 1, border);
        set_text_color(p.hdc, disabled ? 0x838383 : 0x000000);
        if (w->image) {
            GdiObj *b = gdi_get(w->image, GO_BITMAP);
            if (b) {
                uint64_t mdc = gdi_create_dc(DC_MEMORY, 0);
                Dc *m = dc_get(mdc);
                m->bitmap = w->image;
                (void)m;
                gdi_release_dc(mdc);
            }
        }
        uint32_t fmt = 0x20 | 4 | 1; // single line, vcenter, center
        if ((w->style & 0x300) == 0x100) fmt = (fmt & ~1u) | 0;
        else if ((w->style & 0x300) == 0x200) fmt = (fmt & ~1u) | 2;
        if (w->style & 0x2000) fmt = 0x10 | 1; // BS_MULTILINE
        text_in_rect(p.hdc, w->text, 4, 2 + (down ? 1 : 0), w->w - 4, w->h - 2 + (down ? 1 : 0), fmt);
        ctl_end(&p);
        return;
    }
    if (BS_TYPE(w) == BS_GROUP) {
        Paint p = ctl_begin(st, w, WM_CTLCOLORSTATIC);
        int lh = line_height(w);
        int tw = gdi_text_width(font_of(w), w->text, (int)strlen(w->text), NULL);
        if (p.has_bg) gdi_fill_rect_px(p.hdc, 0, 0, w->w, lh, p.bg);
        int y = lh / 2;
        gdi_frame_rect_px(p.hdc, 0, y, w->w, w->h, 0xDCDCDC);
        if (w->text[0]) {
            gdi_fill_rect_px(p.hdc, 6, 0, 6 + tw + 6, lh, p.bg);
            set_text_color(p.hdc, disabled ? 0x838383 : p.fg);
            gdi_text_out_utf8(p.hdc, 9, 0, w->text, -1);
        }
        ctl_end(&p);
        return;
    }
    // check box / radio button
    Paint p = ctl_begin(st, w, WM_CTLCOLORSTATIC);
    if (p.has_bg) gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, p.bg);
    int radio = BS_TYPE(w) == BS_RADIO || BS_TYPE(w) == BS_AUTORADIO;
    int box = 13;
    int left_text = (w->style & 0x20) != 0;
    int bx = left_text ? w->w - box - 1 : 1, by = (w->h - box) / 2;
    uint32_t border = disabled ? 0xBCBCBC : (w->pressed || w->hot) ? 0xD77800 : 0x333333;
    uint32_t inner = disabled ? 0xF0F0F0 : w->pressed ? 0xF7E4CC : 0xFFFFFF;
    if (radio) {
        gdi_fill_ellipse_px(p.hdc, bx, by, bx + box, by + box, inner, border);
        if (w->checked) gdi_fill_ellipse_px(p.hdc, bx + 3, by + 3, bx + box - 3, by + box - 3, disabled ? 0xBCBCBC : 0x333333, disabled ? 0xBCBCBC : 0x333333);
    } else {
        gdi_fill_rect_px(p.hdc, bx, by, bx + box, by + box, inner);
        gdi_frame_rect_px(p.hdc, bx, by, bx + box, by + box, border);
        uint32_t mark = disabled ? 0xA0A0A0 : 0x333333;
        if (w->checked == 1) {
            gdi_line_px(p.hdc, bx + 3, by + 6, bx + 5, by + 9, mark, 2);
            gdi_line_px(p.hdc, bx + 5, by + 9, bx + 10, by + 3, mark, 2);
        } else if (w->checked == 2) {
            gdi_fill_rect_px(p.hdc, bx + 3, by + 3, bx + box - 3, by + box - 3, mark);
        }
    }
    set_text_color(p.hdc, disabled ? 0x838383 : p.fg);
    int tx0 = left_text ? 0 : box + 6, tx1 = left_text ? w->w - box - 6 : w->w;
    text_in_rect(p.hdc, w->text, tx0, 0, tx1, w->h, (w->style & 0x2000) ? 0x10 | 4 : 0x20 | 4);
    if (gui()->focus == w->hwnd && w->text[0]) {
        int tw = gdi_text_width(font_of(w), w->text, (int)strlen(w->text), NULL);
        int lh = line_height(w);
        int ty = (w->h - lh) / 2;
        for (int x = tx0 - 1; x < tx0 + tw + 1 && x < w->w; x += 2) { gdi_fill_rect_px(p.hdc, x, ty, x + 1, ty + 1, 0x707070); gdi_fill_rect_px(p.hdc, x, ty + lh - 1, x + 1, ty + lh, 0x707070); }
    }
    ctl_end(&p);
}

// siblings of w that belong to the same WS_GROUP range
static int group_of(Win *w, Win ***out) {
    Win *parent = win_get(w->parent);
    *out = NULL;
    if (!parent) return 0;
    GuiState *g = gui();
    Win *list[WIN_MAX];
    int n = 0;
    for (int i = 0; i < WIN_MAX; i++) if (g->wins[i].used && g->wins[i].parent == w->parent) list[n++] = &g->wins[i];
    for (int a = 1; a < n; a++) for (int b = a; b > 0 && list[b - 1]->seq > list[b]->seq; b--) { Win *t = list[b]; list[b] = list[b - 1]; list[b - 1] = t; }
    int idx = 0;
    for (int i = 0; i < n; i++) if (list[i] == w) idx = i;
    int start = idx;
    while (start > 0 && !(list[start]->style & WS_GROUP)) start--;
    int end = idx + 1;
    while (end < n && !(list[end]->style & WS_GROUP)) end++;
    Win **r = malloc(sizeof(Win *) * (size_t)(end - start + 1));
    int m = 0;
    for (int i = start; i < end; i++) r[m++] = list[i];
    *out = r;
    return m;
}

static void button_click(X86State *st, Win *w) {
    uint64_t hwnd = w->hwnd;
    int t = BS_TYPE(w);
    if (t == BS_AUTOCHECK) w->checked = !w->checked;
    else if (t == BS_AUTO3STATE) w->checked = (w->checked + 1) % 3;
    else if (t == BS_AUTORADIO) {
        Win **grp;
        int n = group_of(w, &grp);
        for (int i = 0; i < n; i++) if (grp[i] != w && grp[i]->ctl == CTL_BUTTON && BS_TYPE(grp[i]) == BS_AUTORADIO && grp[i]->checked) { grp[i]->checked = 0; win_invalidate_rect(grp[i], NULL, 1); }
        free(grp);
        w->checked = 1;
    }
    win_invalidate_rect(w, NULL, 1);
    if (win_get(hwnd)) win_notify_parent(st, w, 0); // BN_CLICKED
}

static uint64_t button_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    GuiState *g = gui();
    switch (msg) {
    case WM_PAINT: button_paint(st, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (BS_TYPE(w) == BS_GROUP) return 0;
        w->pressed = 1; w->tracking = 1;
        g->capture = w->hwnd;
        if (g->focus != w->hwnd) win_set_focus(st, w->hwnd);
        win_invalidate_rect(w, NULL, 1);
        return 0;
    case WM_MOUSEMOVE:
        if (w->tracking) {
            int x = (int16_t)LOWORD(lp), y = (int16_t)HIWORD(lp);
            int in = x >= 0 && y >= 0 && x < w->w && y < w->h;
            if (in != w->pressed) { w->pressed = in; win_invalidate_rect(w, NULL, 1); }
        }
        return 0;
    case WM_LBUTTONUP:
        if (!w->tracking) return 0;
        w->tracking = 0;
        if (g->capture == w->hwnd) g->capture = 0;
        if (w->pressed) { w->pressed = 0; button_click(st, w); }
        else win_invalidate_rect(w, NULL, 1);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && !w->pressed) { w->pressed = 1; win_invalidate_rect(w, NULL, 1); }
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE && w->pressed) { w->pressed = 0; button_click(st, w); }
        return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        if (msg == WM_KILLFOCUS) w->pressed = 0;
        win_invalidate_rect(w, NULL, 1);
        return 0;
    case WM_GETDLGCODE: {
        int t = BS_TYPE(w);
        uint64_t r = 0x2000; // DLGC_BUTTON
        if (t == BS_DEFPUSH) r |= 0x10; else if (is_push(w)) r |= 0x20;
        if (t == BS_RADIO || t == BS_AUTORADIO) r |= 0x40;
        return r;
    }
    case 0xF0: return (uint64_t)w->checked;                               // BM_GETCHECK
    case 0xF1: w->checked = (int)wp; win_invalidate_rect(w, NULL, 1); return 0; // BM_SETCHECK
    case 0xF2: return (uint64_t)(w->checked | (w->pressed ? 4 : 0) | (g->focus == w->hwnd ? 8 : 0)); // BM_GETSTATE
    case 0xF3: w->pressed = wp != 0; win_invalidate_rect(w, NULL, 1); return 0;
    case 0xF4: w->style = (w->style & ~0xFFFFu) | (uint32_t)(wp & 0xFFFF); if (lp) win_invalidate_rect(w, NULL, 1); return 0; // BM_SETSTYLE
    case 0xF5: button_click(st, w); return 0;                             // BM_CLICK
    case 0xF6: return w->image;                                           // BM_GETIMAGE
    case 0xF7: { uint64_t o = w->image; w->image = lp; win_invalidate_rect(w, NULL, 1); return o; }
    case WM_SETTEXT: { uint64_t r = win_def_proc(st, w, msg, wp, lp, unicode); win_invalidate_rect(w, NULL, 1); return r; }
    case WM_ENABLE: win_invalidate_rect(w, NULL, 1); return 0;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- Static ------------------------------------------------------------------------------------

static void static_paint(X86State *st, Win *w) {
    int type = (int)(w->style & 0x1F);
    if (type == 0xD) { // SS_OWNERDRAW
        uint64_t hdc = gdi_create_dc(DC_WINDOW, w->hwnd);
        w->invalid = 0; w->erase = 0;
        owner_draw(st, w, hdc, 4 /* ODT_STATIC = 5 */ + 1, 0, w->enabled ? 0 : 4, 0, 0, w->w, w->h, 0);
        gdi_release_dc(hdc);
        return;
    }
    Paint p = ctl_begin(st, w, WM_CTLCOLORSTATIC);
    switch (type) {
    case 4: gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, 0x000000); break;
    case 5: gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, 0x808080); break;
    case 6: gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, 0xFFFFFF); break;
    case 7: gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, 0x000000); break;
    case 8: gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, 0x808080); break;
    case 9: gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, 0xFFFFFF); break;
    case 0x10: gdi_fill_rect_px(p.hdc, 0, 0, w->w, 1, 0xA0A0A0); gdi_fill_rect_px(p.hdc, 0, 1, w->w, 2, 0xFFFFFF); break;
    case 0x11: gdi_fill_rect_px(p.hdc, 0, 0, 1, w->h, 0xA0A0A0); gdi_fill_rect_px(p.hdc, 1, 0, 2, w->h, 0xFFFFFF); break;
    case 0x12: gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, 0xA0A0A0); gdi_frame_rect_px(p.hdc, 1, 1, w->w, w->h, 0xFFFFFF); break;
    case 3: case 0xE: { // SS_ICON / SS_BITMAP
        if (p.has_bg) gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, p.bg);
        GdiObj *b = gdi_get(w->image, GO_BITMAP);
        if (b) {
            Win *t = win_top(w);
            int x0, y0, x1, y1;
            if (t && t->pixels && win_visible_rect(w, &x0, &y0, &x1, &y1)) {
                int ox, oy;
                win_client_origin(w, &ox, &oy);
                for (int y = 0; y < b->surf.h; y++) for (int x = 0; x < b->surf.w; x++) {
                    int px = ox + x, py = oy + y;
                    if (px < x0 || py < y0 || px >= x1 || py >= y1) continue;
                    t->pixels[(size_t)py * t->fb_w + px] = surf_get(&b->surf, x, y);
                }
                win_touch(t);
            }
        }
        break;
    }
    default: {
        if (p.has_bg) gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, p.bg);
        uint32_t fmt = (type == 1 ? 1 : type == 2 ? 2 : 0) | ((type == 0xB || type == 0xC) ? 0x20 : 0x10);
        if (w->style & 0x80) fmt |= 0x800;              // SS_NOPREFIX
        if (w->style & 0x200) fmt = (fmt & ~0x10u) | 0x20 | 4; // SS_CENTERIMAGE: single line, vertically centered
        set_text_color(p.hdc, w->enabled ? p.fg : 0x6D6D6D);
        text_in_rect(p.hdc, w->text, 0, 0, w->w, w->h, fmt);
        break;
    }
    }
    if (w->style & 0x1000) { gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, 0xA0A0A0); } // SS_SUNKEN
    ctl_end(&p);
}

static uint64_t static_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    switch (msg) {
    case WM_NCCREATE:
        if (((w->style & 0x1F) == 0xE || (w->style & 0x1F) == 3) && lp) {
            // the window name of an image static may be a resource id (0xFFFF, id)
            uint64_t name; memcpy(&name, (uint8_t *)(uintptr_t)lp + 56, 8);
            if (name >= 0x10000 && unicode && ((uint16_t *)(uintptr_t)name)[0] == 0xFFFF) name = ((uint16_t *)(uintptr_t)name)[1];
            if (name && name < 0x10000 && (w->style & 0x1F) == 0xE) {
                const void *r = pe_resource(w->hinst, 2, name, 1, NULL);
                if (r) w->image = gdi_bitmap_from_dib(r, NULL);
                win_set_text(w, "");
            }
        }
        return 1;
    case WM_PAINT: static_paint(st, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
        if (w->style & 0x100) win_notify_parent(st, w, 0); // STN_CLICKED
        return 0;
    case WM_LBUTTONDBLCLK:
        if (w->style & 0x100) win_notify_parent(st, w, 1); // STN_DBLCLK
        return 0;
    case 0x172: case 0x170: { uint64_t o = w->image; w->image = lp ? lp : wp; win_invalidate_rect(w, NULL, 1); return o; } // STM_SETIMAGE / STM_SETICON
    case 0x173: case 0x171: return w->image;
    case WM_SETTEXT: {
        uint64_t r = win_def_proc(st, w, msg, wp, lp, unicode);
        Win *p = win_get(w->parent);
        int32_t rc[4] = { w->x, w->y, w->x + w->w, w->y + w->h };
        if (p) win_invalidate_rect(p, rc, 1); else win_invalidate_rect(w, NULL, 1);
        return r;
    }
    case WM_GETDLGCODE: return 0x100; // DLGC_STATIC
    case WM_ENABLE: win_invalidate_rect(w, NULL, 1); return 0;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- Edit --------------------------------------------------------------------------------------

#define ES_CENTER 1
#define ES_RIGHT 2
#define ES_MULTILINE 4
#define ES_UPPERCASE 8
#define ES_LOWERCASE 0x10
#define ES_PASSWORD 0x20
#define ES_AUTOHSCROLL 0x80
#define ES_READONLY 0x800
#define ES_WANTRETURN 0x1000
#define ES_NUMBER 0x2000

typedef struct Cps { uint32_t *c; int n; } Cps;

static Cps to_cps(const char *s) {
    Cps r;
    size_t len = strlen(s);
    r.c = malloc(sizeof(uint32_t) * (len + 1));
    r.n = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((uint32_t)(p[0] & 63) << 12) | ((uint32_t)(p[1] & 63) << 6) | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 15) << 12) | ((uint32_t)(p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 31) << 6) | (p[0] & 63); p += 1; }
        r.c[r.n++] = c;
    }
    return r;
}
static char *from_cps(const uint32_t *c, int n) {
    char *s = malloc((size_t)n * 4 + 1), *o = s;
    for (int i = 0; i < n; i++) {
        uint32_t v = c[i];
        if (v < 0x80) *o++ = (char)v;
        else if (v < 0x800) { *o++ = (char)(0xC0 | (v >> 6)); *o++ = (char)(0x80 | (v & 63)); }
        else if (v < 0x10000) { *o++ = (char)(0xE0 | (v >> 12)); *o++ = (char)(0x80 | ((v >> 6) & 63)); *o++ = (char)(0x80 | (v & 63)); }
        else { *o++ = (char)(0xF0 | (v >> 18)); *o++ = (char)(0x80 | ((v >> 12) & 63)); *o++ = (char)(0x80 | ((v >> 6) & 63)); *o++ = (char)(0x80 | (v & 63)); }
    }
    *o = 0;
    return s;
}
static int cp_width(Win *w, uint32_t c) {
    if (w->password_char || (w->style & ES_PASSWORD)) c = w->password_char ? w->password_char : 0x25CF;
    char u[8];
    int n;
    if (c < 0x80) { u[0] = (char)c; n = 1; }
    else { char *t = from_cps(&c, 1); n = (int)strlen(t); memcpy(u, t, (size_t)n); free(t); }
    if (c == 0x25CF) return gdi_text_width(font_of(w), "*", 1, NULL) + 1;
    return gdi_text_width(font_of(w), u, n, NULL);
}

typedef struct Line { int start, end; } Line; // [start, end) code point indices, excluding the line break

static int edit_lines(Win *w, Cps *t, int avail, Line **out) {
    int cap = 16, n = 0;
    Line *ls = malloc(sizeof(Line) * (size_t)cap);
    int multi = (w->style & ES_MULTILINE) != 0;
    int wrap = multi && !(w->style & ES_AUTOHSCROLL) && !(w->style & WS_HSCROLL);
    int i = 0;
    for (;;) {
        int start = i, x = 0, last_space = -1;
        while (i < t->n) {
            uint32_t c = t->c[i];
            if (multi && (c == '\n' || (c == '\r' && i + 1 < t->n && t->c[i + 1] == '\n'))) break;
            if (multi && c == '\r') { i++; continue; }
            int cw = cp_width(w, c);
            if (wrap && x + cw > avail && i > start) {
                if (last_space >= start) i = last_space + 1;
                break;
            }
            if (c == ' ') last_space = i;
            x += cw;
            i++;
        }
        if (n == cap) { cap *= 2; ls = realloc(ls, sizeof(Line) * (size_t)cap); }
        int end = i;
        ls[n].start = start; ls[n].end = end; n++;
        if (i >= t->n) break;
        if (t->c[i] == '\r') i++;
        if (i < t->n && t->c[i] == '\n') { i++; if (i >= t->n) { if (n == cap) { cap *= 2; ls = realloc(ls, sizeof(Line) * (size_t)cap); } ls[n].start = ls[n].end = i; n++; break; } }
        else if (!multi) break;
    }
    *out = ls;
    return n;
}

static int edit_border(Win *w) { return (w->style & WS_BORDER) || (w->exstyle & WS_EX_CLIENTEDGE) ? 1 : 0; }
static void edit_area(Win *w, int *x0, int *y0, int *x1, int *y1) {
    int b = edit_border(w);
    *x0 = b ? 4 : 1; *x1 = w->w - (b ? 4 : 1);
    *y0 = b ? 3 : 0; *y1 = w->h - (b ? 3 : 0);
}
static int line_x(Win *w, Cps *t, Line *l, int idx) {
    int x = 0;
    for (int i = l->start; i < idx && i < l->end; i++) if (t->c[i] != '\r') x += cp_width(w, t->c[i]);
    return x;
}
static int line_of(Line *ls, int n, int idx) {
    for (int i = 0; i < n; i++) if (idx <= ls[i].end && (i + 1 >= n || idx < ls[i + 1].start)) return i;
    return n - 1;
}
static int align_offset(Win *w, int avail, int lw) {
    if (w->style & ES_CENTER) return (avail - lw) / 2;
    if (w->style & ES_RIGHT) return avail - lw;
    return 0;
}

static void edit_paint(X86State *st, Win *w) {
    GuiState *g = gui();
    int focused = g->focus == w->hwnd;
    int ro = w->readonly || !w->enabled;
    Paint p = ctl_begin(st, w, ro ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT);
    uint32_t bg = p.has_bg ? p.bg : 0xFFFFFF;
    if (!w->enabled) bg = 0xF0F0F0;
    gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, bg);
    if (edit_border(w)) gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, focused ? 0xD77800 : 0x7A7A7A);
    int ax0, ay0, ax1, ay1;
    edit_area(w, &ax0, &ay0, &ax1, &ay1);
    Dc *dc = dc_get(p.hdc);
    dc->clip_set = 1; dc->cx0 = ax0; dc->cy0 = ay0; dc->cx1 = ax1; dc->cy1 = ay1;
    Cps t = to_cps(w->text);
    Line *ls;
    int n = edit_lines(w, &t, ax1 - ax0, &ls);
    int lh = line_height(w);
    int multi = (w->style & ES_MULTILINE) != 0;
    int y = multi ? ay0 - w->scroll_y * lh : ay0 + ((ay1 - ay0) - lh) / 2;
    int s0 = w->sel_start < w->sel_end ? w->sel_start : w->sel_end, s1 = w->sel_start < w->sel_end ? w->sel_end : w->sel_start;
    uint32_t fg = w->enabled ? p.fg : 0x6D6D6D;
    int pw = (w->style & ES_PASSWORD) || w->password_char;
    for (int li = 0; li < n; li++, y += lh) {
        if (y + lh < ay0 || y >= ay1) continue;
        Line *l = &ls[li];
        int lw = line_x(w, &t, l, l->end);
        int x = ax0 + align_offset(w, ax1 - ax0, lw) - (multi ? 0 : w->scroll_x);
        for (int i = l->start; i < l->end; i++) {
            uint32_t c = t.c[i];
            if (c == '\r') continue;
            int cw = cp_width(w, c);
            int sel = (focused || (w->style & 0x100)) && i >= s0 && i < s1;
            if (sel) gdi_fill_rect_px(p.hdc, x, y, x + cw, y + lh, gui_sys_color(COLOR_HIGHLIGHT));
            char *u = pw ? strdup("*") : from_cps(&c, 1);
            if (pw) { gdi_fill_ellipse_px(p.hdc, x + cw / 2 - 3, y + lh / 2 - 3, x + cw / 2 + 3, y + lh / 2 + 3, sel ? 0xFFFFFF : fg, sel ? 0xFFFFFF : fg); }
            else { set_text_color(p.hdc, sel ? 0xFFFFFF : fg); gdi_text_out_utf8(p.hdc, x, y, u, -1); }
            free(u);
            x += cw;
        }
    }
    if (focused) {
        int cl = line_of(ls, n, w->sel_end);
        Line *l = &ls[cl];
        int lw = line_x(w, &t, l, l->end);
        int cx = ax0 + align_offset(w, ax1 - ax0, lw) + line_x(w, &t, l, w->sel_end) - (multi ? 0 : w->scroll_x);
        int cy = multi ? ay0 + (cl - w->scroll_y) * lh : ay0 + ((ay1 - ay0) - lh) / 2;
        int visible = cx >= ax0 - 1 && cx <= ax1 && cy + lh > ay0 && cy < ay1 && !w->readonly;
        gui_caret_set(w->hwnd, cx, cy, 1, lh, visible);
    }
    free(ls);
    free(t.c);
    ctl_end(&p);
}

static void edit_scroll_to_caret(Win *w) {
    int ax0, ay0, ax1, ay1;
    edit_area(w, &ax0, &ay0, &ax1, &ay1);
    Cps t = to_cps(w->text);
    Line *ls;
    int n = edit_lines(w, &t, ax1 - ax0, &ls);
    int cl = line_of(ls, n, w->sel_end);
    if (w->style & ES_MULTILINE) {
        int lh = line_height(w), vis = (ay1 - ay0) / (lh ? lh : 1);
        if (vis < 1) vis = 1;
        if (cl < w->scroll_y) w->scroll_y = cl;
        if (cl >= w->scroll_y + vis) w->scroll_y = cl - vis + 1;
    } else {
        int cx = line_x(w, &t, &ls[cl], w->sel_end), avail = ax1 - ax0;
        if (cx - w->scroll_x > avail - 2) w->scroll_x = cx - avail + 2;
        if (cx < w->scroll_x) w->scroll_x = cx > 8 ? cx - 8 : 0;
        int total = line_x(w, &t, &ls[cl], ls[cl].end);
        if (total < avail) w->scroll_x = 0;
    }
    free(ls);
    free(t.c);
}

static void edit_changed(X86State *st, Win *w) {
    uint64_t h = w->hwnd;
    w->modified = 1;
    edit_scroll_to_caret(w);
    win_invalidate_rect(w, NULL, 1);
    win_notify_parent(st, w, 0x400); // EN_UPDATE
    if ((w = win_get(h))) win_notify_parent(st, w, 0x300); // EN_CHANGE
}

// Replaces the selection with text (code points). Returns 0 if nothing changed.
static int edit_replace(Win *w, const uint32_t *ins, int nins) {
    Cps t = to_cps(w->text);
    int s0 = w->sel_start < w->sel_end ? w->sel_start : w->sel_end, s1 = w->sel_start < w->sel_end ? w->sel_end : w->sel_start;
    if (s0 < 0) s0 = 0;
    if (s1 > t.n) s1 = t.n;
    if (s0 > s1) s0 = s1;
    int room = w->limit - (t.n - (s1 - s0));
    if (nins > room) nins = room > 0 ? room : 0;
    if (nins == 0 && s0 == s1) { free(t.c); return 0; }
    uint32_t *nc = malloc(sizeof(uint32_t) * (size_t)(t.n - (s1 - s0) + nins + 1));
    memcpy(nc, t.c, sizeof(uint32_t) * (size_t)s0);
    memcpy(nc + s0, ins, sizeof(uint32_t) * (size_t)nins);
    memcpy(nc + s0 + nins, t.c + s1, sizeof(uint32_t) * (size_t)(t.n - s1));
    char *s = from_cps(nc, t.n - (s1 - s0) + nins);
    win_set_text(w, s);
    free(s); free(nc); free(t.c);
    w->sel_start = w->sel_end = s0 + nins;
    return 1;
}
static void edit_replace_utf8(Win *w, const char *s, int filter) {
    Cps in = to_cps(s);
    int m = 0;
    for (int i = 0; i < in.n; i++) {
        uint32_t c = in.c[i];
        if (filter && !(w->style & ES_MULTILINE) && (c == '\r' || c == '\n')) continue;
        if ((w->style & ES_NUMBER) && filter && !(c >= '0' && c <= '9')) continue;
        if (w->style & ES_UPPERCASE) c = (uint32_t)toupper((int)(c & 0x7F)) | (c & ~0x7Fu);
        in.c[m++] = c;
    }
    edit_replace(w, in.c, m);
    free(in.c);
}
static char *edit_selection(Win *w) {
    Cps t = to_cps(w->text);
    int s0 = w->sel_start < w->sel_end ? w->sel_start : w->sel_end, s1 = w->sel_start < w->sel_end ? w->sel_end : w->sel_start;
    if (s1 > t.n) s1 = t.n;
    if (s0 > s1) s0 = s1;
    char *r = from_cps(t.c + s0, s1 - s0);
    free(t.c);
    return r;
}
static int edit_hit(Win *w, int x, int y) {
    int ax0, ay0, ax1, ay1;
    edit_area(w, &ax0, &ay0, &ax1, &ay1);
    Cps t = to_cps(w->text);
    Line *ls;
    int n = edit_lines(w, &t, ax1 - ax0, &ls);
    int lh = line_height(w);
    int li = (w->style & ES_MULTILINE) ? (y - ay0) / (lh ? lh : 1) + w->scroll_y : 0;
    if (li < 0) li = 0;
    if (li >= n) li = n - 1;
    Line *l = &ls[li];
    int lw = line_x(w, &t, l, l->end);
    int px = x - ax0 - align_offset(w, ax1 - ax0, lw) + ((w->style & ES_MULTILINE) ? 0 : w->scroll_x);
    int idx = l->start, cx = 0;
    while (idx < l->end) {
        int cw = cp_width(w, t.c[idx]);
        if (px < cx + cw / 2) break;
        cx += cw;
        idx++;
    }
    free(ls);
    free(t.c);
    return idx;
}
static int text_len_cps(Win *w) { Cps t = to_cps(w->text); int n = t.n; free(t.c); return n; }

static void edit_move(Win *w, int vk, int shift, int ctrl) {
    Cps t = to_cps(w->text);
    int ax0, ay0, ax1, ay1;
    edit_area(w, &ax0, &ay0, &ax1, &ay1);
    Line *ls;
    int n = edit_lines(w, &t, ax1 - ax0, &ls);
    int pos = w->sel_end, cl = line_of(ls, n, pos);
    switch (vk) {
    case VK_LEFT:
        if (!shift && w->sel_start != w->sel_end) pos = w->sel_start < w->sel_end ? w->sel_start : w->sel_end;
        else if (pos > 0) { pos--; if (pos > 0 && t.c[pos] == '\n' && t.c[pos - 1] == '\r') pos--; if (ctrl) { while (pos > 0 && t.c[pos - 1] == ' ') pos--; while (pos > 0 && t.c[pos - 1] != ' ' && t.c[pos - 1] != '\n') pos--; } }
        break;
    case VK_RIGHT:
        if (!shift && w->sel_start != w->sel_end) pos = w->sel_start > w->sel_end ? w->sel_start : w->sel_end;
        else if (pos < t.n) { if (t.c[pos] == '\r' && pos + 1 < t.n && t.c[pos + 1] == '\n') pos++; pos++; if (ctrl) { while (pos < t.n && t.c[pos] != ' ' && t.c[pos] != '\r') pos++; while (pos < t.n && t.c[pos] == ' ') pos++; } }
        break;
    case VK_HOME: pos = ctrl ? 0 : ls[cl].start; break;
    case VK_END: pos = ctrl ? t.n : ls[cl].end; break;
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: {
        int step = (vk == VK_PRIOR || vk == VK_NEXT) ? 10 : 1;
        int target = (vk == VK_UP || vk == VK_PRIOR) ? cl - step : cl + step;
        if (target < 0) target = 0;
        if (target >= n) target = n - 1;
        int x = line_x(w, &t, &ls[cl], pos);
        int idx = ls[target].start, cx = 0;
        while (idx < ls[target].end) { int cw = cp_width(w, t.c[idx]); if (x < cx + cw / 2) break; cx += cw; idx++; }
        pos = idx;
        break;
    }
    default: break;
    }
    w->sel_end = pos;
    if (!shift) w->sel_start = pos;
    free(ls);
    free(t.c);
}

static void clip_set_text(const char *s) {
    GuiState *g = gui();
    free(g->clip_text);
    g->clip_text = strdup(s);
}

static uint64_t edit_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    GuiState *g = gui();
    uint64_t h = w->hwnd;
    switch (msg) {
    case WM_NCCREATE:
        w->readonly = (w->style & ES_READONLY) != 0;
        if (w->style & ES_PASSWORD) w->password_char = 0x25CF;
        w->sel_start = w->sel_end = 0;
        return 1;
    case WM_PAINT: edit_paint(st, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS:
        win_invalidate_rect(w, NULL, 1);
        win_notify_parent(st, w, 0x100); // EN_SETFOCUS
        return 0;
    case WM_KILLFOCUS:
        if (g->caret_hwnd == h) gui_caret_set(0, 0, 0, 0, 0, 0);
        win_invalidate_rect(w, NULL, 1);
        win_notify_parent(st, w, 0x200); // EN_KILLFOCUS
        return 0;
    case WM_LBUTTONDOWN: {
        if (g->focus != h) win_set_focus(st, h);
        if (!(w = win_get(h))) return 0;
        int idx = edit_hit(w, (int16_t)LOWORD(lp), (int16_t)HIWORD(lp));
        w->sel_end = idx;
        if (!(g->keys[VK_SHIFT] & 0x80)) w->sel_start = idx;
        w->tracking = 1;
        g->capture = h;
        win_invalidate_rect(w, NULL, 1);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (w->tracking) {
            int idx = edit_hit(w, (int16_t)LOWORD(lp), (int16_t)HIWORD(lp));
            if (idx != w->sel_end) { w->sel_end = idx; edit_scroll_to_caret(w); win_invalidate_rect(w, NULL, 1); }
        }
        return 0;
    case WM_LBUTTONUP:
        w->tracking = 0;
        if (g->capture == h) g->capture = 0;
        return 0;
    case WM_LBUTTONDBLCLK: {
        Cps t = to_cps(w->text);
        int i = edit_hit(w, (int16_t)LOWORD(lp), (int16_t)HIWORD(lp)), a = i, b = i;
        while (a > 0 && t.c[a - 1] != ' ' && t.c[a - 1] != '\n') a--;
        while (b < t.n && t.c[b] != ' ' && t.c[b] != '\r' && t.c[b] != '\n') b++;
        w->sel_start = a; w->sel_end = b;
        free(t.c);
        win_invalidate_rect(w, NULL, 1);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (w->style & ES_MULTILINE) {
            int d = (int16_t)HIWORD(wp) / 40;
            w->scroll_y -= d;
            if (w->scroll_y < 0) w->scroll_y = 0;
            win_invalidate_rect(w, NULL, 1);
            return 0;
        }
        return win_def_proc(st, w, msg, wp, lp, unicode);
    case WM_KEYDOWN: {
        int shift = (g->keys[VK_SHIFT] & 0x80) != 0, ctrl = (g->keys[VK_CONTROL] & 0x80) != 0;
        switch (wp) {
        case VK_LEFT: case VK_RIGHT: case VK_HOME: case VK_END: case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT:
            if (!(w->style & ES_MULTILINE) && (wp == VK_UP || wp == VK_DOWN || wp == VK_PRIOR || wp == VK_NEXT)) return 0;
            edit_move(w, (int)wp, shift, ctrl);
            edit_scroll_to_caret(w);
            win_invalidate_rect(w, NULL, 1);
            return 0;
        case VK_DELETE:
            if (w->readonly) return 0;
            if (w->sel_start == w->sel_end) { int n = text_len_cps(w); if (w->sel_end >= n) return 0; w->sel_end++; Cps t = to_cps(w->text); if (w->sel_end < t.n && t.c[w->sel_end - 1] == '\r' && t.c[w->sel_end] == '\n') w->sel_end++; free(t.c); }
            if (edit_replace(w, NULL, 0)) edit_changed(st, w);
            return 0;
        default: return 0;
        }
    }
    case WM_CHAR: {
        uint32_t c = (uint32_t)wp;
        if (!unicode && c >= 0x80) return 0; // UTF-8 bytes of ANSI windows: handled through the UI's UTF-16 path
        if (c == 1) { w->sel_start = 0; w->sel_end = text_len_cps(w); win_invalidate_rect(w, NULL, 1); return 0; } // Ctrl+A
        if (c == 3 || c == 24) { // Ctrl+C / Ctrl+X
            char *s = edit_selection(w);
            if (s[0] && !(w->style & ES_PASSWORD)) clip_set_text(s);
            free(s);
            if (c == 24 && !w->readonly && edit_replace(w, NULL, 0)) edit_changed(st, w);
            return 0;
        }
        if (c == 22) { if (!w->readonly && g->clip_text) { edit_replace_utf8(w, g->clip_text, 1); edit_changed(st, w); } return 0; } // Ctrl+V
        if (w->readonly) return 0;
        if (c == 8) {
            if (w->sel_start == w->sel_end) {
                if (w->sel_end == 0) return 0;
                w->sel_start = w->sel_end - 1;
                Cps t = to_cps(w->text);
                if (w->sel_start > 0 && t.c[w->sel_start] == '\n' && t.c[w->sel_start - 1] == '\r') w->sel_start--;
                free(t.c);
            }
            if (edit_replace(w, NULL, 0)) edit_changed(st, w);
            return 0;
        }
        if (c == '\r' || c == '\n') {
            if (!(w->style & ES_MULTILINE)) return 0;
            uint32_t crlf[2] = { '\r', '\n' };
            if (edit_replace(w, crlf, 2)) edit_changed(st, w);
            return 0;
        }
        if (c == '\t' && !(w->style & ES_MULTILINE)) return 0;
        if (c < 32 && c != '\t') return 0;
        if ((w->style & ES_NUMBER) && !(c >= '0' && c <= '9')) return 0;
        if (c >= 0xD800 && c < 0xDC00) return 0; // surrogate pairs are not supported by the editor
        if (w->style & ES_UPPERCASE) { if (c < 128) c = (uint32_t)toupper((int)c); else if (c == 0x131) c = 'I'; else if (c == 'i') c = 0x130; }
        if (w->style & ES_LOWERCASE) { if (c < 128) c = (uint32_t)tolower((int)c); }
        if (edit_replace(w, &c, 1)) edit_changed(st, w);
        return 0;
    }
    case WM_SETTEXT: {
        char *s = lp ? gstr(lp, unicode) : strdup("");
        win_set_text(w, s ? s : "");
        free(s);
        w->sel_start = w->sel_end = 0;
        w->scroll_x = w->scroll_y = 0;
        w->modified = 0;
        win_invalidate_rect(w, NULL, 1);
        win_notify_parent(st, w, 0x400);
        if ((w = win_get(h))) win_notify_parent(st, w, 0x300);
        return 1;
    }
    case WM_GETDLGCODE: return 0x80 | 0x8 | 0x1 | ((w->style & ES_MULTILINE) ? 0x4 : 0);
    case WM_SETFONT: w->font = wp; win_invalidate_rect(w, NULL, 1); return 0;
    case WM_ENABLE: win_invalidate_rect(w, NULL, 1); return 0;
    case 0x300: { char *s = edit_selection(w); if (s[0]) clip_set_text(s); free(s); if (!w->readonly && edit_replace(w, NULL, 0)) edit_changed(st, w); return 0; } // WM_CUT
    case 0x301: { char *s = edit_selection(w); if (s[0]) clip_set_text(s); free(s); return 0; }       // WM_COPY
    case 0x302: if (!w->readonly && g->clip_text) { edit_replace_utf8(w, g->clip_text, 1); edit_changed(st, w); } return 0; // WM_PASTE
    case 0x303: if (!w->readonly && edit_replace(w, NULL, 0)) edit_changed(st, w); return 0;         // WM_CLEAR
    case 0xB0: { // EM_GETSEL
        int s0 = w->sel_start < w->sel_end ? w->sel_start : w->sel_end, s1 = w->sel_start < w->sel_end ? w->sel_end : w->sel_start;
        if (wp) mem_wr(wp, 4, (uint32_t)s0);
        if (lp) mem_wr(lp, 4, (uint32_t)s1);
        return ((uint32_t)(s1 > 0xFFFF ? 0xFFFF : s1) << 16) | (uint32_t)(s0 > 0xFFFF ? 0xFFFF : s0);
    }
    case 0xB1: { // EM_SETSEL
        int n = text_len_cps(w);
        int a = (int)(int32_t)wp, b = (int)(int32_t)lp;
        if (a < 0) { w->sel_start = w->sel_end = n; }
        else { if (b < 0 || b > n) b = n; if (a > n) a = n; w->sel_start = a; w->sel_end = b; }
        edit_scroll_to_caret(w);
        win_invalidate_rect(w, NULL, 1);
        return 1;
    }
    case 0xB2: { int x0, y0, x1, y1; edit_area(w, &x0, &y0, &x1, &y1); int32_t r[4] = { x0, y0, x1, y1 }; if (lp) memcpy((void *)(uintptr_t)lp, r, 16); return 0; } // EM_GETRECT
    case 0xB5: { int d = wp == 0 ? -1 : wp == 1 ? 1 : wp == 2 ? -10 : 10; w->scroll_y += d; if (w->scroll_y < 0) w->scroll_y = 0; win_invalidate_rect(w, NULL, 1); return 0x10000 | 1; } // EM_SCROLL
    case 0xB6: w->scroll_y += (int)(int32_t)lp; if (w->scroll_y < 0) w->scroll_y = 0; win_invalidate_rect(w, NULL, 1); return 1; // EM_LINESCROLL
    case 0xB7: edit_scroll_to_caret(w); win_invalidate_rect(w, NULL, 1); return 1; // EM_SCROLLCARET
    case 0xB8: return (uint64_t)w->modified;
    case 0xB9: w->modified = wp != 0; return 0;
    case 0xBA: case 0xBB: case 0xC1: case 0xC9: case 0xC4: case 0xCE: { // line queries
        int x0, y0, x1, y1;
        edit_area(w, &x0, &y0, &x1, &y1);
        Cps t = to_cps(w->text);
        Line *ls;
        int n = edit_lines(w, &t, x1 - x0, &ls);
        uint64_t r = 0;
        if (msg == 0xBA) r = (uint64_t)n;
        else if (msg == 0xBB) { int li = (int)(int32_t)wp; if (li < 0) li = line_of(ls, n, w->sel_end); r = li < n ? (uint64_t)ls[li].start : (uint64_t)-1; }
        else if (msg == 0xC1) { int idx = (int)(int32_t)wp; int li = idx < 0 ? line_of(ls, n, w->sel_end) : line_of(ls, n, idx); r = (uint64_t)(ls[li].end - ls[li].start); }
        else if (msg == 0xC9) { int idx = (int)(int32_t)wp; r = (uint64_t)line_of(ls, n, idx < 0 ? w->sel_end : idx); }
        else if (msg == 0xCE) r = (uint64_t)w->scroll_y;
        else { // EM_GETLINE
            int li = (int)wp;
            if (li >= 0 && li < n && lp) {
                uint16_t cap = (uint16_t)mem_rd(lp, 2);
                char *s = from_cps(t.c + ls[li].start, ls[li].end - ls[li].start);
                if (unicode) { uint16_t *tmp = u2w_dup(s); size_t len = wstrlen(tmp); if (len > cap) len = cap; memcpy((void *)(uintptr_t)lp, tmp, len * 2); free(tmp); r = len; }
                else { size_t len = strlen(s); if (len > cap) len = cap; memcpy((void *)(uintptr_t)lp, s, len); r = len; }
                free(s);
            }
        }
        free(ls);
        free(t.c);
        return r;
    }
    case 0xC2: { // EM_REPLACESEL
        char *s = lp ? gstr(lp, unicode) : NULL;
        if (s) { edit_replace_utf8(w, s, 0); free(s); edit_changed(st, w); }
        return 0;
    }
    case 0xC5: w->limit = wp ? (int)wp : 0x7FFFFFFE; return 0; // EM_LIMITTEXT
    case 0xD5: return (uint64_t)w->limit;                           // EM_GETLIMITTEXT
    case 0xCC: w->password_char = (uint32_t)wp; win_invalidate_rect(w, NULL, 1); return 0; // EM_SETPASSWORDCHAR
    case 0xD2: return w->password_char;                             // EM_GETPASSWORDCHAR
    case 0xCF: w->readonly = wp != 0; if (wp) w->style |= ES_READONLY; else w->style &= ~(uint32_t)ES_READONLY; win_invalidate_rect(w, NULL, 1); return 1; // EM_SETREADONLY
    case 0xC6: case 0xC7: case 0x304: return 0;                     // undo
    case 0xCD: case 0xD3: case 0xD4: case 0x1501: return 0;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- ListBox / ComboBox shared item storage --------------------------------------------------------

static int item_insert(Win *w, int pos, const char *s, int sorted) {
    if (w->item_count == w->item_cap) {
        w->item_cap = w->item_cap ? w->item_cap * 2 : 16;
        w->items = realloc(w->items, sizeof(char *) * (size_t)w->item_cap);
        w->item_data = realloc(w->item_data, sizeof(uint64_t) * (size_t)w->item_cap);
    }
    if (sorted) { pos = 0; while (pos < w->item_count && strcasecmp(w->items[pos], s) <= 0) pos++; }
    if (pos < 0 || pos > w->item_count) pos = w->item_count;
    memmove(w->items + pos + 1, w->items + pos, sizeof(char *) * (size_t)(w->item_count - pos));
    memmove(w->item_data + pos + 1, w->item_data + pos, sizeof(uint64_t) * (size_t)(w->item_count - pos));
    w->items[pos] = strdup(s ? s : "");
    w->item_data[pos] = 0;
    w->item_count++;
    if (w->cur_sel >= pos) w->cur_sel++;
    return pos;
}
static int item_delete(Win *w, int pos) {
    if (pos < 0 || pos >= w->item_count) return -1;
    free(w->items[pos]);
    memmove(w->items + pos, w->items + pos + 1, sizeof(char *) * (size_t)(w->item_count - pos - 1));
    memmove(w->item_data + pos, w->item_data + pos + 1, sizeof(uint64_t) * (size_t)(w->item_count - pos - 1));
    w->item_count--;
    if (w->cur_sel == pos) w->cur_sel = -1;
    else if (w->cur_sel > pos) w->cur_sel--;
    return w->item_count;
}
static void items_reset(Win *w) {
    for (int i = 0; i < w->item_count; i++) free(w->items[i]);
    w->item_count = 0;
    w->cur_sel = -1;
    w->top_index = 0;
}
static int item_find(Win *w, int start, const char *s, int exact) {
    size_t n = strlen(s);
    for (int k = 0; k < w->item_count; k++) {
        int i = (start + 1 + k) % w->item_count;
        if (exact ? strcasecmp(w->items[i], s) == 0 : strncasecmp(w->items[i], s, n) == 0) return i;
    }
    return -1;
}
// handles the item messages common to list and combo boxes (msg already mapped to LB_* numbering)
static int items_message(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode, uint64_t *res) {
    (void)st;
    int sorted = w->ctl == CTL_LISTBOX ? (w->style & 2) != 0 : (w->style & 0x100) != 0;
    int has_strings = 1;
    switch (msg) {
    case 0x180: case 0x181: { // LB_ADDSTRING / LB_INSERTSTRING
        char *s = has_strings ? gstr(lp, unicode) : NULL;
        int pos = item_insert(w, msg == 0x180 ? -1 : (int)(int32_t)wp, s ? s : "", msg == 0x180 && sorted);
        if (!s) w->item_data[pos] = lp;
        free(s);
        *res = (uint64_t)pos;
        break;
    }
    case 0x182: *res = (uint64_t)(int64_t)item_delete(w, (int)(int32_t)wp); break;
    case 0x184: items_reset(w); *res = 0; break;
    case 0x18B: *res = (uint64_t)w->item_count; break;
    case 0x188: *res = (uint64_t)(int64_t)w->cur_sel; break;
    case 0x186: { int i = (int)(int32_t)wp; w->cur_sel = i >= 0 && i < w->item_count ? i : -1; *res = w->cur_sel < 0 && i != -1 ? (uint64_t)-1 : (uint64_t)(int64_t)w->cur_sel; break; }
    case 0x189: { int i = (int)(int32_t)wp; if (i < 0 || i >= w->item_count) { *res = (uint64_t)-1; break; } *res = put_str(lp, 1u << 20, w->items[i], unicode); break; }
    case 0x18A: { int i = (int)(int32_t)wp; *res = i < 0 || i >= w->item_count ? (uint64_t)-1 : units(w->items[i], unicode); break; }
    case 0x199: { int i = (int)(int32_t)wp; *res = i < 0 || i >= w->item_count ? (uint64_t)-1 : w->item_data[i]; break; }
    case 0x19A: { int i = (int)(int32_t)wp; if (i < 0 || i >= w->item_count) { *res = (uint64_t)-1; break; } w->item_data[i] = lp; *res = 0; break; }
    case 0x18F: case 0x1A2: case 0x18C: { // FINDSTRING / FINDSTRINGEXACT / SELECTSTRING
        char *s = gstr(lp, unicode);
        int i = s ? item_find(w, (int)(int32_t)wp, s, msg == 0x1A2) : -1;
        free(s);
        if (msg == 0x18C && i >= 0) w->cur_sel = i;
        *res = (uint64_t)(int64_t)i;
        break;
    }
    case 0x1A8: *res = 0; break; // LB_INITSTORAGE
    default: return 0;
    }
    return 1;
}

// ---- ListBox -------------------------------------------------------------------------------------

static int lb_item_height(Win *w) { return line_height(w) + 2; }
static int lb_visible(Win *w) { int ih = lb_item_height(w); int n = (w->h - 2) / (ih ? ih : 1); return n < 1 ? 1 : n; }
static void lb_ensure_visible(Win *w) {
    int vis = lb_visible(w);
    if (w->cur_sel >= 0) {
        if (w->cur_sel < w->top_index) w->top_index = w->cur_sel;
        if (w->cur_sel >= w->top_index + vis) w->top_index = w->cur_sel - vis + 1;
    }
    if (w->top_index > w->item_count - vis) w->top_index = w->item_count - vis;
    if (w->top_index < 0) w->top_index = 0;
}
static void listbox_paint(X86State *st, Win *w) {
    Paint p = ctl_begin(st, w, WM_CTLCOLORLISTBOX);
    uint32_t bg = p.has_bg ? p.bg : 0xFFFFFF;
    gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, bg);
    int b = (w->style & WS_BORDER) || (w->exstyle & WS_EX_CLIENTEDGE);
    int ih = lb_item_height(w);
    int focused = gui()->focus == w->hwnd;
    int ownerdraw = (w->style & 0x30) != 0;
    for (int i = w->top_index, y = b; i < w->item_count && y < w->h; i++, y += ih) {
        int sel = i == w->cur_sel;
        if (ownerdraw) {
            owner_draw(st, w, p.hdc, 2 /* ODT_LISTBOX */, (uint32_t)i, (sel ? 1 : 0) | (focused ? 0x10 : 0), b, y, w->w - b, y + ih, w->item_data[i]);
            continue;
        }
        if (sel) gdi_fill_rect_px(p.hdc, b, y, w->w - b, y + ih, focused ? gui_sys_color(COLOR_HIGHLIGHT) : 0xD9D9D9);
        set_text_color(p.hdc, sel && focused ? 0xFFFFFF : (w->enabled ? p.fg : 0x6D6D6D));
        text_in_rect(p.hdc, w->items[i], b + 3, y, w->w - b - 2, y + ih, 0x20 | 4 | 0x800);
    }
    if (b) gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, focused ? 0xD77800 : 0x828790);
    if (w->item_count > lb_visible(w)) { // thin scroll indicator
        int track = w->h - 4, vis = lb_visible(w);
        int th = track * vis / w->item_count; if (th < 12) th = 12;
        int ty = 2 + (track - th) * w->top_index / (w->item_count - vis > 0 ? w->item_count - vis : 1);
        gdi_fill_rect_px(p.hdc, w->w - 5, ty, w->w - 2, ty + th, 0xA0A0A0);
    }
    ctl_end(&p);
}
static void lb_select(X86State *st, Win *w, int i, int notify) {
    if (i < 0) i = 0;
    if (i >= w->item_count) i = w->item_count - 1;
    if (i == w->cur_sel) return;
    w->cur_sel = i;
    lb_ensure_visible(w);
    win_invalidate_rect(w, NULL, 1);
    if (notify && (w->style & 1)) win_notify_parent(st, w, 1); // LBN_SELCHANGE
}
static uint64_t listbox_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    GuiState *g = gui();
    uint64_t r;
    if (items_message(st, w, msg, wp, lp, unicode, &r)) {
        if (msg != 0x18B && msg != 0x188 && msg != 0x189 && msg != 0x18A && msg != 0x199 && msg != 0x18F && msg != 0x1A2) { lb_ensure_visible(w); win_invalidate_rect(w, NULL, 1); }
        return r;
    }
    switch (msg) {
    case WM_PAINT: listbox_paint(st, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        if (g->focus != w->hwnd) win_set_focus(st, w->hwnd);
        int b = (w->style & WS_BORDER) || (w->exstyle & WS_EX_CLIENTEDGE);
        int i = w->top_index + ((int16_t)HIWORD(lp) - b) / lb_item_height(w);
        if (i >= 0 && i < w->item_count) {
            lb_select(st, w, i, 1);
            if (msg == WM_LBUTTONDBLCLK && (w->style & 1)) win_notify_parent(st, w, 2); // LBN_DBLCLK
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = (int16_t)HIWORD(wp) / 40;
        w->top_index -= d;
        int vis = lb_visible(w);
        if (w->top_index > w->item_count - vis) w->top_index = w->item_count - vis;
        if (w->top_index < 0) w->top_index = 0;
        win_invalidate_rect(w, NULL, 1);
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_UP: lb_select(st, w, w->cur_sel - 1, 1); break;
        case VK_DOWN: lb_select(st, w, w->cur_sel + 1, 1); break;
        case VK_PRIOR: lb_select(st, w, w->cur_sel - lb_visible(w), 1); break;
        case VK_NEXT: lb_select(st, w, w->cur_sel + lb_visible(w), 1); break;
        case VK_HOME: lb_select(st, w, 0, 1); break;
        case VK_END: lb_select(st, w, w->item_count - 1, 1); break;
        default: break;
        }
        return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: win_invalidate_rect(w, NULL, 1); return 0;
    case WM_GETDLGCODE: return 1 | 0x80; // DLGC_WANTARROWS | DLGC_WANTCHARS
    case 0x185: // LB_SETSEL
        if (wp) { int i = (int)(int32_t)lp; w->cur_sel = i < 0 ? 0 : i; } else if ((int)(int32_t)lp == w->cur_sel || (int)(int32_t)lp == -1) w->cur_sel = -1;
        win_invalidate_rect(w, NULL, 1);
        return 0;
    case 0x187: return (int)(int32_t)wp == w->cur_sel && w->cur_sel >= 0;           // LB_GETSEL
    case 0x190: return w->cur_sel >= 0 ? 1 : 0;                                       // LB_GETSELCOUNT
    case 0x191: if (w->cur_sel >= 0 && wp > 0 && lp) { mem_wr(lp, 4, (uint32_t)w->cur_sel); return 1; } return 0; // LB_GETSELITEMS
    case 0x18E: return (uint64_t)w->top_index;                                         // LB_GETTOPINDEX
    case 0x197: w->top_index = (int)(int32_t)wp; lb_ensure_visible(w); win_invalidate_rect(w, NULL, 1); return 0;
    case 0x1A1: return (uint64_t)lb_item_height(w);                                    // LB_GETITEMHEIGHT
    case 0x1A0: return 0;
    case 0x198: { int i = (int)(int32_t)wp, ih = lb_item_height(w); int32_t rc[4] = { 0, (i - w->top_index) * ih, w->w, (i - w->top_index + 1) * ih }; if (lp) memcpy((void *)(uintptr_t)lp, rc, 16); return 1; }
    case 0x1A9: { int i = w->top_index + (int16_t)HIWORD(lp) / lb_item_height(w); return i < w->item_count ? (uint64_t)i : 0x10000u; }
    case 0x194: case 0x195: return 0;
    case WM_ENABLE: win_invalidate_rect(w, NULL, 1); return 0;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- ComboBox ------------------------------------------------------------------------------------

static void combo_sync_text(Win *w) {
    win_set_text(w, w->cur_sel >= 0 && w->cur_sel < w->item_count ? w->items[w->cur_sel] : "");
}
void combo_select_from_popup(X86State *st, Win *w, int index) {
    uint64_t h = w->hwnd;
    if (index < 0 || index >= w->item_count) return;
    int changed = index != w->cur_sel;
    w->cur_sel = index;
    combo_sync_text(w);
    win_invalidate_rect(w, NULL, 1);
    if (changed) win_notify_parent(st, w, 1);        // CBN_SELCHANGE
    if ((w = win_get(h))) win_notify_parent(st, w, 9); // CBN_SELENDOK
    if ((w = win_get(h))) win_notify_parent(st, w, 8); // CBN_CLOSEUP
}
static void combo_paint(X86State *st, Win *w) {
    int ro = (w->style & 3) == 3;
    Paint p = ctl_begin(st, w, ro ? WM_CTLCOLORSTATIC : WM_CTLCOLOREDIT);
    int focused = gui()->focus == w->hwnd;
    int h = w->h;
    gdi_fill_rect_px(p.hdc, 0, 0, w->w, h, !w->enabled ? 0xCCCCCC : ro ? (w->pressed ? 0xF7E4CC : 0xE1E1E1) : 0xFFFFFF);
    gdi_frame_rect_px(p.hdc, 0, 0, w->w, h, focused ? 0xD77800 : 0xADADAD);
    if (w->cur_sel >= 0 && w->cur_sel < w->item_count) {
        if ((w->style & 0x30) && w->parent) owner_draw(st, w, p.hdc, 3 /* ODT_COMBOBOX */, (uint32_t)w->cur_sel, focused ? 0x10 : 0, 2, 2, w->w - 20, h - 2, w->item_data[w->cur_sel]);
        else {
            set_text_color(p.hdc, w->enabled ? 0x000000 : 0x838383);
            text_in_rect(p.hdc, w->items[w->cur_sel], 5, 0, w->w - 20, h, 0x20 | 4 | 0x800);
        }
    } else if (!ro && w->text[0]) {
        set_text_color(p.hdc, 0x000000);
        text_in_rect(p.hdc, w->text, 5, 0, w->w - 20, h, 0x20 | 4 | 0x800);
    }
    int ax = w->w - 13, ay = h / 2 - 1;
    uint32_t ac = w->enabled ? 0x404040 : 0xA0A0A0;
    gdi_line_px(p.hdc, ax - 4, ay - 2, ax, ay + 2, ac, 1);
    gdi_line_px(p.hdc, ax, ay + 2, ax + 4, ay - 2, ac, 1);
    ctl_end(&p);
}
static uint64_t combo_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    GuiState *g = gui();
    static const uint32_t map[] = { // CB_* -> LB_* for the shared item handling
        [0x143 - 0x140] = 0x180, [0x144 - 0x140] = 0x182, [0x146 - 0x140] = 0x18B, [0x147 - 0x140] = 0x188, [0x148 - 0x140] = 0x189,
        [0x149 - 0x140] = 0x18A, [0x14A - 0x140] = 0x181, [0x14B - 0x140] = 0x184, [0x14C - 0x140] = 0x18F, [0x14D - 0x140] = 0x18C,
        [0x14E - 0x140] = 0x186, [0x150 - 0x140] = 0x199, [0x151 - 0x140] = 0x19A, [0x158 - 0x140] = 0x1A2,
    };
    if (msg >= 0x140 && msg <= 0x158 && map[msg - 0x140]) {
        uint64_t r;
        int before = w->cur_sel;
        items_message(st, w, map[msg - 0x140], wp, lp, unicode, &r);
        if (w->cur_sel != before || msg == 0x14B || msg == 0x14E || msg == 0x14D) { combo_sync_text(w); win_invalidate_rect(w, NULL, 1); }
        return r;
    }
    switch (msg) {
    case WM_NCCREATE:
        if ((w->style & 3) != 1) { int ih = line_height(w) + 8; if (w->h > ih || w->h == 0) w->h = ih; }
        return 1;
    case WM_SETFONT:
        w->font = wp;
        if ((w->style & 3) != 1) { int ih = line_height(w) + 8; if (w->h != ih) w->h = ih; }
        win_invalidate_rect(w, NULL, 1);
        return 0;
    case WM_PAINT: combo_paint(st, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (g->focus != w->hwnd) win_set_focus(st, w->hwnd);
        if (!(w = win_get(w->hwnd))) return 0;
        win_notify_parent(st, w, 7); // CBN_DROPDOWN
        if (g_proc->opts.gui_enabled && w->item_count) {
            gui_lock();
            g->popup_active = 1; g->popup_kind = 2; g->popup_hwnd = w->hwnd; g->popup_menu = 0;
            gui_unlock();
            __atomic_add_fetch(&g->generation, 1, __ATOMIC_SEQ_CST);
        } else if (w->item_count) {
            combo_select_from_popup(st, w, (w->cur_sel + 1) % w->item_count);
        }
        return 0;
    case WM_MOUSEWHEEL: case WM_KEYDOWN: {
        int d = 0;
        if (msg == WM_MOUSEWHEEL) d = (int16_t)HIWORD(wp) > 0 ? -1 : 1;
        else if (wp == VK_UP || wp == VK_LEFT) d = -1;
        else if (wp == VK_DOWN || wp == VK_RIGHT) d = 1;
        else if (wp == VK_HOME) d = -w->item_count;
        else if (wp == VK_END) d = w->item_count;
        if (d && w->item_count) {
            int i = w->cur_sel + d;
            if (i < 0) i = 0;
            if (i >= w->item_count) i = w->item_count - 1;
            if (i != w->cur_sel) { uint64_t h = w->hwnd; w->cur_sel = i; combo_sync_text(w); win_invalidate_rect(w, NULL, 1); win_notify_parent(st, w, 1); (void)h; }
        }
        return 0;
    }
    case WM_SETFOCUS: win_invalidate_rect(w, NULL, 1); win_notify_parent(st, w, 3); return 0;
    case WM_KILLFOCUS: win_invalidate_rect(w, NULL, 1); win_notify_parent(st, w, 4); return 0;
    case WM_GETDLGCODE: return 1 | 0x80;
    case 0x14F: return 0;                       // CB_SHOWDROPDOWN
    case 0x157: return g->popup_active && g->popup_kind == 2 && g->popup_hwnd == w->hwnd; // CB_GETDROPPEDSTATE
    case 0x154: return (uint64_t)line_height(w) + 2; // CB_GETITEMHEIGHT
    case 0x153: case 0x160: case 0x141: case 0x142: return 1;
    case 0x140: return 0;
    case 0x152: { int32_t rc[4]; int ox, oy; win_client_origin(w, &ox, &oy); rc[0] = 0; rc[1] = 0; rc[2] = w->w; rc[3] = w->h + 100; if (lp) memcpy((void *)(uintptr_t)lp, rc, 16); return 1; }
    case WM_SETTEXT: {
        char *s = lp ? gstr(lp, unicode) : strdup("");
        int i = s ? item_find(w, -1, s, 1) : -1;
        if (i >= 0) w->cur_sel = i;
        win_set_text(w, s ? s : "");
        free(s);
        win_invalidate_rect(w, NULL, 1);
        return 1;
    }
    case WM_ENABLE: win_invalidate_rect(w, NULL, 1); return 0;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- progress bar --------------------------------------------------------------------------------

static uint64_t progress_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    int old = w->progress_pos;
    switch (msg) {
    case WM_PAINT: {
        Paint p = ctl_begin(st, w, 0);
        gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, 0xE6E6E6);
        gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, 0xBCBCBC);
        int range = w->progress_max - w->progress_min;
        int fill = range > 0 ? (int)((int64_t)(w->progress_pos - w->progress_min) * (w->w - 2) / range) : 0;
        if (fill > 0) gdi_fill_rect_px(p.hdc, 1, 1, 1 + fill, w->h - 1, 0x25B006);
        ctl_end(&p);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case 0x401: w->progress_min = (int)LOWORD(lp); w->progress_max = (int)HIWORD(lp); win_invalidate_rect(w, NULL, 1); return ((uint32_t)old << 16);
    case 0x406: w->progress_min = (int)(int32_t)wp; w->progress_max = (int)(int32_t)lp; win_invalidate_rect(w, NULL, 1); return 0;
    case 0x402: w->progress_pos = (int)(int32_t)wp; break;
    case 0x403: w->progress_pos += (int)(int32_t)wp; break;
    case 0x404: w->top_index = (int)(int32_t)wp; return 0;
    case 0x405: w->progress_pos += w->top_index ? w->top_index : 10; if (w->progress_pos > w->progress_max) w->progress_pos = w->progress_min; break;
    case 0x407: if (lp) { mem_wr(lp, 4, (uint32_t)w->progress_min); mem_wr(lp + 4, 4, (uint32_t)w->progress_max); } return (uint64_t)(int64_t)(wp ? w->progress_min : w->progress_max);
    case 0x408: return (uint64_t)w->progress_pos;
    case 0x409: return 0;
    case 0x40A: return 1;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
    if (w->progress_pos < w->progress_min) w->progress_pos = w->progress_min;
    if (w->progress_pos > w->progress_max) w->progress_pos = w->progress_max;
    win_invalidate_rect(w, NULL, 1);
    return (uint64_t)old;
}

uint64_t ctl_proc(X86State *st, Win *w, int ctl, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    switch (ctl) {
    case CTL_BUTTON: return button_proc(st, w, msg, wp, lp, unicode);
    case CTL_STATIC: return static_proc(st, w, msg, wp, lp, unicode);
    case CTL_EDIT: return edit_proc(st, w, msg, wp, lp, unicode);
    case CTL_LISTBOX: return listbox_proc(st, w, msg, wp, lp, unicode);
    case CTL_COMBOBOX: return combo_proc(st, w, msg, wp, lp, unicode);
    case CTL_PROGRESS: return progress_proc(st, w, msg, wp, lp, unicode);
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- dialogs -------------------------------------------------------------------------------------

static Win **dlg_children(Win *d, int *n) {
    GuiState *g = gui();
    Win **list = malloc(sizeof(Win *) * WIN_MAX);
    int m = 0;
    for (int i = 0; i < WIN_MAX; i++) if (g->wins[i].used && g->wins[i].parent == d->hwnd) list[m++] = &g->wins[i];
    for (int a = 1; a < m; a++) for (int b = a; b > 0 && list[b - 1]->seq > list[b]->seq; b--) { Win *t = list[b]; list[b] = list[b - 1]; list[b - 1] = t; }
    *n = m;
    return list;
}
static Win *dlg_item(Win *d, uint64_t id) {
    GuiState *g = gui();
    for (int i = 0; i < WIN_MAX; i++) if (g->wins[i].used && g->wins[i].parent == d->hwnd && g->wins[i].id == (id & 0xFFFFFFFF)) return &g->wins[i];
    return NULL;
}
static int tabbable(Win *c) { return c->visible && c->enabled && (c->style & WS_TABSTOP); }
static Win *next_tab(Win *d, Win *from, int prev) {
    int n;
    Win **list = dlg_children(d, &n);
    Win *r = NULL;
    int start = -1;
    for (int i = 0; i < n; i++) if (list[i] == from) start = i;
    for (int k = 1; k <= n && !r; k++) {
        int i = prev ? (start - k + 2 * n) % n : (start + k) % n;
        if (start < 0) i = prev ? n - k : k - 1;
        if (tabbable(list[i])) r = list[i];
    }
    free(list);
    return r;
}
static void dlg_focus(X86State *st, Win *d, Win *c) {
    if (!c) return;
    uint64_t h = c->hwnd;
    win_set_focus(st, h);
    if ((c = win_get(h)) && c->ctl == CTL_EDIT) win_send(st, h, 0xB1, 0, (uint64_t)-1);
    if (win_get(d->hwnd)) d->dlg_focus = c ? c->hwnd : 0;
}

uint64_t dlg_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    uint64_t hwnd = w->hwnd;
    if (w->dlgproc) {
        uint64_t args[4] = { hwnd, msg, wp, lp };
        uint64_t r = soul_guest_call(st, w->dlgproc, 4, args);
        if (!(w = win_get(hwnd))) return 0;
        if ((uint32_t)r || (r >> 32)) {
            switch (msg) {
            case WM_CTLCOLORMSGBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: case WM_CTLCOLORDLG:
            case WM_CTLCOLORSCROLLBAR: case WM_CTLCOLORSTATIC: case 0x0039: case 0x002E: case 0x002F: case 0x0037: case WM_INITDIALOG:
                return r;
            default: { uint64_t v = 0; memcpy(&v, w->extra, 8); return v; } // DWLP_MSGRESULT
            }
        }
    }
    switch (msg) {
    case WM_ERASEBKGND: {
        uint64_t br = win_send(st, hwnd, WM_CTLCOLORDLG, wp, hwnd);
        uint32_t c;
        if (!brush_color(br, &c)) c = gui_sys_color(COLOR_BTNFACE);
        gdi_fill_rect_px(wp, 0, 0, w->w, w->h, c);
        return 1;
    }
    case WM_CLOSE: {
        Win *cancel = dlg_item(w, IDCANCEL);
        win_send(st, hwnd, WM_COMMAND, IDCANCEL, cancel ? cancel->hwnd : 0);
        return 0;
    }
    case WM_ACTIVATE:
        if ((wp & 0xFFFF) && !(w->style & WS_CHILD)) {
            Win *f = win_get(w->dlg_focus);
            GuiState *g = gui();
            Win *cur = win_get(g->focus);
            int inside = cur && (cur == w || win_top(cur) == w);
            if (!inside) dlg_focus(st, w, f && f->parent == hwnd ? f : next_tab(w, NULL, 0));
        }
        return 0;
    case WM_SETFOCUS: {
        Win *f = win_get(w->dlg_focus);
        dlg_focus(st, w, f && f->parent == hwnd ? f : next_tab(w, NULL, 0));
        return 0;
    }
    case WM_NEXTDLGCTL: {
        Win *c;
        if (lp & 0xFFFF) c = win_get(wp);
        else c = next_tab(w, win_get(gui()->focus), wp != 0);
        dlg_focus(st, w, c);
        return 0;
    }
    case WM_USER: return w->def_id ? ((uint64_t)0x534B << 16) | (uint16_t)w->def_id : 0; // DM_GETDEFID
    case WM_USER + 1: { Win *o = dlg_item(w, w->def_id), *n = dlg_item(w, wp); w->def_id = wp; if (o) win_invalidate_rect(o, NULL, 1); if (n) win_invalidate_rect(n, NULL, 1); return 1; }
    case WM_GETDLGCODE: return 0;
    case WM_SHOWWINDOW: case WM_INITDIALOG: case WM_COMMAND: case WM_NOTIFY: return 0;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// Returns 1 if the message was processed for the dialog (translated and dispatched as needed).
int dlg_is_message(X86State *st, Win *d, uint8_t *m) {
    uint64_t hwnd; uint32_t msg; uint64_t wp;
    memcpy(&hwnd, m, 8); memcpy(&msg, m + 8, 4); memcpy(&wp, m + 16, 8);
    Win *t = win_get(hwnd);
    if (!t || (t != d && win_top(t) != d && !(d->style & WS_CHILD))) return 0;
    if ((d->style & WS_CHILD) && t != d) { Win *a = t; while (a && a != d) a = win_get(a->parent); if (!a) return 0; }
    GuiState *g = gui();
    if (msg == WM_KEYDOWN) {
        uint64_t code = win_send(st, hwnd, WM_GETDLGCODE, wp, (uint64_t)(uintptr_t)m);
        if (wp == VK_TAB && !(code & 0x6)) {
            Win *n = next_tab(d, win_get(g->focus), (g->keys[VK_SHIFT] & 0x80) != 0);
            dlg_focus(st, d, n);
            return 1;
        }
        if (wp == VK_RETURN && !(code & 0x4)) {
            Win *f = win_get(g->focus);
            uint64_t id = d->def_id ? d->def_id : IDOK;
            Win *b = NULL;
            if (f && f->parent == d->hwnd && f->ctl == CTL_BUTTON && is_push(f)) { id = f->id; b = f; }
            else b = dlg_item(d, id);
            if (b && !b->enabled) return 1;
            win_send(st, d->hwnd, WM_COMMAND, (uint16_t)id, b ? b->hwnd : 0);
            return 1;
        }
        if (wp == VK_ESCAPE && !(code & 0x4)) {
            Win *b = dlg_item(d, IDCANCEL);
            win_send(st, d->hwnd, WM_COMMAND, IDCANCEL, b ? b->hwnd : 0);
            return 1;
        }
        if ((wp == VK_UP || wp == VK_DOWN || wp == VK_LEFT || wp == VK_RIGHT) && !(code & 0x1)) {
            Win *n = next_tab(d, win_get(g->focus), wp == VK_UP || wp == VK_LEFT);
            if (n) dlg_focus(st, d, n);
            return 1;
        }
    }
    if (msg == WM_CHAR && (wp == '\t' || wp == 27)) return 1;
    gui_translate_dispatch(st, m);
    return 1;
}

// sz_Or_Ord
static const uint16_t *read_sz_or_ord(const uint16_t *p, uint64_t *ord, const uint16_t **str) {
    *ord = 0; *str = NULL;
    if (p[0] == 0) return p + 1;
    if (p[0] == 0xFFFF) { *ord = p[1]; return p + 2; }
    *str = p;
    while (*p) p++;
    return p + 1;
}
static const uint16_t *align4(const uint16_t *p) { return (const uint16_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3); }

static const uint16_t k_button[] = { 'B', 'u', 't', 't', 'o', 'n', 0 }, k_edit[] = { 'E', 'd', 'i', 't', 0 }, k_static[] = { 'S', 't', 'a', 't', 'i', 'c', 0 },
                      k_listbox[] = { 'L', 'i', 's', 't', 'B', 'o', 'x', 0 }, k_scroll[] = { 'S', 'c', 'r', 'o', 'l', 'l', 'B', 'a', 'r', 0 },
                      k_combo[] = { 'C', 'o', 'm', 'b', 'o', 'B', 'o', 'x', 0 }, k_dialog[] = { '#', '3', '2', '7', '7', '0', 0 };

uint64_t dlg_create(X86State *st, uint64_t hinst, const uint8_t *tmpl, uint64_t owner, uint64_t proc, uint64_t param, int unicode) {
    GuiState *g = gui();
    const uint16_t *p = (const uint16_t *)(const void *)tmpl;
    int ex = p[0] == 1 && p[1] == 0xFFFF;
    uint32_t style, exstyle;
    uint16_t count;
    int16_t x, y, cx, cy;
    if (ex) {
        memcpy(&exstyle, tmpl + 8, 4); memcpy(&style, tmpl + 12, 4);
        memcpy(&count, tmpl + 16, 2);
        memcpy(&x, tmpl + 18, 2); memcpy(&y, tmpl + 20, 2); memcpy(&cx, tmpl + 22, 2); memcpy(&cy, tmpl + 24, 2);
        p = (const uint16_t *)(const void *)(tmpl + 26);
    } else {
        memcpy(&style, tmpl, 4); memcpy(&exstyle, tmpl + 4, 4);
        memcpy(&count, tmpl + 8, 2);
        memcpy(&x, tmpl + 10, 2); memcpy(&y, tmpl + 12, 2); memcpy(&cx, tmpl + 14, 2); memcpy(&cy, tmpl + 16, 2);
        p = (const uint16_t *)(const void *)(tmpl + 18);
    }
    uint64_t menu_ord, cls_ord;
    const uint16_t *menu_str, *cls_str, *title;
    p = read_sz_or_ord(p, &menu_ord, &menu_str);
    p = read_sz_or_ord(p, &cls_ord, &cls_str);
    title = p;
    while (*p) p++;
    p++;
    int pt = 8, weight = 400;
    char face[64] = "MS Shell Dlg";
    if (style & 0x40) { // DS_SETFONT
        pt = (int16_t)*p++;
        if (ex) { weight = *p++; p++; } // weight, italic + charset
        w2u(p, -1, face, sizeof face);
        while (*p) p++;
        p++;
    }
    if (pt <= 0) pt = 8;
    int by = (int)(pt * 1.62 + 0.5), bx = (int)(pt * 0.75 + 0.5);
    if (bx < 4) bx = 4;
    uint64_t font = gdi_make_font(-(pt * 96 + 36) / 72, weight, "Segoe UI");
    int px = x * bx / 4, py = y * by / 8, pw = cx * bx / 4, ph = cy * by / 8;
    Win *ow = win_get(owner);
    int child = (style & WS_CHILD) && ow;
    if (!child) {
        if (ow) owner = win_top(ow)->hwnd;
        ow = win_get(owner);
        int ox = ow ? ow->x : 0, oy = ow ? ow->y : 0;
        if ((style & 0x800) || (style & 0x1000) || (x == 0 && y == 0) || !ow) { // DS_CENTER / DS_CENTERMOUSE
            if (ow) { px = ow->x + (ow->w - pw) / 2; py = ow->y + (ow->h - ph) / 2; }
            else { px = (g->screen_w - pw) / 2; py = (g->screen_h - ph) / 2; }
        } else { px += ox; py += oy; }
        if (px < 0) px = 0;
        if (py < 0) py = 0;
    }
    uint64_t cls = cls_str ? (uint64_t)(uintptr_t)cls_str : cls_ord ? cls_ord : (uint64_t)(uintptr_t)k_dialog;
    g->pending_dialog = 1;
    g->pending_dlgproc = proc;
    g->pending_dlg_unicode = unicode;
    uint64_t hdlg = win_create(st, exstyle, cls, (uint64_t)(uintptr_t)title, style & ~WS_VISIBLE, px, py, pw, ph, child ? owner : (owner ? owner : 0),
                               menu_ord ? menu_ord : 0, hinst, param, 1);
    g->pending_dialog = 0;
    Win *d = win_get(hdlg);
    if (!d) return 0;
    if (menu_ord || menu_str) {
        const void *r = pe_resource(hinst, 4, menu_str ? (uint64_t)(uintptr_t)menu_str : menu_ord, 1, NULL);
        extern uint64_t menu_from_resource(const uint8_t *res);
        if (r) d->menu = menu_from_resource(r);
    }
    d->is_dialog = 1;
    d->dlg_bx = bx; d->dlg_by = by;
    d->font = font;
    d->def_id = IDOK;
    if (style & 0x40) win_send(st, hdlg, WM_SETFONT, font, 0);
    for (int i = 0; i < count; i++) {
        p = align4(p);
        uint32_t cstyle, cex, id;
        int16_t ix, iy, icx, icy;
        const uint8_t *b = (const uint8_t *)p;
        if (ex) {
            memcpy(&cex, b + 4, 4); memcpy(&cstyle, b + 8, 4);
            memcpy(&ix, b + 12, 2); memcpy(&iy, b + 14, 2); memcpy(&icx, b + 16, 2); memcpy(&icy, b + 18, 2);
            memcpy(&id, b + 20, 4);
            p = (const uint16_t *)(const void *)(b + 24);
        } else {
            memcpy(&cstyle, b, 4); memcpy(&cex, b + 4, 4);
            memcpy(&ix, b + 8, 2); memcpy(&iy, b + 10, 2); memcpy(&icx, b + 12, 2); memcpy(&icy, b + 14, 2);
            uint16_t id16; memcpy(&id16, b + 16, 2); id = id16;
            p = (const uint16_t *)(const void *)(b + 18);
        }
        uint64_t ccls_ord, ctext_ord;
        const uint16_t *ccls_str, *ctext_str;
        p = read_sz_or_ord(p, &ccls_ord, &ccls_str);
        p = read_sz_or_ord(p, &ctext_ord, &ctext_str);
        uint16_t extra = *p++;
        const void *cdata = extra ? (const void *)p : NULL;
        p = (const uint16_t *)(const void *)((const uint8_t *)p + extra);
        uint64_t ccls;
        if (ccls_str) ccls = (uint64_t)(uintptr_t)ccls_str;
        else switch (ccls_ord) {
            case 0x80: ccls = (uint64_t)(uintptr_t)k_button; break;
            case 0x81: ccls = (uint64_t)(uintptr_t)k_edit; break;
            case 0x82: ccls = (uint64_t)(uintptr_t)k_static; break;
            case 0x83: ccls = (uint64_t)(uintptr_t)k_listbox; break;
            case 0x84: ccls = (uint64_t)(uintptr_t)k_scroll; break;
            case 0x85: ccls = (uint64_t)(uintptr_t)k_combo; break;
            default: ccls = (uint64_t)(uintptr_t)k_static; break;
        }
        uint16_t *ord_name = NULL;
        uint64_t ctext = ctext_str ? (uint64_t)(uintptr_t)ctext_str : 0;
        if (!ctext_str && ctext_ord) { ord_name = malloc(8); ord_name[0] = 0xFFFF; ord_name[1] = (uint16_t)ctext_ord; ord_name[2] = 0; ctext = (uint64_t)(uintptr_t)ord_name; }
        uint64_t hc = win_create(st, cex, ccls, ctext, cstyle | WS_CHILD, ix * bx / 4, iy * by / 8, icx * bx / 4, icy * by / 8, hdlg, id, hinst,
                                 (uint64_t)(uintptr_t)cdata, 1);
        free(ord_name);
        if (!win_get(hdlg)) return 0;
        if (!hc) {
            if (g_proc->verbose) soul_log("[diyalog] Denetim oluşturulamadı (kimlik %u)\n", id);
            continue;
        }
        win_send(st, hc, WM_SETFONT, font, 0);
        Win *c = win_get(hc);
        if (c && c->ctl == CTL_BUTTON && (c->style & 0xF) == 1) d->def_id = id;
    }
    d = win_get(hdlg);
    if (!d) return 0;
    Win *first = next_tab(d, NULL, 0);
    uint64_t r = win_send_ex(st, hdlg, WM_INITDIALOG, first ? first->hwnd : 0, param, unicode);
    if (!(d = win_get(hdlg))) return 0;
    if (r) { first = next_tab(d, NULL, 0); if (first) d->dlg_focus = first->hwnd; }
    else if (gui()->focus && win_get(gui()->focus) && win_top(win_get(gui()->focus)) == d) d->dlg_focus = gui()->focus;
    if (style & WS_VISIBLE) win_show(st, d, 5);
    if ((d = win_get(hdlg)) && r && d->visible && first && win_get(first->hwnd)) dlg_focus(st, d, win_get(first->hwnd));
    return hdlg;
}

static const uint8_t *dlg_template(uint64_t hinst, uint64_t name, int wide) { return pe_resource(hinst, 5, name, wide, NULL); }

static int64_t dialog_box(X86State *st, uint64_t hinst, const uint8_t *tmpl, uint64_t owner, uint64_t proc, uint64_t param, int unicode) {
    if (!tmpl) { set_last_error(st, 1814); return -1; } // ERROR_RESOURCE_NAME_NOT_FOUND
    Win *ow = win_get(owner);
    if (ow) { ow = win_top(ow); owner = ow->hwnd; }
    int owner_was_enabled = ow && ow->enabled;
    if (owner_was_enabled) { gui_lock(); ow->enabled = 0; gui_unlock(); win_send(st, owner, WM_ENABLE, 0, 0); }
    uint64_t hdlg = dlg_create(st, hinst, tmpl, owner, proc, param, unicode);
    Win *d = win_get(hdlg);
    int64_t result = -1;
    if (d) {
        if (!d->dlg_ended && !d->visible) win_show(st, d, 5);
        uint8_t *msg = calloc(1, 48);
        while ((d = win_get(hdlg)) && !d->dlg_ended) {
            if (!gui_get_message(st, msg)) { uint64_t code; memcpy(&code, msg + 16, 8); gui_repost_quit((int)code); break; }
            if (!dlg_is_message(st, d, msg)) gui_translate_dispatch(st, msg);
        }
        free(msg);
        if ((d = win_get(hdlg))) { result = (int64_t)d->dlg_result; win_destroy(st, d); }
    }
    if (owner_was_enabled && (ow = win_get(owner))) {
        gui_lock(); ow->enabled = 1; gui_unlock();
        win_send(st, owner, WM_ENABLE, 1, 0);
        if ((ow = win_get(owner)) && ow->visible) {
            ow->zstamp = ++gui()->zstamp;
            GuiState *g = gui();
            if (g->active != owner) {
                uint64_t a = g->active;
                g->active = owner;
                if (a && win_get(a)) win_send(st, a, WM_ACTIVATE, 0, owner);
                win_send(st, owner, WM_ACTIVATE, 1, a);
            }
            __atomic_add_fetch(&g->generation, 1, __ATOMIC_SEQ_CST);
        }
    }
    return result;
}

HLE(c_DialogBoxParamA) { RET((uint64_t)dialog_box(st, ARG(0), dlg_template(ARG(0), ARG(1), 0), ARG(2), ARG(3), hle_arg(st, 4), 0)); }
HLE(c_DialogBoxParamW) { RET((uint64_t)dialog_box(st, ARG(0), dlg_template(ARG(0), ARG(1), 1), ARG(2), ARG(3), hle_arg(st, 4), 1)); }
HLE(c_DialogBoxIndirectParamA) { RET((uint64_t)dialog_box(st, ARG(0), PTR(1), ARG(2), ARG(3), hle_arg(st, 4), 0)); }
HLE(c_DialogBoxIndirectParamW) { RET((uint64_t)dialog_box(st, ARG(0), PTR(1), ARG(2), ARG(3), hle_arg(st, 4), 1)); }
HLE(c_CreateDialogParamA) { const uint8_t *t = dlg_template(ARG(0), ARG(1), 0); RET(t ? dlg_create(st, ARG(0), t, ARG(2), ARG(3), hle_arg(st, 4), 0) : 0); }
HLE(c_CreateDialogParamW) { const uint8_t *t = dlg_template(ARG(0), ARG(1), 1); RET(t ? dlg_create(st, ARG(0), t, ARG(2), ARG(3), hle_arg(st, 4), 1) : 0); }
HLE(c_CreateDialogIndirectParamA) { RET(dlg_create(st, ARG(0), PTR(1), ARG(2), ARG(3), hle_arg(st, 4), 0)); }
HLE(c_CreateDialogIndirectParamW) { RET(dlg_create(st, ARG(0), PTR(1), ARG(2), ARG(3), hle_arg(st, 4), 1)); }
HLE(c_EndDialog) {
    Win *d = win_get(ARG(0));
    if (!d) { RET(0); return; }
    d->dlg_ended = 1;
    d->dlg_result = ARG(1);
    win_show(st, d, 0);
    win_post(ARG(0), WM_NULL, 0, 0);
    RET(1);
}
HLE(c_DefDlgProcA) { Win *w = win_get(ARG(0)); if (!w) { RET(0); return; } uint64_t saved = w->dlgproc; w->dlgproc = 0; RET(dlg_proc(st, w, (uint32_t)ARG(1), ARG(2), ARG(3), 0)); if ((w = win_get(ARG(0)))) w->dlgproc = saved; }
HLE(c_DefDlgProcW) { Win *w = win_get(ARG(0)); if (!w) { RET(0); return; } uint64_t saved = w->dlgproc; w->dlgproc = 0; RET(dlg_proc(st, w, (uint32_t)ARG(1), ARG(2), ARG(3), 1)); if ((w = win_get(ARG(0)))) w->dlgproc = saved; }
HLE(c_IsDialogMessage) { Win *d = win_get(ARG(0)); RET(d ? dlg_is_message(st, d, PTR(1)) : 0); }
HLE(c_GetDlgItem) { Win *d = win_get(ARG(0)); Win *c = d ? dlg_item(d, ARG(1)) : NULL; RET(c ? c->hwnd : 0); }
HLE(c_GetDlgCtrlID) { Win *w = win_get(ARG(0)); RET(w ? w->id : 0); }
static void set_dlg_item_text(X86State *st, int wide) {
    Win *d = win_get(ARG(0)), *c = d ? dlg_item(d, ARG(1)) : NULL;
    RET(c ? win_send_ex(st, c->hwnd, WM_SETTEXT, 0, ARG(2), wide) : 0);
}
HLE(c_SetDlgItemTextA) { set_dlg_item_text(st, 0); }
HLE(c_SetDlgItemTextW) { set_dlg_item_text(st, 1); }
static void get_dlg_item_text(X86State *st, int wide) {
    Win *d = win_get(ARG(0)), *c = d ? dlg_item(d, ARG(1)) : NULL;
    int cap = ARGI(3);
    if (ARG(2) && cap > 0) { if (wide) ((uint16_t *)PTR(2))[0] = 0; else ((char *)PTR(2))[0] = 0; }
    RET(c && cap > 0 ? win_send_ex(st, c->hwnd, WM_GETTEXT, (uint64_t)cap, ARG(2), wide) : 0);
}
HLE(c_GetDlgItemTextA) { get_dlg_item_text(st, 0); }
HLE(c_GetDlgItemTextW) { get_dlg_item_text(st, 1); }
HLE(c_SetDlgItemInt) {
    Win *d = win_get(ARG(0)), *c = d ? dlg_item(d, ARG(1)) : NULL;
    if (!c) { RET(0); return; }
    char buf[32];
    if (ARG(3)) snprintf(buf, sizeof buf, "%d", (int32_t)ARG(2)); else snprintf(buf, sizeof buf, "%u", (uint32_t)ARG(2));
    win_send_ex(st, c->hwnd, WM_SETTEXT, 0, (uint64_t)(uintptr_t)buf, 0);
    RET(1);
}
HLE(c_GetDlgItemInt) {
    Win *d = win_get(ARG(0)), *c = d ? dlg_item(d, ARG(1)) : NULL;
    int ok = 0;
    long v = 0;
    if (c) {
        char *end;
        const char *s = c->text;
        while (*s == ' ') s++;
        v = strtol(s, &end, 10);
        ok = end != s && (*end == 0 || *end == ' ');
        if (!ARG(3) && v < 0) ok = 0;
    }
    if (ARG(2)) mem_wr(ARG(2), 4, (uint32_t)ok);
    RET(ok ? (uint32_t)v : 0);
}
HLE(c_CheckDlgButton) { Win *d = win_get(ARG(0)), *c = d ? dlg_item(d, ARG(1)) : NULL; if (c) win_send(st, c->hwnd, 0xF1, ARG(2), 0); RET(c != NULL); }
HLE(c_IsDlgButtonChecked) { Win *d = win_get(ARG(0)), *c = d ? dlg_item(d, ARG(1)) : NULL; RET(c ? win_send(st, c->hwnd, 0xF0, 0, 0) : 0); }
HLE(c_CheckRadioButton) {
    Win *d = win_get(ARG(0));
    if (!d) { RET(0); return; }
    for (uint32_t id = (uint32_t)ARG(1); id <= (uint32_t)ARG(2); id++) { Win *c = dlg_item(d, id); if (c) win_send(st, c->hwnd, 0xF1, id == (uint32_t)ARG(3), 0); }
    RET(1);
}
static void send_dlg_item_message(X86State *st, int wide) {
    Win *d = win_get(ARG(0)), *c = d ? dlg_item(d, ARG(1)) : NULL;
    RET(c ? win_send_ex(st, c->hwnd, (uint32_t)ARG(2), ARG(3), hle_arg(st, 4), wide) : 0);
}
HLE(c_SendDlgItemMessageA) { send_dlg_item_message(st, 0); }
HLE(c_SendDlgItemMessageW) { send_dlg_item_message(st, 1); }
HLE(c_MapDialogRect) {
    Win *d = win_get(ARG(0));
    int bx = d && d->dlg_bx ? d->dlg_bx : 6, by = d && d->dlg_by ? d->dlg_by : 13;
    int32_t r[4]; memcpy(r, PTR(1), 16);
    r[0] = r[0] * bx / 4; r[2] = r[2] * bx / 4; r[1] = r[1] * by / 8; r[3] = r[3] * by / 8;
    memcpy(PTR(1), r, 16);
    RET(1);
}
HLE(c_GetDialogBaseUnits) { RET((16u << 16) | 8u); }
HLE(c_GetNextDlgTabItem) {
    Win *d = win_get(ARG(0));
    Win *n = d ? next_tab(d, win_get(ARG(1)), ARG(2) != 0) : NULL;
    RET(n ? n->hwnd : 0);
}
HLE(c_GetNextDlgGroupItem) {
    Win *c = win_get(ARG(1));
    if (!c) { RET(0); return; }
    Win **grp;
    int n = group_of(c, &grp), idx = 0;
    for (int i = 0; i < n; i++) if (grp[i] == c) idx = i;
    Win *r = n ? grp[(idx + (ARG(2) ? n - 1 : 1)) % n] : c;
    free(grp);
    RET(r->hwnd);
}
HLE(c_DlgDirListA) { RET(0); }

const HleEntry hle_controls_table[] = {
    { "DialogBoxParamA", c_DialogBoxParamA, NULL }, { "DialogBoxParamW", c_DialogBoxParamW, NULL },
    { "DialogBoxIndirectParamA", c_DialogBoxIndirectParamA, NULL }, { "DialogBoxIndirectParamW", c_DialogBoxIndirectParamW, NULL },
    { "CreateDialogParamA", c_CreateDialogParamA, NULL }, { "CreateDialogParamW", c_CreateDialogParamW, NULL },
    { "CreateDialogIndirectParamA", c_CreateDialogIndirectParamA, NULL }, { "CreateDialogIndirectParamW", c_CreateDialogIndirectParamW, NULL },
    { "EndDialog", c_EndDialog, NULL }, { "DefDlgProcA", c_DefDlgProcA, NULL }, { "DefDlgProcW", c_DefDlgProcW, NULL },
    { "IsDialogMessageA", c_IsDialogMessage, NULL }, { "IsDialogMessageW", c_IsDialogMessage, NULL }, { "IsDialogMessage", c_IsDialogMessage, NULL },
    { "GetDlgItem", c_GetDlgItem, NULL }, { "GetDlgCtrlID", c_GetDlgCtrlID, NULL }, { "SetDlgItemTextA", c_SetDlgItemTextA, NULL },
    { "SetDlgItemTextW", c_SetDlgItemTextW, NULL }, { "GetDlgItemTextA", c_GetDlgItemTextA, NULL }, { "GetDlgItemTextW", c_GetDlgItemTextW, NULL },
    { "SetDlgItemInt", c_SetDlgItemInt, NULL }, { "GetDlgItemInt", c_GetDlgItemInt, NULL }, { "CheckDlgButton", c_CheckDlgButton, NULL },
    { "IsDlgButtonChecked", c_IsDlgButtonChecked, NULL }, { "CheckRadioButton", c_CheckRadioButton, NULL },
    { "SendDlgItemMessageA", c_SendDlgItemMessageA, NULL }, { "SendDlgItemMessageW", c_SendDlgItemMessageW, NULL },
    { "MapDialogRect", c_MapDialogRect, NULL }, { "GetDialogBaseUnits", c_GetDialogBaseUnits, NULL },
    { "GetNextDlgTabItem", c_GetNextDlgTabItem, NULL }, { "GetNextDlgGroupItem", c_GetNextDlgGroupItem, NULL },
    { "DlgDirListA", c_DlgDirListA, NULL },
    { NULL, NULL, NULL }
};
