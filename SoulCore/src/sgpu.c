// SoulCore - CPU backend of the sgpu fixed-function pipeline (see sgpu.h).
#include "sgpu.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_pixels;
uint64_t sg_pixels_drawn(void) { return g_pixels; }

#define NATTR (4 + 2 * SG_UNITS + 1)

// screen-space vertex: attributes are pre-multiplied by 1/w for perspective correction
typedef struct Sv { float x, y, z, iw; float a[NATTR]; } Sv;

static void to_attr(const SgVertex *v, float *a) {
    a[0] = v->c[0]; a[1] = v->c[1]; a[2] = v->c[2]; a[3] = v->c[3];
    for (int u = 0; u < SG_UNITS; u++) { a[4 + 2 * u] = v->t[u][0]; a[5 + 2 * u] = v->t[u][1]; }
    a[4 + 2 * SG_UNITS] = v->fog;
}

static int project(const SgTarget *t, const SgState *s, const SgVertex *v, Sv *o) {
    if (v->w <= 1e-7f) return 0;
    float iw = 1.0f / v->w;
    o->x = (float)s->vp_x + (v->x * iw + 1.0f) * 0.5f * (float)s->vp_w;
    float yw = (float)s->vp_y + (v->y * iw + 1.0f) * 0.5f * (float)s->vp_h;
    o->y = (float)t->h - yw;
    float zn = (v->z * iw + 1.0f) * 0.5f;
    o->z = s->depth_near + zn * (s->depth_far - s->depth_near);
    o->iw = iw;
    float a[NATTR];
    to_attr(v, a);
    for (int i = 0; i < NATTR; i++) o->a[i] = a[i] * iw;
    return 1;
}

// ---- clipping (near and far planes in homogeneous space; x/y are clipped by the raster bounds) ----

