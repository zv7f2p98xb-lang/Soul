// SoulCore - gdi32 emulation: device contexts, objects, drawing, text and bitmaps.
#include "gui.h"
#include "gdi_font_data.h"
#include <math.h>
#include <strings.h>

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))
#define ARGI(n) ((int32_t)hle_arg(st, (n)))

static GdiObj g_gdi[GDI_MAX];
static uint64_t g_stock[20];
static uint64_t g_stock_bitmap;

// ---- object table ------------------------------------------------------------------------------

uint64_t gdi_alloc(int type, GdiObj **out) {
    for (int i = 1; i < GDI_MAX; i++) {
        if (g_gdi[i].type == GO_FREE) {
            memset(&g_gdi[i], 0, sizeof g_gdi[i]);
            g_gdi[i].type = type;
            g_gdi[i].pid = g_proc ? g_proc->pid : 0;
            if (out) *out = &g_gdi[i];
            return GDI_HANDLE_BASE + (uint64_t)i * 4;
        }
    }
    if (out) *out = NULL;
    return 0;
}

GdiObj *gdi_get(uint64_t h, int type) {
    if (h < GDI_HANDLE_BASE) return NULL;
    uint64_t i = (h - GDI_HANDLE_BASE) / 4;
    if (i >= GDI_MAX || (h - GDI_HANDLE_BASE) % 4) return NULL;
    GdiObj *o = &g_gdi[i];
    if (o->type == GO_FREE || (type && o->type != type)) return NULL;
    return o;
}

void gdi_free(uint64_t h) {
    GdiObj *o = gdi_get(h, 0);
    if (!o || o->stock) return;
    if (o->type == GO_BITMAP && o->owns_bits == 1) free(o->surf.bits);
    if (o->type == GO_BITMAP && o->owns_bits == 2) g_free(o->surf.bits);
    if (o->type == GO_DC) free(o->dc);
    memset(o, 0, sizeof *o);
}

Dc *dc_get(uint64_t h) {
    GdiObj *o = gdi_get(h, GO_DC);
    return o ? o->dc : NULL;
}

static uint64_t make_brush(uint32_t color, int style, int stock) {
    GdiObj *o;
    uint64_t h = gdi_alloc(GO_BRUSH, &o);
    o->color = color; o->style = style; o->stock = stock;
    return h;
}
static uint64_t make_pen(uint32_t color, int style, int width, int stock) {
    GdiObj *o;
    uint64_t h = gdi_alloc(GO_PEN, &o);
    o->color = color; o->style = style; o->width = width < 1 ? 1 : width; o->stock = stock;
    return h;
}
static uint64_t make_font(int height, int weight, int italic, int underline, const char *face, int stock) {
    GdiObj *o;
    uint64_t h = gdi_alloc(GO_FONT, &o);
    o->height = height; o->weight = weight; o->italic = italic; o->underline = underline; o->stock = stock;
    snprintf(o->face, sizeof o->face, "%s", face ? face : "");
    return h;
}
static uint64_t make_bitmap(int w, int hgt, int bpp, int top_down, uint8_t *bits, int owns) {
    GdiObj *o;
    uint64_t h = gdi_alloc(GO_BITMAP, &o);
    if (!o) return 0;
    o->surf.w = w; o->surf.h = hgt; o->surf.bpp = bpp; o->surf.top_down = top_down;
    o->surf.stride = ((w * bpp + 31) / 32) * 4;
    if (!bits) { bits = calloc(1, (size_t)o->surf.stride * (size_t)(hgt ? hgt : 1)); owns = 1; }
    o->surf.bits = bits;
    o->owns_bits = owns;
    if (bpp == 1) { o->surf.palette[0] = 0xFF000000u; o->surf.palette[1] = 0xFFFFFFFFu; }
    return h;
}

uint64_t gdi_default_font(void) { return g_stock[17]; }
// A 32-bit top-down bitmap with the given 0xAARRGGBB pixels.
uint64_t gdi_bitmap_from_argb(const uint32_t *px, int w, int h) {
    uint64_t hb = make_bitmap(w, h, 32, 1, NULL, 0);
    GdiObj *o = gdi_get(hb, GO_BITMAP);
    if (!o) return 0;
    for (int y = 0; y < h; y++) memcpy(o->surf.bits + (size_t)y * o->surf.stride, px + (size_t)y * w, (size_t)w * 4);
    return hb;
}
uint64_t gdi_make_brush(uint32_t colorref) { return make_brush(colorref & 0xFFFFFF, 0, 0); }
uint64_t gdi_make_font(int height, int weight, const char *face) { return make_font(height, weight, 0, 0, face, 0); }

// DIB section bits live in guest memory: release them while the process (and its 4 GB window) exists.
void gdi_release_guest_bits(void) {
    for (int i = 0; i < GDI_MAX; i++)
        if (g_gdi[i].type == GO_BITMAP && g_gdi[i].owns_bits == 2) { g_gdi[i].surf.bits = NULL; g_gdi[i].owns_bits = 0; } // went away with the guest memory
}
void gdi_process_exit(uint32_t pid) {
    for (int i = 1; i < GDI_MAX; i++) {
        GdiObj *o = &g_gdi[i];
        if (o->type == GO_FREE || o->stock || o->pid != pid) continue;
        gdi_free(GDI_HANDLE_BASE + (uint64_t)i * 4);
    }
}
void gdi_reset_objects(void) {
    for (int i = 0; i < GDI_MAX; i++) {
        if (g_gdi[i].type == GO_BITMAP && g_gdi[i].owns_bits == 1) free(g_gdi[i].surf.bits);
        if (g_gdi[i].type == GO_DC) free(g_gdi[i].dc);
    }
    memset(g_gdi, 0, sizeof g_gdi);
    g_stock[0] = make_brush(0xFFFFFF, 0, 1);
    g_stock[1] = make_brush(0xC0C0C0, 0, 1);
    g_stock[2] = make_brush(0x808080, 0, 1);
    g_stock[3] = make_brush(0x404040, 0, 1);
    g_stock[4] = make_brush(0x000000, 0, 1);
    g_stock[5] = make_brush(0, 1 /* BS_NULL */, 1);
    g_stock[6] = make_pen(0xFFFFFF, 0, 1, 1);
    g_stock[7] = make_pen(0x000000, 0, 1, 1);
    g_stock[8] = make_pen(0, 5 /* PS_NULL */, 1, 1);
    g_stock[10] = make_font(-13, 400, 0, 0, "Courier", 1);
    g_stock[11] = make_font(-13, 400, 0, 0, "Courier", 1);
    g_stock[12] = make_font(-13, 400, 0, 0, "Tahoma", 1);
    g_stock[13] = make_font(-13, 700, 0, 0, "System", 1);
    g_stock[14] = make_font(-13, 400, 0, 0, "System", 1);
    { GdiObj *o; g_stock[15] = gdi_alloc(GO_PALETTE, &o); o->stock = 1; }
    g_stock[16] = make_font(-13, 400, 0, 0, "Fixedsys", 1);
    g_stock[17] = make_font(-12, 400, 0, 0, "Segoe UI", 1);
    g_stock[18] = make_brush(0xFFFFFF, 0, 1);
    g_stock[19] = make_pen(0x000000, 0, 1, 1);
    g_stock_bitmap = make_bitmap(1, 1, 1, 1, NULL, 1);
    gdi_get(g_stock_bitmap, GO_BITMAP)->stock = 1;
}

uint64_t gdi_create_dc(int kind, uint64_t hwnd) {
    GdiObj *o;
    uint64_t h = gdi_alloc(GO_DC, &o);
    if (!o) return 0;
    Dc *dc = calloc(1, sizeof *dc);
    dc->kind = kind;
    dc->hwnd = hwnd;
    dc->bitmap = kind == DC_MEMORY ? g_stock_bitmap : 0;
    dc->pen = g_stock[7];
    dc->brush = g_stock[0];
    dc->font = g_stock[13];
    Win *w = win_get(hwnd);
    if (w && w->font) dc->font = w->font;
    else if (kind == DC_WINDOW) dc->font = g_stock[17];
    dc->text_color = 0x000000;
    dc->bk_color = 0xFFFFFF;
    dc->bk_mode = 2;
    dc->rop2 = 13;
    o->dc = dc;
    return h;
}

void gdi_release_dc(uint64_t h) {
    Dc *dc = dc_get(h);
    if (!dc) return;
    for (int i = 0; i < dc->saved_count; i++) free(dc->saved[i]);
    gdi_free(h);
}

// ---- surfaces ----------------------------------------------------------------------------------

static inline uint8_t *surf_row(const Surface *s, int y) {
    int row = s->top_down ? y : s->h - 1 - y;
    return s->bits + (size_t)row * (size_t)s->stride;
}

uint32_t surf_get(const Surface *s, int x, int y) {
    uint8_t *r = surf_row(s, y);
    switch (s->bpp) {
    case 32: { uint32_t v; memcpy(&v, r + x * 4, 4); return v | 0xFF000000u; }
    case 24: return 0xFF000000u | ((uint32_t)r[x * 3 + 2] << 16) | ((uint32_t)r[x * 3 + 1] << 8) | r[x * 3];
    case 16: {
        uint16_t v; memcpy(&v, r + x * 2, 2);
        uint32_t R, G, B;
        if (s->bitfields565) { R = (v >> 11) & 31; G = (v >> 5) & 63; B = v & 31; G = (G << 2) | (G >> 4); }
        else { R = (v >> 10) & 31; G = (v >> 5) & 31; B = v & 31; G = (G << 3) | (G >> 2); }
        R = (R << 3) | (R >> 2); B = (B << 3) | (B >> 2);
        return 0xFF000000u | (R << 16) | (G << 8) | B;
    }
    case 8: return s->palette[r[x]] | 0xFF000000u;
    case 4: return s->palette[(x & 1) ? (r[x >> 1] & 15) : (r[x >> 1] >> 4)] | 0xFF000000u;
    case 1: return s->palette[(r[x >> 3] >> (7 - (x & 7))) & 1] | 0xFF000000u;
    default: return 0xFF000000u;
    }
}

