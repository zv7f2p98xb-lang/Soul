// SoulCore - Direct3D 9 drawing: render target setup, Clear, vertex fetch (all D3DDECLTYPEs), the
// fixed-function vertex pipeline (transform, lighting, texture coordinate generation, fog, point size),
// vertex shaders (through d3d9_shader.h), the fixed-function texture stage cascade as an sgpu pixel
// function, pixel shaders, and the hand-over of primitives to sgpu.
//
// D3D9 pixel centres lie at integer coordinates while sgpu's are at +0.5: every clip-space position is shifted
// by half a pixel (x += w / vp_w, y -= w / vp_h) so that XYZRHW quads and shader output cover the pixels they
// do on Windows. Depth is a float; every depth format maps to the sgpu float depth + 8-bit stencil.
#include "d3d9_priv.h"

// ---- small math ------------------------------------------------------------------------------------------------------
static inline float sat(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }
static void m4_mul(float *r, const float *a, const float *b) { // row vectors: r = a * b
    float t[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
        t[i * 4 + j] = s;
    }
    memcpy(r, t, 64);
}
static void v4_xf(const float *v, const float *m, float *o) { // o = v * m
    float t[4];
    for (int j = 0; j < 4; j++) t[j] = v[0] * m[j] + v[1] * m[4 + j] + v[2] * m[8 + j] + v[3] * m[12 + j];
    memcpy(o, t, 16);
}
static int m4_inv(const float *m, float *out) {
    double a[4][8];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { a[i][j] = m[i * 4 + j]; a[i][4 + j] = i == j; }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[p][c])) p = r;
        if (fabs(a[p][c]) < 1e-30) return 0;
        if (p != c) for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
        double d = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= d;
        for (int r = 0; r < 4; r++) if (r != c) { double f = a[r][c]; if (f != 0) for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; }
    }
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[i * 4 + j] = (float)a[i][4 + j];
    return 1;
}
static inline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void norm3(float *v) { float l = sqrtf(dot3(v, v)); if (l > 1e-20f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void col_to_f(uint32_t c, float *o) { o[0] = (float)((c >> 16) & 255) / 255.0f; o[1] = (float)((c >> 8) & 255) / 255.0f; o[2] = (float)(c & 255) / 255.0f; o[3] = (float)(c >> 24) / 255.0f; }
static inline float rsf(const D9State *S, int i) { return u2f(S->rs[i]); }

// ---- targets ---------------------------------------------------------------------------------------------------------------
static int has_stencil(int f) { return f == F_D24S8 || f == F_D15S1 || f == F_D24X4S4 || f == F_D24FS8; }
void d9_surf_target(D9Dev *dev, D9Surf *rt, D9Surf *ds, SgTarget *t) {
    (void)dev;
    memset(t, 0, sizeof *t);
    if (rt->shadow && rt->argb_stale) d9_surf_from_native(rt);
    t->color = rt->argb; t->w = rt->w; t->h = rt->h;
    if (!ds || !ds->depth) return;
    if (ds->w == rt->w && ds->h >= rt->h) { t->depth = ds->depth; t->stencil = has_stencil(ds->fmt) ? ds->stencil : NULL; return; }
    // depth buffer of another width: keep a render-target sized copy (contents are not shared with the larger one)
    if (!ds->sdepth || ds->sw_ != rt->w || ds->sh_ != rt->h) {
        free(ds->sdepth); free(ds->sstencil);
        ds->sw_ = rt->w; ds->sh_ = rt->h;
        ds->sdepth = malloc((size_t)rt->w * (size_t)rt->h * sizeof(float));
        ds->sstencil = calloc((size_t)rt->w * (size_t)rt->h, 1);
        for (size_t i = 0; i < (size_t)rt->w * (size_t)rt->h; i++) ds->sdepth[i] = 1.0f;
    }
    t->depth = ds->sdepth;
    t->stencil = has_stencil(ds->fmt) ? ds->sstencil : NULL;
}

uint32_t d9_clear(D9Dev *dev, uint32_t count, uint64_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!dev->rt) return D3DERR_INVALIDCALL;
    if (count && !rects) return D3DERR_INVALIDCALL;
    if ((flags & 2) && !dev->ds) return D3DERR_INVALIDCALL;
    if ((flags & 4) && (!dev->ds || !has_stencil(dev->ds->fmt))) return D3DERR_INVALIDCALL;
    D9State *S = &dev->st;
    SgTarget t;
    d9_surf_target(dev, dev->rt, dev->ds, &t);
    SgState s;
    memset(&s, 0, sizeof s);
    s.vp_top_left = 1;
    s.mask_r = (S->rs[168] & 1) != 0; s.mask_g = (S->rs[168] & 2) != 0; s.mask_b = (S->rs[168] & 4) != 0; s.mask_a = (S->rs[168] & 8) != 0;
    s.depth_write = 1;
    s.stencil[0].write_mask = (int)(S->rs[59] & 255);
    int vx0 = (int)S->vp.x, vy0 = (int)S->vp.y, vx1 = vx0 + (int)S->vp.w, vy1 = vy0 + (int)S->vp.h;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > t.w) vx1 = t.w;
    if (vy1 > t.h) vy1 = t.h;
    int sgf = ((flags & 1) ? SG_CLEAR_COLOR : 0) | ((flags & 2) ? SG_CLEAR_DEPTH : 0) | ((flags & 4) ? SG_CLEAR_STENCIL : 0);
    if (!sgf) return D3D_OK;
    if (z < 0) z = 0;
    if (z > 1) z = 1;
    uint32_t n = count ? count : 1;
    for (uint32_t i = 0; i < n; i++) {
        int x0 = vx0, y0 = vy0, x1 = vx1, y1 = vy1;
        if (count) {
            int rx0 = (int32_t)mem_rd(rects + 16 * (uint64_t)i, 4), ry0 = (int32_t)mem_rd(rects + 16 * (uint64_t)i + 4, 4);
            int rx1 = (int32_t)mem_rd(rects + 16 * (uint64_t)i + 8, 4), ry1 = (int32_t)mem_rd(rects + 16 * (uint64_t)i + 12, 4);
            if (rx0 > x0) x0 = rx0;
            if (ry0 > y0) y0 = ry0;
            if (rx1 < x1) x1 = rx1;
            if (ry1 < y1) y1 = ry1;
        }
        if (S->rs[174]) {
            if (S->scissor[0] > x0) x0 = S->scissor[0];
            if (S->scissor[1] > y0) y0 = S->scissor[1];
            if (S->scissor[2] < x1) x1 = S->scissor[2];
            if (S->scissor[3] < y1) y1 = S->scissor[3];
        }
        if (x0 >= x1 || y0 >= y1) continue;
        s.scissor = 1; s.sc_x = x0; s.sc_y = y0; s.sc_w = x1 - x0; s.sc_h = y1 - y0;
        sg_clear_ex(&t, &s, sgf, color, z, (uint8_t)stencil);
    }
    if (flags & 1) d9_surf_touch(dev->rt);
    return D3D_OK;
}

// ---- vertex element decoding ---------------------------------------------------------------------------------------------------
static const uint8_t k_tsize[17] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8 };
static const uint8_t k_tcomp[17] = { 1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4 };
static void decode_el(const uint8_t *p, int type, float o[4]) {
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    switch (type) {
    case 0: memcpy(o, p, 4); break;
    case 1: memcpy(o, p, 8); break;
    case 2: memcpy(o, p, 12); break;
    case 3: memcpy(o, p, 16); break;
    case 4: { uint32_t c; memcpy(&c, p, 4); col_to_f(c, o); break; }
    case 5: for (int i = 0; i < 4; i++) o[i] = (float)p[i]; break;
    case 6: { int16_t s[2]; memcpy(s, p, 4); o[0] = s[0]; o[1] = s[1]; break; }
    case 7: { int16_t s[4]; memcpy(s, p, 8); for (int i = 0; i < 4; i++) o[i] = s[i]; break; }
    case 8: for (int i = 0; i < 4; i++) o[i] = (float)p[i] / 255.0f; break;
    case 9: { int16_t s[2]; memcpy(s, p, 4); for (int i = 0; i < 2; i++) { o[i] = (float)s[i] / 32767.0f; if (o[i] < -1) o[i] = -1; } break; }
    case 10: { int16_t s[4]; memcpy(s, p, 8); for (int i = 0; i < 4; i++) { o[i] = (float)s[i] / 32767.0f; if (o[i] < -1) o[i] = -1; } break; }
    case 11: { uint16_t s[2]; memcpy(s, p, 4); o[0] = (float)s[0] / 65535.0f; o[1] = (float)s[1] / 65535.0f; break; }
    case 12: { uint16_t s[4]; memcpy(s, p, 8); for (int i = 0; i < 4; i++) o[i] = (float)s[i] / 65535.0f; break; }
    case 13: { uint32_t v; memcpy(&v, p, 4); o[0] = (float)(v & 1023); o[1] = (float)((v >> 10) & 1023); o[2] = (float)((v >> 20) & 1023); break; }
    case 14: {
        uint32_t v; memcpy(&v, p, 4);
        for (int i = 0; i < 3; i++) { int x = (int)((v >> (10 * i)) & 1023); if (x & 512) x -= 1024; o[i] = (float)x / 511.0f; if (o[i] < -1) o[i] = -1; }
        break;
    }
    case 15: { uint16_t h[2]; memcpy(h, p, 4); o[0] = d9_half_to_float(h[0]); o[1] = d9_half_to_float(h[1]); break; }
    case 16: { uint16_t h[4]; memcpy(h, p, 8); for (int i = 0; i < 4; i++) o[i] = d9_half_to_float(h[i]); break; }
    }
}