static void lerp_v(const SgVertex *a, const SgVertex *b, float t, SgVertex *o) {
    const float *pa = &a->x, *pb = &b->x;
    float *po = &o->x;
    for (size_t i = 0; i < sizeof(SgVertex) / sizeof(float); i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

static int clip_plane(const SgVertex *in, int n, SgVertex *out, int sign) { // sign -1: z >= -w (near), +1: z <= w (far)
    int m = 0;
    for (int i = 0; i < n; i++) {
        const SgVertex *a = &in[i], *b = &in[(i + 1) % n];
        float da = sign < 0 ? a->z + a->w : a->w - a->z;
        float db = sign < 0 ? b->z + b->w : b->w - b->z;
        if (da >= 0) out[m++] = *a;
        if ((da >= 0) != (db >= 0)) lerp_v(a, b, da / (da - db), &out[m++]);
    }
    return m;
}

// ---- texturing -----------------------------------------------------------------------------

static inline void texel(const uint8_t *img, int w, int h, int x, int y, int cs, int ct, float *o) {
    if (cs) { if (x < 0) x = 0; else if (x >= w) x = w - 1; }
    else { x %= w; if (x < 0) x += w; }
    if (ct) { if (y < 0) y = 0; else if (y >= h) y = h - 1; }
    else { y %= h; if (y < 0) y += h; }
    const uint8_t *p = img + ((size_t)y * (size_t)w + (size_t)x) * 4;
    o[0] = p[0] * (1.0f / 255); o[1] = p[1] * (1.0f / 255); o[2] = p[2] * (1.0f / 255); o[3] = p[3] * (1.0f / 255);
}

static void sample(const SgTexture *tx, float s, float t, float lod, float *o) {
    const uint8_t *img = tx->rgba;
    int w = tx->w, h = tx->h;
    int linear = lod > 0 ? tx->min_linear : tx->mag_linear;
    if (tx->use_mips && lod > 0 && tx->levels > 1) {
        int l = (int)(lod + 0.5f);
        if (l >= tx->levels) l = tx->levels - 1;
        img = tx->mip[l]; w = tx->mip_w[l]; h = tx->mip_h[l];
    }
    if (!linear) {
        int x = (int)floorf(s * (float)w), y = (int)floorf(t * (float)h);
        texel(img, w, h, x, y, tx->clamp_s, tx->clamp_t, o);
        return;
    }
    float fx = s * (float)w - 0.5f, fy = t * (float)h - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float ax = fx - (float)x0, ay = fy - (float)y0;
    float c00[4], c10[4], c01[4], c11[4];
    texel(img, w, h, x0, y0, tx->clamp_s, tx->clamp_t, c00);
    texel(img, w, h, x0 + 1, y0, tx->clamp_s, tx->clamp_t, c10);
    texel(img, w, h, x0, y0 + 1, tx->clamp_s, tx->clamp_t, c01);
    texel(img, w, h, x0 + 1, y0 + 1, tx->clamp_s, tx->clamp_t, c11);
    for (int i = 0; i < 4; i++) {
        float top = c00[i] + (c10[i] - c00[i]) * ax, bot = c01[i] + (c11[i] - c01[i]) * ax;
        o[i] = top + (bot - top) * ay;
    }
}

static void tex_combine(int env, int fmt, const float *env_color, float *c, const float *tx) {
    float tr = tx[0], tg = tx[1], tb = tx[2], ta = tx[3];
    int has_rgb = fmt != SG_FMT_ALPHA, has_a = fmt == SG_FMT_RGBA || fmt == SG_FMT_ALPHA || fmt == SG_FMT_LUMINANCE_ALPHA || fmt == SG_FMT_INTENSITY;
    switch (env) {
    case SG_ENV_REPLACE:
        if (has_rgb) { c[0] = tr; c[1] = tg; c[2] = tb; }
        if (has_a) c[3] = ta;
        break;
    case SG_ENV_DECAL:
        if (fmt == SG_FMT_RGBA) { c[0] += (tr - c[0]) * ta; c[1] += (tg - c[1]) * ta; c[2] += (tb - c[2]) * ta; }
        else if (fmt == SG_FMT_RGB) { c[0] = tr; c[1] = tg; c[2] = tb; }
        break;
    case SG_ENV_ADD:
        if (has_rgb) { c[0] += tr; c[1] += tg; c[2] += tb; if (c[0] > 1) c[0] = 1; if (c[1] > 1) c[1] = 1; if (c[2] > 1) c[2] = 1; }
        if (has_a) c[3] *= ta;
        break;
    case SG_ENV_BLEND:
        if (has_rgb) {
            c[0] = c[0] * (1 - tr) + env_color[0] * tr;
            c[1] = c[1] * (1 - tg) + env_color[1] * tg;
            c[2] = c[2] * (1 - tb) + env_color[2] * tb;
        }
        if (has_a) c[3] *= ta;
        break;
    default: // MODULATE
        if (has_rgb) { c[0] *= tr; c[1] *= tg; c[2] *= tb; }
        if (has_a) c[3] *= ta;
        break;
    }
}

// ---- per-fragment operations ----------------------------------------------------------------

static inline int depth_pass(int f, float z, float d) {
    switch (f) {
    case SG_NEVER: return 0;
    case SG_LESS: return z < d;
    case SG_EQUAL: return z == d;
    case SG_LEQUAL: return z <= d;
    case SG_GREATER: return z > d;
    case SG_NOTEQUAL: return z != d;
    case SG_GEQUAL: return z >= d;
    default: return 1;
    }
}

static inline float factor(int f, int ch, const float *src, const float *dst) {
    switch (f) {
    case SG_ZERO: return 0;
    case SG_ONE: return 1;
    case SG_SRC_COLOR: return src[ch];
    case SG_ONE_MINUS_SRC_COLOR: return 1 - src[ch];
    case SG_SRC_ALPHA: return src[3];
    case SG_ONE_MINUS_SRC_ALPHA: return 1 - src[3];
    case SG_DST_ALPHA: return dst[3];
    case SG_ONE_MINUS_DST_ALPHA: return 1 - dst[3];
    case SG_DST_COLOR: return dst[ch];
    case SG_ONE_MINUS_DST_COLOR: return 1 - dst[ch];
    case SG_SRC_ALPHA_SATURATE: { float m = 1 - dst[3]; float f2 = src[3] < m ? src[3] : m; return ch == 3 ? 1 : f2; }
    default: return 1;
    }
}

static inline uint8_t to8(float v) { if (v <= 0) return 0; if (v >= 1) return 255; return (uint8_t)(v * 255.0f + 0.5f); }

// Shades and writes one fragment. a[] holds interpolated (not pre-multiplied) attributes.
static void fragment(SgTarget *t, const SgState *s, int x, int y, float z, const float *a, float lod) {
    size_t idx = (size_t)y * (size_t)t->w + (size_t)x;
    if (s->depth_test && !s->alpha_test && !depth_pass(s->depth_func, z, t->depth[idx])) return;
    float c[4] = { a[0], a[1], a[2], a[3] };
    for (int u = 0; u < SG_UNITS; u++) {
        if (!s->tex_on[u] || !s->tex[u] || !s->tex[u]->rgba) continue;
        float tx[4];
        sample(s->tex[u], a[4 + 2 * u], a[5 + 2 * u], lod, tx);
        tex_combine(s->tex_env[u], s->tex[u]->format, s->env_color[u], c, tx);
    }
    if (s->fog) {
        float f = a[4 + 2 * SG_UNITS];
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        for (int i = 0; i < 3; i++) c[i] = c[i] * f + s->fog_color[i] * (1 - f);
    }
    if (s->alpha_test) {
        if (!depth_pass(s->alpha_func, c[3], s->alpha_ref)) return;
        if (s->depth_test && !depth_pass(s->depth_func, z, t->depth[idx])) return;
    }
    if (s->depth_test && s->depth_write) t->depth[idx] = z;
    uint32_t d = t->color[idx];
    float out[4];
    if (s->blend) {
        float dst[4] = { ((d >> 16) & 255) / 255.0f, ((d >> 8) & 255) / 255.0f, (d & 255) / 255.0f, (d >> 24) / 255.0f };
        for (int i = 0; i < 4; i++) {
            float v = c[i] * factor(s->blend_src, i, c, dst) + dst[i] * factor(s->blend_dst, i, c, dst);
            out[i] = v;
        }
    } else {
        out[0] = c[0]; out[1] = c[1]; out[2] = c[2]; out[3] = c[3];
    }
    uint32_t r = s->mask_r ? to8(out[0]) : (d >> 16) & 255;
    uint32_t g = s->mask_g ? to8(out[1]) : (d >> 8) & 255;
    uint32_t b = s->mask_b ? to8(out[2]) : d & 255;
    uint32_t al = s->mask_a ? to8(out[3]) : d >> 24;
    t->color[idx] = (al << 24) | (r << 16) | (g << 8) | b;
    g_pixels++;
}

typedef struct Clip { int x0, y0, x1, y1; } Clip;

static Clip clip_rect(const SgTarget *t, const SgState *s) {
    Clip c = { s->vp_x, t->h - (s->vp_y + s->vp_h), s->vp_x + s->vp_w, t->h - s->vp_y };
    if (s->scissor) {
        int sx0 = s->sc_x, sy0 = t->h - (s->sc_y + s->sc_h), sx1 = s->sc_x + s->sc_w, sy1 = t->h - s->sc_y;
        if (sx0 > c.x0) c.x0 = sx0;
        if (sy0 > c.y0) c.y0 = sy0;
        if (sx1 < c.x1) c.x1 = sx1;
        if (sy1 < c.y1) c.y1 = sy1;
    }
    if (c.x0 < 0) c.x0 = 0;
    if (c.y0 < 0) c.y0 = 0;
    if (c.x1 > t->w) c.x1 = t->w;
    if (c.y1 > t->h) c.y1 = t->h;
    return c;
}

// ---- triangles --------------------------------------------------------------------------------

static float tex_lod(const SgState *s, const Sv *v0, const Sv *v1, const Sv *v2) {
    // one level of detail per triangle: texel/pixel area ratio of the first enabled unit
    for (int u = 0; u < SG_UNITS; u++) {
        if (!s->tex_on[u] || !s->tex[u] || !s->tex[u]->use_mips) continue;
        float sa = 0;
        const Sv *v[3] = { v0, v1, v2 };
        float st[3][2];
        for (int i = 0; i < 3; i++) { st[i][0] = v[i]->a[4 + 2 * u] / v[i]->iw * (float)s->tex[u]->w; st[i][1] = v[i]->a[5 + 2 * u] / v[i]->iw * (float)s->tex[u]->h; }
        sa = fabsf((st[1][0] - st[0][0]) * (st[2][1] - st[0][1]) - (st[2][0] - st[0][0]) * (st[1][1] - st[0][1]));
        float pa = fabsf((v1->x - v0->x) * (v2->y - v0->y) - (v2->x - v0->x) * (v1->y - v0->y));
        if (pa < 1e-6f) return 0;
        float r = sa / pa;
        return r > 1 ? 0.5f * log2f(r) : 0;
    }
    return 0;
}

static void raster_tri(SgTarget *t, const SgState *s, const Sv *v0, const Sv *v1, const Sv *v2, const float *flat) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v1->y - v0->y) * (v2->x - v0->x);
    if (area == 0) return;
    // screen y grows downwards: a positive area here is clockwise on screen = counter-clockwise in GL
    int ccw = area < 0;
    if (s->cull) {
        int front = ccw == (s->front_ccw != 0);
        if ((front && s->cull_front) || (!front && s->cull_back)) return;
    }
    if (area < 0) { const Sv *tmp = v1; v1 = v2; v2 = tmp; area = -area; }
    Clip c = clip_rect(t, s);
    int x0 = (int)floorf(fminf(v0->x, fminf(v1->x, v2->x))), x1 = (int)ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    int y0 = (int)floorf(fminf(v0->y, fminf(v1->y, v2->y))), y1 = (int)ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    if (x0 < c.x0) x0 = c.x0;
    if (y0 < c.y0) y0 = c.y0;
    if (x1 > c.x1) x1 = c.x1;
    if (y1 > c.y1) y1 = c.y1;
    if (x0 >= x1 || y0 >= y1) return;
    float inv_area = 1.0f / area;
    // edge functions e_i(p) = A_i * px + B_i * py + C_i (positive inside)
    const Sv *v[3] = { v0, v1, v2 };
    float A[3], B[3], C[3];
    int tl[3];
    for (int i = 0; i < 3; i++) {
        const Sv *a = v[(i + 1) % 3], *b = v[(i + 2) % 3];
        A[i] = a->y - b->y; B[i] = b->x - a->x; C[i] = a->x * b->y - a->y * b->x;
        tl[i] = (A[i] > 0) || (A[i] == 0 && B[i] < 0); // top-left rule
    }
    float lod = tex_lod(s, v0, v1, v2);
    float zbias = s->polygon_offset;
    for (int y = y0; y < y1; y++) {
        float py = (float)y + 0.5f;
        float px = (float)x0 + 0.5f;
        float e[3];
        for (int i = 0; i < 3; i++) e[i] = A[i] * px + B[i] * py + C[i];
        for (int x = x0; x < x1; x++, e[0] += A[0], e[1] += A[1], e[2] += A[2]) {
            if (e[0] < 0 || e[1] < 0 || e[2] < 0) continue;
            if ((e[0] == 0 && !tl[0]) || (e[1] == 0 && !tl[1]) || (e[2] == 0 && !tl[2])) continue;
            float l0 = e[0] * inv_area, l1 = e[1] * inv_area, l2 = e[2] * inv_area;
            float z = l0 * v0->z + l1 * v1->z + l2 * v2->z + zbias;
            if (s->depth_test && !s->alpha_test && !depth_pass(s->depth_func, z, t->depth[(size_t)y * (size_t)t->w + (size_t)x])) continue;
            float iw = l0 * v0->iw + l1 * v1->iw + l2 * v2->iw;
            float rw = 1.0f / iw;
            float a[NATTR];
            for (int k = 0; k < NATTR; k++) a[k] = (l0 * v0->a[k] + l1 * v1->a[k] + l2 * v2->a[k]) * rw;
            if (flat) { a[0] = flat[0]; a[1] = flat[1]; a[2] = flat[2]; a[3] = flat[3]; }
            fragment(t, s, x, y, z, a, lod);
        }
    }
}