static int nearest_index(const Surface *s, uint32_t px, int n) {
    int best = 0;
    long bd = 1L << 30;
    for (int i = 0; i < n; i++) {
        uint32_t c = s->palette[i];
        long dr = (long)((c >> 16) & 255) - (long)((px >> 16) & 255);
        long dg = (long)((c >> 8) & 255) - (long)((px >> 8) & 255);
        long db = (long)(c & 255) - (long)(px & 255);
        long d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void surf_put(Surface *s, int x, int y, uint32_t px) {
    uint8_t *r = surf_row(s, y);
    switch (s->bpp) {
    case 32: memcpy(r + x * 4, &px, 4); break;
    case 24: r[x * 3] = (uint8_t)px; r[x * 3 + 1] = (uint8_t)(px >> 8); r[x * 3 + 2] = (uint8_t)(px >> 16); break;
    case 16: {
        uint32_t R = (px >> 16) & 255, G = (px >> 8) & 255, B = px & 255;
        uint16_t v = s->bitfields565 ? (uint16_t)(((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3))
                                     : (uint16_t)(((R >> 3) << 10) | ((G >> 3) << 5) | (B >> 3));
        memcpy(r + x * 2, &v, 2);
        break;
    }
    case 8: r[x] = (uint8_t)nearest_index(s, px, 256); break;
    case 4: { int i = nearest_index(s, px, 16); if (x & 1) r[x >> 1] = (uint8_t)((r[x >> 1] & 0xF0) | i); else r[x >> 1] = (uint8_t)((r[x >> 1] & 0x0F) | (i << 4)); break; }
    case 1: {
        int bit = nearest_index(s, px, 2);
        if (bit) r[x >> 3] |= (uint8_t)(0x80 >> (x & 7)); else r[x >> 3] &= (uint8_t)~(0x80 >> (x & 7));
        break;
    }
    default: break;
    }
}

// Builds a surface over guest DIB bits described by a BITMAPINFO.
static int surface_from_bmi(Surface *s, void *bits, const uint8_t *bmi) {
    memset(s, 0, sizeof *s);
    uint32_t hsize; memcpy(&hsize, bmi, 4);
    int32_t w, h; uint16_t bpp; uint32_t comp, clr_used = 0;
    if (hsize == 12) { // BITMAPCOREHEADER
        uint16_t ww, hh; memcpy(&ww, bmi + 4, 2); memcpy(&hh, bmi + 6, 2); memcpy(&bpp, bmi + 10, 2);
        w = ww; h = hh; comp = 0;
    } else {
        memcpy(&w, bmi + 4, 4); memcpy(&h, bmi + 8, 4); memcpy(&bpp, bmi + 14, 2); memcpy(&comp, bmi + 16, 4);
        memcpy(&clr_used, bmi + 32, 4);
    }
    if (w <= 0 || h == 0 || !(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32)) return 0;
    if (comp != 0 && comp != 3) return 0; // BI_RGB / BI_BITFIELDS only
    s->bits = bits; s->w = w; s->h = h < 0 ? -h : h; s->top_down = h < 0; s->bpp = bpp;
    s->stride = ((w * bpp + 31) / 32) * 4;
    const uint8_t *pal = bmi + hsize;
    if (bpp == 16 && comp == 3) { uint32_t gmask; memcpy(&gmask, pal + 4, 4); s->bitfields565 = gmask == 0x07E0; }
    if (bpp <= 8) {
        int n = clr_used ? (int)clr_used : (1 << bpp);
        if (n > 256) n = 256;
        int entry = hsize == 12 ? 3 : 4;
        for (int i = 0; i < n; i++) s->palette[i] = 0xFF000000u | ((uint32_t)pal[i * entry + 2] << 16) | ((uint32_t)pal[i * entry + 1] << 8) | pal[i * entry];
    }
    return 1;
}

// ---- DC -> surface mapping -----------------------------------------------------------------------

typedef struct Target {
    Surface s;
    int ox, oy;                  // logical (0,0) in surface coordinates
    int x0, y0, x1, y1;          // clip in surface coordinates
    Win *top;
} Target;

static int dc_target(Dc *dc, Target *t) {
    memset(t, 0, sizeof *t);
    if (dc->kind == DC_WINDOW) {
        Win *w = win_get(dc->hwnd);
        if (!w) return 0;
        Win *top = win_top(w);
        if (!top || !top->pixels) return 0;
        t->s.bits = (uint8_t *)top->pixels; t->s.w = top->fb_w; t->s.h = top->fb_h; t->s.bpp = 32; t->s.top_down = 1;
        t->s.stride = top->fb_w * 4;
        int ox, oy;
        win_client_origin(w, &ox, &oy);
        t->ox = ox + dc->vp_x; t->oy = oy + dc->vp_y;
        if (!win_visible_rect(w, &t->x0, &t->y0, &t->x1, &t->y1)) return 0;
        t->top = top;
    } else if (dc->kind == DC_MEMORY) {
        GdiObj *b = gdi_get(dc->bitmap, GO_BITMAP);
        if (!b || !b->surf.bits) return 0;
        t->s = b->surf;
        t->ox = dc->vp_x; t->oy = dc->vp_y;
        t->x0 = 0; t->y0 = 0; t->x1 = b->surf.w; t->y1 = b->surf.h;
    } else {
        return 0;
    }
    if (t->x0 < 0) t->x0 = 0;
    if (t->y0 < 0) t->y0 = 0;
    if (t->x1 > t->s.w) t->x1 = t->s.w;
    if (t->y1 > t->s.h) t->y1 = t->s.h;
    if (dc->clip_set) {
        int cx0 = dc->cx0 + t->ox, cy0 = dc->cy0 + t->oy, cx1 = dc->cx1 + t->ox, cy1 = dc->cy1 + t->oy;
        if (cx0 > t->x0) t->x0 = cx0;
        if (cy0 > t->y0) t->y0 = cy0;
        if (cx1 < t->x1) t->x1 = cx1;
        if (cy1 < t->y1) t->y1 = cy1;
    }
    return t->x0 < t->x1 && t->y0 < t->y1;
}

static void target_done(Target *t) { if (t->top) win_touch(t->top); }

static inline uint32_t rop_apply(int rop2, uint32_t dst, uint32_t src) {
    switch (rop2) {
    case 1: return 0xFF000000u;                       // R2_BLACK
    case 16: return 0xFFFFFFFFu;                      // R2_WHITE
    case 6: return ~dst | 0xFF000000u;                // R2_NOT
    case 7: return (dst ^ src) | 0xFF000000u;         // R2_XORPEN
    case 9: return ~(dst ^ src) | 0xFF000000u;        // R2_NOTXORPEN
    case 11: return dst;                              // R2_NOP
    case 12: return (~src) | 0xFF000000u;             // R2_NOTCOPYPEN
    case 15: return dst | src;                        // R2_MERGEPEN
    case 9 + 100: return dst;
    case 14: return (dst & src) | 0xFF000000u;        // R2_MASKPEN
    default: return src;                              // R2_COPYPEN
    }
}

static void put_px(Target *t, int rop2, int x, int y, uint32_t px) {
    if (x < t->x0 || y < t->y0 || x >= t->x1 || y >= t->y1) return;
    if (rop2 == 13 && t->s.bpp == 32) { uint32_t *p = (uint32_t *)(void *)(t->s.bits + (size_t)y * t->s.stride) + x; *p = px; return; }
    if (rop2 == 13) { surf_put(&t->s, x, y, px); return; }
    surf_put(&t->s, x, y, rop_apply(rop2, surf_get(&t->s, x, y), px));
}

static void fill_span(Target *t, int rop2, int y, int xa, int xb, uint32_t px) { // [xa, xb)
    if (y < t->y0 || y >= t->y1) return;
    if (xa < t->x0) xa = t->x0;
    if (xb > t->x1) xb = t->x1;
    if (xa >= xb) return;
    if (rop2 == 13 && t->s.bpp == 32 && t->s.top_down) {
        uint32_t *p = (uint32_t *)(void *)(t->s.bits + (size_t)y * t->s.stride);
        for (int x = xa; x < xb; x++) p[x] = px;
        return;
    }
    for (int x = xa; x < xb; x++) put_px(t, rop2, x, y, px);
}

static void fill_box(Target *t, int rop2, int x0, int y0, int x1, int y1, uint32_t px) {
    if (y0 < t->y0) y0 = t->y0;
    if (y1 > t->y1) y1 = t->y1;
    for (int y = y0; y < y1; y++) fill_span(t, rop2, y, x0, x1, px);
}

static uint32_t brush_px(Dc *dc, uint64_t hbrush, int *null_out) {
    *null_out = 0;
    if (hbrush > 0 && hbrush < 0x40) return rgb_px(gui_sys_color((int)hbrush - 1)); // (HBRUSH)(COLOR_x + 1)
    GdiObj *b = gdi_get(hbrush, GO_BRUSH);
    if (!b || b->style == 1) { *null_out = 1; return 0; }
    if (hbrush == g_stock[18]) return rgb_px(b->color);
    (void)dc;
    return rgb_px(b->color);
}

static void pen_info(Dc *dc, uint32_t *px, int *width, int *null_out) {
    GdiObj *p = gdi_get(dc->pen, GO_PEN);
    *null_out = !p || p->style == 5;
    *px = p ? rgb_px(p->color) : 0xFF000000u;
    *width = p ? p->width : 1;
}

static void line_px(Target *t, int rop2, int x0, int y0, int x1, int y1, uint32_t px, int width, int skip_last) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int half = (width - 1) / 2;
    for (;;) {
        if (skip_last && x0 == x1 && y0 == y1) break;
        if (width <= 1) put_px(t, rop2, x0, y0, px);
        else fill_box(t, rop2, x0 - half, y0 - half, x0 - half + width, y0 - half + width, px);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ---- exported drawing helpers (also used by controls) -------------------------------------------

void gdi_fill_rect_px(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref) {
    Dc *dc = dc_get(hdc);
    Target t;
    if (!dc || !dc_target(dc, &t)) return;
    fill_box(&t, 13, x0 + t.ox, y0 + t.oy, x1 + t.ox, y1 + t.oy, rgb_px(colorref));
    target_done(&t);
}
void gdi_frame_rect_px(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref) {
    gdi_fill_rect_px(hdc, x0, y0, x1, y0 + 1, colorref);
    gdi_fill_rect_px(hdc, x0, y1 - 1, x1, y1, colorref);
    gdi_fill_rect_px(hdc, x0, y0, x0 + 1, y1, colorref);
    gdi_fill_rect_px(hdc, x1 - 1, y0, x1, y1, colorref);
}

// ---- fonts -------------------------------------------------------------------------------------

typedef struct FontSel { const SoulFontFace *face; float scale; int underline; } FontSel;

static int is_mono_face(const char *f) {
    static const char *mono[] = { "courier", "consol", "mono", "fixed", "terminal", "lucida console", "system fixed", NULL };
    for (int i = 0; mono[i]; i++) if (strcasestr(f, mono[i])) return 1;
    return 0;
}

static FontSel font_select(uint64_t hfont) {
    GdiObj *f = gdi_get(hfont, GO_FONT);
    int height = f ? f->height : -13, weight = f ? f->weight : 400;
    int kind = (f && is_mono_face(f->face)) ? 2 : (weight >= 600 ? 1 : 0);
    float em;
    if (height < 0) em = (float)-height;
    else if (height > 0) em = (float)height * 0.86f;
    else em = 13.0f;
    if (em < 4) em = 4;
    const SoulFontFace *best = NULL;
    float bd = 1e9f;
    for (int i = 0; i < SOUL_FONT_FACE_COUNT; i++) {
        const SoulFontFace *ff = &soul_font_faces[i];
        if (ff->bold != kind) continue;
        float d = fabsf(logf(em / (float)ff->size));
        if (d < bd) { bd = d; best = ff; }
    }
    FontSel fs = { best, em / (float)best->size, f ? f->underline : 0 };
    if (fabsf(fs.scale - 1.0f) < 0.08f) fs.scale = 1.0f;
    return fs;
}

static const SoulGlyph *glyph_for(const SoulFontFace *f, uint32_t cp) {
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp) return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    if (cp != '?') return glyph_for(f, '?');
    return NULL;
}

static uint32_t utf8_next(const char **ps, const char *end) {
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = *s++;
    if (c >= 0xF0 && s + 2 < (const unsigned char *)end + 1) { c = ((c & 7) << 18) | ((uint32_t)(s[0] & 63) << 12) | ((uint32_t)(s[1] & 63) << 6) | (s[2] & 63); s += 3; }
    else if (c >= 0xE0 && s + 1 < (const unsigned char *)end + 1) { c = ((c & 15) << 12) | ((uint32_t)(s[0] & 63) << 6) | (s[1] & 63); s += 2; }
    else if (c >= 0xC0 && s < (const unsigned char *)end + 1) { c = ((c & 31) << 6) | (s[0] & 63); s += 1; }
    *ps = (const char *)s;
    return c;
}

int gdi_text_width(uint64_t font, const char *s, int len, int *height) {
    FontSel fs = font_select(font);
    float x = 0;
    const char *end = s + len;
    while (s < end) {
        uint32_t cp = utf8_next(&s, end);
        if (cp == '\t') { x += 4 * fs.face->glyphs[0].adv * fs.scale; continue; }
        const SoulGlyph *g = glyph_for(fs.face, cp);
        if (g) x += g->adv * fs.scale;
    }
    if (height) *height = (int)lroundf((fs.face->ascent + fs.face->descent) * fs.scale);
    return (int)lroundf(x);
}

void gdi_font_metrics(uint64_t font, int *ascent, int *descent, int *avg_width) {
    FontSel fs = font_select(font);
    if (ascent) *ascent = (int)lroundf(fs.face->ascent * fs.scale);
    if (descent) *descent = (int)lroundf(fs.face->descent * fs.scale);
    if (avg_width) { const SoulGlyph *g = glyph_for(fs.face, 'x'); *avg_width = g ? (int)lroundf(g->adv * fs.scale) : 7; }
}

// Draws UTF-8 text with its top-left corner at (x, y) in surface coordinates.
static int draw_run(Target *t, FontSel fs, const char *s, int len, int x, int y, uint32_t color) {
    float pen = (float)x;
    const char *end = s + len;
    uint32_t fg = rgb_px(color);
    uint32_t fr = (fg >> 16) & 255, fgc = (fg >> 8) & 255, fb = fg & 255;
    float sc = fs.scale;
    while (s < end) {
        uint32_t cp = utf8_next(&s, end);
        if (cp == '\t') { pen += 4 * fs.face->glyphs[0].adv * sc; continue; }
        const SoulGlyph *g = glyph_for(fs.face, cp);
        if (!g) continue;
        int gx0 = (int)lroundf(pen + g->x0 * sc), gy0 = y + (int)lroundf(g->y0 * sc);
        int gw = (int)ceilf(g->w * sc), gh = (int)ceilf(g->h * sc);
        const uint8_t *bits = soul_font_bits + g->off;
        for (int yy = 0; yy < gh; yy++) {
            int py = gy0 + yy;
            if (py < t->y0 || py >= t->y1) continue;
            float fy = sc == 1.0f ? (float)yy : (yy + 0.5f) / sc - 0.5f;
            for (int xx = 0; xx < gw; xx++) {
                int pxx = gx0 + xx;
                if (pxx < t->x0 || pxx >= t->x1) continue;
                uint32_t a;
                if (sc == 1.0f) {
                    int idx = yy * g->w + xx;
                    a = ((idx & 1) ? (bits[idx >> 1] >> 4) : (bits[idx >> 1] & 15)) * 17u;
                } else { // bilinear sampling of the 4-bit coverage
                    float fx = (xx + 0.5f) / sc - 0.5f;
                    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
                    float ax = fx - (float)x0, ay = fy - (float)y0;
                    float acc = 0;
                    for (int k = 0; k < 4; k++) {
                        int sx = x0 + (k & 1), sy = y0 + (k >> 1);
                        if (sx < 0 || sy < 0 || sx >= g->w || sy >= g->h) continue;
                        int idx = sy * g->w + sx;
                        float v = (float)((idx & 1) ? (bits[idx >> 1] >> 4) : (bits[idx >> 1] & 15));
                        acc += v * ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
                    }
                    a = (uint32_t)(acc * 17.0f + 0.5f);
                    if (a > 255) a = 255;
                }
                if (!a) continue;
                if (a >= 250) { put_px(t, 13, pxx, py, fg); continue; }
                uint32_t d = surf_get(&t->s, pxx, py);
                uint32_t r = (fr * a + ((d >> 16) & 255) * (255 - a)) / 255;
                uint32_t gg = (fgc * a + ((d >> 8) & 255) * (255 - a)) / 255;
                uint32_t b = (fb * a + (d & 255) * (255 - a)) / 255;
                put_px(t, 13, pxx, py, 0xFF000000u | (r << 16) | (gg << 8) | b);
            }
        }
        pen += g->adv * sc;
    }
    int w = (int)lroundf(pen) - x;
    if (fs.underline) fill_box(t, 13, x, y + (int)lroundf(fs.face->ascent * sc) + 1, x + w, y + (int)lroundf(fs.face->ascent * sc) + 2, fg);
    return w;
}

// TextOut semantics on a DC (x, y logical).
static void dc_text_out(Dc *dc, int x, int y, const char *s, int len, const int *opaque_rect) {
    Target t;
    if (!dc_target(dc, &t)) return;
    FontSel fs = font_select(dc->font);
    int lh = (int)lroundf((fs.face->ascent + fs.face->descent) * fs.scale);
    int asc = (int)lroundf(fs.face->ascent * fs.scale);
    int w = gdi_text_width(dc->font, s, len, NULL);
    if ((dc->text_align & 1) == 0 && (dc->text_align & 6) == 0 && 0) {}
    int ta = dc->text_align;
    if ((ta & 6) == 6) x -= w / 2;          // TA_CENTER
    else if (ta & 2) x -= w;                // TA_RIGHT
    if ((ta & 24) == 24) y -= asc;          // TA_BASELINE
    else if (ta & 8) y -= lh;               // TA_BOTTOM
    if (ta & 1) { /* TA_UPDATECP */ x = dc->cur_x; y = dc->cur_y; dc->cur_x += w; }
    int sx = x + t.ox, sy = y + t.oy;
    if (opaque_rect) fill_box(&t, 13, opaque_rect[0] + t.ox, opaque_rect[1] + t.oy, opaque_rect[2] + t.ox, opaque_rect[3] + t.oy, rgb_px(dc->bk_color));
    else if (dc->bk_mode == 2) fill_box(&t, 13, sx, sy, sx + w, sy + lh, rgb_px(dc->bk_color));
    draw_run(&t, fs, s, len, sx, sy, dc->text_color);
    target_done(&t);
}

// DrawText with wrapping/alignment. Coordinates are logical. Returns the text height.
static int draw_text_impl(Dc *dc, const char *text, int len, int *rect, uint32_t fmt) {
    char *buf = malloc((size_t)len + 1);
    int n = 0, underline_at = -1;
    for (int i = 0; i < len; i++) {
        char c = text[i];
        if (c == '&' && !(fmt & 0x800)) {
            if (i + 1 < len && text[i + 1] == '&') { buf[n++] = '&'; i++; continue; }
            underline_at = n;
            continue;
        }
        buf[n++] = c;
    }
    buf[n] = 0;
    (void)underline_at;
    int lh;
    gdi_text_width(dc->font, "Ay", 2, &lh);
    int width = rect[2] - rect[0];
    // split lines
    typedef struct { int start, len; } Line;
    Line lines[512];
    int nl = 0;
    int single = (fmt & 0x20) != 0;
    int i = 0;
    while (i <= n && nl < 512) {
        int start = i, last_space = -1;
        int j = i;
        while (j < n && (single || buf[j] != '\n')) {
            if (buf[j] == ' ') last_space = j;
            if ((fmt & 0x10) && !single && width > 0 && gdi_text_width(dc->font, buf + start, j - start + 1, NULL) > width && j > start) {
                if (last_space > start) { j = last_space; break; }
                break;
            }
            j++;
        }
        int l = j - start;
        if (l > 0 && buf[start + l - 1] == '\r') l--;
        lines[nl].start = start; lines[nl].len = l; nl++;
        if (j >= n) break;
        i = (buf[j] == '\n' || buf[j] == ' ') ? j + 1 : j;
    }
    int total_h = nl * lh, max_w = 0;
    for (int k = 0; k < nl; k++) { int w = gdi_text_width(dc->font, buf + lines[k].start, lines[k].len, NULL); if (w > max_w) max_w = w; }
    if (fmt & 0x400) { // DT_CALCRECT
        rect[2] = rect[0] + (single || !(fmt & 0x10) ? max_w : (max_w > width ? max_w : width));
        if (!(fmt & 0x10)) rect[2] = rect[0] + max_w;
        rect[3] = rect[1] + total_h;
        free(buf);
        return total_h;
    }
    int y = rect[1];
    if (single) {
        if (fmt & 4) y = rect[1] + (rect[3] - rect[1] - lh) / 2;
        else if (fmt & 8) y = rect[3] - lh;
    }
    Dc saved = *dc;
    if (!(fmt & 0x100)) { dc->clip_set = 1; dc->cx0 = rect[0]; dc->cy0 = rect[1]; dc->cx1 = rect[2]; dc->cy1 = rect[3];
        if (saved.clip_set) { if (saved.cx0 > dc->cx0) dc->cx0 = saved.cx0; if (saved.cy0 > dc->cy0) dc->cy0 = saved.cy0; if (saved.cx1 < dc->cx1) dc->cx1 = saved.cx1; if (saved.cy1 < dc->cy1) dc->cy1 = saved.cy1; } }
    int ta = dc->text_align;
    dc->text_align = 0;
    for (int k = 0; k < nl; k++) {
        int w = gdi_text_width(dc->font, buf + lines[k].start, lines[k].len, NULL);
        int x = rect[0];
        if (fmt & 1) x = rect[0] + (width - w) / 2;
        else if (fmt & 2) x = rect[2] - w;
        dc_text_out(dc, x, y, buf + lines[k].start, lines[k].len, NULL);
        y += lh;
    }
    dc->text_align = ta;
    dc->clip_set = saved.clip_set; dc->cx0 = saved.cx0; dc->cy0 = saved.cy0; dc->cx1 = saved.cx1; dc->cy1 = saved.cy1;
    free(buf);
    return total_h;
}

void gdi_draw_text_utf8(uint64_t hdc, const char *s, int x0, int y0, int x1, int y1, uint32_t format) {
    Dc *dc = dc_get(hdc);
    if (!dc) return;
    int r[4] = { x0, y0, x1, y1 };
    draw_text_impl(dc, s, (int)strlen(s), r, format);
}

// ---- shapes ------------------------------------------------------------------------------------

static void draw_ellipse(Dc *dc, int l, int tp, int r, int b) {
    Target t;
    if (!dc_target(dc, &t)) return;
    int bnull, pnull, pw;
    uint32_t bpx = brush_px(dc, dc->brush, &bnull), ppx;
    pen_info(dc, &ppx, &pw, &pnull);
    double cx = (l + r - 1) / 2.0, cy = (tp + b - 1) / 2.0, rx = (r - l) / 2.0, ry = (b - tp) / 2.0;
    if (rx <= 0 || ry <= 0) { target_done(&t); return; }
    for (int y = tp; y < b; y++) {
        double dy = (y + 0.5 - (tp + b) / 2.0) / ry;
        if (dy * dy > 1) continue;
        double half = rx * sqrt(1 - dy * dy);
        int xa = (int)lround(cx + 0.5 - half), xb = (int)lround(cx + 0.5 + half);
        if (!bnull) fill_span(&t, dc->rop2, y + t.oy, xa + t.ox, xb + t.ox, bpx);
        if (!pnull) {
            for (int k = 0; k < pw; k++) { put_px(&t, dc->rop2, xa + k + t.ox, y + t.oy, ppx); put_px(&t, dc->rop2, xb - 1 - k + t.ox, y + t.oy, ppx); }
        }
    }
    if (!pnull) { // top/bottom outline
        for (int x = l; x < r; x++) {
            double dx = (x + 0.5 - (l + r) / 2.0) / rx;
            if (dx * dx > 1) continue;
            double half = ry * sqrt(1 - dx * dx);
            int ya = (int)lround(cy + 0.5 - half), yb = (int)lround(cy + 0.5 + half);
            for (int k = 0; k < pw; k++) { put_px(&t, dc->rop2, x + t.ox, ya + k + t.oy, ppx); put_px(&t, dc->rop2, x + t.ox, yb - 1 - k + t.oy, ppx); }
        }
    }
    target_done(&t);
}

static void draw_rect(Dc *dc, int l, int tp, int r, int b) {
    Target t;
    if (!dc_target(dc, &t)) return;
    int bnull, pnull, pw;
    uint32_t bpx = brush_px(dc, dc->brush, &bnull), ppx;
    pen_info(dc, &ppx, &pw, &pnull);
    if (!bnull) fill_box(&t, dc->rop2, l + t.ox + (pnull ? 0 : pw), tp + t.oy + (pnull ? 0 : pw), r + t.ox - (pnull ? 0 : pw), b + t.oy - (pnull ? 0 : pw), bpx);
    if (!pnull) {
        fill_box(&t, dc->rop2, l + t.ox, tp + t.oy, r + t.ox, tp + t.oy + pw, ppx);
        fill_box(&t, dc->rop2, l + t.ox, b + t.oy - pw, r + t.ox, b + t.oy, ppx);
        fill_box(&t, dc->rop2, l + t.ox, tp + t.oy + pw, l + t.ox + pw, b + t.oy - pw, ppx);
        fill_box(&t, dc->rop2, r + t.ox - pw, tp + t.oy + pw, r + t.ox, b + t.oy - pw, ppx);
    }
    target_done(&t);
}

static void draw_polygon(Dc *dc, const int32_t *pts, int n, int fill) {
    Target t;
    if (n < 2 || !dc_target(dc, &t)) return;
    int bnull, pnull, pw;
    uint32_t bpx = brush_px(dc, dc->brush, &bnull), ppx;
    pen_info(dc, &ppx, &pw, &pnull);
    if (fill && !bnull && n >= 3) {
        int ymin = pts[1], ymax = pts[1];
        for (int i = 1; i < n; i++) { if (pts[2 * i + 1] < ymin) ymin = pts[2 * i + 1]; if (pts[2 * i + 1] > ymax) ymax = pts[2 * i + 1]; }
        double xs[256];
        for (int y = ymin; y <= ymax; y++) {
            int k = 0;
            double sy = y + 0.5;
            for (int i = 0; i < n && k < 256; i++) {
                int j = (i + 1) % n;
                double y0 = pts[2 * i + 1], y1 = pts[2 * j + 1], x0 = pts[2 * i], x1 = pts[2 * j];
                if ((y0 <= sy && y1 > sy) || (y1 <= sy && y0 > sy)) xs[k++] = x0 + (sy - y0) * (x1 - x0) / (y1 - y0);
            }
            for (int a = 1; a < k; a++) for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) { double tmp = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = tmp; }
            for (int a = 0; a + 1 < k; a += 2) fill_span(&t, dc->rop2, y + t.oy, (int)lround(xs[a]) + t.ox, (int)lround(xs[a + 1]) + t.ox, bpx);
        }
    }
    if (!pnull) {
        int segs = fill ? n : n - 1;
        for (int i = 0; i < segs; i++) {
            int j = (i + 1) % n;
            line_px(&t, dc->rop2, pts[2 * i] + t.ox, pts[2 * i + 1] + t.oy, pts[2 * j] + t.ox, pts[2 * j + 1] + t.oy, ppx, pw, 0);
        }
    }
    target_done(&t);
}

// ---- blits -------------------------------------------------------------------------------------

#define SRCCOPY 0x00CC0020u
#define SRCPAINT 0x00EE0086u
#define SRCAND 0x008800C6u
#define SRCINVERT 0x00660046u
#define NOTSRCCOPY 0x00330008u
#define BLACKNESS 0x00000042u
#define WHITENESS 0x00FF0062u
#define PATCOPY 0x00F00021u
#define PATINVERT 0x005A0049u
#define DSTINVERT 0x00550009u
#define MERGECOPY 0x00C000CAu

static uint32_t combine(uint32_t rop, uint32_t d, uint32_t s, uint32_t p) {
    switch (rop) {
    case SRCCOPY: return s;
    case SRCPAINT: return d | s;
    case SRCAND: return (d & s) | 0xFF000000u;
    case SRCINVERT: return (d ^ s) | 0xFF000000u;
    case NOTSRCCOPY: return ~s | 0xFF000000u;
    case BLACKNESS: return 0xFF000000u;
    case WHITENESS: return 0xFFFFFFFFu;
    case PATCOPY: return p;
    case PATINVERT: return (d ^ p) | 0xFF000000u;
    case DSTINVERT: return ~d | 0xFF000000u;
    case MERGECOPY: return (s & p) | 0xFF000000u;
    default: return s;
    }
}

// Generic stretch blit from a surface into a DC (logical destination rectangle).
static void blit(Dc *dst, int dx, int dy, int dw, int dh, const Surface *src, int sx, int sy, int sw, int sh, uint32_t rop,
                 uint32_t mono_fg, uint32_t mono_bg, int src_top_down_override) {
    Target t;
    if (!dc_target(dst, &t) || dw == 0 || dh == 0) return;
    (void)src_top_down_override;
    int pnull;
    uint32_t pat = brush_px(dst, dst->brush, &pnull);
    int mirror_x = 0, mirror_y = 0;
    if (dw < 0) { dw = -dw; dx -= dw - 1; mirror_x = 1; }
    if (dh < 0) { dh = -dh; dy -= dh - 1; mirror_y = 1; }
    if (sw < 0) { sw = -sw; sx -= sw - 1; mirror_x ^= 1; }
    if (sh < 0) { sh = -sh; sy -= sh - 1; mirror_y ^= 1; }
    int need_src = !(rop == BLACKNESS || rop == WHITENESS || rop == PATCOPY || rop == PATINVERT || rop == DSTINVERT);
    int mono_to_color = src && src->bpp == 1 && t.s.bpp != 1;
    int color_to_mono = src && src->bpp != 1 && t.s.bpp == 1;
    int fast = src && rop == SRCCOPY && src->bpp == 32 && t.s.bpp == 32 && t.s.top_down && dw == sw && dh == sh && !mirror_x && !mirror_y;
    for (int y = 0; y < dh; y++) {
        int ty = dy + y + t.oy;
        if (ty < t.y0 || ty >= t.y1) continue;
        int syy = sy + (int)((long)(mirror_y ? dh - 1 - y : y) * sh / dh);
        if (need_src && src && (syy < 0 || syy >= src->h)) continue;
        if (fast) {
            int xa = dx + t.ox, xb = dx + dw + t.ox, sxa = sx;
            if (xa < t.x0) { sxa += t.x0 - xa; xa = t.x0; }
            if (xb > t.x1) xb = t.x1;
            if (sxa < 0) { xa -= sxa; sxa = 0; }
            if (xb - xa > src->w - sxa) xb = xa + src->w - sxa;
            if (xa < xb) {
                const uint8_t *sr = surf_row(src, syy) + (size_t)sxa * 4;
                uint32_t *dr = (uint32_t *)(void *)(t.s.bits + (size_t)ty * t.s.stride) + xa;
                for (int k = 0; k < xb - xa; k++) { uint32_t v; memcpy(&v, sr + 4 * k, 4); dr[k] = v | 0xFF000000u; }
            }
            continue;
        }
        for (int x = 0; x < dw; x++) {
            int tx = dx + x + t.ox;
            if (tx < t.x0 || tx >= t.x1) continue;
            uint32_t s = 0;
            if (need_src && src) {
                int sxx = sx + (int)((long)(mirror_x ? dw - 1 - x : x) * sw / dw);
                if (sxx < 0 || sxx >= src->w) continue;
                s = surf_get(src, sxx, syy);
                if (mono_to_color) s = ((s & 0xFFFFFF) == 0) ? rgb_px(mono_fg) : rgb_px(mono_bg);
                if (color_to_mono) s = ((s & 0xFFFFFF) == (rgb_px(mono_bg) & 0xFFFFFF)) ? 0xFFFFFFFFu : 0xFF000000u;
            }
            uint32_t d = (rop == SRCCOPY || rop == PATCOPY || rop == BLACKNESS || rop == WHITENESS || rop == NOTSRCCOPY) ? 0 : surf_get(&t.s, tx, ty);
            surf_put(&t.s, tx, ty, combine(rop, d, s, pat));
        }
    }
    target_done(&t);
}

static Surface *dc_source(uint64_t hdc, Surface *tmp) {
    Dc *dc = dc_get(hdc);
    if (!dc) return NULL;
    Target t;
    if (!dc_target(dc, &t)) return NULL;
    *tmp = t.s;
    if (dc->kind == DC_WINDOW) { // window DCs: shift to the window's client origin
        tmp->bits += (size_t)(t.oy - dc->vp_y) * (size_t)t.s.stride + (size_t)(t.ox - dc->vp_x) * 4;
        Win *w = win_get(dc->hwnd);
        tmp->w = w ? w->w : 0;
        tmp->h = w ? w->h : 0;
    }
    return tmp;
}

// ---- gdi32 exports -------------------------------------------------------------------------------

HLE(g_GetStockObject) { int i = ARGI(0); RET(i >= 0 && i < 20 ? g_stock[i] : 0); }
HLE(g_SelectObject) {
    Dc *dc = dc_get(ARG(0));
    GdiObj *o = gdi_get(ARG(1), 0);
    if (!dc || !o) { RET(0); return; }
    uint64_t old = 0;
    switch (o->type) {
    case GO_BRUSH: old = dc->brush; dc->brush = ARG(1); break;
    case GO_PEN: old = dc->pen; dc->pen = ARG(1); break;
    case GO_FONT: old = dc->font; dc->font = ARG(1); break;
    case GO_BITMAP:
        if (dc->kind != DC_MEMORY) { RET(0); return; }
        old = dc->bitmap; dc->bitmap = ARG(1);
        { GdiObj *ob = gdi_get(old, GO_BITMAP); if (ob) ob->selected = 0; }
        o->selected = 1;
        break;
    case GO_REGION: dc->clip_set = 1; dc->cx0 = o->rx0; dc->cy0 = o->ry0; dc->cx1 = o->rx1; dc->cy1 = o->ry1; RET(3); return; // COMPLEXREGION
    case GO_PALETTE: old = dc->palette; dc->palette = ARG(1); break;
    default: break;
    }
    RET(old);
}
HLE(g_DeleteObject) {
    GdiObj *o = gdi_get(ARG(0), 0);
    if (!o) { RET(0); return; }
    if (o->type == GO_BITMAP && o->selected) { RET(0); return; }
    gdi_free(ARG(0));
    RET(1);
}
HLE(g_GetObjectType) {
    GdiObj *o = gdi_get(ARG(0), 0);
    static const int map[] = { 0, 2, 1, 6, 7, 3, 8, 5 }; // OBJ_BRUSH=2 OBJ_PEN=1 OBJ_FONT=6 OBJ_BITMAP=7 OBJ_DC=3 OBJ_REGION=8 OBJ_PAL=5
    RET(o ? map[o->type] : 0);
}
static void get_object(X86State *st, int wide) {
    GdiObj *o = gdi_get(ARG(0), 0);
    int cb = ARGI(1);
    uint8_t *out = PTR(2);
    if (!o) { RET(0); return; }
    if (o->type == GO_BITMAP) {
        // BITMAP { bmType, bmWidth, bmHeight, bmWidthBytes, bmPlanes, bmBitsPixel, bmBits }: 32 / 24 bytes;
        // DIBSECTION adds BITMAPINFOHEADER, dsBitfields, dshSection, dsOffset: 104 / 84 bytes
        const int bms = g_mode32 ? 24 : 32, dss = g_mode32 ? 84 : 104;
        int dib = o->owns_bits == 2;
        if (!out) { RET(dib ? dss : bms); return; }
        uint8_t bm[32] = { 0 };
        int32_t v[4] = { 0, o->surf.w, o->surf.h, o->surf.stride };
        memcpy(bm, v, 16);
        uint16_t planes = 1, bpp = (uint16_t)o->surf.bpp;
        memcpy(bm + 16, &planes, 2); memcpy(bm + 18, &bpp, 2);
        uint64_t bits = dib ? H2G(o->surf.bits) : 0; // only DIB sections expose their pixels
        memcpy(bm + (g_mode32 ? 20 : 24), &bits, (size_t)PSZ);
        memcpy(out, bm, (size_t)(cb < bms ? cb : bms));
        if (cb >= dss) {
            uint8_t bih[40] = { 0 };
            uint32_t sz = 40; int32_t w = o->surf.w, h = o->surf.top_down ? -o->surf.h : o->surf.h;
            memcpy(bih, &sz, 4); memcpy(bih + 4, &w, 4); memcpy(bih + 8, &h, 4); memcpy(bih + 12, &planes, 2); memcpy(bih + 14, &bpp, 2);
            uint32_t image = (uint32_t)(o->surf.stride * o->surf.h);
            memcpy(bih + 20, &image, 4);
            memcpy(out + bms, bih, 40);
            memset(out + bms + 40, 0, (size_t)(dss - bms - 40));
            RET(dss);
            return;
        }
        RET(cb < bms ? cb : bms);
    } else if (o->type == GO_FONT) {
        size_t sz = wide ? 92 : 60;
        if (!out) { RET(sz); return; }
        uint8_t lf[92] = { 0 };
        int32_t v[5] = { o->height, 0, 0, 0, o->weight };
        memcpy(lf, v, 20);
        lf[20] = (uint8_t)o->italic; lf[21] = (uint8_t)o->underline;
        if (wide) u2w(o->face, -1, (uint16_t *)(void *)(lf + 28), 32); else snprintf((char *)lf + 28, 32, "%s", o->face);
        memcpy(out, lf, (size_t)cb < sz ? (size_t)cb : sz);
        RET((size_t)cb < sz ? (size_t)cb : sz);
    } else if (o->type == GO_BRUSH) {
        if (!out) { RET(16); return; }
        uint32_t lb[4] = { (uint32_t)o->style, o->color, 0, 0 };
        memcpy(out, lb, (size_t)(cb < 16 ? cb : 16));
        RET(cb < 16 ? cb : 16);
    } else if (o->type == GO_PEN) {
        if (!out) { RET(16); return; }
        uint32_t lp[4] = { (uint32_t)o->style, (uint32_t)o->width, 0, o->color };
        memcpy(out, lp, (size_t)(cb < 16 ? cb : 16));
        RET(cb < 16 ? cb : 16);
    } else {
        RET(0);
    }
}
HLE(g_GetObjectA) { get_object(st, 0); }
HLE(g_GetObjectW) { get_object(st, 1); }
HLE(g_GetCurrentObject) {
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    switch (ARGI(1)) {
    case 1: RET(dc->pen); break;
    case 2: RET(dc->brush); break;
    case 6: RET(dc->font); break;
    case 7: RET(dc->bitmap); break;
    default: RET(0); break;
    }
}
HLE(g_CreateSolidBrush) { RET(make_brush((uint32_t)ARG(0) & 0xFFFFFF, 0, 0)); }
HLE(g_CreateHatchBrush) { RET(make_brush((uint32_t)ARG(1) & 0xFFFFFF, 0, 0)); }
HLE(g_CreatePatternBrush) { RET(make_brush(0x808080, 0, 0)); }
HLE(g_CreateBrushIndirect) { uint32_t lb[2]; memcpy(lb, PTR(0), 8); RET(make_brush(lb[1] & 0xFFFFFF, lb[0] == 1 ? 1 : 0, 0)); }
HLE(g_CreatePen) { RET(make_pen((uint32_t)ARG(2) & 0xFFFFFF, ARGI(0) & 0xF, ARGI(1), 0)); }
HLE(g_CreatePenIndirect) { uint32_t lp[4]; memcpy(lp, PTR(0), 16); RET(make_pen(lp[3] & 0xFFFFFF, (int)(lp[0] & 0xF), (int)lp[1], 0)); }
HLE(g_ExtCreatePen) { uint32_t lb[2] = { 0, 0 }; if (ARG(2)) memcpy(lb, PTR(2), 8); RET(make_pen(lb[1] & 0xFFFFFF, ARGI(0) & 0xF, ARGI(1), 0)); }
HLE(g_SetDCBrushColor) { GdiObj *b = gdi_get(g_stock[18], GO_BRUSH); uint32_t old = b->color; b->color = (uint32_t)ARG(1) & 0xFFFFFF; RET(old); }
HLE(g_SetDCPenColor) { GdiObj *p = gdi_get(g_stock[19], GO_PEN); uint32_t old = p->color; p->color = (uint32_t)ARG(1) & 0xFFFFFF; RET(old); }

static uint64_t create_font(X86State *st, int wide) {
    char face[64] = "";
    uint64_t fp = hle_arg(st, 13);
    if (fp) { if (wide) w2u((const uint16_t *)G2H(fp), -1, face, sizeof face); else snprintf(face, sizeof face, "%s", (const char *)G2H(fp)); }
    if ((hle_arg(st, 12) & 3) == 1 && !face[0]) snprintf(face, sizeof face, "Courier");
    return make_font(ARGI(0), ARGI(4), (int)(hle_arg(st, 5) & 0xFF), (int)(hle_arg(st, 6) & 0xFF), face, 0);
}
HLE(g_CreateFontA) { RET(create_font(st, 0)); }
HLE(g_CreateFontW) { RET(create_font(st, 1)); }
static uint64_t create_font_indirect(const uint8_t *lf, int wide) {
    int32_t v[5];
    memcpy(v, lf, 20);
    char face[64];
    if (wide) w2u((const uint16_t *)(const void *)(lf + 28), 32, face, sizeof face);
    else { memcpy(face, lf + 28, 32); face[32] = 0; }
    if ((lf[27] & 3) == 1 && !face[0]) snprintf(face, sizeof face, "Courier");
    return make_font(v[0], v[4], lf[20], lf[21], face, 0);
}
HLE(g_CreateFontIndirectA) { RET(create_font_indirect(PTR(0), 0)); }
HLE(g_CreateFontIndirectW) { RET(create_font_indirect(PTR(0), 1)); }
HLE(g_CreateFontIndirectExW) { RET(create_font_indirect(PTR(0), 1)); }

HLE(g_CreateCompatibleDC) { RET(gdi_create_dc(DC_MEMORY, 0)); }
HLE(g_CreateDCA) { RET(gdi_create_dc(DC_SCREEN, 0)); }
HLE(g_DeleteDC) { gdi_release_dc(ARG(0)); RET(1); }
HLE(g_CreateCompatibleBitmap) {
    int w = ARGI(1), h = ARGI(2);
    if (w <= 0 || h <= 0 || (int64_t)w * h > (64 << 20)) { RET(0); return; }
    RET(make_bitmap(w, h, 32, 1, NULL, 1));
}
HLE(g_CreateBitmap) {
    int w = ARGI(0), h = ARGI(1), bpp = ARGI(3);
    if (w <= 0 || h <= 0) { RET(0); return; }
    int out_bpp = bpp == 1 ? 1 : 32;
    uint64_t hb = make_bitmap(w, h, out_bpp, 1, NULL, 1);
    GdiObj *o = gdi_get(hb, GO_BITMAP);
    if (ARG(4)) {
        const uint8_t *src = PTR(4);
        int sstride = ((w * bpp + 15) / 16) * 2; // CreateBitmap rows are WORD aligned
        Surface s = { 0 };
        s.bits = (uint8_t *)src; s.w = w; s.h = h; s.bpp = bpp; s.top_down = 1; s.stride = sstride;
        s.palette[0] = 0xFF000000u; s.palette[1] = 0xFFFFFFFFu;
        if (bpp == 1 || bpp == 32 || bpp == 24 || bpp == 16)
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) surf_put(&o->surf, x, y, surf_get(&s, x, y));
    }
    RET(hb);
}
HLE(g_CreateBitmapIndirect) {
    int32_t v[4]; memcpy(v, PTR(0), 16);
    uint16_t bpp; memcpy(&bpp, (uint8_t *)PTR(0) + 18, 2);
    RET(make_bitmap(v[1], v[2], bpp == 1 ? 1 : 32, 1, NULL, 1));
}
HLE(g_CreateDIBSection) {
    Surface s;
    uint8_t *bmi = PTR(1);
    if (!surface_from_bmi(&s, NULL, bmi)) { RET(0); return; }
    size_t size = (size_t)s.stride * (size_t)s.h;
    uint8_t *bits = g_calloc(1, size + 64);
    GdiObj *o;
    uint64_t h = gdi_alloc(GO_BITMAP, &o);
    s.bits = bits;
    o->surf = s;
    o->owns_bits = 2;
    if (ARG(3)) wr_ptr(ARG(3), H2G(bits));
    RET(h);
}
HLE(g_CreateDIBitmap) {
    uint8_t *bih = PTR(1);
    int32_t w, h; memcpy(&w, bih + 4, 4); memcpy(&h, bih + 8, 4);
    if (h < 0) h = -h;
    uint64_t hb = make_bitmap(w, h, 32, 1, NULL, 1);
    if (ARG(2) & 4 /* CBM_INIT */) {
        Surface s;
        if (surface_from_bmi(&s, PTR(3), PTR(4))) {
            GdiObj *o = gdi_get(hb, GO_BITMAP);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) surf_put(&o->surf, x, y, surf_get(&s, x, y));
        }
    }
    RET(hb);
}
HLE(g_GetDIBits) {
    GdiObj *o = gdi_get(ARG(1), GO_BITMAP);
    uint8_t *bmi = PTR(5);
    if (!o) { RET(0); return; }
    if (!ARG(4)) { // fill header only
        int32_t w = o->surf.w, h = o->surf.h;
        uint16_t planes = 1, bpp = 32;
        memcpy(bmi + 4, &w, 4); memcpy(bmi + 8, &h, 4); memcpy(bmi + 12, &planes, 2); memcpy(bmi + 14, &bpp, 2);
        uint32_t zero = 0, image = (uint32_t)(w * h * 4);
        memcpy(bmi + 16, &zero, 4); memcpy(bmi + 20, &image, 4);
        RET(1);
        return;
    }
    Surface d;
    if (!surface_from_bmi(&d, PTR(4), bmi)) { RET(0); return; }
    int start = ARGI(2), lines = ARGI(3);
    int n = 0;
    for (int y = start; y < start + lines && y < d.h && y < o->surf.h; y++, n++) {
        int sy = d.top_down ? y : o->surf.h - 1 - y; // scanline numbering is bottom-up for bottom-up DIBs
        int dy = d.top_down ? y : d.h - 1 - y;
        for (int x = 0; x < d.w && x < o->surf.w; x++) surf_put(&d, x, dy, surf_get(&o->surf, x, sy));
    }
    RET(n);
}
HLE(g_SetDIBits) {
    GdiObj *o = gdi_get(ARG(1), GO_BITMAP);
    Surface s;
    if (!o || !surface_from_bmi(&s, PTR(4), PTR(5))) { RET(0); return; }
    int start = ARGI(2), lines = ARGI(3), n = 0;
    for (int y = start; y < start + lines && y < s.h && y < o->surf.h; y++, n++) {
        int sy = s.top_down ? y : s.h - 1 - y, dy = o->surf.top_down ? y : o->surf.h - 1 - y;
        (void)sy;
        for (int x = 0; x < s.w && x < o->surf.w; x++) surf_put(&o->surf, x, s.top_down ? y : o->surf.h - 1 - y, surf_get(&s, x, s.top_down ? y : s.h - 1 - y));
        (void)dy;
    }
    RET(n);
}
HLE(g_StretchDIBits) {
    // (hdc, xDest, yDest, wDest, hDest, xSrc, ySrc, wSrc, hSrc, bits, bmi, usage, rop)
    Dc *dc = dc_get(ARG(0));
    Surface s;
    if (!dc || !surface_from_bmi(&s, G2H(hle_arg(st, 9)), (const uint8_t *)G2H(hle_arg(st, 10)))) { RET(0); return; }
    int ysrc = ARGI(6), hsrc = ARGI(8);
    if (!s.top_down) ysrc = s.h - ysrc - hsrc; // source rectangle is bottom-up for bottom-up DIBs
    blit(dc, ARGI(1), ARGI(2), ARGI(3), ARGI(4), &s, ARGI(5), ysrc, ARGI(7), hsrc, (uint32_t)hle_arg(st, 12), dc->text_color, dc->bk_color, 0);
    RET(hsrc);
}
HLE(g_SetDIBitsToDevice) {
    // (hdc, xDest, yDest, w, h, xSrc, ySrc, uStartScan, cScanLines, bits, bmi, usage)
    Dc *dc = dc_get(ARG(0));
    Surface s;
    if (!dc || !surface_from_bmi(&s, G2H(hle_arg(st, 9)), (const uint8_t *)G2H(hle_arg(st, 10)))) { RET(0); return; }
    int w = ARGI(3), h = ARGI(4), ysrc = ARGI(6);
    if (!s.top_down) ysrc = s.h - ysrc - h;
    blit(dc, ARGI(1), ARGI(2), w, h, &s, ARGI(5), ysrc, w, h, SRCCOPY, 0, 0xFFFFFF, 0);
    RET(h);
}
HLE(g_BitBlt) {
    // (hdcDest, x, y, w, h, hdcSrc, xSrc, ySrc, rop)
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    Surface tmp, *src = hle_arg(st, 5) ? dc_source(hle_arg(st, 5), &tmp) : NULL;
    Dc *sdc = dc_get(hle_arg(st, 5));
    int sx = ARGI(6), sy = ARGI(7);
    if (sdc) { sx += sdc->vp_x; sy += sdc->vp_y; }
    blit(dc, ARGI(1), ARGI(2), ARGI(3), ARGI(4), src, sx, sy, ARGI(3), ARGI(4), (uint32_t)hle_arg(st, 8), dc->text_color, dc->bk_color, 0);
    RET(1);
}
HLE(g_StretchBlt) {
    // (hdcDest, x, y, w, h, hdcSrc, xSrc, ySrc, wSrc, hSrc, rop)
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    Surface tmp, *src = dc_source(hle_arg(st, 5), &tmp);
    Dc *sdc = dc_get(hle_arg(st, 5));
    int sx = ARGI(6), sy = ARGI(7);
    if (sdc) { sx += sdc->vp_x; sy += sdc->vp_y; }
    blit(dc, ARGI(1), ARGI(2), ARGI(3), ARGI(4), src, sx, sy, ARGI(8), ARGI(9), (uint32_t)hle_arg(st, 10), dc->text_color, dc->bk_color, 0);
    RET(1);
}
HLE(g_PatBlt) {
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    blit(dc, ARGI(1), ARGI(2), ARGI(3), ARGI(4), NULL, 0, 0, ARGI(3), ARGI(4), (uint32_t)ARG(5) == PATCOPY ? PATCOPY : (uint32_t)hle_arg(st, 5), 0, 0, 0);
    RET(1);
}
HLE(g_TransparentBlt) {
    // (hdcDest, x, y, w, h, hdcSrc, xs, ys, ws, hs, crTransparent)
    Dc *dc = dc_get(ARG(0));
    Surface tmp, *src = dc_source(hle_arg(st, 5), &tmp);
    Target t;
    if (!dc || !src || !dc_target(dc, &t)) { RET(0); return; }
    int dx = ARGI(1), dy = ARGI(2), dw = ARGI(3), dh = ARGI(4), sx = ARGI(6), sy = ARGI(7), sw = ARGI(8), sh = ARGI(9);
    uint32_t key = rgb_px((uint32_t)hle_arg(st, 10)) & 0xFFFFFF;
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        int sxx = sx + (int)((long)x * sw / (dw ? dw : 1)), syy = sy + (int)((long)y * sh / (dh ? dh : 1));
        if (sxx < 0 || syy < 0 || sxx >= src->w || syy >= src->h) continue;
        uint32_t p = surf_get(src, sxx, syy);
        if ((p & 0xFFFFFF) != key) put_px(&t, 13, dx + x + t.ox, dy + y + t.oy, p);
    }
    target_done(&t);
    RET(1);
}
HLE(g_AlphaBlend) {
    // (hdcDest, x, y, w, h, hdcSrc, xs, ys, ws, hs, BLENDFUNCTION)
    Dc *dc = dc_get(ARG(0));
    Surface tmp, *src = dc_source(hle_arg(st, 5), &tmp);
    Target t;
    if (!dc || !src || !dc_target(dc, &t)) { RET(0); return; }
    uint32_t bf = (uint32_t)hle_arg(st, 10);
    uint32_t ca = (bf >> 16) & 255;
    int per_pixel = ((bf >> 24) & 1) && src->bpp == 32;
    int dx = ARGI(1), dy = ARGI(2), dw = ARGI(3), dh = ARGI(4), sx = ARGI(6), sy = ARGI(7), sw = ARGI(8), sh = ARGI(9);
    for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
        int sxx = sx + (int)((long)x * sw / (dw ? dw : 1)), syy = sy + (int)((long)y * sh / (dh ? dh : 1));
        int tx = dx + x + t.ox, ty = dy + y + t.oy;
        if (sxx < 0 || syy < 0 || sxx >= src->w || syy >= src->h || tx < t.x0 || ty < t.y0 || tx >= t.x1 || ty >= t.y1) continue;
        uint32_t s;
        if (per_pixel) { memcpy(&s, surf_row(src, syy) + sxx * 4, 4); } else s = surf_get(src, sxx, syy);
        uint32_t d = surf_get(&t.s, tx, ty);
        uint32_t sa = per_pixel ? ((s >> 24) * ca) / 255 : ca;
        uint32_t out = 0xFF000000u;
        for (int sh2 = 0; sh2 < 24; sh2 += 8) {
            uint32_t sc = (s >> sh2) & 255, dc2 = (d >> sh2) & 255;
            uint32_t v = per_pixel ? (sc * ca / 255 + dc2 * (255 - sa) / 255) : (sc * sa + dc2 * (255 - sa)) / 255;
            if (v > 255) v = 255;
            out |= v << sh2;
        }
        surf_put(&t.s, tx, ty, out);
    }
    target_done(&t);
    RET(1);
}