// ---- draw context -------------------------------------------------------------------------------------------------------------------
#define MAXLIGHTS 8
typedef struct VLight {
    int type;
    float diffuse[3], specular[3], ambient[3];
    float pos[3], dir[3];        // view space
    float range, falloff, a0, a1, a2, cos_theta, cos_phi;
} VLight;

typedef struct DrawCtx {
    D9Dev *dev;
    D9State *S;
    D9Decl *decl;
    int use_vs, pretrans;
    D9Shader *vs;
    D3dVsCtx vsctx;
    struct { int reg, el; } vsin[16];
    int nvsin;
    // streams
    const uint8_t *sbase[16];
    uint32_t sstride[16], sfreq[16];
    size_t slimit[16];
    // fixed function
    int el_pos, el_nrm, el_col[2], el_tex[8], el_psize;
    float WV[16], WVP[16], VIEW[16], NM[9];
    float tmat[8][16];
    int lighting, specular_en, local_viewer, color_vertex;
    int msrc_d, msrc_s, msrc_a, msrc_e;
    float gamb[3];
    VLight lights[MAXLIGHTS];
    int nlights;
    int fog_vertex, fog_range, fog_mode;
    float fog_start, fog_end, fog_density;
    int nstages_tc;                // texture coordinate sets produced (stages)
    uint32_t tc_stage_mask;
    int tci[8], ttff[8];
    int point_scale;
    float ps_size, ps_min, ps_max, ps_a, ps_b, ps_c, vp_h;
    // viewport
    float vpx, vpy, vpw, vph, vpminz, vpmaxz;
    float shift_x, shift_y;
    int noshift;
    int flags_vs_psize;
} DrawCtx;

static const uint8_t k_stock_map[14] = { 0, 1, 2, 3, 4, 7, 99, 99, 99, 99, 5, 99, 99, 99 };
static int stock_reg(int usage, int idx) {
    if (usage == D3DSU_POSITION && idx == 0) return 0;
    if (usage == D3DSU_BLENDWEIGHT && idx == 0) return 1;
    if (usage == D3DSU_BLENDINDICES && idx == 0) return 2;
    if (usage == D3DSU_NORMAL && idx == 0) return 3;
    if (usage == D3DSU_PSIZE && idx == 0) return 4;
    if (usage == D3DSU_COLOR && idx < 2) return 5 + idx;
    if (usage == D3DSU_TEXCOORD && idx < 8) return 7 + idx;
    (void)k_stock_map;
    return -1;
}

static int fetch_el(const DrawCtx *c, const D9Elem *e, uint32_t vi, uint32_t inst, float out[4]) {
    int s = e->stream;
    out[0] = out[1] = out[2] = 0; out[3] = 1;
    if (e->type > 16 || !c->sbase[s]) return 0;
    uint32_t idx = vi;
    uint32_t f = c->sfreq[s];
    if ((f >> 30) == 2) { uint32_t div = f & 0x3FFFFFFF; idx = div ? inst / div : inst; }
    size_t off = (size_t)idx * c->sstride[s] + e->offset;
    if (off + k_tsize[e->type] > c->slimit[s]) return 0;
    decode_el(c->sbase[s] + off, e->type, out);
    return 1;
}

// ---- fixed-function vertex pipeline ------------------------------------------------------------------------------------------------------
static void setup_ff(DrawCtx *c) {
    D9State *S = c->S;
    const float *W = S->xf[24], *V = S->xf[2], *P = S->xf[3];
    m4_mul(c->WV, W, V);
    m4_mul(c->WVP, c->WV, P);
    memcpy(c->VIEW, V, 64);
    // normal matrix: inverse transpose of the upper 3x3 of WV
    float a[9] = { c->WV[0], c->WV[1], c->WV[2], c->WV[4], c->WV[5], c->WV[6], c->WV[8], c->WV[9], c->WV[10] };
    float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) + a[2] * (a[3] * a[7] - a[4] * a[6]);
    if (fabsf(det) > 1e-20f) {
        float id = 1.0f / det, inv[9];
        inv[0] = (a[4] * a[8] - a[5] * a[7]) * id; inv[1] = (a[2] * a[7] - a[1] * a[8]) * id; inv[2] = (a[1] * a[5] - a[2] * a[4]) * id;
        inv[3] = (a[5] * a[6] - a[3] * a[8]) * id; inv[4] = (a[0] * a[8] - a[2] * a[6]) * id; inv[5] = (a[2] * a[3] - a[0] * a[5]) * id;
        inv[6] = (a[3] * a[7] - a[4] * a[6]) * id; inv[7] = (a[1] * a[6] - a[0] * a[7]) * id; inv[8] = (a[0] * a[4] - a[1] * a[3]) * id;
        // n' = n * inv(A)^T  =>  n'_j = sum_i n_i * inv[j*3 + i]
        memcpy(c->NM, inv, sizeof inv);
    } else memcpy(c->NM, a, sizeof a);
    c->lighting = S->rs[137] != 0;
    c->specular_en = S->rs[29] != 0;
    c->local_viewer = S->rs[142] != 0;
    c->color_vertex = S->rs[141] != 0;
    c->msrc_d = (int)S->rs[145]; c->msrc_s = (int)S->rs[146]; c->msrc_a = (int)S->rs[147]; c->msrc_e = (int)S->rs[148];
    float amb[4];
    col_to_f(S->rs[139], amb);
    memcpy(c->gamb, amb, 12);
    c->nlights = 0;
    for (int i = 0; i < D9_NLIGHT && c->nlights < MAXLIGHTS; i++) {
        if (!S->light_set[i] || !S->light_on[i]) continue;
        const D3DLight9 *l = &S->light[i];
        VLight *o = &c->lights[c->nlights++];
        o->type = (int)l->type;
        for (int k = 0; k < 3; k++) { o->diffuse[k] = l->diffuse[k]; o->specular[k] = l->specular[k]; o->ambient[k] = l->ambient[k]; }
        float p[4] = { l->pos[0], l->pos[1], l->pos[2], 1 }, d[4] = { l->dir[0], l->dir[1], l->dir[2], 0 };
        v4_xf(p, V, p); v4_xf(d, V, d);
        memcpy(o->pos, p, 12); memcpy(o->dir, d, 12);
        norm3(o->dir);
        o->range = l->range; o->falloff = l->falloff; o->a0 = l->att0; o->a1 = l->att1; o->a2 = l->att2;
        o->cos_theta = cosf(l->theta * 0.5f); o->cos_phi = cosf(l->phi * 0.5f);
    }
    // fog
    int fogen = S->rs[28] != 0;
    c->fog_mode = fogen ? (int)S->rs[35] : 0;     // table fog (pixel)
    c->fog_vertex = (fogen && !c->fog_mode) ? (int)S->rs[140] : 0;
    c->fog_range = S->rs[48] != 0;
    c->fog_start = rsf(S, 36); c->fog_end = rsf(S, 37); c->fog_density = rsf(S, 38);
    // texture coordinates
    for (int s = 0; s < 8; s++) {
        c->tci[s] = (int)S->tss[s][11];
        c->ttff[s] = (int)S->tss[s][24];
        memcpy(c->tmat[s], S->xf[16 + s], 64);
    }
    c->point_scale = S->rs[157] != 0;
    c->ps_size = rsf(S, 154); c->ps_min = rsf(S, 155); c->ps_max = rsf(S, 166);
    c->ps_a = rsf(S, 158); c->ps_b = rsf(S, 159); c->ps_c = rsf(S, 160);
}

static float fog_factor(int mode, float d, float start, float end, float density) {
    float f = 1;
    switch (mode) {
    case 1: f = expf(-d * density); break;                       // D3DFOG_EXP
    case 2: f = expf(-(d * density) * (d * density)); break;     // EXP2
    case 3: f = end != start ? (end - d) / (end - start) : (d < start ? 1.0f : 0.0f); break; // LINEAR
    }
    return sat(f);
}

typedef struct VIn { float pos[4], nrm[3], col[2][4], tex[8][4], psize; int has_nrm, has_col[2], ntex[8], has_psize; } VIn;

