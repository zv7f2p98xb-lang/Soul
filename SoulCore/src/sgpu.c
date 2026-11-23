// SoulCore - CPU backend of the sgpu 3D pipeline (see sgpu.h).
#include "sgpu.h"
#include "sgpu_program.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_pixels;
uint64_t sg_pixels_drawn(void) { return g_pixels; }

#define NATTR (4 + 2 * SG_UNITS + 1)
#define MAXPOLY 20
#define MAXPLANES (2 + 4 + SG_MAX_CLIP_PLANES)

// screen-space vertex: attributes and varyings are pre-multiplied by 1/w for perspective correction
typedef struct Sv { float x, y, z, iw; float a[NATTR]; float vy[SG_VARYINGS][4]; } Sv;

static inline int nvary_of(const SgState *s) { return s->nvary < 0 ? 0 : s->nvary > SG_VARYINGS ? SG_VARYINGS : s->nvary; }

static void to_attr(const SgVertex *v, float *a) {
    a[0] = v->c[0]; a[1] = v->c[1]; a[2] = v->c[2]; a[3] = v->c[3];
    for (int u = 0; u < SG_UNITS; u++) { a[4 + 2 * u] = v->t[u][0]; a[5 + 2 * u] = v->t[u][1]; }
    a[4 + 2 * SG_UNITS] = v->fog;
}

static int project(const SgTarget *t, const SgState *s, const SgVertex *v, Sv *o) {
    if (!(v->w > 1e-7f)) return 0; // also rejects NaN
    float iw = 1.0f / v->w;
    float nx = v->x * iw, ny = v->y * iw, nz = v->z * iw;
    if (!isfinite(nx) || !isfinite(ny) || !isfinite(nz)) return 0;
    o->x = (float)s->vp_x + (nx + 1.0f) * 0.5f * (float)s->vp_w;
    if (s->vp_top_left) o->y = (float)s->vp_y + (1.0f - ny) * 0.5f * (float)s->vp_h;
    else {
        float yw = (float)s->vp_y + (ny + 1.0f) * 0.5f * (float)s->vp_h;
        o->y = (float)t->h - yw;
    }
    float zn = s->clip_z_zero ? nz : (nz + 1.0f) * 0.5f;
    o->z = s->depth_near + zn * (s->depth_far - s->depth_near);
    o->iw = iw;
    float a[NATTR];
    to_attr(v, a);
    for (int i = 0; i < NATTR; i++) o->a[i] = a[i] * iw;
    int nv = nvary_of(s);
    for (int k = 0; k < nv; k++) for (int c = 0; c < 4; c++) o->vy[k][c] = v->v[k][c] * iw;
    return 1;
}

static inline float zclamp(const SgState *s, float z) {
    float lo = s->depth_near < s->depth_far ? s->depth_near : s->depth_far;
    float hi = s->depth_near < s->depth_far ? s->depth_far : s->depth_near;
    return z < lo ? lo : z > hi ? hi : z;
}

// ---- clipping (homogeneous space: near/far, guard band, user planes) ------------------------

typedef struct Plane { float a, b, c, d, k; } Plane; // distance = a*x + b*y + c*z + d*w - k, inside when >= 0

static inline float plane_dist(const Plane *p, const SgVertex *v) { return p->a * v->x + p->b * v->y + p->c * v->z + p->d * v->w - p->k; }

static int build_planes(const SgTarget *t, const SgState *s, Plane *pl, int guard) {
    int n = 0;
    if (s->depth_clamp) pl[n++] = (Plane){ 0, 0, 0, 1, 1e-5f };
    else {
        pl[n++] = s->clip_z_zero ? (Plane){ 0, 0, 1, 0, 0 } : (Plane){ 0, 0, 1, 1, 0 };
        pl[n++] = (Plane){ 0, 0, -1, 1, 0 };
    }
    if (guard) { // keep screen coordinates within +-8192 pixels of the viewport centre so the rasteriser stays exact
        float gx = 16384.0f / (float)(s->vp_w > 0 ? s->vp_w : 1), gy = 16384.0f / (float)(s->vp_h > 0 ? s->vp_h : 1);
        if (gx < 1) gx = 1;
        if (gy < 1) gy = 1;
        pl[n++] = (Plane){ 1, 0, 0, gx, 0 }; pl[n++] = (Plane){ -1, 0, 0, gx, 0 };
        pl[n++] = (Plane){ 0, 1, 0, gy, 0 }; pl[n++] = (Plane){ 0, -1, 0, gy, 0 };
    }
    (void)t;
    for (int i = 0; i < SG_MAX_CLIP_PLANES; i++)
        if (s->clip_mask & (1 << i)) pl[n++] = (Plane){ s->clip_plane[i][0], s->clip_plane[i][1], s->clip_plane[i][2], s->clip_plane[i][3], 0 };
    return n;
}

static void lerp_v(const SgVertex *a, const SgVertex *b, float t, int nv, SgVertex *o) {
    const float *pa = &a->x, *pb = &b->x;
    float *po = &o->x;
    size_t nfix = offsetof(SgVertex, psize) / sizeof(float);
    for (size_t i = 0; i < nfix; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
    for (int k = 0; k < nv; k++) for (int c = 0; c < 4; c++) o->v[k][c] = a->v[k][c] + (b->v[k][c] - a->v[k][c]) * t;
}

// Clips a convex polygon against all planes. Returns the vertex count and the buffer holding the result in *res.
static int clip_poly(SgVertex *a, SgVertex *b, int n, const Plane *pl, int np, int nv, SgVertex **res) {
    SgVertex *in = a, *out = b;
    for (int p = 0; p < np; p++) {
        float d[MAXPOLY];
        int neg = 0, pos = 0;
        for (int i = 0; i < n; i++) { d[i] = plane_dist(&pl[p], &in[i]); if (d[i] >= 0) pos++; else neg++; }
        if (!neg) continue;
        if (!pos) return 0;
        int m = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            if (m + 2 > MAXPOLY) break;
            if (d[i] >= 0) out[m++] = in[i];
            if ((d[i] >= 0) != (d[j] >= 0)) lerp_v(&in[i], &in[j], d[i] / (d[i] - d[j]), nv, &out[m++]);
        }
        n = m;
        if (n < 3) return 0;
        SgVertex *tmp = in; in = out; out = tmp;
    }
    *res = in;
    return n;
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

static inline float factor(int f, int ch, const float *src, const float *dst, const float *bc) {
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
    case SG_CONSTANT_COLOR: return bc[ch];
    case SG_ONE_MINUS_CONSTANT_COLOR: return 1 - bc[ch];
    case SG_CONSTANT_ALPHA: return bc[3];
    case SG_ONE_MINUS_CONSTANT_ALPHA: return 1 - bc[3];
    default: return 1;
    }
}