void sg_triangles(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    for (int i = 0; i + 2 < n; i += 3) {
        const SgVertex *a = &v[i], *b = &v[i + 1], *c = &v[i + 2];
        float flat[4];
        if (s->flat) memcpy(flat, c->c, sizeof flat);
        SgVertex poly[12], tmp[12];
        int m = 3;
        int need = (a->z < -a->w || b->z < -b->w || c->z < -c->w || a->z > a->w || b->z > b->w || c->z > c->w);
        if (!need) {
            Sv p0, p1, p2;
            if (project(t, s, a, &p0) && project(t, s, b, &p1) && project(t, s, c, &p2)) raster_tri(t, s, &p0, &p1, &p2, s->flat ? flat : NULL);
            continue;
        }
        poly[0] = *a; poly[1] = *b; poly[2] = *c;
        m = clip_plane(poly, m, tmp, -1);
        if (m < 3) continue;
        m = clip_plane(tmp, m, poly, +1);
        if (m < 3) continue;
        Sv p[12];
        int ok = 1;
        for (int k = 0; k < m; k++) if (!project(t, s, &poly[k], &p[k])) ok = 0;
        if (!ok) continue;
        for (int k = 1; k + 1 < m; k++) raster_tri(t, s, &p[0], &p[k], &p[k + 1], s->flat ? flat : NULL);
    }
}