static void ff_fetch(const DrawCtx *c, uint32_t vi, uint32_t inst, VIn *in) {
    memset(in, 0, sizeof *in);
    float t[4];
    const D9Elem *e = c->decl->e;
    if (c->el_pos >= 0) { fetch_el(c, &e[c->el_pos], vi, inst, t); memcpy(in->pos, t, 16); }
    else in->pos[3] = 1;
    if (c->el_nrm >= 0 && fetch_el(c, &e[c->el_nrm], vi, inst, t)) { memcpy(in->nrm, t, 12); in->has_nrm = 1; }
    for (int k = 0; k < 2; k++) if (c->el_col[k] >= 0 && fetch_el(c, &e[c->el_col[k]], vi, inst, t)) { memcpy(in->col[k], t, 16); in->has_col[k] = 1; }
    for (int k = 0; k < 8; k++) if (c->el_tex[k] >= 0 && fetch_el(c, &e[c->el_tex[k]], vi, inst, t)) {
        memcpy(in->tex[k], t, 16);
        in->ntex[k] = k_tcomp[e[c->el_tex[k]].type];
    }
    if (c->el_psize >= 0 && fetch_el(c, &e[c->el_psize], vi, inst, t)) { in->psize = t[0]; in->has_psize = 1; }
}

static void light_vertex(const DrawCtx *c, const VIn *in, const float pv[3], const float nv[3], float diff[4], float spec[4]) {
    const D3DMaterial9 *m = &c->S->mat;
    const float *vd = in->has_col[0] ? in->col[0] : (const float[4]){ 1, 1, 1, 1 };
    const float *vs = in->has_col[1] ? in->col[1] : (const float[4]){ 0, 0, 0, 0 };
    const float *md = c->color_vertex ? (c->msrc_d == 1 ? vd : c->msrc_d == 2 ? vs : m->diffuse) : m->diffuse;
    const float *ms = c->color_vertex ? (c->msrc_s == 1 ? vd : c->msrc_s == 2 ? vs : m->specular) : m->specular;
    const float *ma = c->color_vertex ? (c->msrc_a == 1 ? vd : c->msrc_a == 2 ? vs : m->ambient) : m->ambient;
    const float *me = c->color_vertex ? (c->msrc_e == 1 ? vd : c->msrc_e == 2 ? vs : m->emissive) : m->emissive;
    float dacc[3] = { 0, 0, 0 }, aacc[3] = { c->gamb[0], c->gamb[1], c->gamb[2] }, sacc[3] = { 0, 0, 0 };
    float eye[3] = { -pv[0], -pv[1], -pv[2] };
    if (c->local_viewer) norm3(eye); else { eye[0] = 0; eye[1] = 0; eye[2] = -1; }
    for (int i = 0; i < c->nlights; i++) {
        const VLight *l = &c->lights[i];
        float L[3], atten = 1, spot = 1;
        if (l->type == 3) { L[0] = -l->dir[0]; L[1] = -l->dir[1]; L[2] = -l->dir[2]; }
        else {
            L[0] = l->pos[0] - pv[0]; L[1] = l->pos[1] - pv[1]; L[2] = l->pos[2] - pv[2];
            float d = sqrtf(dot3(L, L));
            if (l->range > 0 && d > l->range) continue;
            if (d > 1e-20f) { L[0] /= d; L[1] /= d; L[2] /= d; }
            float den = l->a0 + l->a1 * d + l->a2 * d * d;
            atten = den != 0 ? 1.0f / den : 1.0f;
            if (l->type == 2) {
                float rho = -dot3(L, l->dir);
                if (rho <= l->cos_phi) spot = 0;
                else if (rho > l->cos_theta) spot = 1;
                else { float x = (rho - l->cos_phi) / (l->cos_theta - l->cos_phi); spot = l->falloff == 1.0f ? x : powf(x, l->falloff); }
            }
        }
        float f = atten * spot;
        if (f <= 0) continue;
        float ndl = dot3(nv, L);
        for (int k = 0; k < 3; k++) aacc[k] += f * l->ambient[k];
        if (ndl > 0) {
            for (int k = 0; k < 3; k++) dacc[k] += f * l->diffuse[k] * ndl;
            if (c->specular_en) {
                float H[3] = { L[0] + eye[0], L[1] + eye[1], L[2] + eye[2] };
                norm3(H);
                float ndh = dot3(nv, H);
                if (ndh > 0) {
                    float sp = m->power > 0 ? powf(ndh, m->power) : 1.0f;
                    for (int k = 0; k < 3; k++) sacc[k] += f * l->specular[k] * sp;
                }
            }
        }
    }
    for (int k = 0; k < 3; k++) {
        diff[k] = sat(me[k] + ma[k] * aacc[k] + md[k] * dacc[k]);
        spec[k] = c->specular_en ? sat(ms[k] * sacc[k]) : 0;
    }
    diff[3] = sat(md[3]);
    spec[3] = 1;
}

static void ff_vertex(const DrawCtx *c, const VIn *in, SgVertex *o) {
    memset(o, 0, sizeof *o);
    float clip[4];
    float diff[4] = { 1, 1, 1, 1 }, spec[4] = { 0, 0, 0, 1 };
    float pv[3] = { 0, 0, 0 }, nv[3] = { 0, 0, 0 };
    int pre = c->pretrans;
    if (pre) {
        float rhw = in->pos[3];
        float w = rhw != 0 ? 1.0f / rhw : 1.0f;
        // screen position (D3D pixel centres at integers) -> clip space of the sgpu viewport (+0.5)
        float sx = in->pos[0] + (c->noshift ? 0.0f : 0.5f), sy = in->pos[1] + (c->noshift ? 0.0f : 0.5f);
        float nx = (sx - c->vpx) / c->vpw * 2.0f - 1.0f, ny = 1.0f - (sy - c->vpy) / c->vph * 2.0f;
        clip[0] = nx * w; clip[1] = ny * w; clip[2] = in->pos[2] * w; clip[3] = w;
        if (in->has_col[0]) memcpy(diff, in->col[0], 16);
        if (in->has_col[1]) memcpy(spec, in->col[1], 16); else spec[3] = 1;
        pv[2] = in->pos[2];
    } else {
        float p[4] = { in->pos[0], in->pos[1], in->pos[2], in->pos[3] };
        v4_xf(p, c->WVP, clip);
        float pw[4] = { p[0], p[1], p[2], p[3] };
        v4_xf(pw, c->WV, pw);
        pv[0] = pw[0]; pv[1] = pw[1]; pv[2] = pw[2];
        if (in->has_nrm) for (int j = 0; j < 3; j++) nv[j] = in->nrm[0] * c->NM[j * 3 + 0] + in->nrm[1] * c->NM[j * 3 + 1] + in->nrm[2] * c->NM[j * 3 + 2];
        if (c->lighting) light_vertex(c, in, pv, nv, diff, spec);
        else {
            if (in->has_col[0]) memcpy(diff, in->col[0], 16);
            if (in->has_col[1]) memcpy(spec, in->col[1], 16); else { spec[0] = spec[1] = spec[2] = 0; spec[3] = 1; }
        }
    }
    float fog = 1;
    if (c->fog_vertex) {
        if (pre) fog = spec[3];
        else {
            float d = c->fog_range ? sqrtf(dot3(pv, pv)) : fabsf(pv[2]);
            fog = fog_factor(c->fog_vertex, d, c->fog_start, c->fog_end, c->fog_density);
        }
    }
    // texture coordinates, one set per stage
    for (int s = 0; s < 8; s++) {
        if (!(c->tc_stage_mask & (1u << s))) continue;
        int idx = c->tci[s] & 0xFFFF, mode = (c->tci[s] >> 16) & 7;
        float src[4] = { 0, 0, 0, 0 };
        int n = 0;
        if (mode == 0) {
            if (idx < 8) { n = in->ntex[idx]; memcpy(src, in->tex[idx], 16); }
        } else if (mode == 1) { memcpy(src, nv, 12); n = 3; }
        else if (mode == 2) { memcpy(src, pv, 12); n = 3; }
        else {
            float e[3] = { pv[0], pv[1], pv[2] };
            norm3(e);
            float d = dot3(nv, e);
            float r[3] = { e[0] - 2 * d * nv[0], e[1] - 2 * d * nv[1], e[2] - 2 * d * nv[2] };
            if (mode == 3) { memcpy(src, r, 12); n = 3; }
            else { float m = 2.0f * sqrtf(r[0] * r[0] + r[1] * r[1] + (r[2] + 1) * (r[2] + 1)); if (m < 1e-12f) m = 1; src[0] = r[0] / m + 0.5f; src[1] = -r[1] / m + 0.5f; n = 2; }
        }
        int cnt = c->ttff[s] & 0xFF;
        if (cnt >= 1 && cnt <= 4) {
            float vin[4] = { 0, 0, 0, 0 };
            for (int k = 0; k < n && k < 4; k++) vin[k] = src[k];
            if (n < 4) vin[n] = 1;
            float outv[4];
            v4_xf(vin, c->tmat[s], outv);
            for (int k = cnt; k < 4; k++) outv[k] = 0;
            memcpy(o->v[s], outv, 16);
        } else {
            for (int k = 0; k < 4; k++) o->v[s][k] = k < n ? src[k] : 0;
            if (n == 2) o->v[s][2] = 0;
        }
    }
    memcpy(o->v[8], diff, 16);
    memcpy(o->v[9], spec, 16);
    o->v[10][0] = fog;
    // position with the half-pixel shift
    if (!pre && !c->noshift) { clip[0] += clip[3] * c->shift_x; clip[1] -= clip[3] * c->shift_y; }
    o->x = clip[0]; o->y = clip[1]; o->z = clip[2]; o->w = clip[3];
    float sz = c->ps_size;
    if (in->has_psize) sz = in->psize;
    if (c->point_scale && !pre) {
        float d = sqrtf(dot3(pv, pv));
        float den = c->ps_a + c->ps_b * d + c->ps_c * d * d;
        sz = c->vp_h * sz * (den > 0 ? sqrtf(1.0f / den) : 1.0f);
    }
    o->psize = sz;
}

