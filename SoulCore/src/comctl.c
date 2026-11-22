// SoulCore - common controls (comctl32): image lists, header, list view, tree view, tab control,
// toolbar, up-down control and SysLink, drawn with Soul's GDI like the other built-in controls.
// Every structure exchanged with the program (LVITEM, TVITEM, TCITEM, TBBUTTON, NMHDR family...)
// is read and written through layout strings, so 32-bit and 64-bit programs share the code.
#include "gui.h"
#include <ctype.h>
#include <strings.h>

#define LOWORD(v) ((uint32_t)(v) & 0xFFFF)
#define HIWORD(v) (((uint32_t)(v) >> 16) & 0xFFFF)
#define ARGI(n) ((int32_t)hle_arg(st, (n)))
#define MK_CONTROL 8
#define MK_SHIFT 4

// ---- small helpers ------------------------------------------------------------------------------

static char *cc_gstr(uint64_t p, int wide) {
    if (!p || p == (uint64_t)-1 || (g_mode32 && p == 0xFFFFFFFFu)) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
}
static int is_callback_text(uint64_t p) { return p == (uint64_t)-1 || (uint32_t)p == 0xFFFFFFFFu; }
static size_t cc_put(uint64_t dst, size_t cap, const char *s, int wide) {
    if (!dst || !cap) return 0;
    if (!s) s = "";
    if (wide) return u2w(s, -1, (uint16_t *)G2H(dst), cap);
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(G2H(dst), s, n);
    ((char *)G2H(dst))[n] = 0;
    return n;
}
static char *xstrdup(const char *s) { return strdup(s ? s : ""); }
static uint64_t cc_font(Win *w) { return w->font ? w->font : gdi_default_font(); }
static int cc_line(Win *w) { int h; gdi_text_width(cc_font(w), "Ay", 2, &h); return h; }
static int cc_textw(Win *w, const char *s) { int h; return gdi_text_width(cc_font(w), s ? s : "", (int)strlen(s ? s : ""), &h); }
static int parent_unicode(Win *w) { Win *p = win_get(w->parent); return p ? p->unicode : w->unicode; }

typedef struct { uint64_t hdc; } CPaint;
static CPaint cc_begin(Win *w) {
    CPaint p;
    p.hdc = gdi_create_dc(DC_WINDOW, w->hwnd);
    Dc *dc = dc_get(p.hdc);
    if (dc) { dc->font = cc_font(w); dc->bk_mode = 1; }
    w->invalid = 0;
    w->erase = 0;
    return p;
}
static void cc_end(CPaint *p) { gdi_release_dc(p->hdc); }
static void cc_color(uint64_t hdc, uint32_t c) { Dc *dc = dc_get(hdc); if (dc) dc->text_color = c; }
static void cc_text(uint64_t hdc, const char *s, int x0, int y0, int x1, int y1, uint32_t fmt) {
    int32_t r[4] = { x0, y0, x1, y1 };
    gdi_draw_text_dc(hdc, s ? s : "", -1, r, fmt);
}
#define DT_LEFT_VC (0x20 | 4 | 0x800 | 0x8000) // single line, vertically centred, no prefix, end ellipsis
static void cc_check(uint64_t hdc, int x, int y, int checked, int enabled) {
    gdi_fill_rect_px(hdc, x, y, x + 13, y + 13, 0xFFFFFF);
    gdi_frame_rect_px(hdc, x, y, x + 13, y + 13, enabled ? 0x333333 : 0xBCBCBC);
    if (checked) {
        gdi_line_px(hdc, x + 3, y + 6, x + 5, y + 9, 0x333333, 2);
        gdi_line_px(hdc, x + 5, y + 9, x + 10, y + 3, 0x333333, 2);
    }
}
static uint32_t hl_color(void) { return gui_sys_color(13); }   // COLOR_HIGHLIGHT
static void invalidate(Win *w) { win_invalidate_rect(w, NULL, 1); }

// WM_NOTIFY with a structure in guest memory. The layout string starts with NMHDR ("PP4").
typedef struct { uint8_t *p; uint64_t g; const char *L; } Nm;
static Nm nm_new(Win *w, int code, const char *L, size_t extra) {
    Nm n;
    n.L = L;
    n.p = g_calloc(1, gs_size(L) + extra + 16);
    n.g = H2G(n.p);
    gs_set(n.g, L, 0, w->hwnd);
    gs_set(n.g, L, 1, w->id);
    gs_set(n.g, L, 2, (uint32_t)code);
    return n;
}
static uint64_t nm_send(X86State *st, Win *w, Nm *n) {
    uint64_t parent = w->parent, id = w->id;
    if (!win_get(parent)) return 0;
    return win_send(st, parent, WM_NOTIFY, id, n->g);
}
static void nm_free(Nm *n) { g_free(n->p); n->p = NULL; }
static uint64_t notify_simple(X86State *st, Win *w, int code) {
    Nm n = nm_new(w, code, L_NMHDR, 0);
    uint64_t r = nm_send(st, w, &n);
    nm_free(&n);
    return r;
}

// ---- image lists ------------------------------------------------------------------------------------

typedef struct ImgList { int used, cx, cy, count, cap, flags; uint32_t *px; uint32_t bk; } ImgList;
#define IML_MAX 512
#define IML_BASE 0x70000
static ImgList g_iml[IML_MAX];

static ImgList *iml_get(uint64_t h) {
    if (h < IML_BASE || h >= IML_BASE + IML_MAX) return NULL;
    ImgList *l = &g_iml[h - IML_BASE];
    return l->used ? l : NULL;
}
static uint64_t iml_create(int cx, int cy, int flags) {
    if (cx <= 0 || cy <= 0 || cx > 512 || cy > 512) return 0;
    for (int i = 0; i < IML_MAX; i++)
        if (!g_iml[i].used) {
            ImgList *l = &g_iml[i];
            memset(l, 0, sizeof *l);
            l->used = 1; l->cx = cx; l->cy = cy; l->flags = flags; l->bk = 0xFFFFFFFFu;
            return IML_BASE + (uint64_t)i;
        }
    return 0;
}
static uint32_t *iml_slot(ImgList *l, int i) { return l->px + (size_t)i * (size_t)l->cx * (size_t)l->cy; }
static int iml_append(ImgList *l) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->px = realloc(l->px, sizeof(uint32_t) * (size_t)l->cap * (size_t)l->cx * (size_t)l->cy);
    }
    memset(iml_slot(l, l->count), 0, sizeof(uint32_t) * (size_t)l->cx * (size_t)l->cy);
    return l->count++;
}
// Copies images out of a strip of pixels (images side by side). mask: transparent colour (or
// 0xFFFFFFFF for none); maskpx: optional mask bitmap pixels (white = transparent).
static int iml_add_pixels(ImgList *l, const uint32_t *px, int w, int h, uint32_t mask, const uint32_t *maskpx, int at) {
    int n = w / l->cx;
    if (n <= 0) n = 1;
    int first = -1;
    for (int k = 0; k < n; k++) {
        int idx = (at >= 0 && at < l->count && k == 0) ? at : iml_append(l);
        if (first < 0) first = idx;
        uint32_t *d = iml_slot(l, idx);
        for (int y = 0; y < l->cy; y++)
            for (int x = 0; x < l->cx; x++) {
                int sx = k * l->cx + x;
                uint32_t v = (sx < w && y < h) ? px[(size_t)y * (size_t)w + (size_t)sx] : 0;
                if (mask != 0xFFFFFFFFu && (v & 0xFFFFFF) == (mask & 0xFFFFFF)) v = 0;
                if (maskpx && sx < w && y < h && (maskpx[(size_t)y * (size_t)w + (size_t)sx] & 0xFFFFFF) == 0xFFFFFF) v = 0;
                d[(size_t)y * (size_t)l->cx + (size_t)x] = v;
            }
    }
    return first;
}
// A generic document icon for HICONs Soul cannot turn into pixels.
static void iml_generic_icon(ImgList *l, int idx) {
    uint32_t *d = iml_slot(l, idx);
    int cx = l->cx, cy = l->cy;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < cx; x++) {
            int in = x >= cx / 6 && x < cx - cx / 6 && y >= cy / 10 && y < cy - cy / 10;
            int edge = in && (x == cx / 6 || x == cx - cx / 6 - 1 || y == cy / 10 || y == cy - cy / 10 - 1);
            d[(size_t)y * (size_t)cx + (size_t)x] = !in ? 0 : edge ? 0xFF5A7FAAu : (y < cy / 3 ? 0xFF6FA8DCu : 0xFFF4F7FBu);
        }
}
void iml_draw(uint64_t himl, int index, uint64_t hdc, int x, int y) {
    ImgList *l = iml_get(himl);
    if (!l || index < 0 || index >= l->count) return;
    gdi_draw_argb(hdc, x, y, iml_slot(l, index), l->cx, l->cy);
}
int iml_size(uint64_t himl, int *cx, int *cy) {
    ImgList *l = iml_get(himl);
    if (!l) { *cx = *cy = 0; return 0; }
    *cx = l->cx; *cy = l->cy;
    return 1;
}
static int iml_add_bitmap(ImgList *l, uint64_t hbm, uint64_t hmask, uint32_t maskcolor, int at) {
    uint32_t *px = NULL, *mp = NULL;
    int w, h, mw, mh;
    if (!gdi_bitmap_argb(hbm, &px, &w, &h)) return -1;
    if (hmask && gdi_bitmap_argb(hmask, &mp, &mw, &mh) && (mw != w || mh != h)) { free(mp); mp = NULL; }
    if (maskcolor == 0xFF000000u) maskcolor = px[0] & 0xFFFFFF; // CLR_DEFAULT: the top-left pixel
    int r = iml_add_pixels(l, px, w, h, maskcolor, mp, at);
    free(px); free(mp);
    return r;
}

HLE(cc_ImageList_Create) { RET(iml_create(ARGI(0), ARGI(1), ARGI(2))); }
HLE(cc_ImageList_Destroy) { ImgList *l = iml_get(ARG(0)); if (!l) { RET(0); return; } free(l->px); memset(l, 0, sizeof *l); RET(1); }
HLE(cc_ImageList_Add) { ImgList *l = iml_get(ARG(0)); RET(l ? (uint64_t)(int64_t)iml_add_bitmap(l, ARG(1), ARG(2), 0xFFFFFFFFu, -1) : (uint64_t)-1); }
HLE(cc_ImageList_AddMasked) { ImgList *l = iml_get(ARG(0)); RET(l ? (uint64_t)(int64_t)iml_add_bitmap(l, ARG(1), 0, (uint32_t)ARG(2), -1) : (uint64_t)-1); }
HLE(cc_ImageList_Replace) { ImgList *l = iml_get(ARG(0)); RET(l && ARGI(1) >= 0 && ARGI(1) < l->count && iml_add_bitmap(l, ARG(2), ARG(3), 0xFFFFFFFFu, ARGI(1)) >= 0); }
HLE(cc_ImageList_ReplaceIcon) {
    ImgList *l = iml_get(ARG(0));
    if (!l) { RET(-1); return; }
    int i = ARGI(1);
    if (i < 0 || i >= l->count) i = iml_append(l);
    iml_generic_icon(l, i);
    RET(i);
}
HLE(cc_ImageList_Remove) {
    ImgList *l = iml_get(ARG(0));
    if (!l) { RET(0); return; }
    int i = ARGI(1);
    if (i < 0) { l->count = 0; RET(1); return; }
    if (i >= l->count) { RET(0); return; }
    size_t sz = (size_t)l->cx * (size_t)l->cy;
    memmove(iml_slot(l, i), iml_slot(l, i + 1), sizeof(uint32_t) * sz * (size_t)(l->count - i - 1));
    l->count--;
    RET(1);
}
HLE(cc_ImageList_GetImageCount) { ImgList *l = iml_get(ARG(0)); RET(l ? l->count : 0); }
HLE(cc_ImageList_SetImageCount) { ImgList *l = iml_get(ARG(0)); if (!l) { RET(0); return; } int n = ARGI(1); while (l->count < n) iml_append(l); if (n < l->count) l->count = n < 0 ? 0 : n; RET(1); }
HLE(cc_ImageList_GetIconSize) { ImgList *l = iml_get(ARG(0)); if (!l) { RET(0); return; } if (ARG(1)) mem_wr(ARG(1), 4, (uint32_t)l->cx); if (ARG(2)) mem_wr(ARG(2), 4, (uint32_t)l->cy); RET(1); }
HLE(cc_ImageList_SetIconSize) { ImgList *l = iml_get(ARG(0)); if (!l || ARGI(1) <= 0 || ARGI(2) <= 0) { RET(0); return; } l->cx = ARGI(1); l->cy = ARGI(2); l->count = 0; free(l->px); l->px = NULL; l->cap = 0; RET(1); }
HLE(cc_ImageList_SetBkColor) { ImgList *l = iml_get(ARG(0)); if (!l) { RET(0xFFFFFFFFu); return; } uint32_t o = l->bk; l->bk = (uint32_t)ARG(1); RET(o); }
HLE(cc_ImageList_GetBkColor) { ImgList *l = iml_get(ARG(0)); RET(l ? l->bk : 0xFFFFFFFFu); }
HLE(cc_ImageList_Draw) { iml_draw(ARG(0), ARGI(1), ARG(2), ARGI(3), (int)(int32_t)hle_arg(st, 4)); RET(1); }
HLE(cc_ImageList_DrawEx) { iml_draw(ARG(0), ARGI(1), ARG(2), ARGI(3), (int)(int32_t)hle_arg(st, 4)); RET(1); }
HLE(cc_ImageList_GetIcon) { ImgList *l = iml_get(ARG(0)); RET(l && ARGI(1) >= 0 && ARGI(1) < l->count ? 0x60000 + 0x500 + (uint64_t)ARGI(1) : 0); }
HLE(cc_ImageList_Duplicate) {
    ImgList *l = iml_get(ARG(0));
    if (!l) { RET(0); return; }
    uint64_t h = iml_create(l->cx, l->cy, l->flags);
    ImgList *d = iml_get(h);
    if (!d) { RET(0); return; }
    for (int i = 0; i < l->count; i++) { int k = iml_append(d); memcpy(iml_slot(d, k), iml_slot(l, i), sizeof(uint32_t) * (size_t)l->cx * (size_t)l->cy); }
    RET(h);
}
static void load_image_list(X86State *st, int wide) {
    // (hInstance, name, cx, cGrow, crMask, uType, uFlags)
    uint64_t hinst = ARG(0), name = ARG(1);
    int cx = ARGI(2);
    uint32_t mask = (uint32_t)hle_arg(st, 4);
    const void *r = pe_resource(hinst, 2 /* RT_BITMAP */, name, wide, NULL);
    if (!r) { RET(0); return; }
    uint64_t hbm = gdi_bitmap_from_dib(r, NULL);
    uint32_t *px; int w, h;
    if (!gdi_bitmap_argb(hbm, &px, &w, &h)) { gdi_free(hbm); RET(0); return; }
    uint64_t himl = iml_create(cx > 0 ? cx : h, h, 0x20);
    ImgList *l = iml_get(himl);
    if (l) { if (mask == 0xFF000000u) mask = px[0] & 0xFFFFFF; iml_add_pixels(l, px, w, h, mask, NULL, -1); }
    free(px);
    gdi_free(hbm);
    RET(himl);
}
HLE(cc_ImageList_LoadImageA) { load_image_list(st, 0); }
HLE(cc_ImageList_LoadImageW) { load_image_list(st, 1); }
HLE_STUB_RET(cc_ImageList_BeginDrag, 1)
HLE_STUB_RET(cc_ImageList_EndDrag, 0)
HLE_STUB_RET(cc_ImageList_DragEnter, 1)
HLE_STUB_RET(cc_ImageList_DragLeave, 1)
HLE_STUB_RET(cc_ImageList_DragMove, 1)
HLE_STUB_RET(cc_ImageList_SetDragCursorImage, 1)
HLE_STUB_RET(cc_ImageList_SetOverlayImage, 1)

// Built-in toolbar glyphs (IDB_STD_*): simple, recognisable pictures drawn procedurally.
static void std_glyph(uint32_t *d, int cx, int cy, int which) {
    static const uint32_t pal[] = { 0xFF2F6FD0u, 0xFF3C9A3Cu, 0xFFD08A2Fu, 0xFFB03A3Au, 0xFF7A4FC0u, 0xFF4A4A4Au };
    uint32_t c = pal[which % 6];
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < cx; x++) {
            int v = 0;
            switch (which) {
            case 6: case 0: case 1: case 2: // new / cut / copy / paste: a page
                v = x >= 3 && x < cx - 3 && y >= 1 && y < cy - 1; break;
            case 7: v = y >= 4 && y < cy - 2 && x >= 1 && x < cx - 1; break;           // open: a folder
            case 8: v = x >= 2 && x < cx - 2 && y >= 2 && y < cy - 2; break;           // save: a disk
            case 14: case 9: v = y >= 5 && y < cy - 4 && x >= 1 && x < cx - 1; break;   // print
            default: { int dx = x - cx / 2, dy = y - cy / 2; v = dx * dx + dy * dy <= (cx / 2 - 2) * (cx / 2 - 2); break; }
            }
            int border = v && (x == 0 || y == 0 || x == cx - 1 || y == cy - 1);
            d[(size_t)y * (size_t)cx + (size_t)x] = v ? (border ? 0xFF333333u : (which == 6 || which == 1 ? 0xFFF6F6F6u : c)) : 0;
        }
}
static int iml_add_std(ImgList *l, int count) {
    int first = l->count;
    for (int i = 0; i < count; i++) { int k = iml_append(l); std_glyph(iml_slot(l, k), l->cx, l->cy, i); }
    return first;
}

// ---- header (SysHeader32) ----------------------------------------------------------------------------

#define L_HDITEM "44PP444P444P4" // mask cxy pszText hbm cchTextMax fmt lParam iImage iOrder type pvFilter state
typedef struct HdItem { char *text; int cx, fmt, image; uint64_t lparam; } HdItem;
typedef struct HdData { HdItem *it; int n, cap; int pressed, drag, drag_x0, drag_w0; uint64_t iml; } HdData;