static inline uint8_t to8(float v) { if (!(v > 0)) return 0; if (v >= 1) return 255; return (uint8_t)(v * 255.0f + 0.5f); }

static inline int stencil_apply(int op, int cur, int ref) {
    switch (op) {
    case SG_SOP_ZERO: return 0;
    case SG_SOP_REPLACE: return ref;
    case SG_SOP_INCR_SAT: return cur < 255 ? cur + 1 : 255;
    case SG_SOP_DECR_SAT: return cur > 0 ? cur - 1 : 0;
    case SG_SOP_INVERT: return ~cur & 255;
    case SG_SOP_INCR: return (cur + 1) & 255;
    case SG_SOP_DECR: return (cur - 1) & 255;
    default: return cur;
    }
}

static inline int early_depth_ok(const SgState *s) { return !s->alpha_test && !s->stencil_test && !(s->pixel_fn && s->pixel_writes_depth); }

// Final per-fragment stages for an already shaded colour c: alpha test, stencil, depth, blend, mask, write.
static void frag_ops(SgTarget *t, const SgState *s, int x, int y, float z, const float *c, int front) {
    size_t idx = (size_t)y * (size_t)t->w + (size_t)x;
    if (s->alpha_test && !depth_pass(s->alpha_func, c[3], s->alpha_ref)) return;
    const SgStencilFace *sf = NULL;
    int scur = 0;
    if (s->stencil_test && t->stencil) {
        sf = &s->stencil[(s->stencil_two_sided && !front) ? 1 : 0];
        scur = t->stencil[idx];
        if (!depth_pass(sf->func, sf->ref & sf->read_mask & 255, scur & sf->read_mask & 255)) {
            t->stencil[idx] = (uint8_t)((scur & ~sf->write_mask) | (stencil_apply(sf->fail_op, scur, sf->ref & 255) & sf->write_mask));
            return;
        }
    }
    if (s->depth_test && t->depth && !depth_pass(s->depth_func, z, t->depth[idx])) {
        if (sf) t->stencil[idx] = (uint8_t)((scur & ~sf->write_mask) | (stencil_apply(sf->zfail_op, scur, sf->ref & 255) & sf->write_mask));
        return;
    }
    if (sf) t->stencil[idx] = (uint8_t)((scur & ~sf->write_mask) | (stencil_apply(sf->zpass_op, scur, sf->ref & 255) & sf->write_mask));
    if (s->depth_test && s->depth_write && t->depth) t->depth[idx] = z;
    uint32_t d = t->color[idx];
    float out[4];
    if (s->blend) {
        float dst[4] = { ((d >> 16) & 255) / 255.0f, ((d >> 8) & 255) / 255.0f, (d & 255) / 255.0f, (d >> 24) / 255.0f };
        int sa = s->blend_sep_alpha;
        for (int i = 0; i < 4; i++) {
            int a = i == 3 && sa;
            int op = a ? s->blend_op_a : s->blend_op;
            float sv = c[i], dv = dst[i], v;
            if (op == SG_BLEND_MIN) v = sv < dv ? sv : dv;
            else if (op == SG_BLEND_MAX) v = sv > dv ? sv : dv;
            else {
                float fs = factor(a ? s->blend_src_a : s->blend_src, i, c, dst, s->blend_color);
                float fd = factor(a ? s->blend_dst_a : s->blend_dst, i, c, dst, s->blend_color);
                v = op == SG_BLEND_SUBTRACT ? sv * fs - dv * fd : op == SG_BLEND_REV_SUBTRACT ? dv * fd - sv * fs : sv * fs + dv * fd;
            }
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

// Shades with the fixed-function texture/fog path and writes one fragment. a[] holds interpolated attributes.
static void fragment(SgTarget *t, const SgState *s, int x, int y, float z, const float *a, float lod, int front) {
    size_t idx = (size_t)y * (size_t)t->w + (size_t)x;
    if (s->depth_test && t->depth && early_depth_ok(s) && !depth_pass(s->depth_func, z, t->depth[idx])) return;
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
    frag_ops(t, s, x, y, z, c, front);
}

// Programmable path: `in` has v/dx/dy filled by the caller.
static void prog_fragment(SgTarget *t, const SgState *s, SgPixelIn *in, int x, int y, float z, float iw, int front) {
    size_t idx = (size_t)y * (size_t)t->w + (size_t)x;
    if (s->depth_test && t->depth && early_depth_ok(s) && !depth_pass(s->depth_func, z, t->depth[idx])) return;
    in->pos[0] = (float)x + 0.5f; in->pos[1] = (float)y + 0.5f; in->pos[2] = z; in->pos[3] = iw;
    in->front_facing = front;
    float col[4] = { 0, 0, 0, 0 }, zd = z;
    if (!s->pixel_fn(s->pixel_ctx, in, col, &zd)) return;
    for (int i = 0; i < 4; i++) col[i] = col[i] > 0 ? (col[i] < 1 ? col[i] : 1) : 0;
    if (s->depth_clamp) zd = zclamp(s, zd);
    frag_ops(t, s, x, y, zd, col, front);
}

typedef struct Clip { int x0, y0, x1, y1; } Clip;

static Clip clip_rect(const SgTarget *t, const SgState *s) {
    Clip c;
    if (s->vp_top_left) c = (Clip){ s->vp_x, s->vp_y, s->vp_x + s->vp_w, s->vp_y + s->vp_h };
    else c = (Clip){ s->vp_x, t->h - (s->vp_y + s->vp_h), s->vp_x + s->vp_w, t->h - s->vp_y };
    if (s->scissor) {
        int sx0 = s->sc_x, sy0 = t->h - (s->sc_y + s->sc_h), sx1 = s->sc_x + s->sc_w, sy1 = t->h - s->sc_y;
        if (s->vp_top_left) { sy0 = s->sc_y; sy1 = s->sc_y + s->sc_h; }
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

static void raster_tri(SgTarget *t, const SgState *s, const Sv *v0, const Sv *v1, const Sv *v2, const float *flat, const float (*flatv)[4]) {
    float area = (v1->x - v0->x) * (v2->y - v0->y) - (v1->y - v0->y) * (v2->x - v0->x);
    if (area == 0 || area != area) return;
    // screen y grows downwards: a positive area here is clockwise on screen = counter-clockwise in GL
    int ccw = area < 0;
    int front = ccw == (s->front_ccw != 0);
    if (s->cull) {
        if ((front && s->cull_front) || (!front && s->cull_back)) return;
    }
    if (area < 0) { const Sv *tmp = v1; v1 = v2; v2 = tmp; area = -area; }
    Clip c = clip_rect(t, s);
    float fx0 = floorf(fminf(v0->x, fminf(v1->x, v2->x))), fx1 = ceilf(fmaxf(v0->x, fmaxf(v1->x, v2->x)));
    float fy0 = floorf(fminf(v0->y, fminf(v1->y, v2->y))), fy1 = ceilf(fmaxf(v0->y, fmaxf(v1->y, v2->y)));
    if (fx0 < (float)c.x0) fx0 = (float)c.x0;
    if (fy0 < (float)c.y0) fy0 = (float)c.y0;
    if (fx1 > (float)c.x1) fx1 = (float)c.x1;
    if (fy1 > (float)c.y1) fy1 = (float)c.y1;
    if (!(fx0 < fx1) || !(fy0 < fy1)) return;
    int x0 = (int)fx0, x1 = (int)fx1, y0 = (int)fy0, y1 = (int)fy1;
    float inv_area = 1.0f / area;
    // edge functions e_i(p) = A_i * px + B_i * py + C_i (positive inside)
    const Sv *v[3] = { v0, v1, v2 };
    float A[3], B[3], C[3];
    int tl[3];
    for (int i = 0; i < 3; i++) {
        const Sv *a = v[(i + 1) % 3], *b = v[(i + 2) % 3];
        A[i] = a->y - b->y; B[i] = b->x - a->x; C[i] = a->x * b->y - a->y * b->x;
        // screen y grows downwards; B > 0 on a horizontal edge means the interior lies below it (a top edge).
        // Direct3D (top-left origin) includes top and left edges; the GL path keeps its historical bottom-left tie.
        tl[i] = s->vp_top_left ? ((A[i] > 0) || (A[i] == 0 && B[i] > 0)) : ((A[i] > 0) || (A[i] == 0 && B[i] < 0));
    }
    float lod = tex_lod(s, v0, v1, v2);
    float zbias = s->polygon_offset + s->depth_bias;
    if (s->slope_scale_bias != 0) {
        float dzx = (A[0] * v0->z + A[1] * v1->z + A[2] * v2->z) * inv_area;
        float dzy = (B[0] * v0->z + B[1] * v1->z + B[2] * v2->z) * inv_area;
        float sl = fmaxf(fabsf(dzx), fabsf(dzy));
        if (isfinite(sl)) zbias += s->slope_scale_bias * sl;
    }
    int pfn = s->pixel_fn != NULL;
    int nv = pfn ? nvary_of(s) : 0;
    SgPixelIn in;
    float gx[SG_VARYINGS][4], gy[SG_VARYINGS][4], Dx = 0, Dy = 0;
    if (pfn) {
        memset(&in, 0, sizeof in);
        // plane gradients of the pre-multiplied varyings and of 1/w (exact derivative of the perspective-correct value)
        for (int k = 0; k < nv; k++) for (int q = 0; q < 4; q++) {
            gx[k][q] = (A[0] * v0->vy[k][q] + A[1] * v1->vy[k][q] + A[2] * v2->vy[k][q]) * inv_area;
            gy[k][q] = (B[0] * v0->vy[k][q] + B[1] * v1->vy[k][q] + B[2] * v2->vy[k][q]) * inv_area;
        }
        Dx = (A[0] * v0->iw + A[1] * v1->iw + A[2] * v2->iw) * inv_area;
        Dy = (B[0] * v0->iw + B[1] * v1->iw + B[2] * v2->iw) * inv_area;
    }
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
            if (s->depth_clamp) z = zclamp(s, z);
            if (s->depth_test && t->depth && early_depth_ok(s) && !depth_pass(s->depth_func, z, t->depth[(size_t)y * (size_t)t->w + (size_t)x])) continue;
            float iw = l0 * v0->iw + l1 * v1->iw + l2 * v2->iw;
            float rw = 1.0f / iw;
            if (pfn) {
                for (int k = 0; k < nv; k++) {
                    int fl = (s->flat_mask >> k) & 1 && flatv;
                    for (int q = 0; q < 4; q++) {
                        if (fl) { in.v[k][q] = flatv[k][q]; in.dx[k][q] = in.dy[k][q] = 0; continue; }
                        float val = (l0 * v0->vy[k][q] + l1 * v1->vy[k][q] + l2 * v2->vy[k][q]) * rw;
                        in.v[k][q] = val;
                        in.dx[k][q] = (gx[k][q] - val * Dx) * rw;
                        in.dy[k][q] = (gy[k][q] - val * Dy) * rw;
                    }
                }
                prog_fragment(t, s, &in, x, y, z, iw, front);
                continue;
            }
            float a[NATTR];
            for (int k = 0; k < NATTR; k++) a[k] = (l0 * v0->a[k] + l1 * v1->a[k] + l2 * v2->a[k]) * rw;
            if (flat) { a[0] = flat[0]; a[1] = flat[1]; a[2] = flat[2]; a[3] = flat[3]; }
            fragment(t, s, x, y, z, a, lod, front);
        }
    }
}

void sg_cpu_triangles(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    if (!t->color) return;
    Clip cr = clip_rect(t, s);
    if (cr.x0 >= cr.x1 || cr.y0 >= cr.y1) return;
    Plane pl[MAXPLANES];
    int np = build_planes(t, s, pl, 1);
    int nv = nvary_of(s);
    for (int i = 0; i + 2 < n; i += 3) {
        const SgVertex *a = &v[i], *b = &v[i + 1], *c = &v[i + 2];
        const SgVertex *pv = s->flat_first ? a : c;
        float flat[4];
        if (s->flat) memcpy(flat, pv->c, sizeof flat);
        const float *fl = s->flat ? flat : NULL;
        const float (*flv)[4] = s->flat_mask ? (const float (*)[4])pv->v : NULL;
        int need = 0;
        for (int p = 0; p < np && !need; p++)
            if (plane_dist(&pl[p], a) < 0 || plane_dist(&pl[p], b) < 0 || plane_dist(&pl[p], c) < 0) need = 1;
        if (!need) {
            Sv p0, p1, p2;
            if (project(t, s, a, &p0) && project(t, s, b, &p1) && project(t, s, c, &p2)) raster_tri(t, s, &p0, &p1, &p2, fl, flv);
            continue;
        }
        SgVertex buf1[MAXPOLY], buf2[MAXPOLY], *res;
        buf1[0] = *a; buf1[1] = *b; buf1[2] = *c;
        int m = clip_poly(buf1, buf2, 3, pl, np, nv, &res);
        if (m < 3) continue;
        Sv p[MAXPOLY];
        int ok = 1;
        for (int k = 0; k < m; k++) if (!project(t, s, &res[k], &p[k])) ok = 0;
        if (!ok) continue;
        for (int k = 1; k + 1 < m; k++) raster_tri(t, s, &p[0], &p[k], &p[k + 1], fl, flv);
    }
}

// ---- lines and points -----------------------------------------------------------------------

static void raster_line(SgTarget *t, const SgState *s, const Sv *a, const Sv *b, const float *flat, const float (*flatv)[4]) {
    Clip c = clip_rect(t, s);
    float dx = b->x - a->x, dy = b->y - a->y;
    float fs = ceilf(fmaxf(fabsf(dx), fabsf(dy)));
    if (!(fs < 100000.0f)) return;
    int steps = (int)fs;
    if (steps < 1) steps = 1;
    int wdt = s->line_width > 1.5f ? (int)(s->line_width + 0.5f) : 1;
    if (wdt > 256) wdt = 256;
    int horiz = fabsf(dx) >= fabsf(dy);
    int pfn = s->pixel_fn != NULL, nv = pfn ? nvary_of(s) : 0;
    SgPixelIn in;
    if (pfn) memset(&in, 0, sizeof in);
    for (int i = 0; i <= steps; i++) {
        float f = (float)i / (float)steps;
        float x = a->x + dx * f, y = a->y + dy * f;
        float iw = a->iw + (b->iw - a->iw) * f;
        float z = a->z + (b->z - a->z) * f;
        if (s->depth_clamp) z = zclamp(s, z);
        float at[NATTR];
        if (pfn) {
            for (int k = 0; k < nv; k++) for (int q = 0; q < 4; q++)
                in.v[k][q] = ((s->flat_mask >> k) & 1 && flatv) ? flatv[k][q] : (a->vy[k][q] + (b->vy[k][q] - a->vy[k][q]) * f) / iw;
        } else {
            for (int k = 0; k < NATTR; k++) at[k] = (a->a[k] + (b->a[k] - a->a[k]) * f) / iw;
            if (flat) memcpy(at, flat, 4 * sizeof(float));
        }
        for (int k = 0; k < wdt; k++) {
            int px = (int)floorf(x) + (horiz ? 0 : k - wdt / 2), py = (int)floorf(y) + (horiz ? k - wdt / 2 : 0);
            if (px < c.x0 || py < c.y0 || px >= c.x1 || py >= c.y1) continue;
            if (pfn) prog_fragment(t, s, &in, px, py, z, iw, 1);
            else fragment(t, s, px, py, z, at, 0, 1);
        }
    }
}

void sg_cpu_lines(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    if (!t->color) return;
    Plane pl[MAXPLANES];
    int np = build_planes(t, s, pl, 1);
    int nv = nvary_of(s);
    for (int i = 0; i + 1 < n; i += 2) {
        SgVertex a = v[i], b = v[i + 1];
        const SgVertex *pv = s->flat_first ? &v[i] : &v[i + 1];
        int vis = 1;
        for (int p = 0; p < np && vis; p++) {
            float da = plane_dist(&pl[p], &a), db = plane_dist(&pl[p], &b);
            if (da < 0 && db < 0) vis = 0;
            else if (da < 0) lerp_v(&a, &b, da / (da - db), nv, &a);
            else if (db < 0) lerp_v(&a, &b, da / (da - db), nv, &b);
        }
        if (!vis) continue;
        Sv p, q;
        if (!project(t, s, &a, &p) || !project(t, s, &b, &q)) continue;
        raster_line(t, s, &p, &q, s->flat ? pv->c : NULL, s->flat_mask ? (const float (*)[4])pv->v : NULL);
    }
}

void sg_cpu_points(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    if (!t->color) return;
    Clip c = clip_rect(t, s);
    Plane pl[MAXPLANES];
    int np = build_planes(t, s, pl, 0);
    int nv = nvary_of(s), pfn = s->pixel_fn != NULL;
    SgPixelIn in;
    if (pfn) memset(&in, 0, sizeof in);
    for (int i = 0; i < n; i++) {
        int vis = 1;
        for (int p = 0; p < np && vis; p++) if (!(plane_dist(&pl[p], &v[i]) >= 0)) vis = 0;
        if (!vis) continue;
        Sv p;
        if (!project(t, s, &v[i], &p)) continue;
        float z = s->depth_clamp ? zclamp(s, p.z) : p.z;
        float sz = s->point_size_vertex ? v[i].psize : s->point_size;
        if (!(sz == sz)) continue;
        if (s->point_size_max > 0) { if (sz < s->point_size_min) sz = s->point_size_min; if (sz > s->point_size_max) sz = s->point_size_max; }
        float at[NATTR];
        for (int k = 0; k < NATTR; k++) at[k] = p.a[k] / p.iw;
        if (pfn) {
            for (int k = 0; k < nv; k++) for (int q = 0; q < 4; q++) { in.v[k][q] = v[i].v[k][q]; in.dx[k][q] = in.dy[k][q] = 0; }
        }
        if (s->point_sprite) {
            if (sz < 1) sz = 1;
            if (sz > 4096) sz = 4096;
            float half = sz * 0.5f;
            int x0 = (int)ceilf(fmaxf(p.x - half - 0.5f, (float)c.x0)), x1 = (int)ceilf(fminf(p.x + half - 0.5f, (float)c.x1));
            int y0 = (int)ceilf(fmaxf(p.y - half - 0.5f, (float)c.y0)), y1 = (int)ceilf(fminf(p.y + half - 0.5f, (float)c.y1));
            int slot = s->sprite_slot;
            if (pfn && slot >= 0 && slot < nv) { in.dx[slot][0] = 1.0f / sz; in.dx[slot][1] = 0; in.dy[slot][0] = 0; in.dy[slot][1] = 1.0f / sz; in.dx[slot][2] = in.dx[slot][3] = in.dy[slot][2] = in.dy[slot][3] = 0; }
            for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
                float su = ((float)x + 0.5f - (p.x - half)) / sz, sv = ((float)y + 0.5f - (p.y - half)) / sz;
                if (pfn) {
                    if (slot >= 0 && slot < nv) { in.v[slot][0] = su; in.v[slot][1] = sv; in.v[slot][2] = 0; in.v[slot][3] = 1; }
                    prog_fragment(t, s, &in, x, y, z, p.iw, 1);
                } else {
                    at[4] = su; at[5] = sv;
                    fragment(t, s, x, y, z, at, 0, 1);
                }
            }
            continue;
        }
        int size = sz > 1.5f ? (int)(sz + 0.5f) : 1;
        if (size > 4096) size = 4096;
        int x0 = (int)floorf(p.x - size / 2.0f + 0.5f), y0 = (int)floorf(p.y - size / 2.0f + 0.5f);
        for (int y = y0; y < y0 + size; y++) for (int x = x0; x < x0 + size; x++)
            if (x >= c.x0 && y >= c.y0 && x < c.x1 && y < c.y1) {
                if (pfn) prog_fragment(t, s, &in, x, y, z, p.iw, 1);
                else fragment(t, s, x, y, z, at, 0, 1);
            }
    }
}

void sg_cpu_clear_ex(SgTarget *t, const SgState *s, int flags, uint32_t argb, float z, uint8_t stencil) {
    int x0 = 0, y0 = 0, x1 = t->w, y1 = t->h;
    if (s->scissor) {
        x0 = s->sc_x; x1 = s->sc_x + s->sc_w;
        if (s->vp_top_left) { y0 = s->sc_y; y1 = s->sc_y + s->sc_h; }
        else { y0 = t->h - (s->sc_y + s->sc_h); y1 = t->h - s->sc_y; }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > t->w) x1 = t->w;
        if (y1 > t->h) y1 = t->h;
    }
    int nomask = (flags & SG_CLEAR_NOMASK) != 0;
    uint32_t keep = nomask ? 0 : (s->mask_r ? 0 : 0x00FF0000u) | (s->mask_g ? 0 : 0x0000FF00u) | (s->mask_b ? 0 : 0x000000FFu) | (s->mask_a ? 0 : 0xFF000000u);
    int wm = nomask ? 255 : s->stencil[0].write_mask & 255;
    for (int y = y0; y < y1; y++) {
        uint32_t *row = t->color + (size_t)y * (size_t)t->w;
        if ((flags & SG_CLEAR_COLOR) && t->color) {
            if (!keep) for (int x = x0; x < x1; x++) row[x] = argb;
            else for (int x = x0; x < x1; x++) row[x] = (row[x] & keep) | (argb & ~keep);
        }
        if ((flags & SG_CLEAR_DEPTH) && t->depth && (nomask || s->depth_write)) {
            float *drow = t->depth + (size_t)y * (size_t)t->w;
            for (int x = x0; x < x1; x++) drow[x] = z;
        }
        if ((flags & SG_CLEAR_STENCIL) && t->stencil) {
            uint8_t *srow = t->stencil + (size_t)y * (size_t)t->w;
            for (int x = x0; x < x1; x++) srow[x] = (uint8_t)((srow[x] & ~wm) | (stencil & wm));
        }
    }
}

void sg_clear(SgTarget *t, const SgState *s, int color, uint32_t argb, int depth, float z) {
    sg_clear_ex(t, s, (color ? SG_CLEAR_COLOR : 0) | (depth ? SG_CLEAR_DEPTH : 0), argb, z, 0);
}

// Box-filters every level (all cube faces / volume slices) from the one above.
void sg_texture_build_mips(SgTexture *tx) {
    for (int i = 1; i < tx->levels; i++) free(tx->mip[i]);
    tx->levels = 1;
    tx->mip[0] = tx->rgba; tx->mip_w[0] = tx->w; tx->mip_h[0] = tx->h; tx->mip_d[0] = tx->kind == SG_TEX_VOLUME ? (tx->depth > 0 ? tx->depth : 1) : 1;
    int faces = tx->kind == SG_TEX_CUBE ? 6 : 1;
    int w = tx->w, h = tx->h, d = tx->mip_d[0];
    while ((w > 1 || h > 1 || d > 1) && tx->levels < SG_MAX_MIPS) {
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1, nd = d > 1 ? d / 2 : 1;
        const uint8_t *src = tx->mip[tx->levels - 1];
        uint8_t *dst = malloc((size_t)nw * (size_t)nh * (size_t)nd * (size_t)faces * 4);
        if (!dst) break;
        for (int f = 0; f < faces; f++) for (int z = 0; z < nd; z++) for (int y = 0; y < nh; y++) for (int x = 0; x < nw; x++) for (int k = 0; k < 4; k++) {
            int sx0 = x * 2, sy0 = y * 2, sz0 = z * 2, sx1 = sx0 + 1 < w ? sx0 + 1 : sx0, sy1 = sy0 + 1 < h ? sy0 + 1 : sy0, sz1 = sz0 + 1 < d ? sz0 + 1 : sz0;
            const uint8_t *sf = src + (size_t)f * (size_t)w * (size_t)h * (size_t)d * 4;
            int sum = 0;
            for (int zz = 0; zz < 2; zz++) {
                const uint8_t *sl = sf + (size_t)(zz ? sz1 : sz0) * (size_t)w * (size_t)h * 4;
                sum += sl[((size_t)sy0 * w + sx0) * 4 + k] + sl[((size_t)sy0 * w + sx1) * 4 + k] + sl[((size_t)sy1 * w + sx0) * 4 + k] + sl[((size_t)sy1 * w + sx1) * 4 + k];
            }
            dst[(((size_t)f * nd + z) * (size_t)nh * nw + (size_t)y * nw + x) * 4 + k] = (uint8_t)((sum + 4) / 8);
        }
        tx->mip[tx->levels] = dst; tx->mip_w[tx->levels] = nw; tx->mip_h[tx->levels] = nh; tx->mip_d[tx->levels] = nd;
        tx->levels++;
        w = nw; h = nh; d = nd;
    }
    sg_texture_touch(tx);
}

void sg_texture_free(SgTexture *tx) {
    sg_texture_release(tx);
    for (int i = 1; i < tx->levels; i++) free(tx->mip[i]);
    free(tx->rgba);
    memset(tx, 0, sizeof *tx);
}

// ---- sg_sample: D3D-style texture sampling ----------------------------------------------------

static float g_srgb_lut[256];
static int g_srgb_init;

static void srgb_init(void) {
    for (int i = 0; i < 256; i++) {
        float c = (float)i / 255.0f;
        g_srgb_lut[i] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
    g_srgb_init = 1;
}

static void level_view(const SgTexture *t, int l, const uint8_t **p, int *w, int *h, int *d) {
    int vol = t->kind == SG_TEX_VOLUME;
    if (l <= 0 || t->levels <= 1) { *p = t->rgba; *w = t->w; *h = t->h; *d = vol ? (t->depth > 0 ? t->depth : 1) : 1; return; }
    *p = t->mip[l]; *w = t->mip_w[l]; *h = t->mip_h[l]; *d = vol ? (t->mip_d[l] > 0 ? t->mip_d[l] : 1) : 1;
}

static inline int addr_index(int i, int n, int mode) { // -1 = border
    switch (mode) {
    case SG_ADDR_MIRROR: { int m = i % (2 * n); if (m < 0) m += 2 * n; return m >= n ? 2 * n - 1 - m : m; }
    case SG_ADDR_CLAMP: return i < 0 ? 0 : i >= n ? n - 1 : i;
    case SG_ADDR_BORDER: return (i < 0 || i >= n) ? -1 : i;
    case SG_ADDR_MIRROR_ONCE: { if (i < 0) i = -i - 1; return i >= n ? n - 1 : i; }
    default: { int m = i % n; return m < 0 ? m + n : m; }
    }
}

typedef struct TexFetch { const uint8_t *img; int w, h, d; int mode[3]; float border[4]; int srgb; } TexFetch;

static void fetch(const TexFetch *f, int x, int y, int z, float *o) {
    int xi = addr_index(x, f->w, f->mode[0]), yi = addr_index(y, f->h, f->mode[1]), zi = f->d > 1 ? addr_index(z, f->d, f->mode[2]) : 0;
    if (xi < 0 || yi < 0 || zi < 0) { memcpy(o, f->border, 16); return; }
    const uint8_t *p = f->img + (((size_t)zi * (size_t)f->h + (size_t)yi) * (size_t)f->w + (size_t)xi) * 4;
    if (f->srgb) { o[0] = g_srgb_lut[p[0]]; o[1] = g_srgb_lut[p[1]]; o[2] = g_srgb_lut[p[2]]; }
    else { o[0] = p[0] * (1.0f / 255); o[1] = p[1] * (1.0f / 255); o[2] = p[2] * (1.0f / 255); }
    o[3] = p[3] * (1.0f / 255);
}

// Filters inside one level; uvw are normalised coordinates.
static void filter_level(const TexFetch *f, int linear, const float *uvw, float *o) {
    float fu = uvw[0] * (float)f->w, fv = uvw[1] * (float)f->h, fw = uvw[2] * (float)f->d;
    if (!isfinite(fu) || !isfinite(fv) || !isfinite(fw)) { fu = fv = fw = 0; }
    fu = fminf(fmaxf(fu, -1e9f), 1e9f);
    fv = fminf(fmaxf(fv, -1e9f), 1e9f);
    fw = fminf(fmaxf(fw, -1e9f), 1e9f);
    if (!linear) {
        fetch(f, (int)floorf(fu), (int)floorf(fv), (int)floorf(fw), o);
        return;
    }
    fu -= 0.5f; fv -= 0.5f;
    int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
    float ax = fu - (float)x0, ay = fv - (float)y0;
    int vol = f->d > 1;
    int z0 = 0; float az = 0;
    if (vol) { fw -= 0.5f; z0 = (int)floorf(fw); az = fw - (float)z0; }
    float acc[4] = { 0, 0, 0, 0 };
    for (int zz = 0; zz < (vol ? 2 : 1); zz++) {
        float c00[4], c10[4], c01[4], c11[4];
        fetch(f, x0, y0, z0 + zz, c00); fetch(f, x0 + 1, y0, z0 + zz, c10);
        fetch(f, x0, y0 + 1, z0 + zz, c01); fetch(f, x0 + 1, y0 + 1, z0 + zz, c11);
        float wz = vol ? (zz ? az : 1 - az) : 1;
        for (int i = 0; i < 4; i++) {
            float top = c00[i] + (c10[i] - c00[i]) * ax, bot = c01[i] + (c11[i] - c01[i]) * ax;
            acc[i] += (top + (bot - top) * ay) * wz;
        }
    }
    memcpy(o, acc, 16);
}

// One sample at a given LOD (mip level selection + filters). face selects the cube face.
static void sample_lod(const SgTexture *t, const SgSampler *s, const float *uvw, int face, float lod, int force_linear, float *o) {
    int nl = t->levels > 1 ? t->levels : 1;
    int base = s->max_mip_level < 0 ? 0 : s->max_mip_level >= nl ? nl - 1 : s->max_mip_level;
    int minify = lod > 0;
    int filt = minify ? s->min_filter : s->mag_filter;
    int linear = force_linear || filt == SG_FILTER_LINEAR;
    TexFetch f;
    f.mode[0] = s->address_u; f.mode[1] = s->address_v; f.mode[2] = s->address_w;
    if (t->kind == SG_TEX_CUBE) f.mode[0] = f.mode[1] = SG_ADDR_CLAMP;
    uint32_t b = s->border_argb;
    f.border[0] = ((b >> 16) & 255) / 255.0f; f.border[1] = ((b >> 8) & 255) / 255.0f; f.border[2] = (b & 255) / 255.0f; f.border[3] = (b >> 24) / 255.0f;
    f.srgb = s->srgb;
    float lv = lod;
    if (!(lv > (float)base)) lv = (float)base;
    if (lv > (float)(nl - 1)) lv = (float)(nl - 1);
    int l0 = base, l1 = base;
    float fr = 0;
    if (minify && s->mip_filter == SG_FILTER_POINT) l0 = l1 = (int)floorf(lv + 0.5f);
    else if (minify && s->mip_filter == SG_FILTER_LINEAR) { l0 = (int)floorf(lv); l1 = l0 + 1 < nl ? l0 + 1 : l0; fr = lv - (float)l0; }
    if (l0 < base) l0 = base;
    if (l1 < base) l1 = base;
    float c0[4], c1[4];
    const uint8_t *p; int w, h, d;
    level_view(t, l0, &p, &w, &h, &d);
    f.img = p + (size_t)face * (size_t)w * (size_t)h * 4; f.w = w; f.h = h; f.d = d;
    filter_level(&f, linear, uvw, c0);
    if (fr > 0 && l1 != l0) {
        level_view(t, l1, &p, &w, &h, &d);
        f.img = p + (size_t)face * (size_t)w * (size_t)h * 4; f.w = w; f.h = h; f.d = d;
        filter_level(&f, linear, uvw, c1);
        for (int i = 0; i < 4; i++) o[i] = c0[i] + (c1[i] - c0[i]) * fr;
    } else memcpy(o, c0, 16);
}

static inline float len3(float a, float b, float c) { return sqrtf(a * a + b * b + c * c); }

void sg_sample(const SgTexture *t, const SgSampler *s, const float coord[4], const float ddx_in[4], const float ddy_in[4], float lod_override, float out[4]) {
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    if (!t || !s || !t->rgba || t->w <= 0 || t->h <= 0) return;
    if (!g_srgb_init) srgb_init();
    static const float zero[4] = { 0, 0, 0, 0 };
    const float *ddx = ddx_in ? ddx_in : zero, *ddy = ddy_in ? ddy_in : zero;
    int explicit_lod = lod_override == lod_override;
    float lod = 0, uvw[3];
    int face = 0, force_linear = 0, ntap = 1;
    float step[3] = { 0, 0, 0 };
    if (t->kind == SG_TEX_CUBE) {
        float x = coord[0], y = coord[1], z = coord[2];
        float ax = fabsf(x), ay = fabsf(y), az = fabsf(z);
        float ma, sc, tc, dsc[2], dtc[2];
        const float *dd[2] = { ddx, ddy };
        int ci;
        if (ax >= ay && ax >= az) { face = x >= 0 ? 0 : 1; ma = ax; ci = 0; sc = x >= 0 ? -z : z; tc = -y; }
        else if (ay >= az) { face = y >= 0 ? 2 : 3; ma = ay; ci = 1; sc = x; tc = y >= 0 ? z : -z; }
        else { face = z >= 0 ? 4 : 5; ma = az; ci = 2; sc = z >= 0 ? x : -x; tc = -y; }
        if (!(ma > 0) || !isfinite(ma)) return;
        uvw[0] = 0.5f * (sc / ma + 1); uvw[1] = 0.5f * (tc / ma + 1); uvw[2] = 0;
        if (!explicit_lod) {
            for (int k = 0; k < 2; k++) {
                const float *d = dd[k];
                float sgn = ci == 0 ? (x >= 0 ? 1.f : -1.f) : ci == 1 ? (y >= 0 ? 1.f : -1.f) : (z >= 0 ? 1.f : -1.f);
                float dm = sgn * d[ci];
                float dscv, dtcv;
                switch (face) {
                case 0: dscv = -d[2]; dtcv = -d[1]; break;
                case 1: dscv = d[2]; dtcv = -d[1]; break;
                case 2: dscv = d[0]; dtcv = d[2]; break;
                case 3: dscv = d[0]; dtcv = -d[2]; break;
                case 4: dscv = d[0]; dtcv = -d[1]; break;
                default: dscv = -d[0]; dtcv = -d[1]; break;
                }
                dsc[k] = 0.5f * (dscv * ma - sc * dm) / (ma * ma) * (float)t->w;
                dtc[k] = 0.5f * (dtcv * ma - tc * dm) / (ma * ma) * (float)t->h;
            }
            float rho = fmaxf(sqrtf(dsc[0] * dsc[0] + dtc[0] * dtc[0]), sqrtf(dsc[1] * dsc[1] + dtc[1] * dtc[1]));
            lod = rho > 0 ? log2f(rho) : -30.0f;
        }
    } else if (t->kind == SG_TEX_VOLUME) {
        uvw[0] = coord[0]; uvw[1] = coord[1]; uvw[2] = coord[2];
        if (!explicit_lod) {
            float dep = (float)(t->depth > 0 ? t->depth : 1);
            float rx = len3(ddx[0] * (float)t->w, ddx[1] * (float)t->h, ddx[2] * dep), ry = len3(ddy[0] * (float)t->w, ddy[1] * (float)t->h, ddy[2] * dep);
            float rho = fmaxf(rx, ry);
            lod = rho > 0 ? log2f(rho) : -30.0f;
        }
    } else {
        uvw[0] = coord[0]; uvw[1] = coord[1]; uvw[2] = 0;
        if (!explicit_lod) {
            float ex = ddx[0] * (float)t->w, ey = ddx[1] * (float)t->h, fx = ddy[0] * (float)t->w, fy = ddy[1] * (float)t->h;
            float lx = sqrtf(ex * ex + ey * ey), ly = sqrtf(fx * fx + fy * fy);
            float pmax = fmaxf(lx, ly), pmin = fminf(lx, ly);
            int an = s->max_anisotropy;
            if (an > 16) an = 16;
            if (an > 1 && s->min_filter == SG_FILTER_LINEAR && pmax > 0) {
                float nn = pmin > 0 ? ceilf(pmax / pmin) : (float)an;
                ntap = nn > (float)an ? an : (int)nn;
                if (ntap < 1) ntap = 1;
                pmax /= (float)ntap;
                const float *md = lx >= ly ? ddx : ddy;
                step[0] = md[0]; step[1] = md[1];
            }
            lod = pmax > 0 ? log2f(pmax) : -30.0f;
        }
    }
    if (explicit_lod) lod = lod_override;
    lod += s->lod_bias;
    if (ntap > 1) force_linear = 1;
    if (ntap == 1) { sample_lod(t, s, uvw, face, lod, force_linear, out); return; }
    float acc[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < ntap; i++) {
        float o = ((float)i + 0.5f) / (float)ntap - 0.5f;
        float c[3] = { uvw[0] + step[0] * o, uvw[1] + step[1] * o, uvw[2] };
        float v[4];
        sample_lod(t, s, c, face, lod, force_linear, v);
        for (int k = 0; k < 4; k++) acc[k] += v[k];
    }
    for (int k = 0; k < 4; k++) out[k] = acc[k] / (float)ntap;
}

// ---- backend dispatch ---------------------------------------------------------------------------------------------

const SgBackend *sg_mirror_backend(void) __attribute__((weak));

static const SgBackend *g_backend;
static int g_backend_init;

static void backend_init(void) {
    g_backend_init = 1;
    const char *e = getenv("SOUL_GPU");
    if (e && !strcmp(e, "mirror") && sg_mirror_backend) g_backend = sg_mirror_backend();
}
static inline const SgBackend *be(void) {
    if (__builtin_expect(!g_backend_init, 0)) backend_init();
    return g_backend;
}

void sg_backend_set(const SgBackend *b) { g_backend_init = 1; g_backend = b; }
const SgBackend *sg_backend_get(void) { return be(); }

static int bk_draw(const SgBackend *b, SgTarget *t, const SgState *s, int prim, const SgVertex *v, int n) {
    if (!t->color || n <= 0) return 1;   // nothing to do: same as the CPU path
    if (b->draw && b->draw(b->self, t, s, prim, v, n) == 0) return 1;
    return 0;
}
static void bk_fallback_pre(const SgBackend *b, SgTarget *t) { if (b->sync_to_cpu) b->sync_to_cpu(b->self, t); }
static void bk_fallback_post(const SgBackend *b, SgTarget *t) { if (b->cpu_wrote) b->cpu_wrote(b->self, t, 0, 0, 0, 0); }

void sg_triangles(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    const SgBackend *b = be();
    if (!b) { sg_cpu_triangles(t, s, v, n); return; }
    if (bk_draw(b, t, s, SG_PRIM_TRIANGLES, v, n)) return;
    bk_fallback_pre(b, t); sg_cpu_triangles(t, s, v, n); bk_fallback_post(b, t);
}
void sg_lines(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    const SgBackend *b = be();
    if (!b) { sg_cpu_lines(t, s, v, n); return; }
    if (bk_draw(b, t, s, SG_PRIM_LINES, v, n)) return;
    bk_fallback_pre(b, t); sg_cpu_lines(t, s, v, n); bk_fallback_post(b, t);
}
void sg_points(SgTarget *t, const SgState *s, const SgVertex *v, int n) {
    const SgBackend *b = be();
    if (!b) { sg_cpu_points(t, s, v, n); return; }
    if (bk_draw(b, t, s, SG_PRIM_POINTS, v, n)) return;
    bk_fallback_pre(b, t); sg_cpu_points(t, s, v, n); bk_fallback_post(b, t);
}
void sg_clear_ex(SgTarget *t, const SgState *s, int flags, uint32_t argb, float z, uint8_t stencil) {
    const SgBackend *b = be();
    if (!b) { sg_cpu_clear_ex(t, s, flags, argb, z, stencil); return; }
    if (b->clear && b->clear(b->self, t, s, flags, argb, z, stencil) == 0) return;
    bk_fallback_pre(b, t); sg_cpu_clear_ex(t, s, flags, argb, z, stencil); bk_fallback_post(b, t);
}

void sg_target_sync(SgTarget *t) { const SgBackend *b = be(); if (b && b->sync_to_cpu) b->sync_to_cpu(b->self, t); }
void sg_target_cpu_wrote(SgTarget *t, int x, int y, int w, int h) { const SgBackend *b = be(); if (b && b->cpu_wrote) b->cpu_wrote(b->self, t, x, y, w, h); }
void sg_target_gone(const void *color, const void *depth, const void *stencil) {
    const SgBackend *b = be();
    if (b && b->target_gone && (color || depth || stencil)) b->target_gone(b->self, color, depth, stencil);
}
void sg_end_frame(void) { const SgBackend *b = be(); if (b && b->end_frame) b->end_frame(b->self); }

// ---- texture identity -----------------------------------------------------------------------------------------------

static uint64_t g_next_tex_id = 1;

uint64_t sg_texture_id(SgTexture *tx) {
    if (!tx->id) tx->id = g_next_tex_id++;
    return tx->id;
}
void sg_texture_touch(SgTexture *tx) {
    sg_texture_id(tx);
    tx->version++;
}
void sg_texture_release(SgTexture *tx) {
    if (tx->id) {
        const SgBackend *b = be();
        if (b && b->texture_gone) b->texture_gone(b->self, tx);
    }
    tx->id = 0; tx->version = 0;
}

// ---- pixel stage key --------------------------------------------------------------------------------------------------

typedef struct KeyH { uint64_t h; } KeyH;
static inline void kh(KeyH *k, uint64_t v) { k->h = (k->h ^ v) * 0x100000001b3ULL; k->h ^= k->h >> 29; }

uint64_t sg_program_key(const SgState *s) {
    KeyH k = { 0xcbf29ce484222325ULL };
    const SgProgram *p = s->program;
    kh(&k, (uint64_t)(p ? p->kind : SGP_NONE));
    kh(&k, (uint64_t)(unsigned)nvary_of(s));
    kh(&k, (uint64_t)(s->pixel_writes_depth != 0));
    if (!p) {
        for (int u = 0; u < SG_UNITS; u++) {
            int on = s->tex_on[u] && s->tex[u];
            kh(&k, (uint64_t)on);
            if (on) { kh(&k, (uint64_t)s->tex_env[u]); kh(&k, (uint64_t)s->tex[u]->format); }
        }
        kh(&k, (uint64_t)(s->fog != 0)); kh(&k, (uint64_t)(s->flat != 0));
        kh(&k, (uint64_t)(s->alpha_test != 0)); if (s->alpha_test) kh(&k, (uint64_t)s->alpha_func);
        return k.h;
    }
    kh(&k, (uint64_t)(s->alpha_test != 0)); if (s->alpha_test) kh(&k, (uint64_t)s->alpha_func);
    kh(&k, (uint64_t)p->fog.enable); kh(&k, (uint64_t)(p->fog.enable ? p->fog.table : 0));
    if (p->kind == SGP_D3D9_TSS) {
        const SgpTss *t = &p->tss;
        kh(&k, (uint64_t)t->nstages); kh(&k, (uint64_t)(t->specular != 0));
        for (int i = 0; i < t->nstages && i < SGP_MAX_STAGES; i++) {
            const SgpTssStage *st = &t->stage[i];
            kh(&k, (uint64_t)st->colorop); kh(&k, (uint64_t)st->alphaop); kh(&k, (uint64_t)st->result_arg);
            for (int a = 0; a < 3; a++) { kh(&k, (uint64_t)(unsigned)st->color_arg[a]); kh(&k, (uint64_t)(unsigned)st->alpha_arg[a]); }
            kh(&k, (uint64_t)(unsigned)st->slot); kh(&k, (uint64_t)(unsigned)st->ncoord); kh(&k, (uint64_t)(st->projected != 0));
            kh(&k, (uint64_t)(unsigned)st->tex_kind); kh(&k, (uint64_t)(st->bump_signed != 0));
            kh(&k, (uint64_t)(p->samplers[i].tex ? (unsigned)p->samplers[i].tex->kind + 1u : 0u));
        }
    } else if (p->kind == SGP_D3D9_PS) {
        const SgpPs *ps = &p->ps;
        for (int i = 0; i < ps->ntokens; i++) kh(&k, ps->tokens[i]);
        kh(&k, (uint64_t)(ps->writes_depth != 0)); kh(&k, (uint64_t)(ps->uses_vpos != 0)); kh(&k, (uint64_t)(ps->uses_vface != 0));
        for (int i = 0; i < SGP_MAX_SAMPLERS; i++) if (ps->sampler_mask & (1u << i)) {
            kh(&k, (uint64_t)i); kh(&k, (uint64_t)(unsigned)ps->sampler_type[i]);
            kh(&k, (uint64_t)(p->samplers[i].tex ? (unsigned)p->samplers[i].tex->kind + 1u : 0u));
        }
    }
    return k.h;
}