static void vs_vertex(DrawCtx *c, uint32_t vi, uint32_t inst, SgVertex *o) {
    float in[D3DSH_VS_INPUTS][4];
    for (int r = 0; r < 16; r++) { in[r][0] = in[r][1] = in[r][2] = 0; in[r][3] = 1; }
    for (int k = 0; k < c->nvsin; k++) {
        float t[4];
        fetch_el(c, &c->decl->e[c->vsin[k].el], vi, inst, t);
        memcpy(in[c->vsin[k].reg], t, 16);
    }
    D3dVsOut out;
    memset(&out, 0, sizeof out);
    d3dsh_run_vs_ex(c->vs->sh, &c->vsctx, in, &out);
    memset(o, 0, sizeof *o);
    float clip[4] = { out.pos[0], out.pos[1], out.pos[2], out.pos[3] };
    if (!c->noshift) { clip[0] += clip[3] * c->shift_x; clip[1] -= clip[3] * c->shift_y; }
    o->x = clip[0]; o->y = clip[1]; o->z = clip[2]; o->w = clip[3];
    memcpy(o->v, out.v, sizeof o->v);
    o->psize = out.psize > 0 ? out.psize : c->ps_size;
}

// ---- fixed-function pixel stage -------------------------------------------------------------------------------------------------------------
typedef struct FfStage {
    int colorop, alphaop, carg[3], aarg[3], resultarg;
    const SgTexture *tex; SgSampler samp;
    int slot, count, proj, bump_signed;
    float bm[4], lscale, loffset, constant[4];
    int is_cube;
} FfStage;
typedef struct FfCtx {
    int nstages, fast;
    FfStage st[8];
    float tfactor[4];
    int specular;
    int fog_mode, fog_table;
    float fog_start, fog_end, fog_density, fog_color[3];
} FfCtx;

static float sraw(float b) { int v = (int)(b * 255.0f + 0.5f); if (v > 127) v -= 256; float f = (float)v / 127.0f; return f < -1 ? -1 : f; }

static void ff_arg(int arg, const float *tex, const float *cur, const float *tmp, const float *dif, const float *spc, const float *tf, const float *cst, float *o, int alpha_only) {
    const float *src;
    switch (arg & 15) {
    case 0: src = dif; break; case 1: src = cur; break; case 2: src = tex; break; case 3: src = tf; break;
    case 4: src = spc; break; case 5: src = tmp; break; case 6: src = cst; break; default: src = cur; break;
    }
    memcpy(o, src, 16);
    if (arg & 0x10) { o[0] = 1 - o[0]; o[1] = 1 - o[1]; o[2] = 1 - o[2]; o[3] = 1 - o[3]; }
    if ((arg & 0x20) && !alpha_only) o[0] = o[1] = o[2] = o[3];
}
static int arg_uses_tex(int a) { return (a & 15) == 2; }

// One colour or alpha operation on 3 arguments; a = arguments (rgba), result written to r (rgb for colour ops, a for alpha).
static void ff_op(int op, const float *a0, const float *a1, const float *a2, const float *tex, const float *cur, const float *dif, const float *tf, int alpha, float *r) {
    int n = alpha ? 1 : 3, o = alpha ? 3 : 0;
    float bf;
    for (int i = 0; i < n; i++) {
        float x0 = a0[o + i], x1 = a1[o + i], x2 = a2[o + i], v = x1;
        switch (op) {
        case 2: v = x1; break; case 3: v = x2; break;
        case 4: v = x1 * x2; break; case 5: v = x1 * x2 * 2; break; case 6: v = x1 * x2 * 4; break;
        case 7: v = x1 + x2; break; case 8: v = x1 + x2 - 0.5f; break; case 9: v = (x1 + x2 - 0.5f) * 2; break;
        case 10: v = x1 - x2; break; case 11: v = x1 + x2 - x1 * x2; break;
        case 12: bf = dif[3]; v = x1 * bf + x2 * (1 - bf); break;
        case 13: bf = tex[3]; v = x1 * bf + x2 * (1 - bf); break;
        case 14: bf = tf[3]; v = x1 * bf + x2 * (1 - bf); break;
        case 15: v = x1 + x2 * (1 - tex[3]); break;
        case 16: bf = cur[3]; v = x1 * bf + x2 * (1 - bf); break;
        case 17: v = x1; break;
        case 18: v = alpha ? x1 : x1 + a1[3] * x2; break;
        case 19: v = alpha ? x1 : x1 * x2 + a1[3]; break;
        case 20: v = alpha ? x1 : (1 - a1[3]) * x2 + x1; break;
        case 21: v = alpha ? x1 : (1 - x1) * x2 + a1[3]; break;
        case 25: v = x0 + x1 * x2; break;
        case 26: v = x0 * x1 + (1 - x0) * x2; break;
        default: v = x1; break;
        }
        r[i] = sat(v);
    }
}

static int ff_pixel(void *vctx, const SgPixelIn *in, float out[4], float *depth) {
    const FfCtx *c = vctx;
    const float *dif = in->v[8], *spc = in->v[9];
    float cur[4] = { in->v[8][0], in->v[8][1], in->v[8][2], in->v[8][3] };
    float tmp[4] = { 0, 0, 0, 0 };
    float pert[2] = { 0, 0 }, lum = 1;
    int have_pert = 0, have_lum = 0;
    for (int s = 0; s < c->nstages; s++) {
        const FfStage *S = &c->st[s];
        float T[4] = { 0, 0, 0, 1 };
        int need_tex = arg_uses_tex(S->carg[0]) || arg_uses_tex(S->carg[1]) || arg_uses_tex(S->carg[2]) || arg_uses_tex(S->aarg[0]) || arg_uses_tex(S->aarg[1]) || arg_uses_tex(S->aarg[2]) ||
                       S->colorop == 13 || S->colorop == 15 || S->alphaop == 13 || S->alphaop == 15 || S->colorop == 22 || S->colorop == 23;
        if (need_tex && S->tex) {
            float coord[4], dx[4], dy[4];
            const float *v = in->v[S->slot], *vx = in->dx[S->slot], *vy = in->dy[S->slot];
            if (S->proj && S->count >= 2 && !S->is_cube) {
                float q = v[S->count - 1], dqx = vx[S->count - 1], dqy = vy[S->count - 1];
                float iq = q != 0 ? 1.0f / q : 1.0f;
                for (int k = 0; k < 4; k++) {
                    float cv = v[k] * iq;
                    coord[k] = cv;
                    dx[k] = (vx[k] - cv * dqx) * iq;
                    dy[k] = (vy[k] - cv * dqy) * iq;
                }
            } else { for (int k = 0; k < 4; k++) { coord[k] = v[k]; dx[k] = vx[k]; dy[k] = vy[k]; } }
            if (have_pert) { coord[0] += pert[0]; coord[1] += pert[1]; }
            sg_sample(S->tex, &S->samp, coord, dx, dy, NAN, T);
            if (S->bump_signed && (S->colorop == 22 || S->colorop == 23)) {
                float du = sraw(T[0]), dv = sraw(T[1]);
                // the perturbation applies to the next stage
                (void)du; (void)dv;
            }
        } else if (need_tex && !S->tex) break;
        if (have_lum) { /* luminance of the previous bump stage scales this stage's texture */
            T[0] *= lum; T[1] *= lum; T[2] *= lum;
        }
        have_pert = 0; have_lum = 0;
        if (S->colorop == 22 || S->colorop == 23) {
            float du = sraw(T[0]), dv = sraw(T[1]);
            pert[0] = S->bm[0] * du + S->bm[2] * dv;
            pert[1] = S->bm[1] * du + S->bm[3] * dv;
            have_pert = 1;
            if (S->colorop == 23) { lum = sat(T[2] * S->lscale + S->loffset); have_lum = 1; }
            continue;
        }
        float a0[4], a1[4], a2[4], res[4];
        memcpy(res, cur, 16);
        if (S->resultarg == 5) memcpy(res, tmp, 16);
        float rc[4] = { res[0], res[1], res[2], res[3] };
        ff_arg(S->carg[0], T, cur, tmp, dif, spc, c->tfactor, S->constant, a0, 0);
        ff_arg(S->carg[1], T, cur, tmp, dif, spc, c->tfactor, S->constant, a1, 0);
        ff_arg(S->carg[2], T, cur, tmp, dif, spc, c->tfactor, S->constant, a2, 0);
        if (S->colorop == 24) {
            float d = (a1[0] * 2 - 1) * (a2[0] * 2 - 1) + (a1[1] * 2 - 1) * (a2[1] * 2 - 1) + (a1[2] * 2 - 1) * (a2[2] * 2 - 1);
            d = sat(d);
            rc[0] = rc[1] = rc[2] = rc[3] = d;
        } else {
            ff_op(S->colorop, a0, a1, a2, T, cur, dif, c->tfactor, 0, rc);
            if (S->alphaop == 1) rc[3] = cur[3];
            else {
                float b0[4], b1[4], b2[4];
                ff_arg(S->aarg[0], T, cur, tmp, dif, spc, c->tfactor, S->constant, b0, 1);
                ff_arg(S->aarg[1], T, cur, tmp, dif, spc, c->tfactor, S->constant, b1, 1);
                ff_arg(S->aarg[2], T, cur, tmp, dif, spc, c->tfactor, S->constant, b2, 1);
                ff_op(S->alphaop, b0, b1, b2, T, cur, dif, c->tfactor, 1, &rc[3]);
            }
        }
        if (S->resultarg == 5) memcpy(tmp, rc, 16); else memcpy(cur, rc, 16);
    }
    out[0] = cur[0]; out[1] = cur[1]; out[2] = cur[2]; out[3] = cur[3];
    if (c->specular) { out[0] = sat(out[0] + spc[0]); out[1] = sat(out[1] + spc[1]); out[2] = sat(out[2] + spc[2]); }
    if (c->fog_mode) {
        float f = c->fog_table ? fog_factor(c->fog_table, in->pos[2], c->fog_start, c->fog_end, c->fog_density) : sat(in->v[10][0]);
        for (int k = 0; k < 3; k++) out[k] = out[k] * f + c->fog_color[k] * (1 - f);
    }
    (void)depth;
    return 1;
}