HLE(g_SetPixel) {
    Dc *dc = dc_get(ARG(0));
    Target t;
    if (!dc || !dc_target(dc, &t)) { RET((uint64_t)-1); return; }
    put_px(&t, dc->rop2, ARGI(1) + t.ox, ARGI(2) + t.oy, rgb_px((uint32_t)ARG(3)));
    target_done(&t);
    RET((uint32_t)ARG(3) & 0xFFFFFF);
}
HLE(g_GetPixel) {
    Dc *dc = dc_get(ARG(0));
    Target t;
    if (!dc || !dc_target(dc, &t)) { RET(0xFFFFFFFF); return; }
    int x = ARGI(1) + t.ox, y = ARGI(2) + t.oy;
    if (x < t.x0 || y < t.y0 || x >= t.x1 || y >= t.y1) { RET(0xFFFFFFFF); return; }
    RET(px_rgb(surf_get(&t.s, x, y)));
}
HLE(g_MoveToEx) {
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    if (ARG(3)) { mem_wr(ARG(3), 4, (uint32_t)dc->cur_x); mem_wr(ARG(3) + 4, 4, (uint32_t)dc->cur_y); }
    dc->cur_x = ARGI(1); dc->cur_y = ARGI(2);
    RET(1);
}
HLE(g_GetCurrentPositionEx) { Dc *dc = dc_get(ARG(0)); if (dc && ARG(1)) { mem_wr(ARG(1), 4, (uint32_t)dc->cur_x); mem_wr(ARG(1) + 4, 4, (uint32_t)dc->cur_y); } RET(dc != NULL); }
HLE(g_LineTo) {
    Dc *dc = dc_get(ARG(0));
    Target t;
    if (!dc) { RET(0); return; }
    int pnull, pw; uint32_t ppx;
    pen_info(dc, &ppx, &pw, &pnull);
    if (!pnull && dc_target(dc, &t)) {
        line_px(&t, dc->rop2, dc->cur_x + t.ox, dc->cur_y + t.oy, ARGI(1) + t.ox, ARGI(2) + t.oy, ppx, pw, pw <= 1);
        target_done(&t);
    }
    dc->cur_x = ARGI(1); dc->cur_y = ARGI(2);
    RET(1);
}
static int32_t *read_points(uint64_t p, int n) {
    int32_t *pts = malloc(sizeof(int32_t) * 2 * (size_t)(n > 0 ? n : 1));
    memcpy(pts, G2H(p), sizeof(int32_t) * 2 * (size_t)n);
    return pts;
}
HLE(g_Polyline) { Dc *dc = dc_get(ARG(0)); int n = ARGI(2); if (!dc || n < 2) { RET(0); return; } int32_t *p = read_points(ARG(1), n); draw_polygon(dc, p, n, 0); free(p); RET(1); }
HLE(g_PolylineTo) {
    Dc *dc = dc_get(ARG(0)); int n = ARGI(2);
    if (!dc || n < 1) { RET(0); return; }
    int32_t *p = malloc(sizeof(int32_t) * 2 * (size_t)(n + 1));
    p[0] = dc->cur_x; p[1] = dc->cur_y;
    memcpy(p + 2, PTR(1), sizeof(int32_t) * 2 * (size_t)n);
    draw_polygon(dc, p, n + 1, 0);
    dc->cur_x = p[2 * n]; dc->cur_y = p[2 * n + 1];
    free(p);
    RET(1);
}
HLE(g_Polygon) { Dc *dc = dc_get(ARG(0)); int n = ARGI(2); if (!dc || n < 2) { RET(0); return; } int32_t *p = read_points(ARG(1), n); draw_polygon(dc, p, n, 1); free(p); RET(1); }
HLE(g_Rectangle) { Dc *dc = dc_get(ARG(0)); if (dc) draw_rect(dc, ARGI(1), ARGI(2), ARGI(3), ARGI(4)); RET(dc != NULL); }
// Polygon approximation of an elliptic arc from the start radial to the end radial (counter-clockwise).
static int arc_points(int l, int tp, int r, int b, int xs, int ys, int xe, int ye, int32_t *pts, int max) {
    double cx = (l + r) / 2.0, cy = (tp + b) / 2.0, rx = (r - l) / 2.0, ry = (b - tp) / 2.0;
    double a0 = atan2(-(ys - cy), xs - cx), a1 = atan2(-(ye - cy), xe - cx);
    if (a1 <= a0) a1 += 2 * M_PI;
    int n = (int)((a1 - a0) / (2 * M_PI) * 96) + 2;
    if (n > max) n = max;
    for (int i = 0; i < n; i++) {
        double a = a0 + (a1 - a0) * i / (n - 1);
        pts[2 * i] = (int32_t)lround(cx + rx * cos(a));
        pts[2 * i + 1] = (int32_t)lround(cy - ry * sin(a));
    }
    return n;
}
static void draw_round_rect(Dc *dc, int l, int tp, int r, int b, int ew, int eh) {
    if (ew < 2 || eh < 2) { draw_rect(dc, l, tp, r, b); return; }
    if (ew > r - l) ew = r - l;
    if (eh > b - tp) eh = b - tp;
    int32_t pts[2 * 4 * 26];
    int n = 0;
    double rx = ew / 2.0, ry = eh / 2.0;
    const double cxs[4] = { r - rx, l + rx, l + rx, r - rx }, cys[4] = { tp + ry, tp + ry, b - ry, b - ry };
    for (int q = 0; q < 4; q++)
        for (int i = 0; i <= 24; i += 3) {
            double a = (q * 90 + i * 90.0 / 24) * M_PI / 180;
            pts[2 * n] = (int32_t)lround(cxs[q] + rx * cos(a) - (q == 0 || q == 3 ? 1 : 0));
            pts[2 * n + 1] = (int32_t)lround(cys[q] - ry * sin(a) - (q >= 2 ? 1 : 0));
            n++;
        }
    draw_polygon(dc, pts, n, 1);
}
HLE(g_RoundRect) { Dc *dc = dc_get(ARG(0)); if (dc) draw_round_rect(dc, ARGI(1), ARGI(2), ARGI(3), ARGI(4), (int)(int32_t)hle_arg(st, 5), (int)(int32_t)hle_arg(st, 6)); RET(dc != NULL); }
HLE(g_Ellipse) { Dc *dc = dc_get(ARG(0)); if (dc) draw_ellipse(dc, ARGI(1), ARGI(2), ARGI(3), ARGI(4)); RET(dc != NULL); }
static void arc_shape(X86State *st, int kind) { // 0 arc, 1 pie, 2 chord
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    int32_t pts[2 * 100];
    int n = arc_points(ARGI(1), ARGI(2), ARGI(3), ARGI(4), (int)(int32_t)hle_arg(st, 5), (int)(int32_t)hle_arg(st, 6),
                       (int)(int32_t)hle_arg(st, 7), (int)(int32_t)hle_arg(st, 8), pts, 98);
    if (kind == 1) { pts[2 * n] = (ARGI(1) + ARGI(3)) / 2; pts[2 * n + 1] = (ARGI(2) + ARGI(4)) / 2; n++; }
    draw_polygon(dc, pts, n, kind != 0);
    RET(1);
}
HLE(g_Pie) { arc_shape(st, 1); }
HLE(g_Chord) { arc_shape(st, 2); }
HLE(g_Arc) { arc_shape(st, 0); }