static void hd_free(void *p) { HdData *d = p; for (int i = 0; i < d->n; i++) free(d->it[i].text); free(d->it); free(d); }
static HdData *hd_data(Win *w) {
    if (!w->cdata) { w->cdata = calloc(1, sizeof(HdData)); w->cdata_free = hd_free; ((HdData *)w->cdata)->pressed = -1; ((HdData *)w->cdata)->drag = -1; }
    return w->cdata;
}
static int hd_height(Win *w) { return cc_line(w) + 8; }
static void hd_read(HdItem *it, uint64_t g, int wide, int insert) {
    uint32_t mask = (uint32_t)gs_get(g, L_HDITEM, 0);
    if (mask & 1) it->cx = (int32_t)gs_get(g, L_HDITEM, 1);
    if (mask & 2) { uint64_t p = gs_get(g, L_HDITEM, 2); char *t = cc_gstr(p, wide); free(it->text); it->text = t ? t : xstrdup(""); }
    if (mask & 4) it->fmt = (int32_t)gs_get(g, L_HDITEM, 5);
    if (mask & 8) it->lparam = gs_get(g, L_HDITEM, 6);
    if (mask & 0x20) it->image = (int32_t)gs_get(g, L_HDITEM, 7);
    if (insert && !it->text) it->text = xstrdup("");
}
static int hd_insert(Win *w, int at, const HdItem *src) {
    HdData *d = hd_data(w);
    if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 8; d->it = realloc(d->it, sizeof(HdItem) * (size_t)d->cap); }
    if (at < 0 || at > d->n) at = d->n;
    memmove(&d->it[at + 1], &d->it[at], sizeof(HdItem) * (size_t)(d->n - at));
    d->it[at] = *src;
    d->n++;
    return at;
}
static int hd_left(HdData *d, int i) { int x = 0; for (int k = 0; k < i && k < d->n; k++) x += d->it[k].cx; return x; }
static void hd_paint(Win *w) {
    HdData *d = hd_data(w);
    CPaint p = cc_begin(w);
    gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, 0xFFFFFF);
    int x = 0;
    for (int i = 0; i < d->n; i++) {
        HdItem *it = &d->it[i];
        if (i == d->pressed) gdi_fill_rect_px(p.hdc, x, 0, x + it->cx, w->h, 0xF7E4CC);
        cc_color(p.hdc, 0x1E1E1E);
        int tx = x + 6, ix1 = x + it->cx - 6;
        if (it->image >= 0 && d->iml && (it->fmt & 0x800)) { int icx, icy; iml_size(d->iml, &icx, &icy); iml_draw(d->iml, it->image, p.hdc, tx, (w->h - icy) / 2); tx += icx + 3; }
        uint32_t al = (it->fmt & 3) == 1 ? 2 : (it->fmt & 3) == 2 ? 1 : 0;
        cc_text(p.hdc, it->text, tx, 0, ix1 - ((it->fmt & 0x600) ? 10 : 0), w->h, DT_LEFT_VC | al);
        if (it->fmt & 0x600) { // sort arrow
            int ax = x + it->cx - 12, ay = w->h / 2;
            for (int k = 0; k < 4; k++) {
                int yy = (it->fmt & 0x400) ? ay + 2 - k : ay - 2 + k;
                gdi_line_px(p.hdc, ax + 3 - k, yy, ax + 4 + k, yy, 0x606060, 1);
            }
        }
        gdi_line_px(p.hdc, x + it->cx - 1, 3, x + it->cx - 1, w->h - 3, 0xE5E5E5, 1);
        x += it->cx;
    }
    gdi_line_px(p.hdc, 0, w->h - 1, w->w, w->h - 1, 0xE5E5E5, 1);
    cc_end(&p);
}
static uint64_t hd_notify(X86State *st, Win *w, int code_a, int code_w, int item) {
    int u = parent_unicode(w);
    Nm n = nm_new(w, u ? code_w : code_a, "PP4|44P", 0); // NMHEADER: hdr iItem iButton pitem
    gs_set(n.g, n.L, 3, (uint32_t)item);
    uint64_t r = nm_send(st, w, &n);
    nm_free(&n);
    return r;
}
static uint64_t header_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    HdData *d = hd_data(w);
    GuiState *g = gui();
    switch (msg) {
    case WM_PAINT: hd_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        int x = (int16_t)LOWORD(lp);
        for (int i = 0, ex = 0; i < d->n; i++) {
            ex += d->it[i].cx;
            if (x >= ex - 4 && x <= ex + 4) { d->drag = i; d->drag_x0 = x; d->drag_w0 = d->it[i].cx; g->capture = w->hwnd; return 0; }
        }
        for (int i = 0, ex = 0; i < d->n; i++) { if (x >= ex && x < ex + d->it[i].cx) { d->pressed = i; g->capture = w->hwnd; invalidate(w); break; } ex += d->it[i].cx; }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (d->drag >= 0 && d->drag < d->n) {
            int nw = d->drag_w0 + (int16_t)LOWORD(lp) - d->drag_x0;
            d->it[d->drag].cx = nw < 8 ? 8 : nw;
            invalidate(w);
            uint64_t h = w->hwnd;
            hd_notify(st, w, -301, -321, d->drag); // HDN_ITEMCHANGED
            if (!win_get(h)) return 0;
        }
        return 0;
    case WM_LBUTTONUP: {
        if (g->capture == w->hwnd) g->capture = 0;
        if (d->drag >= 0) { int i = d->drag; d->drag = -1; hd_notify(st, w, -307, -327, i); return 0; } // HDN_ENDTRACK
        int i = d->pressed;
        d->pressed = -1;
        invalidate(w);
        int x = (int16_t)LOWORD(lp);
        if (i >= 0 && i < d->n && x >= hd_left(d, i) && x < hd_left(d, i) + d->it[i].cx) hd_notify(st, w, -302, -322, i); // HDN_ITEMCLICK
        return 0;
    }
    case 0x1200: return (uint64_t)d->n;                          // HDM_GETITEMCOUNT
    case 0x1201: case 0x120A: {                                  // HDM_INSERTITEM
        HdItem it = { 0 };
        it.image = -1;
        hd_read(&it, lp, msg == 0x120A, 1);
        int r = hd_insert(w, (int)(int32_t)wp, &it);
        invalidate(w);
        return (uint64_t)r;
    }
    case 0x1202: {                                               // HDM_DELETEITEM
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= d->n) return 0;
        free(d->it[i].text);
        memmove(&d->it[i], &d->it[i + 1], sizeof(HdItem) * (size_t)(d->n - i - 1));
        d->n--;
        invalidate(w);
        return 1;
    }
    case 0x1203: case 0x120B: {                                  // HDM_GETITEM
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= d->n || !lp) return 0;
        uint32_t mask = (uint32_t)gs_get(lp, L_HDITEM, 0);
        if (mask & 1) gs_set(lp, L_HDITEM, 1, (uint32_t)d->it[i].cx);
        if (mask & 2) cc_put(gs_get(lp, L_HDITEM, 2), (size_t)(int32_t)gs_get(lp, L_HDITEM, 4), d->it[i].text, msg == 0x120B);
        if (mask & 4) gs_set(lp, L_HDITEM, 5, (uint32_t)d->it[i].fmt);
        if (mask & 8) gs_set(lp, L_HDITEM, 6, d->it[i].lparam);
        if (mask & 0x20) gs_set(lp, L_HDITEM, 7, (uint32_t)d->it[i].image);
        if (mask & 0x80) gs_set(lp, L_HDITEM, 8, (uint32_t)i);
        return 1;
    }
    case 0x1204: case 0x120C: {                                  // HDM_SETITEM
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= d->n || !lp) return 0;
        hd_read(&d->it[i], lp, msg == 0x120C, 0);
        invalidate(w);
        return 1;
    }
    case 0x1205: {                                               // HDM_LAYOUT (HDLAYOUT: RECT*, WINDOWPOS*)
        uint64_t prc = gs_get(lp, "PP", 0), pwp = gs_get(lp, "PP", 1);
        int32_t r[4]; memcpy(r, G2H(prc), 16);
        int hh = (w->style & 8) ? 0 : hd_height(w); // HDS_HIDDEN
        gs_set(pwp, L_WINDOWPOS, 0, w->hwnd);
        gs_set(pwp, L_WINDOWPOS, 2, (uint32_t)r[0]); gs_set(pwp, L_WINDOWPOS, 3, (uint32_t)r[1]);
        gs_set(pwp, L_WINDOWPOS, 4, (uint32_t)(r[2] - r[0])); gs_set(pwp, L_WINDOWPOS, 5, (uint32_t)hh);
        gs_set(pwp, L_WINDOWPOS, 6, 0);
        r[1] += hh;
        memcpy(G2H(prc), r, 16);
        return 1;
    }
    case 0x1207: {                                               // HDM_GETITEMRECT
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= d->n || !lp) return 0;
        int32_t r[4] = { hd_left(d, i), 0, hd_left(d, i) + d->it[i].cx, w->h };
        memcpy(G2H(lp), r, 16);
        return 1;
    }
    case 0x1206: {                                               // HDM_HITTEST (HDHITTESTINFO: pt, flags, iItem)
        int32_t x = (int32_t)mem_rd(lp, 4);
        for (int i = 0, ex = 0; i < d->n; i++) { if (x >= ex && x < ex + d->it[i].cx) { mem_wr(lp + 8, 4, 2); mem_wr(lp + 12, 4, (uint32_t)i); return (uint64_t)i; } ex += d->it[i].cx; }
        mem_wr(lp + 8, 4, 1);
        return (uint64_t)-1;
    }
    case 0x1208: { uint64_t o = d->iml; d->iml = lp; return o; } // HDM_SETIMAGELIST
    case 0x1209: return d->iml;
    case 0x120F: return wp;                                      // HDM_ORDERTOINDEX
    case 0x1211: { for (int i = 0; i < d->n && i < (int)wp; i++) mem_wr(lp + 4 * (uint64_t)i, 4, (uint32_t)i); return 1; } // HDM_GETORDERARRAY
    case 0x1212: return 1;
    case 0x2005: case 0x2006: case 0x2007: case 0x2008: return msg == 0x2008 ? 6 : 0;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- list view (SysListView32) -------------------------------------------------------------------

#define L_LVITEM "44444P44P4"     // mask iItem iSubItem state stateMask pszText cchTextMax iImage lParam iIndent
#define L_LVCOLUMN "444P444"      // mask fmt cx pszText cchTextMax iSubItem iImage
#define L_NMLISTVIEW "PP4|4444444P" // hdr iItem iSubItem uNewState uOldState uChanged ptAction(x,y) lParam
#define L_NMITEMACTIVATE "PP4|4444444P4"
#define L_LVDISPINFO "PP4|44444P44P4" // hdr + LVITEM
#define LVIS_FOCUSED 1u
#define LVIS_SELECTED 2u

typedef struct LvItem { char **sub; int nsub; int image; uint32_t state; uint64_t lparam; int text_cb; } LvItem;
typedef struct LvData {
    LvItem *it; int n, cap;
    uint64_t header;
    uint64_t iml[3];            // normal, small, state
    uint32_t ex;                // extended style
    uint32_t bk, fg;
    int top;                    // first visible row (report/list) or row of cells (icon view)
    int mark;                   // selection mark (shift-click anchor)
    int virt;                   // LVS_OWNERDATA item count
    uint32_t *vstate;           // states of virtual items
} LvData;

static void lv_free(void *p) {
    LvData *d = p;
    for (int i = 0; i < d->n; i++) { for (int k = 0; k < d->it[i].nsub; k++) free(d->it[i].sub[k]); free(d->it[i].sub); }
    free(d->it); free(d->vstate); free(d);
}
static LvData *lv_data(Win *w) {
    if (!w->cdata) {
        LvData *d = calloc(1, sizeof(LvData));
        d->bk = 0xFFFFFF; d->fg = 0x000000; d->mark = -1;
        w->cdata = d; w->cdata_free = lv_free;
    }
    return w->cdata;
}
static int lv_view(Win *w) { return (int)(w->style & 3); } // 0 icon, 1 report, 2 small icon, 3 list
static int lv_virtual(Win *w) { return (w->style & 0x1000) != 0; }
static int lv_count(Win *w) { LvData *d = lv_data(w); return lv_virtual(w) ? d->virt : d->n; }
static Win *lv_header(Win *w) { return win_get(lv_data(w)->header); }
static HdData *lv_cols(Win *w) { Win *h = lv_header(w); return h ? hd_data(h) : NULL; }
static int lv_header_h(Win *w) { Win *h = lv_header(w); return (lv_view(w) == 1 && h && h->visible) ? h->h : 0; }
static int lv_row_h(Win *w) {
    LvData *d = lv_data(w);
    int h = cc_line(w) + 5, cx, cy;
    if (iml_size(d->iml[1], &cx, &cy) && cy + 2 > h) h = cy + 2;
    if ((d->ex & 4) && h < 17) h = 17;
    return h;
}
static int lv_cell_w(Win *w) { (void)w; return 84; }
static int lv_cell_h(Win *w) { LvData *d = lv_data(w); int cx, cy; if (!iml_size(d->iml[0], &cx, &cy)) cy = 32; return cy + 2 * cc_line(w) + 10; }
static int lv_rows_visible(Win *w) {
    int area = w->h - lv_header_h(w);
    int rh = lv_view(w) == 0 ? lv_cell_h(w) : lv_row_h(w);
    int n = area / (rh > 0 ? rh : 1);
    return n < 1 ? 1 : n;
}
static int lv_per_row(Win *w) { if (lv_view(w) != 0) return 1; int n = w->w / lv_cell_w(w); return n < 1 ? 1 : n; }
static uint32_t lv_state(Win *w, int i) {
    LvData *d = lv_data(w);
    if (i < 0 || i >= lv_count(w)) return 0;
    if (lv_virtual(w)) return d->vstate ? d->vstate[i] : 0;
    return d->it[i].state;
}
static void lv_raw_set_state(Win *w, int i, uint32_t st_) {
    LvData *d = lv_data(w);
    if (lv_virtual(w)) { if (d->vstate) d->vstate[i] = st_; return; }
    d->it[i].state = st_;
}

// Text / image of an item: stored, or asked from the parent (LVS_OWNERDATA, LPSTR_TEXTCALLBACK).
static char *lv_text(X86State *st, Win *w, int i, int sub, int *image) {
    LvData *d = lv_data(w);
    int cb = lv_virtual(w) || (i < d->n && d->it[i].text_cb && sub == 0);
    if (!lv_virtual(w) && i < d->n && image) *image = d->it[i].image;
    if (!cb) {
        if (i < d->n && sub < d->it[i].nsub && d->it[i].sub[sub]) return xstrdup(d->it[i].sub[sub]);
        return xstrdup("");
    }
    int u = parent_unicode(w);
    Nm n = nm_new(w, u ? -177 : -150, L_LVDISPINFO, 0); // LVN_GETDISPINFOW / A
    uint16_t *buf = g_calloc(1, 1040);
    gs_set(n.g, n.L, 3, 1u | (image ? 2u : 0)); // LVIF_TEXT (| LVIF_IMAGE)
    gs_set(n.g, n.L, 4, (uint32_t)i); gs_set(n.g, n.L, 5, (uint32_t)sub);
    gs_set(n.g, n.L, 8, H2G(buf)); gs_set(n.g, n.L, 9, 260);
    gs_set(n.g, n.L, 10, (uint32_t)-1);
    uint64_t h = w->hwnd;
    nm_send(st, w, &n);
    char *t = NULL;
    if (win_get(h)) {
        uint64_t p = gs_get(n.g, n.L, 8);
        t = cc_gstr(p, u);
        if (image) { int32_t im = (int32_t)gs_get(n.g, n.L, 10); if (im >= 0) *image = im; }
    }
    g_free(buf);
    nm_free(&n);
    return t ? t : xstrdup("");
}