// ---- lines and points -----------------------------------------------------------------------

static void raster_line(SgTarget *t, const SgState *s, const Sv *a, const Sv *b, const float *flat) {
    Clip c = clip_rect(t, s);
    float dx = b->x - a->x, dy = b->y - a->y;
    int steps = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));
    if (steps < 1) steps = 1;
    int wdt = s->line_width > 1.5f ? (int)(s->line_width + 0.5f) : 1;
    int horiz = fabsf(dx) >= fabsf(dy);
    for (int i = 0; i <= steps; i++) {
        float f = (float)i / (float)steps;
        float x = a->x + dx * f, y = a->y + dy * f;
        float iw = a->iw + (b->iw - a->iw) * f;
        float z = a->z + (b->z - a->z) * f;
        float at[NATTR];
        for (int k = 0; k < NATTR; k++) at[k] = (a->a[k] + (b->a[k] - a->a[k]) * f) / iw;
        if (flat) memcpy(at, flat, 4 * sizeof(float));
        for (int k = 0; k < wdt; k++) {
            int px = (int)floorf(x) + (horiz ? 0 : k - wdt / 2), py = (int)floorf(y) + (horiz ? k - wdt / 2 : 0);
            if (px < c.x0 || py < c.y0 || px >= c.x1 || py >= c.y1) continue;
            fragment(t, s, px, py, z, at, 0);
        }
    }
}