static void text_out(X86State *st, int wide, int ext) {
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    int x = ARGI(1), y = ARGI(2);
    uint64_t sp; int n; const int32_t *rc = NULL; uint32_t opts = 0;
    if (ext) { opts = (uint32_t)ARG(3); rc = (const int32_t *)G2H(hle_arg(st, 4)); sp = hle_arg(st, 5); n = (int)(int32_t)hle_arg(st, 6); }
    else { sp = ARG(3); n = (int)(int32_t)hle_arg(st, 4); }
    char *s;
    if (wide) { s = malloc((size_t)(n > 0 ? n : 0) * 3 + 4); w2u((const uint16_t *)G2H(sp), n, s, (size_t)(n > 0 ? n : 0) * 3 + 4); }
    else { s = malloc((size_t)(n > 0 ? n : 0) + 1); if (n > 0) memcpy(s, G2H(sp), (size_t)n); s[n > 0 ? n : 0] = 0; }
    int r[4];
    int use_rect = rc && (opts & 2); // ETO_OPAQUE
    if (use_rect) { r[0] = rc[0]; r[1] = rc[1]; r[2] = rc[2]; r[3] = rc[3]; }
    if (sp || !use_rect) dc_text_out(dc, x, y, s, (int)strlen(s), use_rect ? r : NULL);
    else {
        Target t;
        if (dc_target(dc, &t)) { fill_box(&t, 13, r[0] + t.ox, r[1] + t.oy, r[2] + t.ox, r[3] + t.oy, rgb_px(dc->bk_color)); target_done(&t); }
    }
    free(s);
    RET(1);
}
HLE(g_TextOutA) { text_out(st, 0, 0); }
HLE(g_TextOutW) { text_out(st, 1, 0); }
HLE(g_ExtTextOutA) { text_out(st, 0, 1); }
HLE(g_ExtTextOutW) { text_out(st, 1, 1); }
static void text_extent(X86State *st, int wide) {
    Dc *dc = dc_get(ARG(0));
    int n = ARGI(2);
    char *s;
    if (wide) { s = malloc((size_t)(n > 0 ? n : 0) * 3 + 4); w2u(WSTR(1), n, s, (size_t)(n > 0 ? n : 0) * 3 + 4); }
    else { s = malloc((size_t)(n > 0 ? n : 0) + 1); if (n > 0) memcpy(s, CSTR(1), (size_t)n); s[n > 0 ? n : 0] = 0; }
    int h;
    int w = gdi_text_width(dc ? dc->font : g_stock[17], s, (int)strlen(s), &h);
    mem_wr(ARG(3), 4, (uint32_t)w);
    mem_wr(ARG(3) + 4, 4, (uint32_t)h);
    free(s);
    RET(1);
}
HLE(g_GetTextExtentPoint32A) { text_extent(st, 0); }
HLE(g_GetTextExtentPoint32W) { text_extent(st, 1); }
static void text_metrics(X86State *st, int wide) {
    Dc *dc = dc_get(ARG(0));
    int asc, desc, avg;
    gdi_font_metrics(dc ? dc->font : g_stock[17], &asc, &desc, &avg);
    GdiObj *f = gdi_get(dc ? dc->font : 0, GO_FONT);
    uint8_t *tm = PTR(1);
    memset(tm, 0, wide ? 60 : 56);
    int32_t v[11] = { asc + desc, asc, desc, 0, 0, avg, avg * 2, f ? f->weight : 400, 0, 96, 96 };
    memcpy(tm, v, 44);
    if (wide) { uint16_t ch[4] = { 32, 0xFFFC, '?', ' ' }; memcpy(tm + 44, ch, 8); tm[52] = (uint8_t)(f ? f->italic : 0); tm[53] = (uint8_t)(f ? f->underline : 0); tm[55] = (f && is_mono_face(f->face)) ? 0x30 : 0x26; }
    else { tm[44] = 32; tm[45] = 255; tm[46] = '?'; tm[47] = ' '; tm[48] = (uint8_t)(f ? f->italic : 0); tm[49] = (uint8_t)(f ? f->underline : 0); tm[51] = (f && is_mono_face(f->face)) ? 0x30 : 0x26; }
    RET(1);
}
HLE(g_GetTextMetricsA) { text_metrics(st, 0); }
HLE(g_GetTextMetricsW) { text_metrics(st, 1); }
static void text_face(X86State *st, int wide) {
    Dc *dc = dc_get(ARG(0));
    GdiObj *f = gdi_get(dc ? dc->font : 0, GO_FONT);
    const char *face = f && f->face[0] ? f->face : "Segoe UI";
    if (!ARG(2)) { RET(strlen(face) + 1); return; }
    if (wide) u2w(face, -1, PTR(2), (size_t)ARGI(1)); else snprintf(PTR(2), (size_t)ARGI(1), "%s", face);
    RET(strlen(face));
}
HLE(g_GetTextFaceA) { text_face(st, 0); }
HLE(g_GetTextFaceW) { text_face(st, 1); }
HLE(g_GetCharWidth32) {
    Dc *dc = dc_get(ARG(0));
    for (uint32_t c = (uint32_t)ARG(1); c <= (uint32_t)ARG(2); c++) {
        char u[8]; uint16_t w2[2] = { (uint16_t)c, 0 };
        size_t n = w2u(w2, 1, u, sizeof u);
        mem_wr(ARG(3) + 4ull * (c - (uint32_t)ARG(1)), 4, (uint32_t)gdi_text_width(dc ? dc->font : g_stock[17], u, (int)n, NULL));
    }
    RET(1);
}