// Single stage MODULATE(texture, diffuse) with texture alpha or modulated alpha.
static int ff_pixel_fast(void *vctx, const SgPixelIn *in, float out[4], float *depth) {
    const FfCtx *c = vctx;
    const FfStage *S = &c->st[0];
    float T[4], coord[4], dx[4], dy[4];
    const float *v = in->v[S->slot], *vx = in->dx[S->slot], *vy = in->dy[S->slot];
    if (S->proj && S->count >= 2) {
        float q = v[S->count - 1], iq = q != 0 ? 1.0f / q : 1.0f;
        for (int k = 0; k < 4; k++) { coord[k] = v[k] * iq; dx[k] = (vx[k] - coord[k] * vx[S->count - 1]) * iq; dy[k] = (vy[k] - coord[k] * vy[S->count - 1]) * iq; }
    } else for (int k = 0; k < 4; k++) { coord[k] = v[k]; dx[k] = vx[k]; dy[k] = vy[k]; }
    sg_sample(S->tex, &S->samp, coord, dx, dy, NAN, T);
    const float *d = in->v[8];
    out[0] = T[0] * d[0]; out[1] = T[1] * d[1]; out[2] = T[2] * d[2];
    out[3] = S->alphaop == 4 ? T[3] * d[3] : T[3];
    if (c->specular) { out[0] = sat(out[0] + in->v[9][0]); out[1] = sat(out[1] + in->v[9][1]); out[2] = sat(out[2] + in->v[9][2]); }
    if (c->fog_mode) {
        float f = c->fog_table ? fog_factor(c->fog_table, in->pos[2], c->fog_start, c->fog_end, c->fog_density) : sat(in->v[10][0]);
        for (int k = 0; k < 3; k++) out[k] = out[k] * f + c->fog_color[k] * (1 - f);
    }
    (void)depth;
    return 1;
}

// Pixel shader with the fixed-function fog applied afterwards (ps < 3.0).
typedef struct PsFog { D3dPsCtx *ps; FfCtx ff; } PsFog;
static int ps_fog_pixel(void *vctx, const SgPixelIn *in, float out[4], float *depth) {
    PsFog *p = vctx;
    if (!d3dsh_ps_pixel(p->ps, in, out, depth)) return 0;
    const FfCtx *c = &p->ff;
    float f = c->fog_table ? fog_factor(c->fog_table, in->pos[2], c->fog_start, c->fog_end, c->fog_density) : sat(in->v[10][0]);
    for (int k = 0; k < 3; k++) out[k] = out[k] * f + c->fog_color[k] * (1 - f);
    return 1;
}

// ---- sampler state -> SgSampler -------------------------------------------------------------------------------------------------------------------
static void make_sampler(const D9State *S, int idx, SgSampler *o) {
    const uint32_t *s = S->ss[idx];
    memset(o, 0, sizeof *o);
    int mag = (int)s[5], min = (int)s[6], mip = (int)s[7];
    o->mag_filter = mag == 0 ? SG_FILTER_POINT : mag == 1 ? SG_FILTER_POINT : SG_FILTER_LINEAR;
    o->min_filter = min == 0 ? SG_FILTER_POINT : min == 1 ? SG_FILTER_POINT : SG_FILTER_LINEAR;
    o->mip_filter = mip == 0 ? SG_FILTER_NONE : mip == 1 ? SG_FILTER_POINT : SG_FILTER_LINEAR;
    int a[3] = { (int)s[1], (int)s[2], (int)s[3] };
    int *dst[3] = { &o->address_u, &o->address_v, &o->address_w };
    for (int i = 0; i < 3; i++) *dst[i] = a[i] >= 1 && a[i] <= 5 ? a[i] - 1 : SG_ADDR_WRAP;
    o->border_argb = s[4];
    o->lod_bias = u2f(s[8]);
    o->max_mip_level = (int)s[9];
    o->max_anisotropy = s[10] > 1 ? (int)s[10] : 1;
    if (min == 3 || mag == 3) { if (o->max_anisotropy < 2) o->max_anisotropy = 2; } else o->max_anisotropy = 1;
    o->srgb = s[11] != 0;
}
static const SgTexture *bound_tex(const D9State *S, int idx) {
    D9Obj *o = S->tex[idx];
    if (!o || o->type != OT_TEX) return NULL;
    return d9_tex_prepare((D9Tex *)o);
}
static int tex_fmt_bump(const D9State *S, int idx) { D9Obj *o = S->tex[idx]; return o && o->type == OT_TEX && d9_fmt_is_bump(((D9Tex *)o)->fmt); }

static void build_ff_ps(DrawCtx *dc, FfCtx *f) {
    D9State *S = dc->S;
    memset(f, 0, sizeof *f);
    col_to_f(S->rs[60], f->tfactor);
    f->specular = S->rs[29] != 0 && !dc->pretrans ? 1 : (S->rs[29] != 0);
    int fogen = S->rs[28] != 0;
    float fc[4];
    col_to_f(S->rs[34], fc);
    memcpy(f->fog_color, fc, 12);
    f->fog_start = rsf(S, 36); f->fog_end = rsf(S, 37); f->fog_density = rsf(S, 38);
    if (fogen) { f->fog_mode = 1; f->fog_table = (int)S->rs[35]; if (!f->fog_table && !S->rs[140] && !dc->pretrans) f->fog_mode = 0; }
    int n = 0;
    for (int s = 0; s < 8; s++) {
        const uint32_t *t = S->tss[s];
        if (t[1] == 1) break;
        FfStage *st = &f->st[n++];
        st->colorop = (int)t[1]; st->alphaop = (int)t[4];
        st->carg[0] = (int)t[26]; st->carg[1] = (int)t[2]; st->carg[2] = (int)t[3];
        st->aarg[0] = (int)t[27]; st->aarg[1] = (int)t[5]; st->aarg[2] = (int)t[6];
        st->resultarg = (int)t[28];
        st->slot = s;
        st->count = (int)(t[24] & 0xFF);
        st->proj = (t[24] & 256) != 0;
        col_to_f(t[32], st->constant);
        for (int k = 0; k < 4; k++) st->bm[k] = u2f(t[7 + k]);
        st->lscale = u2f(t[22]); st->loffset = u2f(t[23]);
        st->tex = bound_tex(S, s);
        st->is_cube = st->tex && st->tex->kind == SG_TEX_CUBE;
        st->bump_signed = tex_fmt_bump(S, s);
        make_sampler(S, s, &st->samp);
        if (!st->count && st->proj) st->proj = 0;
        // ops with fewer arguments: the unused ones default sanely
    }
    f->nstages = n;
    // fast path: one stage, MODULATE texture * diffuse (alpha from texture or modulated)
    if (n == 1) {
        const FfStage *s = &f->st[0];
        int c1 = (s->carg[1] & 15) == 0 || (s->carg[1] & 15) == 1;
        if (s->tex && s->colorop == 4 && s->carg[0] != 99 && s->carg[1] == (int)S->tss[0][2] && (s->carg[1] & 0x30) == 0 && s->carg[2] == (int)S->tss[0][3] && (s->carg[2] & 0x30) == 0 &&
            (S->tss[0][2] & 0x3F) == 2 && ((S->tss[0][3] & 0x3F) == 1 || (S->tss[0][3] & 0x3F) == 0) && c1 == 0) {
            (void)c1;
        }
        int a1 = (int)S->tss[0][2], a2 = (int)S->tss[0][3];
        int cok = a1 == 2 && (a2 == 1 || a2 == 0);
        int aok = (s->alphaop == 2 && S->tss[0][5] == 2) || (s->alphaop == 4 && S->tss[0][5] == 2 && (S->tss[0][6] == 1 || S->tss[0][6] == 0));
        if (s->tex && s->colorop == 4 && cok && aok && s->resultarg == 1 && !s->is_cube && !s->bump_signed) f->fast = 1;
    }
}