void sg_lines(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    for (int i = 0; i + 1 < n; i += 2) {
        SgVertex a = v[i], b = v[i + 1];
        // clip against the near plane
        float da = a.z + a.w, db = b.z + b.w;
        if (da < 0 && db < 0) continue;
        if (da < 0) lerp_v(&a, &b, da / (da - db), &a);
        else if (db < 0) lerp_v(&a, &b, da / (da - db), &b);
        Sv p, q;
        if (!project(t, s, &a, &p) || !project(t, s, &b, &q)) continue;
        raster_line(t, s, &p, &q, s->flat ? v[i + 1].c : NULL);
    }
}

void sg_points(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    Clip c = clip_rect(t, s);
    int size = s->point_size > 1.5f ? (int)(s->point_size + 0.5f) : 1;
    for (int i = 0; i < n; i++) {
        if (v[i].z < -v[i].w || v[i].z > v[i].w) continue;
        Sv p;
        if (!project(t, s, &v[i], &p)) continue;
        float at[NATTR];
        for (int k = 0; k < NATTR; k++) at[k] = p.a[k] / p.iw;
        int x0 = (int)floorf(p.x - size / 2.0f + 0.5f), y0 = (int)floorf(p.y - size / 2.0f + 0.5f);
        for (int y = y0; y < y0 + size; y++) for (int x = x0; x < x0 + size; x++)
            if (x >= c.x0 && y >= c.y0 && x < c.x1 && y < c.y1) fragment(t, s, x, y, p.z, at, 0);
    }
}