HLE(g_SetTextColor) { Dc *dc = dc_get(ARG(0)); if (!dc) { RET(0xFFFFFFFF); return; } uint32_t o = dc->text_color; dc->text_color = (uint32_t)ARG(1) & 0xFFFFFF; RET(o); }
HLE(g_GetTextColor) { Dc *dc = dc_get(ARG(0)); RET(dc ? dc->text_color : 0); }
HLE(g_SetBkColor) { Dc *dc = dc_get(ARG(0)); if (!dc) { RET(0xFFFFFFFF); return; } uint32_t o = dc->bk_color; dc->bk_color = (uint32_t)ARG(1) & 0xFFFFFF; RET(o); }
HLE(g_GetBkColor) { Dc *dc = dc_get(ARG(0)); RET(dc ? dc->bk_color : 0xFFFFFF); }
HLE(g_SetBkMode) { Dc *dc = dc_get(ARG(0)); if (!dc) { RET(0); return; } int o = dc->bk_mode; dc->bk_mode = ARGI(1); RET(o); }
HLE(g_GetBkMode) { Dc *dc = dc_get(ARG(0)); RET(dc ? dc->bk_mode : 0); }
HLE(g_SetTextAlign) { Dc *dc = dc_get(ARG(0)); if (!dc) { RET(0xFFFFFFFF); return; } int o = dc->text_align; dc->text_align = ARGI(1); RET(o); }
HLE(g_GetTextAlign) { Dc *dc = dc_get(ARG(0)); RET(dc ? dc->text_align : 0); }
HLE(g_SetROP2) { Dc *dc = dc_get(ARG(0)); if (!dc) { RET(0); return; } int o = dc->rop2; dc->rop2 = ARGI(1); RET(o); }
HLE(g_GetROP2) { Dc *dc = dc_get(ARG(0)); RET(dc ? dc->rop2 : 13); }
HLE_STUB_RET(g_SetStretchBltMode, 1)
HLE_STUB_RET(g_GetStretchBltMode, 1)
HLE(g_SetBrushOrgEx) { if (ARG(3)) { mem_wr(ARG(3), 4, 0); mem_wr(ARG(3) + 4, 4, 0); } RET(1); }
HLE_STUB_RET(g_SetMapMode, 1)
HLE_STUB_RET(g_GetMapMode, 1)
HLE(g_SetViewportOrgEx) {
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    if (ARG(3)) { mem_wr(ARG(3), 4, (uint32_t)dc->vp_x); mem_wr(ARG(3) + 4, 4, (uint32_t)dc->vp_y); }
    dc->vp_x = ARGI(1); dc->vp_y = ARGI(2);
    RET(1);
}
HLE(g_OffsetViewportOrgEx) {
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    if (ARG(3)) { mem_wr(ARG(3), 4, (uint32_t)dc->vp_x); mem_wr(ARG(3) + 4, 4, (uint32_t)dc->vp_y); }
    dc->vp_x += ARGI(1); dc->vp_y += ARGI(2);
    RET(1);
}
HLE(g_GetViewportOrgEx) { Dc *dc = dc_get(ARG(0)); if (dc && ARG(1)) { mem_wr(ARG(1), 4, (uint32_t)dc->vp_x); mem_wr(ARG(1) + 4, 4, (uint32_t)dc->vp_y); } RET(dc != NULL); }
HLE(g_SetWindowOrgEx) {
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    if (ARG(3)) { mem_wr(ARG(3), 4, (uint32_t)-dc->vp_x); mem_wr(ARG(3) + 4, 4, (uint32_t)-dc->vp_y); }
    dc->vp_x = -ARGI(1); dc->vp_y = -ARGI(2);
    RET(1);
}
HLE(g_SaveDC) {
    Dc *dc = dc_get(ARG(0));
    if (!dc || dc->saved_count >= 16) { RET(0); return; }
    Dc *copy = malloc(sizeof *copy);
    *copy = *dc;
    copy->saved_count = 0;
    dc->saved[dc->saved_count++] = copy;
    RET(dc->saved_count);
}
HLE(g_RestoreDC) {
    Dc *dc = dc_get(ARG(0));
    if (!dc || dc->saved_count == 0) { RET(0); return; }
    int idx = ARGI(1);
    int target = idx < 0 ? dc->saved_count + idx : idx - 1;
    if (target < 0 || target >= dc->saved_count) { RET(0); return; }
    Dc *s = dc->saved[target];
    Dc *saved[16];
    memcpy(saved, dc->saved, sizeof saved);
    int count = target;
    for (int i = target + 1; i < dc->saved_count; i++) free(saved[i]);
    *dc = *s;
    free(s);
    memcpy(dc->saved, saved, sizeof saved);
    dc->saved_count = count;
    RET(1);
}
HLE(g_IntersectClipRect) {
    Dc *dc = dc_get(ARG(0));
    if (!dc) { RET(0); return; }
    int x0 = ARGI(1), y0 = ARGI(2), x1 = ARGI(3), y1 = ARGI(4);
    if (dc->clip_set) { if (dc->cx0 > x0) x0 = dc->cx0; if (dc->cy0 > y0) y0 = dc->cy0; if (dc->cx1 < x1) x1 = dc->cx1; if (dc->cy1 < y1) y1 = dc->cy1; }
    dc->clip_set = 1; dc->cx0 = x0; dc->cy0 = y0; dc->cx1 = x1; dc->cy1 = y1;
    RET(2);
}
HLE_STUB_RET(g_ExcludeClipRect, 2)
HLE(g_SelectClipRgn) {
    Dc *dc = dc_get(ARG(0));
    GdiObj *r = gdi_get(ARG(1), GO_REGION);
    if (!dc) { RET(0); return; }
    if (!r) dc->clip_set = 0;
    else { dc->clip_set = 1; dc->cx0 = r->rx0; dc->cy0 = r->ry0; dc->cx1 = r->rx1; dc->cy1 = r->ry1; }
    RET(2);
}
HLE(g_GetClipBox) {
    Dc *dc = dc_get(ARG(0));
    Target t;
    int32_t r[4] = { 0, 0, 0, 0 };
    if (dc && dc_target(dc, &t)) { r[0] = t.x0 - t.ox; r[1] = t.y0 - t.oy; r[2] = t.x1 - t.ox; r[3] = t.y1 - t.oy; }
    memcpy(PTR(1), r, 16);
    RET(dc ? 2 : 0);
}
static uint64_t make_region(int x0, int y0, int x1, int y1) { GdiObj *o; uint64_t h = gdi_alloc(GO_REGION, &o); o->rx0 = x0; o->ry0 = y0; o->rx1 = x1; o->ry1 = y1; return h; }
HLE(g_CreateRectRgn) { RET(make_region(ARGI(0), ARGI(1), ARGI(2), ARGI(3))); }
HLE(g_CreateRectRgnIndirect) { int32_t r[4]; memcpy(r, PTR(0), 16); RET(make_region(r[0], r[1], r[2], r[3])); }
HLE(g_CreateRoundRectRgn) { RET(make_region(ARGI(0), ARGI(1), ARGI(2), ARGI(3))); }
HLE(g_CreateEllipticRgn) { RET(make_region(ARGI(0), ARGI(1), ARGI(2), ARGI(3))); }
HLE(g_SetRectRgn) { GdiObj *r = gdi_get(ARG(0), GO_REGION); if (r) { r->rx0 = ARGI(1); r->ry0 = ARGI(2); r->rx1 = ARGI(3); r->ry1 = ARGI(4); } RET(r != NULL); }
HLE(g_CombineRgn) {
    GdiObj *d = gdi_get(ARG(0), GO_REGION), *a = gdi_get(ARG(1), GO_REGION), *b = gdi_get(ARG(2), GO_REGION);
    if (!d || !a) { RET(0); return; }
    int mode = ARGI(3);
    if (mode == 5 || !b) { d->rx0 = a->rx0; d->ry0 = a->ry0; d->rx1 = a->rx1; d->ry1 = a->ry1; }      // RGN_COPY
    else if (mode == 1) { d->rx0 = a->rx0 > b->rx0 ? a->rx0 : b->rx0; d->ry0 = a->ry0 > b->ry0 ? a->ry0 : b->ry0; d->rx1 = a->rx1 < b->rx1 ? a->rx1 : b->rx1; d->ry1 = a->ry1 < b->ry1 ? a->ry1 : b->ry1; }
    else { d->rx0 = a->rx0 < b->rx0 ? a->rx0 : b->rx0; d->ry0 = a->ry0 < b->ry0 ? a->ry0 : b->ry0; d->rx1 = a->rx1 > b->rx1 ? a->rx1 : b->rx1; d->ry1 = a->ry1 > b->ry1 ? a->ry1 : b->ry1; }
    RET(d->rx0 < d->rx1 && d->ry0 < d->ry1 ? 2 : 1);
}
HLE(g_FillRgn) {
    GdiObj *r = gdi_get(ARG(1), GO_REGION);
    Dc *dc = dc_get(ARG(0));
    if (!r || !dc) { RET(0); return; }
    int bnull; uint32_t px = brush_px(dc, ARG(2), &bnull);
    if (!bnull) gdi_fill_rect_px(ARG(0), r->rx0, r->ry0, r->rx1, r->ry1, px_rgb(px));
    RET(1);
}
HLE(g_GetDeviceCaps) {
    GuiState *g = gui();
    switch (ARGI(1)) {
    case 2: RET(1); break;            // TECHNOLOGY = DT_RASDISPLAY
    case 4: RET(338); break;          // HORZSIZE mm
    case 6: RET(211); break;          // VERTSIZE
    case 8: case 118: RET(g->screen_w); break;
    case 10: case 117: RET(g->screen_h); break;
    case 12: RET(32); break;          // BITSPIXEL
    case 14: RET(1); break;           // PLANES
    case 24: RET((uint64_t)-1); break; // NUMCOLORS
    case 38: RET(0x1 | 0x8 | 0x80 | 0x800 | 0x2000); break; // RASTERCAPS
    case 88: case 90: RET(96); break; // LOGPIXELSX/Y
    case 104: RET(0); break;          // SIZEPALETTE
    case 108: RET(24); break;         // COLORRES
    case 116: RET(60); break;         // VREFRESH
    default: RET(0); break;
    }
}
HLE_STUB_RET(g_GdiFlush, 1)
HLE(g_CreatePalette) { GdiObj *o; RET(gdi_alloc(GO_PALETTE, &o)); }
HLE(g_SelectPalette) { Dc *dc = dc_get(ARG(0)); uint64_t old = dc ? dc->palette : 0; if (dc) dc->palette = ARG(1); RET(old ? old : g_stock[15]); }
HLE_STUB_RET(g_RealizePalette, 0)
HLE_STUB_RET(g_AddFontResourceEx, 1)
// Font enumeration: the faces Soul draws with its built-in fonts, reported as TrueType fonts.
static const char *const k_enum_faces[] = { "Arial", "Calibri", "Consolas", "Courier New", "Lucida Console", "Microsoft Sans Serif",
    "MS Sans Serif", "MS Shell Dlg", "MS Shell Dlg 2", "Segoe UI", "Tahoma", "Times New Roman", "Trebuchet MS", "Verdana", NULL };
