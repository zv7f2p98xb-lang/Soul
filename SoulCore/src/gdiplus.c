// SoulCore - GDI+ (gdiplus.dll flat API): graphics, pens, brushes, paths, matrices, regions, images, text.
//
// Rendering: every primitive is turned into polygons (curves flattened, strokes expanded to polygons),
// rasterised by a scanline coverage rasteriser (16 sub-scanlines, exact horizontal coverage; alternate and
// winding fill) into an 8-bit mask, multiplied by the clip mask and painted with the brush colour / gradient /
// texture. Image targets are drawn into directly; HDC targets through a temporary ARGB layer composited with
// gdi_draw_argb (clipping and viewport of the DC are honoured by it).
#include "gui.h"
#include "imgcodec.h"
#include "stream.h"
#include <math.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

enum { Ok = 0, GenericError = 1, InvalidParameter = 2, OutOfMemory = 3, ObjectBusy = 4, InsufficientBuffer = 5, NotImplemented = 6,
       Win32Error = 7, WrongState = 8, Aborted = 9, FileNotFound = 10, ValueOverflow = 11, AccessDenied = 12, UnknownImageFormat = 13,
       FontFamilyNotFound = 14, FontStyleNotFound = 15, NotTrueTypeFont = 16, UnsupportedGdiplusVersion = 17, GdiplusNotInitialized = 18,
       PropertyNotFound = 19, PropertyNotSupported = 20 };

// ---- argument access (floats: XMM0-3 in 64-bit programs for the first four, stack slots otherwise) ------------

static inline float fa(X86State *st, int i) {
    if (!g_mode32 && i < 4) return st->xmm[i].f[0];
    uint32_t u = (uint32_t)hle_arg(st, i);
    float f; memcpy(&f, &u, 4); return f;
}
#define FA(i) fa(st, (i))
#define IA(i) ((int32_t)ARG(i))
#define GP(name, n) static void gp_##name(X86State *st)
#define STATUS(v) do { RET((uint64_t)(v)); return; } while (0)

static inline float rdf(uint64_t a) { uint32_t u = (uint32_t)mem_rd(a, 4); float f; memcpy(&f, &u, 4); return f; }
static inline void wrf(uint64_t a, float f) { uint32_t u; memcpy(&u, &f, 4); mem_wr(a, 4, u); }
static inline int32_t rdi(uint64_t a) { return (int32_t)mem_rd(a, 4); }

// ---- basic types -----------------------------------------------------------------------------------------------

typedef struct Pt { float x, y; } Pt;
typedef struct Mat { float a, b, c, d, e, f; } Mat; // x' = a x + c y + e ; y' = b x + d y + f

static const Mat M_ID = { 1, 0, 0, 1, 0, 0 };
static inline Pt mpt(const Mat *m, Pt p) { Pt r = { p.x * m->a + p.y * m->c + m->e, p.x * m->b + p.y * m->d + m->f }; return r; }
static Mat mmul(Mat A, Mat B) { // apply A, then B
    Mat r = { A.a * B.a + A.b * B.c, A.a * B.b + A.b * B.d, A.c * B.a + A.d * B.c, A.c * B.b + A.d * B.d,
              A.e * B.a + A.f * B.c + B.e, A.e * B.b + A.f * B.d + B.f };
    return r;
}
static float mdet(const Mat *m) { return m->a * m->d - m->b * m->c; }
static int minv(const Mat *m, Mat *o) {
    float det = mdet(m);
    if (fabsf(det) < 1e-20f || det != det) return 0;
    float id = 1.0f / det;
    o->a = m->d * id; o->b = -m->b * id; o->c = -m->c * id; o->d = m->a * id;
    o->e = -(m->e * o->a + m->f * o->c); o->f = -(m->e * o->b + m->f * o->d);
    return 1;
}
static inline float mscale(const Mat *m) { float d = fabsf(mdet(m)); return d > 0 ? sqrtf(d) : 1.0f; }
static Mat mat_translate(float dx, float dy) { Mat m = { 1, 0, 0, 1, dx, dy }; return m; }
static Mat mat_scale(float sx, float sy) { Mat m = { sx, 0, 0, sy, 0, 0 }; return m; }
static Mat mat_rotate(float deg) { float r = deg * (float)M_PI / 180.0f, c = cosf(r), s = sinf(r); Mat m = { c, s, -s, c, 0, 0 }; return m; }
static Mat mat_shear(float sx, float sy) { Mat m = { 1, sy, sx, 1, 0, 0 }; return m; }
static int mat_is_ident(const Mat *m) { return m->a == 1 && m->b == 0 && m->c == 0 && m->d == 1 && m->e == 0 && m->f == 0; }

enum { K_GRAPHICS = 0x47500001, K_PEN, K_BRUSH, K_PATH, K_MATRIX, K_REGION, K_IMAGE, K_ATTR, K_FAMILY, K_FONT, K_SFORMAT, K_CACHED,
       K_FONTCOLL, K_PATHITER };

typedef struct Obj { int kind; } Obj;
static void *objp(uint64_t a, int kind) {
    if (!a) return NULL;
    Obj *o = G2H(a);
    return o->kind == kind ? o : NULL;
}
static void *onew(size_t sz, int kind) {
    Obj *o = g_calloc(1, sz);
    if (o) o->kind = kind;
    return o;
}
static void odel(void *p) { if (p) { ((Obj *)p)->kind = 0; g_free(p); } }
#define OB(T, n, k) ((T *)objp(ARG(n), (k)))
#define NEWOBJ(T, k) ((T *)onew(sizeof(T), (k)))

// ---- paths -------------------------------------------------------------------------------------------------------

typedef struct Path {
    int kind;
    Pt *pts; uint8_t *types; int n, cap;
    int fill;        // FillModeAlternate 0 / Winding 1
    int need_start;
} Path;

enum { PT_START = 0, PT_LINE = 1, PT_BEZIER = 3, PT_MASK = 7, PT_CLOSE = 0x80 };

static int path_reserve(Path *p, int extra) {
    if (p->n + extra <= p->cap) return 1;
    int nc = p->cap ? p->cap * 2 : 32;
    while (nc < p->n + extra) nc *= 2;
    Pt *np = g_realloc(p->pts, sizeof(Pt) * (size_t)nc);
    if (!np) return 0;
    p->pts = np;
    uint8_t *nt = g_realloc(p->types, (size_t)nc);
    if (!nt) return 0;
    p->types = nt; p->cap = nc;
    return 1;
}
static void path_add(Path *p, float x, float y, int type) {
    if (!path_reserve(p, 1)) return;
    p->pts[p->n].x = x; p->pts[p->n].y = y; p->types[p->n] = (uint8_t)type; p->n++;
}
static void path_start_or_line(Path *p, Pt q) { // connect to the current figure or start one
    if (p->n == 0 || p->need_start || (p->types[p->n - 1] & PT_CLOSE)) { path_add(p, q.x, q.y, PT_START); p->need_start = 0; return; }
    Pt l = p->pts[p->n - 1];
    if (l.x != q.x || l.y != q.y) path_add(p, q.x, q.y, PT_LINE);
}
static void path_close(Path *p) { if (p->n > 0 && !(p->types[p->n - 1] & PT_CLOSE)) p->types[p->n - 1] |= PT_CLOSE; p->need_start = 1; }
static void path_free(Path *p) { if (!p) return; g_free(p->pts); g_free(p->types); odel(p); }
static Path *path_new(int fill) { Path *p = NEWOBJ(Path, K_PATH); if (p) p->fill = fill; return p; }
static Path *path_clone(const Path *s) {
    Path *p = path_new(s->fill);
    if (!p) return NULL;
    if (s->n && path_reserve(p, s->n)) { memcpy(p->pts, s->pts, sizeof(Pt) * (size_t)s->n); memcpy(p->types, s->types, (size_t)s->n); p->n = s->n; }
    p->need_start = s->need_start;
    return p;
}
static void path_xform(Path *p, const Mat *m) { for (int i = 0; i < p->n; i++) p->pts[i] = mpt(m, p->pts[i]); }

static void path_arc_pts(Path *p, float x, float y, float w, float h, float start, float sweep, int connect) {
    if (w <= 0 || h <= 0) return;
    if (sweep > 360) sweep = 360; else if (sweep < -360) sweep = -360;
    float cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
    float a0 = start * (float)M_PI / 180.0f, total = sweep * (float)M_PI / 180.0f;
    int nseg = (int)ceilf(fabsf(total) / ((float)M_PI / 2) - 1e-4f);
    if (nseg < 1) nseg = 1;
    float step = total / (float)nseg;
    float k = 4.0f / 3.0f * tanf(step / 4.0f);
    Pt s0 = { cx + rx * cosf(a0), cy + ry * sinf(a0) };
    if (connect) path_start_or_line(p, s0); else { path_add(p, s0.x, s0.y, PT_START); p->need_start = 0; }
    float a = a0;
    for (int i = 0; i < nseg; i++) {
        float b = a + step;
        float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        path_add(p, cx + rx * (ca - k * sa), cy + ry * (sa + k * ca), PT_BEZIER);
        path_add(p, cx + rx * (cb + k * sb), cy + ry * (sb - k * cb), PT_BEZIER);
        path_add(p, cx + rx * cb, cy + ry * sb, PT_BEZIER);
        a = b;
    }
}
// GDI+ arc angles are "visual" angles: convert to the ellipse parameter
static float arc_param(float deg, float w, float h) {
    float r = deg * (float)M_PI / 180.0f;
    return atan2f(sinf(r) * (w > 0 ? 1.0f / h : 1), cosf(r) * (h > 0 ? 1.0f / w : 1)) * 180.0f / (float)M_PI;
}
static void path_arc(Path *p, float x, float y, float w, float h, float start, float sweep, int connect) {
    if (w <= 0 || h <= 0) return;
    float s = arc_param(start, w, h), e = arc_param(start + sweep, w, h);
    float sw = e - s;
    if (sweep >= 0) { while (sw < 0) sw += 360; if (sweep >= 360) sw = 360; else while (sw > sweep + 180) sw -= 360; }
    else { while (sw > 0) sw -= 360; if (sweep <= -360) sw = -360; else while (sw < sweep - 180) sw += 360; }
    path_arc_pts(p, x, y, w, h, s, sw, connect);
}
static void path_rect(Path *p, float x, float y, float w, float h) {
    path_add(p, x, y, PT_START); path_add(p, x + w, y, PT_LINE); path_add(p, x + w, y + h, PT_LINE); path_add(p, x, y + h, PT_LINE);
    path_close(p);
}
static void path_ellipse(Path *p, float x, float y, float w, float h) {
    path_arc_pts(p, x, y, w, h, 0, 360, 0);
    path_close(p);
}
static void path_pie(Path *p, float x, float y, float w, float h, float start, float sweep) {
    Pt c = { x + w / 2, y + h / 2 };
    path_add(p, c.x, c.y, PT_START); p->need_start = 0;
    path_arc(p, x, y, w, h, start, sweep, 1);
    path_close(p);
}
// cardinal spline through pts -> beziers
static void path_curve(Path *p, const Pt *q, int n, float tension, int closed, int connect, int off, int nseg) {
    if (n < 2) return;
    int segs = closed ? n : nseg;
    #define P(i) q[closed ? (((i) % n) + n) % n : ((i) < 0 ? 0 : ((i) >= n ? n - 1 : (i)))]
    if (closed) { path_add(p, q[0].x, q[0].y, PT_START); p->need_start = 0; }
    else if (connect) path_start_or_line(p, q[off]); else { path_add(p, q[off].x, q[off].y, PT_START); p->need_start = 0; }
    for (int s = 0; s < segs; s++) {
        int i = off + s;
        Pt p0 = P(i), p1 = P(i + 1), pm = P(i - 1), p2 = P(i + 2);
        Pt t0 = { tension * (p1.x - pm.x), tension * (p1.y - pm.y) }, t1 = { tension * (p2.x - p0.x), tension * (p2.y - p0.y) };
        if (!closed && i == 0) { t0.x = tension * (p1.x - p0.x) * 1.0f; t0.y = tension * (p1.y - p0.y) * 1.0f; }
        if (!closed && i + 1 == n - 1) { t1.x = tension * (p1.x - p0.x); t1.y = tension * (p1.y - p0.y); }
        path_add(p, p0.x + t0.x / 3, p0.y + t0.y / 3, PT_BEZIER);
        path_add(p, p1.x - t1.x / 3, p1.y - t1.y / 3, PT_BEZIER);
        path_add(p, p1.x, p1.y, PT_BEZIER);
    }
    #undef P
    if (closed) path_close(p);
}

// ---- flattened polylines ----------------------------------------------------------------------------------------

typedef struct Poly { Pt *p; int n; int closed; } Poly;
typedef struct PolySet { Poly *s; int n, cap; } PolySet;

static Poly *ps_new(PolySet *ps) {
    if (ps->n == ps->cap) {
        int nc = ps->cap ? ps->cap * 2 : 8;
        Poly *ns = realloc(ps->s, sizeof(Poly) * (size_t)nc);
        if (!ns) return NULL;
        ps->s = ns; ps->cap = nc;
    }
    Poly *q = &ps->s[ps->n++];
    memset(q, 0, sizeof *q);
    return q;
}
static int poly_add(Poly *q, Pt p, int *cap) {
    if (q->n == *cap) {
        int nc = *cap ? *cap * 2 : 16;
        Pt *np = realloc(q->p, sizeof(Pt) * (size_t)nc);
        if (!np) return 0;
        q->p = np; *cap = nc;
    }
    q->p[q->n++] = p;
    return 1;
}
static void ps_free(PolySet *ps) { for (int i = 0; i < ps->n; i++) free(ps->s[i].p); free(ps->s); memset(ps, 0, sizeof *ps); }
static void ps_add_poly(PolySet *ps, const Pt *pts, int n, int closed) {
    Poly *q = ps_new(ps);
    if (!q) return;
    q->p = malloc(sizeof(Pt) * (size_t)(n ? n : 1));
    if (!q->p) { ps->n--; return; }
    memcpy(q->p, pts, sizeof(Pt) * (size_t)n);
    q->n = n; q->closed = closed;
}

// Flattens a path (optionally transformed first, so the tolerance applies in the output space).
static void path_flatten(const Path *p, const Mat *xf, PolySet *out) {
    Poly *cur = NULL; int cap = 0;
    for (int i = 0; i < p->n; i++) {
        Pt pt = xf ? mpt(xf, p->pts[i]) : p->pts[i];
        int t = p->types[i] & PT_MASK;
        if (t == PT_START || !cur) {
            cur = ps_new(out); cap = 0;
            if (!cur) return;
            poly_add(cur, pt, &cap);
        } else if (t == PT_LINE) poly_add(cur, pt, &cap);
        else if (t == PT_BEZIER && i + 2 < p->n) {
            Pt p0 = cur->p[cur->n - 1];
            Pt p1 = pt, p2 = xf ? mpt(xf, p->pts[i + 1]) : p->pts[i + 1], p3 = xf ? mpt(xf, p->pts[i + 2]) : p->pts[i + 2];
            float len = hypotf(p1.x - p0.x, p1.y - p0.y) + hypotf(p2.x - p1.x, p2.y - p1.y) + hypotf(p3.x - p2.x, p3.y - p2.y);
            int ns = (int)ceilf(sqrtf(len) * 1.3f);
            if (ns < 6) ns = 6; else if (ns > 120) ns = 120;
            for (int k = 1; k <= ns; k++) {
                float u = (float)k / (float)ns, v = 1 - u;
                Pt q = { v * v * v * p0.x + 3 * v * v * u * p1.x + 3 * v * u * u * p2.x + u * u * u * p3.x,
                         v * v * v * p0.y + 3 * v * v * u * p1.y + 3 * v * u * u * p2.y + u * u * u * p3.y };
                poly_add(cur, q, &cap);
            }
            i += 2;
            if (p->types[i] & PT_CLOSE) cur->closed = 1;
            continue;
        }
        if (p->types[i] & PT_CLOSE) cur->closed = 1;
    }
}

// ---- pens / brushes -------------------------------------------------------------------------------------------------

enum { BR_SOLID = 0, BR_HATCH = 1, BR_TEXTURE = 2, BR_PATHGRAD = 3, BR_LINEAR = 4 };

typedef struct Image Image;
typedef struct AttrSet AttrSet;
struct AttrSet {
    int cm_on; float cm[25]; int cm_flags;
    int gm_on; float gm[25];
    int key_on; uint32_t lo, hi;
    int gamma_on; float gamma;
    int nocolor;
};
typedef struct ImgAttr {
    int kind;
    AttrSet cat[5];   // default, bitmap, brush, pen, text
    int wrap; uint32_t wrap_color; int clamp;
} ImgAttr;

typedef struct Brush {
    int kind;
    int type;
    uint32_t color, color2;      // solid / hatch fore, back / linear start, end / path-gradient centre
    int hatch;
    Mat xf;                      // brush transform
    int wrap;
    // linear gradient
    Pt p1, p2; Pt rc_c; float rc_w, rc_h, rc_angle; int rc_based, rc_scalable;
    int nblend; float *bfac, *bpos;
    int npreset; uint32_t *pcol; float *ppos;
    // texture
    Image *img; Pt tex_origin; int tex_x, tex_y, tex_w, tex_h; void *attr;
    // path gradient
    Pt *gpts; uint32_t *gcol; int gn; Pt center; int ngcol; float focus_x, focus_y;
} Brush;

typedef struct Pen {
    int kind;
    float width; uint32_t color; Brush *brush;
    int start_cap, end_cap, dash_cap, join, dash_style, align;
    float miter, dash_off;
    float *dash; int ndash;
    Mat xf;
    int unit;
} Pen;

static void image_free(Image *im);
static Image *image_clone(const Image *s);
static void brush_free(Brush *b) {
    if (!b) return;
    if (b->img) image_free(b->img);
    g_free(b->attr);
    g_free(b->bfac); g_free(b->bpos); g_free(b->pcol); g_free(b->ppos); g_free(b->gpts); g_free(b->gcol);
    odel(b);
}
static Brush *brush_new(int type) { Brush *b = NEWOBJ(Brush, K_BRUSH); if (b) { b->type = type; b->xf = M_ID; } return b; }

static void *dupmem(const void *s, size_t n) { void *d = g_malloc(n ? n : 1); if (d && n) memcpy(d, s, n); return d; }

static Brush *brush_clone(const Brush *s) {
    Brush *b = NEWOBJ(Brush, K_BRUSH);
    if (!b) return NULL;
    *b = *s;
    if (s->bfac) { b->bfac = dupmem(s->bfac, sizeof(float) * (size_t)s->nblend); b->bpos = dupmem(s->bpos, sizeof(float) * (size_t)s->nblend); }
    if (s->pcol) { b->pcol = dupmem(s->pcol, 4 * (size_t)s->npreset); b->ppos = dupmem(s->ppos, sizeof(float) * (size_t)s->npreset); }
    if (s->gpts) b->gpts = dupmem(s->gpts, sizeof(Pt) * (size_t)s->gn);
    if (s->gcol) b->gcol = dupmem(s->gcol, 4 * (size_t)s->ngcol);
    if (s->img) b->img = image_clone(s->img);
    if (s->attr) b->attr = dupmem(s->attr, sizeof(AttrSet));
    return b;
}

// ---- images ---------------------------------------------------------------------------------------------------------

enum { PF_1BPP = 0x30101, PF_4BPP = 0x30402, PF_8BPP = 0x30803, PF_16GRAY = 0x101004, PF_555 = 0x21005, PF_565 = 0x21006,
       PF_1555 = 0x61007, PF_24 = 0x21808, PF_32RGB = 0x22009, PF_ARGB = 0x26200A, PF_PARGB = 0xE200B, PF_48 = 0x10300C,
       PF_64ARGB = 0x34400D, PF_64PARGB = 0x1C400E };

typedef struct LockRec { uint64_t scan0; int x, y, w, h, fmt, mode, stride, user; uint8_t *buf; } LockRec;

struct Image {
    int kind;
    int type;               // 1 bitmap
    int w, h;
    uint32_t *px;           // non-premultiplied ARGB (guest visible memory)
    int pixfmt;
    float dpix, dpiy;
    Img *src;               // decoded source with all frames (NULL for created bitmaps)
    int frame;
    int rawfmt;             // IMG_* of the file format, 0 = memory bitmap
    uint32_t pal[256]; int pal_n; int pal_flags;
    LockRec lk[8]; int nlk;
    int gdc;                // a Graphics currently draws into it (count)
};

static int pf_bpp(int pf) { return (pf >> 8) & 255; }

static Image *image_new(int w, int h, int pf) {
    if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > (1u << 25)) return NULL;
    Image *im = NEWOBJ(Image, K_IMAGE);
    if (!im) return NULL;
    im->type = 1; im->w = w; im->h = h; im->pixfmt = pf; im->dpix = im->dpiy = 96.0f;
    im->px = g_calloc((size_t)w * (size_t)h, 4);
    if (!im->px) { odel(im); return NULL; }
    return im;
}
static void image_free(Image *im) {
    if (!im) return;
    img_free(im->src);
    for (int i = 0; i < im->nlk; i++) if (!im->lk[i].user) g_free(im->lk[i].buf);
    g_free(im->px);
    odel(im);
}
static Image *image_clone(const Image *s) {
    Image *im = image_new(s->w, s->h, s->pixfmt);
    if (!im) return NULL;
    memcpy(im->px, s->px, (size_t)s->w * (size_t)s->h * 4);
    im->dpix = s->dpix; im->dpiy = s->dpiy; im->rawfmt = s->rawfmt;
    memcpy(im->pal, s->pal, sizeof im->pal); im->pal_n = s->pal_n; im->pal_flags = s->pal_flags;
    if (s->src) { // keep the frames: re-decoding is not possible, copy them
        Img *c = calloc(1, sizeof *c);
        if (c) {
            *c = *s->src;
            c->fr = calloc((size_t)c->nframes, sizeof(ImgFrame));
            int ok = c->fr != NULL;
            for (int i = 0; ok && i < c->nframes; i++) {
                size_t n = (size_t)c->w * (size_t)c->h * 4;
                c->fr[i].px = malloc(n); c->fr[i].delay_cs = s->src->fr[i].delay_cs;
                if (!c->fr[i].px) { ok = 0; break; }
                memcpy(c->fr[i].px, s->src->fr[i].px, n);
            }
            if (ok) { im->src = c; im->frame = s->frame; }
            else { for (int i = 0; i < c->nframes; i++) if (c->fr) free(c->fr[i].px); free(c->fr); free(c); }
        }
    }
    return im;
}
static Image *image_from_img(Img *d) {
    int pf;
    if (d->indexed) pf = d->src_bpp == 1 ? PF_1BPP : d->src_bpp == 4 ? PF_4BPP : PF_8BPP;
    else if (d->fmt == IMG_PNG && d->gray && d->src_bpp == 16 && !d->has_alpha) pf = PF_16GRAY;
    else if (d->fmt == IMG_PNG && d->src_bpp == 64) pf = PF_64ARGB;
    else if (d->fmt == IMG_PNG && d->src_bpp == 48) pf = PF_48;
    else if (d->has_alpha || d->fmt == IMG_ICO) pf = PF_ARGB;
    else if (d->src_bpp == 32) pf = PF_32RGB;
    else pf = PF_24;
    Image *im = image_new(d->w, d->h, pf);
    if (!im) { img_free(d); return NULL; }
    memcpy(im->px, d->fr[0].px, (size_t)d->w * (size_t)d->h * 4);
    im->src = d; im->rawfmt = d->fmt;
    im->dpix = d->dpix > 0 ? d->dpix : 96.0f; im->dpiy = d->dpiy > 0 ? d->dpiy : 96.0f;
    if (d->indexed) { im->pal_n = d->pal_n; memcpy(im->pal, d->pal, sizeof im->pal); im->pal_flags = d->has_alpha ? 1 : 0; }
    return im;
}

// ---- regions ---------------------------------------------------------------------------------------------------------

enum { RG_INF, RG_EMPTY, RG_PATH, RG_COMB };
typedef struct Region {
    int kind;
    int type;
    Path *path;
    struct Region *l, *r;
    int mode;
} Region;

static Region *region_new(int type) { Region *r = NEWOBJ(Region, K_REGION); if (r) r->type = type; return r; }
static void region_free(Region *r) {
    if (!r) return;
    path_free(r->path); region_free(r->l); region_free(r->r); odel(r);
}
static Region *region_clone(const Region *s) {
    Region *r = region_new(s->type);
    if (!r) return NULL;
    r->mode = s->mode;
    if (s->path) r->path = path_clone(s->path);
    if (s->l) r->l = region_clone(s->l);
    if (s->r) r->r = region_clone(s->r);
    return r;
}
static Region *region_from_path(const Path *p, const Mat *xf) {
    Region *r = region_new(RG_PATH);
    if (!r) return NULL;
    r->path = path_clone(p);
    if (xf) path_xform(r->path, xf);
    return r;
}
static Region *region_from_rect(float x, float y, float w, float h, const Mat *xf) {
    Path *p = path_new(0);
    path_rect(p, x, y, w, h);
    Region *r = region_from_path(p, xf);
    path_free(p);
    return r;
}
static void region_xform(Region *r, const Mat *m) {
    if (!r) return;
    if (r->path) path_xform(r->path, m);
    region_xform(r->l, m); region_xform(r->r, m);
}
// r = r (mode) other. Takes ownership of other.
static Region *region_combine(Region *r, Region *other, int mode) {
    if (mode == 0) { region_free(r); return other; }
    Region *n = region_new(RG_COMB);
    if (!n) return r;
    n->l = r; n->r = other; n->mode = mode;
    return n;
}

// ---- rasteriser ----------------------------------------------------------------------------------------------------

typedef struct Mask { int x0, y0, w, h; uint8_t *a; } Mask;
typedef struct Edge { float x0, y0, x1, y1, slope; int dir; } Edge;

static int edge_cmp(const void *a, const void *b) {
    float ya = ((const Edge *)a)->y0, yb = ((const Edge *)b)->y0;
    return ya < yb ? -1 : ya > yb;
}
static void span_add(float *area, float *diff, int w, float xa, float xb) {
    if (xb <= 0 || xa >= (float)w) return;
    if (xa < 0) xa = 0;
    if (xb > (float)w) xb = (float)w;
    if (xb <= xa) return;
    int ia = (int)xa, ib = (int)xb;
    if (ia == ib) { area[ia] += xb - xa; return; }
    area[ia] += (float)(ia + 1) - xa;
    if (ib < w) area[ib] += xb - (float)ib;
    diff[ia + 1] += 1; diff[ib] -= 1;
}

// Rasterises polygons into a mask restricted to the limit rectangle [lx0, lx1) x [ly0, ly1). Returns 0 when empty.
static int raster_polys(const PolySet *ps, int evenodd, int aa, int lx0, int ly0, int lx1, int ly1, Mask *m) {
    size_t ne = 0;
    for (int i = 0; i < ps->n; i++) ne += (size_t)ps->s[i].n;
    if (!ne) return 0;
    Edge *edges = malloc(sizeof(Edge) * ne);
    if (!edges) return 0;
    size_t cnt = 0;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < ps->n; i++) {
        const Poly *q = &ps->s[i];
        if (q->n < 2) continue;
        for (int k = 0; k < q->n; k++) {
            Pt a = q->p[k], b = q->p[(k + 1) % q->n];
            if (!(a.x == a.x && a.y == a.y && b.x == b.x && b.y == b.y)) continue;
            if (fabsf(a.x) > 1e7f || fabsf(a.y) > 1e7f || fabsf(b.x) > 1e7f || fabsf(b.y) > 1e7f) continue;
            if (a.x < minx) minx = a.x;
            if (a.x > maxx) maxx = a.x;
            if (a.y < miny) miny = a.y;
            if (a.y > maxy) maxy = a.y;
            if (a.y == b.y) continue;
            Edge e;
            if (a.y < b.y) { e.x0 = a.x; e.y0 = a.y; e.x1 = b.x; e.y1 = b.y; e.dir = 1; }
            else { e.x0 = b.x; e.y0 = b.y; e.x1 = a.x; e.y1 = a.y; e.dir = -1; }
            e.slope = (e.x1 - e.x0) / (e.y1 - e.y0);
            edges[cnt++] = e;
        }
    }
    if (!cnt) { free(edges); return 0; }
    int bx0 = (int)floorf(minx), by0 = (int)floorf(miny), bx1 = (int)ceilf(maxx) + 1, by1 = (int)ceilf(maxy) + 1;
    if (bx0 < lx0) bx0 = lx0;
    if (by0 < ly0) by0 = ly0;
    if (bx1 > lx1) bx1 = lx1;
    if (by1 > ly1) by1 = ly1;
    if (bx1 <= bx0 || by1 <= by0) { free(edges); return 0; }
    int w = bx1 - bx0, h = by1 - by0;
    uint8_t *out = calloc((size_t)w * (size_t)h, 1);
    float *area = malloc(sizeof(float) * (size_t)(w + 2)), *diff = malloc(sizeof(float) * (size_t)(w + 2));
    Edge **act = malloc(sizeof(Edge *) * cnt);
    float *xs = malloc(sizeof(float) * cnt);
    int *ds = malloc(sizeof(int) * cnt);
    if (!out || !area || !diff || !act || !xs || !ds) { free(edges); free(out); free(area); free(diff); free(act); free(xs); free(ds); return 0; }
    qsort(edges, cnt, sizeof(Edge), edge_cmp);
    size_t next = 0; int nact = 0;
    const int SS = 16;
    for (int row = by0; row < by1; row++) {
        while (next < cnt && edges[next].y0 < (float)(row + 1)) act[nact++] = &edges[next++];
        int j = 0;
        for (int i = 0; i < nact; i++) if (act[i]->y1 > (float)row) act[j++] = act[i];
        nact = j;
        if (!nact) continue;
        uint8_t *orow = out + (size_t)(row - by0) * (size_t)w;
        if (aa) { memset(area, 0, sizeof(float) * (size_t)(w + 2)); memset(diff, 0, sizeof(float) * (size_t)(w + 2)); }
        int nsub = aa ? SS : 1;
        int any = 0;
        for (int s = 0; s < nsub; s++) {
            float y = aa ? (float)row + ((float)s + 0.5f) / (float)SS : (float)row + 0.5f;
            int nc = 0;
            for (int i = 0; i < nact; i++) {
                const Edge *e = act[i];
                if (y < e->y0 || y >= e->y1) continue;
                float x = e->x0 + (y - e->y0) * e->slope;
                int k = nc++;
                while (k > 0 && xs[k - 1] > x) { xs[k] = xs[k - 1]; ds[k] = ds[k - 1]; k--; }
                xs[k] = x; ds[k] = e->dir;
            }
            if (!nc) continue;
            int wind = 0;
            for (int k = 0; k + 1 < nc; k++) {
                wind += ds[k];
                int inside = evenodd ? ((k + 1) & 1) : (wind != 0);
                if (!inside) continue;
                float xa = xs[k] - (float)bx0, xb = xs[k + 1] - (float)bx0;
                any = 1;
                if (aa) span_add(area, diff, w, xa, xb);
                else {
                    int ia = (int)ceilf(xa - 0.5f), ib = (int)ceilf(xb - 0.5f);
                    if (ia < 0) ia = 0;
                    if (ib > w) ib = w;
                    for (int x = ia; x < ib; x++) orow[x] = 255;
                }
            }
        }
        if (aa && any) {
            float run = 0;
            for (int x = 0; x < w; x++) {
                run += diff[x];
                float c = (area[x] + run) * (255.0f / (float)SS);
                int v = (int)(c + 0.5f);
                orow[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
            }
        }
    }
    free(edges); free(area); free(diff); free(act); free(xs); free(ds);
    m->x0 = bx0; m->y0 = by0; m->w = w; m->h = h; m->a = out;
    return 1;
}

// ---- region evaluation ---------------------------------------------------------------------------------------------

static void region_mask(const Region *r, int x0, int y0, int w, int h, uint8_t *out) {
    size_t n = (size_t)w * (size_t)h;
    switch (r->type) {
    case RG_INF: memset(out, 255, n); return;
    case RG_EMPTY: memset(out, 0, n); return;
    case RG_PATH: {
        memset(out, 0, n);
        PolySet ps; memset(&ps, 0, sizeof ps);
        path_flatten(r->path, NULL, &ps);
        Mask m;
        if (raster_polys(&ps, r->path->fill == 0, 1, x0, y0, x0 + w, y0 + h, &m)) {
            for (int y = 0; y < m.h; y++) memcpy(out + (size_t)(m.y0 - y0 + y) * (size_t)w + (size_t)(m.x0 - x0), m.a + (size_t)y * (size_t)m.w, (size_t)m.w);
            free(m.a);
        }
        ps_free(&ps);
        return;
    }
    default: {
        uint8_t *t = malloc(n ? n : 1);
        if (!t) { memset(out, 0, n); return; }
        region_mask(r->l, x0, y0, w, h, out);
        region_mask(r->r, x0, y0, w, h, t);
        for (size_t i = 0; i < n; i++) {
            int a = out[i], b = t[i], v;
            switch (r->mode) {
            case 1: v = a * b / 255; break;
            case 2: v = a + b - a * b / 255; break;
            case 3: v = a + b - 2 * a * b / 255; break;
            case 4: v = a * (255 - b) / 255; break;
            default: v = b * (255 - a) / 255; break;
            }
            out[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        free(t);
    }
    }
}
static int path_pt_in(const Path *p, float x, float y) {
    PolySet ps; memset(&ps, 0, sizeof ps);
    path_flatten(p, NULL, &ps);
    int wind = 0, cross = 0;
    for (int i = 0; i < ps.n; i++) {
        Poly *q = &ps.s[i];
        for (int k = 0; k < q->n; k++) {
            Pt a = q->p[k], b = q->p[(k + 1) % q->n];
            if ((a.y <= y) != (b.y <= y)) {
                float xi = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
                if (xi > x) { cross++; wind += b.y > a.y ? 1 : -1; }
            }
        }
    }
    ps_free(&ps);
    return p->fill == 0 ? (cross & 1) : wind != 0;
}
static int region_has(const Region *r, float x, float y) {
    switch (r->type) {
    case RG_INF: return 1;
    case RG_EMPTY: return 0;
    case RG_PATH: return path_pt_in(r->path, x, y);
    default: {
        int a = region_has(r->l, x, y), b = region_has(r->r, x, y);
        switch (r->mode) { case 1: return a && b; case 2: return a || b; case 3: return a != b; case 4: return a && !b; default: return b && !a; }
    }
    }
}
// Bounds (analytic): returns 0 for an empty region, -1 for an infinite one.
static int region_bounds(const Region *r, float *x0, float *y0, float *x1, float *y1) {
    switch (r->type) {
    case RG_INF: return -1;
    case RG_EMPTY: return 0;
    case RG_PATH: {
        if (!r->path->n) return 0;
        float a = 1e30f, b = 1e30f, c = -1e30f, d = -1e30f;
        for (int i = 0; i < r->path->n; i++) {
            Pt p = r->path->pts[i];
            if (p.x < a) a = p.x;
            if (p.x > c) c = p.x;
            if (p.y < b) b = p.y;
            if (p.y > d) d = p.y;
        }
        *x0 = a; *y0 = b; *x1 = c; *y1 = d;
        return 1;
    }
    default: {
        float lx0, ly0, lx1, ly1, rx0, ry0, rx1, ry1;
        int lr = region_bounds(r->l, &lx0, &ly0, &lx1, &ly1), rr = region_bounds(r->r, &rx0, &ry0, &rx1, &ry1);
        if (r->mode == 4) { if (lr == 1) { *x0 = lx0; *y0 = ly0; *x1 = lx1; *y1 = ly1; } return lr; }
        if (r->mode == 5) { if (rr == 1) { *x0 = rx0; *y0 = ry0; *x1 = rx1; *y1 = ry1; } return rr; }
        if (r->mode == 1) {
            if (lr == 0 || rr == 0) return 0;
            if (lr == -1) { if (rr == 1) { *x0 = rx0; *y0 = ry0; *x1 = rx1; *y1 = ry1; } return rr; }
            if (rr == -1) { *x0 = lx0; *y0 = ly0; *x1 = lx1; *y1 = ly1; return 1; }
            *x0 = fmaxf(lx0, rx0); *y0 = fmaxf(ly0, ry0); *x1 = fminf(lx1, rx1); *y1 = fminf(ly1, ry1);
            return (*x1 > *x0 && *y1 > *y0) ? 1 : 0;
        }
        if (lr == -1 || rr == -1) return -1;
        if (lr == 0) { if (rr == 1) { *x0 = rx0; *y0 = ry0; *x1 = rx1; *y1 = ry1; } return rr; }
        if (rr == 0) { *x0 = lx0; *y0 = ly0; *x1 = lx1; *y1 = ly1; return 1; }
        *x0 = fminf(lx0, rx0); *y0 = fminf(ly0, ry0); *x1 = fmaxf(lx1, rx1); *y1 = fmaxf(ly1, ry1);
        return 1;
    }
    }
}

// ---- image attributes ----------------------------------------------------------------------------------------------

static uint32_t attr_apply(const AttrSet *as, uint32_t px) {
    if (!as) return px;
    if (as->key_on) {
        uint32_t r = (px >> 16) & 255, g = (px >> 8) & 255, b = px & 255;
        uint32_t lr = (as->lo >> 16) & 255, lg = (as->lo >> 8) & 255, lb = as->lo & 255;
        uint32_t hr = (as->hi >> 16) & 255, hg = (as->hi >> 8) & 255, hb = as->hi & 255;
        if (r >= lr && r <= hr && g >= lg && g <= hg && b >= lb && b <= hb) return 0;
    }
    if (as->cm_on || as->gm_on || as->gamma_on) {
        float v[5] = { ((px >> 16) & 255) / 255.0f, ((px >> 8) & 255) / 255.0f, (px & 255) / 255.0f, (px >> 24) / 255.0f, 1.0f };
        if (as->cm_on) {
            float o[5] = { 0, 0, 0, 0, 0 };
            for (int j = 0; j < 5; j++) for (int i = 0; i < 5; i++) o[j] += v[i] * as->cm[i * 5 + j];
            memcpy(v, o, sizeof v);
        }
        if (as->gamma_on && as->gamma > 0) for (int i = 0; i < 3; i++) v[i] = powf(v[i] < 0 ? 0 : v[i], 1.0f / as->gamma);
        uint32_t o = 0;
        for (int i = 0; i < 4; i++) { int c = (int)(v[i] * 255.0f + 0.5f); c = c < 0 ? 0 : c > 255 ? 255 : c; o |= (uint32_t)c << (i == 0 ? 16 : i == 1 ? 8 : i == 2 ? 0 : 24); }
        return o;
    }
    return px;
}

// ---- graphics ----------------------------------------------------------------------------------------------------------

typedef struct GState {
    int id; Mat world; Region *clip;
    int smoothing, pixoff, interp, comp_mode, comp_quality, texthint, textcontrast, unit; float pscale;
} GState;

typedef struct Graphics {
    int kind;
    Image *img;
    uint64_t hdc; int hdc_owned;
    int w, h;
    float dpix, dpiy;
    int smoothing, pixoff, interp, comp_mode, comp_quality, texthint, textcontrast, unit;
    float pscale;
    Mat world;
    Region *clip;     // device space
    GState *stack; int nstack, cap, next_id;
    int busy;
} Graphics;

static float unit_per_inch(Graphics *g, int unit) {
    switch (unit) {
    case 3: return 72.0f; case 4: return 1.0f; case 5: return 300.0f; case 6: return 25.4f;
    default: return g->dpix;
    }
}
static Mat gpage(Graphics *g) {
    float s = g->pscale;
    if (g->unit >= 3) s *= g->dpix / unit_per_inch(g, g->unit);
    return mat_scale(s, s);
}
static Mat gtotal(Graphics *g) { Mat p = gpage(g); return mmul(g->world, p); }

static Graphics *graphics_new(void) {
    Graphics *g = NEWOBJ(Graphics, K_GRAPHICS);
    if (!g) return NULL;
    g->world = M_ID; g->unit = 2; g->pscale = 1; g->dpix = g->dpiy = 96; g->texthint = 0; g->comp_mode = 0; g->comp_quality = 0;
    g->clip = region_new(RG_INF);
    return g;
}
static void graphics_free(Graphics *g) {
    if (!g) return;
    if (g->img && g->img->gdc > 0) g->img->gdc--;
    region_free(g->clip);
    for (int i = 0; i < g->nstack; i++) region_free(g->stack[i].clip);
    g_free(g->stack);
    if (g->hdc_owned && g->hdc) gdi_release_dc(g->hdc);
    odel(g);
}
static void hdc_extent(uint64_t hdc, int *w, int *h) {
    *w = *h = 8192;
    Dc *dc = dc_get(hdc);
    if (!dc) return;
    if (dc->kind == DC_MEMORY) {
        GdiObj *b = gdi_get(dc->bitmap, GO_BITMAP);
        if (b) { *w = b->surf.w; *h = b->surf.h; }
    } else if (dc->kind == DC_WINDOW) {
        Win *wn = win_get(dc->hwnd);
        if (wn) { *w = wn->w > 0 ? wn->w : 8192; *h = wn->h > 0 ? wn->h : 8192; }
    }
}

// ---- paint sources -------------------------------------------------------------------------------------------------------

enum { P_SOLID, P_BRUSH, P_IMAGE };
typedef struct Paint {
    int type;
    uint32_t col;
    const Brush *b;
    Mat inv;          // device (pixel centre) -> brush space / source pixel space
    int bilinear;
    // images
    const Image *img; float sx, sy, sw, sh; int interp; const AttrSet *attr; float foot; int clampedge;
} Paint;

static uint32_t lerp_col(uint32_t a, uint32_t b, float t) {
    if (t <= 0) return a;
    if (t >= 1) return b;
    uint32_t o = 0;
    for (int k = 0; k < 32; k += 8) {
        float v = (float)((a >> k) & 255) * (1 - t) + (float)((b >> k) & 255) * t;
        o |= (uint32_t)(v + 0.5f) << k;
    }
    return o;
}
static float wrap_t(float t, int wrap) {
    switch (wrap) {
    case 4: return t < 0 ? 0 : t > 1 ? 1 : t;
    case 1: case 2: case 3: { float f = fmodf(t, 2.0f); if (f < 0) f += 2; return f > 1 ? 2 - f : f; }
    default: { float f = t - floorf(t); return f; }
    }
}
static const uint8_t bayer8[8][8] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 }, { 12, 44, 4, 36, 14, 46, 6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 }, { 15, 47, 7, 39, 13, 45, 5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 } };
static int hatch_on(int style, int x, int y) {
    x &= 7; y &= 7;
    static const int pct[] = { 5, 10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90 };
    if (style >= 6 && style <= 17) return bayer8[y][x] < pct[style - 6] * 64 / 100;
    switch (style) {
    case 0: return y == 0;
    case 1: return x == 0;
    case 2: return ((x - y) & 7) == 0;
    case 3: return ((x + y) & 7) == 7;
    case 4: return x == 0 || y == 0;
    case 5: return ((x - y) & 7) == 0 || ((x + y) & 7) == 7;
    case 18: return ((x - y) & 3) == 0;
    case 19: return ((x + y) & 3) == 3;
    case 20: return (((x - y) & 3) == 0) || (((x - y) & 3) == 1);
    case 21: return (((x + y) & 3) == 3) || (((x + y) & 3) == 2);
    case 22: return (((x - y) & 7) <= 1) ;
    case 23: return (((x + y) & 7) >= 6);
    case 24: return (x & 3) == 0;
    case 25: return (y & 3) == 0;
    case 26: return (x & 1) == 0;
    case 27: return (y & 1) == 0;
    case 28: return (x & 3) < 2;
    case 29: return (y & 3) < 2;
    case 30: return ((x - y) & 7) == 0 && (x & 2) == 0;
    case 31: return ((x + y) & 7) == 7 && (x & 2) == 0;
    case 32: return y == 0 && (x & 4) == 0;
    case 33: return x == 0 && (y & 4) == 0;
    case 34: return (x * 3 + y * 5) % 8 == 0;
    case 35: return (x * 3 + y * 5) % 8 < 2;
    case 43: return (x & 3) == 0 && (y & 1) == 0;
    case 48: return (x & 3) == 0 || (y & 3) == 0;
    case 49: return ((x + y) & 1) == 0;
    case 50: return (((x >> 2) + (y >> 2)) & 1) == 0;
    default: return ((x + y) & 1) == 0;
    }
}

static uint32_t sample_img(const Paint *p, float u, float v) { // u, v: source pixel coordinates (continuous)
    const Image *im = p->img;
    int x0 = (int)p->sx, y0 = (int)p->sy, w = (int)p->sw, h = (int)p->sh;
    if (w <= 0 || h <= 0) return 0;
    if (p->interp <= 0) { // nearest
        int ix = (int)floorf(u), iy = (int)floorf(v);
        if (p->clampedge) { if (ix < 0) ix = 0; if (ix >= w) ix = w - 1; if (iy < 0) iy = 0; if (iy >= h) iy = h - 1; }
        else if (ix < 0 || iy < 0 || ix >= w || iy >= h) return 0;
        return im->px[(size_t)(y0 + iy) * (size_t)im->w + (size_t)(x0 + ix)];
    }
    float fu = u - 0.5f, fv = v - 0.5f;
    int ix = (int)floorf(fu), iy = (int)floorf(fv);
    float ax = fu - (float)ix, ay = fv - (float)iy;
    float acc[4] = { 0, 0, 0, 0 }, wsum = 0;
    for (int k = 0; k < 4; k++) {
        int sx = ix + (k & 1), sy = iy + (k >> 1);
        float wt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
        if (wt <= 0) continue;
        if (p->clampedge) { if (sx < 0) sx = 0; if (sx >= w) sx = w - 1; if (sy < 0) sy = 0; if (sy >= h) sy = h - 1; }
        else if (sx < 0 || sy < 0 || sx >= w || sy >= h) { continue; }
        uint32_t c = im->px[(size_t)(y0 + sy) * (size_t)im->w + (size_t)(x0 + sx)];
        float al = (float)(c >> 24) * wt;
        acc[0] += (float)((c >> 16) & 255) * al; acc[1] += (float)((c >> 8) & 255) * al; acc[2] += (float)(c & 255) * al; acc[3] += al;
        wsum += wt;
    }
    if (acc[3] <= 0) return 0;
    float alpha = acc[3] / 1.0f; // premultiplied accumulation
    uint32_t o = ((uint32_t)(alpha + 0.5f) << 24);
    for (int i = 0; i < 3; i++) { int c = (int)(acc[i] / acc[3] + 0.5f); o |= (uint32_t)(c > 255 ? 255 : c) << (16 - 8 * i); }
    if (!p->clampedge && wsum < 1.0f && wsum > 0) { /* edge pixel: keep alpha scaled by coverage */ }
    return o;
}

static uint32_t paint_px(const Paint *p, int x, int y) {
    if (p->type == P_SOLID) return p->col;
    float cx = (float)x + 0.5f, cy = (float)y + 0.5f;
    if (p->type == P_IMAGE) {
        float u = cx * p->inv.a + cy * p->inv.c + p->inv.e, v = cx * p->inv.b + cy * p->inv.d + p->inv.f;
        uint32_t c;
        if (p->foot > 1.5f && p->interp >= 2) { // box filter for strong down-scaling
            int n = (int)ceilf(p->foot); if (n > 6) n = 6;
            float acc[4] = { 0, 0, 0, 0 }; int cnt = 0;
            float du = p->inv.a * (1.0f / n), dv = p->inv.d * (1.0f / n);
            for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
                float uu = u + ((float)i - (n - 1) / 2.0f) * du * 1.0f, vv = v + ((float)j - (n - 1) / 2.0f) * dv * 1.0f;
                Paint t = *p; t.bilinear = 0; t.interp = 0;
                uint32_t s = sample_img(&t, uu, vv);
                float al = (float)(s >> 24);
                acc[0] += (float)((s >> 16) & 255) * al; acc[1] += (float)((s >> 8) & 255) * al; acc[2] += (float)(s & 255) * al; acc[3] += al; cnt++;
            }
            if (acc[3] <= 0) return 0;
            c = ((uint32_t)(acc[3] / (float)cnt + 0.5f) << 24);
            for (int i = 0; i < 3; i++) c |= (uint32_t)(acc[i] / acc[3] + 0.5f) << (16 - 8 * i);
        } else c = sample_img(p, u, v);
        return attr_apply(p->attr, c);
    }
    const Brush *b = p->b;
    float u = cx * p->inv.a + cy * p->inv.c + p->inv.e, v = cx * p->inv.b + cy * p->inv.d + p->inv.f;
    switch (b->type) {
    case BR_HATCH: return hatch_on(b->hatch, x, y) ? b->color : b->color2;
    case BR_TEXTURE: {
        int tw = b->tex_w, th = b->tex_h;
        float fu = u, fv = v;
        int wrap = b->wrap;
        if (wrap == 4) { if (fu < 0 || fv < 0 || fu >= (float)tw || fv >= (float)th) return 0; }
        else {
            fu = fmodf(fu, (float)tw * ((wrap & 1) ? 2.0f : 1.0f)); if (fu < 0) fu += (float)tw * ((wrap & 1) ? 2.0f : 1.0f);
            fv = fmodf(fv, (float)th * ((wrap & 2) ? 2.0f : 1.0f)); if (fv < 0) fv += (float)th * ((wrap & 2) ? 2.0f : 1.0f);
            if ((wrap & 1) && fu >= (float)tw) fu = 2.0f * (float)tw - fu - 0.001f;
            if ((wrap & 2) && fv >= (float)th) fv = 2.0f * (float)th - fv - 0.001f;
        }
        Paint t; memset(&t, 0, sizeof t);
        t.img = b->img; t.sx = (float)b->tex_x; t.sy = (float)b->tex_y; t.sw = (float)tw; t.sh = (float)th; t.bilinear = p->bilinear; t.interp = p->bilinear ? 1 : 0; t.clampedge = 1;
        return attr_apply((const AttrSet *)b->attr, sample_img(&t, fu, fv));
    }
    case BR_LINEAR: {
        float t;
        if (b->rc_based) {
            float ca = cosf(b->rc_angle * (float)M_PI / 180.0f), sa = sinf(b->rc_angle * (float)M_PI / 180.0f);
            float qx = u - b->rc_c.x, qy = v - b->rc_c.y;
            if (b->rc_scalable) t = (qx / b->rc_w * ca + qy / b->rc_h * sa) / (fabsf(ca) + fabsf(sa)) + 0.5f;
            else t = (qx * ca + qy * sa) / (b->rc_w * fabsf(ca) + b->rc_h * fabsf(sa)) + 0.5f;
        } else {
            float dx = b->p2.x - b->p1.x, dy = b->p2.y - b->p1.y, l2 = dx * dx + dy * dy;
            t = l2 > 0 ? ((u - b->p1.x) * dx + (v - b->p1.y) * dy) / l2 : 0;
        }
        t = wrap_t(t, b->wrap);
        if (b->nblend > 0) {
            float f = b->bfac[b->nblend - 1];
            if (t <= b->bpos[0]) f = b->bfac[0];
            else for (int i = 1; i < b->nblend; i++) if (t <= b->bpos[i]) {
                float span = b->bpos[i] - b->bpos[i - 1];
                f = span > 0 ? b->bfac[i - 1] + (b->bfac[i] - b->bfac[i - 1]) * (t - b->bpos[i - 1]) / span : b->bfac[i];
                break;
            }
            t = f;
        }
        if (b->npreset > 0) {
            if (t <= b->ppos[0]) return b->pcol[0];
            for (int i = 1; i < b->npreset; i++) if (t <= b->ppos[i]) {
                float span = b->ppos[i] - b->ppos[i - 1];
                return lerp_col(b->pcol[i - 1], b->pcol[i], span > 0 ? (t - b->ppos[i - 1]) / span : 1);
            }
            return b->pcol[b->npreset - 1];
        }
        return lerp_col(b->color, b->color2, t);
    }
    case BR_PATHGRAD: {
        // find the boundary point hit by the ray centre -> q
        float dx = u - b->center.x, dy = v - b->center.y;
        float dist = hypotf(dx, dy);
        if (dist < 1e-6f) return b->color;
        float best = 1e30f; int bi = 0; float bt = 0;
        for (int i = 0; i < b->gn; i++) {
            Pt a = b->gpts[i], c = b->gpts[(i + 1) % b->gn];
            float ex = c.x - a.x, ey = c.y - a.y;
            float den = dx * ey - dy * ex;
            if (fabsf(den) < 1e-9f) continue;
            float wx = a.x - b->center.x, wy = a.y - b->center.y;
            float s = (wx * ey - wy * ex) / den;       // along the ray
            float tt = (wx * dy - wy * dx) / den;      // along the edge
            if (s > 1e-6f && tt >= -1e-4f && tt <= 1.0001f && s < best) { best = s; bi = i; bt = tt; }
        }
        if (best > 1e29f) return 0;
        float r = 1.0f / best; // ray parameter 1 = q; boundary at best
        if (r > 1.0f) return 0;
        uint32_t sc = b->ngcol <= 1 ? b->gcol[0] : lerp_col(b->gcol[bi % b->ngcol], b->gcol[(bi + 1) % b->ngcol], bt);
        float t = r;
        if (b->nblend > 0) {
            float f = b->bfac[b->nblend - 1];
            for (int i = 1; i < b->nblend; i++) if (t <= b->bpos[i]) { float span = b->bpos[i] - b->bpos[i - 1]; f = span > 0 ? b->bfac[i - 1] + (b->bfac[i] - b->bfac[i - 1]) * (t - b->bpos[i - 1]) / span : b->bfac[i]; break; }
            if (t <= b->bpos[0]) f = b->bfac[0];
            t = f;
        }
        return lerp_col(b->color, sc, t);
    }
    default: return b->color;
    }
}

static int paint_from_brush(Graphics *g, const Brush *b, Paint *p) {
    memset(p, 0, sizeof *p);
    if (!b) return 0;
    if (b->type == BR_SOLID) { p->type = P_SOLID; p->col = b->color; return 1; }
    p->type = P_BRUSH; p->b = b;
    Mat bx = mmul(b->xf, gtotal(g));
    if (b->type == BR_HATCH) bx = M_ID;
    if (!minv(&bx, &p->inv)) return 0;
    if (b->type == BR_TEXTURE) p->bilinear = !(fabsf(bx.a - 1) < 1e-4f && fabsf(bx.d - 1) < 1e-4f && fabsf(bx.b) < 1e-4f && fabsf(bx.c) < 1e-4f && fabsf(bx.e - roundf(bx.e)) < 1e-4f && fabsf(bx.f - roundf(bx.f)) < 1e-4f);
    return 1;
}

// ---- compositing -----------------------------------------------------------------------------------------------------------

static inline uint32_t blend_over(uint32_t d, uint32_t s, uint32_t cov) {
    uint32_t sa = (s >> 24) * cov;
    sa = (sa + 127) / 255;
    if (!sa) return d;
    uint32_t da = d >> 24;
    if (sa == 255) return s | 0xFF000000u;
    if (da == 255) {
        uint32_t o = 0xFF000000u;
        for (int k = 0; k < 24; k += 8) o |= ((((s >> k) & 255) * sa + ((d >> k) & 255) * (255 - sa) + 127) / 255) << k;
        return o;
    }
    if (!da) return (s & 0xFFFFFFu) | (sa << 24);
    uint32_t oa = sa * 255 + da * (255 - sa); // scaled by 255
    uint32_t o = ((oa + 127) / 255) << 24;
    for (int k = 0; k < 24; k += 8) {
        uint32_t v = (((s >> k) & 255) * sa * 255 + ((d >> k) & 255) * da * (255 - sa) + oa / 2) / (oa ? oa : 1);
        o |= (v > 255 ? 255 : v) << k;
    }
    return o;
}
static inline uint32_t blend_copy(uint32_t d, uint32_t s, uint32_t cov) {
    if (cov >= 255) return s;
    if (!cov) return d;
    uint32_t o = 0;
    for (int k = 0; k < 32; k += 8) o |= ((((s >> k) & 255) * cov + ((d >> k) & 255) * (255 - cov) + 127) / 255) << k;
    return o;
}

// Paints the mask with the paint source: clip, composite into the target.
static void composite(Graphics *g, const Mask *m, const Paint *p, int force_copy) {
    int x0 = m->x0, y0 = m->y0, x1 = m->x0 + m->w, y1 = m->y0 + m->h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g->w) x1 = g->w;
    if (y1 > g->h) y1 = g->h;
    if (x1 <= x0 || y1 <= y0) return;
    int w = x1 - x0, h = y1 - y0;
    uint8_t *cm = NULL;
    if (g->clip->type != RG_INF) {
        cm = malloc((size_t)w * (size_t)h);
        if (!cm) return;
        region_mask(g->clip, x0, y0, w, h, cm);
    }
    int copy = force_copy || g->comp_mode == 1;
    uint32_t *layer = NULL;
    if (!g->img) { layer = calloc((size_t)w * (size_t)h, 4); if (!layer) { free(cm); return; } }
    for (int y = 0; y < h; y++) {
        const uint8_t *mrow = m->a + (size_t)(y0 + y - m->y0) * (size_t)m->w + (size_t)(x0 - m->x0);
        const uint8_t *crow = cm ? cm + (size_t)y * (size_t)w : NULL;
        uint32_t *drow = g->img ? g->img->px + (size_t)(y0 + y) * (size_t)g->img->w + (size_t)x0 : layer + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t cov = mrow[x];
            if (!cov) continue;
            if (crow) { cov = cov * crow[x] / 255; if (!cov) continue; }
            uint32_t s = paint_px(p, x0 + x, y0 + y);
            if (!g->img && copy && cov == 255) { drow[x] = s; continue; }
            drow[x] = copy ? blend_copy(drow[x], s, cov) : blend_over(drow[x], s, cov);
        }
    }
    if (layer) { gdi_draw_argb(g->hdc, x0, y0, layer, w, h); free(layer); }
    free(cm);
}

static int g_aa(Graphics *g) { return g->smoothing == 2 || g->smoothing >= 4; }
static float g_shift(Graphics *g) { return (g->pixoff == 2 || g->pixoff == 4) ? 0.0f : 0.5f; }

static void ps_xform(PolySet *ps, const Mat *m, float dx, float dy) {
    for (int i = 0; i < ps->n; i++)
        for (int k = 0; k < ps->s[i].n; k++) { Pt p = mpt(m, ps->s[i].p[k]); p.x += dx; p.y += dy; ps->s[i].p[k] = p; }
}
static void ps_orient(PolySet *ps) { // make every polygon positively oriented so that unions work with the non-zero rule
    for (int i = 0; i < ps->n; i++) {
        Poly *q = &ps->s[i];
        double a = 0;
        for (int k = 0; k < q->n; k++) { Pt p0 = q->p[k], p1 = q->p[(k + 1) % q->n]; a += (double)p0.x * p1.y - (double)p1.x * p0.y; }
        if (a < 0) for (int k = 0; k < q->n / 2; k++) { Pt t = q->p[k]; q->p[k] = q->p[q->n - 1 - k]; q->p[q->n - 1 - k] = t; }
    }
}

// Fills device-space polygons (already shifted) with a paint.
static void fill_dev(Graphics *g, const PolySet *ps, int evenodd, int aa, const Paint *p) {
    Mask m;
    if (!raster_polys(ps, evenodd, aa, 0, 0, g->w, g->h, &m)) return;
    composite(g, &m, p, 0);
    free(m.a);
}
static void fill_path_g(Graphics *g, const Path *path, const Brush *b) {
    Paint p;
    if (!paint_from_brush(g, b, &p)) return;
    Mat t = gtotal(g);
    PolySet ps; memset(&ps, 0, sizeof ps);
    path_flatten(path, &t, &ps);
    float sh = g_shift(g);
    ps_xform(&ps, &M_ID, sh, sh);
    fill_dev(g, &ps, path->fill == 0, g_aa(g), &p);
    ps_free(&ps);
}

// ---- stroking --------------------------------------------------------------------------------------------------------------

typedef struct StrokeParams {
    float hw;                // half width (world units)
    int start_cap, end_cap, join; float miter;
    int nseg;                // segments of a full circle
} StrokeParams;

static void add_circle(PolySet *out, Pt c, float r, int n) {
    Pt *pts = malloc(sizeof(Pt) * (size_t)n);
    if (!pts) return;
    for (int i = 0; i < n; i++) { float a = 2 * (float)M_PI * (float)i / (float)n; pts[i].x = c.x + r * cosf(a); pts[i].y = c.y + r * sinf(a); }
    ps_add_poly(out, pts, n, 1);
    free(pts);
}
static void add_tri(PolySet *out, Pt a, Pt b, Pt c) { Pt t[3] = { a, b, c }; ps_add_poly(out, t, 3, 1); }
static void add_quad(PolySet *out, Pt a, Pt b, Pt c, Pt d) { Pt t[4] = { a, b, c, d }; ps_add_poly(out, t, 4, 1); }

static void add_cap(PolySet *out, Pt p, Pt d /* unit, pointing away from the line */, int cap, float hw, int nseg) {
    Pt n = { -d.y, d.x };
    switch (cap & 15) {
    case 1: add_quad(out, (Pt){ p.x + n.x * hw, p.y + n.y * hw }, (Pt){ p.x + n.x * hw + d.x * hw, p.y + n.y * hw + d.y * hw },
                     (Pt){ p.x - n.x * hw + d.x * hw, p.y - n.y * hw + d.y * hw }, (Pt){ p.x - n.x * hw, p.y - n.y * hw }); break;
    case 2: add_circle(out, p, hw, nseg); break;
    case 3: add_tri(out, (Pt){ p.x + n.x * hw, p.y + n.y * hw }, (Pt){ p.x + d.x * hw, p.y + d.y * hw }, (Pt){ p.x - n.x * hw, p.y - n.y * hw }); break;
    default: break;
    }
}

static void stroke_poly(const Pt *in, int n0, int closed, const StrokeParams *sp, PolySet *out) {
    Pt *q = malloc(sizeof(Pt) * (size_t)(n0 + 1));
    if (!q) return;
    int n = 0;
    for (int i = 0; i < n0; i++) if (!n || in[i].x != q[n - 1].x || in[i].y != q[n - 1].y) q[n++] = in[i];
    if (closed && n > 1 && q[0].x == q[n - 1].x && q[0].y == q[n - 1].y) n--;
    float hw = sp->hw;
    if (n == 1) { // a dot
        if ((sp->start_cap & 15) == 2) add_circle(out, q[0], hw, sp->nseg);
        else if ((sp->start_cap & 15) == 1) add_quad(out, (Pt){ q[0].x - hw, q[0].y - hw }, (Pt){ q[0].x + hw, q[0].y - hw }, (Pt){ q[0].x + hw, q[0].y + hw }, (Pt){ q[0].x - hw, q[0].y + hw });
        free(q); return;
    }
    if (n < 2) { free(q); return; }
    int ns = closed ? n : n - 1;
    for (int i = 0; i < ns; i++) {
        Pt a = q[i], b = q[(i + 1) % n];
        float dx = b.x - a.x, dy = b.y - a.y, l = hypotf(dx, dy);
        if (l <= 0) continue;
        dx /= l; dy /= l;
        float nx = -dy * hw, ny = dx * hw;
        add_quad(out, (Pt){ a.x + nx, a.y + ny }, (Pt){ b.x + nx, b.y + ny }, (Pt){ b.x - nx, b.y - ny }, (Pt){ a.x - nx, a.y - ny });
    }
    // joins
    int j0 = closed ? 0 : 1, j1 = closed ? n : n - 1;
    for (int i = j0; i < j1; i++) {
        Pt pv = q[(i + n - 1) % n], v = q[i], nx = q[(i + 1) % n];
        float d0x = v.x - pv.x, d0y = v.y - pv.y, l0 = hypotf(d0x, d0y), d1x = nx.x - v.x, d1y = nx.y - v.y, l1 = hypotf(d1x, d1y);
        if (l0 <= 0 || l1 <= 0) continue;
        d0x /= l0; d0y /= l0; d1x /= l1; d1y /= l1;
        float cross = d0x * d1y - d0y * d1x, dot = d0x * d1x + d0y * d1y;
        if (fabsf(cross) < 1e-5f && dot > 0) continue;
        if (sp->join == 2) { add_circle(out, v, hw, sp->nseg); continue; }
        float s = cross > 0 ? -1.0f : 1.0f;
        Pt n0 = { -d0y, d0x }, n1 = { -d1y, d1x };
        Pt A = { v.x + s * hw * n0.x, v.y + s * hw * n0.y }, B = { v.x + s * hw * n1.x, v.y + s * hw * n1.y };
        float ratio = dot > -0.9999f ? sqrtf(2.0f / (1.0f + dot)) : 1e9f;
        if ((sp->join == 0 || sp->join == 3) && ratio <= sp->miter) {
            float k = s * hw / (1.0f + dot);
            Pt tip = { v.x + k * (n0.x + n1.x), v.y + k * (n0.y + n1.y) };
            add_quad(out, v, A, tip, B);
        } else add_tri(out, v, A, B);
    }
    if (!closed) {
        Pt d0 = { q[0].x - q[1].x, q[0].y - q[1].y };
        float l = hypotf(d0.x, d0.y); d0.x /= l; d0.y /= l;
        add_cap(out, q[0], d0, sp->start_cap, hw, sp->nseg);
        Pt d1 = { q[n - 1].x - q[n - 2].x, q[n - 1].y - q[n - 2].y };
        l = hypotf(d1.x, d1.y); d1.x /= l; d1.y /= l;
        add_cap(out, q[n - 1], d1, sp->end_cap, hw, sp->nseg);
    }
    free(q);
}

static void dash_poly(const Poly *src, const float *pat, int np, float off, PolySet *out) {
    int n = src->n + (src->closed ? 1 : 0);
    if (n < 2 || np < 1) { ps_add_poly(out, src->p, src->n, src->closed); return; }
    float total = 0;
    for (int i = 0; i < np; i++) total += pat[i];
    if (total <= 0) { ps_add_poly(out, src->p, src->n, src->closed); return; }
    int idx = 0; float rem;
    off = fmodf(off, total * ((np & 1) ? 2.0f : 1.0f)); if (off < 0) off += total;
    int on = 1;
    rem = pat[0];
    while (off > 0) {
        if (off >= rem) { off -= rem; idx = (idx + 1) % np; on = !on; rem = pat[idx]; }
        else { rem -= off; off = 0; }
    }
    Pt *cur = malloc(sizeof(Pt) * (size_t)(n * 64 + 4)); int cn = 0, ccap = n * 64 + 4;
    if (!cur) return;
    Pt pos = src->p[0];
    if (on) cur[cn++] = pos;
    for (int i = 1; i < n; i++) {
        Pt b = src->p[i % src->n];
        float dx = b.x - pos.x, dy = b.y - pos.y, l = hypotf(dx, dy), done = 0;
        while (l - done > rem) {
            done += rem;
            Pt p = { pos.x + dx * done / l, pos.y + dy * done / l };
            if (on) { if (cn < ccap) cur[cn++] = p; ps_add_poly(out, cur, cn, 0); cn = 0; }
            else { cn = 0; cur[cn++] = p; }
            on = !on; idx = (idx + 1) % np; rem = pat[idx] > 1e-4f ? pat[idx] : 1e-4f;
            if (out->n > 200000) { free(cur); return; }
        }
        rem -= l - done;
        pos = b;
        if (on && cn < ccap) cur[cn++] = pos;
    }
    if (on && cn >= 2) ps_add_poly(out, cur, cn, 0);
    free(cur);
}

static const float DASH_STD[5][6] = { { 0 }, { 3, 1 }, { 1, 1 }, { 3, 1, 1, 1 }, { 3, 1, 1, 1, 1, 1 } };
static const int DASH_N[5] = { 0, 2, 2, 4, 6 };

static void stroke_path_g(Graphics *g, const Path *path, const Pen *pen) {
    Brush tmp; Brush *b = pen->brush;
    if (!b) { memset(&tmp, 0, sizeof tmp); tmp.type = BR_SOLID; tmp.color = pen->color; b = &tmp; }
    Paint p;
    if (!paint_from_brush(g, b, &p)) return;
    Mat t = gtotal(g);
    float sc = mscale(&t);
    float width = pen->width * (pen->unit > 0 && pen->unit != 0 ? 1.0f : 1.0f);
    if (width * sc < 1.0f) width = 1.0f / sc;
    StrokeParams sp;
    sp.hw = width / 2; sp.start_cap = pen->start_cap; sp.end_cap = pen->end_cap; sp.join = pen->join; sp.miter = pen->miter < 1 ? 1 : pen->miter;
    int dc = pen->dash_cap == 0 ? 0 : (pen->dash_cap == 2 ? 2 : pen->dash_cap == 3 ? 3 : 0);
    float rdev = sp.hw * sc;
    sp.nseg = (int)(sqrtf(rdev) * 6) + 10; if (sp.nseg > 96) sp.nseg = 96;
    PolySet lines; memset(&lines, 0, sizeof lines);
    path_flatten(path, NULL, &lines);
    PolySet dashed; memset(&dashed, 0, sizeof dashed);
    PolySet *use = &lines;
    if (pen->dash_style != 0) {
        float pat[64]; int np;
        if (pen->dash_style == 5 && pen->ndash > 0) { np = pen->ndash > 64 ? 64 : pen->ndash; for (int i = 0; i < np; i++) pat[i] = pen->dash[i] * width; }
        else if (pen->dash_style >= 1 && pen->dash_style <= 4) { np = DASH_N[pen->dash_style]; for (int i = 0; i < np; i++) pat[i] = DASH_STD[pen->dash_style][i] * width; }
        else np = 0;
        if (np) {
            for (int i = 0; i < lines.n; i++) dash_poly(&lines.s[i], pat, np, pen->dash_off * width, &dashed);
            use = &dashed;
            sp.start_cap = sp.end_cap = dc;
        }
    }
    PolySet outp; memset(&outp, 0, sizeof outp);
    for (int i = 0; i < use->n; i++) {
        const Poly *q = &use->s[i];
        StrokeParams s2 = sp;
        if (q->closed && use == &lines) { s2.start_cap = s2.end_cap = 0; }
        stroke_poly(q->p, q->n, q->closed && use == &lines, &s2, &outp);
    }
    float sh = g_shift(g);
    ps_xform(&outp, &t, sh, sh);
    ps_orient(&outp);
    fill_dev(g, &outp, 0, g_aa(g), &p);
    ps_free(&outp); ps_free(&dashed); ps_free(&lines);
}

// ======================================================================================================================
// Flat API
// ======================================================================================================================

#define GET_INT(fn, T, k, expr) GP(fn, 2) { T *o = OB(T, 0, k); if (!o || !ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)(expr)); STATUS(Ok); }
#define SET_INT(fn, T, k, stmt) GP(fn, 2) { T *o = OB(T, 0, k); if (!o) STATUS(InvalidParameter); int v = IA(1); (void)v; stmt; STATUS(Ok); }
#define GET_FLT(fn, T, k, expr) GP(fn, 2) { T *o = OB(T, 0, k); if (!o || !ARG(1)) STATUS(InvalidParameter); wrf(ARG(1), (float)(expr)); STATUS(Ok); }
#define SET_FLT(fn, T, k, stmt) GP(fn, 2) { T *o = OB(T, 0, k); if (!o) STATUS(InvalidParameter); float v = FA(1); (void)v; stmt; STATUS(Ok); }
#define GRAPHICS(n) Graphics *g = OB(Graphics, n, K_GRAPHICS); if (!g) STATUS(InvalidParameter)

static int g_started; // number of GdiplusStartup calls
SOUL_PROCESS_VAR(g_started)

// ---- startup ----------------------------------------------------------------------------------------------------------

static uint64_t g_hook_thunk[2];
SOUL_PROCESS_VAR(g_hook_thunk)
static uint64_t make_thunk(HleFn fn, const char *name, int nargs) {
    uint32_t id = hle_thunk_for(g_proc, fn, "gdiplus", name);
    g_proc->thunks[id].argbytes = g_mode32 ? (uint16_t)(4 * nargs) : 0;
    return g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE;
}
GP(NotificationHook, 1) { if (ARG(0)) wr_ptr(ARG(0), 1); STATUS(Ok); }
GP(NotificationUnhook, 1) { STATUS(Ok); }

GP(GdiplusStartup, 3) {
    uint64_t tok = ARG(0), in = ARG(1), out = ARG(2);
    if (!tok || !in) STATUS(InvalidParameter);
    uint32_t ver = (uint32_t)mem_rd(in, 4);
    if (ver < 1 || ver > 2) STATUS(UnsupportedGdiplusVersion);
    const char *L = "4P44";
    uint32_t suppress = (uint32_t)gs_get(in, L, 2);
    if (suppress && out) {
        if (!g_hook_thunk[0]) { g_hook_thunk[0] = make_thunk(gp_NotificationHook, "NotificationHook", 1); g_hook_thunk[1] = make_thunk(gp_NotificationUnhook, "NotificationUnhook", 1); }
        wr_ptr(out, g_hook_thunk[0]);
        wr_ptr(out + PSZ, g_hook_thunk[1]);
    }
    g_started++;
    wr_ptr(tok, 0x1000 + (uint64_t)g_started);
    STATUS(Ok);
}
GP(GdiplusShutdown, 1) { if (g_started > 0) g_started--; RET(0); }
GP(GdipAlloc, 1) { RET(g_malloc((size_t)ARG(0) ? (size_t)ARG(0) : 1)); }
GP(GdipFree, 1) { g_free(PTR(0)); RET(0); }

// ---- graphics creation -------------------------------------------------------------------------------------------------

static void graphics_set_dpi(Graphics *g, uint64_t hdc) {
    (void)hdc;
    g->dpix = g->dpiy = 96.0f;
}
GP(GdipCreateFromHDC, 2) {
    uint64_t hdc = ARG(0), pp = ARG(1);
    if (!hdc || !pp || !dc_get(hdc)) STATUS(InvalidParameter);
    Graphics *g = graphics_new();
    if (!g) STATUS(OutOfMemory);
    g->hdc = hdc;
    hdc_extent(hdc, &g->w, &g->h);
    graphics_set_dpi(g, hdc);
    wr_ptr(pp, H2G(g));
    STATUS(Ok);
}
GP(GdipCreateFromHDC2, 3) {
    uint64_t hdc = ARG(0), pp = ARG(2);
    if (!hdc || !pp || !dc_get(hdc)) STATUS(InvalidParameter);
    Graphics *g = graphics_new();
    if (!g) STATUS(OutOfMemory);
    g->hdc = hdc;
    hdc_extent(hdc, &g->w, &g->h);
    wr_ptr(pp, H2G(g));
    STATUS(Ok);
}
GP(GdipCreateFromHWND, 2) {
    uint64_t hwnd = ARG(0), pp = ARG(1);
    if (!pp) STATUS(InvalidParameter);
    Win *w = hwnd ? win_get(hwnd) : NULL;
    if (hwnd && !w) STATUS(InvalidParameter);
    uint64_t hdc = gdi_create_dc(hwnd ? DC_WINDOW : DC_SCREEN, hwnd);
    if (!hdc) STATUS(OutOfMemory);
    Graphics *g = graphics_new();
    if (!g) { gdi_release_dc(hdc); STATUS(OutOfMemory); }
    g->hdc = hdc; g->hdc_owned = 1;
    hdc_extent(hdc, &g->w, &g->h);
    wr_ptr(pp, H2G(g));
    STATUS(Ok);
}
GP(GdipGetImageGraphicsContext, 2) {
    Image *im = OB(Image, 0, K_IMAGE);
    if (!im || !ARG(1)) STATUS(InvalidParameter);
    if (pf_bpp(im->pixfmt) <= 8 && im->pixfmt != PF_16GRAY) STATUS(GenericError); // no graphics on indexed images
    Graphics *g = graphics_new();
    if (!g) STATUS(OutOfMemory);
    g->img = im; g->w = im->w; g->h = im->h; g->dpix = im->dpix; g->dpiy = im->dpiy;
    im->gdc++;
    wr_ptr(ARG(1), H2G(g));
    STATUS(Ok);
}
GP(GdipDeleteGraphics, 1) { Graphics *g = OB(Graphics, 0, K_GRAPHICS); if (!g) STATUS(InvalidParameter); graphics_free(g); STATUS(Ok); }
GP(GdipGetDC, 2) {
    GRAPHICS(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    if (g->busy) STATUS(ObjectBusy);
    if (g->hdc) { g->busy = 1; wr_ptr(ARG(1), g->hdc); STATUS(Ok); }
    // image target: a memory DC holding a copy of the pixels
    uint64_t hdc = gdi_create_dc(DC_MEMORY, 0);
    uint64_t hb = gdi_bitmap_from_argb(g->img->px, g->img->w, g->img->h);
    Dc *dc = dc_get(hdc); GdiObj *bo = gdi_get(hb, GO_BITMAP);
    if (!hdc || !dc || !bo) STATUS(OutOfMemory);
    dc->bitmap = hb; bo->selected = 1;
    g->hdc = hdc; g->hdc_owned = 2; g->busy = 1;
    wr_ptr(ARG(1), hdc);
    STATUS(Ok);
}
GP(GdipReleaseDC, 2) {
    GRAPHICS(0);
    if (!g->busy) STATUS(InvalidParameter);
    g->busy = 0;
    if (g->hdc_owned == 2) { // copy the pixels back
        Dc *dc = dc_get(g->hdc);
        GdiObj *bo = dc ? gdi_get(dc->bitmap, GO_BITMAP) : NULL;
        if (bo && g->img) {
            for (int y = 0; y < g->img->h && y < bo->surf.h; y++)
                for (int x = 0; x < g->img->w && x < bo->surf.w; x++) g->img->px[(size_t)y * (size_t)g->img->w + (size_t)x] = surf_get(&bo->surf, x, y) | (g->img->px[(size_t)y * (size_t)g->img->w + (size_t)x] & 0xFF000000u);
            uint64_t hb = dc->bitmap;
            bo->selected = 0; dc->bitmap = 0;
            gdi_free(hb);
        }
        gdi_release_dc(g->hdc);
        g->hdc = 0; g->hdc_owned = 0;
    }
    STATUS(Ok);
}
GP(GdipGraphicsClear, 2) {
    GRAPHICS(0);
    Paint p; memset(&p, 0, sizeof p); p.type = P_SOLID; p.col = (uint32_t)ARG(1);
    uint8_t *full = malloc((size_t)g->w * (size_t)g->h);
    if (!full) STATUS(OutOfMemory);
    memset(full, 255, (size_t)g->w * (size_t)g->h);
    Mask m = { 0, 0, g->w, g->h, full };
    composite(g, &m, &p, 1);
    free(full);
    STATUS(Ok);
}
GP(GdipFlush, 2) { GRAPHICS(0); STATUS(Ok); }
GP(GdipSetRenderingOrigin, 3) { GRAPHICS(0); STATUS(Ok); }
GP(GdipGetRenderingOrigin, 3) { GRAPHICS(0); if (ARG(1)) mem_wr(ARG(1), 4, 0); if (ARG(2)) mem_wr(ARG(2), 4, 0); STATUS(Ok); }
GP(GdipComment, 3) { GRAPHICS(0); STATUS(Ok); }
GP(GdipGetNearestColor, 2) { GRAPHICS(0); STATUS(Ok); }
GP(GdipTestControl, 2) { STATUS(NotImplemented); }

// ---- graphics state ----------------------------------------------------------------------------------------------------

SET_INT(GdipSetSmoothingMode, Graphics, K_GRAPHICS, o->smoothing = v == 0 ? 0 : v)
GET_INT(GdipGetSmoothingMode, Graphics, K_GRAPHICS, o->smoothing)
SET_INT(GdipSetPixelOffsetMode, Graphics, K_GRAPHICS, o->pixoff = v)
GET_INT(GdipGetPixelOffsetMode, Graphics, K_GRAPHICS, o->pixoff)
SET_INT(GdipSetInterpolationMode, Graphics, K_GRAPHICS, o->interp = v)
GET_INT(GdipGetInterpolationMode, Graphics, K_GRAPHICS, o->interp == 0 ? 0 : o->interp)
SET_INT(GdipSetCompositingMode, Graphics, K_GRAPHICS, o->comp_mode = v)
GET_INT(GdipGetCompositingMode, Graphics, K_GRAPHICS, o->comp_mode)
SET_INT(GdipSetCompositingQuality, Graphics, K_GRAPHICS, o->comp_quality = v)
GET_INT(GdipGetCompositingQuality, Graphics, K_GRAPHICS, o->comp_quality)
SET_INT(GdipSetTextRenderingHint, Graphics, K_GRAPHICS, o->texthint = v)
GET_INT(GdipGetTextRenderingHint, Graphics, K_GRAPHICS, o->texthint)
SET_INT(GdipSetTextContrast, Graphics, K_GRAPHICS, o->textcontrast = v)
GET_INT(GdipGetTextContrast, Graphics, K_GRAPHICS, o->textcontrast)
GP(GdipSetPageUnit, 2) {
    GRAPHICS(0);
    int u = IA(1);
    if (u < 1 || u > 6) STATUS(InvalidParameter);
    g->unit = u;
    STATUS(Ok);
}
GET_INT(GdipGetPageUnit, Graphics, K_GRAPHICS, o->unit)
GP(GdipSetPageScale, 2) { GRAPHICS(0); float s = FA(1); if (s <= 0) STATUS(InvalidParameter); g->pscale = s; STATUS(Ok); }
GET_FLT(GdipGetPageScale, Graphics, K_GRAPHICS, o->pscale)
GET_FLT(GdipGetDpiX, Graphics, K_GRAPHICS, o->dpix)
GET_FLT(GdipGetDpiY, Graphics, K_GRAPHICS, o->dpiy)

// ---- world transform ------------------------------------------------------------------------------------------------------

static void mat_write(uint64_t a, const Mat *m) { wrf(a, m->a); wrf(a + 4, m->b); wrf(a + 8, m->c); wrf(a + 12, m->d); wrf(a + 16, m->e); wrf(a + 20, m->f); }

typedef struct MatObj { int kind; Mat m; } MatObj;
#define MATRIX(n) MatObj *mo = OB(MatObj, n, K_MATRIX); if (!mo) STATUS(InvalidParameter)

static Mat apply_order(Mat cur, Mat op, int order) { return order == 1 ? mmul(cur, op) : mmul(op, cur); } // order: Prepend 0, Append 1

GP(GdipSetWorldTransform, 2) { GRAPHICS(0); MATRIX(1); if (mdet(&mo->m) == 0) STATUS(InvalidParameter); g->world = mo->m; STATUS(Ok); }
GP(GdipGetWorldTransform, 2) { GRAPHICS(0); MATRIX(1); mo->m = g->world; STATUS(Ok); }
GP(GdipResetWorldTransform, 1) { GRAPHICS(0); g->world = M_ID; STATUS(Ok); }
GP(GdipMultiplyWorldTransform, 3) { GRAPHICS(0); MATRIX(1); g->world = apply_order(g->world, mo->m, IA(2)); STATUS(Ok); }
GP(GdipTranslateWorldTransform, 4) { GRAPHICS(0); g->world = apply_order(g->world, mat_translate(FA(1), FA(2)), IA(3)); STATUS(Ok); }
GP(GdipScaleWorldTransform, 4) { GRAPHICS(0); g->world = apply_order(g->world, mat_scale(FA(1), FA(2)), IA(3)); STATUS(Ok); }
GP(GdipRotateWorldTransform, 3) { GRAPHICS(0); g->world = apply_order(g->world, mat_rotate(FA(1)), IA(2)); STATUS(Ok); }

// coordinate spaces: World 0, Page 1, Device 2
static int space_matrix(Graphics *g, int dst, int src, Mat *m) {
    if (dst < 0 || dst > 2 || src < 0 || src > 2) return 0;
    Mat toDev = M_ID; // src -> device
    if (src == 0) toDev = gtotal(g); else if (src == 1) toDev = gpage(g);
    Mat fromDev = M_ID;
    if (dst == 0) { Mat t = gtotal(g); if (!minv(&t, &fromDev)) return 0; }
    else if (dst == 1) { Mat t = gpage(g); if (!minv(&t, &fromDev)) return 0; }
    *m = mmul(toDev, fromDev);
    return 1;
}
static void transform_points(X86State *st, int is_int) {
    Graphics *g = OB(Graphics, 0, K_GRAPHICS);
    uint64_t pts = ARG(3); int n = IA(4);
    Mat m;
    if (!g || !pts || n <= 0 || !space_matrix(g, IA(1), IA(2), &m)) STATUS(InvalidParameter);
    for (int i = 0; i < n; i++) {
        Pt p;
        if (is_int) { p.x = (float)rdi(pts + (uint64_t)i * 8); p.y = (float)rdi(pts + (uint64_t)i * 8 + 4); }
        else { p.x = rdf(pts + (uint64_t)i * 8); p.y = rdf(pts + (uint64_t)i * 8 + 4); }
        p = mpt(&m, p);
        if (is_int) { mem_wr(pts + (uint64_t)i * 8, 4, (uint32_t)(int32_t)lrintf(p.x)); mem_wr(pts + (uint64_t)i * 8 + 4, 4, (uint32_t)(int32_t)lrintf(p.y)); }
        else { wrf(pts + (uint64_t)i * 8, p.x); wrf(pts + (uint64_t)i * 8 + 4, p.y); }
    }
    STATUS(Ok);
}
GP(GdipTransformPoints, 5) { transform_points(st, 0); }
GP(GdipTransformPointsI, 5) { transform_points(st, 1); }

// ---- clipping -------------------------------------------------------------------------------------------------------------

static void clip_apply(Graphics *g, Region *r, int mode) { g->clip = region_combine(g->clip, r, mode); }
static void clip_rect(Graphics *g, float x, float y, float w, float h, int mode) {
    Mat t = gtotal(g);
    Region *r = region_from_rect(x, y, w, h, &t);
    if (r) clip_apply(g, r, mode);
}
GP(GdipSetClipRect, 6) { GRAPHICS(0); int mode = IA(5); if (mode < 0 || mode > 5) STATUS(InvalidParameter); clip_rect(g, FA(1), FA(2), FA(3), FA(4), mode); STATUS(Ok); }
GP(GdipSetClipRectI, 6) { GRAPHICS(0); int mode = IA(5); if (mode < 0 || mode > 5) STATUS(InvalidParameter); clip_rect(g, (float)IA(1), (float)IA(2), (float)IA(3), (float)IA(4), mode); STATUS(Ok); }
GP(GdipSetClipPath, 3) {
    GRAPHICS(0);
    Path *p = OB(Path, 1, K_PATH);
    if (!p) STATUS(InvalidParameter);
    Mat t = gtotal(g);
    Region *r = region_from_path(p, &t);
    if (r) clip_apply(g, r, IA(2));
    STATUS(Ok);
}
GP(GdipSetClipRegion, 3) {
    GRAPHICS(0);
    Region *src = OB(Region, 1, K_REGION);
    if (!src) STATUS(InvalidParameter);
    Region *r = region_clone(src);
    Mat t = gtotal(g);
    region_xform(r, &t);
    clip_apply(g, r, IA(2));
    STATUS(Ok);
}
GP(GdipSetClipGraphics, 3) {
    GRAPHICS(0);
    Graphics *s = OB(Graphics, 1, K_GRAPHICS);
    if (!s) STATUS(InvalidParameter);
    clip_apply(g, region_clone(s->clip), IA(2));
    STATUS(Ok);
}
GP(GdipSetClipHrgn, 3) {
    GRAPHICS(0);
    GdiObj *o = gdi_get(ARG(1), GO_REGION);
    if (!o) STATUS(InvalidParameter);
    clip_apply(g, region_from_rect((float)o->rx0, (float)o->ry0, (float)(o->rx1 - o->rx0), (float)(o->ry1 - o->ry0), NULL), IA(2));
    STATUS(Ok);
}
GP(GdipResetClip, 1) { GRAPHICS(0); region_free(g->clip); g->clip = region_new(RG_INF); STATUS(Ok); }
static void clip_translate(Graphics *g, float dx, float dy) {
    Mat t = gtotal(g);
    Pt v = { dx * t.a + dy * t.c, dx * t.b + dy * t.d };
    Mat m = mat_translate(v.x, v.y);
    region_xform(g->clip, &m);
}
GP(GdipTranslateClip, 3) { GRAPHICS(0); clip_translate(g, FA(1), FA(2)); STATUS(Ok); }
GP(GdipTranslateClipI, 3) { GRAPHICS(0); clip_translate(g, (float)IA(1), (float)IA(2)); STATUS(Ok); }
GP(GdipGetClip, 2) {
    GRAPHICS(0);
    Region *dst = OB(Region, 1, K_REGION);
    if (!dst) STATUS(InvalidParameter);
    Region *c = region_clone(g->clip);
    Mat t = gtotal(g), inv;
    if (minv(&t, &inv)) region_xform(c, &inv);
    path_free(dst->path); region_free(dst->l); region_free(dst->r);
    dst->type = c->type; dst->path = c->path; dst->l = c->l; dst->r = c->r; dst->mode = c->mode;
    odel(c);
    STATUS(Ok);
}
static int clip_bounds_world(Graphics *g, int visible, float *x, float *y, float *w, float *h) {
    float x0, y0, x1, y1;
    int r = region_bounds(g->clip, &x0, &y0, &x1, &y1);
    if (r == -1) {
        if (!visible) { *x = -4194304; *y = -4194304; *w = 8388608; *h = 8388608; return 1; }
        x0 = 0; y0 = 0; x1 = (float)g->w; y1 = (float)g->h;
    } else if (r == 0) { *x = *y = *w = *h = 0; return 1; }
    else if (visible) { if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > (float)g->w) x1 = (float)g->w; if (y1 > (float)g->h) y1 = (float)g->h; if (x1 < x0 || y1 < y0) { *x = *y = *w = *h = 0; return 1; } }
    Mat t = gtotal(g), inv;
    if (!minv(&t, &inv)) return 0;
    Pt c[4] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    float a = 1e30f, b = 1e30f, cc = -1e30f, d = -1e30f;
    for (int i = 0; i < 4; i++) { Pt p = mpt(&inv, c[i]); if (p.x < a) a = p.x; if (p.x > cc) cc = p.x; if (p.y < b) b = p.y; if (p.y > d) d = p.y; }
    *x = a; *y = b; *w = cc - a; *h = d - b;
    return 1;
}
static void write_rectf(uint64_t a, float x, float y, float w, float h) { wrf(a, x); wrf(a + 4, y); wrf(a + 8, w); wrf(a + 12, h); }
static void write_recti(uint64_t a, float x, float y, float w, float h) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    mem_wr(a, 4, (uint32_t)x0); mem_wr(a + 4, 4, (uint32_t)y0); mem_wr(a + 8, 4, (uint32_t)(x1 - x0)); mem_wr(a + 12, 4, (uint32_t)(y1 - y0));
}
static void clip_bounds_api(X86State *st, int visible, int is_int) {
    Graphics *g = OB(Graphics, 0, K_GRAPHICS);
    float x, y, w, h;
    if (!g || !ARG(1) || !clip_bounds_world(g, visible, &x, &y, &w, &h)) STATUS(InvalidParameter);
    if (is_int) write_recti(ARG(1), x, y, w, h); else write_rectf(ARG(1), x, y, w, h);
    STATUS(Ok);
}
GP(GdipGetClipBounds, 2) { clip_bounds_api(st, 0, 0); }
GP(GdipGetClipBoundsI, 2) { clip_bounds_api(st, 0, 1); }
GP(GdipGetVisibleClipBounds, 2) { clip_bounds_api(st, 1, 0); }
GP(GdipGetVisibleClipBoundsI, 2) { clip_bounds_api(st, 1, 1); }
GP(GdipIsClipEmpty, 2) {
    GRAPHICS(0);
    float a, b, c, d;
    if (!ARG(1)) STATUS(InvalidParameter);
    mem_wr(ARG(1), 4, region_bounds(g->clip, &a, &b, &c, &d) == 0);
    STATUS(Ok);
}
GP(GdipIsVisibleClipEmpty, 2) {
    GRAPHICS(0);
    float x, y, w, h;
    if (!ARG(1) || !clip_bounds_world(g, 1, &x, &y, &w, &h)) STATUS(InvalidParameter);
    mem_wr(ARG(1), 4, w <= 0 || h <= 0);
    STATUS(Ok);
}
static int g_visible_pt(Graphics *g, float x, float y) {
    Mat t = gtotal(g);
    Pt p = mpt(&t, (Pt){ x, y });
    if (p.x < 0 || p.y < 0 || p.x >= (float)g->w || p.y >= (float)g->h) return 0;
    return region_has(g->clip, p.x, p.y);
}
GP(GdipIsVisiblePoint, 4) { GRAPHICS(0); if (!ARG(3)) STATUS(InvalidParameter); mem_wr(ARG(3), 4, g_visible_pt(g, FA(1), FA(2))); STATUS(Ok); }
GP(GdipIsVisiblePointI, 4) { GRAPHICS(0); if (!ARG(3)) STATUS(InvalidParameter); mem_wr(ARG(3), 4, g_visible_pt(g, (float)IA(1), (float)IA(2))); STATUS(Ok); }
static int g_visible_rect(Graphics *g, float x, float y, float w, float h) {
    Mat t = gtotal(g);
    PolySet ps; memset(&ps, 0, sizeof ps);
    Path *p = path_new(0); path_rect(p, x, y, w, h); path_xform(p, &t);
    path_flatten(p, NULL, &ps);
    Mask m; int vis = 0;
    if (raster_polys(&ps, 0, 0, 0, 0, g->w, g->h, &m)) {
        uint8_t *cm = malloc((size_t)m.w * (size_t)m.h);
        if (cm) {
            region_mask(g->clip, m.x0, m.y0, m.w, m.h, cm);
            for (int i = 0; i < m.w * m.h; i++) if (m.a[i] && cm[i]) { vis = 1; break; }
            free(cm);
        }
        free(m.a);
    }
    ps_free(&ps); path_free(p);
    return vis;
}
GP(GdipIsVisibleRect, 6) { GRAPHICS(0); if (!ARG(5)) STATUS(InvalidParameter); mem_wr(ARG(5), 4, g_visible_rect(g, FA(1), FA(2), FA(3), FA(4))); STATUS(Ok); }
GP(GdipIsVisibleRectI, 6) { GRAPHICS(0); if (!ARG(5)) STATUS(InvalidParameter); mem_wr(ARG(5), 4, g_visible_rect(g, (float)IA(1), (float)IA(2), (float)IA(3), (float)IA(4))); STATUS(Ok); }

// ---- save / restore -----------------------------------------------------------------------------------------------------

static int g_push(Graphics *g) {
    if (g->nstack == g->cap) {
        int nc = g->cap ? g->cap * 2 : 8;
        GState *ns = g_realloc(g->stack, sizeof(GState) * (size_t)nc);
        if (!ns) return 0;
        g->stack = ns; g->cap = nc;
    }
    GState *s = &g->stack[g->nstack++];
    s->id = ++g->next_id; s->world = g->world; s->clip = region_clone(g->clip);
    s->smoothing = g->smoothing; s->pixoff = g->pixoff; s->interp = g->interp; s->comp_mode = g->comp_mode; s->comp_quality = g->comp_quality;
    s->texthint = g->texthint; s->textcontrast = g->textcontrast; s->unit = g->unit; s->pscale = g->pscale;
    return s->id;
}
static int g_pop(Graphics *g, int id) {
    int idx = -1;
    for (int i = g->nstack - 1; i >= 0; i--) if (g->stack[i].id == id) { idx = i; break; }
    if (idx < 0) return 0;
    GState *s = &g->stack[idx];
    g->world = s->world; region_free(g->clip); g->clip = s->clip; s->clip = NULL;
    g->smoothing = s->smoothing; g->pixoff = s->pixoff; g->interp = s->interp; g->comp_mode = s->comp_mode; g->comp_quality = s->comp_quality;
    g->texthint = s->texthint; g->textcontrast = s->textcontrast; g->unit = s->unit; g->pscale = s->pscale;
    for (int i = idx + 1; i < g->nstack; i++) region_free(g->stack[i].clip);
    for (int i = idx; i < g->nstack; i++) if (g->stack[i].clip && i != idx) region_free(g->stack[i].clip);
    g->nstack = idx;
    return 1;
}
GP(GdipSaveGraphics, 2) { GRAPHICS(0); if (!ARG(1)) STATUS(InvalidParameter); int id = g_push(g); if (!id) STATUS(OutOfMemory); mem_wr(ARG(1), 4, (uint32_t)id); STATUS(Ok); }
GP(GdipRestoreGraphics, 2) { GRAPHICS(0); if (!g_pop(g, IA(1))) STATUS(InvalidParameter); STATUS(Ok); }
GP(GdipBeginContainer2, 2) { GRAPHICS(0); if (!ARG(1)) STATUS(InvalidParameter); int id = g_push(g); if (!id) STATUS(OutOfMemory); mem_wr(ARG(1), 4, (uint32_t)id); STATUS(Ok); }
GP(GdipEndContainer, 2) { GRAPHICS(0); if (!g_pop(g, IA(1))) STATUS(InvalidParameter); STATUS(Ok); }
static void begin_container(X86State *st, int is_int) {
    Graphics *g = OB(Graphics, 0, K_GRAPHICS);
    uint64_t dr = ARG(1), sr = ARG(2); int unit = IA(3);
    if (!g || !dr || !sr || !ARG(4)) STATUS(InvalidParameter);
    float d[4], s[4];
    for (int i = 0; i < 4; i++) { d[i] = is_int ? (float)rdi(dr + 4 * (uint64_t)i) : rdf(dr + 4 * (uint64_t)i); s[i] = is_int ? (float)rdi(sr + 4 * (uint64_t)i) : rdf(sr + 4 * (uint64_t)i); }
    int id = g_push(g);
    if (!id) STATUS(OutOfMemory);
    // source rectangle (in `unit`) is mapped to the destination rectangle (current units)
    float k = 1;
    if (unit >= 3 && unit <= 6) k = g->dpix / unit_per_inch(g, unit); // source units -> pixels
    float sx = s[2] != 0 ? d[2] / (s[2] * k) : 1, sy = s[3] != 0 ? d[3] / (s[3] * k) : 1;
    Mat m = { sx * k, 0, 0, sy * k, d[0] - s[0] * k * sx, d[1] - s[1] * k * sy };
    (void)m;
    Mat inner = { sx, 0, 0, sy, d[0] - s[0] * sx, d[1] - s[1] * sy };
    if (unit == 2 || unit == 0 || unit == 1) g->world = mmul(inner, g->world);
    mem_wr(ARG(4), 4, (uint32_t)id);
    STATUS(Ok);
}
GP(GdipBeginContainer, 5) { begin_container(st, 0); }
GP(GdipBeginContainerI, 5) { begin_container(st, 1); }

// ---- matrices -----------------------------------------------------------------------------------------------------------

GP(GdipCreateMatrix, 1) { MatObj *o = NEWOBJ(MatObj, K_MATRIX); if (!o || !ARG(0)) STATUS(InvalidParameter); o->m = M_ID; wr_ptr(ARG(0), H2G(o)); STATUS(Ok); }
GP(GdipCreateMatrix2, 7) {
    if (!ARG(6)) STATUS(InvalidParameter);
    MatObj *o = NEWOBJ(MatObj, K_MATRIX);
    if (!o) STATUS(OutOfMemory);
    o->m = (Mat){ FA(0), FA(1), FA(2), FA(3), FA(4), FA(5) };
    wr_ptr(ARG(6), H2G(o));
    STATUS(Ok);
}
static void create_matrix3(X86State *st, int is_int) {
    uint64_t r = ARG(0), pl = ARG(1);
    if (!r || !pl || !ARG(2)) STATUS(InvalidParameter);
    float rc[4]; Pt p[3];
    for (int i = 0; i < 4; i++) rc[i] = is_int ? (float)rdi(r + 4 * (uint64_t)i) : rdf(r + 4 * (uint64_t)i);
    for (int i = 0; i < 3; i++) { p[i].x = is_int ? (float)rdi(pl + 8 * (uint64_t)i) : rdf(pl + 8 * (uint64_t)i); p[i].y = is_int ? (float)rdi(pl + 8 * (uint64_t)i + 4) : rdf(pl + 8 * (uint64_t)i + 4); }
    if (rc[2] == 0 || rc[3] == 0) STATUS(InvalidParameter);
    MatObj *o = NEWOBJ(MatObj, K_MATRIX);
    if (!o) STATUS(OutOfMemory);
    float a = (p[1].x - p[0].x) / rc[2], b = (p[1].y - p[0].y) / rc[2], c = (p[2].x - p[0].x) / rc[3], d = (p[2].y - p[0].y) / rc[3];
    o->m = (Mat){ a, b, c, d, p[0].x - rc[0] * a - rc[1] * c, p[0].y - rc[0] * b - rc[1] * d };
    wr_ptr(ARG(2), H2G(o));
    STATUS(Ok);
}
GP(GdipCreateMatrix3, 3) { create_matrix3(st, 0); }
GP(GdipCreateMatrix3I, 3) { create_matrix3(st, 1); }
GP(GdipCloneMatrix, 2) { MATRIX(0); MatObj *o = NEWOBJ(MatObj, K_MATRIX); if (!o || !ARG(1)) STATUS(InvalidParameter); o->m = mo->m; wr_ptr(ARG(1), H2G(o)); STATUS(Ok); }
GP(GdipDeleteMatrix, 1) { MATRIX(0); odel(mo); STATUS(Ok); }
GP(GdipSetMatrixElements, 7) { MATRIX(0); mo->m = (Mat){ FA(1), FA(2), FA(3), FA(4), FA(5), FA(6) }; STATUS(Ok); }
GP(GdipMultiplyMatrix, 3) { MATRIX(0); MatObj *o2 = OB(MatObj, 1, K_MATRIX); if (!o2) STATUS(InvalidParameter); mo->m = apply_order(mo->m, o2->m, IA(2)); STATUS(Ok); }
GP(GdipTranslateMatrix, 4) { MATRIX(0); mo->m = apply_order(mo->m, mat_translate(FA(1), FA(2)), IA(3)); STATUS(Ok); }
GP(GdipScaleMatrix, 4) { MATRIX(0); mo->m = apply_order(mo->m, mat_scale(FA(1), FA(2)), IA(3)); STATUS(Ok); }
GP(GdipRotateMatrix, 3) { MATRIX(0); mo->m = apply_order(mo->m, mat_rotate(FA(1)), IA(2)); STATUS(Ok); }
GP(GdipShearMatrix, 4) { MATRIX(0); mo->m = apply_order(mo->m, mat_shear(FA(1), FA(2)), IA(3)); STATUS(Ok); }
GP(GdipInvertMatrix, 1) { MATRIX(0); Mat r; if (!minv(&mo->m, &r)) STATUS(InvalidParameter); mo->m = r; STATUS(Ok); }
static void mat_points(X86State *st, int vector, int is_int) {
    MatObj *mo = OB(MatObj, 0, K_MATRIX);
    uint64_t pts = ARG(1); int n = IA(2);
    if (!mo || !pts || n < 0) STATUS(InvalidParameter);
    Mat m = mo->m;
    if (vector) { m.e = 0; m.f = 0; }
    for (int i = 0; i < n; i++) {
        Pt p;
        if (is_int) { p.x = (float)rdi(pts + (uint64_t)i * 8); p.y = (float)rdi(pts + (uint64_t)i * 8 + 4); }
        else { p.x = rdf(pts + (uint64_t)i * 8); p.y = rdf(pts + (uint64_t)i * 8 + 4); }
        p = mpt(&m, p);
        if (is_int) { mem_wr(pts + (uint64_t)i * 8, 4, (uint32_t)(int32_t)lrintf(p.x)); mem_wr(pts + (uint64_t)i * 8 + 4, 4, (uint32_t)(int32_t)lrintf(p.y)); }
        else { wrf(pts + (uint64_t)i * 8, p.x); wrf(pts + (uint64_t)i * 8 + 4, p.y); }
    }
    STATUS(Ok);
}
GP(GdipTransformMatrixPoints, 3) { mat_points(st, 0, 0); }
GP(GdipTransformMatrixPointsI, 3) { mat_points(st, 0, 1); }
GP(GdipVectorTransformMatrixPoints, 3) { mat_points(st, 1, 0); }
GP(GdipVectorTransformMatrixPointsI, 3) { mat_points(st, 1, 1); }
GP(GdipGetMatrixElements, 2) { MATRIX(0); if (!ARG(1)) STATUS(InvalidParameter); mat_write(ARG(1), &mo->m); STATUS(Ok); }
GP(GdipIsMatrixInvertible, 2) { MATRIX(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, fabsf(mdet(&mo->m)) > 1e-12f); STATUS(Ok); }
GP(GdipIsMatrixIdentity, 2) { MATRIX(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, mat_is_ident(&mo->m)); STATUS(Ok); }
GP(GdipIsMatrixEqual, 3) { MATRIX(0); MatObj *o2 = OB(MatObj, 1, K_MATRIX); if (!o2 || !ARG(2)) STATUS(InvalidParameter); mem_wr(ARG(2), 4, memcmp(&mo->m, &o2->m, sizeof(Mat)) == 0); STATUS(Ok); }

// ---- helpers for the drawing functions ----------------------------------------------------------------------------------

#define AN(i) (ii ? (float)IA(i) : FA(i))
#define PAIR(name, n) static void name##_impl(X86State *st, int ii); GP(name, n) { name##_impl(st, 0); } GP(name##I, n) { name##_impl(st, 1); } static void name##_impl(X86State *st, int ii)
#define PEN(n) Pen *pen = OB(Pen, n, K_PEN); if (!pen) STATUS(InvalidParameter)
#define BRUSH(n) Brush *br = OB(Brush, n, K_BRUSH); if (!br) STATUS(InvalidParameter)
#define PATH(n) Path *pa = OB(Path, n, K_PATH); if (!pa) STATUS(InvalidParameter)

static Pt *rd_pts(uint64_t a, int n, int is_int) {
    if (!a || n <= 0 || n > (1 << 24)) return NULL;
    Pt *p = malloc(sizeof(Pt) * (size_t)n);
    if (!p) return NULL;
    for (int i = 0; i < n; i++) {
        p[i].x = is_int ? (float)rdi(a + (uint64_t)i * 8) : rdf(a + (uint64_t)i * 8);
        p[i].y = is_int ? (float)rdi(a + (uint64_t)i * 8 + 4) : rdf(a + (uint64_t)i * 8 + 4);
    }
    return p;
}
static void wr_pts(uint64_t a, const Pt *p, int n, int is_int) {
    for (int i = 0; i < n; i++) {
        if (is_int) { mem_wr(a + (uint64_t)i * 8, 4, (uint32_t)(int32_t)lrintf(p[i].x)); mem_wr(a + (uint64_t)i * 8 + 4, 4, (uint32_t)(int32_t)lrintf(p[i].y)); }
        else { wrf(a + (uint64_t)i * 8, p[i].x); wrf(a + (uint64_t)i * 8 + 4, p[i].y); }
    }
}
static void path_poly(Path *p, const Pt *q, int n, int closed) {
    for (int i = 0; i < n; i++) path_add(p, q[i].x, q[i].y, i == 0 ? PT_START : PT_LINE);
    if (closed) path_close(p);
}
// runs fn with a temporary path
#define TMPPATH(p, fill) Path *p = path_new(fill); if (!p) STATUS(OutOfMemory)

// ---- drawing -----------------------------------------------------------------------------------------------------------------

PAIR(GdipDrawLine, 6) {
    GRAPHICS(0); PEN(1);
    TMPPATH(p, 0);
    path_add(p, AN(2), AN(3), PT_START); path_add(p, AN(4), AN(5), PT_LINE);
    stroke_path_g(g, p, pen);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipDrawLines, 4) {
    GRAPHICS(0); PEN(1);
    int n = IA(3);
    Pt *q = rd_pts(ARG(2), n, ii);
    if (!q || n < 2) { free(q); STATUS(InvalidParameter); }
    TMPPATH(p, 0);
    path_poly(p, q, n, 0);
    stroke_path_g(g, p, pen);
    path_free(p); free(q);
    STATUS(Ok);
}
PAIR(GdipDrawRectangle, 6) {
    GRAPHICS(0); PEN(1);
    TMPPATH(p, 0);
    path_rect(p, AN(2), AN(3), AN(4), AN(5));
    stroke_path_g(g, p, pen);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipDrawRectangles, 4) {
    GRAPHICS(0); PEN(1);
    int n = IA(3);
    if (!ARG(2) || n <= 0) STATUS(InvalidParameter);
    TMPPATH(p, 0);
    for (int i = 0; i < n; i++) {
        uint64_t r = ARG(2) + (uint64_t)i * 16;
        float v[4];
        for (int k = 0; k < 4; k++) v[k] = ii ? (float)rdi(r + 4 * (uint64_t)k) : rdf(r + 4 * (uint64_t)k);
        path_rect(p, v[0], v[1], v[2], v[3]);
    }
    stroke_path_g(g, p, pen);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipDrawEllipse, 6) {
    GRAPHICS(0); PEN(1);
    TMPPATH(p, 0);
    path_ellipse(p, AN(2), AN(3), AN(4), AN(5));
    stroke_path_g(g, p, pen);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipDrawArc, 8) {
    GRAPHICS(0); PEN(1);
    TMPPATH(p, 0);
    path_arc(p, AN(2), AN(3), AN(4), AN(5), FA(6), FA(7), 0);
    stroke_path_g(g, p, pen);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipDrawPie, 8) {
    GRAPHICS(0); PEN(1);
    TMPPATH(p, 0);
    path_pie(p, AN(2), AN(3), AN(4), AN(5), FA(6), FA(7));
    stroke_path_g(g, p, pen);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipDrawPolygon, 4) {
    GRAPHICS(0); PEN(1);
    int n = IA(3);
    Pt *q = rd_pts(ARG(2), n, ii);
    if (!q || n < 2) { free(q); STATUS(InvalidParameter); }
    TMPPATH(p, 0);
    path_poly(p, q, n, 1);
    stroke_path_g(g, p, pen);
    path_free(p); free(q);
    STATUS(Ok);
}
PAIR(GdipDrawBezier, 10) {
    GRAPHICS(0); PEN(1);
    TMPPATH(p, 0);
    path_add(p, AN(2), AN(3), PT_START);
    path_add(p, AN(4), AN(5), PT_BEZIER); path_add(p, AN(6), AN(7), PT_BEZIER); path_add(p, AN(8), AN(9), PT_BEZIER);
    stroke_path_g(g, p, pen);
    path_free(p);
    STATUS(Ok);
}
static void add_beziers(Path *p, const Pt *q, int n) {
    path_start_or_line(p, q[0]);
    for (int i = 1; i + 2 < n + 0 && i + 2 <= n - 1 + 0; i += 3) { path_add(p, q[i].x, q[i].y, PT_BEZIER); path_add(p, q[i + 1].x, q[i + 1].y, PT_BEZIER); path_add(p, q[i + 2].x, q[i + 2].y, PT_BEZIER); }
}
PAIR(GdipDrawBeziers, 4) {
    GRAPHICS(0); PEN(1);
    int n = IA(3);
    Pt *q = rd_pts(ARG(2), n, ii);
    if (!q || n < 4 || (n - 1) % 3) { free(q); STATUS(InvalidParameter); }
    TMPPATH(p, 0);
    add_beziers(p, q, n);
    stroke_path_g(g, p, pen);
    path_free(p); free(q);
    STATUS(Ok);
}
static void curve_common(X86State *st, int ii, int variant, int fill) { // variant 1: tension 0.5, 2: tension arg, 3: offset/segments/tension
    Graphics *g = OB(Graphics, 0, K_GRAPHICS);
    Pen *pen = fill ? NULL : OB(Pen, 1, K_PEN);
    Brush *br = fill ? OB(Brush, 1, K_BRUSH) : NULL;
    if (!g || (!pen && !br)) STATUS(InvalidParameter);
    int n = IA(3), off = 0, nseg = n - 1; float tension = 0.5f;
    if (variant == 2) tension = FA(4);
    if (variant == 3) { off = IA(4); nseg = IA(5); tension = FA(6); }
    Pt *q = rd_pts(ARG(2), n, ii);
    if (!q || n < 2 || off < 0 || off + nseg > n - 1 || nseg < 1) { free(q); STATUS(InvalidParameter); }
    TMPPATH(p, 0);
    path_curve(p, q, n, tension, 0, 0, off, nseg);
    if (fill) fill_path_g(g, p, br); else stroke_path_g(g, p, pen);
    path_free(p); free(q);
    STATUS(Ok);
}
PAIR(GdipDrawCurve, 4) { curve_common(st, ii, 1, 0); }
PAIR(GdipDrawCurve2, 5) { curve_common(st, ii, 2, 0); }
PAIR(GdipDrawCurve3, 7) { curve_common(st, ii, 3, 0); }
static void closed_curve_common(X86State *st, int ii, int variant, int fill) { // variant 1: tension 0.5, 2: tension (+ fill mode when filling)
    Graphics *g = OB(Graphics, 0, K_GRAPHICS);
    Pen *pen = fill ? NULL : OB(Pen, 1, K_PEN);
    Brush *br = fill ? OB(Brush, 1, K_BRUSH) : NULL;
    if (!g || (!pen && !br)) STATUS(InvalidParameter);
    int n = IA(3); float tension = variant == 2 ? FA(4) : 0.5f; int mode = (fill && variant == 2) ? IA(5) : 0;
    Pt *q = rd_pts(ARG(2), n, ii);
    if (!q || n < 3) { free(q); STATUS(InvalidParameter); }
    TMPPATH(p, mode);
    path_curve(p, q, n, tension, 1, 0, 0, n);
    if (fill) fill_path_g(g, p, br); else stroke_path_g(g, p, pen);
    path_free(p); free(q);
    STATUS(Ok);
}
PAIR(GdipDrawClosedCurve, 4) { closed_curve_common(st, ii, 1, 0); }
PAIR(GdipDrawClosedCurve2, 5) { closed_curve_common(st, ii, 2, 0); }
GP(GdipDrawPath, 3) { GRAPHICS(0); PEN(1); PATH(2); stroke_path_g(g, pa, pen); STATUS(Ok); }

PAIR(GdipFillRectangle, 6) {
    GRAPHICS(0); BRUSH(1);
    TMPPATH(p, 0);
    path_rect(p, AN(2), AN(3), AN(4), AN(5));
    fill_path_g(g, p, br);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipFillRectangles, 4) {
    GRAPHICS(0); BRUSH(1);
    int n = IA(3);
    if (!ARG(2) || n <= 0) STATUS(InvalidParameter);
    TMPPATH(p, 1);
    for (int i = 0; i < n; i++) {
        uint64_t r = ARG(2) + (uint64_t)i * 16;
        float v[4];
        for (int k = 0; k < 4; k++) v[k] = ii ? (float)rdi(r + 4 * (uint64_t)k) : rdf(r + 4 * (uint64_t)k);
        path_rect(p, v[0], v[1], v[2], v[3]);
    }
    fill_path_g(g, p, br);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipFillEllipse, 6) {
    GRAPHICS(0); BRUSH(1);
    TMPPATH(p, 0);
    path_ellipse(p, AN(2), AN(3), AN(4), AN(5));
    fill_path_g(g, p, br);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipFillPie, 8) {
    GRAPHICS(0); BRUSH(1);
    TMPPATH(p, 0);
    path_pie(p, AN(2), AN(3), AN(4), AN(5), FA(6), FA(7));
    fill_path_g(g, p, br);
    path_free(p);
    STATUS(Ok);
}
PAIR(GdipFillPolygon, 5) {
    GRAPHICS(0); BRUSH(1);
    int n = IA(3);
    Pt *q = rd_pts(ARG(2), n, ii);
    if (!q || n < 3) { free(q); STATUS(InvalidParameter); }
    TMPPATH(p, IA(4) ? 1 : 0);
    path_poly(p, q, n, 1);
    fill_path_g(g, p, br);
    path_free(p); free(q);
    STATUS(Ok);
}
PAIR(GdipFillPolygon2, 4) {
    GRAPHICS(0); BRUSH(1);
    int n = IA(3);
    Pt *q = rd_pts(ARG(2), n, ii);
    if (!q || n < 3) { free(q); STATUS(InvalidParameter); }
    TMPPATH(p, 0);
    path_poly(p, q, n, 1);
    fill_path_g(g, p, br);
    path_free(p); free(q);
    STATUS(Ok);
}
PAIR(GdipFillClosedCurve, 4) { closed_curve_common(st, ii, 1, 1); }
PAIR(GdipFillClosedCurve2, 6) { closed_curve_common(st, ii, 2, 1); }
GP(GdipFillPath, 3) { GRAPHICS(0); BRUSH(1); PATH(2); fill_path_g(g, pa, br); STATUS(Ok); }
GP(GdipFillRegion, 3) {
    GRAPHICS(0); BRUSH(1);
    Region *rg = OB(Region, 2, K_REGION);
    if (!rg) STATUS(InvalidParameter);
    Paint p;
    if (!paint_from_brush(g, br, &p)) STATUS(Ok);
    Region *c = region_clone(rg);
    Mat t = gtotal(g);
    region_xform(c, &t);
    float x0, y0, x1, y1;
    int r = region_bounds(c, &x0, &y0, &x1, &y1);
    int bx0 = 0, by0 = 0, bx1 = g->w, by1 = g->h;
    if (r == 0) { region_free(c); STATUS(Ok); }
    if (r == 1) { bx0 = (int)floorf(x0) - 1; by0 = (int)floorf(y0) - 1; bx1 = (int)ceilf(x1) + 1; by1 = (int)ceilf(y1) + 1; }
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > g->w) bx1 = g->w;
    if (by1 > g->h) by1 = g->h;
    if (bx1 > bx0 && by1 > by0) {
        Mask m = { bx0, by0, bx1 - bx0, by1 - by0, malloc((size_t)(bx1 - bx0) * (size_t)(by1 - by0)) };
        if (m.a) { region_mask(c, bx0, by0, m.w, m.h, m.a); composite(g, &m, &p, 0); free(m.a); }
    }
    region_free(c);
    STATUS(Ok);
}

// ---- pens ---------------------------------------------------------------------------------------------------------------------

static Pen *pen_new(float width) {
    Pen *p = NEWOBJ(Pen, K_PEN);
    if (!p) return NULL;
    p->width = width; p->miter = 10; p->xf = M_ID; p->unit = 2;
    return p;
}
GP(GdipCreatePen1, 4) {
    if (!ARG(3)) STATUS(InvalidParameter);
    Pen *p = pen_new(FA(1));
    if (!p) STATUS(OutOfMemory);
    p->color = (uint32_t)ARG(0); p->unit = IA(2);
    wr_ptr(ARG(3), H2G(p));
    STATUS(Ok);
}
GP(GdipCreatePen2, 4) {
    Brush *b = OB(Brush, 0, K_BRUSH);
    if (!b || !ARG(3)) STATUS(InvalidParameter);
    Pen *p = pen_new(FA(1));
    if (!p) STATUS(OutOfMemory);
    p->brush = brush_clone(b); p->unit = IA(2);
    if (b->type == BR_SOLID) { p->color = b->color; brush_free(p->brush); p->brush = NULL; }
    wr_ptr(ARG(3), H2G(p));
    STATUS(Ok);
}
GP(GdipClonePen, 2) {
    PEN(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    Pen *p = NEWOBJ(Pen, K_PEN);
    if (!p) STATUS(OutOfMemory);
    *p = *pen;
    if (pen->brush) p->brush = brush_clone(pen->brush);
    if (pen->dash) p->dash = dupmem(pen->dash, sizeof(float) * (size_t)pen->ndash);
    wr_ptr(ARG(1), H2G(p));
    STATUS(Ok);
}
GP(GdipDeletePen, 1) { PEN(0); brush_free(pen->brush); g_free(pen->dash); odel(pen); STATUS(Ok); }
SET_FLT(GdipSetPenWidth, Pen, K_PEN, o->width = v)
GET_FLT(GdipGetPenWidth, Pen, K_PEN, o->width)
SET_INT(GdipSetPenUnit, Pen, K_PEN, o->unit = v)
GET_INT(GdipGetPenUnit, Pen, K_PEN, o->unit)
GP(GdipSetPenLineCap197819, 4) { PEN(0); pen->start_cap = IA(1); pen->end_cap = IA(2); pen->dash_cap = IA(3); STATUS(Ok); }
SET_INT(GdipSetPenStartCap, Pen, K_PEN, o->start_cap = v)
SET_INT(GdipSetPenEndCap, Pen, K_PEN, o->end_cap = v)
GET_INT(GdipGetPenStartCap, Pen, K_PEN, o->start_cap)
GET_INT(GdipGetPenEndCap, Pen, K_PEN, o->end_cap)
SET_INT(GdipSetPenDashCap197819, Pen, K_PEN, o->dash_cap = v)
GET_INT(GdipGetPenDashCap197819, Pen, K_PEN, o->dash_cap)
SET_INT(GdipSetPenLineJoin, Pen, K_PEN, o->join = v)
GET_INT(GdipGetPenLineJoin, Pen, K_PEN, o->join)
SET_FLT(GdipSetPenMiterLimit, Pen, K_PEN, o->miter = v)
GET_FLT(GdipGetPenMiterLimit, Pen, K_PEN, o->miter)
SET_INT(GdipSetPenMode, Pen, K_PEN, o->align = v)
GET_INT(GdipGetPenMode, Pen, K_PEN, o->align)
GP(GdipSetPenColor, 2) { PEN(0); pen->color = (uint32_t)ARG(1); brush_free(pen->brush); pen->brush = NULL; STATUS(Ok); }
GP(GdipGetPenColor, 2) { PEN(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, pen->brush ? pen->brush->color : pen->color); STATUS(Ok); }
GP(GdipSetPenBrushFill, 2) {
    PEN(0); BRUSH(1);
    brush_free(pen->brush); pen->brush = NULL;
    if (br->type == BR_SOLID) pen->color = br->color; else pen->brush = brush_clone(br);
    STATUS(Ok);
}
GP(GdipGetPenBrushFill, 2) {
    PEN(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    Brush *b;
    if (pen->brush) b = brush_clone(pen->brush);
    else { b = brush_new(BR_SOLID); if (b) b->color = pen->color; }
    if (!b) STATUS(OutOfMemory);
    wr_ptr(ARG(1), H2G(b));
    STATUS(Ok);
}
GP(GdipGetPenFillType, 2) { PEN(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, pen->brush ? pen->brush->type : 0); STATUS(Ok); }
GP(GdipSetPenDashStyle, 2) { PEN(0); pen->dash_style = IA(1); if (pen->dash_style != 5) { g_free(pen->dash); pen->dash = NULL; pen->ndash = 0; } STATUS(Ok); }
GET_INT(GdipGetPenDashStyle, Pen, K_PEN, o->dash_style)
SET_FLT(GdipSetPenDashOffset, Pen, K_PEN, o->dash_off = v)
GET_FLT(GdipGetPenDashOffset, Pen, K_PEN, o->dash_off)
GP(GdipGetPenDashCount, 2) { PEN(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, pen->dash_style == 5 ? (uint32_t)pen->ndash : (uint32_t)DASH_N[pen->dash_style < 5 ? pen->dash_style : 0]); STATUS(Ok); }
GP(GdipSetPenDashArray, 3) {
    PEN(0);
    int n = IA(2);
    if (!ARG(1) || n <= 0) STATUS(InvalidParameter);
    g_free(pen->dash);
    pen->dash = g_malloc(sizeof(float) * (size_t)n);
    if (!pen->dash) STATUS(OutOfMemory);
    for (int i = 0; i < n; i++) pen->dash[i] = rdf(ARG(1) + 4 * (uint64_t)i);
    pen->ndash = n; pen->dash_style = 5;
    STATUS(Ok);
}
GP(GdipGetPenDashArray, 3) {
    PEN(0);
    int n = IA(2);
    if (!ARG(1)) STATUS(InvalidParameter);
    int have = pen->dash_style == 5 ? pen->ndash : DASH_N[pen->dash_style < 5 ? pen->dash_style : 0];
    if (n > have) n = have;
    for (int i = 0; i < n; i++) wrf(ARG(1) + 4 * (uint64_t)i, pen->dash_style == 5 ? pen->dash[i] : DASH_STD[pen->dash_style][i]);
    STATUS(Ok);
}
GP(GdipSetPenCompoundArray, 3) { PEN(0); STATUS(Ok); }
GP(GdipGetPenCompoundCount, 2) { PEN(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, 0); STATUS(Ok); }
GP(GdipSetPenTransform, 2) { PEN(0); MATRIX(1); pen->xf = mo->m; STATUS(Ok); }
GP(GdipGetPenTransform, 2) { PEN(0); MATRIX(1); mo->m = pen->xf; STATUS(Ok); }
GP(GdipResetPenTransform, 1) { PEN(0); pen->xf = M_ID; STATUS(Ok); }
GP(GdipMultiplyPenTransform, 3) { PEN(0); MATRIX(1); pen->xf = apply_order(pen->xf, mo->m, IA(2)); STATUS(Ok); }
GP(GdipTranslatePenTransform, 4) { PEN(0); pen->xf = apply_order(pen->xf, mat_translate(FA(1), FA(2)), IA(3)); STATUS(Ok); }
GP(GdipScalePenTransform, 4) { PEN(0); pen->xf = apply_order(pen->xf, mat_scale(FA(1), FA(2)), IA(3)); STATUS(Ok); }
GP(GdipRotatePenTransform, 3) { PEN(0); pen->xf = apply_order(pen->xf, mat_rotate(FA(1)), IA(2)); STATUS(Ok); }
GP(GdipSetPenCustomStartCap, 2) { STATUS(NotImplemented); }
GP(GdipSetPenCustomEndCap, 2) { STATUS(NotImplemented); }

// ---- brushes -----------------------------------------------------------------------------------------------------------------

GP(GdipCreateSolidFill, 2) {
    if (!ARG(1)) STATUS(InvalidParameter);
    Brush *b = brush_new(BR_SOLID);
    if (!b) STATUS(OutOfMemory);
    b->color = (uint32_t)ARG(0);
    wr_ptr(ARG(1), H2G(b));
    STATUS(Ok);
}
GP(GdipSetSolidFillColor, 2) { BRUSH(0); br->color = (uint32_t)ARG(1); STATUS(Ok); }
GP(GdipGetSolidFillColor, 2) { BRUSH(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->color); STATUS(Ok); }
GP(GdipCreateHatchBrush, 4) {
    if (!ARG(3) || IA(0) < 0 || IA(0) > 52) STATUS(InvalidParameter);
    Brush *b = brush_new(BR_HATCH);
    if (!b) STATUS(OutOfMemory);
    b->hatch = IA(0); b->color = (uint32_t)ARG(1); b->color2 = (uint32_t)ARG(2);
    wr_ptr(ARG(3), H2G(b));
    STATUS(Ok);
}
GP(GdipGetHatchStyle, 2) { BRUSH(0); if (!ARG(1) || br->type != BR_HATCH) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->hatch); STATUS(Ok); }
GP(GdipGetHatchForegroundColor, 2) { BRUSH(0); if (!ARG(1) || br->type != BR_HATCH) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->color); STATUS(Ok); }
GP(GdipGetHatchBackgroundColor, 2) { BRUSH(0); if (!ARG(1) || br->type != BR_HATCH) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->color2); STATUS(Ok); }
GP(GdipCloneBrush, 2) { BRUSH(0); if (!ARG(1)) STATUS(InvalidParameter); Brush *b = brush_clone(br); if (!b) STATUS(OutOfMemory); wr_ptr(ARG(1), H2G(b)); STATUS(Ok); }
GP(GdipDeleteBrush, 1) { BRUSH(0); brush_free(br); STATUS(Ok); }
GP(GdipGetBrushType, 2) { BRUSH(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->type); STATUS(Ok); }

// transform functions shared by texture / line / path-gradient brushes
#define BRUSH_XF(PFX, TYPE_OK) \
    GP(GdipSet##PFX##Transform, 2) { BRUSH(0); MATRIX(1); if (!(TYPE_OK)) STATUS(WrongState); br->xf = mo->m; STATUS(Ok); } \
    GP(GdipGet##PFX##Transform, 2) { BRUSH(0); MATRIX(1); if (!(TYPE_OK)) STATUS(WrongState); mo->m = br->xf; STATUS(Ok); } \
    GP(GdipReset##PFX##Transform, 1) { BRUSH(0); if (!(TYPE_OK)) STATUS(WrongState); br->xf = M_ID; STATUS(Ok); } \
    GP(GdipMultiply##PFX##Transform, 3) { BRUSH(0); MATRIX(1); if (!(TYPE_OK)) STATUS(WrongState); br->xf = apply_order(br->xf, mo->m, IA(2)); STATUS(Ok); } \
    GP(GdipTranslate##PFX##Transform, 4) { BRUSH(0); if (!(TYPE_OK)) STATUS(WrongState); br->xf = apply_order(br->xf, mat_translate(FA(1), FA(2)), IA(3)); STATUS(Ok); } \
    GP(GdipScale##PFX##Transform, 4) { BRUSH(0); if (!(TYPE_OK)) STATUS(WrongState); br->xf = apply_order(br->xf, mat_scale(FA(1), FA(2)), IA(3)); STATUS(Ok); } \
    GP(GdipRotate##PFX##Transform, 3) { BRUSH(0); if (!(TYPE_OK)) STATUS(WrongState); br->xf = apply_order(br->xf, mat_rotate(FA(1)), IA(2)); STATUS(Ok); }
BRUSH_XF(Texture, br->type == BR_TEXTURE)
BRUSH_XF(Line, br->type == BR_LINEAR)
BRUSH_XF(PathGradient, br->type == BR_PATHGRAD)

// texture brushes
static Brush *texture_brush(Image *im, int wrap, int x, int y, int w, int h, ImgAttr *ia) {
    Brush *b = brush_new(BR_TEXTURE);
    if (!b) return NULL;
    b->img = image_clone(im);
    if (!b->img) { odel(b); return NULL; }
    b->wrap = wrap; b->tex_x = x; b->tex_y = y; b->tex_w = w; b->tex_h = h;
    if (ia) { b->attr = dupmem(&ia->cat[ia->cat[1].cm_on || ia->cat[1].key_on || ia->cat[1].gamma_on ? 1 : 0], sizeof(AttrSet)); }
    return b;
}
GP(GdipCreateTexture, 3) {
    Image *im = OB(Image, 0, K_IMAGE);
    if (!im || !ARG(2) || IA(1) < 0 || IA(1) > 4) STATUS(InvalidParameter);
    Brush *b = texture_brush(im, IA(1), 0, 0, im->w, im->h, NULL);
    if (!b) STATUS(OutOfMemory);
    wr_ptr(ARG(2), H2G(b));
    STATUS(Ok);
}
PAIR(GdipCreateTexture2, 7) {
    Image *im = OB(Image, 0, K_IMAGE);
    if (!im || !ARG(6) || IA(1) < 0 || IA(1) > 4) STATUS(InvalidParameter);
    int x = (int)AN(2), y = (int)AN(3), w = (int)AN(4), h = (int)AN(5);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > im->w || y + h > im->h) STATUS(InvalidParameter);
    Brush *b = texture_brush(im, IA(1), x, y, w, h, NULL);
    if (!b) STATUS(OutOfMemory);
    wr_ptr(ARG(6), H2G(b));
    STATUS(Ok);
}
PAIR(GdipCreateTextureIA, 7) {
    Image *im = OB(Image, 0, K_IMAGE);
    ImgAttr *ia = OB(ImgAttr, 1, K_ATTR);
    if (!im || !ARG(6)) STATUS(InvalidParameter);
    int x = (int)AN(2), y = (int)AN(3), w = (int)AN(4), h = (int)AN(5);
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > im->w || y + h > im->h) STATUS(InvalidParameter);
    Brush *b = texture_brush(im, ia ? ia->wrap : 0, x, y, w, h, ia);
    if (!b) STATUS(OutOfMemory);
    wr_ptr(ARG(6), H2G(b));
    STATUS(Ok);
}
GP(GdipGetTextureImage, 2) {
    BRUSH(0);
    if (!ARG(1) || br->type != BR_TEXTURE) STATUS(InvalidParameter);
    Image *c = image_clone(br->img);
    if (!c) STATUS(OutOfMemory);
    wr_ptr(ARG(1), H2G(c));
    STATUS(Ok);
}
GP(GdipSetTextureWrapMode, 2) { BRUSH(0); if (br->type != BR_TEXTURE) STATUS(WrongState); br->wrap = IA(1); STATUS(Ok); }
GP(GdipGetTextureWrapMode, 2) { BRUSH(0); if (!ARG(1) || br->type != BR_TEXTURE) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->wrap); STATUS(Ok); }

// linear gradient brushes
static Brush *linear_brush(uint32_t c1, uint32_t c2, int wrap) {
    Brush *b = brush_new(BR_LINEAR);
    if (!b) return NULL;
    b->color = c1; b->color2 = c2; b->wrap = wrap;
    return b;
}
PAIR(GdipCreateLineBrush, 6) {
    uint64_t a = ARG(0), c = ARG(1);
    if (!a || !c || !ARG(5)) STATUS(InvalidParameter);
    Pt p1 = { ii ? (float)rdi(a) : rdf(a), ii ? (float)rdi(a + 4) : rdf(a + 4) }, p2 = { ii ? (float)rdi(c) : rdf(c), ii ? (float)rdi(c + 4) : rdf(c + 4) };
    if (p1.x == p2.x && p1.y == p2.y) STATUS(OutOfMemory);
    Brush *b = linear_brush((uint32_t)ARG(2), (uint32_t)ARG(3), IA(4));
    if (!b) STATUS(OutOfMemory);
    b->p1 = p1; b->p2 = p2;
    Pt d = { p2.x - p1.x, p2.y - p1.y };
    b->rc_c = (Pt){ (p1.x + p2.x) / 2, (p1.y + p2.y) / 2 };
    (void)d;
    wr_ptr(ARG(5), H2G(b));
    STATUS(Ok);
}
static int rc_read(uint64_t a, int ii, float *r) { if (!a) return 0; for (int i = 0; i < 4; i++) r[i] = ii ? (float)rdi(a + 4 * (uint64_t)i) : rdf(a + 4 * (uint64_t)i); return r[2] > 0 && r[3] > 0; }
PAIR(GdipCreateLineBrushFromRect, 6) {
    float r[4];
    if (!rc_read(ARG(0), ii, r) || !ARG(5)) STATUS(InvalidParameter);
    int mode = IA(3);
    if (mode < 0 || mode > 3) STATUS(InvalidParameter);
    Brush *b = linear_brush((uint32_t)ARG(1), (uint32_t)ARG(2), IA(4));
    if (!b) STATUS(OutOfMemory);
    b->rc_based = 1; b->rc_c = (Pt){ r[0] + r[2] / 2, r[1] + r[3] / 2 }; b->rc_w = r[2]; b->rc_h = r[3];
    static const float ang[4] = { 0, 90, 45, 135 };
    b->rc_angle = ang[mode]; b->rc_scalable = mode >= 2;
    b->p1 = (Pt){ r[0], r[1] }; b->p2 = (Pt){ r[0] + r[2], r[1] + r[3] };
    wr_ptr(ARG(5), H2G(b));
    STATUS(Ok);
}
PAIR(GdipCreateLineBrushFromRectWithAngle, 7) {
    float r[4];
    if (!rc_read(ARG(0), ii, r) || !ARG(6)) STATUS(InvalidParameter);
    Brush *b = linear_brush((uint32_t)ARG(1), (uint32_t)ARG(2), IA(5));
    if (!b) STATUS(OutOfMemory);
    b->rc_based = 1; b->rc_c = (Pt){ r[0] + r[2] / 2, r[1] + r[3] / 2 }; b->rc_w = r[2]; b->rc_h = r[3];
    b->rc_angle = FA(3); b->rc_scalable = IA(4) != 0;
    b->p1 = (Pt){ r[0], r[1] }; b->p2 = (Pt){ r[0] + r[2], r[1] + r[3] };
    wr_ptr(ARG(6), H2G(b));
    STATUS(Ok);
}
#define LINEB(n) BRUSH(n); if (br->type != BR_LINEAR) STATUS(WrongState)
GP(GdipSetLineColors, 3) { LINEB(0); br->color = (uint32_t)ARG(1); br->color2 = (uint32_t)ARG(2); STATUS(Ok); }
GP(GdipGetLineColors, 2) { LINEB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->color); mem_wr(ARG(1) + 4, 4, br->color2); STATUS(Ok); }
PAIR(GdipGetLineRect, 2) {
    LINEB(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    float x, y, w, h;
    if (br->rc_based) { x = br->rc_c.x - br->rc_w / 2; y = br->rc_c.y - br->rc_h / 2; w = br->rc_w; h = br->rc_h; }
    else { x = fminf(br->p1.x, br->p2.x); y = fminf(br->p1.y, br->p2.y); w = fabsf(br->p2.x - br->p1.x); h = fabsf(br->p2.y - br->p1.y); }
    if (ii) write_recti(ARG(1), x, y, w, h); else write_rectf(ARG(1), x, y, w, h);
    STATUS(Ok);
}
GP(GdipSetLineGammaCorrection, 2) { LINEB(0); STATUS(Ok); }
GP(GdipGetLineGammaCorrection, 2) { LINEB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, 0); STATUS(Ok); }
static void set_blend_arrays(Brush *b, const float *f, const float *p, int n) {
    g_free(b->bfac); g_free(b->bpos); b->bfac = b->bpos = NULL; b->nblend = 0;
    if (n <= 0) return;
    b->bfac = dupmem(f, sizeof(float) * (size_t)n); b->bpos = dupmem(p, sizeof(float) * (size_t)n); b->nblend = n;
}
static void set_triangle_blend(Brush *b, float focus, float scale, int sigma) {
    float f[3], p[3]; int n = 0;
    if (scale <= 0) scale = 1e-4f;
    if (focus > 0) { f[n] = 0; p[n++] = 0; }
    f[n] = scale; p[n++] = focus;
    if (focus < 1) { f[n] = 0; p[n++] = 1; }
    (void)sigma;
    set_blend_arrays(b, f, p, n);
}
GP(GdipSetLineLinearBlend, 3) { LINEB(0); set_triangle_blend(br, FA(1), FA(2), 0); STATUS(Ok); }
GP(GdipSetLineSigmaBlend, 3) { LINEB(0); set_triangle_blend(br, FA(1), FA(2), 1); STATUS(Ok); }
GP(GdipGetLineBlendCount, 2) { LINEB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->nblend ? br->nblend : 1); STATUS(Ok); }
GP(GdipGetLineBlend, 4) {
    LINEB(0);
    int n = IA(3);
    if (!ARG(1) || !ARG(2) || n < 1) STATUS(InvalidParameter);
    if (!br->nblend) { wrf(ARG(1), 1); wrf(ARG(2), 0); STATUS(Ok); }
    if (n < br->nblend) STATUS(InsufficientBuffer);
    for (int i = 0; i < br->nblend; i++) { wrf(ARG(1) + 4 * (uint64_t)i, br->bfac[i]); wrf(ARG(2) + 4 * (uint64_t)i, br->bpos[i]); }
    STATUS(Ok);
}
GP(GdipSetLineBlend, 4) {
    LINEB(0);
    int n = IA(3);
    if (!ARG(1) || !ARG(2) || n < 1) STATUS(InvalidParameter);
    float *f = malloc(sizeof(float) * (size_t)n), *p = malloc(sizeof(float) * (size_t)n);
    if (!f || !p) { free(f); free(p); STATUS(OutOfMemory); }
    for (int i = 0; i < n; i++) { f[i] = rdf(ARG(1) + 4 * (uint64_t)i); p[i] = rdf(ARG(2) + 4 * (uint64_t)i); }
    set_blend_arrays(br, f, p, n);
    free(f); free(p);
    STATUS(Ok);
}
GP(GdipGetLinePresetBlendCount, 2) { LINEB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->npreset); STATUS(Ok); }
GP(GdipGetLinePresetBlend, 4) {
    LINEB(0);
    int n = IA(3);
    if (!ARG(1) || !ARG(2) || n < br->npreset || br->npreset < 1) STATUS(InvalidParameter);
    for (int i = 0; i < br->npreset; i++) { mem_wr(ARG(1) + 4 * (uint64_t)i, 4, br->pcol[i]); wrf(ARG(2) + 4 * (uint64_t)i, br->ppos[i]); }
    STATUS(Ok);
}
static void set_preset(Brush *b, uint64_t cols, uint64_t pos, int n) {
    g_free(b->pcol); g_free(b->ppos);
    b->pcol = g_malloc(4 * (size_t)n); b->ppos = g_malloc(sizeof(float) * (size_t)n); b->npreset = n;
    for (int i = 0; i < n; i++) { b->pcol[i] = (uint32_t)mem_rd(cols + 4 * (uint64_t)i, 4); b->ppos[i] = rdf(pos + 4 * (uint64_t)i); }
}
GP(GdipSetLinePresetBlend, 4) {
    LINEB(0);
    int n = IA(3);
    if (!ARG(1) || !ARG(2) || n < 2) STATUS(InvalidParameter);
    set_preset(br, ARG(1), ARG(2), n);
    STATUS(Ok);
}
GP(GdipSetLineWrapMode, 2) { LINEB(0); if (IA(1) < 0 || IA(1) > 4 || IA(1) == 4) STATUS(InvalidParameter); br->wrap = IA(1); STATUS(Ok); }
GP(GdipGetLineWrapMode, 2) { LINEB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->wrap); STATUS(Ok); }

// path gradient brushes
static Brush *pathgrad_brush(const Pt *pts, int n, int wrap) {
    if (n < 2) return NULL;
    Brush *b = brush_new(BR_PATHGRAD);
    if (!b) return NULL;
    b->gpts = dupmem(pts, sizeof(Pt) * (size_t)n); b->gn = n; b->wrap = wrap;
    b->gcol = g_malloc(4); b->gcol[0] = 0xFFFFFFFFu; b->ngcol = 1; b->color = 0xFF000000u;
    float cx = 0, cy = 0;
    for (int i = 0; i < n; i++) { cx += pts[i].x; cy += pts[i].y; }
    b->center = (Pt){ cx / (float)n, cy / (float)n };
    b->focus_x = b->focus_y = 0;
    return b;
}
PAIR(GdipCreatePathGradient, 4) {
    int n = IA(1);
    Pt *q = rd_pts(ARG(0), n, ii);
    if (!q || !ARG(3)) { free(q); STATUS(InvalidParameter); }
    Brush *b = pathgrad_brush(q, n, IA(2));
    free(q);
    if (!b) STATUS(OutOfMemory);
    wr_ptr(ARG(3), H2G(b));
    STATUS(Ok);
}
GP(GdipCreatePathGradientFromPath, 2) {
    PATH(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    PolySet ps; memset(&ps, 0, sizeof ps);
    path_flatten(pa, NULL, &ps);
    if (!ps.n) STATUS(OutOfMemory);
    Brush *b = pathgrad_brush(ps.s[0].p, ps.s[0].n, 0);
    ps_free(&ps);
    if (!b) STATUS(OutOfMemory);
    wr_ptr(ARG(1), H2G(b));
    STATUS(Ok);
}
#define PGB(n) BRUSH(n); if (br->type != BR_PATHGRAD) STATUS(WrongState)
GP(GdipGetPathGradientCenterColor, 2) { PGB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->color); STATUS(Ok); }
GP(GdipSetPathGradientCenterColor, 2) { PGB(0); br->color = (uint32_t)ARG(1); STATUS(Ok); }
GP(GdipGetPathGradientSurroundColorsWithCount, 3) {
    PGB(0);
    if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter);
    int n = rdi(ARG(2));
    int have = br->ngcol <= 1 ? br->gn : br->ngcol;
    if (n < have) STATUS(InsufficientBuffer);
    for (int i = 0; i < have; i++) mem_wr(ARG(1) + 4 * (uint64_t)i, 4, br->ngcol <= 1 ? br->gcol[0] : br->gcol[i]);
    mem_wr(ARG(2), 4, (uint32_t)have);
    STATUS(Ok);
}
GP(GdipSetPathGradientSurroundColorsWithCount, 3) {
    PGB(0);
    if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter);
    int n = rdi(ARG(2));
    if (n < 1 || n > br->gn) STATUS(InvalidParameter);
    g_free(br->gcol);
    br->gcol = g_malloc(4 * (size_t)n);
    for (int i = 0; i < n; i++) br->gcol[i] = (uint32_t)mem_rd(ARG(1) + 4 * (uint64_t)i, 4);
    // a single colour or all-equal colours behave as one
    br->ngcol = n;
    int same = 1;
    for (int i = 1; i < n; i++) if (br->gcol[i] != br->gcol[0]) same = 0;
    if (same) br->ngcol = 1;
    STATUS(Ok);
}
GP(GdipGetPathGradientSurroundColorCount, 2) { PGB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->ngcol <= 1 ? br->gn : br->ngcol); STATUS(Ok); }
PAIR(GdipGetPathGradientCenterPoint, 2) {
    PGB(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    wr_pts(ARG(1), &br->center, 1, ii);
    STATUS(Ok);
}
PAIR(GdipSetPathGradientCenterPoint, 2) {
    PGB(0);
    Pt *q = rd_pts(ARG(1), 1, ii);
    if (!q) STATUS(InvalidParameter);
    br->center = q[0]; free(q);
    STATUS(Ok);
}
PAIR(GdipGetPathGradientRect, 2) {
    PGB(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    float a = 1e30f, b = 1e30f, c = -1e30f, d = -1e30f;
    for (int i = 0; i < br->gn; i++) { Pt p = br->gpts[i]; if (p.x < a) a = p.x; if (p.x > c) c = p.x; if (p.y < b) b = p.y; if (p.y > d) d = p.y; }
    if (ii) write_recti(ARG(1), a, b, c - a, d - b); else write_rectf(ARG(1), a, b, c - a, d - b);
    STATUS(Ok);
}
GP(GdipGetPathGradientPointCount, 2) { PGB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->gn); STATUS(Ok); }
GP(GdipSetPathGradientFocusScales, 3) { PGB(0); br->focus_x = FA(1); br->focus_y = FA(2); STATUS(Ok); }
GP(GdipGetPathGradientFocusScales, 3) { PGB(0); if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter); wrf(ARG(1), br->focus_x); wrf(ARG(2), br->focus_y); STATUS(Ok); }
GP(GdipSetPathGradientWrapMode, 2) { PGB(0); br->wrap = IA(1); STATUS(Ok); }
GP(GdipGetPathGradientWrapMode, 2) { PGB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->wrap); STATUS(Ok); }
GP(GdipSetPathGradientLinearBlend, 3) { PGB(0); set_triangle_blend(br, FA(1), FA(2), 0); STATUS(Ok); }
GP(GdipSetPathGradientSigmaBlend, 3) { PGB(0); set_triangle_blend(br, FA(1), FA(2), 1); STATUS(Ok); }
GP(GdipSetPathGradientBlend, 4) {
    PGB(0);
    int n = IA(3);
    if (!ARG(1) || !ARG(2) || n < 1) STATUS(InvalidParameter);
    float *f = malloc(sizeof(float) * (size_t)n), *p = malloc(sizeof(float) * (size_t)n);
    if (!f || !p) { free(f); free(p); STATUS(OutOfMemory); }
    for (int i = 0; i < n; i++) { f[i] = rdf(ARG(1) + 4 * (uint64_t)i); p[i] = rdf(ARG(2) + 4 * (uint64_t)i); }
    set_blend_arrays(br, f, p, n);
    free(f); free(p);
    STATUS(Ok);
}
GP(GdipGetPathGradientBlendCount, 2) { PGB(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, br->nblend ? br->nblend : 1); STATUS(Ok); }
GP(GdipSetPathGradientPresetBlend, 4) {
    PGB(0);
    int n = IA(3);
    if (!ARG(1) || !ARG(2) || n < 2) STATUS(InvalidParameter);
    set_preset(br, ARG(1), ARG(2), n);
    STATUS(Ok);
}

// ---- paths -----------------------------------------------------------------------------------------------------------------------

GP(GdipCreatePath, 2) {
    if (!ARG(1)) STATUS(InvalidParameter);
    Path *p = path_new(IA(0) ? 1 : 0);
    if (!p) STATUS(OutOfMemory);
    wr_ptr(ARG(1), H2G(p));
    STATUS(Ok);
}
PAIR(GdipCreatePath2, 5) {
    int n = IA(2);
    Pt *q = rd_pts(ARG(0), n, ii);
    if (!q || !ARG(1) || !ARG(4)) { free(q); STATUS(InvalidParameter); }
    Path *p = path_new(IA(3) ? 1 : 0);
    if (!p) { free(q); STATUS(OutOfMemory); }
    for (int i = 0; i < n; i++) path_add(p, q[i].x, q[i].y, (int)mem_rd(ARG(1) + (uint64_t)i, 1));
    free(q);
    wr_ptr(ARG(4), H2G(p));
    STATUS(Ok);
}
GP(GdipClonePath, 2) { PATH(0); if (!ARG(1)) STATUS(InvalidParameter); Path *c = path_clone(pa); if (!c) STATUS(OutOfMemory); wr_ptr(ARG(1), H2G(c)); STATUS(Ok); }
GP(GdipDeletePath, 1) { PATH(0); path_free(pa); STATUS(Ok); }
GP(GdipResetPath, 1) { PATH(0); pa->n = 0; pa->need_start = 0; pa->fill = 0; STATUS(Ok); }
GP(GdipGetPointCount, 2) { PATH(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)pa->n); STATUS(Ok); }
GP(GdipGetPathTypes, 3) {
    PATH(0);
    if (!ARG(1) || IA(2) < pa->n) STATUS(InsufficientBuffer);
    memcpy(G2H(ARG(1)), pa->types, (size_t)pa->n);
    STATUS(Ok);
}
PAIR(GdipGetPathPoints, 3) {
    PATH(0);
    if (!ARG(1) || IA(2) < pa->n) STATUS(InsufficientBuffer);
    wr_pts(ARG(1), pa->pts, pa->n, ii);
    STATUS(Ok);
}
GP(GdipGetPathData, 2) {
    PATH(0);
    uint64_t d = ARG(1);
    if (!d) STATUS(InvalidParameter);
    int cnt = rdi(d);
    if (cnt < pa->n) STATUS(InsufficientBuffer);
    uint64_t pts = rd_ptr(d + (uint64_t)PSZ), types = rd_ptr(d + 2 * (uint64_t)PSZ);
    if (pa->n && (!pts || !types)) STATUS(InvalidParameter);
    wr_pts(pts, pa->pts, pa->n, 0);
    if (pa->n) memcpy(G2H(types), pa->types, (size_t)pa->n);
    mem_wr(d, 4, (uint32_t)pa->n);
    STATUS(Ok);
}
GP(GdipGetPathFillMode, 2) { PATH(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)pa->fill); STATUS(Ok); }
GP(GdipSetPathFillMode, 2) { PATH(0); pa->fill = IA(1) ? 1 : 0; STATUS(Ok); }
GP(GdipStartPathFigure, 1) { PATH(0); pa->need_start = 1; STATUS(Ok); }
GP(GdipClosePathFigure, 1) { PATH(0); path_close(pa); STATUS(Ok); }
GP(GdipClosePathFigures, 1) {
    PATH(0);
    for (int i = 0; i < pa->n; i++) if (i + 1 == pa->n || (pa->types[i + 1] & PT_MASK) == PT_START) pa->types[i] |= PT_CLOSE;
    pa->need_start = 1;
    STATUS(Ok);
}
GP(GdipSetPathMarker, 1) { PATH(0); if (pa->n) pa->types[pa->n - 1] |= 0x20; STATUS(Ok); }
GP(GdipClearPathMarkers, 1) { PATH(0); for (int i = 0; i < pa->n; i++) pa->types[i] &= (uint8_t)~0x20; STATUS(Ok); }
GP(GdipReversePath, 1) {
    PATH(0);
    int n = pa->n;
    Pt *np = g_malloc(sizeof(Pt) * (size_t)(n ? n : 1)); uint8_t *nt = g_malloc((size_t)(n ? n : 1));
    if (!np || !nt) STATUS(OutOfMemory);
    // reverse every figure separately, keep figure order reversed as GDI+ does
    int out = 0, i = n;
    while (i > 0) {
        int e = i - 1, s = e;
        while (s > 0 && (pa->types[s] & PT_MASK) != PT_START) s--;
        int closed = (pa->types[e] & PT_CLOSE) != 0;
        for (int k = e; k >= s; k--) {
            np[out] = pa->pts[k];
            int t = pa->types[k] & PT_MASK;
            if (k == e) t = PT_START;
            else { int tp = pa->types[k + 1] & PT_MASK; t = tp == PT_BEZIER ? PT_BEZIER : PT_LINE; }
            nt[out] = (uint8_t)t; out++;
        }
        if (closed) nt[out - 1] |= PT_CLOSE;
        i = s;
    }
    g_free(pa->pts); g_free(pa->types);
    pa->pts = np; pa->types = nt; pa->cap = n ? n : 1;
    STATUS(Ok);
}
GP(GdipGetPathLastPoint, 2) { PATH(0); if (!ARG(1) || !pa->n) STATUS(InvalidParameter); wrf(ARG(1), pa->pts[pa->n - 1].x); wrf(ARG(1) + 4, pa->pts[pa->n - 1].y); STATUS(Ok); }
PAIR(GdipAddPathLine, 5) {
    PATH(0);
    path_start_or_line(pa, (Pt){ AN(1), AN(2) });
    path_start_or_line(pa, (Pt){ AN(3), AN(4) });
    STATUS(Ok);
}
PAIR(GdipAddPathLine2, 3) {
    PATH(0);
    int n = IA(2);
    Pt *q = rd_pts(ARG(1), n, ii);
    if (!q) STATUS(InvalidParameter);
    for (int i = 0; i < n; i++) path_start_or_line(pa, q[i]);
    free(q);
    STATUS(Ok);
}
PAIR(GdipAddPathArc, 7) { PATH(0); if (AN(3) <= 0 || AN(4) <= 0) STATUS(InvalidParameter); path_arc(pa, AN(1), AN(2), AN(3), AN(4), FA(5), FA(6), 1); STATUS(Ok); }
PAIR(GdipAddPathBezier, 9) {
    PATH(0);
    path_start_or_line(pa, (Pt){ AN(1), AN(2) });
    path_add(pa, AN(3), AN(4), PT_BEZIER); path_add(pa, AN(5), AN(6), PT_BEZIER); path_add(pa, AN(7), AN(8), PT_BEZIER);
    STATUS(Ok);
}
PAIR(GdipAddPathBeziers, 3) {
    PATH(0);
    int n = IA(2);
    Pt *q = rd_pts(ARG(1), n, ii);
    if (!q || n < 4 || (n - 1) % 3) { free(q); STATUS(InvalidParameter); }
    add_beziers(pa, q, n);
    free(q);
    STATUS(Ok);
}
PAIR(GdipAddPathCurve, 3) { PATH(0); int n = IA(2); Pt *q = rd_pts(ARG(1), n, ii); if (!q || n < 2) { free(q); STATUS(InvalidParameter); } path_curve(pa, q, n, 0.5f, 0, 1, 0, n - 1); free(q); STATUS(Ok); }
PAIR(GdipAddPathCurve2, 4) { PATH(0); int n = IA(2); Pt *q = rd_pts(ARG(1), n, ii); if (!q || n < 2) { free(q); STATUS(InvalidParameter); } path_curve(pa, q, n, FA(3), 0, 1, 0, n - 1); free(q); STATUS(Ok); }
PAIR(GdipAddPathCurve3, 6) {
    PATH(0);
    int n = IA(2), off = IA(3), ns = IA(4);
    Pt *q = rd_pts(ARG(1), n, ii);
    if (!q || n < 2 || off < 0 || ns < 1 || off + ns > n - 1) { free(q); STATUS(InvalidParameter); }
    path_curve(pa, q, n, FA(5), 0, 1, off, ns);
    free(q);
    STATUS(Ok);
}
PAIR(GdipAddPathClosedCurve, 3) { PATH(0); int n = IA(2); Pt *q = rd_pts(ARG(1), n, ii); if (!q || n < 3) { free(q); STATUS(InvalidParameter); } path_curve(pa, q, n, 0.5f, 1, 0, 0, n); free(q); STATUS(Ok); }
PAIR(GdipAddPathClosedCurve2, 4) { PATH(0); int n = IA(2); Pt *q = rd_pts(ARG(1), n, ii); if (!q || n < 3) { free(q); STATUS(InvalidParameter); } path_curve(pa, q, n, FA(3), 1, 0, 0, n); free(q); STATUS(Ok); }
PAIR(GdipAddPathRectangle, 5) { PATH(0); pa->need_start = 1; path_rect(pa, AN(1), AN(2), AN(3), AN(4)); STATUS(Ok); }
PAIR(GdipAddPathRectangles, 3) {
    PATH(0);
    int n = IA(2);
    if (!ARG(1) || n <= 0) STATUS(InvalidParameter);
    for (int i = 0; i < n; i++) {
        uint64_t r = ARG(1) + (uint64_t)i * 16;
        float v[4];
        for (int k = 0; k < 4; k++) v[k] = ii ? (float)rdi(r + 4 * (uint64_t)k) : rdf(r + 4 * (uint64_t)k);
        path_rect(pa, v[0], v[1], v[2], v[3]);
    }
    STATUS(Ok);
}
PAIR(GdipAddPathEllipse, 5) { PATH(0); pa->need_start = 1; path_ellipse(pa, AN(1), AN(2), AN(3), AN(4)); STATUS(Ok); }
PAIR(GdipAddPathPie, 7) { PATH(0); pa->need_start = 1; path_pie(pa, AN(1), AN(2), AN(3), AN(4), FA(5), FA(6)); STATUS(Ok); }
PAIR(GdipAddPathPolygon, 3) {
    PATH(0);
    int n = IA(2);
    Pt *q = rd_pts(ARG(1), n, ii);
    if (!q || n < 3) { free(q); STATUS(InvalidParameter); }
    pa->need_start = 1;
    path_poly(pa, q, n, 1);
    free(q);
    STATUS(Ok);
}
GP(GdipAddPathPath, 3) {
    PATH(0);
    Path *o = OB(Path, 1, K_PATH);
    if (!o) STATUS(InvalidParameter);
    int connect = IA(2);
    Path *c = path_clone(o);
    if (!c) STATUS(OutOfMemory);
    for (int i = 0; i < c->n; i++) {
        int t = c->types[i];
        if (i == 0 && connect && pa->n && !(pa->types[pa->n - 1] & PT_CLOSE)) {
            Pt l = pa->pts[pa->n - 1];
            if (l.x != c->pts[0].x || l.y != c->pts[0].y) path_add(pa, c->pts[0].x, c->pts[0].y, PT_LINE);
            continue;
        }
        path_add(pa, c->pts[i].x, c->pts[i].y, t);
    }
    path_free(c);
    STATUS(Ok);
}
GP(GdipAddPathString, 8) { STATUS(NotImplemented); }
GP(GdipAddPathStringI, 8) { STATUS(NotImplemented); }
GP(GdipTransformPath, 2) { PATH(0); MatObj *m = OB(MatObj, 1, K_MATRIX); if (m) path_xform(pa, &m->m); STATUS(Ok); }
PAIR(GdipGetPathWorldBounds, 4) {
    PATH(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    MatObj *m = OB(MatObj, 2, K_MATRIX);
    Pen *pen = OB(Pen, 3, K_PEN);
    PolySet ps; memset(&ps, 0, sizeof ps);
    path_flatten(pa, m ? &m->m : NULL, &ps);
    float a = 1e30f, b = 1e30f, c = -1e30f, d = -1e30f; int any = 0;
    for (int i = 0; i < ps.n; i++) for (int k = 0; k < ps.s[i].n; k++) {
        Pt p = ps.s[i].p[k]; any = 1;
        if (p.x < a) a = p.x;
        if (p.x > c) c = p.x;
        if (p.y < b) b = p.y;
        if (p.y > d) d = p.y;
    }
    ps_free(&ps);
    if (!any) { a = b = c = d = 0; }
    if (pen) { float hw = pen->width / 2; a -= hw; b -= hw; c += hw; d += hw; }
    if (ii) write_recti(ARG(1), a, b, c - a, d - b); else write_rectf(ARG(1), a, b, c - a, d - b);
    STATUS(Ok);
}
GP(GdipFlattenPath, 3) {
    PATH(0);
    MatObj *m = OB(MatObj, 1, K_MATRIX);
    PolySet ps; memset(&ps, 0, sizeof ps);
    path_flatten(pa, m ? &m->m : NULL, &ps);
    pa->n = 0; pa->need_start = 0;
    for (int i = 0; i < ps.n; i++) { path_poly(pa, ps.s[i].p, ps.s[i].n, ps.s[i].closed); }
    ps_free(&ps);
    STATUS(Ok);
}
GP(GdipWindingModeOutline, 3) { STATUS(NotImplemented); }
GP(GdipWidenPath, 4) { STATUS(NotImplemented); }
GP(GdipWarpPath, 10) { STATUS(NotImplemented); }
PAIR(GdipIsVisiblePathPoint, 5) {
    PATH(0);
    if (!ARG(4)) STATUS(InvalidParameter);
    mem_wr(ARG(4), 4, path_pt_in(pa, AN(1), AN(2)));
    STATUS(Ok);
}
PAIR(GdipIsOutlineVisiblePathPoint, 6) {
    PATH(0); PEN(4);
    if (!ARG(5)) STATUS(InvalidParameter);
    float x = AN(1), y = AN(2);
    PolySet ps; memset(&ps, 0, sizeof ps);
    path_flatten(pa, NULL, &ps);
    float hw = pen->width / 2, best = 1e30f;
    for (int i = 0; i < ps.n; i++) {
        Poly *q = &ps.s[i];
        int ns = q->closed ? q->n : q->n - 1;
        for (int k = 0; k < ns; k++) {
            Pt a = q->p[k], b = q->p[(k + 1) % q->n];
            float dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, t = l2 > 0 ? ((x - a.x) * dx + (y - a.y) * dy) / l2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float d = hypotf(x - (a.x + t * dx), y - (a.y + t * dy));
            if (d < best) best = d;
        }
    }
    ps_free(&ps);
    mem_wr(ARG(5), 4, best <= hw);
    STATUS(Ok);
}

// path iterator
typedef struct PathIter { int kind; Path *path; int pos; } PathIter;
GP(GdipCreatePathIter, 2) {
    PATH(1);
    if (!ARG(0)) STATUS(InvalidParameter);
    PathIter *it = NEWOBJ(PathIter, K_PATHITER);
    if (!it) STATUS(OutOfMemory);
    it->path = path_clone(pa);
    wr_ptr(ARG(0), H2G(it));
    STATUS(Ok);
}
#define ITER(n) PathIter *it = OB(PathIter, n, K_PATHITER); if (!it) STATUS(InvalidParameter)
GP(GdipDeletePathIter, 1) { ITER(0); path_free(it->path); odel(it); STATUS(Ok); }
GP(GdipPathIterGetCount, 2) { ITER(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)it->path->n); STATUS(Ok); }
GP(GdipPathIterGetSubpathCount, 2) {
    ITER(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    int c = 0;
    for (int i = 0; i < it->path->n; i++) if ((it->path->types[i] & PT_MASK) == PT_START) c++;
    mem_wr(ARG(1), 4, (uint32_t)c);
    STATUS(Ok);
}
GP(GdipPathIterHasCurve, 2) {
    ITER(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    int c = 0;
    for (int i = 0; i < it->path->n; i++) if ((it->path->types[i] & PT_MASK) == PT_BEZIER) c = 1;
    mem_wr(ARG(1), 4, (uint32_t)c);
    STATUS(Ok);
}
GP(GdipPathIterRewind, 1) { ITER(0); it->pos = 0; STATUS(Ok); }
GP(GdipPathIterNextSubpath, 5) {
    ITER(0);
    if (!ARG(1) || !ARG(2) || !ARG(3) || !ARG(4)) STATUS(InvalidParameter);
    Path *p = it->path;
    if (it->pos >= p->n) { mem_wr(ARG(1), 4, 0); STATUS(Ok); }
    int s = it->pos, e = s + 1;
    while (e < p->n && (p->types[e] & PT_MASK) != PT_START) e++;
    mem_wr(ARG(2), 4, (uint32_t)s); mem_wr(ARG(3), 4, (uint32_t)(e - 1)); mem_wr(ARG(4), 4, (p->types[e - 1] & PT_CLOSE) != 0);
    mem_wr(ARG(1), 4, (uint32_t)(e - s));
    it->pos = e;
    STATUS(Ok);
}
GP(GdipPathIterEnumerate, 5) {
    ITER(0);
    int n = IA(4);
    if (!ARG(1) || !ARG(2) || !ARG(3) || n < it->path->n) STATUS(InvalidParameter);
    wr_pts(ARG(2), it->path->pts, it->path->n, 0);
    if (it->path->n) memcpy(G2H(ARG(3)), it->path->types, (size_t)it->path->n);
    mem_wr(ARG(1), 4, (uint32_t)it->path->n);
    STATUS(Ok);
}
GP(GdipPathIterCopyData, 6) {
    ITER(0);
    int s = IA(4), e = IA(5);
    if (!ARG(1) || !ARG(2) || !ARG(3) || s < 0 || e >= it->path->n || e < s) STATUS(InvalidParameter);
    wr_pts(ARG(2), it->path->pts + s, e - s + 1, 0);
    memcpy(G2H(ARG(3)), it->path->types + s, (size_t)(e - s + 1));
    mem_wr(ARG(1), 4, (uint32_t)(e - s + 1));
    STATUS(Ok);
}

// ---- regions -----------------------------------------------------------------------------------------------------------------

#define REGION(n) Region *rg = OB(Region, n, K_REGION); if (!rg) STATUS(InvalidParameter)
GP(GdipCreateRegion, 1) { if (!ARG(0)) STATUS(InvalidParameter); Region *r = region_new(RG_INF); if (!r) STATUS(OutOfMemory); wr_ptr(ARG(0), H2G(r)); STATUS(Ok); }
PAIR(GdipCreateRegionRect, 2) {
    float r[4];
    if (!ARG(0) || !ARG(1)) STATUS(InvalidParameter);
    for (int i = 0; i < 4; i++) r[i] = ii ? (float)rdi(ARG(0) + 4 * (uint64_t)i) : rdf(ARG(0) + 4 * (uint64_t)i);
    Region *rg = region_from_rect(r[0], r[1], r[2], r[3], NULL);
    if (!rg) STATUS(OutOfMemory);
    wr_ptr(ARG(1), H2G(rg));
    STATUS(Ok);
}
GP(GdipCreateRegionPath, 2) { PATH(0); if (!ARG(1)) STATUS(InvalidParameter); Region *r = region_from_path(pa, NULL); if (!r) STATUS(OutOfMemory); wr_ptr(ARG(1), H2G(r)); STATUS(Ok); }
GP(GdipCreateRegionRgnData, 3) { STATUS(NotImplemented); }
GP(GdipCreateRegionHrgn, 2) {
    GdiObj *o = gdi_get(ARG(0), GO_REGION);
    if (!o || !ARG(1)) STATUS(InvalidParameter);
    Region *r = region_from_rect((float)o->rx0, (float)o->ry0, (float)(o->rx1 - o->rx0), (float)(o->ry1 - o->ry0), NULL);
    if (!r) STATUS(OutOfMemory);
    wr_ptr(ARG(1), H2G(r));
    STATUS(Ok);
}
GP(GdipCloneRegion, 2) { REGION(0); if (!ARG(1)) STATUS(InvalidParameter); Region *c = region_clone(rg); if (!c) STATUS(OutOfMemory); wr_ptr(ARG(1), H2G(c)); STATUS(Ok); }
GP(GdipDeleteRegion, 1) { REGION(0); region_free(rg); STATUS(Ok); }
static void region_reset(Region *r, int type) {
    path_free(r->path); region_free(r->l); region_free(r->r);
    r->path = NULL; r->l = r->r = NULL; r->type = type;
}
GP(GdipSetInfinite, 1) { REGION(0); region_reset(rg, RG_INF); STATUS(Ok); }
GP(GdipSetEmpty, 1) { REGION(0); region_reset(rg, RG_EMPTY); STATUS(Ok); }
static void region_combine_inplace(Region *r, Region *other, int mode) {
    if (mode == 0) { // replace
        region_reset(r, RG_EMPTY);
        r->type = other->type; r->path = other->path; r->l = other->l; r->r = other->r; r->mode = other->mode;
        odel(other);
        return;
    }
    Region *copy = region_new(r->type);
    copy->path = r->path; copy->l = r->l; copy->r = r->r; copy->mode = r->mode;
    r->type = RG_COMB; r->path = NULL; r->l = copy; r->r = other; r->mode = mode;
}
PAIR(GdipCombineRegionRect, 3) {
    REGION(0);
    float r[4];
    if (!ARG(1) || IA(2) < 0 || IA(2) > 5) STATUS(InvalidParameter);
    for (int i = 0; i < 4; i++) r[i] = ii ? (float)rdi(ARG(1) + 4 * (uint64_t)i) : rdf(ARG(1) + 4 * (uint64_t)i);
    Region *o = region_from_rect(r[0], r[1], r[2], r[3], NULL);
    if (!o) STATUS(OutOfMemory);
    region_combine_inplace(rg, o, IA(2));
    STATUS(Ok);
}
GP(GdipCombineRegionPath, 3) {
    REGION(0); PATH(1);
    if (IA(2) < 0 || IA(2) > 5) STATUS(InvalidParameter);
    Region *o = region_from_path(pa, NULL);
    if (!o) STATUS(OutOfMemory);
    region_combine_inplace(rg, o, IA(2));
    STATUS(Ok);
}
GP(GdipCombineRegionRegion, 3) {
    REGION(0);
    Region *s = OB(Region, 1, K_REGION);
    if (!s || IA(2) < 0 || IA(2) > 5) STATUS(InvalidParameter);
    region_combine_inplace(rg, region_clone(s), IA(2));
    STATUS(Ok);
}
PAIR(GdipTranslateRegion, 3) { REGION(0); Mat m = mat_translate(AN(1), AN(2)); region_xform(rg, &m); STATUS(Ok); }
GP(GdipTransformRegion, 2) { REGION(0); MATRIX(1); region_xform(rg, &mo->m); STATUS(Ok); }
PAIR(GdipGetRegionBounds, 3) {
    REGION(0);
    if (!ARG(2)) STATUS(InvalidParameter);
    float x0, y0, x1, y1;
    int r = region_bounds(rg, &x0, &y0, &x1, &y1);
    if (r == -1) { x0 = -4194304; y0 = -4194304; x1 = 4194304; y1 = 4194304; }
    else if (r == 0) { x0 = y0 = x1 = y1 = 0; }
    if (ii) write_recti(ARG(2), x0, y0, x1 - x0, y1 - y0); else write_rectf(ARG(2), x0, y0, x1 - x0, y1 - y0);
    STATUS(Ok);
}
GP(GdipGetRegionHRgn, 3) {
    REGION(0);
    if (!ARG(2)) STATUS(InvalidParameter);
    float x0, y0, x1, y1;
    int r = region_bounds(rg, &x0, &y0, &x1, &y1);
    if (r == -1) { x0 = -4194304; y0 = -4194304; x1 = 4194304; y1 = 4194304; } else if (r == 0) { x0 = y0 = x1 = y1 = 0; }
    GdiObj *o;
    uint64_t h = gdi_alloc(GO_REGION, &o);
    if (!o) STATUS(OutOfMemory);
    o->rx0 = (int)floorf(x0); o->ry0 = (int)floorf(y0); o->rx1 = (int)ceilf(x1); o->ry1 = (int)ceilf(y1);
    wr_ptr(ARG(2), h);
    STATUS(Ok);
}
GP(GdipIsEmptyRegion, 3) {
    REGION(0);
    if (!ARG(2)) STATUS(InvalidParameter);
    float a, b, c, d;
    int r = region_bounds(rg, &a, &b, &c, &d);
    mem_wr(ARG(2), 4, r == 0 || (r == 1 && (c <= a || d <= b)));
    STATUS(Ok);
}
GP(GdipIsInfiniteRegion, 3) { REGION(0); if (!ARG(2)) STATUS(InvalidParameter); float a, b, c, d; mem_wr(ARG(2), 4, region_bounds(rg, &a, &b, &c, &d) == -1); STATUS(Ok); }
GP(GdipIsEqualRegion, 4) {
    REGION(0);
    Region *o = OB(Region, 1, K_REGION);
    if (!o || !ARG(3)) STATUS(InvalidParameter);
    float a, b, c, d, a2, b2, c2, d2;
    int r1 = region_bounds(rg, &a, &b, &c, &d), r2 = region_bounds(o, &a2, &b2, &c2, &d2);
    int eq;
    if (r1 != r2) eq = 0;
    else if (r1 != 1) eq = 1;
    else {
        int x0 = (int)floorf(fminf(a, a2)), y0 = (int)floorf(fminf(b, b2)), x1 = (int)ceilf(fmaxf(c, c2)), y1 = (int)ceilf(fmaxf(d, d2));
        int w = x1 - x0, h = y1 - y0;
        eq = 1;
        if (w > 0 && h > 0 && (int64_t)w * h < (1 << 24)) {
            uint8_t *m1 = malloc((size_t)w * (size_t)h), *m2 = malloc((size_t)w * (size_t)h);
            if (m1 && m2) { region_mask(rg, x0, y0, w, h, m1); region_mask(o, x0, y0, w, h, m2); eq = memcmp(m1, m2, (size_t)w * (size_t)h) == 0; }
            free(m1); free(m2);
        }
    }
    mem_wr(ARG(3), 4, eq);
    STATUS(Ok);
}
PAIR(GdipIsVisibleRegionPoint, 5) { REGION(0); if (!ARG(4)) STATUS(InvalidParameter); mem_wr(ARG(4), 4, region_has(rg, AN(1), AN(2))); STATUS(Ok); }
PAIR(GdipIsVisibleRegionRect, 7) {
    REGION(0);
    if (!ARG(6)) STATUS(InvalidParameter);
    float x = AN(1), y = AN(2), w = AN(3), h = AN(4);
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h), vis = 0;
    if (x1 > x0 && y1 > y0 && (int64_t)(x1 - x0) * (y1 - y0) < (1 << 24)) {
        uint8_t *m = malloc((size_t)(x1 - x0) * (size_t)(y1 - y0));
        if (m) { region_mask(rg, x0, y0, x1 - x0, y1 - y0, m); for (int i = 0; i < (x1 - x0) * (y1 - y0); i++) if (m[i]) { vis = 1; break; } free(m); }
    }
    mem_wr(ARG(6), 4, vis);
    STATUS(Ok);
}
GP(GdipGetRegionDataSize, 2) { REGION(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, 0); STATUS(NotImplemented); }
GP(GdipGetRegionData, 4) { STATUS(NotImplemented); }
GP(GdipGetRegionScansCount, 3) { STATUS(NotImplemented); }
GP(GdipGetRegionScans, 4) { STATUS(NotImplemented); }
GP(GdipGetRegionScansI, 4) { STATUS(NotImplemented); }

// ---- pixel format conversion ---------------------------------------------------------------------------------------------

static uint32_t premul(uint32_t p) {
    uint32_t a = p >> 24;
    if (a == 255) return p;
    return (a << 24) | ((((p >> 16) & 255) * a + 127) / 255 << 16) | ((((p >> 8) & 255) * a + 127) / 255 << 8) | (((p & 255) * a + 127) / 255);
}
static uint32_t unpremul(uint32_t p) {
    uint32_t a = p >> 24;
    if (a == 255 || a == 0) return a ? p : 0;
    uint32_t r = ((p >> 16) & 255) * 255 / a, g = ((p >> 8) & 255) * 255 / a, b = (p & 255) * 255 / a;
    return (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
}
static int pf_supported(int pf) {
    switch (pf) {
    case PF_ARGB: case PF_PARGB: case PF_32RGB: case PF_24: case PF_565: case PF_555: case PF_1555: case PF_8BPP: case PF_4BPP: case PF_1BPP:
    case PF_64ARGB: case PF_48: case PF_16GRAY: return 1;
    default: return 0;
    }
}
static int pf_stride(int pf, int w) { return ((w * pf_bpp(pf) + 31) / 32) * 4; }
static int pal_nearest(const uint32_t *pal, int n, uint32_t px) {
    int best = 0; long bd = 1L << 30;
    if ((px >> 24) < 128) for (int i = 0; i < n; i++) if ((pal[i] >> 24) < 128) return i;
    for (int i = 0; i < n; i++) {
        long dr = (long)((pal[i] >> 16) & 255) - (long)((px >> 16) & 255), dg = (long)((pal[i] >> 8) & 255) - (long)((px >> 8) & 255), db = (long)(pal[i] & 255) - (long)(px & 255);
        long d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
// ARGB row -> format row (dst is host memory)
static void row_from_argb(int pf, const Image *im, const uint32_t *src, uint8_t *dst, int w) {
    for (int x = 0; x < w; x++) {
        uint32_t p = src[x];
        switch (pf) {
        case PF_ARGB: memcpy(dst + x * 4, &p, 4); break;
        case PF_PARGB: { uint32_t q = premul(p); memcpy(dst + x * 4, &q, 4); break; }
        case PF_32RGB: { uint32_t q = p | 0xFF000000u; memcpy(dst + x * 4, &q, 4); break; }
        case PF_24: dst[x * 3] = (uint8_t)p; dst[x * 3 + 1] = (uint8_t)(p >> 8); dst[x * 3 + 2] = (uint8_t)(p >> 16); break;
        case PF_565: { uint16_t v = (uint16_t)((((p >> 19) & 31) << 11) | (((p >> 10) & 63) << 5) | ((p >> 3) & 31)); memcpy(dst + x * 2, &v, 2); break; }
        case PF_555: { uint16_t v = (uint16_t)((((p >> 19) & 31) << 10) | (((p >> 11) & 31) << 5) | ((p >> 3) & 31)); memcpy(dst + x * 2, &v, 2); break; }
        case PF_1555: { uint16_t v = (uint16_t)(((p >> 31) << 15) | (((p >> 19) & 31) << 10) | (((p >> 11) & 31) << 5) | ((p >> 3) & 31)); memcpy(dst + x * 2, &v, 2); break; }
        case PF_8BPP: dst[x] = (uint8_t)pal_nearest(im->pal, im->pal_n ? im->pal_n : 1, p); break;
        case PF_4BPP: { int i = pal_nearest(im->pal, im->pal_n ? im->pal_n : 1, p); if (x & 1) dst[x >> 1] |= (uint8_t)i; else dst[x >> 1] = (uint8_t)(i << 4); break; }
        case PF_1BPP: { int i = pal_nearest(im->pal, im->pal_n ? im->pal_n : 1, p); if (!(x & 7)) dst[x >> 3] = 0; if (i) dst[x >> 3] |= (uint8_t)(0x80 >> (x & 7)); break; }
        case PF_64ARGB: { uint16_t v[4] = { (uint16_t)((p & 255) * 257), (uint16_t)(((p >> 8) & 255) * 257), (uint16_t)(((p >> 16) & 255) * 257), (uint16_t)((p >> 24) * 257) }; memcpy(dst + x * 8, v, 8); break; }
        case PF_48: { uint16_t v[3] = { (uint16_t)(((p >> 16) & 255) * 257), (uint16_t)(((p >> 8) & 255) * 257), (uint16_t)((p & 255) * 257) }; memcpy(dst + x * 6, v, 6); break; }
        case PF_16GRAY: { uint32_t gr = (((p >> 16) & 255) * 77 + ((p >> 8) & 255) * 150 + (p & 255) * 29) >> 8; uint16_t v = (uint16_t)(gr * 257); memcpy(dst + x * 2, &v, 2); break; }
        }
    }
}
static void row_to_argb(int pf, const Image *im, const uint8_t *src, uint32_t *dst, int w) {
    for (int x = 0; x < w; x++) {
        uint32_t p;
        switch (pf) {
        case PF_ARGB: memcpy(&p, src + x * 4, 4); break;
        case PF_PARGB: memcpy(&p, src + x * 4, 4); p = unpremul(p); break;
        case PF_32RGB: memcpy(&p, src + x * 4, 4); p |= 0xFF000000u; break;
        case PF_24: p = 0xFF000000u | ((uint32_t)src[x * 3 + 2] << 16) | ((uint32_t)src[x * 3 + 1] << 8) | src[x * 3]; break;
        case PF_565: { uint16_t v; memcpy(&v, src + x * 2, 2); uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31; p = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2)); break; }
        case PF_555: { uint16_t v; memcpy(&v, src + x * 2, 2); uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31; p = 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2)); break; }
        case PF_1555: { uint16_t v; memcpy(&v, src + x * 2, 2); uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31; p = ((v >> 15) ? 0xFF000000u : 0) | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) | ((b << 3) | (b >> 2)); break; }
        case PF_8BPP: p = im->pal[src[x]]; if (!im->pal_n) p = 0xFF000000u | (uint32_t)src[x] * 0x10101u; break;
        case PF_4BPP: { int i = (x & 1) ? (src[x >> 1] & 15) : (src[x >> 1] >> 4); p = im->pal_n ? im->pal[i] : 0xFF000000u | (uint32_t)(i * 17) * 0x10101u; break; }
        case PF_1BPP: { int i = (src[x >> 3] >> (7 - (x & 7))) & 1; p = im->pal_n ? im->pal[i] : (i ? 0xFFFFFFFFu : 0xFF000000u); break; }
        case PF_64ARGB: { uint16_t v[4]; memcpy(v, src + x * 8, 8); p = ((uint32_t)(v[3] >> 8) << 24) | ((uint32_t)(v[2] >> 8) << 16) | ((uint32_t)(v[1] >> 8) << 8) | (v[0] >> 8); break; }
        case PF_48: { uint16_t v[3]; memcpy(v, src + x * 6, 6); p = 0xFF000000u | ((uint32_t)(v[0] >> 8) << 16) | ((uint32_t)(v[1] >> 8) << 8) | (v[2] >> 8); break; }
        case PF_16GRAY: { uint16_t v; memcpy(&v, src + x * 2, 2); p = 0xFF000000u | (uint32_t)(v >> 8) * 0x10101u; break; }
        default: p = 0;
        }
        dst[x] = p;
    }
}
static int pf_has_alpha(int pf) { return (pf & 0x40000) != 0; }
static void image_default_palette(Image *im) {
    int n = im->pixfmt == PF_1BPP ? 2 : im->pixfmt == PF_4BPP ? 16 : im->pixfmt == PF_8BPP ? 256 : 0;
    if (!n || im->pal_n) return;
    im->pal_n = n;
    for (int i = 0; i < n; i++) { uint32_t v = (uint32_t)(i * 255 / (n - 1)); im->pal[i] = 0xFF000000u | v * 0x10101u; }
}

// ---- image loading -------------------------------------------------------------------------------------------------------

static uint8_t *read_file(const char *host, size_t *len, int *err) {
    int fd = open(host, O_RDONLY);
    if (fd < 0) { *err = errno == EACCES ? AccessDenied : FileNotFound; return NULL; }
    struct stat sb;
    if (fstat(fd, &sb) != 0 || S_ISDIR(sb.st_mode) || sb.st_size > ((off_t)1 << 30)) { close(fd); *err = GenericError; return NULL; }
    uint8_t *buf = malloc((size_t)sb.st_size + 1);
    if (!buf) { close(fd); *err = OutOfMemory; return NULL; }
    size_t got = 0;
    while (got < (size_t)sb.st_size) { ssize_t r = read(fd, buf + got, (size_t)sb.st_size - got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    *len = got;
    return buf;
}
static int load_file_image(uint64_t wpath, Image **out, int file_err) {
    if (!wpath) return InvalidParameter;
    char *u = w2u_dup((const uint16_t *)G2H(wpath));
    char host[2600];
    path_w2h(u, host, sizeof host);
    free(u);
    size_t n; int err = 0;
    uint8_t *buf = read_file(host, &n, &err);
    if (!buf) return err;
    Img *d = img_decode(buf, n);
    free(buf);
    if (!d) return file_err;
    Image *im = image_from_img(d);
    if (!im) return OutOfMemory;
    *out = im;
    return Ok;
}
static int load_stream_image(X86State *st, uint64_t pstm, Image **out) {
    if (!pstm) return InvalidParameter;
    size_t n;
    uint8_t *buf = stream_read_all(st, pstm, &n);
    if (!buf) return InvalidParameter;
    Img *d = img_decode(buf, n);
    free(buf);
    if (!d) return InvalidParameter;
    Image *im = image_from_img(d);
    if (!im) return OutOfMemory;
    *out = im;
    return Ok;
}
GP(GdipLoadImageFromFile, 2) { Image *im = NULL; int r = load_file_image(ARG(0), &im, OutOfMemory); if (!ARG(1)) STATUS(InvalidParameter); if (r == Ok) wr_ptr(ARG(1), H2G(im)); STATUS(r); }
GP(GdipLoadImageFromFileICM, 2) { Image *im = NULL; int r = load_file_image(ARG(0), &im, OutOfMemory); if (!ARG(1)) STATUS(InvalidParameter); if (r == Ok) wr_ptr(ARG(1), H2G(im)); STATUS(r); }
GP(GdipCreateBitmapFromFile, 2) { Image *im = NULL; int r = load_file_image(ARG(0), &im, OutOfMemory); if (!ARG(1)) STATUS(InvalidParameter); if (r == Ok) wr_ptr(ARG(1), H2G(im)); STATUS(r); }
GP(GdipCreateBitmapFromFileICM, 2) { Image *im = NULL; int r = load_file_image(ARG(0), &im, OutOfMemory); if (!ARG(1)) STATUS(InvalidParameter); if (r == Ok) wr_ptr(ARG(1), H2G(im)); STATUS(r); }
GP(GdipLoadImageFromStream, 2) { Image *im = NULL; if (!ARG(1)) STATUS(InvalidParameter); int r = load_stream_image(st, ARG(0), &im); if (r == Ok) wr_ptr(ARG(1), H2G(im)); STATUS(r); }
GP(GdipLoadImageFromStreamICM, 2) { Image *im = NULL; if (!ARG(1)) STATUS(InvalidParameter); int r = load_stream_image(st, ARG(0), &im); if (r == Ok) wr_ptr(ARG(1), H2G(im)); STATUS(r); }
GP(GdipCreateBitmapFromStream, 2) { Image *im = NULL; if (!ARG(1)) STATUS(InvalidParameter); int r = load_stream_image(st, ARG(0), &im); if (r == Ok) wr_ptr(ARG(1), H2G(im)); STATUS(r); }
GP(GdipCreateBitmapFromStreamICM, 2) { Image *im = NULL; if (!ARG(1)) STATUS(InvalidParameter); int r = load_stream_image(st, ARG(0), &im); if (r == Ok) wr_ptr(ARG(1), H2G(im)); STATUS(r); }

GP(GdipCreateBitmapFromScan0, 6) {
    int w = IA(0), h = IA(1), stride = IA(2), pf = IA(3);
    uint64_t scan0 = ARG(4);
    if (!ARG(5) || w <= 0 || h <= 0) STATUS(InvalidParameter);
    if (!pf_supported(pf)) STATUS(InvalidParameter);
    if (scan0 && (stride == 0 || abs(stride) < ((w * pf_bpp(pf) + 7) / 8))) STATUS(InvalidParameter);
    Image *im = image_new(w, h, pf);
    if (!im) STATUS(OutOfMemory);
    image_default_palette(im);
    if (scan0) {
        for (int y = 0; y < h; y++) row_to_argb(pf, im, (const uint8_t *)G2H(scan0 + (uint64_t)((int64_t)stride * y)), im->px + (size_t)y * (size_t)w, w);
    } else if (!pf_has_alpha(pf) && pf_bpp(pf) > 8) for (size_t i = 0; i < (size_t)w * (size_t)h; i++) im->px[i] = 0xFF000000u; // black
    else if (pf_bpp(pf) <= 8 && im->pal_n) for (size_t i = 0; i < (size_t)w * (size_t)h; i++) im->px[i] = im->pal[0];
    wr_ptr(ARG(5), H2G(im));
    STATUS(Ok);
}
GP(GdipCreateBitmapFromGraphics, 4) {
    Graphics *g = OB(Graphics, 2, K_GRAPHICS);
    if (!g || !ARG(3) || IA(0) <= 0 || IA(1) <= 0) STATUS(InvalidParameter);
    Image *im = image_new(IA(0), IA(1), PF_PARGB == 0 ? PF_ARGB : PF_ARGB);
    if (!im) STATUS(OutOfMemory);
    im->dpix = g->dpix; im->dpiy = g->dpiy;
    wr_ptr(ARG(3), H2G(im));
    STATUS(Ok);
}
GP(GdipCreateBitmapFromHBITMAP, 3) {
    uint32_t *px; int w, h;
    if (!ARG(2) || !gdi_bitmap_argb(ARG(0), &px, &w, &h)) STATUS(InvalidParameter);
    int alpha = 0;
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) if ((px[i] >> 24) != 255) { alpha = 1; break; }
    Image *im = image_new(w, h, alpha ? PF_ARGB : PF_32RGB);
    if (!im) { free(px); STATUS(OutOfMemory); }
    memcpy(im->px, px, (size_t)w * (size_t)h * 4);
    free(px);
    wr_ptr(ARG(2), H2G(im));
    STATUS(Ok);
}
GP(GdipCreateBitmapFromGdiDib, 3) {
    if (!ARG(0) || !ARG(1) || !ARG(2)) STATUS(InvalidParameter);
    uint64_t hb = gdi_bitmap_from_dib(PTR(0), PTR(1));
    uint32_t *px; int w, h;
    if (!hb || !gdi_bitmap_argb(hb, &px, &w, &h)) STATUS(InvalidParameter);
    gdi_free(hb);
    Image *im = image_new(w, h, PF_24);
    if (!im) { free(px); STATUS(OutOfMemory); }
    memcpy(im->px, px, (size_t)w * (size_t)h * 4);
    free(px);
    wr_ptr(ARG(2), H2G(im));
    STATUS(Ok);
}
GP(GdipCreateBitmapFromHICON, 2) { STATUS(NotImplemented); }
GP(GdipCreateBitmapFromResource, 3) { // RT_BITMAP resources (DIBs without a file header)
    uint64_t hinst = ARG(0), name = ARG(1);
    if (!ARG(2) || !name) STATUS(InvalidParameter);
    uint64_t base = hinst ? hinst : (g_proc->main_module ? g_proc->main_module->base : 0);
    uint64_t e = base ? pe_resource_entry(base, 2, name, 1) : 0;
    uint32_t sz = 0;
    uint64_t d = e ? pe_resource_data(base, e, &sz) : 0;
    if (!d || sz < 12) STATUS(InvalidParameter);
    uint64_t hb = gdi_bitmap_from_dib(G2H(d), NULL);
    uint32_t *px; int w, h;
    if (!hb || !gdi_bitmap_argb(hb, &px, &w, &h)) STATUS(InvalidParameter);
    gdi_free(hb);
    Image *im = image_new(w, h, PF_24);
    if (!im) { free(px); STATUS(OutOfMemory); }
    memcpy(im->px, px, (size_t)w * (size_t)h * 4);
    free(px);
    wr_ptr(ARG(2), H2G(im));
    STATUS(Ok);
}
GP(GdipCreateBitmapFromDirectDrawSurface, 2) { STATUS(NotImplemented); }
GP(GdipCreateHBITMAPFromBitmap, 3) {
    Image *im = OB(Image, 0, K_IMAGE);
    if (!im || !ARG(1)) STATUS(InvalidParameter);
    size_t n = (size_t)im->w * (size_t)im->h;
    uint32_t *tmp = malloc(n * 4);
    if (!tmp) STATUS(OutOfMemory);
    uint32_t bg = (uint32_t)ARG(2);
    for (size_t i = 0; i < n; i++) {
        uint32_t p = im->px[i];
        if ((p >> 24) != 255) p = blend_over(bg | 0xFF000000u, p, 255);
        tmp[i] = p | 0xFF000000u;
    }
    uint64_t hb = gdi_bitmap_from_argb(tmp, im->w, im->h);
    free(tmp);
    if (!hb) STATUS(OutOfMemory);
    wr_ptr(ARG(1), hb);
    STATUS(Ok);
}
GP(GdipCreateHICONFromBitmap, 2) { STATUS(NotImplemented); }
GP(GdipCloneImage, 2) { Image *im = OB(Image, 0, K_IMAGE); if (!im || !ARG(1)) STATUS(InvalidParameter); Image *c = image_clone(im); if (!c) STATUS(OutOfMemory); wr_ptr(ARG(1), H2G(c)); STATUS(Ok); }
GP(GdipDisposeImage, 1) { Image *im = OB(Image, 0, K_IMAGE); if (!im) STATUS(InvalidParameter); if (im->gdc > 0) STATUS(ObjectBusy); image_free(im); STATUS(Ok); }
PAIR(GdipCloneBitmapArea, 7) {
    Image *src = OB(Image, 5, K_IMAGE);
    if (!src || !ARG(6)) STATUS(InvalidParameter);
    int x = (int)AN(0), y = (int)AN(1), w = (int)AN(2), h = (int)AN(3), pf = IA(4);
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > src->w || y + h > src->h) STATUS(InvalidParameter);
    if (!pf_supported(pf)) pf = src->pixfmt;
    Image *im = image_new(w, h, pf);
    if (!im) STATUS(OutOfMemory);
    for (int j = 0; j < h; j++) memcpy(im->px + (size_t)j * (size_t)w, src->px + (size_t)(y + j) * (size_t)src->w + (size_t)x, (size_t)w * 4);
    if (!pf_has_alpha(pf)) for (size_t i = 0; i < (size_t)w * (size_t)h; i++) im->px[i] |= 0xFF000000u;
    im->dpix = src->dpix; im->dpiy = src->dpiy;
    if (src->pal_n && pf_bpp(pf) <= 8) { memcpy(im->pal, src->pal, sizeof im->pal); im->pal_n = src->pal_n; }
    wr_ptr(ARG(6), H2G(im));
    STATUS(Ok);
}

// ---- image properties -------------------------------------------------------------------------------------------------------

#define IMAGE(n) Image *im = OB(Image, n, K_IMAGE); if (!im) STATUS(InvalidParameter)
GP(GdipGetImageWidth, 2) { IMAGE(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)im->w); STATUS(Ok); }
GP(GdipGetImageHeight, 2) { IMAGE(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)im->h); STATUS(Ok); }
GP(GdipGetImageDimension, 3) { IMAGE(0); if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter); wrf(ARG(1), (float)im->w); wrf(ARG(2), (float)im->h); STATUS(Ok); }
GP(GdipGetImageBounds, 3) { IMAGE(0); if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter); write_rectf(ARG(1), 0, 0, (float)im->w, (float)im->h); mem_wr(ARG(2), 4, 2); STATUS(Ok); }
GP(GdipGetImagePixelFormat, 2) { IMAGE(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)im->pixfmt); STATUS(Ok); }
GP(GdipGetImageType, 2) { IMAGE(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, 1); STATUS(Ok); }
GP(GdipGetImageFlags, 2) {
    IMAGE(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    uint32_t f = 0x10; // ColorSpaceRGB
    int alpha = 0, trans = 0;
    for (size_t i = 0; i < (size_t)im->w * (size_t)im->h; i++) { uint32_t a = im->px[i] >> 24; if (a != 255) { alpha = 1; if (a) trans = 1; } }
    if (alpha || pf_has_alpha(im->pixfmt)) f |= 2;
    if (trans) f |= 4;
    if (im->rawfmt) f |= 0x10000 * 0 + 0x20000 * 0;
    mem_wr(ARG(1), 4, f);
    STATUS(Ok);
}
static const uint8_t FMT_GUIDS[8][16] = { // BMP, JPEG, PNG, GIF, TIFF, MemoryBMP, ICO, Undefined
    { 0xAB, 0x3C, 0x6B, 0xB9, 0x28, 0x07, 0xD3, 0x11, 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E },
    { 0xAE, 0x3C, 0x6B, 0xB9, 0x28, 0x07, 0xD3, 0x11, 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E },
    { 0xAF, 0x3C, 0x6B, 0xB9, 0x28, 0x07, 0xD3, 0x11, 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E },
    { 0xB0, 0x3C, 0x6B, 0xB9, 0x28, 0x07, 0xD3, 0x11, 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E },
    { 0xB1, 0x3C, 0x6B, 0xB9, 0x28, 0x07, 0xD3, 0x11, 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E },
    { 0xAA, 0x3C, 0x6B, 0xB9, 0x28, 0x07, 0xD3, 0x11, 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E },
    { 0xB5, 0x3C, 0x6B, 0xB9, 0x28, 0x07, 0xD3, 0x11, 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E },
    { 0xA9, 0x3C, 0x6B, 0xB9, 0x28, 0x07, 0xD3, 0x11, 0x9D, 0x7B, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
static const int FMT_OF[8] = { IMG_BMP, IMG_JPEG, IMG_PNG, IMG_GIF, IMG_TIFF, 0, IMG_ICO, -1 };
GP(GdipGetImageRawFormat, 2) {
    IMAGE(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    int idx = 5;
    for (int i = 0; i < 8; i++) if (FMT_OF[i] == im->rawfmt && im->rawfmt) idx = i;
    memcpy(G2H(ARG(1)), FMT_GUIDS[idx], 16);
    STATUS(Ok);
}
GET_FLT(GdipGetImageHorizontalResolution, Image, K_IMAGE, o->dpix)
GET_FLT(GdipGetImageVerticalResolution, Image, K_IMAGE, o->dpiy)
GP(GdipBitmapSetResolution, 3) { IMAGE(0); float x = FA(1), y = FA(2); if (x <= 0 || y <= 0) STATUS(InvalidParameter); im->dpix = x; im->dpiy = y; STATUS(Ok); }
GP(GdipImageForceValidation, 1) { IMAGE(0); STATUS(Ok); }

// lock bits
GP(GdipBitmapLockBits, 5) {
    IMAGE(0);
    uint64_t rc = ARG(1), bd = ARG(4);
    int mode = IA(2), pf = IA(3);
    if (!bd || !(mode & 3) || !pf_supported(pf)) STATUS(InvalidParameter);
    int x = 0, y = 0, w = im->w, h = im->h;
    if (rc) { x = rdi(rc); y = rdi(rc + 4); w = rdi(rc + 8); h = rdi(rc + 12); }
    if (w <= 0 || h <= 0 || x < 0 || y < 0 || x + w > im->w || y + h > im->h) STATUS(InvalidParameter);
    if (im->nlk >= 8) STATUS(Win32Error);
    const char *L = "4444PP";
    LockRec *lk = &im->lk[im->nlk];
    memset(lk, 0, sizeof *lk);
    lk->x = x; lk->y = y; lk->w = w; lk->h = h; lk->fmt = pf; lk->mode = mode;
    int stride = pf_stride(pf, w);
    if (mode & 4) { // user supplied buffer
        lk->user = 1;
        lk->scan0 = gs_get(bd, L, 4); stride = (int32_t)gs_get(bd, L, 2);
        if (!lk->scan0 || stride == 0) STATUS(InvalidParameter);
        lk->stride = stride;
        lk->buf = G2H(lk->scan0);
    } else if (pf == PF_ARGB) {
        lk->buf = (uint8_t *)(im->px + (size_t)y * (size_t)im->w + (size_t)x);
        stride = im->w * 4; lk->stride = stride; lk->scan0 = H2G(lk->buf); lk->user = 2; // direct access
    } else {
        lk->buf = g_calloc(1, (size_t)stride * (size_t)h);
        if (!lk->buf) STATUS(OutOfMemory);
        lk->scan0 = H2G(lk->buf); lk->stride = stride;
    }
    if ((mode & 1) && lk->user != 2) {
        image_default_palette(im);
        for (int j = 0; j < h; j++) row_from_argb(pf, im, im->px + (size_t)(y + j) * (size_t)im->w + (size_t)x, lk->buf + (int64_t)lk->stride * j, w);
    }
    im->nlk++;
    gs_set(bd, L, 0, (uint64_t)w); gs_set(bd, L, 1, (uint64_t)h); gs_set(bd, L, 2, (uint32_t)stride); gs_set(bd, L, 3, (uint64_t)pf);
    gs_set(bd, L, 4, lk->scan0); gs_set(bd, L, 5, 0);
    STATUS(Ok);
}
GP(GdipBitmapUnlockBits, 2) {
    IMAGE(0);
    uint64_t bd = ARG(1);
    if (!bd) STATUS(InvalidParameter);
    uint64_t scan0 = gs_get(bd, "4444PP", 4);
    int idx = -1;
    for (int i = 0; i < im->nlk; i++) if (im->lk[i].scan0 == scan0) idx = i;
    if (idx < 0) STATUS(InvalidParameter);
    LockRec *lk = &im->lk[idx];
    if ((lk->mode & 2) && lk->user != 2) {
        for (int j = 0; j < lk->h; j++) row_to_argb(lk->fmt, im, lk->buf + (int64_t)lk->stride * j, im->px + (size_t)(lk->y + j) * (size_t)im->w + (size_t)lk->x, lk->w);
        if (!pf_has_alpha(im->pixfmt)) for (int j = 0; j < lk->h; j++) for (int i = 0; i < lk->w; i++) im->px[(size_t)(lk->y + j) * (size_t)im->w + (size_t)(lk->x + i)] |= 0xFF000000u;
    }
    if (!lk->user) g_free(lk->buf);
    for (int i = idx; i + 1 < im->nlk; i++) im->lk[i] = im->lk[i + 1];
    im->nlk--;
    STATUS(Ok);
}
GP(GdipBitmapGetPixel, 4) {
    IMAGE(0);
    int x = IA(1), y = IA(2);
    if (!ARG(3) || x < 0 || y < 0 || x >= im->w || y >= im->h) STATUS(InvalidParameter);
    mem_wr(ARG(3), 4, im->px[(size_t)y * (size_t)im->w + (size_t)x]);
    STATUS(Ok);
}
GP(GdipBitmapSetPixel, 4) {
    IMAGE(0);
    int x = IA(1), y = IA(2);
    if (x < 0 || y < 0 || x >= im->w || y >= im->h) STATUS(InvalidParameter);
    uint32_t p = (uint32_t)ARG(3);
    if (!pf_has_alpha(im->pixfmt)) p |= 0xFF000000u;
    im->px[(size_t)y * (size_t)im->w + (size_t)x] = p;
    STATUS(Ok);
}

// frames
static const uint8_t DIM_TIME[16] = { 0x6D, 0xBD, 0xED, 0x6A, 0xB5, 0x3F, 0x8A, 0x41, 0x83, 0xA6, 0x7F, 0x45, 0x22, 0x9D, 0xC8, 0x72 };
static const uint8_t DIM_PAGE[16] = { 0x86, 0xDC, 0x62, 0x74, 0x80, 0x61, 0x7E, 0x4C, 0x8E, 0x3F, 0xEE, 0x73, 0x33, 0xA7, 0xA4, 0x83 };
GP(GdipImageGetFrameDimensionsCount, 2) { IMAGE(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, 1); STATUS(Ok); }
GP(GdipImageGetFrameDimensionsList, 3) {
    IMAGE(0);
    if (!ARG(1) || IA(2) != 1) STATUS(InvalidParameter);
    memcpy(G2H(ARG(1)), im->rawfmt == IMG_GIF ? DIM_TIME : DIM_PAGE, 16);
    STATUS(Ok);
}
static int frame_dim_ok(Image *im, uint64_t g) { return g && memcmp(G2H(g), im->rawfmt == IMG_GIF ? DIM_TIME : DIM_PAGE, 16) == 0; }
GP(GdipImageGetFrameCount, 3) {
    IMAGE(0);
    if (!ARG(2) || !frame_dim_ok(im, ARG(1))) STATUS(InvalidParameter);
    mem_wr(ARG(2), 4, (uint32_t)(im->src ? im->src->nframes : 1));
    STATUS(Ok);
}
GP(GdipImageSelectActiveFrame, 3) {
    IMAGE(0);
    int idx = IA(2);
    int nf = im->src ? im->src->nframes : 1;
    if (!frame_dim_ok(im, ARG(1)) || idx < 0 || idx >= nf) STATUS(InvalidParameter);
    if (im->src) { memcpy(im->px, im->src->fr[idx].px, (size_t)im->w * (size_t)im->h * 4); im->frame = idx; }
    STATUS(Ok);
}

// rotate / flip
GP(GdipImageRotateFlip, 2) {
    IMAGE(0);
    int t = IA(1);
    if (t < 0 || t > 7) STATUS(InvalidParameter);
    int rot = t & 3, flip = t & 4, w = im->w, h = im->h;
    int nw = (rot & 1) ? h : w, nh = (rot & 1) ? w : h;
    uint32_t *np = g_calloc((size_t)nw * (size_t)nh, 4);
    if (!np) STATUS(OutOfMemory);
    for (int y = 0; y < nh; y++)
        for (int x = 0; x < nw; x++) {
            int sx = x, sy = y;
            if (flip) sx = nw - 1 - sx;
            int ox, oy; // source coordinates of the rotated image
            switch (rot) {
            case 0: ox = sx; oy = sy; break;
            case 1: ox = sy; oy = h - 1 - sx; break;
            case 2: ox = w - 1 - sx; oy = h - 1 - sy; break;
            default: ox = w - 1 - sy; oy = sx; break;
            }
            np[(size_t)y * (size_t)nw + (size_t)x] = im->px[(size_t)oy * (size_t)w + (size_t)ox];
        }
    g_free(im->px);
    im->px = np; im->w = nw; im->h = nh;
    if (rot & 1) { float t2 = im->dpix; im->dpix = im->dpiy; im->dpiy = t2; }
    img_free(im->src); im->src = NULL;
    STATUS(Ok);
}
static Image *scale_image(const Image *s, int dw, int dh) {
    Image *im = image_new(dw, dh, PF_ARGB);
    if (!im) return NULL;
    float fx = (float)s->w / (float)dw, fy = (float)s->h / (float)dh;
    int bx = fx > 1.0f ? (int)ceilf(fx) : 1, by = fy > 1.0f ? (int)ceilf(fy) : 1;
    if (bx > 8) bx = 8;
    if (by > 8) by = 8;
    for (int y = 0; y < dh; y++)
        for (int x = 0; x < dw; x++) {
            float acc[4] = { 0, 0, 0, 0 }; int cnt = 0;
            for (int j = 0; j < by; j++) for (int i = 0; i < bx; i++) {
                float sxp = ((float)x + ((float)i + 0.5f) / (float)bx) * fx, syp = ((float)y + ((float)j + 0.5f) / (float)by) * fy;
                int ix = (int)sxp, iy = (int)syp;
                if (ix >= s->w) ix = s->w - 1;
                if (iy >= s->h) iy = s->h - 1;
                uint32_t c = s->px[(size_t)iy * (size_t)s->w + (size_t)ix];
                float a = (float)(c >> 24);
                acc[0] += (float)((c >> 16) & 255) * a; acc[1] += (float)((c >> 8) & 255) * a; acc[2] += (float)(c & 255) * a; acc[3] += a; cnt++;
            }
            uint32_t o = 0;
            if (acc[3] > 0) { o = ((uint32_t)(acc[3] / (float)cnt + 0.5f) << 24); for (int k = 0; k < 3; k++) o |= (uint32_t)(acc[k] / acc[3] + 0.5f) << (16 - 8 * k); }
            im->px[(size_t)y * (size_t)dw + (size_t)x] = o;
        }
    return im;
}
GP(GdipGetImageThumbnail, 6) {
    IMAGE(0);
    int w = IA(1), h = IA(2);
    if (!ARG(3)) STATUS(InvalidParameter);
    if (w <= 0) w = 120;
    if (h <= 0) h = 120;
    Image *t = scale_image(im, w, h);
    if (!t) STATUS(OutOfMemory);
    t->pixfmt = PF_ARGB;
    wr_ptr(ARG(3), H2G(t));
    STATUS(Ok);
}

// palettes
GP(GdipGetImagePaletteSize, 2) {
    IMAGE(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    if (pf_bpp(im->pixfmt) > 8 || !(im->pixfmt & 0x10000)) { mem_wr(ARG(1), 4, 0); STATUS(GenericError); }
    mem_wr(ARG(1), 4, 8 + 4 * (uint32_t)im->pal_n);
    STATUS(Ok);
}
GP(GdipGetImagePalette, 3) {
    IMAGE(0);
    int size = IA(2);
    if (!ARG(1) || !(im->pixfmt & 0x10000) || pf_bpp(im->pixfmt) > 8) STATUS(GenericError);
    if (size < 8 + 4 * im->pal_n) STATUS(InsufficientBuffer);
    mem_wr(ARG(1), 4, im->pal_flags); mem_wr(ARG(1) + 4, 4, (uint32_t)im->pal_n);
    for (int i = 0; i < im->pal_n; i++) mem_wr(ARG(1) + 8 + 4 * (uint64_t)i, 4, im->pal[i]);
    STATUS(Ok);
}
GP(GdipSetImagePalette, 2) {
    IMAGE(0);
    if (!ARG(1) || !(im->pixfmt & 0x10000) || pf_bpp(im->pixfmt) > 8) STATUS(GenericError);
    int n = (int)mem_rd(ARG(1) + 4, 4);
    if (n < 0 || n > 256) STATUS(InvalidParameter);
    im->pal_flags = (int)mem_rd(ARG(1), 4); im->pal_n = n;
    for (int i = 0; i < n; i++) im->pal[i] = (uint32_t)mem_rd(ARG(1) + 8 + 4 * (uint64_t)i, 4);
    STATUS(Ok);
}

// properties (GIF frame delays and loop count)
static int prop_count(Image *im) { return (im->src && im->rawfmt == IMG_GIF) ? 2 : 0; }
GP(GdipGetPropertyCount, 2) { IMAGE(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)prop_count(im)); STATUS(Ok); }
GP(GdipGetPropertyIdList, 3) {
    IMAGE(0);
    int n = IA(1);
    if (n != prop_count(im)) STATUS(InvalidParameter);
    if (n && ARG(2)) { mem_wr(ARG(2), 4, 0x5100); mem_wr(ARG(2) + 4, 4, 0x5101); }
    STATUS(Ok);
}
static int prop_size(Image *im, uint32_t id) {
    if (!im->src || im->rawfmt != IMG_GIF) return -1;
    if (id == 0x5100) return 4 * im->src->nframes;
    if (id == 0x5101) return 2;
    return -1;
}
static size_t PI_SIZE(void) { return gs_size("44" "2" "P"); }
GP(GdipGetPropertyItemSize, 3) {
    IMAGE(0);
    int s = prop_size(im, (uint32_t)ARG(1));
    if (!ARG(2)) STATUS(InvalidParameter);
    if (s < 0) STATUS(PropertyNotFound);
    mem_wr(ARG(2), 4, (uint32_t)(PI_SIZE() + (size_t)s));
    STATUS(Ok);
}
static void prop_fill(Image *im, uint32_t id, uint64_t buf) {
    const char *L = "442P";
    int s = prop_size(im, id);
    uint64_t data = buf + PI_SIZE();
    gs_set(buf, L, 0, id); gs_set(buf, L, 1, (uint64_t)s); gs_set(buf, L, 3, data);
    if (id == 0x5100) { gs_set(buf, L, 2, 4); for (int i = 0; i < im->src->nframes; i++) mem_wr(data + 4 * (uint64_t)i, 4, (uint32_t)im->src->fr[i].delay_cs); }
    else { gs_set(buf, L, 2, 3); mem_wr(data, 2, (uint32_t)im->src->loop); }
}
GP(GdipGetPropertyItem, 4) {
    IMAGE(0);
    int s = prop_size(im, (uint32_t)ARG(1));
    if (!ARG(3)) STATUS(InvalidParameter);
    if (s < 0) STATUS(PropertyNotFound);
    if ((size_t)IA(2) != PI_SIZE() + (size_t)s) STATUS(InvalidParameter);
    prop_fill(im, (uint32_t)ARG(1), ARG(3));
    STATUS(Ok);
}
GP(GdipGetPropertySize, 3) {
    IMAGE(0);
    if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter);
    uint32_t tot = 0;
    for (int i = 0; i < prop_count(im); i++) tot += (uint32_t)(PI_SIZE() + (size_t)prop_size(im, i ? 0x5101 : 0x5100));
    mem_wr(ARG(1), 4, tot); mem_wr(ARG(2), 4, (uint32_t)prop_count(im));
    STATUS(Ok);
}
GP(GdipGetAllPropertyItems, 4) {
    IMAGE(0);
    int n = prop_count(im);
    if (!ARG(3) || IA(2) != n) STATUS(InvalidParameter);
    uint64_t data = ARG(3) + (uint64_t)n * PI_SIZE();
    for (int i = 0; i < n; i++) {
        uint32_t id = i ? 0x5101 : 0x5100;
        uint64_t item = ARG(3) + (uint64_t)i * PI_SIZE();
        prop_fill(im, id, item); // writes the value after the item; move it to the shared data area
        int s = prop_size(im, id);
        uint64_t src = item + PI_SIZE();
        uint8_t tmp[4096]; if ((size_t)s > sizeof tmp) STATUS(GenericError);
        memcpy(tmp, G2H(src), (size_t)s);
        memcpy(G2H(data), tmp, (size_t)s);
        gs_set(item, "442P", 3, data);
        data += (uint64_t)s;
    }
    STATUS(Ok);
}
GP(GdipRemovePropertyItem, 2) { STATUS(NotImplemented); }
GP(GdipSetPropertyItem, 2) { STATUS(NotImplemented); }

// ---- codecs info and saving ---------------------------------------------------------------------------------------------------

typedef struct CodecDef { const char *name, *ext, *mime, *desc; uint8_t clsid[16]; int fmt; int fidx; uint32_t flags; int enc; } CodecDef;
#define CLS(n) { 0x00 + (n), 0xF4, 0x7C, 0x55, 0x04, 0x1A, 0xD3, 0x11, 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E }
static const CodecDef CODECS[] = {
    { "Built-in BMP Codec", "*.BMP;*.DIB;*.RLE", "image/bmp", "BMP", CLS(0), IMG_BMP, 0, 0x10007 | 0x10, 1 },
    { "Built-in JPEG Codec", "*.JPG;*.JPEG;*.JPE;*.JFIF", "image/jpeg", "JPEG", CLS(1), IMG_JPEG, 1, 0x10007 | 0x10, 1 },
    { "Built-in GIF Codec", "*.GIF", "image/gif", "GIF", CLS(2), IMG_GIF, 3, 0x10007 | 0x10, 1 },
    { "Built-in EMF Codec", "*.EMF", "image/x-emf", "EMF", CLS(3), -1, 7, 0x1000E, 0 },
    { "Built-in WMF Codec", "*.WMF", "image/x-wmf", "WMF", CLS(4), -1, 7, 0x1000E, 0 },
    { "Built-in TIFF Codec", "*.TIF;*.TIFF", "image/tiff", "TIFF", CLS(5), IMG_TIFF, 4, 0x10007 | 0x10, 1 },
    { "Built-in PNG Codec", "*.PNG", "image/png", "PNG", CLS(6), IMG_PNG, 2, 0x10007 | 0x10, 1 },
    { "Built-in ICO Codec", "*.ICO", "image/x-icon", "ICO", CLS(7), IMG_ICO, 6, 0x10006, 0 },
};
static const char *L_ICI = "44444444PPPPP4444PP";
static void codec_list(X86State *st, int enc, int fill) {
    int idx[8], n = 0;
    for (int i = 0; i < 8; i++) if (!enc || CODECS[i].enc) idx[n++] = i;
    size_t isz = gs_size(L_ICI), strs = 0;
    for (int k = 0; k < n; k++) { const CodecDef *c = &CODECS[idx[k]]; strs += 2 * (strlen(c->name) + strlen(c->ext) + strlen(c->mime) + strlen(c->desc) + 4); }
    size_t total = isz * (size_t)n + strs;
    if (!fill) {
        if (!ARG(0) || !ARG(1)) STATUS(InvalidParameter);
        mem_wr(ARG(0), 4, (uint32_t)n); mem_wr(ARG(1), 4, (uint32_t)total);
        STATUS(Ok);
    }
    if (!ARG(2) || IA(0) != n || (size_t)IA(1) != total) STATUS(GenericError);
    uint64_t base = ARG(2), sp = base + isz * (size_t)n;
    for (int k = 0; k < n; k++) {
        const CodecDef *c = &CODECS[idx[k]];
        uint64_t o = base + isz * (size_t)k;
        memset(G2H(o), 0, isz);
        memcpy(G2H(o), c->clsid, 16);
        memcpy(G2H(o + 16), FMT_GUIDS[c->fidx], 16);
        const char *strs4[4] = { c->name, c->desc, c->ext, c->mime };
        uint64_t ptrs[4];
        for (int s = 0; s < 4; s++) {
            size_t l = strlen(strs4[s]);
            uint16_t *w = G2H(sp);
            for (size_t i = 0; i <= l; i++) w[i] = (uint8_t)strs4[s][i];
            ptrs[s] = sp; sp += 2 * (l + 1);
        }
        gs_set(o, L_ICI, 8, ptrs[0]);   // CodecName
        gs_set(o, L_ICI, 9, 0);         // DllName
        gs_set(o, L_ICI, 10, ptrs[1]);  // FormatDescription
        gs_set(o, L_ICI, 11, ptrs[2]);  // FilenameExtension
        gs_set(o, L_ICI, 12, ptrs[3]);  // MimeType
        gs_set(o, L_ICI, 13, c->flags); gs_set(o, L_ICI, 14, 1);
    }
    STATUS(Ok);
}
GP(GdipGetImageDecodersSize, 2) { codec_list(st, 0, 0); }
GP(GdipGetImageDecoders, 3) { codec_list(st, 0, 1); }
GP(GdipGetImageEncodersSize, 2) { codec_list(st, 1, 0); }
GP(GdipGetImageEncoders, 3) { codec_list(st, 1, 1); }
GP(GdipGetEncoderParameterListSize, 3) {
    IMAGE(0);
    if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter);
    int ok = 0;
    for (int i = 0; i < 8; i++) if (CODECS[i].enc && memcmp(G2H(ARG(1)), CODECS[i].clsid, 16) == 0) ok = 1;
    if (!ok) STATUS(Win32Error);
    mem_wr(ARG(2), 4, 0);
    STATUS(NotImplemented);
}
GP(GdipGetEncoderParameterList, 4) { STATUS(NotImplemented); }

static uint8_t *encode_image(Image *im, int fmt, size_t *len) {
    int alpha = pf_has_alpha(im->pixfmt);
    if (fmt == IMG_PNG) return img_encode_png(im->px, im->w, im->h, alpha, im->dpix, im->dpiy, len);
    return img_encode_bmp(im->px, im->w, im->h, alpha, im->dpix, im->dpiy, len);
}
static int codec_fmt(uint64_t clsid) {
    if (!clsid) return -1;
    for (int i = 0; i < 8; i++) if (CODECS[i].enc && memcmp(G2H(clsid), CODECS[i].clsid, 16) == 0) return CODECS[i].fmt;
    return -1;
}
GP(GdipSaveImageToFile, 4) {
    IMAGE(0);
    if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter);
    int fmt = codec_fmt(ARG(2));
    if (fmt < 0) STATUS(UnknownImageFormat);
    if (fmt != IMG_PNG && fmt != IMG_BMP) STATUS(NotImplemented);
    size_t n;
    uint8_t *buf = encode_image(im, fmt, &n);
    if (!buf) STATUS(OutOfMemory);
    char *u = w2u_dup((const uint16_t *)G2H(ARG(1)));
    char host[2600];
    path_w2h(u, host, sizeof host);
    free(u);
    int fd = open(host, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { free(buf); STATUS(errno == EACCES ? AccessDenied : Win32Error); }
    size_t put = 0;
    while (put < n) { ssize_t r = write(fd, buf + put, n - put); if (r <= 0) break; put += (size_t)r; }
    close(fd);
    free(buf);
    STATUS(put == n ? Ok : Win32Error);
}
GP(GdipSaveImageToStream, 4) {
    IMAGE(0);
    if (!ARG(1) || !ARG(2)) STATUS(InvalidParameter);
    int fmt = codec_fmt(ARG(2));
    if (fmt < 0) STATUS(UnknownImageFormat);
    if (fmt != IMG_PNG && fmt != IMG_BMP) STATUS(NotImplemented);
    size_t n;
    uint8_t *buf = encode_image(im, fmt, &n);
    if (!buf) STATUS(OutOfMemory);
    int r = stream_write_all(st, ARG(1), buf, n);
    free(buf);
    STATUS(r ? Win32Error : Ok);
}
GP(GdipSaveAdd, 2) { STATUS(NotImplemented); }
GP(GdipSaveAddImage, 3) { STATUS(NotImplemented); }

// ---- image attributes API ----------------------------------------------------------------------------------------------------

#define IATTR(n) ImgAttr *ia = OB(ImgAttr, n, K_ATTR); if (!ia) STATUS(InvalidParameter)
GP(GdipCreateImageAttributes, 1) {
    if (!ARG(0)) STATUS(InvalidParameter);
    ImgAttr *a = NEWOBJ(ImgAttr, K_ATTR);
    if (!a) STATUS(OutOfMemory);
    wr_ptr(ARG(0), H2G(a));
    STATUS(Ok);
}
GP(GdipCloneImageAttributes, 2) { IATTR(0); if (!ARG(1)) STATUS(InvalidParameter); ImgAttr *a = NEWOBJ(ImgAttr, K_ATTR); if (!a) STATUS(OutOfMemory); *a = *ia; wr_ptr(ARG(1), H2G(a)); STATUS(Ok); }
GP(GdipDisposeImageAttributes, 1) { IATTR(0); odel(ia); STATUS(Ok); }
GP(GdipSetImageAttributesToIdentity, 2) { IATTR(0); int t = IA(1); if (t < 0 || t > 4) STATUS(InvalidParameter); memset(&ia->cat[t], 0, sizeof(AttrSet)); STATUS(Ok); }
GP(GdipResetImageAttributes, 2) { IATTR(0); int t = IA(1); if (t < 0 || t > 4) STATUS(InvalidParameter); memset(&ia->cat[t], 0, sizeof(AttrSet)); STATUS(Ok); }
GP(GdipSetImageAttributesColorMatrix, 6) {
    IATTR(0);
    int t = IA(1);
    if (t < 0 || t > 4) STATUS(InvalidParameter);
    AttrSet *as = &ia->cat[t];
    as->cm_on = IA(2) != 0;
    if (as->cm_on) {
        if (!ARG(3)) STATUS(InvalidParameter);
        for (int i = 0; i < 25; i++) as->cm[i] = rdf(ARG(3) + 4 * (uint64_t)i);
        as->cm_flags = IA(5);
    }
    STATUS(Ok);
}
GP(GdipSetImageAttributesThreshold, 4) { IATTR(0); STATUS(Ok); }
GP(GdipSetImageAttributesGamma, 4) {
    IATTR(0);
    int t = IA(1);
    if (t < 0 || t > 4) STATUS(InvalidParameter);
    ia->cat[t].gamma_on = IA(2) != 0; ia->cat[t].gamma = FA(3);
    STATUS(Ok);
}
GP(GdipSetImageAttributesNoOp, 3) { IATTR(0); STATUS(Ok); }
GP(GdipSetImageAttributesColorKeys, 5) {
    IATTR(0);
    int t = IA(1);
    if (t < 0 || t > 4) STATUS(InvalidParameter);
    ia->cat[t].key_on = IA(2) != 0; ia->cat[t].lo = (uint32_t)ARG(3); ia->cat[t].hi = (uint32_t)ARG(4);
    STATUS(Ok);
}
GP(GdipSetImageAttributesOutputChannel, 4) { IATTR(0); STATUS(Ok); }
GP(GdipSetImageAttributesOutputChannelColorProfile, 4) { IATTR(0); STATUS(Ok); }
GP(GdipSetImageAttributesRemapTable, 5) { IATTR(0); STATUS(Ok); }
GP(GdipSetImageAttributesCachedBackground, 2) { IATTR(0); STATUS(Ok); }
GP(GdipSetImageAttributesWrapMode, 4) { IATTR(0); ia->wrap = IA(1); ia->wrap_color = (uint32_t)ARG(2); ia->clamp = IA(3); STATUS(Ok); }
GP(GdipGetImageAttributesAdjustedPalette, 3) { IATTR(0); STATUS(Ok); }

// ---- DrawImage -----------------------------------------------------------------------------------------------------------------------

static float src_unit_scale(Image *im, int unit, int horizontal) {
    float dpi = horizontal ? im->dpix : im->dpiy;
    switch (unit) { case 3: return dpi / 72.0f; case 4: return dpi; case 5: return dpi / 300.0f; case 6: return dpi / 25.4f; default: return 1.0f; }
}
// dst: world coordinates of the images (sx,sy)-origin, (sx+sw, sy) and (sx, sy+sh) corners; src rect in pixels.
static int draw_image_core(X86State *st, Graphics *g, Image *im, const Pt *dst, float sx, float sy, float sw, float sh, ImgAttr *ia, uint64_t cb, uint64_t cbdata) {
    if (sw <= 0 || sh <= 0) return InvalidParameter;
    if (cb) { uint64_t a = cbdata; if (soul_guest_call(st, cb, 1, &a) & 0xFFFFFFFFu) return Aborted; }
    float rx0 = sx, ry0 = sy, rx1 = sx + sw, ry1 = sy + sh;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < 0) ry0 = 0;
    if (rx1 > (float)im->w) rx1 = (float)im->w;
    if (ry1 > (float)im->h) ry1 = (float)im->h;
    if (rx1 <= rx0 || ry1 <= ry0) return Ok;
    Mat s2w = { (dst[1].x - dst[0].x) / sw, (dst[1].y - dst[0].y) / sw, (dst[2].x - dst[0].x) / sh, (dst[2].y - dst[0].y) / sh, dst[0].x, dst[0].y };
    Mat tot = gtotal(g);
    Mat s2d = mmul(mmul(mat_translate(-sx, -sy), s2w), tot); // absolute source pixel -> device
    Paint p; memset(&p, 0, sizeof p);
    p.type = P_IMAGE; p.img = im; p.sx = rx0; p.sy = ry0; p.sw = rx1 - rx0; p.sh = ry1 - ry0; p.clampedge = 1; p.bilinear = 1;
    Mat to_dev = mmul(mat_translate(rx0, ry0), s2d); // coordinates relative to the (clipped) source rect -> device
    if (!minv(&to_dev, &p.inv)) return Ok;
    float foot = fmaxf(hypotf(p.inv.a, p.inv.b), hypotf(p.inv.c, p.inv.d));
    p.foot = foot;
    int ip = g->interp;
    p.interp = (ip == 5) ? 0 : (ip == 2 || ip == 4 || ip == 6 || ip == 7) ? 2 : 1;
    if (p.interp > 0 && fabsf(foot - 1.0f) < 1e-3f && fabsf(p.inv.b) < 1e-4f && fabsf(p.inv.c) < 1e-4f && fabsf(p.inv.e - roundf(p.inv.e)) < 1e-3f && fabsf(p.inv.f - roundf(p.inv.f)) < 1e-3f) p.interp = 0; // pixel exact copy
    if (ia) p.attr = (ia->cat[1].cm_on || ia->cat[1].key_on || ia->cat[1].gamma_on) ? &ia->cat[1] : ((ia->cat[0].cm_on || ia->cat[0].key_on || ia->cat[0].gamma_on) ? &ia->cat[0] : NULL);
    Pt c[4] = { { rx0, ry0 }, { rx1, ry0 }, { rx1, ry1 }, { rx0, ry1 } };
    for (int i = 0; i < 4; i++) c[i] = mpt(&s2d, c[i]);
    PolySet ps; memset(&ps, 0, sizeof ps);
    ps_add_poly(&ps, c, 4, 1);
    ps_orient(&ps);
    int axis = fabsf(s2d.b) < 1e-5f && fabsf(s2d.c) < 1e-5f;
    // snap nearly integral edges of axis aligned draws so that pixel exact copies do not get blended borders
    if (axis) for (int i = 0; i < 4; i++) { c[i].x = fabsf(c[i].x - roundf(c[i].x)) < 1e-3f ? roundf(c[i].x) : c[i].x; c[i].y = fabsf(c[i].y - roundf(c[i].y)) < 1e-3f ? roundf(c[i].y) : c[i].y; }
    if (axis) { ps_free(&ps); ps_add_poly(&ps, c, 4, 1); ps_orient(&ps); }
    fill_dev(g, &ps, 0, !axis && g_aa(g), &p);
    ps_free(&ps);
    return Ok;
}
static float page_units_per_inch(Graphics *g) { return g->unit >= 3 ? unit_per_inch(g, g->unit) : g->dpix; }
static void draw_pointsrect(X86State *st, Image *im, Graphics *g, const Pt *dst, float sx, float sy, float sw, float sh, int unit, ImgAttr *ia, uint64_t cb, uint64_t cbd) {
    float kx = src_unit_scale(im, unit, 1), ky = src_unit_scale(im, unit, 0);
    int r = draw_image_core(st, g, im, dst, sx * kx, sy * ky, sw * kx, sh * ky, ia, cb, cbd);
    STATUS(r);
}
PAIR(GdipDrawImage, 4) {
    GRAPHICS(0); Image *im = OB(Image, 1, K_IMAGE); if (!im) STATUS(InvalidParameter);
    float x = AN(2), y = AN(3);
    float w = (float)im->w * page_units_per_inch(g) / im->dpix, h = (float)im->h * page_units_per_inch(g) / im->dpiy;
    Pt d[3] = { { x, y }, { x + w, y }, { x, y + h } };
    STATUS(draw_image_core(st, g, im, d, 0, 0, (float)im->w, (float)im->h, NULL, 0, 0));
}
PAIR(GdipDrawImageRect, 6) {
    GRAPHICS(0); Image *im = OB(Image, 1, K_IMAGE); if (!im) STATUS(InvalidParameter);
    float x = AN(2), y = AN(3), w = AN(4), h = AN(5);
    Pt d[3] = { { x, y }, { x + w, y }, { x, y + h } };
    STATUS(draw_image_core(st, g, im, d, 0, 0, (float)im->w, (float)im->h, NULL, 0, 0));
}
PAIR(GdipDrawImagePointRect, 9) {
    GRAPHICS(0); Image *im = OB(Image, 1, K_IMAGE); if (!im) STATUS(InvalidParameter);
    float x = AN(2), y = AN(3), sx = AN(4), sy = AN(5), sw = AN(6), sh = AN(7);
    int unit = IA(8);
    float kx = src_unit_scale(im, unit, 1), ky = src_unit_scale(im, unit, 0);
    float w = sw * kx * page_units_per_inch(g) / im->dpix, h = sh * ky * page_units_per_inch(g) / im->dpiy;
    Pt d[3] = { { x, y }, { x + w, y }, { x, y + h } };
    draw_pointsrect(st, im, g, d, sx, sy, sw, sh, unit, NULL, 0, 0);
}
PAIR(GdipDrawImageRectRect, 14) {
    GRAPHICS(0); Image *im = OB(Image, 1, K_IMAGE); if (!im) STATUS(InvalidParameter);
    float x = AN(2), y = AN(3), w = AN(4), h = AN(5);
    ImgAttr *ia = OB(ImgAttr, 11, K_ATTR);
    Pt d[3] = { { x, y }, { x + w, y }, { x, y + h } };
    draw_pointsrect(st, im, g, d, AN(6), AN(7), AN(8), AN(9), IA(10), ia, ARG(12), ARG(13));
}
PAIR(GdipDrawImagePoints, 4) {
    GRAPHICS(0); Image *im = OB(Image, 1, K_IMAGE); if (!im) STATUS(InvalidParameter);
    Pt *q = rd_pts(ARG(2), 3, ii);
    if (IA(3) != 3 || !q) { free(q); STATUS(InvalidParameter); }
    int r = draw_image_core(st, g, im, q, 0, 0, (float)im->w, (float)im->h, NULL, 0, 0);
    free(q);
    STATUS(r);
}
PAIR(GdipDrawImagePointsRect, 12) {
    GRAPHICS(0); Image *im = OB(Image, 1, K_IMAGE); if (!im) STATUS(InvalidParameter);
    Pt *q = rd_pts(ARG(2), 3, ii);
    if (IA(3) != 3 || !q) { free(q); STATUS(InvalidParameter); }
    ImgAttr *ia = OB(ImgAttr, 9, K_ATTR);
    float kx = src_unit_scale(im, IA(8), 1), ky = src_unit_scale(im, IA(8), 0);
    int r = draw_image_core(st, g, im, q, AN(4) * kx, AN(5) * ky, AN(6) * kx, AN(7) * ky, ia, ARG(10), ARG(11));
    free(q);
    STATUS(r);
}

// cached bitmaps
typedef struct Cached { int kind; Image *img; } Cached;
GP(GdipCreateCachedBitmap, 3) {
    Image *im = OB(Image, 0, K_IMAGE);
    if (!im || !OB(Graphics, 1, K_GRAPHICS) || !ARG(2)) STATUS(InvalidParameter);
    Cached *c = NEWOBJ(Cached, K_CACHED);
    if (!c) STATUS(OutOfMemory);
    c->img = image_clone(im);
    if (!c->img) { odel(c); STATUS(OutOfMemory); }
    wr_ptr(ARG(2), H2G(c));
    STATUS(Ok);
}
GP(GdipDeleteCachedBitmap, 1) { Cached *c = OB(Cached, 0, K_CACHED); if (!c) STATUS(InvalidParameter); image_free(c->img); odel(c); STATUS(Ok); }
GP(GdipDrawCachedBitmap, 4) {
    GRAPHICS(0);
    Cached *c = OB(Cached, 1, K_CACHED);
    if (!c) STATUS(InvalidParameter);
    // drawn in device pixels, ignoring the world transform
    Graphics tmp = *g;
    tmp.world = M_ID; tmp.unit = 2; tmp.pscale = 1; tmp.interp = 5;
    float x = (float)IA(2), y = (float)IA(3);
    Pt d[3] = { { x, y }, { x + (float)c->img->w, y }, { x, y + (float)c->img->h } };
    int r = draw_image_core(st, &tmp, c->img, d, 0, 0, (float)c->img->w, (float)c->img->h, NULL, 0, 0);
    STATUS(r);
}

// ======================================================================================================================
// Text
// ======================================================================================================================

typedef struct FamDef { const char *name; int kind; int em, asc, desc, line; } FamDef;
static const FamDef FAMS[] = {
    { "Arial", 0, 2048, 1854, 434, 2355 }, { "Microsoft Sans Serif", 0, 2048, 1854, 434, 2355 }, { "Tahoma", 0, 2048, 2049, 423, 2472 },
    { "Verdana", 0, 2048, 2059, 430, 2489 }, { "Segoe UI", 0, 2048, 2210, 514, 2724 }, { "Calibri", 0, 2048, 1536, 512, 2500 },
    { "Trebuchet MS", 0, 2048, 1923, 455, 2380 }, { "Comic Sans MS", 0, 2048, 2257, 555, 2812 }, { "Lucida Sans Unicode", 0, 2048, 2003, 431, 2434 },
    { "Times New Roman", 1, 2048, 1825, 443, 2355 }, { "Georgia", 1, 2048, 1878, 449, 2327 }, { "Cambria", 1, 2048, 1946, 455, 2401 },
    { "Courier New", 2, 2048, 1705, 615, 2320 }, { "Consolas", 2, 2048, 1521, 527, 2048 }, { "Lucida Console", 2, 2048, 1624, 440, 2064 },
};
#define NFAMS ((int)(sizeof FAMS / sizeof FAMS[0]))

typedef struct Family { int kind; int idx; char name[64]; } Family;
typedef struct FontColl { int kind; int priv; char names[16][64]; int n; } FontColl;
typedef struct Font { int kind; int fam; char name[64]; float em; int style; int unit; } Font;

static int fam_lookup(const char *name) {
    static const struct { const char *alias; int idx; } AL[] = { { "MS Shell Dlg", 4 }, { "MS Shell Dlg 2", 4 }, { "MS Sans Serif", 1 }, { "Helvetica", 0 }, { "Sans Serif", 0 },
        { "Segoe UI Semibold", 4 }, { "Arial Unicode MS", 0 }, { "Times", 9 }, { "Courier", 12 }, { "Fixedsys", 12 }, { "Terminal", 12 }, { "System", 1 } };
    for (int i = 0; i < NFAMS; i++) if (strcasecmp(FAMS[i].name, name) == 0) return i;
    for (size_t i = 0; i < sizeof AL / sizeof AL[0]; i++) if (strcasecmp(AL[i].alias, name) == 0) return AL[i].idx;
    return -1;
}
static Font *font_new(int fam, const char *name, float em, int style, int unit) {
    Font *f = NEWOBJ(Font, K_FONT);
    if (!f) return NULL;
    f->fam = fam; f->em = em; f->style = style; f->unit = unit;
    snprintf(f->name, sizeof f->name, "%s", name);
    return f;
}

GP(GdipCreateFontFamilyFromName, 3) {
    if (!ARG(0) || !ARG(2)) STATUS(InvalidParameter);
    char *n = w2u_dup((const uint16_t *)G2H(ARG(0)));
    FontColl *fc = OB(FontColl, 1, K_FONTCOLL);
    int idx = fam_lookup(n);
    int found = idx >= 0;
    char canon[64];
    if (found) snprintf(canon, sizeof canon, "%s", FAMS[idx].name);
    if (fc && fc->priv) {
        found = 0;
        for (int i = 0; i < fc->n; i++) if (strcasecmp(fc->names[i], n) == 0) { found = 1; snprintf(canon, sizeof canon, "%s", fc->names[i]); if (idx < 0) idx = 0; }
        if (!found && idx >= 0) found = 0;
    } else if (!found) { // private fonts added earlier are visible through the name too
        found = 0;
    }
    free(n);
    if (!found) STATUS(FontFamilyNotFound);
    Family *f = NEWOBJ(Family, K_FAMILY);
    if (!f) STATUS(OutOfMemory);
    f->idx = idx; snprintf(f->name, sizeof f->name, "%s", canon);
    wr_ptr(ARG(2), H2G(f));
    STATUS(Ok);
}
#define FAMILY(n) Family *fa_ = OB(Family, n, K_FAMILY); if (!fa_) STATUS(InvalidParameter)
GP(GdipDeleteFontFamily, 1) { FAMILY(0); odel(fa_); STATUS(Ok); }
GP(GdipCloneFontFamily, 2) { FAMILY(0); if (!ARG(1)) STATUS(InvalidParameter); Family *f = NEWOBJ(Family, K_FAMILY); if (!f) STATUS(OutOfMemory); *f = *fa_; wr_ptr(ARG(1), H2G(f)); STATUS(Ok); }
static void generic_family(X86State *st, int idx) {
    if (!ARG(0)) STATUS(InvalidParameter);
    Family *f = NEWOBJ(Family, K_FAMILY);
    if (!f) STATUS(OutOfMemory);
    f->idx = idx; snprintf(f->name, sizeof f->name, "%s", FAMS[idx].name);
    wr_ptr(ARG(0), H2G(f));
    STATUS(Ok);
}
GP(GdipGetGenericFontFamilySansSerif, 1) { generic_family(st, 1); }
GP(GdipGetGenericFontFamilySerif, 1) { generic_family(st, 9); }
GP(GdipGetGenericFontFamilyMonospace, 1) { generic_family(st, 12); }
GP(GdipGetFamilyName, 3) {
    FAMILY(0);
    if (!ARG(1)) STATUS(InvalidParameter);
    uint16_t *w = u2w_dup(fa_->name);
    size_t n = wstrlen(w);
    if (n > 31) n = 31;
    memcpy(G2H(ARG(1)), w, n * 2);
    ((uint16_t *)G2H(ARG(1)))[n] = 0;
    free(w);
    STATUS(Ok);
}
GP(GdipIsStyleAvailable, 3) { FAMILY(0); if (!ARG(2)) STATUS(InvalidParameter); mem_wr(ARG(2), 4, 1); STATUS(Ok); }
GP(GdipGetEmHeight, 3) { FAMILY(0); if (!ARG(2)) STATUS(InvalidParameter); mem_wr(ARG(2), 2, (uint32_t)FAMS[fa_->idx].em); STATUS(Ok); }
GP(GdipGetCellAscent, 3) { FAMILY(0); if (!ARG(2)) STATUS(InvalidParameter); mem_wr(ARG(2), 2, (uint32_t)FAMS[fa_->idx].asc); STATUS(Ok); }
GP(GdipGetCellDescent, 3) { FAMILY(0); if (!ARG(2)) STATUS(InvalidParameter); mem_wr(ARG(2), 2, (uint32_t)FAMS[fa_->idx].desc); STATUS(Ok); }
GP(GdipGetLineSpacing, 3) { FAMILY(0); if (!ARG(2)) STATUS(InvalidParameter); mem_wr(ARG(2), 2, (uint32_t)FAMS[fa_->idx].line); STATUS(Ok); }

// font collections
GP(GdipNewInstalledFontCollection, 1) { if (!ARG(0)) STATUS(InvalidParameter); FontColl *c = NEWOBJ(FontColl, K_FONTCOLL); if (!c) STATUS(OutOfMemory); wr_ptr(ARG(0), H2G(c)); STATUS(Ok); }
GP(GdipNewPrivateFontCollection, 1) { if (!ARG(0)) STATUS(InvalidParameter); FontColl *c = NEWOBJ(FontColl, K_FONTCOLL); if (!c) STATUS(OutOfMemory); c->priv = 1; wr_ptr(ARG(0), H2G(c)); STATUS(Ok); }
GP(GdipDeletePrivateFontCollection, 1) { FontColl *c = ARG(0) ? OB(FontColl, 0, K_FONTCOLL) : NULL; if (ARG(0) && rd_ptr(ARG(0))) c = objp(rd_ptr(ARG(0)), K_FONTCOLL); if (!c) STATUS(InvalidParameter); odel(c); STATUS(Ok); }
static int ttf_family_name(const uint8_t *d, size_t n, char *out, size_t cap) {
    if (n < 12) return 0;
    uint32_t off = 0;
    if (!memcmp(d, "ttcf", 4)) { if (n < 16) return 0; off = ((uint32_t)d[12] << 24) | (d[13] << 16) | (d[14] << 8) | d[15]; if (off + 12 > n) return 0; }
    int nt = (d[off + 4] << 8) | d[off + 5];
    for (int i = 0; i < nt; i++) {
        size_t r = off + 12 + (size_t)i * 16;
        if (r + 16 > n) return 0;
        if (memcmp(d + r, "name", 4)) continue;
        uint32_t to = ((uint32_t)d[r + 8] << 24) | (d[r + 9] << 16) | (d[r + 10] << 8) | d[r + 11];
        if (to + 6 > n) return 0;
        int cnt = (d[to + 2] << 8) | d[to + 3], so = (d[to + 4] << 8) | d[to + 5];
        for (int k = 0; k < cnt; k++) {
            size_t e = to + 6 + (size_t)k * 12;
            if (e + 12 > n) return 0;
            int pid = (d[e] << 8) | d[e + 1], nid = (d[e + 6] << 8) | d[e + 7], len = (d[e + 8] << 8) | d[e + 9], so2 = (d[e + 10] << 8) | d[e + 11];
            if (nid != 1 || (size_t)to + (size_t)so + (size_t)so2 + (size_t)len > n) continue;
            const uint8_t *s = d + to + so + so2;
            size_t o = 0;
            if (pid == 3 || pid == 0) for (int c = 0; c + 1 < len && o + 1 < cap; c += 2) out[o++] = (char)((s[c] << 8 | s[c + 1]) < 128 ? s[c + 1] : '?');
            else for (int c = 0; c < len && o + 1 < cap; c++) out[o++] = (char)s[c];
            out[o] = 0;
            return o > 0;
        }
    }
    return 0;
}
static void coll_add(FontColl *c, const char *name) {
    for (int i = 0; i < c->n; i++) if (!strcasecmp(c->names[i], name)) return;
    if (c->n < 16) snprintf(c->names[c->n++], 64, "%s", name);
}
GP(GdipPrivateAddFontFile, 2) {
    FontColl *c = OB(FontColl, 0, K_FONTCOLL);
    if (!c || !c->priv || !ARG(1)) STATUS(InvalidParameter);
    char *u = w2u_dup((const uint16_t *)G2H(ARG(1)));
    char host[2600];
    path_w2h(u, host, sizeof host);
    free(u);
    size_t n; int err = 0;
    uint8_t *buf = read_file(host, &n, &err);
    if (!buf) STATUS(FileNotFound);
    char name[64];
    int ok = ttf_family_name(buf, n, name, sizeof name);
    free(buf);
    if (!ok) STATUS(FileNotFound);
    coll_add(c, name);
    STATUS(Ok);
}
GP(GdipPrivateAddMemoryFont, 3) {
    FontColl *c = OB(FontColl, 0, K_FONTCOLL);
    if (!c || !c->priv || !ARG(1) || IA(2) <= 0) STATUS(InvalidParameter);
    char name[64];
    if (!ttf_family_name(G2H(ARG(1)), (size_t)IA(2), name, sizeof name)) STATUS(InvalidParameter);
    coll_add(c, name);
    STATUS(Ok);
}
GP(GdipGetFontCollectionFamilyCount, 2) { FontColl *c = OB(FontColl, 0, K_FONTCOLL); if (!c || !ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)(c->priv ? c->n : NFAMS)); STATUS(Ok); }
GP(GdipGetFontCollectionFamilyList, 4) {
    FontColl *c = OB(FontColl, 0, K_FONTCOLL);
    int want = IA(1);
    if (!c || !ARG(2) || !ARG(3) || want < 0) STATUS(InvalidParameter);
    int have = c->priv ? c->n : NFAMS, n = want < have ? want : have;
    for (int i = 0; i < n; i++) {
        Family *f = NEWOBJ(Family, K_FAMILY);
        if (!f) STATUS(OutOfMemory);
        f->idx = c->priv ? 1 : i; snprintf(f->name, sizeof f->name, "%s", c->priv ? c->names[i] : FAMS[i].name);
        wr_ptr(ARG(2) + (uint64_t)i * PSZ, H2G(f));
    }
    mem_wr(ARG(3), 4, (uint32_t)n);
    STATUS(Ok);
}

// fonts
GP(GdipCreateFont, 5) {
    FAMILY(0);
    float em = FA(1);
    if (!ARG(4) || em <= 0 || IA(3) < 0 || IA(3) > 6) STATUS(InvalidParameter);
    Font *f = font_new(fa_->idx, fa_->name, em, IA(2), IA(3));
    if (!f) STATUS(OutOfMemory);
    wr_ptr(ARG(4), H2G(f));
    STATUS(Ok);
}
GP(GdipCloneFont, 2) { Font *f = OB(Font, 0, K_FONT); if (!f || !ARG(1)) STATUS(InvalidParameter); Font *c = font_new(f->fam, f->name, f->em, f->style, f->unit); if (!c) STATUS(OutOfMemory); wr_ptr(ARG(1), H2G(c)); STATUS(Ok); }
GP(GdipDeleteFont, 1) { Font *f = OB(Font, 0, K_FONT); if (!f) STATUS(InvalidParameter); odel(f); STATUS(Ok); }
GET_FLT(GdipGetFontSize, Font, K_FONT, o->em)
GET_INT(GdipGetFontStyle, Font, K_FONT, o->style)
GET_INT(GdipGetFontUnit, Font, K_FONT, o->unit)
GP(GdipGetFamily, 2) {
    Font *f = OB(Font, 0, K_FONT);
    if (!f || !ARG(1)) STATUS(InvalidParameter);
    Family *fa = NEWOBJ(Family, K_FAMILY);
    if (!fa) STATUS(OutOfMemory);
    fa->idx = f->fam; snprintf(fa->name, sizeof fa->name, "%s", f->name);
    wr_ptr(ARG(1), H2G(fa));
    STATUS(Ok);
}
static float font_px(const Font *f, float dpi) { // em size in pixels (unit World counts as pixels)
    switch (f->unit) { case 3: return f->em * dpi / 72.0f; case 4: return f->em * dpi; case 5: return f->em * dpi / 300.0f; case 6: return f->em * dpi / 25.4f; default: return f->em; }
}
static float page_ppu(Graphics *g) { return (g->unit >= 3 ? g->dpix / unit_per_inch(g, g->unit) : 1.0f) * g->pscale; } // pixels per page unit
static float font_em_world(Graphics *g, const Font *f) { return f->unit == 0 ? f->em : font_px(f, g->dpix) / page_ppu(g); }
GP(GdipGetFontHeight, 3) {
    Font *f = OB(Font, 0, K_FONT);
    Graphics *g = OB(Graphics, 1, K_GRAPHICS);
    if (!f || !ARG(2)) STATUS(InvalidParameter);
    float ratio = (float)FAMS[f->fam].line / (float)FAMS[f->fam].em;
    float h = g ? font_em_world(g, f) * ratio : f->em * ratio;
    wrf(ARG(2), h);
    STATUS(Ok);
}
GP(GdipGetFontHeightGivenDPI, 3) {
    Font *f = OB(Font, 0, K_FONT);
    if (!f || !ARG(2)) STATUS(InvalidParameter);
    float dpi = FA(1);
    wrf(ARG(2), font_px(f, dpi) * (float)FAMS[f->fam].line / (float)FAMS[f->fam].em);
    STATUS(Ok);
}
static int logfont_to_font(X86State *st, uint64_t lf, int wide, Font **out) {
    if (!lf) return InvalidParameter;
    int h = rdi(lf), weight = rdi(lf + 16);
    int italic = (int)mem_rd(lf + 20, 1), under = (int)mem_rd(lf + 21, 1), strike = (int)mem_rd(lf + 22, 1);
    char face[64];
    if (wide) { char *u = w2u_dup((const uint16_t *)G2H(lf + 28)); snprintf(face, sizeof face, "%s", u); free(u); }
    else snprintf(face, sizeof face, "%.31s", (const char *)G2H(lf + 28));
    int idx = fam_lookup(face);
    if (idx < 0) idx = 0;
    float em = h < 0 ? (float)-h : (h == 0 ? 12.0f : (float)h * (float)FAMS[idx].em / (float)(FAMS[idx].asc + FAMS[idx].desc));
    int style = (weight >= 600 ? 1 : 0) | (italic ? 2 : 0) | (under ? 4 : 0) | (strike ? 8 : 0);
    Font *f = font_new(idx, FAMS[idx].name, em, style, 2);
    if (!f) return OutOfMemory;
    *out = f;
    (void)st;
    return Ok;
}
GP(GdipCreateFontFromLogfontA, 3) { Font *f; if (!ARG(2)) STATUS(InvalidParameter); int r = logfont_to_font(st, ARG(1), 0, &f); if (r == Ok) wr_ptr(ARG(2), H2G(f)); STATUS(r); }
GP(GdipCreateFontFromLogfontW, 3) { Font *f; if (!ARG(2)) STATUS(InvalidParameter); int r = logfont_to_font(st, ARG(1), 1, &f); if (r == Ok) wr_ptr(ARG(2), H2G(f)); STATUS(r); }
GP(GdipCreateFontFromDC, 2) {
    Dc *dc = dc_get(ARG(0));
    if (!dc || !ARG(1)) STATUS(InvalidParameter);
    GdiObj *fo = gdi_get(dc->font, GO_FONT);
    int idx = fo ? fam_lookup(fo->face) : -1;
    if (idx < 0) idx = 0;
    float em = fo ? (fo->height < 0 ? (float)-fo->height : (fo->height ? (float)fo->height * 0.86f : 12.0f)) : 12.0f;
    Font *f = font_new(idx, FAMS[idx].name, em, (fo && fo->weight >= 600 ? 1 : 0) | (fo && fo->italic ? 2 : 0) | (fo && fo->underline ? 4 : 0), 2);
    if (!f) STATUS(OutOfMemory);
    wr_ptr(ARG(1), H2G(f));
    STATUS(Ok);
}
static void get_logfont(X86State *st, int wide) {
    Font *f = OB(Font, 0, K_FONT);
    Graphics *g = OB(Graphics, 1, K_GRAPHICS);
    uint64_t lf = ARG(2);
    if (!f || !lf) STATUS(InvalidParameter);
    float px = font_px(f, g ? g->dpiy : 96.0f);
    memset(G2H(lf), 0, wide ? 92 : 60);
    mem_wr(lf, 4, (uint32_t)(int32_t)-lrintf(px));
    mem_wr(lf + 16, 4, (f->style & 1) ? 700 : 400);
    mem_wr(lf + 20, 1, (f->style & 2) != 0); mem_wr(lf + 21, 1, (f->style & 4) != 0); mem_wr(lf + 22, 1, (f->style & 8) != 0);
    mem_wr(lf + 23, 1, 1); // DEFAULT_CHARSET
    if (wide) { uint16_t *w = u2w_dup(f->name); size_t n = wstrlen(w); if (n > 31) n = 31; memcpy(G2H(lf + 28), w, n * 2); free(w); }
    else { size_t n = strlen(f->name); if (n > 31) n = 31; memcpy(G2H(lf + 28), f->name, n); }
}
GP(GdipGetLogFontA, 3) { get_logfont(st, 0); STATUS(Ok); }
GP(GdipGetLogFontW, 3) { get_logfont(st, 1); STATUS(Ok); }

// string formats
typedef struct StrFmt {
    int kind;
    int flags, align, line_align, trimming, hotkey;
    float first_tab; int ntabs; float tabs[32];
    int nranges; int ranges[64][2];
    int digit_lang, digit_method;
} StrFmt;
#define SFMT(n) StrFmt *sf = OB(StrFmt, n, K_SFORMAT); if (!sf) STATUS(InvalidParameter)
GP(GdipCreateStringFormat, 3) {
    if (!ARG(2)) STATUS(InvalidParameter);
    StrFmt *f = NEWOBJ(StrFmt, K_SFORMAT);
    if (!f) STATUS(OutOfMemory);
    f->flags = IA(0); f->trimming = 1;
    wr_ptr(ARG(2), H2G(f));
    STATUS(Ok);
}
GP(GdipStringFormatGetGenericDefault, 1) {
    if (!ARG(0)) STATUS(InvalidParameter);
    StrFmt *f = NEWOBJ(StrFmt, K_SFORMAT);
    if (!f) STATUS(OutOfMemory);
    f->trimming = 1;
    wr_ptr(ARG(0), H2G(f));
    STATUS(Ok);
}
GP(GdipStringFormatGetGenericTypographic, 1) {
    if (!ARG(0)) STATUS(InvalidParameter);
    StrFmt *f = NEWOBJ(StrFmt, K_SFORMAT);
    if (!f) STATUS(OutOfMemory);
    f->flags = 0x4000 | 0x2000 | 0x4; f->trimming = 0;
    wr_ptr(ARG(0), H2G(f));
    STATUS(Ok);
}
GP(GdipDeleteStringFormat, 1) { SFMT(0); odel(sf); STATUS(Ok); }
GP(GdipCloneStringFormat, 2) { SFMT(0); if (!ARG(1)) STATUS(InvalidParameter); StrFmt *c = NEWOBJ(StrFmt, K_SFORMAT); if (!c) STATUS(OutOfMemory); *c = *sf; wr_ptr(ARG(1), H2G(c)); STATUS(Ok); }
SET_INT(GdipSetStringFormatFlags, StrFmt, K_SFORMAT, o->flags = v)
GET_INT(GdipGetStringFormatFlags, StrFmt, K_SFORMAT, o->flags)
SET_INT(GdipSetStringFormatAlign, StrFmt, K_SFORMAT, o->align = v)
GET_INT(GdipGetStringFormatAlign, StrFmt, K_SFORMAT, o->align)
SET_INT(GdipSetStringFormatLineAlign, StrFmt, K_SFORMAT, o->line_align = v)
GET_INT(GdipGetStringFormatLineAlign, StrFmt, K_SFORMAT, o->line_align)
SET_INT(GdipSetStringFormatTrimming, StrFmt, K_SFORMAT, o->trimming = v)
GET_INT(GdipGetStringFormatTrimming, StrFmt, K_SFORMAT, o->trimming)
SET_INT(GdipSetStringFormatHotkeyPrefix, StrFmt, K_SFORMAT, o->hotkey = v)
GET_INT(GdipGetStringFormatHotkeyPrefix, StrFmt, K_SFORMAT, o->hotkey)
GP(GdipSetStringFormatTabStops, 4) {
    SFMT(0);
    int n = IA(2);
    if (n < 0 || n > 32 || (n && !ARG(3))) STATUS(InvalidParameter);
    sf->first_tab = FA(1); sf->ntabs = n;
    for (int i = 0; i < n; i++) sf->tabs[i] = rdf(ARG(3) + 4 * (uint64_t)i);
    STATUS(Ok);
}
GP(GdipGetStringFormatTabStops, 4) {
    SFMT(0);
    int n = IA(1);
    if (!ARG(2) || !ARG(3)) STATUS(InvalidParameter);
    wrf(ARG(2), sf->first_tab);
    for (int i = 0; i < n && i < sf->ntabs; i++) wrf(ARG(3) + 4 * (uint64_t)i, sf->tabs[i]);
    STATUS(Ok);
}
GP(GdipGetStringFormatTabStopCount, 2) { SFMT(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)sf->ntabs); STATUS(Ok); }
GP(GdipSetStringFormatMeasurableCharacterRanges, 3) {
    SFMT(0);
    int n = IA(1);
    if (n < 0 || n > 64 || (n && !ARG(2))) STATUS(InvalidParameter);
    sf->nranges = n;
    for (int i = 0; i < n; i++) { sf->ranges[i][0] = rdi(ARG(2) + 8 * (uint64_t)i); sf->ranges[i][1] = rdi(ARG(2) + 8 * (uint64_t)i + 4); }
    STATUS(Ok);
}
GP(GdipGetStringFormatMeasurableCharacterRangeCount, 2) { SFMT(0); if (!ARG(1)) STATUS(InvalidParameter); mem_wr(ARG(1), 4, (uint32_t)sf->nranges); STATUS(Ok); }
GP(GdipSetStringFormatDigitSubstitution, 3) { SFMT(0); sf->digit_lang = IA(1); sf->digit_method = IA(2); STATUS(Ok); }
GP(GdipGetStringFormatDigitSubstitution, 3) { SFMT(0); if (ARG(1)) mem_wr(ARG(1), 2, (uint32_t)sf->digit_lang); if (ARG(2)) mem_wr(ARG(2), 4, (uint32_t)sf->digit_method); STATUS(Ok); }

// ---- text layout ----------------------------------------------------------------------------------------------------------------

typedef struct LGlyph { uint32_t cp; int idx; float x, w; int line; int under; } LGlyph;
typedef struct LLine { float y, w, x; int first, last; /* glyph indices [first, last) */ } LLine;
typedef struct Layout {
    LGlyph *gl; int ng; LLine *ln; int nl;
    float lh, asc, em_world, em_dev, S;
    int kind;
    int fitted;       // code points laid out (not trimmed)
    float bx, by, bw, bh; // bounding box in world coordinates
} Layout;

static void layout_free(Layout *l) { free(l->gl); free(l->ln); memset(l, 0, sizeof *l); }
static int font_kind(const Font *f) { return (f->style & 1) ? 1 : (FAMS[f->fam].kind == 2 ? 2 : 0); }
static float glyph_adv(const Layout *l, uint32_t cp) {
    GdiGlyph gg;
    if (!gdi_glyph_raster(l->em_dev, l->kind, cp, &gg, NULL, 0)) return 0;
    return gg.adv / l->S;
}
static int utf16_cps(const uint16_t *w, int len, uint32_t **cps, int **idx) {
    uint32_t *c = malloc(sizeof(uint32_t) * (size_t)(len + 1)); int *ix = malloc(sizeof(int) * (size_t)(len + 1));
    if (!c || !ix) { free(c); free(ix); return -1; }
    int n = 0;
    for (int i = 0; i < len; i++) {
        uint32_t u = w[i];
        if (u >= 0xD800 && u < 0xDC00 && i + 1 < len && w[i + 1] >= 0xDC00 && w[i + 1] < 0xE000) { u = 0x10000 + ((u - 0xD800) << 10) + (w[i + 1] - 0xDC00); ix[n] = i; i++; }
        else ix[n] = i;
        c[n++] = u;
    }
    *cps = c; *idx = ix;
    return n;
}
static void wstr_len_arg(uint64_t p, int *len) { if (*len < 0) *len = (int)wstrlen((const uint16_t *)G2H(p)); }

// Lays out text in the given rectangle (world units). rect w/h of 0 = unbounded.
static int layout_text(Graphics *g, const uint16_t *text, int len, const Font *font, float rw, float rh, const StrFmt *fmt, Layout *L) {
    memset(L, 0, sizeof *L);
    uint32_t *cps; int *sidx;
    int n = utf16_cps(text, len, &cps, &sidx);
    if (n < 0) return OutOfMemory;
    Mat tot = gtotal(g);
    L->S = mscale(&tot);
    L->em_world = font_em_world(g, font);
    L->em_dev = L->em_world * L->S;
    L->kind = font_kind(font);
    const FamDef *fd = &FAMS[font->fam];
    L->lh = L->em_world * (float)fd->line / (float)fd->em;
    L->asc = L->em_world * (float)fd->asc / (float)fd->em;
    int flags = fmt ? fmt->flags : 0, trimming = fmt ? fmt->trimming : 1, hotkey = fmt ? fmt->hotkey : 0;
    int align = fmt ? fmt->align : 0, lalign = fmt ? fmt->line_align : 0;
    int wrap = !(flags & 0x1000) && rw > 0;
    // hotkey prefix processing
    uint8_t *under = calloc((size_t)n + 1, 1);
    if (hotkey) {
        int o = 0;
        for (int i = 0; i < n; i++) {
            if (cps[i] == '&' && i + 1 < n) {
                if (cps[i + 1] == '&') { cps[o] = '&'; sidx[o] = sidx[i]; o++; i++; continue; }
                if (hotkey == 1) under[o] = 1;
                continue; // the next character takes the mark
            }
            if (cps[i] == '&' && i + 1 >= n) continue;
            // mark applies to the character after '&': handled by under[o] set before this character is copied
            cps[o] = cps[i]; sidx[o] = sidx[i];
            o++;
        }
        // marks were recorded at the index of the next copied character
        n = o;
    }
    // glyph advances
    LGlyph *gl = calloc((size_t)n + 4, sizeof(LGlyph));
    LLine *ln = calloc((size_t)n + 2, sizeof(LLine));
    if (!gl || !ln) { free(cps); free(sidx); free(under); free(gl); free(ln); return OutOfMemory; }
    float space = glyph_adv(L, ' ');
    float tabw = space * 8;
    int ng = 0, nl = 0;
    float x = 0;
    int line_first = 0, last_break = -1;
    int i = 0;
    float ellip_w = glyph_adv(L, '.') * 3;
    int more_text_hidden = 0;
    #define END_LINE(upto, hard) do { \
        ln[nl].first = line_first; ln[nl].last = (upto); nl++; line_first = (upto); x = 0; last_break = -1; (void)(hard); } while (0)
    while (i < n) {
        uint32_t cp = cps[i];
        if (cp == '\r' && i + 1 < n && cps[i + 1] == '\n') { i++; continue; }
        if (cp == '\n' || cp == '\r') {
            // the newline itself is not a glyph
            ln[nl].first = line_first; ln[nl].last = ng; nl++; line_first = ng; x = 0; last_break = -1;
            gl[ng].cp = '\n'; gl[ng].idx = sidx[i]; gl[ng].x = 0; gl[ng].w = 0; gl[ng].line = -1; ng++; line_first = ng; // placeholder glyph (zero width) keeps indices
            ln[nl - 1].last = ng - 1;
            i++;
            continue;
        }
        float adv;
        if (cp == '\t') {
            adv = tabw - fmodf(x, tabw);
            if (fmt && fmt->ntabs > 0) { // tab stops: the first one at first_tab, each next one relative to the previous
                float stop = fmt->first_tab, found = -1;
                for (int k = 0; k < fmt->ntabs; k++) { stop += fmt->tabs[k]; if (stop > x + 0.001f) { found = stop; break; } }
                if (found > 0) adv = found - x;
            }
        } else adv = glyph_adv(L, cp);
        // wrapping
        if (wrap && x + adv > rw + 0.001f && ng > line_first && cp != ' ' && cp != '\t') {
            int brk = last_break >= 0 ? last_break : ng; // glyph index where the new line starts
            if (last_break < 0 && trimming == 0) brk = ng;
            if (!(flags & 0x1000) && brk > line_first) {
                // move glyphs after the break to the next line
                ln[nl].first = line_first; ln[nl].last = brk; nl++;
                line_first = brk;
                float shift = (brk < ng) ? gl[brk].x : x;
                for (int k = brk; k < ng; k++) gl[k].x -= shift;
                x -= shift;
                last_break = -1;
                // skip spaces hanging at the start of the new line is not needed (they stay on the previous line)
            }
        }
        gl[ng].cp = cp; gl[ng].idx = sidx[i]; gl[ng].x = x; gl[ng].w = adv; gl[ng].under = under[i]; ng++;
        x += adv;
        if (cp == ' ' || cp == '\t' || cp == '-') last_break = ng; // a break is possible after this glyph
        i++;
    }
    ln[nl].first = line_first; ln[nl].last = ng; nl++;
    // compute line widths (ignoring trailing spaces), handle placeholder newline glyphs
    for (int k = 0; k < nl; k++) {
        float w = 0;
        int last = ln[k].last;
        for (int q = ln[k].first; q < last; q++) {
            if (gl[q].cp == '\n') continue;
            gl[q].line = k;
            if (gl[q].cp == ' ') { /* trailing spaces do not count */ }
        }
        int e = last;
        while (e > ln[k].first && (gl[e - 1].cp == ' ' || gl[e - 1].cp == '\n')) e--;
        if (e > ln[k].first) w = gl[e - 1].x + gl[e - 1].w;
        ln[k].w = w;
    }
    // line limit / vertical overflow
    int vis = nl;
    if (rh > 0) {
        int maxl = (int)floorf(rh / L->lh + 0.001f);
        if (maxl < 1) maxl = (flags & 0x2000) ? 0 : 1;
        if (!(flags & 0x2000) && (float)maxl * L->lh < rh - 0.001f) maxl++;
        if (nl > maxl) { vis = maxl; more_text_hidden = 1; }
    }
    // horizontal trimming of overflowing lines
    int do_trim = rw > 0 && (flags & 0x1000 ? 1 : 0);
    if (rw > 0 && wrap) do_trim = 0;
    int ell = trimming >= 3 && rw > 0;
    for (int k = 0; k < vis; k++) {
        int hide_tail = (k == vis - 1 && more_text_hidden && ell);
        if (!(do_trim || hide_tail) && !(ln[k].w > rw && rw > 0 && !wrap && trimming > 0)) continue;
        if (trimming == 0 && !hide_tail) continue;
        if (ln[k].w <= rw && !hide_tail) continue;
        float limit = rw - (ell ? ellip_w : 0);
        int cut = ln[k].first;
        int wordcut = -1;
        for (int q = ln[k].first; q < ln[k].last; q++) {
            if (gl[q].cp == '\n') break;
            if (gl[q].x + gl[q].w > limit + 0.001f) break;
            cut = q + 1;
            if (gl[q].cp == ' ') wordcut = q;
        }
        if ((trimming == 2 || trimming == 4) && wordcut > ln[k].first && cut < ln[k].last) cut = wordcut;
        // keep the glyphs up to cut and append an ellipsis
        int nlast = cut;
        float endx = nlast > ln[k].first ? gl[nlast - 1].x + gl[nlast - 1].w : 0;
        for (int q = nlast; q < ln[k].last; q++) gl[q].line = -2; // trimmed
        if (ell) {
            // the ellipsis uses the three glyph slots of the first removed glyphs (or appended ones)
            int base = ln[k].last;
            LGlyph *ng2 = realloc(gl, sizeof(LGlyph) * (size_t)(ng + 3 + 4));
            if (ng2) {
                gl = ng2;
                for (int d = 0; d < 3; d++) { gl[ng + d].cp = '.'; gl[ng + d].idx = -1; gl[ng + d].x = endx + (float)d * (ellip_w / 3); gl[ng + d].w = ellip_w / 3; gl[ng + d].line = k; gl[ng + d].under = 0; }
                ng += 3; (void)base;
            }
            ln[k].w = endx + ellip_w;
        } else ln[k].w = endx;
    }
    // vertical placement
    float total_h = (float)vis * L->lh;
    float vy = 0;
    if (lalign == 1) vy = (rh - total_h) / 2; else if (lalign == 2) vy = rh - total_h;
    float maxw = 0;
    for (int k = 0; k < vis; k++) {
        float lx = 0;
        if (align == 1) lx = (rw - ln[k].w) / 2; else if (align == 2) lx = rw - ln[k].w;
        ln[k].x = lx; ln[k].y = vy + (float)k * L->lh;
        if (ln[k].w > maxw) maxw = ln[k].w;
    }
    // drop invisible lines' glyphs
    for (int q = 0; q < ng; q++) if (gl[q].line >= vis) gl[q].line = -2;
    int fitted = 0;
    for (int q = 0; q < ng; q++) if (gl[q].line >= 0 || gl[q].cp == '\n' || gl[q].cp == ' ') fitted++;
    L->gl = gl; L->ng = ng; L->ln = ln; L->nl = vis; L->fitted = fitted;
    L->bw = maxw; L->bh = total_h;
    L->bx = 0; L->by = vy;
    if (vis > 0) { float mn = 1e30f; for (int k = 0; k < vis; k++) if (ln[k].x < mn) mn = ln[k].x; L->bx = mn; }
    free(cps); free(sidx); free(under);
    return Ok;
}

// ---- glyph rendering ---------------------------------------------------------------------------------------------------------

typedef struct RunGlyph { uint32_t cp; Pt origin; int under; } RunGlyph; // origin = baseline-left in world coordinates

static void union_px(uint8_t *d, int v) { int a = *d; *d = (uint8_t)(a + v - a * v / 255); }

static int render_glyphs(Graphics *g, const RunGlyph *rg, int n, float em_world, int kind, int style, const Mat *extra, const float *clip /* x0,y0,x1,y1 world or NULL */, const Brush *brush) {
    Mat tot = gtotal(g);
    if (extra) tot = mmul(*extra, tot);
    float S = mscale(&tot);
    float em_dev = em_world * S;
    // rotation part
    Mat R = { tot.a / S, tot.b / S, tot.c / S, tot.d / S, 0, 0 };
    int simple = fabsf(R.b) < 1e-4f && fabsf(R.c) < 1e-4f && R.a > 0 && R.d > 0;
    // bounding box in device space
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < n; i++) {
        Pt o = mpt(&tot, rg[i].origin);
        if (o.x - em_dev * 2 < minx) minx = o.x - em_dev * 2;
        if (o.x + em_dev * 3 > maxx) maxx = o.x + em_dev * 3;
        if (o.y - em_dev * 2 < miny) miny = o.y - em_dev * 2;
        if (o.y + em_dev * 2 > maxy) maxy = o.y + em_dev * 2;
    }
    if (n == 0) return Ok;
    int bx0 = (int)floorf(minx), by0 = (int)floorf(miny), bx1 = (int)ceilf(maxx), by1 = (int)ceilf(maxy);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > g->w) bx1 = g->w;
    if (by1 > g->h) by1 = g->h;
    if (clip) { // clip rectangle (world) -> device bounding box
        Pt c[4] = { { clip[0], clip[1] }, { clip[2], clip[1] }, { clip[2], clip[3] }, { clip[0], clip[3] } };
        float a = 1e30f, b = 1e30f, cc = -1e30f, d = -1e30f;
        for (int i = 0; i < 4; i++) { Pt p = mpt(&tot, c[i]); if (p.x < a) a = p.x; if (p.x > cc) cc = p.x; if (p.y < b) b = p.y; if (p.y > d) d = p.y; }
        if ((int)floorf(a) > bx0) bx0 = (int)floorf(a);
        if ((int)floorf(b) > by0) by0 = (int)floorf(b);
        if ((int)ceilf(cc) < bx1) bx1 = (int)ceilf(cc);
        if ((int)ceilf(d) < by1) by1 = (int)ceilf(d);
    }
    if (bx1 <= bx0 || by1 <= by0) return Ok;
    int mw = bx1 - bx0, mh = by1 - by0;
    if ((int64_t)mw * mh > (1 << 26)) return Ok;
    uint8_t *mask = calloc((size_t)mw * (size_t)mh, 1);
    uint8_t *cov = malloc(256 * 256);
    if (!mask || !cov) { free(mask); free(cov); return OutOfMemory; }
    int threshold = (g->texthint == 1 || g->texthint == 2);
    for (int i = 0; i < n; i++) {
        GdiGlyph gg;
        uint32_t cp = rg[i].cp;
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') { if (!rg[i].under) continue; }
        Pt o = mpt(&tot, rg[i].origin);
        if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r' && gdi_glyph_raster(em_dev, kind, cp, &gg, cov, 256 * 256) && gg.w > 0 && gg.h > 0 && (size_t)gg.w * (size_t)gg.h <= 256 * 256) {
            float basey_off = -gg.ascent; // glyph pixel (px,py) lies at (x0 + px, y0 + py + basey_off) relative to the baseline origin
            if (simple) {
                int left = (int)lroundf(o.x + (float)gg.x0), top = (int)lroundf(o.y + basey_off + (float)gg.y0);
                for (int y = 0; y < gg.h; y++) {
                    int dy = top + y - by0;
                    if (dy < 0 || dy >= mh) continue;
                    for (int x = 0; x < gg.w; x++) {
                        int dx = left + x - bx0;
                        if (dx < 0 || dx >= mw) continue;
                        int v = cov[y * gg.w + x];
                        if (threshold) v = v >= 128 ? 255 : 0;
                        if (v) union_px(&mask[(size_t)dy * (size_t)mw + (size_t)dx], v);
                    }
                }
            } else { // rotated / sheared: inverse sampling
                Mat Rm = R, Ri;
                if (!minv(&Rm, &Ri)) continue;
                float cx[4] = { (float)gg.x0, (float)(gg.x0 + gg.w), (float)(gg.x0 + gg.w), (float)gg.x0 };
                float cy[4] = { basey_off + (float)gg.y0, basey_off + (float)gg.y0, basey_off + (float)(gg.y0 + gg.h), basey_off + (float)(gg.y0 + gg.h) };
                float a = 1e30f, b = 1e30f, c2 = -1e30f, d2 = -1e30f;
                for (int k = 0; k < 4; k++) { float px = o.x + cx[k] * R.a + cy[k] * R.c, py = o.y + cx[k] * R.b + cy[k] * R.d; if (px < a) a = px; if (px > c2) c2 = px; if (py < b) b = py; if (py > d2) d2 = py; }
                for (int py = (int)floorf(b); py <= (int)ceilf(d2); py++) {
                    int dy = py - by0;
                    if (dy < 0 || dy >= mh) continue;
                    for (int px = (int)floorf(a); px <= (int)ceilf(c2); px++) {
                        int dx = px - bx0;
                        if (dx < 0 || dx >= mw) continue;
                        float qx = (float)px + 0.5f - o.x, qy = (float)py + 0.5f - o.y;
                        float u = qx * Ri.a + qy * Ri.c - (float)gg.x0, v = qx * Ri.b + qy * Ri.d - basey_off - (float)gg.y0;
                        int iu = (int)floorf(u), iv = (int)floorf(v);
                        if (iu < 0 || iv < 0 || iu >= gg.w || iv >= gg.h) continue;
                        int val = cov[iv * gg.w + iu];
                        if (threshold) val = val >= 128 ? 255 : 0;
                        if (val) union_px(&mask[(size_t)dy * (size_t)mw + (size_t)dx], val);
                    }
                }
            }
        }
        if (((style & 4) || rg[i].under) && gdi_glyph_raster(em_dev, kind, cp == '\t' ? ' ' : cp, &gg, NULL, 0)) { // underline
            float th = em_dev / 14.0f; if (th < 1) th = 1;
            float ux = o.x, uy = o.y + em_dev * 0.12f;
            int w = (int)lroundf(gg.adv), t = (int)lroundf(th);
            for (int y = 0; y < t; y++) for (int x = 0; x < w; x++) {
                int dx = (int)lroundf(ux) + x - bx0, dy = (int)lroundf(uy) + y - by0;
                if (dx >= 0 && dx < mw && dy >= 0 && dy < mh) mask[(size_t)dy * (size_t)mw + (size_t)dx] = 255;
            }
        }
        if (style & 8) { // strikeout
            if (gdi_glyph_raster(em_dev, kind, cp == '\t' ? ' ' : cp, &gg, NULL, 0)) {
                int w = (int)lroundf(gg.adv), t = (int)fmaxf(1.0f, lroundf(em_dev / 14.0f));
                for (int y = 0; y < t; y++) for (int x = 0; x < w; x++) {
                    int dx = (int)lroundf(o.x) + x - bx0, dy = (int)lroundf(o.y - em_dev * 0.3f) + y - by0;
                    if (dx >= 0 && dx < mw && dy >= 0 && dy < mh) mask[(size_t)dy * (size_t)mw + (size_t)dx] = 255;
                }
            }
        }
    }
    Paint p;
    if (paint_from_brush(g, brush, &p)) { Mask m = { bx0, by0, mw, mh, mask }; composite(g, &m, &p, 0); }
    free(mask); free(cov);
    return Ok;
}

static StrFmt DEFAULT_FMT_STORAGE;
static const StrFmt *default_fmt(void) { memset(&DEFAULT_FMT_STORAGE, 0, sizeof DEFAULT_FMT_STORAGE); DEFAULT_FMT_STORAGE.trimming = 1; return &DEFAULT_FMT_STORAGE; }

static void rect_args(X86State *st, int n, float *r) { for (int i = 0; i < 4; i++) r[i] = rdf(ARG(n) + 4 * (uint64_t)i); }

GP(GdipDrawString, 7) {
    GRAPHICS(0);
    Font *font = OB(Font, 3, K_FONT);
    Brush *br = OB(Brush, 6, K_BRUSH);
    StrFmt *sf = ARG(5) ? OB(StrFmt, 5, K_SFORMAT) : NULL;
    int len = IA(2);
    if (!ARG(1) || !font || !br || !ARG(4) || (ARG(5) && !sf)) STATUS(InvalidParameter);
    wstr_len_arg(ARG(1), &len);
    if (len == 0) STATUS(Ok);
    float r[4]; rect_args(st, 4, r);
    Layout L;
    int rc = layout_text(g, (const uint16_t *)G2H(ARG(1)), len, font, r[2], r[3], sf ? sf : default_fmt(), &L);
    if (rc != Ok) STATUS(rc);
    RunGlyph *rg = malloc(sizeof(RunGlyph) * (size_t)(L.ng + 1));
    int n = 0;
    if (rg) {
        for (int k = 0; k < L.nl; k++)
            for (int q = L.ln[k].first; q < L.ln[k].last; q++) {
                if (L.gl[q].line != k || L.gl[q].cp == '\n') continue;
                rg[n].cp = L.gl[q].cp; rg[n].under = L.gl[q].under;
                rg[n].origin = (Pt){ r[0] + L.ln[k].x + L.gl[q].x, r[1] + L.ln[k].y + L.asc };
                n++;
            }
        for (int q = 0; q < L.ng; q++) { // ellipsis glyphs appended at the end
            if (L.gl[q].idx == -1 && L.gl[q].line >= 0) {
                int k = L.gl[q].line;
                rg[n].cp = '.'; rg[n].under = 0; rg[n].origin = (Pt){ r[0] + L.ln[k].x + L.gl[q].x, r[1] + L.ln[k].y + L.asc }; n++;
            }
        }
        float clip[4] = { r[0], r[1], r[0] + r[2], r[1] + r[3] };
        int noclip = sf && (sf->flags & 0x4000);
        render_glyphs(g, rg, n, L.em_world, L.kind, font->style, NULL, (r[2] > 0 && r[3] > 0 && !noclip) ? clip : NULL, br);
        free(rg);
    }
    layout_free(&L);
    STATUS(Ok);
}
GP(GdipMeasureString, 9) {
    GRAPHICS(0);
    Font *font = OB(Font, 3, K_FONT);
    StrFmt *sf = ARG(5) ? OB(StrFmt, 5, K_SFORMAT) : NULL;
    int len = IA(2);
    if (!ARG(1) || !font || !ARG(4) || !ARG(6) || (ARG(5) && !sf)) STATUS(InvalidParameter);
    wstr_len_arg(ARG(1), &len);
    float r[4]; rect_args(st, 4, r);
    if (len == 0) { write_rectf(ARG(6), r[0], r[1], 0, 0); if (ARG(7)) mem_wr(ARG(7), 4, 0); if (ARG(8)) mem_wr(ARG(8), 4, 0); STATUS(Ok); }
    Layout L;
    int rc = layout_text(g, (const uint16_t *)G2H(ARG(1)), len, font, r[2], r[3], sf ? sf : default_fmt(), &L);
    if (rc != Ok) STATUS(rc);
    write_rectf(ARG(6), r[0] + L.bx, r[1] + L.by, L.bw, L.bh);
    if (ARG(7)) mem_wr(ARG(7), 4, (uint32_t)(L.fitted > len ? len : L.fitted));
    if (ARG(8)) mem_wr(ARG(8), 4, (uint32_t)L.nl);
    layout_free(&L);
    STATUS(Ok);
}
GP(GdipMeasureCharacterRanges, 8) {
    GRAPHICS(0);
    Font *font = OB(Font, 3, K_FONT);
    StrFmt *sf = ARG(5) ? OB(StrFmt, 5, K_SFORMAT) : NULL;
    int len = IA(2), cnt = IA(6);
    if (!ARG(1) || !font || !ARG(4) || !ARG(7) || !sf || cnt != sf->nranges) STATUS(InvalidParameter);
    wstr_len_arg(ARG(1), &len);
    float r[4]; rect_args(st, 4, r);
    Layout L;
    int rc = layout_text(g, (const uint16_t *)G2H(ARG(1)), len, font, r[2], r[3], sf, &L);
    if (rc != Ok) STATUS(rc);
    for (int k = 0; k < cnt; k++) {
        int first = sf->ranges[k][0], length = sf->ranges[k][1];
        Region *reg = NULL;
        for (int ln = 0; ln < L.nl; ln++) {
            float x0 = 1e30f, x1 = -1e30f;
            for (int q = L.ln[ln].first; q < L.ln[ln].last; q++) {
                if (L.gl[q].line != ln || L.gl[q].cp == '\n') continue;
                if (L.gl[q].idx >= first && L.gl[q].idx < first + length) { if (L.gl[q].x < x0) x0 = L.gl[q].x; if (L.gl[q].x + L.gl[q].w > x1) x1 = L.gl[q].x + L.gl[q].w; }
            }
            if (x1 > x0) {
                Region *rr = region_from_rect(r[0] + L.ln[ln].x + x0, r[1] + L.ln[ln].y, x1 - x0, L.lh, NULL);
                reg = reg ? region_combine(reg, rr, 2) : rr;
            }
        }
        if (!reg) reg = region_new(RG_EMPTY);
        Region *dst = (Region *)objp(rd_ptr(ARG(7) + (uint64_t)k * PSZ), K_REGION);
        if (dst) {
            region_reset(dst, RG_EMPTY);
            dst->type = reg->type; dst->path = reg->path; dst->l = reg->l; dst->r = reg->r; dst->mode = reg->mode;
            odel(reg);
        } else region_free(reg);
    }
    layout_free(&L);
    STATUS(Ok);
}
GP(GdipDrawDriverString, 8) {
    GRAPHICS(0);
    Font *font = OB(Font, 3, K_FONT);
    Brush *br = OB(Brush, 4, K_BRUSH);
    int len = IA(2), flags = IA(6);
    MatObj *mo = ARG(7) ? OB(MatObj, 7, K_MATRIX) : NULL;
    if (!ARG(1) || !font || !br || !ARG(5)) STATUS(InvalidParameter);
    wstr_len_arg(ARG(1), &len);
    if (len <= 0) STATUS(Ok);
    RunGlyph *rg = malloc(sizeof(RunGlyph) * (size_t)len);
    if (!rg) STATUS(OutOfMemory);
    Layout L; memset(&L, 0, sizeof L);
    Mat tot = gtotal(g);
    L.S = mscale(&tot); L.em_world = font_em_world(g, font); L.em_dev = L.em_world * L.S; L.kind = font_kind(font);
    float cx = rdf(ARG(5)), cy = rdf(ARG(5) + 4);
    for (int i = 0; i < len; i++) {
        uint32_t cp = (uint32_t)mem_rd(ARG(1) + 2 * (uint64_t)i, 2);
        rg[i].cp = cp; rg[i].under = 0;
        if (flags & 4) { rg[i].origin = (Pt){ cx, cy }; cx += glyph_adv(&L, cp); }
        else rg[i].origin = (Pt){ rdf(ARG(5) + 8 * (uint64_t)i), rdf(ARG(5) + 8 * (uint64_t)i + 4) };
    }
    render_glyphs(g, rg, len, L.em_world, L.kind, font->style, mo ? &mo->m : NULL, NULL, br);
    free(rg);
    STATUS(Ok);
}
GP(GdipMeasureDriverString, 8) {
    GRAPHICS(0);
    Font *font = OB(Font, 3, K_FONT);
    int len = IA(2), flags = IA(5);
    if (!ARG(1) || !font || !ARG(4) || !ARG(7)) STATUS(InvalidParameter);
    wstr_len_arg(ARG(1), &len);
    Layout L; memset(&L, 0, sizeof L);
    Mat tot = gtotal(g);
    L.S = mscale(&tot); L.em_world = font_em_world(g, font); L.em_dev = L.em_world * L.S; L.kind = font_kind(font);
    float lh = L.em_world * (float)FAMS[font->fam].line / (float)FAMS[font->fam].em;
    float a = 1e30f, b = 1e30f, c = -1e30f, d = -1e30f, cx = rdf(ARG(4)), cy = rdf(ARG(4) + 4);
    for (int i = 0; i < len; i++) {
        uint32_t cp = (uint32_t)mem_rd(ARG(1) + 2 * (uint64_t)i, 2);
        float x = (flags & 4) ? cx : rdf(ARG(4) + 8 * (uint64_t)i), y = (flags & 4) ? cy : rdf(ARG(4) + 8 * (uint64_t)i + 4);
        float w = glyph_adv(&L, cp);
        if (x < a) a = x;
        if (x + w > c) c = x + w;
        float top = y - L.em_world * (float)FAMS[font->fam].asc / (float)FAMS[font->fam].em;
        if (top < b) b = top;
        if (top + lh > d) d = top + lh;
        cx += w;
    }
    if (len <= 0) a = b = c = d = 0;
    write_rectf(ARG(7), a, b, c - a, d - b);
    STATUS(Ok);
}

// ---- remaining entry points: custom line caps, metafiles and rarely used getters are not implemented ------------------------

GP(GdipCreateFromHWNDICM, 2) { gp_GdipCreateFromHWND(st); }
GP(GdipResetPageTransform, 1) { GRAPHICS(0); g->unit = 2; g->pscale = 1; STATUS(Ok); }
GP(GdipCloneCustomLineCap, 2) { STATUS(NotImplemented); }
GP(GdipCreateAdjustableArrowCap, 4) { STATUS(NotImplemented); }
GP(GdipCreateCustomLineCap, 5) { STATUS(NotImplemented); }
GP(GdipCreateHalftonePalette, 0) { STATUS(NotImplemented); }
GP(GdipCreateMetafileFromEmf, 3) { STATUS(NotImplemented); }
GP(GdipCreateMetafileFromFile, 2) { STATUS(NotImplemented); }
GP(GdipCreateMetafileFromStream, 2) { STATUS(NotImplemented); }
GP(GdipCreateMetafileFromWmf, 4) { STATUS(NotImplemented); }
GP(GdipCreateMetafileFromWmfFile, 3) { STATUS(NotImplemented); }
GP(GdipCreateStreamOnFile, 3) { STATUS(NotImplemented); }
GP(GdipDeleteCustomLineCap, 1) { STATUS(NotImplemented); }
GP(GdipEmfToWmfBits, 5) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileDestPoint, 6) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileDestPointI, 6) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileDestPoints, 7) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileDestPointsI, 7) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileDestRect, 6) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileDestRectI, 6) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileSrcRectDestPoint, 8) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileSrcRectDestPointI, 8) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileSrcRectDestPoints, 9) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileSrcRectDestPointsI, 9) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileSrcRectDestRect, 8) { STATUS(NotImplemented); }
GP(GdipEnumerateMetafileSrcRectDestRectI, 8) { STATUS(NotImplemented); }
GP(GdipGetAdjustableArrowCapFillState, 2) { STATUS(NotImplemented); }
GP(GdipGetAdjustableArrowCapHeight, 2) { STATUS(NotImplemented); }
GP(GdipGetAdjustableArrowCapMiddleInset, 2) { STATUS(NotImplemented); }
GP(GdipGetAdjustableArrowCapWidth, 2) { STATUS(NotImplemented); }
GP(GdipGetCustomLineCapBaseCap, 2) { STATUS(NotImplemented); }
GP(GdipGetCustomLineCapBaseInset, 2) { STATUS(NotImplemented); }
GP(GdipGetCustomLineCapStrokeCaps, 3) { STATUS(NotImplemented); }
GP(GdipGetCustomLineCapStrokeJoin, 2) { STATUS(NotImplemented); }
GP(GdipGetCustomLineCapType, 2) { STATUS(NotImplemented); }
GP(GdipGetCustomLineCapWidthScale, 2) { STATUS(NotImplemented); }
GP(GdipGetHemfFromMetafile, 2) { STATUS(NotImplemented); }
GP(GdipGetMetafileDownLevelRasterizationLimit, 2) { STATUS(NotImplemented); }
GP(GdipGetMetafileHeaderFromEmf, 2) { STATUS(NotImplemented); }
GP(GdipGetMetafileHeaderFromFile, 2) { STATUS(NotImplemented); }
GP(GdipGetMetafileHeaderFromMetafile, 2) { STATUS(NotImplemented); }
GP(GdipGetMetafileHeaderFromStream, 2) { STATUS(NotImplemented); }
GP(GdipGetMetafileHeaderFromWmf, 3) { STATUS(NotImplemented); }
GP(GdipGetPathGradientBlend, 4) { STATUS(NotImplemented); }
GP(GdipGetPathGradientGammaCorrection, 2) { STATUS(NotImplemented); }
GP(GdipGetPathGradientPath, 2) { STATUS(NotImplemented); }
GP(GdipGetPathGradientPresetBlend, 4) { STATUS(NotImplemented); }
GP(GdipGetPathGradientPresetBlendCount, 2) { STATUS(NotImplemented); }
GP(GdipGetPenCompoundArray, 3) { STATUS(NotImplemented); }
GP(GdipGetPenCustomEndCap, 2) { STATUS(NotImplemented); }
GP(GdipGetPenCustomStartCap, 2) { STATUS(NotImplemented); }
GP(GdipPathIterIsValid, 2) { STATUS(NotImplemented); }
GP(GdipPathIterNextMarker, 4) { STATUS(NotImplemented); }
GP(GdipPathIterNextMarkerPath, 3) { STATUS(NotImplemented); }
GP(GdipPathIterNextPathType, 5) { STATUS(NotImplemented); }
GP(GdipPathIterNextSubpathPath, 4) { STATUS(NotImplemented); }
GP(GdipPlayMetafileRecord, 5) { STATUS(NotImplemented); }
GP(GdipRecordMetafile, 6) { STATUS(NotImplemented); }
GP(GdipRecordMetafileFileName, 7) { STATUS(NotImplemented); }
GP(GdipRecordMetafileFileNameI, 7) { STATUS(NotImplemented); }
GP(GdipRecordMetafileI, 6) { STATUS(NotImplemented); }
GP(GdipRecordMetafileStream, 7) { STATUS(NotImplemented); }
GP(GdipRecordMetafileStreamI, 7) { STATUS(NotImplemented); }
GP(GdipSetAdjustableArrowCapFillState, 2) { STATUS(NotImplemented); }
GP(GdipSetAdjustableArrowCapHeight, 2) { STATUS(NotImplemented); }
GP(GdipSetAdjustableArrowCapMiddleInset, 2) { STATUS(NotImplemented); }
GP(GdipSetAdjustableArrowCapWidth, 2) { STATUS(NotImplemented); }
GP(GdipSetCustomLineCapBaseCap, 2) { STATUS(NotImplemented); }
GP(GdipSetCustomLineCapBaseInset, 2) { STATUS(NotImplemented); }
GP(GdipSetCustomLineCapStrokeCaps, 3) { STATUS(NotImplemented); }
GP(GdipSetCustomLineCapStrokeJoin, 2) { STATUS(NotImplemented); }
GP(GdipSetCustomLineCapWidthScale, 2) { STATUS(NotImplemented); }
GP(GdipSetMetafileDownLevelRasterizationLimit, 2) { STATUS(NotImplemented); }
GP(GdipSetPathGradientGammaCorrection, 2) { STATUS(NotImplemented); }
GP(GdipSetPathGradientPath, 2) { STATUS(NotImplemented); }

// ---- export table ----------------------------------------------------------------------------------------------------

const HleEntry hle_gdiplus_table[] = {
    { "GdiplusStartup", gp_GdiplusStartup, NULL }, { "GdiplusShutdown", gp_GdiplusShutdown, NULL }, { "GdipAlloc", gp_GdipAlloc, NULL },
    { "GdipFree", gp_GdipFree, NULL }, { "GdipCreateFromHDC", gp_GdipCreateFromHDC, NULL }, { "GdipCreateFromHDC2", gp_GdipCreateFromHDC2, NULL },
    { "GdipCreateFromHWND", gp_GdipCreateFromHWND, NULL }, { "GdipGetImageGraphicsContext", gp_GdipGetImageGraphicsContext, NULL }, { "GdipDeleteGraphics", gp_GdipDeleteGraphics, NULL },
    { "GdipGetDC", gp_GdipGetDC, NULL }, { "GdipReleaseDC", gp_GdipReleaseDC, NULL }, { "GdipGraphicsClear", gp_GdipGraphicsClear, NULL },
    { "GdipFlush", gp_GdipFlush, NULL }, { "GdipSetRenderingOrigin", gp_GdipSetRenderingOrigin, NULL }, { "GdipGetRenderingOrigin", gp_GdipGetRenderingOrigin, NULL },
    { "GdipComment", gp_GdipComment, NULL }, { "GdipGetNearestColor", gp_GdipGetNearestColor, NULL }, { "GdipTestControl", gp_GdipTestControl, NULL },
    { "GdipSetPageUnit", gp_GdipSetPageUnit, NULL }, { "GdipSetPageScale", gp_GdipSetPageScale, NULL }, { "GdipSetWorldTransform", gp_GdipSetWorldTransform, NULL },
    { "GdipGetWorldTransform", gp_GdipGetWorldTransform, NULL }, { "GdipResetWorldTransform", gp_GdipResetWorldTransform, NULL }, { "GdipMultiplyWorldTransform", gp_GdipMultiplyWorldTransform, NULL },
    { "GdipTranslateWorldTransform", gp_GdipTranslateWorldTransform, NULL }, { "GdipScaleWorldTransform", gp_GdipScaleWorldTransform, NULL }, { "GdipRotateWorldTransform", gp_GdipRotateWorldTransform, NULL },
    { "GdipTransformPoints", gp_GdipTransformPoints, NULL }, { "GdipTransformPointsI", gp_GdipTransformPointsI, NULL }, { "GdipSetClipRect", gp_GdipSetClipRect, NULL },
    { "GdipSetClipRectI", gp_GdipSetClipRectI, NULL }, { "GdipSetClipPath", gp_GdipSetClipPath, NULL }, { "GdipSetClipRegion", gp_GdipSetClipRegion, NULL },
    { "GdipSetClipGraphics", gp_GdipSetClipGraphics, NULL }, { "GdipSetClipHrgn", gp_GdipSetClipHrgn, NULL }, { "GdipResetClip", gp_GdipResetClip, NULL },
    { "GdipTranslateClip", gp_GdipTranslateClip, NULL }, { "GdipTranslateClipI", gp_GdipTranslateClipI, NULL }, { "GdipGetClip", gp_GdipGetClip, NULL },
    { "GdipGetClipBounds", gp_GdipGetClipBounds, NULL }, { "GdipGetClipBoundsI", gp_GdipGetClipBoundsI, NULL }, { "GdipGetVisibleClipBounds", gp_GdipGetVisibleClipBounds, NULL },
    { "GdipGetVisibleClipBoundsI", gp_GdipGetVisibleClipBoundsI, NULL }, { "GdipIsClipEmpty", gp_GdipIsClipEmpty, NULL }, { "GdipIsVisibleClipEmpty", gp_GdipIsVisibleClipEmpty, NULL },
    { "GdipIsVisiblePoint", gp_GdipIsVisiblePoint, NULL }, { "GdipIsVisiblePointI", gp_GdipIsVisiblePointI, NULL }, { "GdipIsVisibleRect", gp_GdipIsVisibleRect, NULL },
    { "GdipIsVisibleRectI", gp_GdipIsVisibleRectI, NULL }, { "GdipSaveGraphics", gp_GdipSaveGraphics, NULL }, { "GdipRestoreGraphics", gp_GdipRestoreGraphics, NULL },
    { "GdipBeginContainer2", gp_GdipBeginContainer2, NULL }, { "GdipEndContainer", gp_GdipEndContainer, NULL }, { "GdipBeginContainer", gp_GdipBeginContainer, NULL },
    { "GdipBeginContainerI", gp_GdipBeginContainerI, NULL }, { "GdipCreateMatrix", gp_GdipCreateMatrix, NULL }, { "GdipCreateMatrix2", gp_GdipCreateMatrix2, NULL },
    { "GdipCreateMatrix3", gp_GdipCreateMatrix3, NULL }, { "GdipCreateMatrix3I", gp_GdipCreateMatrix3I, NULL }, { "GdipCloneMatrix", gp_GdipCloneMatrix, NULL },
    { "GdipDeleteMatrix", gp_GdipDeleteMatrix, NULL }, { "GdipSetMatrixElements", gp_GdipSetMatrixElements, NULL }, { "GdipMultiplyMatrix", gp_GdipMultiplyMatrix, NULL },
    { "GdipTranslateMatrix", gp_GdipTranslateMatrix, NULL }, { "GdipScaleMatrix", gp_GdipScaleMatrix, NULL }, { "GdipRotateMatrix", gp_GdipRotateMatrix, NULL },
    { "GdipShearMatrix", gp_GdipShearMatrix, NULL }, { "GdipInvertMatrix", gp_GdipInvertMatrix, NULL }, { "GdipTransformMatrixPoints", gp_GdipTransformMatrixPoints, NULL },
    { "GdipTransformMatrixPointsI", gp_GdipTransformMatrixPointsI, NULL }, { "GdipVectorTransformMatrixPoints", gp_GdipVectorTransformMatrixPoints, NULL }, { "GdipVectorTransformMatrixPointsI", gp_GdipVectorTransformMatrixPointsI, NULL },
    { "GdipGetMatrixElements", gp_GdipGetMatrixElements, NULL }, { "GdipIsMatrixInvertible", gp_GdipIsMatrixInvertible, NULL }, { "GdipIsMatrixIdentity", gp_GdipIsMatrixIdentity, NULL },
    { "GdipIsMatrixEqual", gp_GdipIsMatrixEqual, NULL }, { "GdipDrawPath", gp_GdipDrawPath, NULL }, { "GdipFillPath", gp_GdipFillPath, NULL },
    { "GdipFillRegion", gp_GdipFillRegion, NULL }, { "GdipCreatePen1", gp_GdipCreatePen1, NULL }, { "GdipCreatePen2", gp_GdipCreatePen2, NULL },
    { "GdipClonePen", gp_GdipClonePen, NULL }, { "GdipDeletePen", gp_GdipDeletePen, NULL }, { "GdipSetPenLineCap197819", gp_GdipSetPenLineCap197819, NULL },
    { "GdipSetPenColor", gp_GdipSetPenColor, NULL }, { "GdipGetPenColor", gp_GdipGetPenColor, NULL }, { "GdipSetPenBrushFill", gp_GdipSetPenBrushFill, NULL },
    { "GdipGetPenBrushFill", gp_GdipGetPenBrushFill, NULL }, { "GdipGetPenFillType", gp_GdipGetPenFillType, NULL }, { "GdipSetPenDashStyle", gp_GdipSetPenDashStyle, NULL },
    { "GdipGetPenDashCount", gp_GdipGetPenDashCount, NULL }, { "GdipSetPenDashArray", gp_GdipSetPenDashArray, NULL }, { "GdipGetPenDashArray", gp_GdipGetPenDashArray, NULL },
    { "GdipSetPenCompoundArray", gp_GdipSetPenCompoundArray, NULL }, { "GdipGetPenCompoundCount", gp_GdipGetPenCompoundCount, NULL }, { "GdipSetPenTransform", gp_GdipSetPenTransform, NULL },
    { "GdipGetPenTransform", gp_GdipGetPenTransform, NULL }, { "GdipResetPenTransform", gp_GdipResetPenTransform, NULL }, { "GdipMultiplyPenTransform", gp_GdipMultiplyPenTransform, NULL },
    { "GdipTranslatePenTransform", gp_GdipTranslatePenTransform, NULL }, { "GdipScalePenTransform", gp_GdipScalePenTransform, NULL }, { "GdipRotatePenTransform", gp_GdipRotatePenTransform, NULL },
    { "GdipSetPenCustomStartCap", gp_GdipSetPenCustomStartCap, NULL }, { "GdipSetPenCustomEndCap", gp_GdipSetPenCustomEndCap, NULL }, { "GdipCreateSolidFill", gp_GdipCreateSolidFill, NULL },
    { "GdipSetSolidFillColor", gp_GdipSetSolidFillColor, NULL }, { "GdipGetSolidFillColor", gp_GdipGetSolidFillColor, NULL }, { "GdipCreateHatchBrush", gp_GdipCreateHatchBrush, NULL },
    { "GdipGetHatchStyle", gp_GdipGetHatchStyle, NULL }, { "GdipGetHatchForegroundColor", gp_GdipGetHatchForegroundColor, NULL }, { "GdipGetHatchBackgroundColor", gp_GdipGetHatchBackgroundColor, NULL },
    { "GdipCloneBrush", gp_GdipCloneBrush, NULL }, { "GdipDeleteBrush", gp_GdipDeleteBrush, NULL }, { "GdipGetBrushType", gp_GdipGetBrushType, NULL },
    { "GdipCreateTexture", gp_GdipCreateTexture, NULL }, { "GdipGetTextureImage", gp_GdipGetTextureImage, NULL }, { "GdipSetTextureWrapMode", gp_GdipSetTextureWrapMode, NULL },
    { "GdipGetTextureWrapMode", gp_GdipGetTextureWrapMode, NULL }, { "GdipSetLineColors", gp_GdipSetLineColors, NULL }, { "GdipGetLineColors", gp_GdipGetLineColors, NULL },
    { "GdipSetLineGammaCorrection", gp_GdipSetLineGammaCorrection, NULL }, { "GdipGetLineGammaCorrection", gp_GdipGetLineGammaCorrection, NULL }, { "GdipSetLineLinearBlend", gp_GdipSetLineLinearBlend, NULL },
    { "GdipSetLineSigmaBlend", gp_GdipSetLineSigmaBlend, NULL }, { "GdipGetLineBlendCount", gp_GdipGetLineBlendCount, NULL }, { "GdipGetLineBlend", gp_GdipGetLineBlend, NULL },
    { "GdipSetLineBlend", gp_GdipSetLineBlend, NULL }, { "GdipGetLinePresetBlendCount", gp_GdipGetLinePresetBlendCount, NULL }, { "GdipGetLinePresetBlend", gp_GdipGetLinePresetBlend, NULL },
    { "GdipSetLinePresetBlend", gp_GdipSetLinePresetBlend, NULL }, { "GdipSetLineWrapMode", gp_GdipSetLineWrapMode, NULL }, { "GdipGetLineWrapMode", gp_GdipGetLineWrapMode, NULL },
    { "GdipCreatePathGradientFromPath", gp_GdipCreatePathGradientFromPath, NULL }, { "GdipGetPathGradientCenterColor", gp_GdipGetPathGradientCenterColor, NULL }, { "GdipSetPathGradientCenterColor", gp_GdipSetPathGradientCenterColor, NULL },
    { "GdipGetPathGradientSurroundColorsWithCount", gp_GdipGetPathGradientSurroundColorsWithCount, NULL }, { "GdipSetPathGradientSurroundColorsWithCount", gp_GdipSetPathGradientSurroundColorsWithCount, NULL }, { "GdipGetPathGradientSurroundColorCount", gp_GdipGetPathGradientSurroundColorCount, NULL },
    { "GdipGetPathGradientPointCount", gp_GdipGetPathGradientPointCount, NULL }, { "GdipSetPathGradientFocusScales", gp_GdipSetPathGradientFocusScales, NULL }, { "GdipGetPathGradientFocusScales", gp_GdipGetPathGradientFocusScales, NULL },
    { "GdipSetPathGradientWrapMode", gp_GdipSetPathGradientWrapMode, NULL }, { "GdipGetPathGradientWrapMode", gp_GdipGetPathGradientWrapMode, NULL }, { "GdipSetPathGradientLinearBlend", gp_GdipSetPathGradientLinearBlend, NULL },
    { "GdipSetPathGradientSigmaBlend", gp_GdipSetPathGradientSigmaBlend, NULL }, { "GdipSetPathGradientBlend", gp_GdipSetPathGradientBlend, NULL }, { "GdipGetPathGradientBlendCount", gp_GdipGetPathGradientBlendCount, NULL },
    { "GdipSetPathGradientPresetBlend", gp_GdipSetPathGradientPresetBlend, NULL }, { "GdipCreatePath", gp_GdipCreatePath, NULL }, { "GdipClonePath", gp_GdipClonePath, NULL },
    { "GdipDeletePath", gp_GdipDeletePath, NULL }, { "GdipResetPath", gp_GdipResetPath, NULL }, { "GdipGetPointCount", gp_GdipGetPointCount, NULL },
    { "GdipGetPathTypes", gp_GdipGetPathTypes, NULL }, { "GdipGetPathData", gp_GdipGetPathData, NULL }, { "GdipGetPathFillMode", gp_GdipGetPathFillMode, NULL },
    { "GdipSetPathFillMode", gp_GdipSetPathFillMode, NULL }, { "GdipStartPathFigure", gp_GdipStartPathFigure, NULL }, { "GdipClosePathFigure", gp_GdipClosePathFigure, NULL },
    { "GdipClosePathFigures", gp_GdipClosePathFigures, NULL }, { "GdipSetPathMarker", gp_GdipSetPathMarker, NULL }, { "GdipClearPathMarkers", gp_GdipClearPathMarkers, NULL },
    { "GdipReversePath", gp_GdipReversePath, NULL }, { "GdipGetPathLastPoint", gp_GdipGetPathLastPoint, NULL }, { "GdipAddPathPath", gp_GdipAddPathPath, NULL },
    { "GdipAddPathString", gp_GdipAddPathString, NULL }, { "GdipAddPathStringI", gp_GdipAddPathStringI, NULL }, { "GdipTransformPath", gp_GdipTransformPath, NULL },
    { "GdipFlattenPath", gp_GdipFlattenPath, NULL }, { "GdipWindingModeOutline", gp_GdipWindingModeOutline, NULL }, { "GdipWidenPath", gp_GdipWidenPath, NULL },
    { "GdipWarpPath", gp_GdipWarpPath, NULL }, { "GdipCreatePathIter", gp_GdipCreatePathIter, NULL }, { "GdipDeletePathIter", gp_GdipDeletePathIter, NULL },
    { "GdipPathIterGetCount", gp_GdipPathIterGetCount, NULL }, { "GdipPathIterGetSubpathCount", gp_GdipPathIterGetSubpathCount, NULL }, { "GdipPathIterHasCurve", gp_GdipPathIterHasCurve, NULL },
    { "GdipPathIterRewind", gp_GdipPathIterRewind, NULL }, { "GdipPathIterNextSubpath", gp_GdipPathIterNextSubpath, NULL }, { "GdipPathIterEnumerate", gp_GdipPathIterEnumerate, NULL },
    { "GdipPathIterCopyData", gp_GdipPathIterCopyData, NULL }, { "GdipCreateRegion", gp_GdipCreateRegion, NULL }, { "GdipCreateRegionPath", gp_GdipCreateRegionPath, NULL },
    { "GdipCreateRegionRgnData", gp_GdipCreateRegionRgnData, NULL }, { "GdipCreateRegionHrgn", gp_GdipCreateRegionHrgn, NULL }, { "GdipCloneRegion", gp_GdipCloneRegion, NULL },
    { "GdipDeleteRegion", gp_GdipDeleteRegion, NULL }, { "GdipSetInfinite", gp_GdipSetInfinite, NULL }, { "GdipSetEmpty", gp_GdipSetEmpty, NULL },
    { "GdipCombineRegionPath", gp_GdipCombineRegionPath, NULL }, { "GdipCombineRegionRegion", gp_GdipCombineRegionRegion, NULL }, { "GdipTransformRegion", gp_GdipTransformRegion, NULL },
    { "GdipGetRegionHRgn", gp_GdipGetRegionHRgn, NULL }, { "GdipIsEmptyRegion", gp_GdipIsEmptyRegion, NULL }, { "GdipIsInfiniteRegion", gp_GdipIsInfiniteRegion, NULL },
    { "GdipIsEqualRegion", gp_GdipIsEqualRegion, NULL }, { "GdipGetRegionDataSize", gp_GdipGetRegionDataSize, NULL }, { "GdipGetRegionData", gp_GdipGetRegionData, NULL },
    { "GdipGetRegionScansCount", gp_GdipGetRegionScansCount, NULL }, { "GdipGetRegionScans", gp_GdipGetRegionScans, NULL }, { "GdipGetRegionScansI", gp_GdipGetRegionScansI, NULL },
    { "GdipLoadImageFromFile", gp_GdipLoadImageFromFile, NULL }, { "GdipLoadImageFromFileICM", gp_GdipLoadImageFromFileICM, NULL }, { "GdipCreateBitmapFromFile", gp_GdipCreateBitmapFromFile, NULL },
    { "GdipCreateBitmapFromFileICM", gp_GdipCreateBitmapFromFileICM, NULL }, { "GdipLoadImageFromStream", gp_GdipLoadImageFromStream, NULL }, { "GdipLoadImageFromStreamICM", gp_GdipLoadImageFromStreamICM, NULL },
    { "GdipCreateBitmapFromStream", gp_GdipCreateBitmapFromStream, NULL }, { "GdipCreateBitmapFromStreamICM", gp_GdipCreateBitmapFromStreamICM, NULL }, { "GdipCreateBitmapFromScan0", gp_GdipCreateBitmapFromScan0, NULL },
    { "GdipCreateBitmapFromGraphics", gp_GdipCreateBitmapFromGraphics, NULL }, { "GdipCreateBitmapFromHBITMAP", gp_GdipCreateBitmapFromHBITMAP, NULL }, { "GdipCreateBitmapFromGdiDib", gp_GdipCreateBitmapFromGdiDib, NULL },
    { "GdipCreateBitmapFromHICON", gp_GdipCreateBitmapFromHICON, NULL }, { "GdipCreateBitmapFromResource", gp_GdipCreateBitmapFromResource, NULL }, { "GdipCreateBitmapFromDirectDrawSurface", gp_GdipCreateBitmapFromDirectDrawSurface, NULL },
    { "GdipCreateHBITMAPFromBitmap", gp_GdipCreateHBITMAPFromBitmap, NULL }, { "GdipCreateHICONFromBitmap", gp_GdipCreateHICONFromBitmap, NULL }, { "GdipCloneImage", gp_GdipCloneImage, NULL },
    { "GdipDisposeImage", gp_GdipDisposeImage, NULL }, { "GdipGetImageWidth", gp_GdipGetImageWidth, NULL }, { "GdipGetImageHeight", gp_GdipGetImageHeight, NULL },
    { "GdipGetImageDimension", gp_GdipGetImageDimension, NULL }, { "GdipGetImageBounds", gp_GdipGetImageBounds, NULL }, { "GdipGetImagePixelFormat", gp_GdipGetImagePixelFormat, NULL },
    { "GdipGetImageType", gp_GdipGetImageType, NULL }, { "GdipGetImageFlags", gp_GdipGetImageFlags, NULL }, { "GdipGetImageRawFormat", gp_GdipGetImageRawFormat, NULL },
    { "GdipBitmapSetResolution", gp_GdipBitmapSetResolution, NULL }, { "GdipImageForceValidation", gp_GdipImageForceValidation, NULL }, { "GdipBitmapLockBits", gp_GdipBitmapLockBits, NULL },
    { "GdipBitmapUnlockBits", gp_GdipBitmapUnlockBits, NULL }, { "GdipBitmapGetPixel", gp_GdipBitmapGetPixel, NULL }, { "GdipBitmapSetPixel", gp_GdipBitmapSetPixel, NULL },
    { "GdipImageGetFrameDimensionsCount", gp_GdipImageGetFrameDimensionsCount, NULL }, { "GdipImageGetFrameDimensionsList", gp_GdipImageGetFrameDimensionsList, NULL }, { "GdipImageGetFrameCount", gp_GdipImageGetFrameCount, NULL },
    { "GdipImageSelectActiveFrame", gp_GdipImageSelectActiveFrame, NULL }, { "GdipImageRotateFlip", gp_GdipImageRotateFlip, NULL }, { "GdipGetImageThumbnail", gp_GdipGetImageThumbnail, NULL },
    { "GdipGetImagePaletteSize", gp_GdipGetImagePaletteSize, NULL }, { "GdipGetImagePalette", gp_GdipGetImagePalette, NULL }, { "GdipSetImagePalette", gp_GdipSetImagePalette, NULL },
    { "GdipGetPropertyCount", gp_GdipGetPropertyCount, NULL }, { "GdipGetPropertyIdList", gp_GdipGetPropertyIdList, NULL }, { "GdipGetPropertyItemSize", gp_GdipGetPropertyItemSize, NULL },
    { "GdipGetPropertyItem", gp_GdipGetPropertyItem, NULL }, { "GdipGetPropertySize", gp_GdipGetPropertySize, NULL }, { "GdipGetAllPropertyItems", gp_GdipGetAllPropertyItems, NULL },
    { "GdipRemovePropertyItem", gp_GdipRemovePropertyItem, NULL }, { "GdipSetPropertyItem", gp_GdipSetPropertyItem, NULL }, { "GdipGetImageDecodersSize", gp_GdipGetImageDecodersSize, NULL },
    { "GdipGetImageDecoders", gp_GdipGetImageDecoders, NULL }, { "GdipGetImageEncodersSize", gp_GdipGetImageEncodersSize, NULL }, { "GdipGetImageEncoders", gp_GdipGetImageEncoders, NULL },
    { "GdipGetEncoderParameterListSize", gp_GdipGetEncoderParameterListSize, NULL }, { "GdipGetEncoderParameterList", gp_GdipGetEncoderParameterList, NULL }, { "GdipSaveImageToFile", gp_GdipSaveImageToFile, NULL },
    { "GdipSaveImageToStream", gp_GdipSaveImageToStream, NULL }, { "GdipSaveAdd", gp_GdipSaveAdd, NULL }, { "GdipSaveAddImage", gp_GdipSaveAddImage, NULL },
    { "GdipCreateImageAttributes", gp_GdipCreateImageAttributes, NULL }, { "GdipCloneImageAttributes", gp_GdipCloneImageAttributes, NULL }, { "GdipDisposeImageAttributes", gp_GdipDisposeImageAttributes, NULL },
    { "GdipSetImageAttributesToIdentity", gp_GdipSetImageAttributesToIdentity, NULL }, { "GdipResetImageAttributes", gp_GdipResetImageAttributes, NULL }, { "GdipSetImageAttributesColorMatrix", gp_GdipSetImageAttributesColorMatrix, NULL },
    { "GdipSetImageAttributesThreshold", gp_GdipSetImageAttributesThreshold, NULL }, { "GdipSetImageAttributesGamma", gp_GdipSetImageAttributesGamma, NULL }, { "GdipSetImageAttributesNoOp", gp_GdipSetImageAttributesNoOp, NULL },
    { "GdipSetImageAttributesColorKeys", gp_GdipSetImageAttributesColorKeys, NULL }, { "GdipSetImageAttributesOutputChannel", gp_GdipSetImageAttributesOutputChannel, NULL }, { "GdipSetImageAttributesOutputChannelColorProfile", gp_GdipSetImageAttributesOutputChannelColorProfile, NULL },
    { "GdipSetImageAttributesRemapTable", gp_GdipSetImageAttributesRemapTable, NULL }, { "GdipSetImageAttributesCachedBackground", gp_GdipSetImageAttributesCachedBackground, NULL }, { "GdipSetImageAttributesWrapMode", gp_GdipSetImageAttributesWrapMode, NULL },
    { "GdipGetImageAttributesAdjustedPalette", gp_GdipGetImageAttributesAdjustedPalette, NULL }, { "GdipCreateCachedBitmap", gp_GdipCreateCachedBitmap, NULL }, { "GdipDeleteCachedBitmap", gp_GdipDeleteCachedBitmap, NULL },
    { "GdipDrawCachedBitmap", gp_GdipDrawCachedBitmap, NULL }, { "GdipCreateFontFamilyFromName", gp_GdipCreateFontFamilyFromName, NULL }, { "GdipDeleteFontFamily", gp_GdipDeleteFontFamily, NULL },
    { "GdipCloneFontFamily", gp_GdipCloneFontFamily, NULL }, { "GdipGetGenericFontFamilySansSerif", gp_GdipGetGenericFontFamilySansSerif, NULL }, { "GdipGetGenericFontFamilySerif", gp_GdipGetGenericFontFamilySerif, NULL },
    { "GdipGetGenericFontFamilyMonospace", gp_GdipGetGenericFontFamilyMonospace, NULL }, { "GdipGetFamilyName", gp_GdipGetFamilyName, NULL }, { "GdipIsStyleAvailable", gp_GdipIsStyleAvailable, NULL },
    { "GdipGetEmHeight", gp_GdipGetEmHeight, NULL }, { "GdipGetCellAscent", gp_GdipGetCellAscent, NULL }, { "GdipGetCellDescent", gp_GdipGetCellDescent, NULL },
    { "GdipGetLineSpacing", gp_GdipGetLineSpacing, NULL }, { "GdipNewInstalledFontCollection", gp_GdipNewInstalledFontCollection, NULL }, { "GdipNewPrivateFontCollection", gp_GdipNewPrivateFontCollection, NULL },
    { "GdipDeletePrivateFontCollection", gp_GdipDeletePrivateFontCollection, NULL }, { "GdipPrivateAddFontFile", gp_GdipPrivateAddFontFile, NULL }, { "GdipPrivateAddMemoryFont", gp_GdipPrivateAddMemoryFont, NULL },
    { "GdipGetFontCollectionFamilyCount", gp_GdipGetFontCollectionFamilyCount, NULL }, { "GdipGetFontCollectionFamilyList", gp_GdipGetFontCollectionFamilyList, NULL }, { "GdipCreateFont", gp_GdipCreateFont, NULL },
    { "GdipCloneFont", gp_GdipCloneFont, NULL }, { "GdipDeleteFont", gp_GdipDeleteFont, NULL }, { "GdipGetFamily", gp_GdipGetFamily, NULL },
    { "GdipGetFontHeight", gp_GdipGetFontHeight, NULL }, { "GdipGetFontHeightGivenDPI", gp_GdipGetFontHeightGivenDPI, NULL }, { "GdipCreateFontFromLogfontA", gp_GdipCreateFontFromLogfontA, NULL },
    { "GdipCreateFontFromLogfontW", gp_GdipCreateFontFromLogfontW, NULL }, { "GdipCreateFontFromDC", gp_GdipCreateFontFromDC, NULL }, { "GdipGetLogFontA", gp_GdipGetLogFontA, NULL },
    { "GdipGetLogFontW", gp_GdipGetLogFontW, NULL }, { "GdipCreateStringFormat", gp_GdipCreateStringFormat, NULL }, { "GdipStringFormatGetGenericDefault", gp_GdipStringFormatGetGenericDefault, NULL },
    { "GdipStringFormatGetGenericTypographic", gp_GdipStringFormatGetGenericTypographic, NULL }, { "GdipDeleteStringFormat", gp_GdipDeleteStringFormat, NULL }, { "GdipCloneStringFormat", gp_GdipCloneStringFormat, NULL },
    { "GdipSetStringFormatTabStops", gp_GdipSetStringFormatTabStops, NULL }, { "GdipGetStringFormatTabStops", gp_GdipGetStringFormatTabStops, NULL }, { "GdipGetStringFormatTabStopCount", gp_GdipGetStringFormatTabStopCount, NULL },
    { "GdipSetStringFormatMeasurableCharacterRanges", gp_GdipSetStringFormatMeasurableCharacterRanges, NULL }, { "GdipGetStringFormatMeasurableCharacterRangeCount", gp_GdipGetStringFormatMeasurableCharacterRangeCount, NULL }, { "GdipSetStringFormatDigitSubstitution", gp_GdipSetStringFormatDigitSubstitution, NULL },
    { "GdipGetStringFormatDigitSubstitution", gp_GdipGetStringFormatDigitSubstitution, NULL }, { "GdipDrawString", gp_GdipDrawString, NULL }, { "GdipMeasureString", gp_GdipMeasureString, NULL },
    { "GdipMeasureCharacterRanges", gp_GdipMeasureCharacterRanges, NULL }, { "GdipDrawDriverString", gp_GdipDrawDriverString, NULL }, { "GdipMeasureDriverString", gp_GdipMeasureDriverString, NULL },
    { "GdipCreateFromHWNDICM", gp_GdipCreateFromHWNDICM, NULL }, { "GdipResetPageTransform", gp_GdipResetPageTransform, NULL }, { "GdipCloneCustomLineCap", gp_GdipCloneCustomLineCap, NULL },
    { "GdipCreateAdjustableArrowCap", gp_GdipCreateAdjustableArrowCap, NULL }, { "GdipCreateCustomLineCap", gp_GdipCreateCustomLineCap, NULL }, { "GdipCreateHalftonePalette", gp_GdipCreateHalftonePalette, NULL },
    { "GdipCreateMetafileFromEmf", gp_GdipCreateMetafileFromEmf, NULL }, { "GdipCreateMetafileFromFile", gp_GdipCreateMetafileFromFile, NULL }, { "GdipCreateMetafileFromStream", gp_GdipCreateMetafileFromStream, NULL },
    { "GdipCreateMetafileFromWmf", gp_GdipCreateMetafileFromWmf, NULL }, { "GdipCreateMetafileFromWmfFile", gp_GdipCreateMetafileFromWmfFile, NULL }, { "GdipCreateStreamOnFile", gp_GdipCreateStreamOnFile, NULL },
    { "GdipDeleteCustomLineCap", gp_GdipDeleteCustomLineCap, NULL }, { "GdipEmfToWmfBits", gp_GdipEmfToWmfBits, NULL }, { "GdipEnumerateMetafileDestPoint", gp_GdipEnumerateMetafileDestPoint, NULL },
    { "GdipEnumerateMetafileDestPointI", gp_GdipEnumerateMetafileDestPointI, NULL }, { "GdipEnumerateMetafileDestPoints", gp_GdipEnumerateMetafileDestPoints, NULL }, { "GdipEnumerateMetafileDestPointsI", gp_GdipEnumerateMetafileDestPointsI, NULL },
    { "GdipEnumerateMetafileDestRect", gp_GdipEnumerateMetafileDestRect, NULL }, { "GdipEnumerateMetafileDestRectI", gp_GdipEnumerateMetafileDestRectI, NULL }, { "GdipEnumerateMetafileSrcRectDestPoint", gp_GdipEnumerateMetafileSrcRectDestPoint, NULL },
    { "GdipEnumerateMetafileSrcRectDestPointI", gp_GdipEnumerateMetafileSrcRectDestPointI, NULL }, { "GdipEnumerateMetafileSrcRectDestPoints", gp_GdipEnumerateMetafileSrcRectDestPoints, NULL }, { "GdipEnumerateMetafileSrcRectDestPointsI", gp_GdipEnumerateMetafileSrcRectDestPointsI, NULL },
    { "GdipEnumerateMetafileSrcRectDestRect", gp_GdipEnumerateMetafileSrcRectDestRect, NULL }, { "GdipEnumerateMetafileSrcRectDestRectI", gp_GdipEnumerateMetafileSrcRectDestRectI, NULL }, { "GdipGetAdjustableArrowCapFillState", gp_GdipGetAdjustableArrowCapFillState, NULL },
    { "GdipGetAdjustableArrowCapHeight", gp_GdipGetAdjustableArrowCapHeight, NULL }, { "GdipGetAdjustableArrowCapMiddleInset", gp_GdipGetAdjustableArrowCapMiddleInset, NULL }, { "GdipGetAdjustableArrowCapWidth", gp_GdipGetAdjustableArrowCapWidth, NULL },
    { "GdipGetCustomLineCapBaseCap", gp_GdipGetCustomLineCapBaseCap, NULL }, { "GdipGetCustomLineCapBaseInset", gp_GdipGetCustomLineCapBaseInset, NULL }, { "GdipGetCustomLineCapStrokeCaps", gp_GdipGetCustomLineCapStrokeCaps, NULL },
    { "GdipGetCustomLineCapStrokeJoin", gp_GdipGetCustomLineCapStrokeJoin, NULL }, { "GdipGetCustomLineCapType", gp_GdipGetCustomLineCapType, NULL }, { "GdipGetCustomLineCapWidthScale", gp_GdipGetCustomLineCapWidthScale, NULL },
    { "GdipGetHemfFromMetafile", gp_GdipGetHemfFromMetafile, NULL }, { "GdipGetMetafileDownLevelRasterizationLimit", gp_GdipGetMetafileDownLevelRasterizationLimit, NULL }, { "GdipGetMetafileHeaderFromEmf", gp_GdipGetMetafileHeaderFromEmf, NULL },
    { "GdipGetMetafileHeaderFromFile", gp_GdipGetMetafileHeaderFromFile, NULL }, { "GdipGetMetafileHeaderFromMetafile", gp_GdipGetMetafileHeaderFromMetafile, NULL }, { "GdipGetMetafileHeaderFromStream", gp_GdipGetMetafileHeaderFromStream, NULL },
    { "GdipGetMetafileHeaderFromWmf", gp_GdipGetMetafileHeaderFromWmf, NULL }, { "GdipGetPathGradientBlend", gp_GdipGetPathGradientBlend, NULL }, { "GdipGetPathGradientGammaCorrection", gp_GdipGetPathGradientGammaCorrection, NULL },
    { "GdipGetPathGradientPath", gp_GdipGetPathGradientPath, NULL }, { "GdipGetPathGradientPresetBlend", gp_GdipGetPathGradientPresetBlend, NULL }, { "GdipGetPathGradientPresetBlendCount", gp_GdipGetPathGradientPresetBlendCount, NULL },
    { "GdipGetPenCompoundArray", gp_GdipGetPenCompoundArray, NULL }, { "GdipGetPenCustomEndCap", gp_GdipGetPenCustomEndCap, NULL }, { "GdipGetPenCustomStartCap", gp_GdipGetPenCustomStartCap, NULL },
    { "GdipPathIterIsValid", gp_GdipPathIterIsValid, NULL }, { "GdipPathIterNextMarker", gp_GdipPathIterNextMarker, NULL }, { "GdipPathIterNextMarkerPath", gp_GdipPathIterNextMarkerPath, NULL },
    { "GdipPathIterNextPathType", gp_GdipPathIterNextPathType, NULL }, { "GdipPathIterNextSubpathPath", gp_GdipPathIterNextSubpathPath, NULL }, { "GdipPlayMetafileRecord", gp_GdipPlayMetafileRecord, NULL },
    { "GdipRecordMetafile", gp_GdipRecordMetafile, NULL }, { "GdipRecordMetafileFileName", gp_GdipRecordMetafileFileName, NULL }, { "GdipRecordMetafileFileNameI", gp_GdipRecordMetafileFileNameI, NULL },
    { "GdipRecordMetafileI", gp_GdipRecordMetafileI, NULL }, { "GdipRecordMetafileStream", gp_GdipRecordMetafileStream, NULL }, { "GdipRecordMetafileStreamI", gp_GdipRecordMetafileStreamI, NULL },
    { "GdipSetAdjustableArrowCapFillState", gp_GdipSetAdjustableArrowCapFillState, NULL }, { "GdipSetAdjustableArrowCapHeight", gp_GdipSetAdjustableArrowCapHeight, NULL }, { "GdipSetAdjustableArrowCapMiddleInset", gp_GdipSetAdjustableArrowCapMiddleInset, NULL },
    { "GdipSetAdjustableArrowCapWidth", gp_GdipSetAdjustableArrowCapWidth, NULL }, { "GdipSetCustomLineCapBaseCap", gp_GdipSetCustomLineCapBaseCap, NULL }, { "GdipSetCustomLineCapBaseInset", gp_GdipSetCustomLineCapBaseInset, NULL },
    { "GdipSetCustomLineCapStrokeCaps", gp_GdipSetCustomLineCapStrokeCaps, NULL }, { "GdipSetCustomLineCapStrokeJoin", gp_GdipSetCustomLineCapStrokeJoin, NULL }, { "GdipSetCustomLineCapWidthScale", gp_GdipSetCustomLineCapWidthScale, NULL },
    { "GdipSetMetafileDownLevelRasterizationLimit", gp_GdipSetMetafileDownLevelRasterizationLimit, NULL }, { "GdipSetPathGradientGammaCorrection", gp_GdipSetPathGradientGammaCorrection, NULL }, { "GdipSetPathGradientPath", gp_GdipSetPathGradientPath, NULL },
    { "GdipDrawLine", gp_GdipDrawLine, NULL }, { "GdipDrawLineI", gp_GdipDrawLineI, NULL }, { "GdipDrawLines", gp_GdipDrawLines, NULL },
    { "GdipDrawLinesI", gp_GdipDrawLinesI, NULL }, { "GdipDrawRectangle", gp_GdipDrawRectangle, NULL }, { "GdipDrawRectangleI", gp_GdipDrawRectangleI, NULL },
    { "GdipDrawRectangles", gp_GdipDrawRectangles, NULL }, { "GdipDrawRectanglesI", gp_GdipDrawRectanglesI, NULL }, { "GdipDrawEllipse", gp_GdipDrawEllipse, NULL },
    { "GdipDrawEllipseI", gp_GdipDrawEllipseI, NULL }, { "GdipDrawArc", gp_GdipDrawArc, NULL }, { "GdipDrawArcI", gp_GdipDrawArcI, NULL },
    { "GdipDrawPie", gp_GdipDrawPie, NULL }, { "GdipDrawPieI", gp_GdipDrawPieI, NULL }, { "GdipDrawPolygon", gp_GdipDrawPolygon, NULL },
    { "GdipDrawPolygonI", gp_GdipDrawPolygonI, NULL }, { "GdipDrawBezier", gp_GdipDrawBezier, NULL }, { "GdipDrawBezierI", gp_GdipDrawBezierI, NULL },
    { "GdipDrawBeziers", gp_GdipDrawBeziers, NULL }, { "GdipDrawBeziersI", gp_GdipDrawBeziersI, NULL }, { "GdipDrawCurve", gp_GdipDrawCurve, NULL },
    { "GdipDrawCurveI", gp_GdipDrawCurveI, NULL }, { "GdipDrawCurve2", gp_GdipDrawCurve2, NULL }, { "GdipDrawCurve2I", gp_GdipDrawCurve2I, NULL },
    { "GdipDrawCurve3", gp_GdipDrawCurve3, NULL }, { "GdipDrawCurve3I", gp_GdipDrawCurve3I, NULL }, { "GdipDrawClosedCurve", gp_GdipDrawClosedCurve, NULL },
    { "GdipDrawClosedCurveI", gp_GdipDrawClosedCurveI, NULL }, { "GdipDrawClosedCurve2", gp_GdipDrawClosedCurve2, NULL }, { "GdipDrawClosedCurve2I", gp_GdipDrawClosedCurve2I, NULL },
    { "GdipFillRectangle", gp_GdipFillRectangle, NULL }, { "GdipFillRectangleI", gp_GdipFillRectangleI, NULL }, { "GdipFillRectangles", gp_GdipFillRectangles, NULL },
    { "GdipFillRectanglesI", gp_GdipFillRectanglesI, NULL }, { "GdipFillEllipse", gp_GdipFillEllipse, NULL }, { "GdipFillEllipseI", gp_GdipFillEllipseI, NULL },
    { "GdipFillPie", gp_GdipFillPie, NULL }, { "GdipFillPieI", gp_GdipFillPieI, NULL }, { "GdipFillPolygon", gp_GdipFillPolygon, NULL },
    { "GdipFillPolygonI", gp_GdipFillPolygonI, NULL }, { "GdipFillPolygon2", gp_GdipFillPolygon2, NULL }, { "GdipFillPolygon2I", gp_GdipFillPolygon2I, NULL },
    { "GdipFillClosedCurve", gp_GdipFillClosedCurve, NULL }, { "GdipFillClosedCurveI", gp_GdipFillClosedCurveI, NULL }, { "GdipFillClosedCurve2", gp_GdipFillClosedCurve2, NULL },
    { "GdipFillClosedCurve2I", gp_GdipFillClosedCurve2I, NULL }, { "GdipCreateTexture2", gp_GdipCreateTexture2, NULL }, { "GdipCreateTexture2I", gp_GdipCreateTexture2I, NULL },
    { "GdipCreateTextureIA", gp_GdipCreateTextureIA, NULL }, { "GdipCreateTextureIAI", gp_GdipCreateTextureIAI, NULL }, { "GdipCreateLineBrush", gp_GdipCreateLineBrush, NULL },
    { "GdipCreateLineBrushI", gp_GdipCreateLineBrushI, NULL }, { "GdipCreateLineBrushFromRect", gp_GdipCreateLineBrushFromRect, NULL }, { "GdipCreateLineBrushFromRectI", gp_GdipCreateLineBrushFromRectI, NULL },
    { "GdipCreateLineBrushFromRectWithAngle", gp_GdipCreateLineBrushFromRectWithAngle, NULL }, { "GdipCreateLineBrushFromRectWithAngleI", gp_GdipCreateLineBrushFromRectWithAngleI, NULL }, { "GdipGetLineRect", gp_GdipGetLineRect, NULL },
    { "GdipGetLineRectI", gp_GdipGetLineRectI, NULL }, { "GdipCreatePathGradient", gp_GdipCreatePathGradient, NULL }, { "GdipCreatePathGradientI", gp_GdipCreatePathGradientI, NULL },
    { "GdipGetPathGradientCenterPoint", gp_GdipGetPathGradientCenterPoint, NULL }, { "GdipGetPathGradientCenterPointI", gp_GdipGetPathGradientCenterPointI, NULL }, { "GdipSetPathGradientCenterPoint", gp_GdipSetPathGradientCenterPoint, NULL },
    { "GdipSetPathGradientCenterPointI", gp_GdipSetPathGradientCenterPointI, NULL }, { "GdipGetPathGradientRect", gp_GdipGetPathGradientRect, NULL }, { "GdipGetPathGradientRectI", gp_GdipGetPathGradientRectI, NULL },
    { "GdipCreatePath2", gp_GdipCreatePath2, NULL }, { "GdipCreatePath2I", gp_GdipCreatePath2I, NULL }, { "GdipGetPathPoints", gp_GdipGetPathPoints, NULL },
    { "GdipGetPathPointsI", gp_GdipGetPathPointsI, NULL }, { "GdipAddPathLine", gp_GdipAddPathLine, NULL }, { "GdipAddPathLineI", gp_GdipAddPathLineI, NULL },
    { "GdipAddPathLine2", gp_GdipAddPathLine2, NULL }, { "GdipAddPathLine2I", gp_GdipAddPathLine2I, NULL }, { "GdipAddPathArc", gp_GdipAddPathArc, NULL },
    { "GdipAddPathArcI", gp_GdipAddPathArcI, NULL }, { "GdipAddPathBezier", gp_GdipAddPathBezier, NULL }, { "GdipAddPathBezierI", gp_GdipAddPathBezierI, NULL },
    { "GdipAddPathBeziers", gp_GdipAddPathBeziers, NULL }, { "GdipAddPathBeziersI", gp_GdipAddPathBeziersI, NULL }, { "GdipAddPathCurve", gp_GdipAddPathCurve, NULL },
    { "GdipAddPathCurveI", gp_GdipAddPathCurveI, NULL }, { "GdipAddPathCurve2", gp_GdipAddPathCurve2, NULL }, { "GdipAddPathCurve2I", gp_GdipAddPathCurve2I, NULL },
    { "GdipAddPathCurve3", gp_GdipAddPathCurve3, NULL }, { "GdipAddPathCurve3I", gp_GdipAddPathCurve3I, NULL }, { "GdipAddPathClosedCurve", gp_GdipAddPathClosedCurve, NULL },
    { "GdipAddPathClosedCurveI", gp_GdipAddPathClosedCurveI, NULL }, { "GdipAddPathClosedCurve2", gp_GdipAddPathClosedCurve2, NULL }, { "GdipAddPathClosedCurve2I", gp_GdipAddPathClosedCurve2I, NULL },
    { "GdipAddPathRectangle", gp_GdipAddPathRectangle, NULL }, { "GdipAddPathRectangleI", gp_GdipAddPathRectangleI, NULL }, { "GdipAddPathRectangles", gp_GdipAddPathRectangles, NULL },
    { "GdipAddPathRectanglesI", gp_GdipAddPathRectanglesI, NULL }, { "GdipAddPathEllipse", gp_GdipAddPathEllipse, NULL }, { "GdipAddPathEllipseI", gp_GdipAddPathEllipseI, NULL },
    { "GdipAddPathPie", gp_GdipAddPathPie, NULL }, { "GdipAddPathPieI", gp_GdipAddPathPieI, NULL }, { "GdipAddPathPolygon", gp_GdipAddPathPolygon, NULL },
    { "GdipAddPathPolygonI", gp_GdipAddPathPolygonI, NULL }, { "GdipGetPathWorldBounds", gp_GdipGetPathWorldBounds, NULL }, { "GdipGetPathWorldBoundsI", gp_GdipGetPathWorldBoundsI, NULL },
    { "GdipIsVisiblePathPoint", gp_GdipIsVisiblePathPoint, NULL }, { "GdipIsVisiblePathPointI", gp_GdipIsVisiblePathPointI, NULL }, { "GdipIsOutlineVisiblePathPoint", gp_GdipIsOutlineVisiblePathPoint, NULL },
    { "GdipIsOutlineVisiblePathPointI", gp_GdipIsOutlineVisiblePathPointI, NULL }, { "GdipCreateRegionRect", gp_GdipCreateRegionRect, NULL }, { "GdipCreateRegionRectI", gp_GdipCreateRegionRectI, NULL },
    { "GdipCombineRegionRect", gp_GdipCombineRegionRect, NULL }, { "GdipCombineRegionRectI", gp_GdipCombineRegionRectI, NULL }, { "GdipTranslateRegion", gp_GdipTranslateRegion, NULL },
    { "GdipTranslateRegionI", gp_GdipTranslateRegionI, NULL }, { "GdipGetRegionBounds", gp_GdipGetRegionBounds, NULL }, { "GdipGetRegionBoundsI", gp_GdipGetRegionBoundsI, NULL },
    { "GdipIsVisibleRegionPoint", gp_GdipIsVisibleRegionPoint, NULL }, { "GdipIsVisibleRegionPointI", gp_GdipIsVisibleRegionPointI, NULL }, { "GdipIsVisibleRegionRect", gp_GdipIsVisibleRegionRect, NULL },
    { "GdipIsVisibleRegionRectI", gp_GdipIsVisibleRegionRectI, NULL }, { "GdipCloneBitmapArea", gp_GdipCloneBitmapArea, NULL }, { "GdipCloneBitmapAreaI", gp_GdipCloneBitmapAreaI, NULL },
    { "GdipDrawImage", gp_GdipDrawImage, NULL }, { "GdipDrawImageI", gp_GdipDrawImageI, NULL }, { "GdipDrawImageRect", gp_GdipDrawImageRect, NULL },
    { "GdipDrawImageRectI", gp_GdipDrawImageRectI, NULL }, { "GdipDrawImagePointRect", gp_GdipDrawImagePointRect, NULL }, { "GdipDrawImagePointRectI", gp_GdipDrawImagePointRectI, NULL },
    { "GdipDrawImageRectRect", gp_GdipDrawImageRectRect, NULL }, { "GdipDrawImageRectRectI", gp_GdipDrawImageRectRectI, NULL }, { "GdipDrawImagePoints", gp_GdipDrawImagePoints, NULL },
    { "GdipDrawImagePointsI", gp_GdipDrawImagePointsI, NULL }, { "GdipDrawImagePointsRect", gp_GdipDrawImagePointsRect, NULL }, { "GdipDrawImagePointsRectI", gp_GdipDrawImagePointsRectI, NULL },
    { "GdipSetSmoothingMode", gp_GdipSetSmoothingMode, NULL }, { "GdipGetSmoothingMode", gp_GdipGetSmoothingMode, NULL }, { "GdipSetPixelOffsetMode", gp_GdipSetPixelOffsetMode, NULL },
    { "GdipGetPixelOffsetMode", gp_GdipGetPixelOffsetMode, NULL }, { "GdipSetInterpolationMode", gp_GdipSetInterpolationMode, NULL }, { "GdipGetInterpolationMode", gp_GdipGetInterpolationMode, NULL },
    { "GdipSetCompositingMode", gp_GdipSetCompositingMode, NULL }, { "GdipGetCompositingMode", gp_GdipGetCompositingMode, NULL }, { "GdipSetCompositingQuality", gp_GdipSetCompositingQuality, NULL },
    { "GdipGetCompositingQuality", gp_GdipGetCompositingQuality, NULL }, { "GdipSetTextRenderingHint", gp_GdipSetTextRenderingHint, NULL }, { "GdipGetTextRenderingHint", gp_GdipGetTextRenderingHint, NULL },
    { "GdipSetTextContrast", gp_GdipSetTextContrast, NULL }, { "GdipGetTextContrast", gp_GdipGetTextContrast, NULL }, { "GdipGetPageUnit", gp_GdipGetPageUnit, NULL },
    { "GdipGetPageScale", gp_GdipGetPageScale, NULL }, { "GdipGetDpiX", gp_GdipGetDpiX, NULL }, { "GdipGetDpiY", gp_GdipGetDpiY, NULL },
    { "GdipSetPenWidth", gp_GdipSetPenWidth, NULL }, { "GdipGetPenWidth", gp_GdipGetPenWidth, NULL }, { "GdipSetPenUnit", gp_GdipSetPenUnit, NULL },
    { "GdipGetPenUnit", gp_GdipGetPenUnit, NULL }, { "GdipSetPenStartCap", gp_GdipSetPenStartCap, NULL }, { "GdipSetPenEndCap", gp_GdipSetPenEndCap, NULL },
    { "GdipGetPenStartCap", gp_GdipGetPenStartCap, NULL }, { "GdipGetPenEndCap", gp_GdipGetPenEndCap, NULL }, { "GdipSetPenDashCap197819", gp_GdipSetPenDashCap197819, NULL },
    { "GdipGetPenDashCap197819", gp_GdipGetPenDashCap197819, NULL }, { "GdipSetPenLineJoin", gp_GdipSetPenLineJoin, NULL }, { "GdipGetPenLineJoin", gp_GdipGetPenLineJoin, NULL },
    { "GdipSetPenMiterLimit", gp_GdipSetPenMiterLimit, NULL }, { "GdipGetPenMiterLimit", gp_GdipGetPenMiterLimit, NULL }, { "GdipSetPenMode", gp_GdipSetPenMode, NULL },
    { "GdipGetPenMode", gp_GdipGetPenMode, NULL }, { "GdipGetPenDashStyle", gp_GdipGetPenDashStyle, NULL }, { "GdipSetPenDashOffset", gp_GdipSetPenDashOffset, NULL },
    { "GdipGetPenDashOffset", gp_GdipGetPenDashOffset, NULL }, { "GdipGetImageHorizontalResolution", gp_GdipGetImageHorizontalResolution, NULL }, { "GdipGetImageVerticalResolution", gp_GdipGetImageVerticalResolution, NULL },
    { "GdipGetFontSize", gp_GdipGetFontSize, NULL }, { "GdipGetFontStyle", gp_GdipGetFontStyle, NULL }, { "GdipGetFontUnit", gp_GdipGetFontUnit, NULL },
    { "GdipSetStringFormatFlags", gp_GdipSetStringFormatFlags, NULL }, { "GdipGetStringFormatFlags", gp_GdipGetStringFormatFlags, NULL }, { "GdipSetStringFormatAlign", gp_GdipSetStringFormatAlign, NULL },
    { "GdipGetStringFormatAlign", gp_GdipGetStringFormatAlign, NULL }, { "GdipSetStringFormatLineAlign", gp_GdipSetStringFormatLineAlign, NULL }, { "GdipGetStringFormatLineAlign", gp_GdipGetStringFormatLineAlign, NULL },
    { "GdipSetStringFormatTrimming", gp_GdipSetStringFormatTrimming, NULL }, { "GdipGetStringFormatTrimming", gp_GdipGetStringFormatTrimming, NULL }, { "GdipSetStringFormatHotkeyPrefix", gp_GdipSetStringFormatHotkeyPrefix, NULL },
    { "GdipGetStringFormatHotkeyPrefix", gp_GdipGetStringFormatHotkeyPrefix, NULL }, { "GdipSetTextureTransform", gp_GdipSetTextureTransform, NULL }, { "GdipGetTextureTransform", gp_GdipGetTextureTransform, NULL },
    { "GdipResetTextureTransform", gp_GdipResetTextureTransform, NULL }, { "GdipMultiplyTextureTransform", gp_GdipMultiplyTextureTransform, NULL }, { "GdipTranslateTextureTransform", gp_GdipTranslateTextureTransform, NULL },
    { "GdipScaleTextureTransform", gp_GdipScaleTextureTransform, NULL }, { "GdipRotateTextureTransform", gp_GdipRotateTextureTransform, NULL }, { "GdipSetLineTransform", gp_GdipSetLineTransform, NULL },
    { "GdipGetLineTransform", gp_GdipGetLineTransform, NULL }, { "GdipResetLineTransform", gp_GdipResetLineTransform, NULL }, { "GdipMultiplyLineTransform", gp_GdipMultiplyLineTransform, NULL },
    { "GdipTranslateLineTransform", gp_GdipTranslateLineTransform, NULL }, { "GdipScaleLineTransform", gp_GdipScaleLineTransform, NULL }, { "GdipRotateLineTransform", gp_GdipRotateLineTransform, NULL },
    { "GdipSetPathGradientTransform", gp_GdipSetPathGradientTransform, NULL }, { "GdipGetPathGradientTransform", gp_GdipGetPathGradientTransform, NULL }, { "GdipResetPathGradientTransform", gp_GdipResetPathGradientTransform, NULL },
    { "GdipMultiplyPathGradientTransform", gp_GdipMultiplyPathGradientTransform, NULL }, { "GdipTranslatePathGradientTransform", gp_GdipTranslatePathGradientTransform, NULL }, { "GdipScalePathGradientTransform", gp_GdipScalePathGradientTransform, NULL },
    { "GdipRotatePathGradientTransform", gp_GdipRotatePathGradientTransform, NULL }, { "GdiplusNotificationHook", gp_NotificationHook, NULL }, { "GdiplusNotificationUnhook", gp_NotificationUnhook, NULL },
    { "CreateStreamOnHGlobal", ole_CreateStreamOnHGlobal, NULL }, { "GetHGlobalFromStream", ole_GetHGlobalFromStream, NULL }, { "SHCreateMemStream", shl_SHCreateMemStream, NULL },
    { "SHCreateStreamOnFileA", shl_SHCreateStreamOnFileA, NULL }, { "SHCreateStreamOnFileW", shl_SHCreateStreamOnFileW, NULL }, { "SHCreateStreamOnFileEx", shl_SHCreateStreamOnFileEx, NULL },
    { NULL, NULL, NULL }
};