void sg_clear(SgTarget *t, const SgState *s, int color, uint32_t argb, int depth, float z) {
    int x0 = 0, y0 = 0, x1 = t->w, y1 = t->h;
    if (s->scissor) {
        x0 = s->sc_x; x1 = s->sc_x + s->sc_w; y0 = t->h - (s->sc_y + s->sc_h); y1 = t->h - s->sc_y;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > t->w) x1 = t->w;
        if (y1 > t->h) y1 = t->h;
    }
    uint32_t keep = (s->mask_r ? 0 : 0x00FF0000u) | (s->mask_g ? 0 : 0x0000FF00u) | (s->mask_b ? 0 : 0x000000FFu) | (s->mask_a ? 0 : 0xFF000000u);
    for (int y = y0; y < y1; y++) {
        uint32_t *row = t->color + (size_t)y * (size_t)t->w;
        float *drow = t->depth + (size_t)y * (size_t)t->w;
        if (color) {
            if (!keep) for (int x = x0; x < x1; x++) row[x] = argb;
            else for (int x = x0; x < x1; x++) row[x] = (row[x] & keep) | (argb & ~keep);
        }
        if (depth && s->depth_write) for (int x = x0; x < x1; x++) drow[x] = z;
    }
}

void sg_texture_build_mips(SgTexture *tx) {
    for (int i = 1; i < tx->levels; i++) free(tx->mip[i]);
    tx->levels = 1;
    tx->mip[0] = tx->rgba; tx->mip_w[0] = tx->w; tx->mip_h[0] = tx->h;
    int w = tx->w, h = tx->h;
    while ((w > 1 || h > 1) && tx->levels < 14) {
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        const uint8_t *src = tx->mip[tx->levels - 1];
        uint8_t *dst = malloc((size_t)nw * (size_t)nh * 4);
        for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int k = 0; k < 4; k++) {
            int sx0 = x * 2, sy0 = y * 2, sx1 = sx0 + 1 < w ? sx0 + 1 : sx0, sy1 = sy0 + 1 < h ? sy0 + 1 : sy0;
            int sum = src[((size_t)sy0 * w + sx0) * 4 + k] + src[((size_t)sy0 * w + sx1) * 4 + k] + src[((size_t)sy1 * w + sx0) * 4 + k] + src[((size_t)sy1 * w + sx1) * 4 + k];
            dst[((size_t)y * nw + x) * 4 + k] = (uint8_t)((sum + 2) / 4);
        }
        tx->mip[tx->levels] = dst; tx->mip_w[tx->levels] = nw; tx->mip_h[tx->levels] = nh;
        tx->levels++;
        w = nw; h = nh;
    }
}

void sg_texture_free(SgTexture *tx) {
    for (int i = 1; i < tx->levels; i++) free(tx->mip[i]);
    free(tx->rgba);
    memset(tx, 0, sizeof *tx);
}