static int face_fixed(const char *f) { return !strcasecmp(f, "Consolas") || !strcasecmp(f, "Courier New") || !strcasecmp(f, "Lucida Console"); }
// mode 0: EnumFontFamiliesEx (hdc, logfont, proc, lparam, flags); 1: EnumFontFamilies / EnumFonts (hdc, face, proc, lparam)
static void enum_fonts(X86State *st, int wide, int mode) {
    char want[64] = "";
    uint64_t proc = ARG(2), lparam = ARG(3);
    if (mode == 0 && ARG(1)) {
        uint64_t lf = ARG(1);
        if (wide) { char *s = w2u_dup(G2H(lf + 28)); snprintf(want, sizeof want, "%s", s); free(s); }
        else snprintf(want, sizeof want, "%s", (const char *)G2H(lf + 28));
    } else if (mode == 1 && ARG(1)) {
        if (wide) { char *s = w2u_dup(PTR(1)); snprintf(want, sizeof want, "%s", s); free(s); }
        else snprintf(want, sizeof want, "%s", (const char *)PTR(1));
    }
    size_t cs = wide ? 2 : 1;
    size_t lf_size = 28 + 32 * cs, elf_size = lf_size + (64 + 32 + 32) * cs;
    size_t tm_size = wide ? 60 : 56, ntm_size = tm_size + 16 + 24;
    uint8_t *elf = g_calloc(1, elf_size), *ntm = g_calloc(1, ntm_size);
    uint64_t r = 1;
    for (int i = 0; k_enum_faces[i] && r; i++) {
        const char *f = k_enum_faces[i];
        if (want[0] && strcasecmp(want, f)) continue;
        memset(elf, 0, elf_size); memset(ntm, 0, ntm_size);
        int32_t lf[7] = { -16, 0, 0, 0, 400, 0, 0 };
        memcpy(elf, lf, sizeof lf);
        elf[23] = 1;                              // DEFAULT_CHARSET would be 1; ANSI_CHARSET = 0
        elf[23] = 0;
        elf[24] = 7;                              // OUT_TT_PRECIS
        elf[27] = (uint8_t)((face_fixed(f) ? 1 : 2) | (face_fixed(f) ? 0x30 : 0x20)); // pitch | FF_MODERN / FF_SWISS
        if (wide) {
            u2w(f, -1, (uint16_t *)(elf + 28), 32);
            u2w(f, -1, (uint16_t *)(elf + lf_size), 64);
            u2w("Regular", -1, (uint16_t *)(elf + lf_size + 128), 32);
            u2w("Western", -1, (uint16_t *)(elf + lf_size + 192), 32);
        } else {
            snprintf((char *)elf + 28, 32, "%s", f);
            snprintf((char *)elf + lf_size, 64, "%s", f);
            snprintf((char *)elf + lf_size + 64, 32, "Regular");
            snprintf((char *)elf + lf_size + 96, 32, "Western");
        }
        int32_t tm[11] = { 16, 13, 3, 0, 0, face_fixed(f) ? 8 : 7, 16, 400, 0, 96, 96 };
        memcpy(ntm, tm, sizeof tm);
        uint8_t *tail = ntm + 44 + (wide ? 8 : 4); // after the first/last/default/break characters
        if (wide) { uint16_t ch[4] = { 32, 0xFFFC, 0x1F, 32 }; memcpy(ntm + 44, ch, 8); }
        else { ntm[44] = 32; ntm[45] = 255; ntm[46] = 31; ntm[47] = 32; }
        tail[3] = (uint8_t)((face_fixed(f) ? 0 : 1) | 4 | 2); // tmPitchAndFamily: variable pitch | TrueType | vector
        uint32_t flags = 0x40;                     // NTM_REGULAR
        memcpy(ntm + tm_size, &flags, 4);
        uint64_t args[4] = { H2G(elf), H2G(ntm), 4 /* TRUETYPE_FONTTYPE */, lparam };
        r = soul_guest_call(st, proc, 4, args) & 0xFFFFFFFF;
    }
    g_free(elf); g_free(ntm);
    RET(r);
}
HLE(g_EnumFontFamiliesExW) { enum_fonts(st, 1, 0); }
HLE(g_EnumFontFamiliesExA) { enum_fonts(st, 0, 0); }
HLE(g_EnumFontFamiliesW) { enum_fonts(st, 1, 1); }
HLE(g_EnumFontFamiliesA) { enum_fonts(st, 0, 1); }
HLE(g_RectVisible) { RET(1); }
HLE(g_UnrealizeObject) { RET(1); }
HLE(g_GetDIBColorTable) { // (hdc, start, count, RGBQUAD*): the palette of the bitmap selected into the DC
    Dc *dc = dc_get(ARG(0));
    if (!dc || !ARG(3)) { RET(0); return; }
    GdiObj *bm = gdi_get(dc->bitmap, GO_BITMAP);
    if (!bm || bm->surf.bpp > 8) { RET(0); return; }
    uint32_t n = 1u << bm->surf.bpp, start = (uint32_t)ARG(1), cnt = (uint32_t)ARG(2), k = 0;
    for (uint32_t i = start; i < n && k < cnt; i++, k++) {
        uint32_t c = bm->surf.palette[i];
        uint8_t q[4] = { (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)(c >> 16), 0 };
        memcpy((uint8_t *)PTR(3) + 4 * k, q, 4);
    }
    RET(k);
}
HLE(g_GetRegionData) { RET(0); }