static uint64_t lv_notify_item(X86State *st, Win *w, int code, int item, int sub, uint32_t nst, uint32_t ost, uint32_t changed) {
    LvData *d = lv_data(w);
    Nm n = nm_new(w, code, L_NMLISTVIEW, 0);
    gs_set(n.g, n.L, 3, (uint32_t)item); gs_set(n.g, n.L, 4, (uint32_t)sub);
    gs_set(n.g, n.L, 5, nst); gs_set(n.g, n.L, 6, ost); gs_set(n.g, n.L, 7, changed);
    if (item >= 0 && item < d->n && !lv_virtual(w)) gs_set(n.g, n.L, 10, d->it[item].lparam);
    uint64_t r = nm_send(st, w, &n);
    nm_free(&n);
    return r;
}
static uint64_t lv_notify_activate(X86State *st, Win *w, int code, int item, int sub, int x, int y) {
    Nm n = nm_new(w, code, L_NMITEMACTIVATE, 0);
    gs_set(n.g, n.L, 3, (uint32_t)item); gs_set(n.g, n.L, 4, (uint32_t)sub);
    gs_set(n.g, n.L, 8, (uint32_t)x); gs_set(n.g, n.L, 9, (uint32_t)y);
    uint64_t r = nm_send(st, w, &n);
    nm_free(&n);
    return r;
}
// Changes the state of an item with LVN_ITEMCHANGING / LVN_ITEMCHANGED. Returns 0 if vetoed.
static int lv_set_state(X86State *st, Win *w, int i, uint32_t state, uint32_t mask) {
    uint32_t old = lv_state(w, i), nw = (old & ~mask) | (state & mask);
    if (nw == old) return 1;
    uint64_t h = w->hwnd;
    if (!lv_virtual(w) && lv_notify_item(st, w, -100, i, 0, nw, old, 8)) return 0; // LVN_ITEMCHANGING vetoed
    if (!(w = win_get(h)) || i >= lv_count(w)) return 0;
    if (nw & LVIS_FOCUSED) for (int k = 0; k < lv_count(w); k++) if (k != i && (lv_state(w, k) & LVIS_FOCUSED)) lv_raw_set_state(w, k, lv_state(w, k) & ~LVIS_FOCUSED);
    lv_raw_set_state(w, i, nw);
    invalidate(w);
    lv_notify_item(st, w, -101, i, 0, nw, old, 8); // LVN_ITEMCHANGED
    return 1;
}
static void lv_select_only(X86State *st, Win *w, int i) {
    uint64_t h = w->hwnd;
    int n = lv_count(w);
    for (int k = 0; k < n; k++) {
        if (!(w = win_get(h))) return;
        if (k != i && (lv_state(w, k) & LVIS_SELECTED)) lv_set_state(st, w, k, 0, LVIS_SELECTED);
    }
    if (!(w = win_get(h))) return;
    if (i >= 0 && i < lv_count(w)) lv_set_state(st, w, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
}
static void lv_ensure_visible(Win *w, int i) {
    LvData *d = lv_data(w);
    int per = lv_per_row(w), row = i / per, vis = lv_rows_visible(w);
    if (row < d->top) d->top = row;
    if (row >= d->top + vis) d->top = row - vis + 1;
    if (d->top < 0) d->top = 0;
}
static void lv_clamp_top(Win *w) {
    LvData *d = lv_data(w);
    int per = lv_per_row(w), rows = (lv_count(w) + per - 1) / per, vis = lv_rows_visible(w);
    if (d->top > rows - vis) d->top = rows - vis;
    if (d->top < 0) d->top = 0;
}
// Item rectangle in client coordinates (LVIR_BOUNDS).
static void lv_item_rect(Win *w, int i, int32_t *r) {
    LvData *d = lv_data(w);
    int hh = lv_header_h(w);
    if (lv_view(w) == 0) {
        int per = lv_per_row(w), cw = lv_cell_w(w), ch = lv_cell_h(w);
        int row = i / per - d->top, col = i % per;
        r[0] = col * cw; r[1] = hh + row * ch; r[2] = r[0] + cw; r[3] = r[1] + ch;
        return;
    }
    int rh = lv_row_h(w);
    int width = w->w;
    HdData *c = lv_cols(w);
    if (lv_view(w) == 1 && c && c->n) { int t = hd_left(c, c->n); if (t > width) width = t; }
    r[0] = 0; r[1] = hh + (i - d->top) * rh; r[2] = width; r[3] = r[1] + rh;
}
static int lv_hit(Win *w, int x, int y, int *sub, int *on_check) {
    LvData *d = lv_data(w);
    int hh = lv_header_h(w), n = lv_count(w);
    if (sub) *sub = 0;
    if (on_check) *on_check = 0;
    if (y < hh) return -1;
    int i;
    if (lv_view(w) == 0) {
        int per = lv_per_row(w), cw = lv_cell_w(w), ch = lv_cell_h(w);
        int col = x / cw;
        if (col >= per) return -1;
        i = (d->top + (y - hh) / ch) * per + col;
    } else {
        i = d->top + (y - hh) / lv_row_h(w);
    }
    if (i < 0 || i >= n) return -1;
    if (sub && lv_view(w) == 1) { HdData *c = lv_cols(w); if (c) for (int k = 0, ex = 0; k < c->n; k++) { if (x >= ex && x < ex + c->it[k].cx) { *sub = k; break; } ex += c->it[k].cx; } }
    if (on_check && (d->ex & 4) && lv_view(w) != 0 && x >= 2 && x < 18) *on_check = 1;
    return i;
}
static void lv_paint(X86State *st, Win *w) {
    LvData *d = lv_data(w);
    uint64_t h = w->hwnd;
    CPaint p = cc_begin(w);
    uint32_t bk = d->bk == 0xFFFFFFFFu ? 0xFFFFFF : d->bk;
    gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, bk);
    int focused = gui()->focus == w->hwnd;
    int n = lv_count(w), view = lv_view(w);
    int hh = lv_header_h(w);
    if (view == 0) {
        int per = lv_per_row(w), cw = lv_cell_w(w), ch = lv_cell_h(w), icx = 32, icy = 32;
        iml_size(d->iml[0], &icx, &icy);
        for (int i = d->top * per; i < n; i++) {
            int row = i / per - d->top, col = i % per;
            int x = col * cw, y = hh + row * ch;
            if (y >= w->h) break;
            int image = -1;
            char *t = lv_text(st, w, i, 0, &image);
            if (!(w = win_get(h))) { free(t); cc_end(&p); return; }
            uint32_t s = lv_state(w, i);
            if (d->iml[0] && image >= 0) iml_draw(d->iml[0], image, p.hdc, x + (cw - icx) / 2, y + 4);
            int ty = y + icy + 6;
            if (s & LVIS_SELECTED) gdi_fill_rect_px(p.hdc, x + 2, ty, x + cw - 2, y + ch - 2, focused ? hl_color() : 0xD9D9D9);
            cc_color(p.hdc, (s & LVIS_SELECTED) && focused ? 0xFFFFFF : d->fg);
            cc_text(p.hdc, t, x + 2, ty, x + cw - 2, y + ch - 2, 1 | 0x10 | 0x800 | 0x40000); // centred, word break, no prefix, end ellipsis
            free(t);
        }
    } else {
        int rh = lv_row_h(w);
        HdData *cols = view == 1 ? lv_cols(w) : NULL;
        int ncol = cols && cols->n ? cols->n : 1;
        int scx = 16, scy = 16;
        int has_small = iml_size(d->iml[1], &scx, &scy);
        int fullrow = (d->ex & 0x20) != 0;
        for (int i = d->top, row = 0; i < n; i++, row++) {
            int y = hh + row * rh;
            if (y >= w->h) break;
            uint32_t s = lv_state(w, i);
            int sel = (s & LVIS_SELECTED) && (focused || (w->style & 8));
            int x = 0;
            for (int c = 0; c < ncol; c++) {
                int cx = cols && cols->n ? cols->it[c].cx : w->w;
                int image = -1;
                char *t = lv_text(st, w, i, c, c == 0 ? &image : NULL);
                if (!(w = win_get(h))) { free(t); cc_end(&p); return; }
                int tx = x + 4;
                if (c == 0) {
                    if (d->ex & 4) { uint32_t chk = (s >> 12) & 15; cc_check(p.hdc, x + 3, y + (rh - 13) / 2, chk == 2, w->enabled); tx += 18; }
                    if (has_small && image >= 0) { iml_draw(d->iml[1], image, p.hdc, tx, y + (rh - scy) / 2); tx += scx + 3; }
                    else if (has_small) tx += scx + 3;
                }
                int tw = cc_textw(w, t);
                if (sel && (c == 0 || fullrow)) {
                    int hx0 = c == 0 ? tx - 2 : x, hx1 = fullrow ? x + cx : tx + tw + 4;
                    if (hx1 > x + cx) hx1 = x + cx;
                    gdi_fill_rect_px(p.hdc, hx0, y, hx1, y + rh, focused ? hl_color() : 0xD9D9D9);
                }
                cc_color(p.hdc, sel && focused && (c == 0 || fullrow) ? 0xFFFFFF : (w->enabled ? d->fg : 0x6D6D6D));
                uint32_t al = cols && cols->n ? ((cols->it[c].fmt & 3) == 1 ? 2 : (cols->it[c].fmt & 3) == 2 ? 1 : 0) : 0;
                cc_text(p.hdc, t, tx, y, x + cx - 4, y + rh, DT_LEFT_VC | al);
                free(t);
                x += cx;
                if (view != 1) break;
            }
            if (d->ex & 1) gdi_line_px(p.hdc, 0, y + rh - 1, x, y + rh - 1, 0xF0F0F0, 1); // LVS_EX_GRIDLINES
            if ((s & LVIS_FOCUSED) && focused && !sel) gdi_frame_rect_px(p.hdc, 1, y, (fullrow || view != 1) ? x : (cols && cols->n ? cols->it[0].cx : w->w), y + rh, 0xA0A0A0);
        }
    }
    if ((w->style & WS_BORDER) || (w->exstyle & WS_EX_CLIENTEDGE)) gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, focused ? 0xD77800 : 0x828790);
    cc_end(&p);
}
static void lv_layout_header(Win *w) {
    Win *hw = lv_header(w);
    if (!hw) return;
    int show = lv_view(w) == 1 && !(w->style & 0x4000);
    hw->x = 0; hw->y = 0; hw->w = w->w; hw->h = hd_height(hw);
    hw->visible = show;
    invalidate(hw);
}
static void lv_create_header(X86State *st, Win *w) {
    LvData *d = lv_data(w);
    if (d->header) return;
    char *cls = g_strdup("SysHeader32");
    uint64_t hh = win_create(st, 0, H2G(cls), 0, WS_CHILD | 0x2 /* HDS_BUTTONS */, 0, 0, w->w, cc_line(w) + 8, w->hwnd, 0, w->hinst, 0, 0);
    g_free(cls);
    if (!(w = win_get(w->hwnd))) return;
    d = lv_data(w);
    d->header = hh;
    Win *hw = win_get(hh);
    if (hw) hw->font = w->font;
    lv_layout_header(w);
}
static int lv_insert(Win *w, int at) {
    LvData *d = lv_data(w);
    if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 16; d->it = realloc(d->it, sizeof(LvItem) * (size_t)d->cap); }
    if (at < 0 || at > d->n) at = d->n;
    memmove(&d->it[at + 1], &d->it[at], sizeof(LvItem) * (size_t)(d->n - at));
    memset(&d->it[at], 0, sizeof(LvItem));
    d->it[at].image = -1;
    d->n++;
    return at;
}
static void lv_set_sub(LvItem *it, int sub, char *t) {
    if (sub >= it->nsub) {
        it->sub = realloc(it->sub, sizeof(char *) * (size_t)(sub + 1));
        for (int k = it->nsub; k <= sub; k++) it->sub[k] = NULL;
        it->nsub = sub + 1;
    }
    free(it->sub[sub]);
    it->sub[sub] = t;
}
static int lv_cmp_text(const void *a, const void *b) {
    const LvItem *x = a, *y = b;
    return strcasecmp(x->nsub && x->sub[0] ? x->sub[0] : "", y->nsub && y->sub[0] ? y->sub[0] : "");
}
// LVM_SORTITEMS: insertion sort calling the program's comparison function.
static void lv_sort(X86State *st, Win *w, uint64_t fn, uint64_t lparam_sort, int by_index) {
    uint64_t h = w->hwnd;
    LvData *d = lv_data(w);
    for (int i = 1; i < d->n; i++) {
        for (int j = i; j > 0; j--) {
            d = lv_data(w);
            uint64_t a[3] = { by_index ? (uint64_t)(j - 1) : d->it[j - 1].lparam, by_index ? (uint64_t)j : d->it[j].lparam, lparam_sort };
            int r = (int)(int32_t)soul_guest_call(st, fn, 3, a);
            if (!(w = win_get(h))) return;
            d = lv_data(w);
            if (r <= 0) break;
            LvItem t = d->it[j]; d->it[j] = d->it[j - 1]; d->it[j - 1] = t;
        }
    }
}
static void lv_read_item(X86State *st, Win *w, uint64_t g, int wide, int at_insert) {
    (void)st;
    LvData *d = lv_data(w);
    uint32_t mask = (uint32_t)gs_get(g, L_LVITEM, 0);
    int i = (int32_t)gs_get(g, L_LVITEM, 1), sub = (int32_t)gs_get(g, L_LVITEM, 2);
    if (at_insert) i = lv_insert(w, i);
    if (i < 0 || i >= d->n) return;
    LvItem *it = &d->it[i];
    if (mask & 1) {
        uint64_t p = gs_get(g, L_LVITEM, 5);
        if (is_callback_text(p)) { if (sub == 0) it->text_cb = 1; }
        else { char *t = cc_gstr(p, wide); lv_set_sub(it, sub, t ? t : xstrdup("")); if (sub == 0) it->text_cb = 0; }
    }
    if (sub == 0) {
        if (mask & 2) it->image = (int32_t)gs_get(g, L_LVITEM, 7);
        if (mask & 4) it->lparam = gs_get(g, L_LVITEM, 8);
        if (mask & 8) { uint32_t sm = (uint32_t)gs_get(g, L_LVITEM, 4); it->state = (it->state & ~sm) | ((uint32_t)gs_get(g, L_LVITEM, 3) & sm); }
    }
    if (at_insert && !it->nsub) lv_set_sub(it, 0, xstrdup(""));
    if (at_insert && (d->ex & 4) && !(it->state & 0xF000)) it->state |= 0x1000; // LVS_EX_CHECKBOXES: unchecked
    if (at_insert) gs_set(g, L_LVITEM, 1, (uint32_t)i);
}
static uint64_t lv_column_msg(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp) {
    Win *hw = lv_header(w);
    if (!hw) return (uint64_t)-1;
    HdData *c = hd_data(hw);
    int wide = msg >= 0x105F;
    switch (msg) {
    case 0x101B: case 0x1061: { // LVM_INSERTCOLUMN
        HdItem it = { 0 };
        it.image = -1;
        uint32_t mask = (uint32_t)gs_get(lp, L_LVCOLUMN, 0);
        it.cx = (mask & 2) ? (int32_t)gs_get(lp, L_LVCOLUMN, 2) : 80;
        it.fmt = (mask & 1) ? (int32_t)gs_get(lp, L_LVCOLUMN, 1) : 0;
        char *t = (mask & 4) ? cc_gstr(gs_get(lp, L_LVCOLUMN, 3), wide) : NULL;
        it.text = t ? t : xstrdup("");
        int r = hd_insert(hw, (int)(int32_t)wp, &it);
        invalidate(hw); invalidate(w);
        return (uint64_t)r;
    }
    case 0x101A: case 0x1060: { // LVM_SETCOLUMN
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= c->n) return 0;
        uint32_t mask = (uint32_t)gs_get(lp, L_LVCOLUMN, 0);
        if (mask & 2) c->it[i].cx = (int32_t)gs_get(lp, L_LVCOLUMN, 2);
        if (mask & 1) c->it[i].fmt = (int32_t)gs_get(lp, L_LVCOLUMN, 1);
        if (mask & 4) { char *t = cc_gstr(gs_get(lp, L_LVCOLUMN, 3), wide); free(c->it[i].text); c->it[i].text = t ? t : xstrdup(""); }
        invalidate(hw); invalidate(w);
        return 1;
    }
    case 0x1019: case 0x105F: { // LVM_GETCOLUMN
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= c->n) return 0;
        uint32_t mask = (uint32_t)gs_get(lp, L_LVCOLUMN, 0);
        if (mask & 2) gs_set(lp, L_LVCOLUMN, 2, (uint32_t)c->it[i].cx);
        if (mask & 1) gs_set(lp, L_LVCOLUMN, 1, (uint32_t)c->it[i].fmt);
        if (mask & 4) cc_put(gs_get(lp, L_LVCOLUMN, 3), (size_t)(int32_t)gs_get(lp, L_LVCOLUMN, 4), c->it[i].text, wide);
        if (mask & 8) gs_set(lp, L_LVCOLUMN, 5, (uint32_t)i);
        return 1;
    }
    case 0x101C: { // LVM_DELETECOLUMN
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= c->n) return 0;
        free(c->it[i].text);
        memmove(&c->it[i], &c->it[i + 1], sizeof(HdItem) * (size_t)(c->n - i - 1));
        c->n--;
        invalidate(hw); invalidate(w);
        return 1;
    }
    case 0x101D: { int i = (int)(int32_t)wp; return i >= 0 && i < c->n ? (uint64_t)c->it[i].cx : 0; } // LVM_GETCOLUMNWIDTH
    case 0x101E: { // LVM_SETCOLUMNWIDTH
        int i = (int)(int32_t)wp, cx = (int)(int16_t)LOWORD(lp);
        if (i < 0 || i >= c->n) return 0;
        if (cx == -1 || cx == -2) { // LVSCW_AUTOSIZE(_USEHEADER)
            int best = cx == -2 ? cc_textw(w, c->it[i].text) + 16 : 0;
            uint64_t h = w->hwnd;
            for (int k = 0; k < lv_count(w) && k < 2000; k++) {
                char *t = lv_text(st, w, k, i, NULL);
                if (!(w = win_get(h))) { free(t); return 0; }
                int tw = cc_textw(w, t) + (i == 0 ? 30 : 12);
                if (tw > best) best = tw;
                free(t);
            }
            if (cx == -2 && i == c->n - 1) { int left = hd_left(c, i); if (w->w - left > best) best = w->w - left; }
            cx = best < 20 ? 20 : best;
        }
        c->it[i].cx = cx;
        invalidate(hw); invalidate(w);
        return 1;
    }
    default: return 0;
    }
}
static uint64_t listview_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    LvData *d = lv_data(w);
    GuiState *g = gui();
    uint64_t h = w->hwnd;
    switch (msg) {
    case WM_CREATE: lv_create_header(st, w); return 0;
    case WM_SIZE: lv_layout_header(w); lv_clamp_top(w); invalidate(w); return 0;
    case WM_STYLECHANGED: lv_layout_header(w); invalidate(w); return 0;
    case WM_SETFONT: w->font = wp; { Win *hw = lv_header(w); if (hw) { hw->font = wp; hw->h = hd_height(hw); } } invalidate(w); return 0;
    case WM_PAINT: lv_paint(st, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        invalidate(w);
        notify_simple(st, w, msg == WM_SETFOCUS ? -7 : -8);
        return 0;
    case WM_GETDLGCODE: return 1 | 0x80;
    case WM_MOUSEWHEEL:
        d->top -= (int16_t)HIWORD(wp) / 40;
        lv_clamp_top(w);
        invalidate(w);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: {
        if (g->focus != h) win_set_focus(st, h);
        if (!(w = win_get(h))) return 0;
        d = lv_data(w);
        int x = (int16_t)LOWORD(lp), y = (int16_t)HIWORD(lp), sub, chk;
        int i = lv_hit(w, x, y, &sub, &chk);
        if (msg == WM_LBUTTONDOWN && chk && i >= 0) {
            uint32_t s = lv_state(w, i), c = (s >> 12) & 15;
            lv_set_state(st, w, i, (c == 2 ? 1u : 2u) << 12, 0xF000);
            return 0;
        }
        if (i >= 0) {
            if ((wp & MK_CONTROL) && !(w->style & 4)) lv_set_state(st, w, i, lv_state(w, i) ^ LVIS_SELECTED, LVIS_SELECTED);
            else if ((wp & MK_SHIFT) && !(w->style & 4) && d->mark >= 0) {
                int a = d->mark < i ? d->mark : i, b = d->mark < i ? i : d->mark;
                for (int k = 0; k < lv_count(w) && (w = win_get(h)); k++) lv_set_state(st, w, k, (k >= a && k <= b) ? LVIS_SELECTED : 0, LVIS_SELECTED);
            } else if (!(msg == WM_RBUTTONDOWN && (lv_state(w, i) & LVIS_SELECTED))) {
                lv_select_only(st, w, i);
                if ((w = win_get(h))) lv_data(w)->mark = i;
            }
        } else if (msg != WM_RBUTTONDOWN && !(w->style & 8)) {
            lv_select_only(st, w, -1);
        }
        if (!(w = win_get(h))) return 0;
        if (msg == WM_LBUTTONDOWN) lv_notify_activate(st, w, -2, i, sub, x, y);               // NM_CLICK
        else if (msg == WM_RBUTTONDOWN) lv_notify_activate(st, w, -5, i, sub, x, y);         // NM_RCLICK
        else {
            lv_notify_activate(st, w, -3, i, sub, x, y);                                       // NM_DBLCLK
            if (i >= 0 && (w = win_get(h))) lv_notify_activate(st, w, -114, i, sub, x, y);    // LVN_ITEMACTIVATE
        }
        return 0;
    }
    case WM_KEYDOWN: {
        {
            Nm n = nm_new(w, -155, "PP4|24", 0); // LVN_KEYDOWN (NMLVKEYDOWN: wVKey, flags)
            gs_set(n.g, n.L, 3, (uint16_t)wp);
            nm_send(st, w, &n);
            nm_free(&n);
            if (!(w = win_get(h))) return 0;
            d = lv_data(w);
        }
        int n = lv_count(w), cur = -1;
        for (int k = 0; k < n; k++) if (lv_state(w, k) & LVIS_FOCUSED) { cur = k; break; }
        if (cur < 0) for (int k = 0; k < n; k++) if (lv_state(w, k) & LVIS_SELECTED) { cur = k; break; }
        int per = lv_per_row(w), nx = cur;
        switch (wp) {
        case VK_UP: nx = cur < 0 ? 0 : cur - per; break;
        case VK_DOWN: nx = cur < 0 ? 0 : cur + per; break;
        case VK_LEFT: nx = lv_view(w) == 0 ? cur - 1 : cur; break;
        case VK_RIGHT: nx = lv_view(w) == 0 ? cur + 1 : cur; break;
        case VK_PRIOR: nx = cur - lv_rows_visible(w) * per; break;
        case VK_NEXT: nx = cur + lv_rows_visible(w) * per; break;
        case VK_HOME: nx = 0; break;
        case VK_END: nx = n - 1; break;
        case VK_SPACE:
            if ((d->ex & 4) && cur >= 0) { uint32_t c = (lv_state(w, cur) >> 12) & 15; lv_set_state(st, w, cur, (c == 2 ? 1u : 2u) << 12, 0xF000); }
            return 0;
        case VK_RETURN: notify_simple(st, w, -4); if ((w = win_get(h)) && cur >= 0) lv_notify_activate(st, w, -114, cur, 0, 0, 0); return 0;
        default: return 0;
        }
        if (n == 0) return 0;
        if (nx < 0) nx = 0;
        if (nx >= n) nx = n - 1;
        if (nx != cur) {
            lv_select_only(st, w, nx);
            if ((w = win_get(h))) { lv_data(w)->mark = nx; lv_ensure_visible(w, nx); invalidate(w); }
        }
        return 0;
    }
    case WM_NOTIFY: { // from the header
        int code = (int32_t)mem_rd(lp + 2 * (uint64_t)PSZ, 4);
        int item = (int32_t)mem_rd(lp + gs_off("PP4|4", 3), 4);
        if (code == -302 || code == -322) { lv_notify_item(st, w, -108, -1, item, 0, 0, 0); return 0; } // LVN_COLUMNCLICK
        invalidate(w);
        return 0;
    }
    case 0x1004: return (uint64_t)lv_count(w);                                   // LVM_GETITEMCOUNT
    case 0x102F: {                                                               // LVM_SETITEMCOUNT
        if (lv_virtual(w)) { int n = (int)(int32_t)wp; d->vstate = realloc(d->vstate, sizeof(uint32_t) * (size_t)(n > 0 ? n : 1)); for (int k = d->virt; k < n; k++) d->vstate[k] = (d->ex & 4) ? 0x1000 : 0; d->virt = n < 0 ? 0 : n; lv_clamp_top(w); invalidate(w); }
        return 1;
    }
    case 0x1007: case 0x104D: {                                                  // LVM_INSERTITEM
        if (lv_virtual(w) || !lp) return (uint64_t)-1;
        lv_read_item(st, w, lp, msg == 0x104D, 1);
        int i = (int32_t)gs_get(lp, L_LVITEM, 1);
        if ((w->style & 0x30) && !d->it[i].text_cb) { // LVS_SORTASCENDING / DESCENDING
            LvItem t = d->it[i];
            qsort(d->it, (size_t)d->n, sizeof(LvItem), lv_cmp_text);
            if (w->style & 0x20) for (int a = 0, b = d->n - 1; a < b; a++, b--) { LvItem x = d->it[a]; d->it[a] = d->it[b]; d->it[b] = x; }
            for (int k = 0; k < d->n; k++) if (d->it[k].sub == t.sub) { i = k; break; }
        }
        invalidate(w);
        lv_notify_item(st, w, -102, i, 0, 0, 0, 0); // LVN_INSERTITEM
        return (uint64_t)i;
    }
    case 0x1006: case 0x104C: {                                                  // LVM_SETITEM
        if (lv_virtual(w) || !lp) return 0;
        int i = (int32_t)gs_get(lp, L_LVITEM, 1);
        if (i < 0 || i >= d->n) return 0;
        uint32_t old = d->it[i].state;
        lv_read_item(st, w, lp, msg == 0x104C, 0);
        if ((w = win_get(h)) && lv_data(w)->it[i].state != old) lv_notify_item(st, w, -101, i, 0, lv_data(w)->it[i].state, old, 8);
        if ((w = win_get(h))) invalidate(w);
        return 1;
    }
    case 0x1005: case 0x104B: {                                                  // LVM_GETITEM
        if (!lp) return 0;
        int wide = msg == 0x104B;
        uint32_t mask = (uint32_t)gs_get(lp, L_LVITEM, 0);
        int i = (int32_t)gs_get(lp, L_LVITEM, 1), sub = (int32_t)gs_get(lp, L_LVITEM, 2);
        if (i < 0 || i >= lv_count(w)) return 0;
        if (mask & 1) { char *t = lv_text(st, w, i, sub, NULL); if (!(w = win_get(h))) { free(t); return 0; } cc_put(gs_get(lp, L_LVITEM, 5), (size_t)(int32_t)gs_get(lp, L_LVITEM, 6), t, wide); free(t); d = lv_data(w); }
        if (!lv_virtual(w)) {
            if (mask & 2) gs_set(lp, L_LVITEM, 7, (uint32_t)d->it[i].image);
            if (mask & 4) gs_set(lp, L_LVITEM, 8, d->it[i].lparam);
        }
        if (mask & 8) gs_set(lp, L_LVITEM, 3, lv_state(w, i) & (uint32_t)gs_get(lp, L_LVITEM, 4));
        return 1;
    }
    case 0x102E: case 0x1074: {                                                  // LVM_SETITEMTEXT
        int i = (int)(int32_t)wp;
        if (lv_virtual(w) || i < 0 || i >= d->n || !lp) return 0;
        int sub = (int32_t)gs_get(lp, L_LVITEM, 2);
        uint64_t p = gs_get(lp, L_LVITEM, 5);
        if (is_callback_text(p)) { if (sub == 0) d->it[i].text_cb = 1; }
        else { char *t = cc_gstr(p, msg == 0x1074); lv_set_sub(&d->it[i], sub, t ? t : xstrdup("")); }
        invalidate(w);
        return 1;
    }
    case 0x102D: case 0x1073: {                                                  // LVM_GETITEMTEXT
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= lv_count(w) || !lp) return 0;
        char *t = lv_text(st, w, i, (int32_t)gs_get(lp, L_LVITEM, 2), NULL);
        size_t r = cc_put(gs_get(lp, L_LVITEM, 5), (size_t)(int32_t)gs_get(lp, L_LVITEM, 6), t, msg == 0x1073);
        free(t);
        return r;
    }
    case 0x1008: {                                                               // LVM_DELETEITEM
        int i = (int)(int32_t)wp;
        if (lv_virtual(w) || i < 0 || i >= d->n) return 0;
        lv_notify_item(st, w, -103, i, 0, 0, 0, 0);
        if (!(w = win_get(h))) return 0;
        d = lv_data(w);
        if (i >= d->n) return 0;
        for (int k = 0; k < d->it[i].nsub; k++) free(d->it[i].sub[k]);
        free(d->it[i].sub);
        memmove(&d->it[i], &d->it[i + 1], sizeof(LvItem) * (size_t)(d->n - i - 1));
        d->n--;
        lv_clamp_top(w);
        invalidate(w);
        return 1;
    }
    case 0x1009: {                                                               // LVM_DELETEALLITEMS
        if (lv_virtual(w)) { d->virt = 0; invalidate(w); return 1; }
        int skip = (int)lv_notify_item(st, w, -104, -1, 0, 0, 0, 0) != 0; // LVN_DELETEALLITEMS
        for (int i = 0; !skip && (w = win_get(h)) && i < lv_data(w)->n; i++) lv_notify_item(st, w, -103, i, 0, 0, 0, 0);
        if (!(w = win_get(h))) return 0;
        d = lv_data(w);
        for (int i = 0; i < d->n; i++) { for (int k = 0; k < d->it[i].nsub; k++) free(d->it[i].sub[k]); free(d->it[i].sub); }
        d->n = 0; d->top = 0; d->mark = -1;
        invalidate(w);
        return 1;
    }
    case 0x102B: {                                                               // LVM_SETITEMSTATE
        if (!lp) return 0;
        int i = (int)(int32_t)wp;
        uint32_t s = (uint32_t)gs_get(lp, L_LVITEM, 3), m = (uint32_t)gs_get(lp, L_LVITEM, 4);
        if (i == -1) { for (int k = 0; (w = win_get(h)) && k < lv_count(w); k++) lv_set_state(st, w, k, s, m); return 1; }
        if (i < 0 || i >= lv_count(w)) return 0;
        lv_set_state(st, w, i, s, m);
        return 1;
    }
    case 0x102C: return lv_state(w, (int)(int32_t)wp) & (uint32_t)lp;            // LVM_GETITEMSTATE
    case 0x1032: { int c = 0; for (int k = 0; k < lv_count(w); k++) if (lv_state(w, k) & LVIS_SELECTED) c++; return (uint64_t)c; } // LVM_GETSELECTEDCOUNT
    case 0x100C: {                                                               // LVM_GETNEXTITEM
        int start = (int)(int32_t)wp, n = lv_count(w);
        uint32_t f = (uint32_t)lp & 0xFFFF;
        uint32_t want = ((f & 1) ? LVIS_FOCUSED : 0) | ((f & 2) ? LVIS_SELECTED : 0);
        if (f & 0x100) { // LVNI_ABOVE
            for (int k = start - 1; k >= 0; k--) if ((lv_state(w, k) & want) == want) return (uint64_t)k;
            return (uint64_t)-1;
        }
        for (int k = start + 1; k < n; k++) if ((lv_state(w, k) & want) == want) return (uint64_t)k;
        return (uint64_t)-1;
    }
    case 0x1042: return (uint64_t)(int64_t)d->mark;                              // LVM_GETSELECTIONMARK
    case 0x1043: { int o = d->mark; d->mark = (int)(int32_t)lp; return (uint64_t)(int64_t)o; }
    case 0x1013: { int i = (int)(int32_t)wp; if (i >= 0 && i < lv_count(w)) { lv_ensure_visible(w, i); invalidate(w); } return 1; } // LVM_ENSUREVISIBLE
    case 0x1027: return (uint64_t)(d->top * lv_per_row(w));                      // LVM_GETTOPINDEX
    case 0x1028: return (uint64_t)(lv_rows_visible(w) * lv_per_row(w));          // LVM_GETCOUNTPERPAGE
    case 0x1014: { int rh = lv_view(w) == 0 ? lv_cell_h(w) : lv_row_h(w); d->top += (int)(int32_t)lp / (rh ? rh : 1); lv_clamp_top(w); invalidate(w); return 1; } // LVM_SCROLL
    case 0x1015: case 0x102A: invalidate(w); return 1;                           // LVM_REDRAWITEMS / LVM_UPDATE
    case 0x1003: { int t = (int)wp; if (t < 0 || t > 2) return 0; uint64_t o = d->iml[t]; d->iml[t] = lp; invalidate(w); return o; } // LVM_SETIMAGELIST
    case 0x1002: { int t = (int)wp; return t >= 0 && t <= 2 ? d->iml[t] : 0; }   // LVM_GETIMAGELIST
    case 0x1036: { // LVM_SETEXTENDEDLISTVIEWSTYLE; turning on checkboxes gives every item the "unchecked" state image
        uint32_t m = (uint32_t)wp ? (uint32_t)wp : 0xFFFFFFFFu, o = d->ex;
        d->ex = (d->ex & ~m) | ((uint32_t)lp & m);
        if ((d->ex & 4) && !(o & 4)) for (int k = 0; k < lv_count(w); k++) if (!(lv_state(w, k) & 0xF000)) lv_raw_set_state(w, k, lv_state(w, k) | 0x1000);
        invalidate(w);
        return o;
    }
    case 0x1037: return d->ex;
    case 0x101F: return d->header;                                               // LVM_GETHEADER
    case 0x1000: return d->bk;                                                   // LVM_GETBKCOLOR
    case 0x1001: d->bk = (uint32_t)lp; invalidate(w); return 1;                  // LVM_SETBKCOLOR
    case 0x1023: return d->fg;                                                   // LVM_GETTEXTCOLOR
    case 0x1024: d->fg = (uint32_t)lp; invalidate(w); return 1;                  // LVM_SETTEXTCOLOR
    case 0x1025: return d->bk;                                                   // LVM_GETTEXTBKCOLOR
    case 0x1026: return 1;                                                       // LVM_SETTEXTBKCOLOR
    case 0x100E: case 0x1038: {                                                  // LVM_GETITEMRECT / LVM_GETSUBITEMRECT
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= lv_count(w) || !lp) return 0;
        int32_t r[4], code = (int32_t)mem_rd(lp, 4), sub = msg == 0x1038 ? (int32_t)mem_rd(lp + 4, 4) : 0;
        lv_item_rect(w, i, r);
        HdData *c = lv_cols(w);
        if (msg == 0x1038 && c && sub > 0 && sub < c->n) { r[0] = hd_left(c, sub); r[2] = r[0] + c->it[sub].cx; }
        else if (code == 2 && c && c->n && lv_view(w) == 1) r[2] = c->it[0].cx;     // LVIR_LABEL
        memcpy(G2H(lp), r, 16);
        return 1;
    }
    case 0x1012: case 0x1039: {                                                  // LVM_HITTEST / LVM_SUBITEMHITTEST
        if (!lp) return (uint64_t)-1;
        int x = (int32_t)mem_rd(lp, 4), y = (int32_t)mem_rd(lp + 4, 4), sub = 0, chk = 0;
        int i = lv_hit(w, x, y, &sub, &chk);
        mem_wr(lp + 8, 4, i < 0 ? 1 : chk ? 8 : 4);
        mem_wr(lp + 12, 4, (uint32_t)i);
        if (msg == 0x1039) mem_wr(lp + 16, 4, (uint32_t)sub);
        return (uint64_t)(int64_t)i;
    }
    case 0x100D: case 0x1053: {                                                  // LVM_FINDITEM (LVFINDINFO: flags psz lParam)
        if (!lp) return (uint64_t)-1;
        uint32_t f = (uint32_t)gs_get(lp, "4PP", 0);
        char *s = (f & 2) ? cc_gstr(gs_get(lp, "4PP", 1), msg == 0x1053) : NULL;
        uint64_t param = gs_get(lp, "4PP", 2);
        int start = (int)(int32_t)wp, n = lv_count(w), found = -1;
        for (int k = start + 1; k < n && found < 0; k++) {
            if (f & 1) { if (!lv_virtual(w) && d->it[k].lparam == param) found = k; continue; }
            if (s) {
                char *t = lv_text(st, w, k, 0, NULL);
                if (!(w = win_get(h))) { free(t); free(s); return (uint64_t)-1; }
                d = lv_data(w);
                if ((f & 8) ? strncasecmp(t, s, strlen(s)) == 0 : strcasecmp(t, s) == 0) found = k;
                free(t);
            }
        }
        free(s);
        return (uint64_t)(int64_t)found;
    }
    case 0x1030: case 0x1051: if (!lv_virtual(w) && lp) { lv_sort(st, w, lp, wp, msg == 0x1051); if ((w = win_get(h))) invalidate(w); } return 1; // LVM_SORTITEMS(EX)
    case 0x1011: case 0x1057: { char *s = cc_gstr(lp, msg == 0x1057); int r = cc_textw(w, s); free(s); return (uint64_t)r; } // LVM_GETSTRINGWIDTH
    case 0x108E: w->style = (w->style & ~3u) | ((uint32_t)wp & 3); lv_layout_header(w); invalidate(w); return 1; // LVM_SETVIEW
    case 0x108F: return w->style & 3;                                            // LVM_GETVIEW
    case 0x1029: { if (lp) { int32_t r[4] = { 0, 0, w->w, w->h }; memcpy(G2H(lp), r, 16); } return 1; } // LVM_GETVIEWRECT
    case 0x1041: return 0;                                                       // LVM_GETORIGIN
    case 0x103A: case 0x103B: { HdData *c = lv_cols(w); if (msg == 0x103B && c) for (int k = 0; k < c->n && k < (int)wp; k++) mem_wr(lp + 4 * (uint64_t)k, 4, (uint32_t)k); return 1; }
    case 0x101B: case 0x1061: case 0x101A: case 0x1060: case 0x1019: case 0x105F: case 0x101C: case 0x101D: case 0x101E:
        return lv_column_msg(st, w, msg, wp, lp);
    case 0x109D: case 0x10A1: case 0x1017: case 0x100F: case 0x1035: case 0x1016: case 0x1076: case 0x103C: case 0x103E: case 0x108B: return 0;
    case 0x2005: case 0x2006: return 1;
    case 0x2007: return 6; // CCM_SETVERSION
    case 0x2008: return 6;
    default:
        if (msg >= 0x1000 && msg < 0x1100) return 0;
        return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- tree view (SysTreeView32) ---------------------------------------------------------------------