// ---- state -> SgState -------------------------------------------------------------------------------------------------------------------------------------
static int blend_f(uint32_t v, int *both_inv) {
    *both_inv = 0;
    switch (v) {
    case 1: return SG_ZERO; case 2: return SG_ONE; case 3: return SG_SRC_COLOR; case 4: return SG_ONE_MINUS_SRC_COLOR;
    case 5: return SG_SRC_ALPHA; case 6: return SG_ONE_MINUS_SRC_ALPHA; case 7: return SG_DST_ALPHA; case 8: return SG_ONE_MINUS_DST_ALPHA;
    case 9: return SG_DST_COLOR; case 10: return SG_ONE_MINUS_DST_COLOR; case 11: return SG_SRC_ALPHA_SATURATE;
    case 12: return SG_SRC_ALPHA; case 13: *both_inv = 1; return SG_ONE_MINUS_SRC_ALPHA;
    case 14: return SG_CONSTANT_COLOR; case 15: return SG_ONE_MINUS_CONSTANT_COLOR;
    }
    return SG_ONE;
}
static void stencil_face(SgStencilFace *f, const D9State *S, int ccw) {
    const uint32_t *r = S->rs;
    int b = ccw ? 186 : 53, fn = ccw ? 189 : 56;
    f->func = (int)r[fn] >= 1 && (int)r[fn] <= 8 ? (int)r[fn] - 1 : SG_ALWAYS;
    f->ref = (int)(r[57] & 255); f->read_mask = (int)(r[58] & 255); f->write_mask = (int)(r[59] & 255);
    f->fail_op = (int)r[b] - 1; f->zfail_op = (int)r[b + 1] - 1; f->zpass_op = (int)r[b + 2] - 1;
    if (f->fail_op < 0 || f->fail_op > 7) f->fail_op = SG_SOP_KEEP;
    if (f->zfail_op < 0 || f->zfail_op > 7) f->zfail_op = SG_SOP_KEEP;
    if (f->zpass_op < 0 || f->zpass_op > 7) f->zpass_op = SG_SOP_KEEP;
}
static void fill_state(DrawCtx *dc, SgState *s, const SgTarget *t) {
    D9State *S = dc->S;
    const uint32_t *r = S->rs;
    memset(s, 0, sizeof *s);
    s->vp_top_left = 1; s->clip_z_zero = 1;
    int vx = (int)S->vp.x, vy = (int)S->vp.y, vw = (int)S->vp.w, vh = (int)S->vp.h;
    s->vp_x = vx; s->vp_y = vy; s->vp_w = vw; s->vp_h = vh;
    s->depth_near = S->vp.minz; s->depth_far = S->vp.maxz;
    if (r[174]) { s->scissor = 1; s->sc_x = S->scissor[0]; s->sc_y = S->scissor[1]; s->sc_w = S->scissor[2] - S->scissor[0]; s->sc_h = S->scissor[3] - S->scissor[1]; if (s->sc_w <= 0 || s->sc_h <= 0) { s->sc_w = 0; s->sc_h = 0; } }
    s->depth_test = r[7] != 0 && t->depth != NULL;
    s->depth_write = s->depth_test && r[14] != 0;
    s->depth_func = r[23] >= 1 && r[23] <= 8 ? (int)r[23] - 1 : SG_LEQUAL;
    s->blend = r[27] != 0;
    int bi;
    s->blend_src = blend_f(r[19], &bi); if (r[19] == 13) s->blend_src = SG_ONE_MINUS_SRC_ALPHA;
    s->blend_dst = blend_f(r[20], &bi);
    if (r[19] == 12 || r[19] == 13) { s->blend_src = r[19] == 12 ? SG_SRC_ALPHA : SG_ONE_MINUS_SRC_ALPHA; s->blend_dst = r[19] == 12 ? SG_ONE_MINUS_SRC_ALPHA : SG_SRC_ALPHA; }
    s->blend_op = r[171] >= 1 && r[171] <= 5 ? (int)r[171] - 1 : SG_BLEND_ADD;
    s->blend_sep_alpha = r[206] != 0;
    s->blend_src_a = blend_f(r[207], &bi); s->blend_dst_a = blend_f(r[208], &bi);
    s->blend_op_a = r[209] >= 1 && r[209] <= 5 ? (int)r[209] - 1 : SG_BLEND_ADD;
    { float bc[4]; col_to_f(r[193], bc); s->blend_color[0] = bc[0]; s->blend_color[1] = bc[1]; s->blend_color[2] = bc[2]; s->blend_color[3] = bc[3]; }
    s->alpha_test = r[15] != 0;
    s->alpha_func = r[25] >= 1 && r[25] <= 8 ? (int)r[25] - 1 : SG_ALWAYS;
    s->alpha_ref = (float)(r[24] & 255) / 255.0f;
    s->cull = r[22] == 2 || r[22] == 3;
    s->cull_front = r[22] == 2;      // D3DCULL_CW culls clockwise triangles = the "front" ones (front_ccw = 0)
    s->cull_back = r[22] == 3;
    s->front_ccw = 0;
    s->mask_r = (r[168] & 1) != 0; s->mask_g = (r[168] & 2) != 0; s->mask_b = (r[168] & 4) != 0; s->mask_a = (r[168] & 8) != 0;
    s->flat_first = 1;
    s->flat_mask = r[9] == 1 ? ((1u << 8) | (1u << 9)) : 0;
    s->depth_bias = rsf(S, 195);
    s->slope_scale_bias = rsf(S, 175);
    if (r[52] && t->stencil) {
        s->stencil_test = 1;
        stencil_face(&s->stencil[0], S, 0);
        if (r[185]) { s->stencil_two_sided = 1; stencil_face(&s->stencil[1], S, 1); }
    }
    s->line_width = 1.0f;
    s->point_size_vertex = 1;
    s->point_size_min = rsf(S, 155);
    s->point_size_max = rsf(S, 166);
    if (s->point_size_max <= 0) s->point_size_max = 64.0f;
    s->point_sprite = r[156] != 0;
    s->sprite_slot = 0;
    // clip planes: world space for the fixed pipeline, clip space with shaders
    s->clip_mask = (int)(r[152] & 63);
    if (s->clip_mask) {
        float vp[16], inv[16];
        int ok = 0;
        if (!dc->use_vs) { m4_mul(vp, S->xf[2], S->xf[3]); ok = m4_inv(vp, inv); }
        for (int i = 0; i < 6; i++) {
            const float *p = S->clip[i];
            if (!dc->use_vs && ok) for (int j = 0; j < 4; j++) s->clip_plane[i][j] = inv[j * 4 + 0] * p[0] + inv[j * 4 + 1] * p[1] + inv[j * 4 + 2] * p[2] + inv[j * 4 + 3] * p[3];
            else memcpy(s->clip_plane[i], p, 16);
            // D3D clip volume 0 <= z <= w: planes are used unchanged
        }
    }
}

// ---- the draw ------------------------------------------------------------------------------------------------------------------------------------------------
static size_t prim_index_count(int type, uint32_t n) {
    switch (type) {
    case 1: return n; case 2: return (size_t)n * 2; case 3: return (size_t)n + 1;
    case 4: return (size_t)n * 3; case 5: case 6: return (size_t)n + 2;
    }
    return 0;
}