// ---- helpers used by user32 / built-in controls -------------------------------------------------

void gdi_text_out_utf8(uint64_t hdc, int x, int y, const char *s, int len) {
    Dc *dc = dc_get(hdc);
    if (dc) dc_text_out(dc, x, y, s, len < 0 ? (int)strlen(s) : len, NULL);
}

int gdi_draw_text_dc(uint64_t hdc, const char *s, int len, int32_t *rect, uint32_t format) {
    Dc *dc = dc_get(hdc);
    if (!dc) return 0;
    int r[4] = { rect[0], rect[1], rect[2], rect[3] };
    int h = draw_text_impl(dc, s, len < 0 ? (int)strlen(s) : len, r, format);
    if (format & 0x400) { rect[2] = r[2]; rect[3] = r[3]; }
    return h;
}

void gdi_fill_ellipse_px(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t fill, uint32_t border) {
    Dc *dc = dc_get(hdc);
    if (!dc) return;
    uint64_t ob = dc->brush, op = dc->pen;
    uint64_t b = make_brush(fill, 0, 0), pn = make_pen(border, 0, 1, 0);
    dc->brush = b; dc->pen = pn;
    draw_ellipse(dc, x0, y0, x1, y1);
    dc->brush = ob; dc->pen = op;
    gdi_free(b); gdi_free(pn);
}