#define L_TVITEM "4P44P4444P"   // mask hItem state stateMask pszText cchTextMax iImage iSelectedImage cChildren lParam
#define TV_BASE 0x00A00000u
#define TVI_ROOT_32 0xFFFF0000u
#define TVI_FIRST_32 0xFFFF0001u
#define TVI_LAST_32 0xFFFF0002u
#define TVI_SORT_32 0xFFFF0003u
#define TVIS_SELECTED 2u
#define TVIS_EXPANDED 0x20u
#define TVIS_BOLD 0x10u

typedef struct TvItem { int used; char *text; int text_cb, image, simage, children; uint32_t state; uint64_t lparam; int parent, first, next, prev; } TvItem;
typedef struct TvData { TvItem *it; int cap; int first; int sel; int top; uint64_t iml, iml_state; int indent, item_h; uint32_t bk, fg; } TvData;

static void tv_free(void *p) { TvData *d = p; for (int i = 0; i < d->cap; i++) if (d->it[i].used) free(d->it[i].text); free(d->it); free(d); }
static TvData *tv_data(Win *w) {
    if (!w->cdata) {
        TvData *d = calloc(1, sizeof(TvData));
        d->first = -1; d->sel = -1; d->indent = 19; d->bk = 0xFFFFFFFFu; d->fg = 0xFFFFFFFFu;
        w->cdata = d; w->cdata_free = tv_free;
    }
    return w->cdata;
}
static int tv_index(TvData *d, uint64_t h) {
    uint32_t v = (uint32_t)h;
    if (v < TV_BASE) return -1;
    int i = (int)(v - TV_BASE);
    return i < d->cap && d->it[i].used ? i : -1;
}
static uint64_t tv_handle(int i) { return i < 0 ? 0 : TV_BASE + (uint64_t)i; }
static int tv_alloc(TvData *d) {
    for (int i = 0; i < d->cap; i++) if (!d->it[i].used) { memset(&d->it[i], 0, sizeof(TvItem)); d->it[i].used = 1; return i; }
    int i = d->cap;
    d->cap = d->cap ? d->cap * 2 : 64;
    d->it = realloc(d->it, sizeof(TvItem) * (size_t)d->cap);
    memset(&d->it[i], 0, sizeof(TvItem) * (size_t)(d->cap - i));
    d->it[i].used = 1;
    return i;
}
static int tv_first_child(TvData *d, int p) { return p < 0 ? d->first : d->it[p].first; }
static int tv_has_children(TvData *d, int i) { return d->it[i].first >= 0 || d->it[i].children == 1 || d->it[i].children == -1; }
static int tv_item_h(Win *w) {
    TvData *d = tv_data(w);
    if (d->item_h > 0) return d->item_h;
    int h = cc_line(w) + 4, cx, cy;
    if (iml_size(d->iml, &cx, &cy) && cy + 2 > h) h = cy + 2;
    return h < 18 ? 18 : h;
}
// Visible items in display order with their depths.
static int tv_visible(TvData *d, int *items, int *depth, int max) {
    int n = 0;
    int stack[64], sd = 0;
    int cur = d->first, dep = 0;
    while (cur >= 0 && n < max) {
        items[n] = cur; depth[n] = dep; n++;
        if ((d->it[cur].state & TVIS_EXPANDED) && d->it[cur].first >= 0 && sd < 64) { stack[sd++] = cur; cur = d->it[cur].first; dep++; continue; }
        while (cur >= 0 && d->it[cur].next < 0 && sd > 0) { cur = stack[--sd]; dep--; }
        cur = cur >= 0 ? d->it[cur].next : -1;
    }
    return n;
}
static char *tv_text(X86State *st, Win *w, int i) {
    TvData *d = tv_data(w);
    if (!d->it[i].text_cb) return xstrdup(d->it[i].text);
    int u = parent_unicode(w);
    size_t hdr = gs_size(L_NMHDR);
    Nm n = nm_new(w, u ? -452 : -403, "PP4", gs_size(L_TVITEM) + 8); // TVN_GETDISPINFO: NMTVDISPINFO = hdr + TVITEM
    uint64_t item = n.g + ((hdr + (size_t)PSZ - 1) & ~(size_t)(PSZ - 1));
    uint16_t *buf = g_calloc(1, 1040);
    gs_set(item, L_TVITEM, 0, 1); gs_set(item, L_TVITEM, 1, tv_handle(i));
    gs_set(item, L_TVITEM, 4, H2G(buf)); gs_set(item, L_TVITEM, 5, 260); gs_set(item, L_TVITEM, 9, d->it[i].lparam);
    uint64_t h = w->hwnd;
    nm_send(st, w, &n);
    char *t = win_get(h) ? cc_gstr(gs_get(item, L_TVITEM, 4), u) : NULL;
    g_free(buf);
    nm_free(&n);
    return t ? t : xstrdup("");
}
static void tv_paint(X86State *st, Win *w) {
    TvData *d = tv_data(w);
    uint64_t h = w->hwnd;
    CPaint p = cc_begin(w);
    uint32_t bk = d->bk == 0xFFFFFFFFu ? 0xFFFFFF : d->bk, fg = d->fg == 0xFFFFFFFFu ? 0x000000 : d->fg;
    gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, bk);
    int ih = tv_item_h(w), focused = gui()->focus == w->hwnd;
    int max = 4096, *items = malloc(sizeof(int) * (size_t)max), *depth = malloc(sizeof(int) * (size_t)max);
    int n = tv_visible(d, items, depth, max);
    int icx = 0, icy = 0, has_img = iml_size(d->iml, &icx, &icy);
    int rootline = (w->style & 4) != 0 && (w->style & 3);
    for (int r = d->top, y = 0; r < n && y < w->h; r++, y += ih) {
        int i = items[r];
        int x = 2 + (depth[r] + (rootline ? 1 : 0)) * d->indent;
        if ((w->style & 1) && tv_has_children(d, i) && (rootline || depth[r] > 0)) { // TVS_HASBUTTONS
            int bx = x - d->indent + (d->indent - 9) / 2, by = y + (ih - 9) / 2;
            gdi_fill_rect_px(p.hdc, bx, by, bx + 9, by + 9, 0xFFFFFF);
            gdi_frame_rect_px(p.hdc, bx, by, bx + 9, by + 9, 0x919191);
            gdi_line_px(p.hdc, bx + 2, by + 4, bx + 7, by + 4, 0x333333, 1);
            if (!(d->it[i].state & TVIS_EXPANDED)) gdi_line_px(p.hdc, bx + 4, by + 2, bx + 4, by + 7, 0x333333, 1);
        }
        if (w->style & 0x100) { uint32_t c = (d->it[i].state >> 12) & 15; cc_check(p.hdc, x, y + (ih - 13) / 2, c == 2, w->enabled); x += 17; } // TVS_CHECKBOXES
        if (has_img) {
            int im = (d->it[i].state & TVIS_SELECTED) || i == d->sel ? d->it[i].simage : d->it[i].image;
            if (im >= 0) iml_draw(d->iml, im, p.hdc, x, y + (ih - icy) / 2);
            x += icx + 3;
        }
        char *t = tv_text(st, w, i);
        if (!(w = win_get(h))) { free(t); free(items); free(depth); cc_end(&p); return; }
        d = tv_data(w);
        int tw = cc_textw(w, t);
        int sel = i == d->sel && (focused || (w->style & 0x20));
        if (sel) gdi_fill_rect_px(p.hdc, x - 2, y, (w->style & 0x1000) ? w->w : x + tw + 4, y + ih, focused ? hl_color() : 0xD9D9D9);
        cc_color(p.hdc, sel && focused ? 0xFFFFFF : (w->enabled ? fg : 0x6D6D6D));
        cc_text(p.hdc, t, x, y, w->w - 2, y + ih, DT_LEFT_VC);
        free(t);
    }
    free(items); free(depth);
    if ((w->style & WS_BORDER) || (w->exstyle & WS_EX_CLIENTEDGE)) gdi_frame_rect_px(p.hdc, 0, 0, w->w, w->h, focused ? 0xD77800 : 0x828790);
    cc_end(&p);
}
// NMTREEVIEW: hdr, action, itemOld, itemNew, ptDrag
static uint64_t tv_notify(X86State *st, Win *w, int code_a, int code_w, uint32_t action, int olditem, int newitem) {
    TvData *d = tv_data(w);
    int u = parent_unicode(w);
    size_t hdr = gs_size(L_NMHDR), tvs = gs_size(L_TVITEM);
    size_t a = (hdr + 3) & ~(size_t)3;
    size_t o_old = (a + 4 + (size_t)PSZ - 1) & ~(size_t)(PSZ - 1), o_new = o_old + tvs;
    Nm n = nm_new(w, u ? code_w : code_a, "PP4", o_new + tvs + 16 - gs_size("PP4"));
    mem_wr(n.g + a, 4, action);
    int which[2] = { olditem, newitem };
    size_t offs[2] = { o_old, o_new };
    for (int k = 0; k < 2; k++) {
        int i = which[k];
        if (i < 0) continue;
        uint64_t it = n.g + offs[k];
        gs_set(it, L_TVITEM, 0, 0x1F); gs_set(it, L_TVITEM, 1, tv_handle(i));
        gs_set(it, L_TVITEM, 2, d->it[i].state); gs_set(it, L_TVITEM, 9, d->it[i].lparam);
        gs_set(it, L_TVITEM, 6, (uint32_t)d->it[i].image); gs_set(it, L_TVITEM, 7, (uint32_t)d->it[i].simage);
    }
    uint64_t r = nm_send(st, w, &n);
    nm_free(&n);
    return r;
}
static void tv_select(X86State *st, Win *w, int i, uint32_t action) {
    TvData *d = tv_data(w);
    if (i == d->sel) return;
    uint64_t h = w->hwnd;
    int old = d->sel;
    if (tv_notify(st, w, -401, -450, action, old, i)) return; // TVN_SELCHANGING vetoed
    if (!(w = win_get(h))) return;
    d = tv_data(w);
    if (old >= 0 && old < d->cap && d->it[old].used) d->it[old].state &= ~TVIS_SELECTED;
    d->sel = i;
    if (i >= 0) d->it[i].state |= TVIS_SELECTED;
    invalidate(w);
    tv_notify(st, w, -402, -451, action, old, i); // TVN_SELCHANGED
}
static void tv_expand(X86State *st, Win *w, int i, int how) { // 1 collapse, 2 expand, 3 toggle
    TvData *d = tv_data(w);
    if (i < 0) return;
    int ex = (d->it[i].state & TVIS_EXPANDED) != 0;
    int want = how == 3 ? !ex : how == 2;
    if (want == ex) return;
    uint64_t h = w->hwnd;
    if (tv_notify(st, w, -405, -454, want ? 2 : 1, -1, i)) return; // TVN_ITEMEXPANDING vetoed
    if (!(w = win_get(h))) return;
    d = tv_data(w);
    if (!d->it[i].used) return;
    if (want) d->it[i].state |= TVIS_EXPANDED; else d->it[i].state &= ~TVIS_EXPANDED;
    if (!want && d->sel >= 0) { for (int a = d->it[d->sel].parent; a >= 0; a = d->it[a].parent) if (a == i) { tv_select(st, w, i, 0); if (!(w = win_get(h))) return; d = tv_data(w); break; } }
    invalidate(w);
    tv_notify(st, w, -406, -455, want ? 2 : 1, -1, i); // TVN_ITEMEXPANDED
}
static void tv_unlink(TvData *d, int i) {
    TvItem *it = &d->it[i];
    if (it->prev >= 0) d->it[it->prev].next = it->next;
    else if (it->parent >= 0) d->it[it->parent].first = it->next;
    else d->first = it->next;
    if (it->next >= 0) d->it[it->next].prev = it->prev;
}
static void tv_delete(X86State *st, Win *w, int i) {
    TvData *d = tv_data(w);
    uint64_t h = w->hwnd;
    while ((w = win_get(h)) && (d = tv_data(w)) && d->it[i].first >= 0) tv_delete(st, w, d->it[i].first);
    if (!(w = win_get(h))) return;
    tv_notify(st, w, -409, -458, 0, i, -1); // TVN_DELETEITEM
    if (!(w = win_get(h))) return;
    d = tv_data(w);
    if (!d->it[i].used) return;
    tv_unlink(d, i);
    if (d->sel == i) d->sel = -1;
    free(d->it[i].text);
    d->it[i].used = 0;
}
static int tv_insert(X86State *st, Win *w, uint64_t g, int wide) {
    (void)st;
    TvData *d = tv_data(w);
    uint64_t hp = gs_get(g, "PP", 0), ha = gs_get(g, "PP", 1);
    uint64_t item = g + 2 * (uint64_t)PSZ;
    int parent = (uint32_t)hp == TVI_ROOT_32 || !hp ? -1 : tv_index(d, hp);
    int i = tv_alloc(d);
    TvItem *it = &d->it[i];
    it->parent = parent; it->first = it->next = it->prev = -1; it->image = it->simage = 0;
    uint32_t mask = (uint32_t)gs_get(item, L_TVITEM, 0);
    if (mask & 1) { uint64_t p = gs_get(item, L_TVITEM, 4); if (is_callback_text(p)) it->text_cb = 1; else it->text = cc_gstr(p, wide); }
    if (!it->text) it->text = xstrdup("");
    if (mask & 2) it->image = (int32_t)gs_get(item, L_TVITEM, 6);
    if (mask & 0x20) it->simage = (int32_t)gs_get(item, L_TVITEM, 7); else it->simage = it->image;
    if (mask & 4) it->lparam = gs_get(item, L_TVITEM, 9);
    if (mask & 0x40) it->children = (int32_t)gs_get(item, L_TVITEM, 8);
    if (mask & 8) { uint32_t sm = (uint32_t)gs_get(item, L_TVITEM, 3); it->state = (uint32_t)gs_get(item, L_TVITEM, 2) & sm & ~TVIS_SELECTED; }
    if ((w->style & 0x100) && !(it->state & 0xF000)) it->state |= 1u << 12; // checkbox: unchecked
    // link into the sibling list
    int first = tv_first_child(d, parent);
    uint32_t after = (uint32_t)ha;
    int prev = -1;
    if (after == TVI_FIRST_32) prev = -1;
    else if (after == TVI_SORT_32) { for (int c = first; c >= 0; c = d->it[c].next) { if (strcasecmp(d->it[c].text, it->text) > 0) break; prev = c; } }
    else if (after == TVI_LAST_32 || !ha || tv_index(d, ha) < 0) { for (int c = first; c >= 0; c = d->it[c].next) prev = c; }
    else prev = tv_index(d, ha);
    it = &d->it[i];
    it->prev = prev;
    it->next = prev >= 0 ? d->it[prev].next : first;
    if (prev >= 0) d->it[prev].next = i;
    else if (parent >= 0) d->it[parent].first = i;
    else d->first = i;
    if (it->next >= 0) d->it[it->next].prev = i;
    invalidate(w);
    return i;
}
static uint64_t treeview_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    TvData *d = tv_data(w);
    GuiState *g = gui();
    uint64_t h = w->hwnd;
    switch (msg) {
    case WM_PAINT: tv_paint(st, w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: case WM_KILLFOCUS: invalidate(w); notify_simple(st, w, msg == WM_SETFOCUS ? -7 : -8); return 0;
    case WM_GETDLGCODE: return 1 | 0x80;
    case WM_MOUSEWHEEL: d->top -= (int16_t)HIWORD(wp) / 40; if (d->top < 0) d->top = 0; invalidate(w); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: {
        if (g->focus != h) win_set_focus(st, h);
        if (!(w = win_get(h))) return 0;
        d = tv_data(w);
        int ih = tv_item_h(w), x = (int16_t)LOWORD(lp), y = (int16_t)HIWORD(lp);
        int max = 4096, *items = malloc(sizeof(int) * (size_t)max), *depth = malloc(sizeof(int) * (size_t)max);
        int n = tv_visible(d, items, depth, max), r = d->top + y / ih;
        int i = r >= 0 && r < n ? items[r] : -1, dep = r >= 0 && r < n ? depth[r] : 0;
        free(items); free(depth);
        if (i >= 0) {
            int rootline = (w->style & 4) != 0 && (w->style & 3);
            int x0 = 2 + (dep + (rootline ? 1 : 0)) * d->indent;
            if (msg != WM_RBUTTONDOWN && x >= x0 - d->indent && x < x0 && tv_has_children(d, i)) { tv_expand(st, w, i, 3); return 0; }
            if (msg == WM_LBUTTONDOWN && (w->style & 0x100) && x >= x0 && x < x0 + 15) {
                uint32_t c = (d->it[i].state >> 12) & 15;
                d->it[i].state = (d->it[i].state & ~0xF000u) | ((c == 2 ? 1u : 2u) << 12);
                invalidate(w);
                notify_simple(st, w, -2);
                return 0;
            }
            tv_select(st, w, i, 1); // TVC_BYMOUSE
            if (!(w = win_get(h))) return 0;
        }
        if (msg == WM_LBUTTONDBLCLK) { if (!notify_simple(st, w, -3) && i >= 0 && (w = win_get(h))) tv_expand(st, w, i, 3); }
        else notify_simple(st, w, msg == WM_LBUTTONDOWN ? -2 : -5);
        return 0;
    }
    case WM_KEYDOWN: {
        int max = 4096, *items = malloc(sizeof(int) * (size_t)max), *depth = malloc(sizeof(int) * (size_t)max);
        int n = tv_visible(d, items, depth, max), r = -1;
        for (int k = 0; k < n; k++) if (items[k] == d->sel) r = k;
        int target = -1;
        switch (wp) {
        case VK_UP: target = r > 0 ? items[r - 1] : (n ? items[0] : -1); break;
        case VK_DOWN: target = r + 1 < n ? items[r + 1] : (r < 0 && n ? items[0] : -1); break;
        case VK_HOME: target = n ? items[0] : -1; break;
        case VK_END: target = n ? items[n - 1] : -1; break;
        case VK_LEFT: if (d->sel >= 0) { if (d->it[d->sel].state & TVIS_EXPANDED) tv_expand(st, w, d->sel, 1); else target = d->it[d->sel].parent; } break;
        case VK_RIGHT: if (d->sel >= 0) { if (!(d->it[d->sel].state & TVIS_EXPANDED) && tv_has_children(d, d->sel)) tv_expand(st, w, d->sel, 2); else target = d->it[d->sel].first; } break;
        case VK_SPACE: if (d->sel >= 0 && (w->style & 0x100)) { uint32_t c = (d->it[d->sel].state >> 12) & 15; d->it[d->sel].state = (d->it[d->sel].state & ~0xF000u) | ((c == 2 ? 1u : 2u) << 12); invalidate(w); } break;
        case VK_RETURN: notify_simple(st, w, -4); break;
        default: break;
        }
        free(items); free(depth);
        if (target >= 0 && (w = win_get(h))) {
            tv_select(st, w, target, 2); // TVC_BYKEYBOARD
            if ((w = win_get(h))) { // keep the selection visible
                d = tv_data(w);
                int *it2 = malloc(sizeof(int) * 4096), *dp2 = malloc(sizeof(int) * 4096);
                int n2 = tv_visible(d, it2, dp2, 4096), vis = w->h / tv_item_h(w);
                for (int k = 0; k < n2; k++) if (it2[k] == d->sel) { if (k < d->top) d->top = k; if (k >= d->top + vis) d->top = k - vis + 1; }
                free(it2); free(dp2);
                invalidate(w);
            }
        }
        return 0;
    }
    case 0x1100: case 0x1132: { int i = tv_insert(st, w, lp, msg == 0x1132); return tv_handle(i); } // TVM_INSERTITEM
    case 0x1101: {                                                                         // TVM_DELETEITEM
        if (!lp || (uint32_t)lp == TVI_ROOT_32) { while ((w = win_get(h)) && tv_data(w)->first >= 0) tv_delete(st, w, tv_data(w)->first); if ((w = win_get(h))) invalidate(w); return 1; }
        int i = tv_index(d, lp);
        if (i < 0) return 0;
        tv_delete(st, w, i);
        if ((w = win_get(h))) invalidate(w);
        return 1;
    }
    case 0x1102: { int i = tv_index(d, lp); if (i < 0) return 0; tv_expand(st, w, i, (int)(wp & 3)); return 1; } // TVM_EXPAND
    case 0x1105: { int c = 0; for (int i = 0; i < d->cap; i++) if (d->it[i].used) c++; return (uint64_t)c; } // TVM_GETCOUNT
    case 0x1106: return (uint64_t)d->indent;
    case 0x1107: d->indent = (int)wp < 8 ? 8 : (int)wp; invalidate(w); return 0;
    case 0x1108: return wp == 2 ? d->iml_state : d->iml;                                  // TVM_GETIMAGELIST
    case 0x1109: { uint64_t o; if (wp == 2) { o = d->iml_state; d->iml_state = lp; } else { o = d->iml; d->iml = lp; } invalidate(w); return o; }
    case 0x110A: {                                                                         // TVM_GETNEXTITEM
        int i = tv_index(d, lp);
        switch (wp) {
        case 0: return tv_handle(d->first);                                                // TVGN_ROOT
        case 1: return i >= 0 ? tv_handle(d->it[i].next) : 0;                              // TVGN_NEXT
        case 2: return i >= 0 ? tv_handle(d->it[i].prev) : 0;                              // TVGN_PREVIOUS
        case 3: return i >= 0 ? tv_handle(d->it[i].parent) : 0;                            // TVGN_PARENT
        case 4: return i >= 0 ? tv_handle(d->it[i].first) : (lp == 0 || (uint32_t)lp == TVI_ROOT_32 ? tv_handle(d->first) : 0); // TVGN_CHILD
        case 9: return tv_handle(d->sel);                                                  // TVGN_CARET
        case 8: return 0;                                                                  // TVGN_DROPHILITE
        case 5: case 6: case 7: case 0xA: {                                                // visible navigation
            int *items = malloc(sizeof(int) * 4096), *depth = malloc(sizeof(int) * 4096);
            int n = tv_visible(d, items, depth, 4096), r = -1, res = -1;
            for (int k = 0; k < n; k++) if (items[k] == i) r = k;
            if (wp == 5) res = d->top < n ? items[d->top] : -1;
            else if (wp == 6) res = r >= 0 && r + 1 < n ? items[r + 1] : -1;
            else if (wp == 7) res = r > 0 ? items[r - 1] : -1;
            else res = n ? items[n - 1] : -1;
            free(items); free(depth);
            return tv_handle(res);
        }
        default: return 0;
        }
    }
    case 0x110B: {                                                                         // TVM_SELECTITEM
        int i = tv_index(d, lp);
        if (wp == 9) { tv_select(st, w, i, 0); return 1; }
        if (wp == 5 && i >= 0) { int *items = malloc(sizeof(int) * 4096), *depth = malloc(sizeof(int) * 4096); int n = tv_visible(d, items, depth, 4096); for (int k = 0; k < n; k++) if (items[k] == i) d->top = k; free(items); free(depth); invalidate(w); }
        return 1;
    }
    case 0x110C: case 0x113E: {                                                            // TVM_GETITEM
        if (!lp) return 0;
        int i = tv_index(d, gs_get(lp, L_TVITEM, 1));
        if (i < 0) return 0;
        uint32_t mask = (uint32_t)gs_get(lp, L_TVITEM, 0);
        if (mask & 1) { char *t = tv_text(st, w, i); if (!(w = win_get(h))) { free(t); return 0; } cc_put(gs_get(lp, L_TVITEM, 4), (size_t)(int32_t)gs_get(lp, L_TVITEM, 5), t, msg == 0x113E); free(t); d = tv_data(w); }
        if (mask & 2) gs_set(lp, L_TVITEM, 6, (uint32_t)d->it[i].image);
        if (mask & 0x20) gs_set(lp, L_TVITEM, 7, (uint32_t)d->it[i].simage);
        if (mask & 4) gs_set(lp, L_TVITEM, 9, d->it[i].lparam);
        if (mask & 8) gs_set(lp, L_TVITEM, 2, d->it[i].state & ((uint32_t)gs_get(lp, L_TVITEM, 3) | 0xF000u));
        if (mask & 0x40) gs_set(lp, L_TVITEM, 8, (uint32_t)(d->it[i].first >= 0 ? 1 : d->it[i].children));
        return 1;
    }
    case 0x110D: case 0x113F: {                                                            // TVM_SETITEM
        if (!lp) return 0;
        int i = tv_index(d, gs_get(lp, L_TVITEM, 1));
        if (i < 0) return 0;
        uint32_t mask = (uint32_t)gs_get(lp, L_TVITEM, 0);
        TvItem *it = &d->it[i];
        if (mask & 1) { uint64_t p = gs_get(lp, L_TVITEM, 4); if (is_callback_text(p)) it->text_cb = 1; else { char *t = cc_gstr(p, msg == 0x113F); free(it->text); it->text = t ? t : xstrdup(""); it->text_cb = 0; } }
        if (mask & 2) it->image = (int32_t)gs_get(lp, L_TVITEM, 6);
        if (mask & 0x20) it->simage = (int32_t)gs_get(lp, L_TVITEM, 7);
        if (mask & 4) it->lparam = gs_get(lp, L_TVITEM, 9);
        if (mask & 0x40) it->children = (int32_t)gs_get(lp, L_TVITEM, 8);
        if (mask & 8) { uint32_t sm = (uint32_t)gs_get(lp, L_TVITEM, 3) & ~TVIS_SELECTED; it->state = (it->state & ~sm) | ((uint32_t)gs_get(lp, L_TVITEM, 2) & sm); }
        invalidate(w);
        return 1;
    }
    case 0x1127: { int i = tv_index(d, wp); return i >= 0 ? (d->it[i].state & (uint32_t)lp) : 0; } // TVM_GETITEMSTATE
    case 0x1104: {                                                                         // TVM_GETITEMRECT (in: HTREEITEM at *prc)
        if (!lp) return 0;
        int i = tv_index(d, rd_ptr(lp));
        int *items = malloc(sizeof(int) * 4096), *depth = malloc(sizeof(int) * 4096);
        int n = tv_visible(d, items, depth, 4096), r = -1, dep = 0;
        for (int k = 0; k < n; k++) if (items[k] == i) { r = k; dep = depth[k]; }
        free(items); free(depth);
        if (r < 0) return 0;
        int ih = tv_item_h(w), y = (r - d->top) * ih;
        int x0 = wp ? 2 + (dep + 1) * d->indent : 0;
        int32_t rc[4] = { x0, y, wp ? x0 + cc_textw(w, d->it[i].text) + 4 : w->w, y + ih };
        memcpy(G2H(lp), rc, 16);
        return 1;
    }
    case 0x1111: {                                                                         // TVM_HITTEST (pt, flags, hItem)
        if (!lp) return 0;
        int y = (int32_t)mem_rd(lp + 4, 4), ih = tv_item_h(w);
        int *items = malloc(sizeof(int) * 4096), *depth = malloc(sizeof(int) * 4096);
        int n = tv_visible(d, items, depth, 4096), r = d->top + y / ih;
        int i = y >= 0 && r < n ? items[r] : -1;
        free(items); free(depth);
        mem_wr(lp + 8, 4, i >= 0 ? 4 : 1);
        wr_ptr(lp + gs_off("444P", 3), tv_handle(i));
        return tv_handle(i);
    }
    case 0x1113: { // TVM_SORTCHILDREN
        int p = !lp || (uint32_t)lp == TVI_ROOT_32 ? -1 : tv_index(d, lp);
        for (int sorted = 0; !sorted;) {
            sorted = 1;
            for (int c = tv_first_child(d, p); c >= 0 && d->it[c].next >= 0; c = d->it[c].next) {
                int nx = d->it[c].next;
                if (strcasecmp(d->it[c].text, d->it[nx].text) > 0) { // swap payloads
                    TvItem a = d->it[c], b = d->it[nx];
                    TvItem ta = b, tb = a;
                    ta.parent = a.parent; ta.next = a.next; ta.prev = a.prev;
                    tb.parent = b.parent; tb.next = b.next; tb.prev = b.prev;
                    d->it[c] = ta; d->it[nx] = tb;
                    for (int k = d->it[c].first; k >= 0; k = d->it[k].next) d->it[k].parent = c;
                    for (int k = d->it[nx].first; k >= 0; k = d->it[k].next) d->it[k].parent = nx;
                    if (d->sel == c) d->sel = nx; else if (d->sel == nx) d->sel = c;
                    sorted = 0;
                }
            }
        }
        invalidate(w);
        return 1;
    }
    case 0x1114: {                                                                         // TVM_ENSUREVISIBLE
        int i = tv_index(d, lp);
        if (i < 0) return 0;
        for (int a = d->it[i].parent; a >= 0; a = d->it[a].parent) d->it[a].state |= TVIS_EXPANDED;
        invalidate(w);
        return 1;
    }
    case 0x1110: return (uint64_t)(w->h / tv_item_h(w));                                  // TVM_GETVISIBLECOUNT
    case 0x111B: { int o = tv_item_h(w); d->item_h = (int)(int32_t)wp; invalidate(w); return (uint64_t)o; } // TVM_SETITEMHEIGHT
    case 0x111C: return (uint64_t)tv_item_h(w);
    case 0x111D: { uint32_t o = d->bk; d->bk = (uint32_t)lp; invalidate(w); return o; } // TVM_SETBKCOLOR
    case 0x111E: { uint32_t o = d->fg; d->fg = (uint32_t)lp; invalidate(w); return o; } // TVM_SETTEXTCOLOR
    case 0x111F: return d->bk;
    case 0x1120: return d->fg;
    case 0x110F: case 0x112C: case 0x112D: case 0x1118: case 0x1119: case 0x1121: case 0x1129: return 0;
    case 0x2005: case 0x2006: return 1;
    case 0x2007: case 0x2008: return 6;
    default:
        if (msg >= 0x1100 && msg < 0x1200) return 0;
        return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- tab control (SysTabControl32) -----------------------------------------------------------------

#define L_TCITEM "444P44P" // mask dwState dwStateMask pszText cchTextMax iImage lParam
typedef struct TcItem { char *text; int image; uint64_t lparam; } TcItem;
typedef struct TcData { TcItem *it; int n, cap, cur, focus; uint64_t iml; int item_w, item_h; } TcData;
static void tc_free(void *p) { TcData *d = p; for (int i = 0; i < d->n; i++) free(d->it[i].text); free(d->it); free(d); }
static TcData *tc_data(Win *w) { if (!w->cdata) { TcData *d = calloc(1, sizeof(TcData)); d->cur = -1; w->cdata = d; w->cdata_free = tc_free; } return w->cdata; }
static int tc_strip_h(Win *w) { TcData *d = tc_data(w); return d->item_h > 0 ? d->item_h : cc_line(w) + 10; }
static int tc_item_w(Win *w, int i) {
    TcData *d = tc_data(w);
    if (d->item_w > 0 && (w->style & 0x400)) return d->item_w; // TCS_FIXEDWIDTH
    int cx = 0, cy = 0;
    int img = d->it[i].image >= 0 && iml_size(d->iml, &cx, &cy);
    return cc_textw(w, d->it[i].text) + 18 + (img ? cx + 4 : 0);
}
static void tc_item_rect(Win *w, int i, int32_t *r) {
    int x = 2;
    for (int k = 0; k < i; k++) x += tc_item_w(w, k);
    r[0] = x; r[1] = 2; r[2] = x + tc_item_w(w, i); r[3] = tc_strip_h(w);
}
static void tc_paint(Win *w) {
    TcData *d = tc_data(w);
    CPaint p = cc_begin(w);
    int sh = tc_strip_h(w);
    gdi_fill_rect_px(p.hdc, 0, 0, w->w, sh, gui_sys_color(15)); // COLOR_BTNFACE behind the tabs
    gdi_frame_rect_px(p.hdc, 0, sh - 1, w->w, w->h, 0xD9D9D9);
    for (int i = 0; i < d->n; i++) {
        int32_t r[4];
        tc_item_rect(w, i, r);
        int sel = i == d->cur;
        int top = sel ? 0 : 2;
        gdi_fill_rect_px(p.hdc, r[0], top, r[2], sh + (sel ? 1 : 0), sel ? 0xFFFFFF : 0xF0F0F0);
        gdi_line_px(p.hdc, r[0], top, r[2], top, 0xD9D9D9, 1);
        gdi_line_px(p.hdc, r[0], top, r[0], sh, 0xD9D9D9, 1);
        gdi_line_px(p.hdc, r[2], top, r[2], sh, 0xD9D9D9, 1);
        int tx = r[0] + 9, cx, cy;
        if (d->it[i].image >= 0 && iml_size(d->iml, &cx, &cy)) { iml_draw(d->iml, d->it[i].image, p.hdc, tx, (sh - cy) / 2); tx += cx + 4; }
        cc_color(p.hdc, w->enabled ? 0x000000 : 0x6D6D6D);
        cc_text(p.hdc, d->it[i].text, tx, top, r[2] - 4, sh, DT_LEFT_VC);
        if (sel && gui()->focus == w->hwnd) gdi_frame_rect_px(p.hdc, r[0] + 3, top + 3, r[2] - 3, sh - 2, 0xA0A0A0);
    }
    cc_end(&p);
}
static void tc_set_cur(X86State *st, Win *w, int i, int notify) {
    TcData *d = tc_data(w);
    if (i < 0 || i >= d->n || i == d->cur) return;
    uint64_t h = w->hwnd;
    if (notify && notify_simple(st, w, -552)) return; // TCN_SELCHANGING vetoed
    if (!(w = win_get(h))) return;
    d = tc_data(w);
    d->cur = i; d->focus = i;
    invalidate(w);
    if (notify) notify_simple(st, w, -551); // TCN_SELCHANGE
}
static void tc_read(TcItem *it, uint64_t g, int wide) {
    uint32_t mask = (uint32_t)gs_get(g, L_TCITEM, 0);
    if (mask & 1) { char *t = cc_gstr(gs_get(g, L_TCITEM, 3), wide); free(it->text); it->text = t ? t : xstrdup(""); }
    if (mask & 2) it->image = (int32_t)gs_get(g, L_TCITEM, 5);
    if (mask & 8) it->lparam = gs_get(g, L_TCITEM, 6);
}
static uint64_t tab_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    TcData *d = tc_data(w);
    uint64_t h = w->hwnd;
    switch (msg) {
    case WM_PAINT: tc_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: case WM_KILLFOCUS: invalidate(w); return 0;
    case WM_GETDLGCODE: return 1;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        int x = (int16_t)LOWORD(lp), y = (int16_t)HIWORD(lp);
        for (int i = 0; i < d->n && y < tc_strip_h(w); i++) {
            int32_t r[4];
            tc_item_rect(w, i, r);
            if (x >= r[0] && x < r[2]) {
                if (msg == WM_LBUTTONDOWN) { if (!(w->style & 0x10000)) win_set_focus(st, h); if ((w = win_get(h))) tc_set_cur(st, w, i, 1); }
                else notify_simple(st, w, -5);
                return 0;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_LEFT) tc_set_cur(st, w, d->cur - 1, 1);
        else if (wp == VK_RIGHT) tc_set_cur(st, w, d->cur + 1, 1);
        return 0;
    case 0x1304: return (uint64_t)d->n;                                       // TCM_GETITEMCOUNT
    case 0x1307: case 0x133E: {                                               // TCM_INSERTITEM
        if (!lp) return (uint64_t)-1;
        if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 8; d->it = realloc(d->it, sizeof(TcItem) * (size_t)d->cap); }
        int at = (int)(int32_t)wp;
        if (at < 0 || at > d->n) at = d->n;
        memmove(&d->it[at + 1], &d->it[at], sizeof(TcItem) * (size_t)(d->n - at));
        memset(&d->it[at], 0, sizeof(TcItem));
        d->it[at].image = -1;
        tc_read(&d->it[at], lp, msg == 0x133E);
        if (!d->it[at].text) d->it[at].text = xstrdup("");
        d->n++;
        if (d->cur < 0) d->cur = 0; else if (at <= d->cur && d->n > 1) d->cur++;
        invalidate(w);
        return (uint64_t)at;
    }
    case 0x1306: case 0x133D: { int i = (int)(int32_t)wp; if (i < 0 || i >= d->n || !lp) return 0; tc_read(&d->it[i], lp, msg == 0x133D); invalidate(w); return 1; } // TCM_SETITEM
    case 0x1305: case 0x133C: {                                               // TCM_GETITEM
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= d->n || !lp) return 0;
        uint32_t mask = (uint32_t)gs_get(lp, L_TCITEM, 0);
        if (mask & 1) cc_put(gs_get(lp, L_TCITEM, 3), (size_t)(int32_t)gs_get(lp, L_TCITEM, 4), d->it[i].text, msg == 0x133C);
        if (mask & 2) gs_set(lp, L_TCITEM, 5, (uint32_t)d->it[i].image);
        if (mask & 8) gs_set(lp, L_TCITEM, 6, d->it[i].lparam);
        return 1;
    }
    case 0x1308: {                                                            // TCM_DELETEITEM
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= d->n) return 0;
        free(d->it[i].text);
        memmove(&d->it[i], &d->it[i + 1], sizeof(TcItem) * (size_t)(d->n - i - 1));
        d->n--;
        if (d->cur >= d->n) d->cur = d->n - 1;
        invalidate(w);
        return 1;
    }
    case 0x1309: for (int i = 0; i < d->n; i++) free(d->it[i].text); d->n = 0; d->cur = -1; invalidate(w); return 1; // TCM_DELETEALLITEMS
    case 0x130B: return (uint64_t)(int64_t)d->cur;                            // TCM_GETCURSEL
    case 0x130C: { int o = d->cur; int i = (int)(int32_t)wp; if (i >= 0 && i < d->n) { d->cur = i; d->focus = i; invalidate(w); } return (uint64_t)(int64_t)o; } // TCM_SETCURSEL
    case 0x132F: return (uint64_t)(int64_t)d->focus;                          // TCM_GETCURFOCUS
    case 0x1330: tc_set_cur(st, w, (int)(int32_t)wp, 1); return 0;           // TCM_SETCURFOCUS
    case 0x130A: { int i = (int)(int32_t)wp; if (i < 0 || i >= d->n || !lp) return 0; int32_t r[4]; tc_item_rect(w, i, r); memcpy(G2H(lp), r, 16); return 1; } // TCM_GETITEMRECT
    case 0x130D: {                                                            // TCM_HITTEST (TCHITTESTINFO: pt, flags)
        int x = (int32_t)mem_rd(lp, 4), y = (int32_t)mem_rd(lp + 4, 4);
        for (int i = 0; i < d->n; i++) { int32_t r[4]; tc_item_rect(w, i, r); if (x >= r[0] && x < r[2] && y >= 0 && y < r[3]) { mem_wr(lp + 8, 4, 6); return (uint64_t)i; } }
        mem_wr(lp + 8, 4, 1);
        return (uint64_t)-1;
    }
    case 0x1328: {                                                            // TCM_ADJUSTRECT
        if (!lp) return 0;
        int32_t r[4]; memcpy(r, G2H(lp), 16);
        int sh = tc_strip_h(w);
        if (wp) { r[0] -= 4; r[1] -= sh + 4; r[2] += 4; r[3] += 4; }
        else { r[0] += 4; r[1] += sh + 4; r[2] -= 4; r[3] -= 4; }
        memcpy(G2H(lp), r, 16);
        return 0;
    }
    case 0x1303: { uint64_t o = d->iml; d->iml = lp; invalidate(w); return o; } // TCM_SETIMAGELIST
    case 0x1302: return d->iml;
    case 0x1329: { uint32_t o = ((uint32_t)tc_strip_h(w) << 16) | 80; d->item_w = (int)LOWORD(lp); d->item_h = (int)HIWORD(lp); invalidate(w); return o; } // TCM_SETITEMSIZE
    case 0x132C: return 1;                                                    // TCM_GETROWCOUNT
    case 0x132B: case 0x1331: case 0x1332: case 0x1333: case 0x1334: case 0x1335: case 0x132D: case 0x132E: return 0;
    case 0x2005: case 0x2006: return 1;
    default:
        if (msg >= 0x1300 && msg < 0x1400) return 0;
        return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

int comctl_transparent_at(Win *w, int x, int y) {
    (void)x;
    if (w->ctl == CTL_TAB) return y >= tc_strip_h(w);
    return 0;
}

// ---- toolbar (ToolbarWindow32) ----------------------------------------------------------------------

#define L_TBBUTTON "44112PP" // iBitmap idCommand fsState fsStyle bReserved dwData iString
typedef struct TbButton { int image, id; uint8_t state, style; uint64_t data; char *text; } TbButton;
typedef struct TbData { TbButton *b; int n, cap; uint64_t iml, own_iml; char **strings; int nstr; int bmp_w, bmp_h, btn_w, btn_h; int hot, pressed; uint32_t ex; } TbData;
static void tb_free(void *p) {
    TbData *d = p;
    for (int i = 0; i < d->n; i++) free(d->b[i].text);
    for (int i = 0; i < d->nstr; i++) free(d->strings[i]);
    free(d->b); free(d->strings);
    ImgList *l = iml_get(d->own_iml);
    if (l) { free(l->px); memset(l, 0, sizeof *l); }
    free(d);
}
static TbData *tb_data(Win *w) { if (!w->cdata) { TbData *d = calloc(1, sizeof(TbData)); d->bmp_w = 16; d->bmp_h = 15; d->hot = d->pressed = -1; w->cdata = d; w->cdata_free = tb_free; } return w->cdata; }
static int tb_has_text(TbData *d) { for (int i = 0; i < d->n; i++) if (d->b[i].text && d->b[i].text[0]) return 1; return 0; }
static int tb_list(Win *w) { return (w->style & 0x1000) != 0; } // TBSTYLE_LIST
static int tb_btn_w(Win *w, TbData *d, int i) {
    TbButton *b = &d->b[i];
    if (b->style & 1) return 8; // separator
    if (b->state & 8) return 0; // hidden
    int iw = d->iml ? d->bmp_w : 0, tw = b->text && b->text[0] ? cc_textw(w, b->text) : 0;
    int bw = tb_list(w) ? iw + (tw ? tw + 6 : 0) + 10 : (tw + 10 > iw + 8 ? tw + 10 : iw + 8);
    if (!tb_list(w) && tb_has_text(d) && tw == 0) bw = iw + 8;
    if (d->btn_w > bw && !tb_list(w)) bw = d->btn_w;
    if (b->style & 8) bw += 10; // BTNS_DROPDOWN arrow
    return bw;
}
static int tb_height(Win *w) {
    TbData *d = tb_data(w);
    int h = (d->iml ? d->bmp_h : 0) + 8;
    if (tb_has_text(d) && !tb_list(w)) h += cc_line(w) + 2;
    if (tb_has_text(d) && tb_list(w) && cc_line(w) + 8 > h) h = cc_line(w) + 8;
    if (d->btn_h > h) h = d->btn_h;
    return h < 22 ? 22 : h;
}
static void tb_rect(Win *w, int i, int32_t *r) {
    TbData *d = tb_data(w);
    int x = 3;
    for (int k = 0; k < i; k++) x += tb_btn_w(w, d, k);
    r[0] = x; r[1] = 2; r[2] = x + tb_btn_w(w, d, i); r[3] = tb_height(w) - 2;
}
static void tb_autosize(Win *w) {
    if (w->style & 4) return; // CCS_NORESIZE
    Win *p = win_get(w->parent);
    int h = tb_height(w);
    if (p && !(w->style & 8)) { w->x = 0; w->y = (w->style & 3) == 3 ? p->h - h : 0; w->w = p->w; } // CCS_TOP / CCS_BOTTOM
    w->h = h;
    if (p) win_invalidate_rect(p, NULL, 1);
    invalidate(w);
}
static void tb_paint(Win *w) {
    TbData *d = tb_data(w);
    CPaint p = cc_begin(w);
    if (!(w->style & 0x800)) gdi_fill_rect_px(p.hdc, 0, 0, w->w, w->h, gui_sys_color(15)); // unless TBSTYLE_TRANSPARENT
    int text_below = tb_has_text(d) && !tb_list(w);
    for (int i = 0; i < d->n; i++) {
        TbButton *b = &d->b[i];
        if (b->state & 8) continue;
        int32_t r[4];
        tb_rect(w, i, r);
        if (b->style & 1) { gdi_line_px(p.hdc, r[0] + 3, r[1] + 3, r[0] + 3, r[3] - 3, 0xC0C0C0, 1); continue; }
        int enabled = (b->state & 4) != 0 && w->enabled, checked = (b->state & 1) != 0;
        if (enabled && (i == d->pressed || checked)) gdi_fill_rect_px(p.hdc, r[0], r[1], r[2], r[3], i == d->pressed ? 0xF7E4CC : 0xFCF0E0);
        else if (enabled && i == d->hot) gdi_fill_rect_px(p.hdc, r[0], r[1], r[2], r[3], 0xFCF0E0);
        if (enabled && (i == d->pressed || i == d->hot || checked)) gdi_frame_rect_px(p.hdc, r[0], r[1], r[2], r[3], 0xD7A060);
        int ix, iy;
        if (tb_list(w)) { ix = r[0] + 5; iy = (r[1] + r[3] - d->bmp_h) / 2; }
        else { ix = (r[0] + r[2] - d->bmp_w) / 2 - ((b->style & 8) ? 5 : 0); iy = r[1] + 3; }
        if (d->iml && b->image >= 0) iml_draw(d->iml, b->image, p.hdc, ix, iy);
        if (b->text && b->text[0]) {
            cc_color(p.hdc, enabled ? 0x000000 : 0x8D8D8D);
            if (tb_list(w)) cc_text(p.hdc, b->text, ix + (d->iml ? d->bmp_w + 4 : 0), r[1], r[2] - 3, r[3], DT_LEFT_VC);
            else if (text_below) cc_text(p.hdc, b->text, r[0] + 2, iy + (d->iml ? d->bmp_h : 0) + 1, r[2] - 2, r[3], 1 | 0x20 | 0x800);
        }
        if (b->style & 8) { int ax = r[2] - 9, ay = (r[1] + r[3]) / 2; for (int k = 0; k < 3; k++) gdi_line_px(p.hdc, ax + k, ay - 1 + k, ax + 5 - k, ay - 1 + k, 0x404040, 1); }
    }
    cc_end(&p);
}
static int tb_hit(Win *w, int x, int y) {
    TbData *d = tb_data(w);
    for (int i = 0; i < d->n; i++) { int32_t r[4]; tb_rect(w, i, r); if (x >= r[0] && x < r[2] && y >= r[1] && y < r[3] && !(d->b[i].style & 1) && !(d->b[i].state & 8)) return i; }
    return -1;
}
static int tb_index_of(TbData *d, int id) { for (int i = 0; i < d->n; i++) if (d->b[i].id == id) return i; return -1; }
static void tb_read_button(Win *w, TbButton *b, uint64_t g, int wide) {
    TbData *d = tb_data(w);
    b->image = (int32_t)gs_get(g, L_TBBUTTON, 0);
    b->id = (int32_t)gs_get(g, L_TBBUTTON, 1);
    b->state = (uint8_t)gs_get(g, L_TBBUTTON, 2);
    b->style = (uint8_t)gs_get(g, L_TBBUTTON, 3);
    b->data = gs_get(g, L_TBBUTTON, 5);
    uint64_t s = gs_get(g, L_TBBUTTON, 6);
    b->text = NULL;
    if (s >= 0x10000 && !(g_mode32 && (int32_t)s < 0)) b->text = cc_gstr(s, wide);
    else if ((int64_t)s >= 0 && (int)s < d->nstr) b->text = xstrdup(d->strings[(int)s]);
}
static int tb_add_strings(TbData *d, uint64_t hinst, uint64_t p, int wide) {
    int first = d->nstr;
    if (hinst || p < 0x10000) return first; // string resources: not loaded
    for (;;) {
        char *s = cc_gstr(p, wide);
        if (!s || !*s) { free(s); break; }
        d->strings = realloc(d->strings, sizeof(char *) * (size_t)(d->nstr + 1));
        d->strings[d->nstr++] = s;
        p += wide ? (wstrlen((const uint16_t *)G2H(p)) + 1) * 2 : strlen((const char *)G2H(p)) + 1;
    }
    return first;
}
static int tb_add_bitmap(Win *w, int nbuttons, uint64_t hinst, uint64_t id) {
    TbData *d = tb_data(w);
    ImgList *l = iml_get(d->own_iml);
    if (!l) { d->own_iml = iml_create(d->bmp_w, d->bmp_h, 0x20); l = iml_get(d->own_iml); if (!d->iml) d->iml = d->own_iml; }
    if (!l) return -1;
    int is_comctl = hinst == (uint64_t)-1 || (g_mode32 && (uint32_t)hinst == 0xFFFFFFFFu);
    if (is_comctl) return iml_add_std(l, (id & 1) ? 15 : 15); // IDB_STD_*/IDB_VIEW_*/IDB_HIST_*: generic glyphs
    uint64_t hbm = 0;
    int temp = 0;
    if (!hinst) hbm = id;
    else { const void *r = pe_resource(hinst, 2, id, 1, NULL); if (r) { hbm = gdi_bitmap_from_dib(r, NULL); temp = 1; } }
    int first = -1;
    if (hbm) first = iml_add_bitmap(l, hbm, 0, 0xC0C0C0, -1); // classic toolbar bitmaps use light grey as transparency
    if (temp) gdi_free(hbm);
    (void)nbuttons;
    return first;
}
static uint64_t toolbar_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    TbData *d = tb_data(w);
    GuiState *g = gui();
    uint64_t h = w->hwnd;
    switch (msg) {
    case WM_CREATE: tb_autosize(w); return 0;
    case WM_PAINT: tb_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: { int i = tb_hit(w, (int16_t)LOWORD(lp), (int16_t)HIWORD(lp)); if (i != d->hot) { d->hot = i; invalidate(w); } return 0; }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int i = tb_hit(w, (int16_t)LOWORD(lp), (int16_t)HIWORD(lp));
        if (i >= 0 && (d->b[i].state & 4)) {
            d->pressed = i; g->capture = h; invalidate(w);
            if (d->b[i].style & 8) { // BTNS_DROPDOWN: TBN_DROPDOWN (NMTOOLBAR: hdr iItem TBBUTTON pszText rcButton)
                Nm n = nm_new(w, -710, "PP4|4", gs_size(L_TBBUTTON) + 48);
                gs_set(n.g, n.L, 3, (uint32_t)d->b[i].id);
                nm_send(st, w, &n);
                nm_free(&n);
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g->capture == h) g->capture = 0;
        int i = d->pressed, at = tb_hit(w, (int16_t)LOWORD(lp), (int16_t)HIWORD(lp));
        d->pressed = -1;
        invalidate(w);
        if (i >= 0 && i == at && (d->b[i].state & 4)) {
            if (d->b[i].style & 2) d->b[i].state ^= 1; // TBSTYLE_CHECK
            int id = d->b[i].id;
            notify_simple(st, w, -2); // NM_CLICK
            if ((w = win_get(h))) win_send(st, w->parent, WM_COMMAND, (uint32_t)id & 0xFFFF, h);
        }
        return 0;
    }
    case 0x41E: return 0;                                                        // TB_BUTTONSTRUCTSIZE
    case 0x414: case 0x444: case 0x415: case 0x443: {                           // TB_ADDBUTTONS / TB_INSERTBUTTON
        int add = msg == 0x414 || msg == 0x444, wide = msg == 0x444 || msg == 0x443;
        int cnt = add ? (int)(int32_t)wp : 1, at = add ? d->n : (int)(int32_t)wp;
        if (at < 0 || at > d->n) at = d->n;
        size_t sz = gs_size(L_TBBUTTON);
        for (int k = 0; k < cnt; k++) {
            if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 16; d->b = realloc(d->b, sizeof(TbButton) * (size_t)d->cap); }
            memmove(&d->b[at + 1], &d->b[at], sizeof(TbButton) * (size_t)(d->n - at));
            tb_read_button(w, &d->b[at], lp + sz * (uint64_t)k, wide);
            d->n++;
            at++;
        }
        tb_autosize(w);
        return 1;
    }
    case 0x416: { int i = (int)(int32_t)wp; if (i < 0 || i >= d->n) return 0; free(d->b[i].text); memmove(&d->b[i], &d->b[i + 1], sizeof(TbButton) * (size_t)(d->n - i - 1)); d->n--; invalidate(w); return 1; } // TB_DELETEBUTTON
    case 0x417: {                                                                // TB_GETBUTTON
        int i = (int)(int32_t)wp;
        if (i < 0 || i >= d->n || !lp) return 0;
        gs_set(lp, L_TBBUTTON, 0, (uint32_t)d->b[i].image); gs_set(lp, L_TBBUTTON, 1, (uint32_t)d->b[i].id);
        gs_set(lp, L_TBBUTTON, 2, d->b[i].state); gs_set(lp, L_TBBUTTON, 3, d->b[i].style);
        gs_set(lp, L_TBBUTTON, 5, d->b[i].data); gs_set(lp, L_TBBUTTON, 6, 0);
        return 1;
    }
    case 0x418: return (uint64_t)d->n;                                           // TB_BUTTONCOUNT
    case 0x419: return (uint64_t)(int64_t)tb_index_of(d, (int)(int32_t)wp);      // TB_COMMANDTOINDEX
    case 0x401: case 0x402: case 0x403: case 0x404: case 0x40C: {               // TB_ENABLEBUTTON / CHECK / PRESS / HIDE / INDETERMINATE
        int i = tb_index_of(d, (int)(int32_t)wp);
        if (i < 0) return 0;
        uint8_t bit = msg == 0x401 ? 4 : msg == 0x402 ? 1 : msg == 0x403 ? 2 : msg == 0x404 ? 8 : 16;
        if (LOWORD(lp)) d->b[i].state |= bit; else d->b[i].state &= (uint8_t)~bit;
        if (msg == 0x404) tb_autosize(w); else invalidate(w);
        return 1;
    }
    case 0x409: case 0x40A: case 0x40B: case 0x40D: {                           // TB_ISBUTTONENABLED / CHECKED / PRESSED / HIDDEN
        int i = tb_index_of(d, (int)(int32_t)wp);
        if (i < 0) return 0;
        uint8_t bit = msg == 0x409 ? 4 : msg == 0x40A ? 1 : msg == 0x40B ? 2 : 8;
        return (d->b[i].state & bit) != 0;
    }
    case 0x411: { int i = tb_index_of(d, (int)(int32_t)wp); if (i < 0) return 0; d->b[i].state = (uint8_t)lp; invalidate(w); return 1; } // TB_SETSTATE
    case 0x412: { int i = tb_index_of(d, (int)(int32_t)wp); return i < 0 ? (uint64_t)-1 : d->b[i].state; } // TB_GETSTATE
    case 0x413: { uint64_t hi = rd_ptr(lp), id = rd_ptr(lp + PSZ); int r = tb_add_bitmap(w, (int)(int32_t)wp, hi, id); tb_autosize(w); return (uint64_t)(int64_t)r; } // TB_ADDBITMAP
    case 0x432: { int r = tb_add_bitmap(w, 0, lp, wp); tb_autosize(w); return (uint64_t)(int64_t)r; } // TB_LOADIMAGES
    case 0x41C: case 0x44D: return (uint64_t)(int64_t)tb_add_strings(d, wp, lp, msg == 0x44D); // TB_ADDSTRING
    case 0x421: tb_autosize(w); return 0;                                        // TB_AUTOSIZE
    case 0x430: { uint64_t o = d->iml; d->iml = lp; int cx, cy; if (iml_size(lp, &cx, &cy)) { d->bmp_w = cx; d->bmp_h = cy; } tb_autosize(w); return o; } // TB_SETIMAGELIST
    case 0x431: return d->iml;                                                   // TB_GETIMAGELIST
    case 0x420: d->bmp_w = (int)LOWORD(lp); d->bmp_h = (int)HIWORD(lp); return 1; // TB_SETBITMAPSIZE
    case 0x41F: d->btn_w = (int)LOWORD(lp); d->btn_h = (int)HIWORD(lp); tb_autosize(w); return 1; // TB_SETBUTTONSIZE
    case 0x43A: { int32_t r[4]; if (d->n) tb_rect(w, 0, r); else { r[0] = 0; r[2] = 24; } return ((uint64_t)(uint32_t)tb_height(w) << 16) | (uint32_t)(r[2] - r[0]); } // TB_GETBUTTONSIZE
    case 0x41D: case 0x433: {                                                    // TB_GETITEMRECT / TB_GETRECT
        int i = msg == 0x41D ? (int)(int32_t)wp : tb_index_of(d, (int)(int32_t)wp);
        if (i < 0 || i >= d->n || !lp) return 0;
        int32_t r[4]; tb_rect(w, i, r);
        memcpy(G2H(lp), r, 16);
        return 1;
    }
    case 0x445: return (uint64_t)(int64_t)tb_hit(w, (int32_t)mem_rd(lp, 4), (int32_t)mem_rd(lp + 4, 4)); // TB_HITTEST
    case 0x42D: case 0x44B: {                                                    // TB_GETBUTTONTEXT
        int i = tb_index_of(d, (int)(int32_t)wp);
        if (i < 0) return (uint64_t)-1;
        const char *t = d->b[i].text ? d->b[i].text : "";
        if (lp) cc_put(lp, 256, t, msg == 0x44B);
        return msg == 0x44B ? u2w(t, -1, NULL, 0) : strlen(t);
    }
    case 0x440: case 0x442: case 0x43F: case 0x441: {                           // TB_SETBUTTONINFO / TB_GETBUTTONINFO
        // TBBUTTONINFO: cbSize dwMask idCommand iImage fsState fsStyle cx lParam pszText cchText
        const char *L = "4444112PP4";
        int by_index = ((uint32_t)gs_get(lp, L, 1) & 0x80000000u) != 0;
        int i = by_index ? (int)(int32_t)wp : tb_index_of(d, (int)(int32_t)wp);
        if (i < 0 || i >= d->n || !lp) return (uint64_t)-1;
        uint32_t mask = (uint32_t)gs_get(lp, L, 1);
        int wide = msg == 0x440 || msg == 0x43F, set = msg == 0x440 || msg == 0x442;
        TbButton *b = &d->b[i];
        if (set) {
            if (mask & 0x20) b->id = (int32_t)gs_get(lp, L, 2);
            if (mask & 1) b->image = (int32_t)gs_get(lp, L, 3);
            if (mask & 4) b->state = (uint8_t)gs_get(lp, L, 4);
            if (mask & 8) b->style = (uint8_t)gs_get(lp, L, 5);
            if (mask & 0x10) b->data = gs_get(lp, L, 7);
            if (mask & 2) { char *t = cc_gstr(gs_get(lp, L, 8), wide); free(b->text); b->text = t; }
            tb_autosize(w);
            return 1;
        }
        if (mask & 0x20) gs_set(lp, L, 2, (uint32_t)b->id);
        if (mask & 1) gs_set(lp, L, 3, (uint32_t)b->image);
        if (mask & 4) gs_set(lp, L, 4, b->state);
        if (mask & 8) gs_set(lp, L, 5, b->style);
        if (mask & 0x10) gs_set(lp, L, 7, b->data);
        if (mask & 2) cc_put(gs_get(lp, L, 8), (size_t)(int32_t)gs_get(lp, L, 9), b->text, wide);
        return (uint64_t)i;
    }
    case 0x454: { uint32_t o = d->ex; d->ex = (uint32_t)lp; return o; }         // TB_SETEXTENDEDSTYLE
    case 0x455: return d->ex;
    case 0x428: return 1;                                                        // TB_GETROWS
    case 0x453: case 0x463: { if (lp) { int32_t r[4]; int x = 3; for (int i = 0; i < d->n; i++) x += tb_btn_w(w, d, i); mem_wr(lp, 4, (uint32_t)x); mem_wr(lp + 4, 4, (uint32_t)tb_height(w)); (void)r; } return 1; } // TB_GETMAXSIZE / GETIDEALSIZE
    case 0x423: case 0x425: case 0x427: case 0x42F: case 0x434: case 0x435: case 0x436: case 0x437: case 0x438: case 0x43C:
    case 0x446: case 0x447: case 0x448: case 0x449: case 0x44A: case 0x456: case 0x457: case 0x458: case 0x459: case 0x45A: case 0x46B: return 0;
    case 0x2005: case 0x2006: return 1;
    case 0x2007: case 0x2008: return 6;
    default:
        if (msg >= 0x400 && msg < 0x500) return 0;
        return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- up-down control (msctls_updown32) -----------------------------------------------------------------

typedef struct UdData { int lo, hi, pos; uint64_t buddy; int pressed; int base; } UdData;
static void ud_free(void *p) { free(p); }
static UdData *ud_data(Win *w) { if (!w->cdata) { UdData *d = calloc(1, sizeof(UdData)); d->lo = 100; d->hi = 0; d->pressed = -1; d->base = 10; w->cdata = d; w->cdata_free = ud_free; } return w->cdata; }
static void ud_sync_buddy(X86State *st, Win *w) {
    UdData *d = ud_data(w);
    if (!(w->style & 2) || !win_get(d->buddy)) return; // UDS_SETBUDDYINT
    char buf[32];
    if (d->base == 16) snprintf(buf, sizeof buf, "0x%X", d->pos);
    else if ((w->style & 0x80) || (d->pos < 1000 && d->pos > -1000)) snprintf(buf, sizeof buf, "%d", d->pos); // UDS_NOTHOUSANDS
    else { char t[32]; snprintf(t, sizeof t, "%d", d->pos < 0 ? -d->pos : d->pos); size_t n = strlen(t); char *o = buf; if (d->pos < 0) *o++ = '-'; for (size_t k = 0; k < n; k++) { *o++ = t[k]; if ((n - k - 1) % 3 == 0 && k + 1 < n) *o++ = '.'; } *o = 0; }
    char *g = g_strdup(buf);
    win_send_ex(st, d->buddy, WM_SETTEXT, 0, H2G(g), 0);
    g_free(g);
}
static void ud_attach(X86State *st, Win *w) {
    UdData *d = ud_data(w);
    Win *b = win_get(d->buddy);
    if (!b) return;
    if (w->style & 0xC) { // UDS_ALIGNRIGHT / ALIGNLEFT: sit next to the buddy
        int bw = 17;
        w->h = b->h; w->y = b->y;
        if (w->style & 4) { b->w -= bw; w->x = b->x + b->w; }
        else { w->x = b->x; b->x += bw; b->w -= bw; }
        w->w = bw;
        Win *p = win_get(w->parent);
        if (p) win_invalidate_rect(p, NULL, 1);
    }
    ud_sync_buddy(st, w);
}
static void ud_step(X86State *st, Win *w, int delta) {
    UdData *d = ud_data(w);
    uint64_t h = w->hwnd;
    int lo = d->lo < d->hi ? d->lo : d->hi, hi = d->lo < d->hi ? d->hi : d->lo;
    if (d->lo > d->hi) delta = -delta; // reversed range: the up arrow decreases
    Nm n = nm_new(w, -722, "PP4|44", 0); // UDN_DELTAPOS (NMUPDOWN: iPos iDelta)
    gs_set(n.g, n.L, 3, (uint32_t)d->pos); gs_set(n.g, n.L, 4, (uint32_t)delta);
    uint64_t veto = nm_send(st, w, &n);
    nm_free(&n);
    if (veto || !(w = win_get(h))) return;
    d = ud_data(w);
    int np = d->pos + delta;
    if (np > hi) np = (w->style & 1) ? lo : hi;  // UDS_WRAP
    if (np < lo) np = (w->style & 1) ? hi : lo;
    if (np == d->pos) return;
    d->pos = np;
    ud_sync_buddy(st, w);
    if (!(w = win_get(h))) return;
    win_send(st, w->parent, (w->style & 0x40) ? 0x114 : 0x115, ((uint64_t)(uint16_t)np << 16) | 4, h); // WM_H/VSCROLL, SB_THUMBPOSITION
}
static uint64_t updown_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    UdData *d = ud_data(w);
    switch (msg) {
    case WM_CREATE:
        if (w->style & 0x10) { // UDS_AUTOBUDDY: the previous sibling
            GuiState *g = gui();
            Win *best = NULL;
            for (int i = 0; i < WIN_MAX; i++) { Win *s = &g->wins[i]; if (s->used && s != w && s->parent == w->parent && s->seq < w->seq && (!best || s->seq > best->seq)) best = s; }
            if (best) { d->buddy = best->hwnd; ud_attach(st, w); }
        }
        return 0;
    case WM_PAINT: {
        CPaint p = cc_begin(w);
        int horz = (w->style & 0x40) != 0, half = horz ? w->w / 2 : w->h / 2;
        for (int k = 0; k < 2; k++) {
            int x0 = horz ? k * half : 0, y0 = horz ? 0 : k * half, x1 = horz ? x0 + half : w->w, y1 = horz ? w->h : y0 + half;
            gdi_fill_rect_px(p.hdc, x0, y0, x1, y1, d->pressed == k ? 0xF7E4CC : 0xF0F0F0);
            gdi_frame_rect_px(p.hdc, x0, y0, x1, y1, 0xADADAD);
            int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
            for (int r = 0; r < 3; r++) {
                if (horz) { int xx = k == 0 ? cx - 1 + r : cx + 1 - r; gdi_line_px(p.hdc, xx, cy - r, xx, cy + r + 1, w->enabled ? 0x404040 : 0xA0A0A0, 1); }
                else { int yy = k == 0 ? cy - 1 + r : cy + 1 - r; gdi_line_px(p.hdc, cx - r, yy, cx + r + 1, yy, w->enabled ? 0x404040 : 0xA0A0A0, 1); }
            }
        }
        cc_end(&p);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int horz = (w->style & 0x40) != 0;
        int k = horz ? ((int16_t)LOWORD(lp) < w->w / 2 ? 0 : 1) : ((int16_t)HIWORD(lp) < w->h / 2 ? 0 : 1);
        d->pressed = k;
        invalidate(w);
        ud_step(st, w, horz ? (k == 0 ? -1 : 1) : (k == 0 ? 1 : -1));
        return 0;
    }
    case WM_LBUTTONUP: d->pressed = -1; invalidate(w); return 0;
    case 0x465: d->hi = (int16_t)LOWORD(lp); d->lo = (int16_t)HIWORD(lp); return 0;      // UDM_SETRANGE (max, min)
    case 0x466: return ((uint64_t)(uint16_t)d->lo << 16) | (uint16_t)d->hi;              // UDM_GETRANGE
    case 0x467: case 0x471: { int o = d->pos; d->pos = msg == 0x467 ? (int16_t)LOWORD(lp) : (int)(int32_t)lp; ud_sync_buddy(st, w); return (uint64_t)(uint32_t)o; } // UDM_SETPOS(32)
    case 0x468: return (uint16_t)d->pos;                                                  // UDM_GETPOS
    case 0x472: if (lp) mem_wr(lp, 4, 0); return (uint64_t)(int64_t)d->pos;              // UDM_GETPOS32
    case 0x469: { uint64_t o = d->buddy; d->buddy = wp; ud_attach(st, w); return o; }      // UDM_SETBUDDY
    case 0x46A: return d->buddy;
    case 0x46F: d->lo = (int)(int32_t)wp; d->hi = (int)(int32_t)lp; return 0;            // UDM_SETRANGE32
    case 0x470: if (wp) mem_wr(wp, 4, (uint32_t)d->lo); if (lp) mem_wr(lp, 4, (uint32_t)d->hi); return 0;
    case 0x46D: { int o = d->base; d->base = (int)wp == 16 ? 16 : 10; return (uint64_t)o; } // UDM_SETBASE
    case 0x46E: return (uint64_t)d->base;
    case 0x46B: case 0x46C: return 1;                                                     // UDM_SETACCEL / GETACCEL
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- SysLink -----------------------------------------------------------------------------------------

typedef struct LinkSeg { char *text; int link; char url[512]; char id[48]; } LinkSeg;
typedef struct LinkData { LinkSeg *s; int n; } LinkData;
static void link_free(void *p) { LinkData *d = p; for (int i = 0; i < d->n; i++) free(d->s[i].text); free(d->s); free(d); }
static LinkData *link_parse(Win *w) {
    if (w->cdata) link_free(w->cdata);
    LinkData *d = calloc(1, sizeof(LinkData));
    w->cdata = d; w->cdata_free = link_free;
    const char *p = w->text ? w->text : "";
    while (*p) {
        const char *a = strcasestr(p, "<a");
        const char *end = a ? a : p + strlen(p);
        if (end > p) { d->s = realloc(d->s, sizeof(LinkSeg) * (size_t)(d->n + 1)); LinkSeg *s = &d->s[d->n++]; memset(s, 0, sizeof *s); s->text = strndup(p, (size_t)(end - p)); }
        if (!a) break;
        const char *gt = strchr(a, '>'), *close = gt ? strcasestr(gt, "</a>") : NULL;
        if (!gt || !close) { p = a + 2; continue; }
        d->s = realloc(d->s, sizeof(LinkSeg) * (size_t)(d->n + 1));
        LinkSeg *s = &d->s[d->n++];
        memset(s, 0, sizeof *s);
        s->link = 1;
        s->text = strndup(gt + 1, (size_t)(close - gt - 1));
        const char *hr = strcasestr(a, "href=");
        if (hr && hr < gt) { hr += 5; char q = *hr == '"' || *hr == '\'' ? *hr++ : ' '; size_t k = 0; while (*hr && *hr != q && hr < gt && k + 1 < sizeof s->url) s->url[k++] = *hr++; s->url[k] = 0; }
        const char *idp = strcasestr(a, "id=");
        if (idp && idp < gt) { idp += 3; char q = *idp == '"' || *idp == '\'' ? *idp++ : ' '; size_t k = 0; while (*idp && *idp != q && idp < gt && k + 1 < sizeof s->id) s->id[k++] = *idp++; s->id[k] = 0; }
        p = close + 4;
    }
    return d;
}
static uint64_t syslink_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    switch (msg) {
    case WM_PAINT: {
        LinkData *d = w->cdata ? w->cdata : link_parse(w);
        CPaint p = cc_begin(w);
        Win *par = win_get(w->parent);
        (void)par;
        int x = 0;
        for (int i = 0; i < d->n; i++) {
            int tw = cc_textw(w, d->s[i].text);
            cc_color(p.hdc, d->s[i].link ? 0xCC6600 : 0x000000);
            cc_text(p.hdc, d->s[i].text, x, 0, x + tw + 2, w->h, 0x20 | 0x800);
            if (d->s[i].link) { int lh = cc_line(w); gdi_line_px(p.hdc, x, lh, x + tw, lh, 0xCC6600, 1); }
            x += tw;
        }
        cc_end(&p);
        return 0;
    }
    case WM_SETTEXT: { uint64_t r = win_def_proc(st, w, msg, wp, lp, unicode); link_parse(w); invalidate(w); return r; }
    case WM_LBUTTONDOWN: {
        LinkData *d = w->cdata ? w->cdata : link_parse(w);
        int x = (int16_t)LOWORD(lp), cx = 0, idx = 0;
        for (int i = 0; i < d->n; i++) {
            int tw = cc_textw(w, d->s[i].text);
            if (d->s[i].link) {
                if (x >= cx && x < cx + tw) {
                    // NMLINK: hdr + LITEM { mask, iLink, state, stateMask, szID[48], szUrl[2084] } (WCHAR)
                    Nm n = nm_new(w, -2, "PP4|4444", 96 + 2084 * 2);
                    uint64_t li = n.g + gs_off("PP4|4444", 3);
                    mem_wr(li, 4, 1); mem_wr(li + 4, 4, (uint32_t)idx);
                    u2w(d->s[i].id, -1, (uint16_t *)G2H(li + 16), 48);
                    u2w(d->s[i].url, -1, (uint16_t *)G2H(li + 16 + 96), 2084);
                    nm_send(st, w, &n);
                    nm_free(&n);
                    return 0;
                }
                idx++;
            }
            cx += tw;
        }
        return 0;
    }
    case WM_ERASEBKGND: return 0;
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}

// ---- dispatch ------------------------------------------------------------------------------------------

uint64_t comctl_proc(X86State *st, Win *w, int ctl, uint32_t msg, uint64_t wp, uint64_t lp, int unicode) {
    switch (ctl) {
    case CTL_LISTVIEW: return listview_proc(st, w, msg, wp, lp, unicode);
    case CTL_TREEVIEW: return treeview_proc(st, w, msg, wp, lp, unicode);
    case CTL_TAB: return tab_proc(st, w, msg, wp, lp, unicode);
    case CTL_TOOLBAR: return toolbar_proc(st, w, msg, wp, lp, unicode);
    case CTL_UPDOWN: return updown_proc(st, w, msg, wp, lp, unicode);
    case CTL_HEADER: return header_proc(st, w, msg, wp, lp, unicode);
    case CTL_SYSLINK: return syslink_proc(st, w, msg, wp, lp, unicode);
    default: return win_def_proc(st, w, msg, wp, lp, unicode);
    }
}


// CreateToolbarEx / CreateUpDownControl / CreateStatusWindow helpers some older programs use.
HLE(cc_CreateUpDownControl) {
    // (style, x, y, cx, cy, parent, id, hInst, buddy, upper, lower, pos)
    char *cls = g_strdup("msctls_updown32");
    uint64_t h = win_create(st, 0, H2G(cls), 0, (uint32_t)ARG(0), ARGI(1), ARGI(2), ARGI(3), (int)(int32_t)hle_arg(st, 4), hle_arg(st, 5), hle_arg(st, 6), hle_arg(st, 7), 0, 0);
    g_free(cls);
    Win *w = win_get(h);
    if (w) {
        UdData *d = ud_data(w);
        d->hi = (int)(int32_t)hle_arg(st, 9); d->lo = (int)(int32_t)hle_arg(st, 10); d->pos = (int)(int32_t)hle_arg(st, 11);
        d->buddy = hle_arg(st, 8);
        ud_attach(st, w);
    }
    RET(h);
}
HLE(cc_CreateToolbarEx) {
    // (hwnd, ws, wID, nBitmaps, hBMInst, wBMID, lpButtons, iNumButtons, dxButton, dyButton, dxBitmap, dyBitmap, uStructSize)
    uint64_t parent = ARG(0), id = ARG(2);
    uint32_t style = (uint32_t)ARG(1);
    uint64_t hbi = hle_arg(st, 4), bmid = hle_arg(st, 5), buttons = hle_arg(st, 6);
    int nb = (int)(int32_t)hle_arg(st, 7), bw = (int)(int32_t)hle_arg(st, 10), bh = (int)(int32_t)hle_arg(st, 11), nbmp = ARGI(3);
    char *cls = g_strdup("ToolbarWindow32");
    uint64_t h = win_create(st, 0, H2G(cls), 0, style | WS_CHILD, 0, 0, 100, 30, parent, id, 0, 0, 0);
    g_free(cls);
    Win *w = win_get(h);
    if (w) {
        TbData *d = tb_data(w);
        if (bw > 0) d->bmp_w = bw;
        if (bh > 0) d->bmp_h = bh;
        if (nbmp > 0) tb_add_bitmap(w, nbmp, hbi, bmid);
        if (buttons && nb > 0) toolbar_proc(st, w, 0x414, (uint64_t)nb, buttons, 0);
        if ((w = win_get(h))) tb_autosize(w);
    }
    RET(h);
}
HLE(cc_GetEffectiveClientRect) { (void)st; RET(0); }
HLE(cc_DrawStatusTextW) {
    int32_t r[4]; memcpy(r, PTR(1), 16);
    char *s = ARG(2) ? w2u_dup((const uint16_t *)PTR(2)) : NULL;
    gdi_fill_rect_px(ARG(0), r[0], r[1], r[2], r[3], gui_sys_color(15));
    if (s) { int32_t rr[4] = { r[0] + 4, r[1], r[2], r[3] }; gdi_draw_text_dc(ARG(0), s, -1, rr, DT_LEFT_VC); }
    free(s);
    RET(0);
}
HLE(cc_DrawStatusTextA) {
    int32_t r[4]; memcpy(r, PTR(1), 16);
    gdi_fill_rect_px(ARG(0), r[0], r[1], r[2], r[3], gui_sys_color(15));
    if (ARG(2)) { int32_t rr[4] = { r[0] + 4, r[1], r[2], r[3] }; gdi_draw_text_dc(ARG(0), (const char *)PTR(2), -1, rr, DT_LEFT_VC); }
    RET(0);
}
// ---- window subclassing (SetWindowSubclass / DefSubclassProc) ----------------------------------------
// The window procedure is replaced by a Soul thunk that calls the newest SUBCLASSPROC
// (hwnd, msg, wParam, lParam, uIdSubclass, dwRefData); DefSubclassProc moves one level down and
// finally reaches the original procedure.
typedef struct Subclass { uint64_t hwnd, orig; int n, level, orig_unicode; uint64_t proc[8], id[8], ref[8]; } Subclass;
static Subclass g_sub[128];
static Subclass *sub_find(uint64_t hwnd) { for (int i = 0; i < 128; i++) if (g_sub[i].hwnd == hwnd && g_sub[i].n) return &g_sub[i]; return NULL; }
static uint64_t sub_call_level(X86State *st, Subclass *e, int level, uint32_t msg, uint64_t wp, uint64_t lp) {
    uint64_t hwnd = e->hwnd;
    if (level < 0) {
        Win *w = win_get(hwnd);
        return w ? win_call_proc(st, w, e->orig, msg, wp, lp, e->orig_unicode) : 0;
    }
    int saved = e->level;
    e->level = level;
    uint64_t a[6] = { hwnd, msg, wp, lp, e->id[level], e->ref[level] };
    uint64_t r = soul_guest_call(st, e->proc[level], 6, a);
    if ((e = sub_find(hwnd))) e->level = saved;
    return r;
}
static void sub_thunk(X86State *st) {
    Subclass *e = sub_find(ARG(0));
    if (!e) { RET(0); return; }
    RET(sub_call_level(st, e, e->n - 1, (uint32_t)ARG(1), ARG(2), ARG(3)));
}
static uint64_t sub_thunk_addr(void) {
    uint32_t id = hle_thunk_for(g_proc, sub_thunk, "comctl32", "SoulSubclassProc");
    g_proc->thunks[id].argbytes = g_mode32 ? 16 : 0;
    return g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE;
}
HLE(cc_SetWindowSubclass) {
    Win *w = win_get(ARG(0));
    if (!w || !ARG(1)) { RET(0); return; }
    Subclass *e = sub_find(w->hwnd);
    if (!e) {
        for (int i = 0; i < 128 && !e; i++) if (!g_sub[i].n) e = &g_sub[i];
        if (!e) { RET(0); return; }
        memset(e, 0, sizeof *e);
        e->hwnd = w->hwnd; e->orig = w->wndproc; e->orig_unicode = w->unicode; e->level = -1;
        w->wndproc = sub_thunk_addr();
        w->unicode = 1;
    }
    for (int k = 0; k < e->n; k++) if (e->proc[k] == ARG(1) && e->id[k] == ARG(2)) { e->ref[k] = ARG(3); RET(1); return; }
    if (e->n == 8) { RET(0); return; }
    e->proc[e->n] = ARG(1); e->id[e->n] = ARG(2); e->ref[e->n] = ARG(3);
    e->n++;
    RET(1);
}
HLE(cc_RemoveWindowSubclass) {
    Subclass *e = sub_find(ARG(0));
    if (!e) { RET(0); return; }
    for (int k = 0; k < e->n; k++)
        if (e->proc[k] == ARG(1) && e->id[k] == ARG(2)) {
            memmove(&e->proc[k], &e->proc[k + 1], sizeof(uint64_t) * (size_t)(e->n - k - 1));
            memmove(&e->id[k], &e->id[k + 1], sizeof(uint64_t) * (size_t)(e->n - k - 1));
            memmove(&e->ref[k], &e->ref[k + 1], sizeof(uint64_t) * (size_t)(e->n - k - 1));
            e->n--;
            if (!e->n) { Win *w = win_get(e->hwnd); if (w) { w->wndproc = e->orig; w->unicode = e->orig_unicode; } e->hwnd = 0; }
            RET(1);
            return;
        }
    RET(0);
}
HLE(cc_GetWindowSubclass) {
    Subclass *e = sub_find(ARG(0));
    if (e) for (int k = 0; k < e->n; k++) if (e->proc[k] == ARG(1) && e->id[k] == ARG(2)) { if (ARG(3)) wr_ptr(ARG(3), e->ref[k]); RET(1); return; }
    RET(0);
}
HLE(cc_DefSubclassProc) {
    Subclass *e = sub_find(ARG(0));
    if (e) { RET(sub_call_level(st, e, e->level - 1, (uint32_t)ARG(1), ARG(2), ARG(3))); return; }
    Win *w = win_get(ARG(0));
    if (!w) { RET(0); return; }
    RET(win_call_proc(st, w, w->wndproc, (uint32_t)ARG(1), ARG(2), ARG(3), w->unicode));
}
HLE_STUB_RET(cc_FlatSB_Stub, 1)
HLE(cc_LoadIconMetric) { if (hle_arg(st, 3)) wr_ptr(hle_arg(st, 3), 0x60000 + 0x600); RET(0); }
HLE(cc_LoadIconWithScaleDown) { if (hle_arg(st, 4)) wr_ptr(hle_arg(st, 4), 0x60000 + 0x600); RET(0); }

void comctl_reset(void) {
    for (int i = 0; i < IML_MAX; i++) free(g_iml[i].px);
    memset(g_iml, 0, sizeof g_iml);
    memset(g_sub, 0, sizeof g_sub);
}

#define C(n, f) { n, f, NULL }
const HleEntry hle_comctl_table[] = {
    C("ImageList_Create", cc_ImageList_Create), C("ImageList_Destroy", cc_ImageList_Destroy), C("ImageList_Add", cc_ImageList_Add),
    C("ImageList_AddMasked", cc_ImageList_AddMasked), C("ImageList_Replace", cc_ImageList_Replace), C("ImageList_ReplaceIcon", cc_ImageList_ReplaceIcon),
    C("ImageList_Remove", cc_ImageList_Remove), C("ImageList_GetImageCount", cc_ImageList_GetImageCount), C("ImageList_SetImageCount", cc_ImageList_SetImageCount),
    C("ImageList_GetIconSize", cc_ImageList_GetIconSize), C("ImageList_SetIconSize", cc_ImageList_SetIconSize), C("ImageList_SetBkColor", cc_ImageList_SetBkColor),
    C("ImageList_GetBkColor", cc_ImageList_GetBkColor), C("ImageList_Draw", cc_ImageList_Draw), C("ImageList_DrawEx", cc_ImageList_DrawEx),
    C("ImageList_GetIcon", cc_ImageList_GetIcon), C("ImageList_Duplicate", cc_ImageList_Duplicate), C("ImageList_LoadImageA", cc_ImageList_LoadImageA),
    C("ImageList_LoadImageW", cc_ImageList_LoadImageW), C("ImageList_LoadImage", cc_ImageList_LoadImageA), C("ImageList_BeginDrag", cc_ImageList_BeginDrag),
    C("ImageList_EndDrag", cc_ImageList_EndDrag), C("ImageList_DragEnter", cc_ImageList_DragEnter), C("ImageList_DragLeave", cc_ImageList_DragLeave),
    C("ImageList_DragMove", cc_ImageList_DragMove), C("ImageList_SetDragCursorImage", cc_ImageList_SetDragCursorImage),
    C("ImageList_SetOverlayImage", cc_ImageList_SetOverlayImage),
    C("CreateUpDownControl", cc_CreateUpDownControl), C("CreateToolbarEx", cc_CreateToolbarEx), C("GetEffectiveClientRect", cc_GetEffectiveClientRect),
    C("DrawStatusTextW", cc_DrawStatusTextW), C("DrawStatusTextA", cc_DrawStatusTextA), C("DrawStatusText", cc_DrawStatusTextA),
    C("DefSubclassProc", cc_DefSubclassProc), C("SetWindowSubclass", cc_SetWindowSubclass), C("RemoveWindowSubclass", cc_RemoveWindowSubclass),
    C("GetWindowSubclass", cc_GetWindowSubclass), C("InitializeFlatSB", cc_FlatSB_Stub), C("UninitializeFlatSB", cc_FlatSB_Stub),
    C("LoadIconMetric", cc_LoadIconMetric), C("LoadIconWithScaleDown", cc_LoadIconWithScaleDown),
    { NULL, NULL, NULL }
};