static int grow(void **p, size_t *cap, size_t need, size_t esz) {
    if (need <= *cap) return 1;
    size_t nc = *cap ? *cap : 1024;
    while (nc < need) nc *= 2;
    void *q = realloc(*p, nc * esz);
    if (!q) return 0;
    *p = q; *cap = nc;
    return 1;
}

static void setup_streams(DrawCtx *c, const void *up_vertices, uint32_t up_stride) {
    D9State *S = c->S;
    for (int i = 0; i < 16; i++) {
        c->sbase[i] = NULL; c->sstride[i] = 0; c->slimit[i] = 0; c->sfreq[i] = S->stream[i].freq;
        if (i == 0 && up_vertices) { c->sbase[0] = up_vertices; c->sstride[0] = up_stride; c->slimit[0] = (size_t)-1 / 2; c->sfreq[0] = 1; continue; }
        D9Buf *b = S->stream[i].vb;
        if (!b || S->stream[i].offset > b->size) continue;
        c->sbase[i] = b->data + S->stream[i].offset;
        c->sstride[i] = S->stream[i].stride;
        c->slimit[i] = b->size - S->stream[i].offset;
    }
}

static void setup_decl_maps(DrawCtx *c) {
    const D9Decl *d = c->decl;
    c->el_pos = c->el_nrm = c->el_psize = -1;
    c->el_col[0] = c->el_col[1] = -1;
    for (int k = 0; k < 8; k++) c->el_tex[k] = -1;
    for (int i = 0; i < d->n; i++) {
        const D9Elem *e = &d->e[i];
        if (e->usage == D3DSU_POSITION && e->index == 0 && c->el_pos < 0) c->el_pos = i;
        else if (e->usage == D3DSU_POSITIONT && e->index == 0) { c->el_pos = i; c->pretrans = 1; }
        else if (e->usage == D3DSU_NORMAL && e->index == 0) c->el_nrm = i;
        else if (e->usage == D3DSU_PSIZE && e->index == 0) c->el_psize = i;
        else if (e->usage == D3DSU_COLOR && e->index < 2) c->el_col[e->index] = i;
        else if (e->usage == D3DSU_TEXCOORD && e->index < 8) c->el_tex[e->index] = i;
    }
}

static void setup_vs_inputs(DrawCtx *c) {
    D3dShaderInput in[16];
    int n = d3dsh_vs_inputs(c->vs->sh, in, 16);
    c->nvsin = 0;
    for (int i = 0; i < c->decl->n; i++) {
        const D9Elem *e = &c->decl->e[i];
        int usage = e->usage == D3DSU_POSITIONT ? D3DSU_POSITION : e->usage;
        int reg = -1;
        if (n > 0) { for (int k = 0; k < n; k++) if (in[k].usage == usage && in[k].usage_index == e->index) { reg = in[k].reg; break; } }
        else reg = stock_reg(usage, e->index);
        if (reg < 0 || reg > 15) continue;
        c->vsin[c->nvsin].reg = reg; c->vsin[c->nvsin].el = i; c->nvsin++;
    }
}

uint32_t d9_draw(D9Dev *dev, int ptype, int base_vertex, uint32_t min_vertex, uint32_t num_vertices, uint32_t start_index, uint32_t prim_count,
                 int indexed, const void *up_vertices, uint32_t up_stride, const void *up_indices, int up_index32) {
    D9State *S = &dev->st;
    if (!dev->rt) return D3DERR_INVALIDCALL;
    if (ptype < 1 || ptype > 6) return D3DERR_INVALIDCALL;
    if (prim_count == 0) return D3D_OK;
    dev->draw_calls++;
    D9Decl *decl = S->decl;
    D9Shader *vs = S->vs && S->vs->sh ? S->vs : NULL;
    if (!decl) return D3DERR_INVALIDCALL;
    if (indexed && !up_indices && !S->ib) return D3DERR_INVALIDCALL;

    DrawCtx *c = calloc(1, sizeof *c);
    if (!c) return E_OUTOFMEMORY_;
    c->dev = dev; c->S = S; c->decl = decl; c->vs = vs; c->use_vs = vs != NULL;
    setup_decl_maps(c);
    if (c->use_vs) c->pretrans = 0;
    setup_streams(c, up_vertices, up_stride);
    SgTarget tg;
    d9_surf_target(dev, dev->rt, dev->ds, &tg);
    SgState sgs;
    fill_state(c, &sgs, &tg);
    c->vpx = (float)S->vp.x; c->vpy = (float)S->vp.y; c->vpw = (float)(S->vp.w ? S->vp.w : 1); c->vph = (float)(S->vp.h ? S->vp.h : 1);
    c->shift_x = 1.0f / c->vpw; c->shift_y = 1.0f / c->vph;
    c->vp_h = c->vph;
    c->ps_size = rsf(S, 154);

    // ---- vertex stage setup
    D9Shader *ps = S->ps && S->ps->sh ? S->ps : NULL;
    uint32_t ps_inmask = 0;
    if (c->use_vs) {
        setup_vs_inputs(c);
        c->vsctx.c = &S->vsc;
        for (int i = 0; i < D3DSH_VS_TEXTURES; i++) { c->vsctx.tex[i] = bound_tex(S, 17 + i); make_sampler(S, 17 + i, &c->vsctx.samp[i]); }
        setup_ff(c);       // fog parameters, point size
    } else {
        setup_ff(c);
        if (c->el_pos < 0) { free(c); return D3DERR_INVALIDCALL; }
        if (ps) ps_inmask = d3dsh_ps_input_mask(ps->sh);
        if (ps) c->tc_stage_mask = ps_inmask & 0xFF;
        else { for (int s = 0; s < 8; s++) { if (S->tss[s][1] == 1) break; c->tc_stage_mask |= 1u << s; } }
    }

    // ---- pixel stage setup
    FfCtx ff;
    D3dPsCtx *psc = NULL;
    PsFog *psf = NULL;
    if (ps) {
        psc = calloc(1, sizeof *psc);
        psc->ps = ps->sh; psc->c = &S->psc;
        for (int i = 0; i < D3DSH_MAX_SAMPLERS; i++) {
            psc->tex[i] = bound_tex(S, i);
            make_sampler(S, i, &psc->samp[i]);
            for (int k = 0; k < 4; k++) psc->bump_mat[i][k] = u2f(S->tss[i < 8 ? i : 0][7 + k]);
            psc->bump_lscale[i] = u2f(S->tss[i < 8 ? i : 0][22]); psc->bump_loffset[i] = u2f(S->tss[i < 8 ? i : 0][23]);
        }
        sgs.nvary = d3dsh_ps_nvary(ps->sh);
        sgs.pixel_writes_depth = d3dsh_ps_writes_depth(ps->sh);
        build_ff_ps(c, &ff);
        if (ff.fog_mode && d3dsh_major(ps->sh) < 3) {
            psf = calloc(1, sizeof *psf);
            psf->ps = psc; psf->ff = ff;
            if (sgs.nvary < 11) sgs.nvary = 11;
            sgs.pixel_fn = ps_fog_pixel; sgs.pixel_ctx = psf;
        } else { sgs.pixel_fn = d3dsh_ps_pixel; sgs.pixel_ctx = psc; }
    } else {
        build_ff_ps(c, &ff);
        sgs.nvary = ff.fog_mode ? 11 : 10;
        sgs.pixel_fn = ff.nstages == 0 ? ff_pixel : ff.fast ? ff_pixel_fast : ff_pixel;
        sgs.pixel_ctx = &ff;
        if (ff.nstages == 0) { /* no stages: diffuse (+ specular, fog) through the cascade with nothing active */ }
    }
    if (c->use_vs && !ps) { /* VS + fixed-function pixel stage: outputs come from the shader's varyings */ }
    if (c->use_vs && !ps) c->tc_stage_mask = 0;

    // ---- index list
    uint32_t nidx = (uint32_t)prim_index_count(ptype, prim_count);
    uint32_t ninst = 1;
    if ((S->stream[0].freq & (1u << 30)) && !up_vertices) { ninst = S->stream[0].freq & 0x3FFFFFFF; if (ninst == 0) ninst = 1; }
    uint32_t *list = malloc((size_t)nidx * 4 + 4);
    if (!list) { free(c); free(psc); free(psf); return E_OUTOFMEMORY_; }
    uint32_t vmin = 0xFFFFFFFFu, vmax = 0;
    const D9Buf *ib = S->ib;
    for (uint32_t i = 0; i < nidx; i++) {
        int64_t v;
        if (indexed) {
            uint32_t ix = start_index + i;
            if (up_indices) v = up_index32 ? ((const uint32_t *)up_indices)[ix] : ((const uint16_t *)up_indices)[ix];
            else {
                size_t esz = ib->fmt == F_INDEX32 ? 4 : 2;
                if ((size_t)(ix + 1) * esz > ib->size) v = 0;
                else v = esz == 4 ? ((const uint32_t *)(const void *)ib->data)[ix] : ((const uint16_t *)(const void *)ib->data)[ix];
            }
            v += base_vertex;
        } else v = (int64_t)start_index + i;
        if (v < 0) v = 0;
        list[i] = (uint32_t)v;
        if (list[i] < vmin) vmin = list[i];
        if (list[i] > vmax) vmax = list[i];
    }
    (void)min_vertex; (void)num_vertices;
    size_t range = (size_t)vmax - vmin + 1;
    int32_t *vmap = NULL;
    uint32_t *keep_list = list;
    (void)keep_list;
    if (range > (1u << 24)) { free(list); free(c); free(psc); free(psf); return D3DERR_INVALIDCALL; }
    vmap = malloc(range * sizeof(int32_t));
    SgVertex *pool = NULL; size_t pcap = 0, npool = 0;
    SgVertex *obuf = malloc(3 * 1024 * sizeof(SgVertex));
    if (!vmap || !obuf) { free(vmap); free(obuf); free(list); free(c); free(psc); free(psf); return E_OUTOFMEMORY_; }

    int fill = (int)S->rs[8];     // 1 point, 2 wireframe, 3 solid
    for (uint32_t inst = 0; inst < ninst; inst++) {
        for (size_t i = 0; i < range; i++) vmap[i] = -1;
        npool = 0;
        // process every referenced vertex (in order of first use)
        for (uint32_t i = 0; i < nidx; i++) {
            int32_t *m = &vmap[list[i] - vmin];
            if (*m >= 0) continue;
            if (!grow((void **)&pool, &pcap, npool + 1, sizeof(SgVertex))) break;
            if (c->use_vs) vs_vertex(c, list[i], inst, &pool[npool]);
            else { VIn vin; ff_fetch(c, list[i], inst, &vin); ff_vertex(c, &vin, &pool[npool]); }
            *m = (int32_t)npool++;
        }
        // emit primitives in chunks
        uint32_t cnt = 0;
        #define SLOT(k) (&pool[vmap[list[(k)] - vmin]])
        #define FLUSH_T() do { if (cnt) { if (fill == 3) sg_triangles(&tg, &sgs, obuf, (int)cnt); else if (fill == 2) { \
                for (uint32_t q = 0; q + 2 < cnt; q += 3) { SgVertex ln[6] = { obuf[q], obuf[q + 1], obuf[q + 1], obuf[q + 2], obuf[q + 2], obuf[q] }; sg_lines(&tg, &sgs, ln, 6); } \
            } else sg_points(&tg, &sgs, obuf, (int)cnt); cnt = 0; } } while (0)
        switch (ptype) {
        case 1: for (uint32_t i = 0; i < nidx; i++) { obuf[cnt++] = *SLOT(i); if (cnt == 3072) { sg_points(&tg, &sgs, obuf, (int)cnt); cnt = 0; } } if (cnt) sg_points(&tg, &sgs, obuf, (int)cnt); break;
        case 2: for (uint32_t i = 0; i + 1 < nidx; i += 2) { obuf[cnt++] = *SLOT(i); obuf[cnt++] = *SLOT(i + 1); if (cnt >= 3070) { sg_lines(&tg, &sgs, obuf, (int)cnt); cnt = 0; } } if (cnt) sg_lines(&tg, &sgs, obuf, (int)cnt); break;
        case 3: for (uint32_t i = 0; i + 1 < nidx; i++) { obuf[cnt++] = *SLOT(i); obuf[cnt++] = *SLOT(i + 1); if (cnt >= 3070) { sg_lines(&tg, &sgs, obuf, (int)cnt); cnt = 0; } } if (cnt) sg_lines(&tg, &sgs, obuf, (int)cnt); break;
        case 4: for (uint32_t i = 0; i + 2 < nidx; i += 3) { obuf[cnt++] = *SLOT(i); obuf[cnt++] = *SLOT(i + 1); obuf[cnt++] = *SLOT(i + 2); if (cnt >= 3069) FLUSH_T(); } FLUSH_T(); break;
        case 5: for (uint32_t i = 0; i + 2 < nidx; i++) {
                    if (i & 1) { obuf[cnt++] = *SLOT(i); obuf[cnt++] = *SLOT(i + 2); obuf[cnt++] = *SLOT(i + 1); }
                    else { obuf[cnt++] = *SLOT(i); obuf[cnt++] = *SLOT(i + 1); obuf[cnt++] = *SLOT(i + 2); }
                    if (cnt >= 3069) FLUSH_T();
                } FLUSH_T(); break;
        case 6: for (uint32_t i = 1; i + 1 < nidx; i++) { obuf[cnt++] = *SLOT(0); obuf[cnt++] = *SLOT(i); obuf[cnt++] = *SLOT(i + 1); if (cnt >= 3069) FLUSH_T(); } FLUSH_T(); break;
        }
        #undef SLOT
        #undef FLUSH_T
    }
    d9_surf_touch(dev->rt);
    free(pool); free(obuf); free(vmap); free(list); free(psc); free(psf); free(c);
    return D3D_OK;
}