void gdi_rect_dc(uint64_t hdc, int l, int t, int r, int b) { Dc *dc = dc_get(hdc); if (dc) draw_rect(dc, l, t, r, b); }

void gdi_line_px(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref, int width) {
    Dc *dc = dc_get(hdc);
    Target t;
    if (!dc || !dc_target(dc, &t)) return;
    line_px(&t, 13, x0 + t.ox, y0 + t.oy, x1 + t.ox, y1 + t.oy, rgb_px(colorref), width, 0);
    target_done(&t);
}

// Draws straight-alpha ARGB pixels at (x, y) of a DC (image lists, toolbar glyphs).
void gdi_draw_argb(uint64_t hdc, int x, int y, const uint32_t *px, int w, int h) {
    Dc *dc = dc_get(hdc);
    Target t;
    if (!dc || !px || !dc_target(dc, &t)) return;
    for (int j = 0; j < h; j++) {
        int ty = y + j + t.oy;
        if (ty < t.y0 || ty >= t.y1) continue;
        for (int i = 0; i < w; i++) {
            int tx = x + i + t.ox;
            if (tx < t.x0 || tx >= t.x1) continue;
            uint32_t s = px[(size_t)j * (size_t)w + (size_t)i], a = s >> 24;
            if (!a) continue;
            if (a == 255) { surf_put(&t.s, tx, ty, s | 0xFF000000u); continue; }
            uint32_t d = surf_get(&t.s, tx, ty), o = 0xFF000000u;
            for (int k = 0; k < 24; k += 8) {
                uint32_t sc = (s >> k) & 255, dcol = (d >> k) & 255;
                o |= ((sc * a + dcol * (255 - a) + 127) / 255) << k;
            }
            surf_put(&t.s, tx, ty, o);
        }
    }
    target_done(&t);
}
// Pixels of a bitmap as ARGB (malloc'ed); 32 bpp DIBs keep their alpha when any pixel uses it.
int gdi_bitmap_argb(uint64_t hbm, uint32_t **out, int *w, int *h) {
    GdiObj *o = gdi_get(hbm, GO_BITMAP);
    if (!o || !o->surf.bits || o->surf.w <= 0 || o->surf.h <= 0) return 0;
    const Surface *s = &o->surf;
    uint32_t *p = malloc(sizeof(uint32_t) * (size_t)s->w * (size_t)s->h);
    int has_alpha = 0;
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            uint32_t v = surf_get(s, x, y);
            if (s->bpp == 32) { uint32_t raw; memcpy(&raw, surf_row(s, y) + x * 4, 4); if (raw >> 24) has_alpha = 1; v = (v & 0xFFFFFFu) | (raw & 0xFF000000u); }
            p[(size_t)y * (size_t)s->w + (size_t)x] = v;
        }
    if (!has_alpha) for (size_t i = 0; i < (size_t)s->w * (size_t)s->h; i++) p[i] |= 0xFF000000u;
    *out = p; *w = s->w; *h = s->h;
    return 1;
}

uint64_t gdi_bitmap_from_dib(const uint8_t *bmi, const uint8_t *bits) {
    Surface s;
    uint32_t hsize; memcpy(&hsize, bmi, 4);
    if (!bits) {
        // packed DIB: the pixels follow the header and the colour table
        uint16_t bpp; uint32_t comp = 0, used = 0;
        if (hsize == 12) memcpy(&bpp, bmi + 10, 2);
        else { memcpy(&bpp, bmi + 14, 2); memcpy(&comp, bmi + 16, 4); memcpy(&used, bmi + 32, 4); }
        size_t colors = bpp <= 8 ? (used ? used : (1u << bpp)) : (comp == 3 ? 3 : 0);
        bits = bmi + hsize + colors * (hsize == 12 ? 3 : 4);
    }
    if (!surface_from_bmi(&s, (void *)bits, bmi)) return 0;
    uint64_t hb = make_bitmap(s.w, s.h, 32, 1, NULL, 1);
    GdiObj *o = gdi_get(hb, GO_BITMAP);
    if (!o) return 0;
    for (int y = 0; y < s.h; y++) for (int x = 0; x < s.w; x++) surf_put(&o->surf, x, y, surf_get(&s, x, y));
    return hb;
}

const HleEntry hle_gdi32_table[] = {
    { "GetStockObject", g_GetStockObject, NULL }, { "SelectObject", g_SelectObject, NULL }, { "DeleteObject", g_DeleteObject, NULL },
    { "GetObjectType", g_GetObjectType, NULL }, { "GetObjectA", g_GetObjectA, NULL }, { "GetObjectW", g_GetObjectW, NULL },
    { "GetCurrentObject", g_GetCurrentObject, NULL }, { "CreateSolidBrush", g_CreateSolidBrush, NULL }, { "CreateHatchBrush", g_CreateHatchBrush, NULL },
    { "CreatePatternBrush", g_CreatePatternBrush, NULL }, { "CreateBrushIndirect", g_CreateBrushIndirect, NULL }, { "CreatePen", g_CreatePen, NULL },
    { "CreatePenIndirect", g_CreatePenIndirect, NULL }, { "ExtCreatePen", g_ExtCreatePen, NULL }, { "SetDCBrushColor", g_SetDCBrushColor, NULL },
    { "SetDCPenColor", g_SetDCPenColor, NULL }, { "CreateFontA", g_CreateFontA, NULL }, { "CreateFontW", g_CreateFontW, NULL },
    { "CreateFontIndirectA", g_CreateFontIndirectA, NULL }, { "CreateFontIndirectW", g_CreateFontIndirectW, NULL },
    { "CreateFontIndirectExW", g_CreateFontIndirectExW, NULL }, { "CreateCompatibleDC", g_CreateCompatibleDC, NULL },
    { "CreateDCA", g_CreateDCA, NULL }, { "CreateDCW", g_CreateDCA, NULL }, { "DeleteDC", g_DeleteDC, NULL },
    { "CreateCompatibleBitmap", g_CreateCompatibleBitmap, NULL }, { "CreateDiscardableBitmap", g_CreateCompatibleBitmap, NULL },
    { "CreateBitmap", g_CreateBitmap, NULL }, { "CreateBitmapIndirect", g_CreateBitmapIndirect, NULL }, { "CreateDIBSection", g_CreateDIBSection, NULL },
    { "CreateDIBitmap", g_CreateDIBitmap, NULL }, { "GetDIBits", g_GetDIBits, NULL }, { "SetDIBits", g_SetDIBits, NULL },
    { "StretchDIBits", g_StretchDIBits, NULL }, { "SetDIBitsToDevice", g_SetDIBitsToDevice, NULL }, { "BitBlt", g_BitBlt, NULL },
    { "StretchBlt", g_StretchBlt, NULL }, { "PatBlt", g_PatBlt, NULL }, { "TransparentBlt", g_TransparentBlt, NULL },
    { "GdiTransparentBlt", g_TransparentBlt, NULL }, { "AlphaBlend", g_AlphaBlend, NULL }, { "GdiAlphaBlend", g_AlphaBlend, NULL },
    { "SetPixel", g_SetPixel, NULL }, { "SetPixelV", g_SetPixel, NULL }, { "GetPixel", g_GetPixel, NULL }, { "MoveToEx", g_MoveToEx, NULL },
    { "GetCurrentPositionEx", g_GetCurrentPositionEx, NULL }, { "LineTo", g_LineTo, NULL }, { "Polyline", g_Polyline, NULL },
    { "PolylineTo", g_PolylineTo, NULL }, { "Polygon", g_Polygon, NULL }, { "Rectangle", g_Rectangle, NULL }, { "RoundRect", g_RoundRect, NULL },
    { "Ellipse", g_Ellipse, NULL }, { "Pie", g_Pie, NULL }, { "Chord", g_Chord, NULL }, { "Arc", g_Arc, NULL }, { "ArcTo", g_Arc, NULL },
    { "TextOutA", g_TextOutA, NULL }, { "TextOutW", g_TextOutW, NULL }, { "ExtTextOutA", g_ExtTextOutA, NULL }, { "ExtTextOutW", g_ExtTextOutW, NULL },
    { "GetTextExtentPoint32A", g_GetTextExtentPoint32A, NULL }, { "GetTextExtentPoint32W", g_GetTextExtentPoint32W, NULL },
    { "GetTextExtentPointA", g_GetTextExtentPoint32A, NULL }, { "GetTextExtentPointW", g_GetTextExtentPoint32W, NULL },
    { "GetTextMetricsA", g_GetTextMetricsA, NULL }, { "GetTextMetricsW", g_GetTextMetricsW, NULL }, { "GetTextFaceA", g_GetTextFaceA, NULL },
    { "GetTextFaceW", g_GetTextFaceW, NULL }, { "GetCharWidth32W", g_GetCharWidth32, NULL }, { "GetCharWidth32A", g_GetCharWidth32, NULL },
    { "GetCharWidthW", g_GetCharWidth32, NULL }, { "SetTextColor", g_SetTextColor, NULL }, { "GetTextColor", g_GetTextColor, NULL },
    { "SetBkColor", g_SetBkColor, NULL }, { "GetBkColor", g_GetBkColor, NULL }, { "SetBkMode", g_SetBkMode, NULL }, { "GetBkMode", g_GetBkMode, NULL },
    { "SetTextAlign", g_SetTextAlign, NULL }, { "GetTextAlign", g_GetTextAlign, NULL }, { "SetROP2", g_SetROP2, NULL }, { "GetROP2", g_GetROP2, NULL },
    { "SetStretchBltMode", g_SetStretchBltMode, NULL }, { "GetStretchBltMode", g_GetStretchBltMode, NULL }, { "SetBrushOrgEx", g_SetBrushOrgEx, NULL },
    { "SetMapMode", g_SetMapMode, NULL }, { "GetMapMode", g_GetMapMode, NULL }, { "SetViewportOrgEx", g_SetViewportOrgEx, NULL },
    { "OffsetViewportOrgEx", g_OffsetViewportOrgEx, NULL }, { "GetViewportOrgEx", g_GetViewportOrgEx, NULL }, { "SetWindowOrgEx", g_SetWindowOrgEx, NULL },
    { "SaveDC", g_SaveDC, NULL }, { "RestoreDC", g_RestoreDC, NULL }, { "IntersectClipRect", g_IntersectClipRect, NULL },
    { "ExcludeClipRect", g_ExcludeClipRect, NULL }, { "SelectClipRgn", g_SelectClipRgn, NULL }, { "GetClipBox", g_GetClipBox, NULL },
    { "CreateRectRgn", g_CreateRectRgn, NULL }, { "CreateRectRgnIndirect", g_CreateRectRgnIndirect, NULL }, { "CreateRoundRectRgn", g_CreateRoundRectRgn, NULL },
    { "CreateEllipticRgn", g_CreateEllipticRgn, NULL }, { "SetRectRgn", g_SetRectRgn, NULL }, { "CombineRgn", g_CombineRgn, NULL }, { "FillRgn", g_FillRgn, NULL },
    { "GetDeviceCaps", g_GetDeviceCaps, NULL }, { "GdiFlush", g_GdiFlush, NULL }, { "CreatePalette", g_CreatePalette, NULL },
    { "SelectPalette", g_SelectPalette, NULL }, { "RealizePalette", g_RealizePalette, NULL }, { "AddFontResourceExW", g_AddFontResourceEx, NULL },
    { "AddFontResourceExA", g_AddFontResourceEx, NULL }, { "EnumFontFamiliesExW", g_EnumFontFamiliesExW, NULL }, { "EnumFontFamiliesExA", g_EnumFontFamiliesExA, NULL },
    { "EnumFontFamiliesW", g_EnumFontFamiliesW, NULL }, { "EnumFontFamiliesA", g_EnumFontFamiliesA, NULL },
    { "EnumFontsW", g_EnumFontFamiliesW, NULL }, { "EnumFontsA", g_EnumFontFamiliesA, NULL },
    { "RectVisible", g_RectVisible, NULL }, { "UnrealizeObject", g_UnrealizeObject, NULL }, { "GetDIBColorTable", g_GetDIBColorTable, NULL },
    { "GetRegionData", g_GetRegionData, NULL },
    { NULL, NULL, NULL }
};