// ProcessVertices: runs the vertex pipeline and writes the results (screen space with the destination layout).
uint32_t d9_process_vertices(D9Dev *dev, uint32_t src_start, uint32_t dest_index, uint32_t count, D9Buf *dst, D9Decl *ddecl, uint32_t flags) {
    (void)flags;
    D9State *S = &dev->st;
    if (!dst || !S->decl || count == 0) return D3DERR_INVALIDCALL;
    D9Decl *od = ddecl;
    int own = 0;
    if (!od && dst->fvf) { od = d9_decl_from_fvf(dev, dst->fvf); own = 1; }
    if (!od) return D3DERR_INVALIDCALL;
    D9Shader *vs = S->vs && S->vs->sh ? S->vs : NULL;
    DrawCtx *c = calloc(1, sizeof *c);
    c->dev = dev; c->S = S; c->decl = S->decl; c->vs = vs; c->use_vs = vs != NULL;
    setup_decl_maps(c);
    if (c->use_vs) c->pretrans = 0;
    setup_streams(c, NULL, 0);
    c->vpx = (float)S->vp.x; c->vpy = (float)S->vp.y; c->vpw = (float)(S->vp.w ? S->vp.w : 1); c->vph = (float)(S->vp.h ? S->vp.h : 1);
    c->vp_h = c->vph; c->noshift = 1;
    setup_ff(c);
    if (c->use_vs) {
        setup_vs_inputs(c);
        c->vsctx.c = &S->vsc;
        for (int i = 0; i < D3DSH_VS_TEXTURES; i++) { c->vsctx.tex[i] = bound_tex(S, 17 + i); make_sampler(S, 17 + i, &c->vsctx.samp[i]); }
    } else for (int s = 0; s < 8; s++) { if (S->tss[s][1] == 1) break; c->tc_stage_mask |= 1u << s; }
    int ds = d9_decl_size(od->e, od->n, 0);
    for (uint32_t i = 0; i < count; i++) {
        SgVertex o;
        if (c->use_vs) vs_vertex(c, src_start + i, 0, &o);
        else { VIn vin; ff_fetch(c, src_start + i, 0, &vin); ff_vertex(c, &vin, &o); }
        size_t base = (size_t)(dest_index + i) * (size_t)ds;
        if (base + (size_t)ds > dst->size) break;
        float rhw = o.w != 0 ? 1.0f / o.w : 1.0f;
        float sx = c->vpx + (o.x * rhw + 1.0f) * 0.5f * c->vpw, sy = c->vpy + (1.0f - o.y * rhw) * 0.5f * c->vph, sz = o.z * rhw;
        for (int k = 0; k < od->n; k++) {
            const D9Elem *e = &od->e[k];
            if (e->stream != 0 || e->type > 16) continue;
            uint8_t *p = dst->data + base + e->offset;
            if (e->offset + k_tsize[e->type] > ds) continue;
            if (e->usage == D3DSU_POSITIONT || e->usage == D3DSU_POSITION) { float f[4] = { sx, sy, sz, rhw }; memcpy(p, f, e->type == 2 ? 12 : 16); }
            else if (e->usage == D3DSU_COLOR && e->type == 4 && e->index < 2) {
                const float *cv = o.v[8 + e->index];
                uint32_t col = ((uint32_t)(sat(cv[3]) * 255.0f + 0.5f) << 24) | ((uint32_t)(sat(cv[0]) * 255.0f + 0.5f) << 16) | ((uint32_t)(sat(cv[1]) * 255.0f + 0.5f) << 8) | (uint32_t)(sat(cv[2]) * 255.0f + 0.5f);
                memcpy(p, &col, 4);
            } else if (e->usage == D3DSU_TEXCOORD && e->index < 8 && e->type <= 3) memcpy(p, o.v[e->index], (size_t)k_tsize[e->type]);
        }
    }
    free(c);
    if (own) { /* cached in the device's FVF table when created through SetFVF; otherwise released here */ if (od->o.refs == 0 && od->o.irefs == 0) d9_destroy(&od->o); }
    return D3D_OK;
}
