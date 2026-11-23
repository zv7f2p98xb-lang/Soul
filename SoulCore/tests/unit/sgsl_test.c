// Unit test for the shader translator (src/sgsl*.c): for many configurations the C rendering of the generated shader is
// compiled with gcc at test time, loaded with dlopen and compared with the CPU implementations
//   * GL fixed function pixel stage  (sgpu.c: tex_combine / sample / fog / alpha test; included below so its statics are usable),
//   * D3D9 texture stage cascade     (d3d9_draw.c ff_pixel: verbatim copy below, the original is static in a large file),
//   * D3D9 pixel shaders             (d3dsh_ps_pixel, the interpreter itself, with the ps<3 fog wrapper of d3d9_draw.c).
// Every configuration's MSL text is also written to build/unit/sgsl_msl/*.metal (+ index.txt) for the macOS CI step
// (`xcrun metal -c`), and checked here for structure.
#include "../../src/sgpu.c"      // CPU rasteriser (statics: sample, tex_combine, depth_pass)
#include "sgsl.h"
#include "d3d9_shader.h"
#include "shaders/d3dsh_shaders.h"
enum { NTAB_U = SH_TABLE_N };
#undef SH_TABLE_N
#define SH_TABLE SH_TABLE_W
#define D3dshTab D3dshTabW
#include "../win32/d3d9_shaders.h"
enum { NTAB_W = SH_TABLE_N };
#undef SH_TABLE
#undef SH_TABLE_N
#undef D3dshTab
#include <ctype.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int g_pass, g_fail;
static void report(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_pass++; else g_fail++;
}

// ---- random numbers -----------------------------------------------------------------------------------------------------
static uint64_t g_rs = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (uint32_t)(g_rs >> 16); }
static float rf(float lo, float hi) { return lo + (hi - lo) * ((float)(rnd() & 0xFFFFFF) / 16777216.0f); }
static float sat01f(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }

// ---- textures ----------------------------------------------------------------------------------------------------------------
static SgTexture POOL2D[2], POOLCUBE, POOLVOL;
static SgTexture tex_make(int kind, int w, int h, int d, int build_mips) {
    SgTexture t; memset(&t, 0, sizeof t);
    t.kind = kind; t.w = w; t.h = h; t.depth = d; t.levels = 1; t.format = SG_FMT_RGBA;
    size_t n = (size_t)w * h * (kind == SG_TEX_VOLUME ? d : 1) * (kind == SG_TEX_CUBE ? 6 : 1) * 4;
    t.rgba = malloc(n);
    for (size_t i = 0; i < n; i++) t.rgba[i] = (uint8_t)rnd();
    t.mip[0] = t.rgba; t.mip_w[0] = w; t.mip_h[0] = h; t.mip_d[0] = d;
    if (build_mips) { sg_texture_build_mips(&t); t.use_mips = 1; }
    return t;
}
static void pool_init(void) {
    POOL2D[0] = tex_make(SG_TEX_2D, 8, 8, 1, 1);
    POOL2D[1] = tex_make(SG_TEX_2D, 4, 4, 1, 0);
    POOLCUBE = tex_make(SG_TEX_CUBE, 4, 4, 1, 1);
    POOLVOL = tex_make(SG_TEX_VOLUME, 4, 4, 4, 1);
}
static const SgTexture *pool_tex(int kind, int variant) {
    return kind == SG_TEX_CUBE ? &POOLCUBE : kind == SG_TEX_VOLUME ? &POOLVOL : &POOL2D[variant & 1];
}
static void rand_sampler(SgSampler *s) {
    memset(s, 0, sizeof *s);
    s->min_filter = rnd() & 1 ? SG_FILTER_LINEAR : SG_FILTER_POINT;
    s->mag_filter = rnd() & 1 ? SG_FILTER_LINEAR : SG_FILTER_POINT;
    s->mip_filter = (int)(rnd() % 3);
    s->address_u = (int)(rnd() % 4); s->address_v = (int)(rnd() % 4); s->address_w = (int)(rnd() % 4);
    s->border_argb = rnd();
    static const float bias[] = { 0, 0, 0.5f, -0.5f, 1.0f };
    s->lod_bias = bias[rnd() % 5];
    s->max_mip_level = rnd() % 5 == 0 ? 1 : 0;
    s->max_anisotropy = rnd() % 4 == 0 ? 4 : 1;
    s->srgb = rnd() % 5 == 0;
}

// ---- reference: D3D9 texture stage cascade (verbatim from d3d9_draw.c) -----------------------------------------------------
static float fog_factor(int mode, float d, float start, float end, float density) {
    float f = 1;
    switch (mode) {
    case 1: f = expf(-d * density); break;
    case 2: f = expf(-(d * density) * (d * density)); break;
    case 3: f = end != start ? (end - d) / (end - start) : (d < start ? 1.0f : 0.0f); break;
    }
    return sat01f(f);
}
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
        r[i] = sat01f(v);
    }
}
static int ff_pixel(const FfCtx *c, const SgPixelIn *in, float out[4]) {
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
        } else if (need_tex && !S->tex) break;
        if (have_lum) { T[0] *= lum; T[1] *= lum; T[2] *= lum; }
        have_pert = 0; have_lum = 0;
        if (S->colorop == 22 || S->colorop == 23) {
            float du = sraw(T[0]), dv = sraw(T[1]);
            pert[0] = S->bm[0] * du + S->bm[2] * dv;
            pert[1] = S->bm[1] * du + S->bm[3] * dv;
            have_pert = 1;
            if (S->colorop == 23) { lum = sat01f(T[2] * S->lscale + S->loffset); have_lum = 1; }
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
            d = sat01f(d);
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
    if (c->specular) { out[0] = sat01f(out[0] + spc[0]); out[1] = sat01f(out[1] + spc[1]); out[2] = sat01f(out[2] + spc[2]); }
    if (c->fog_mode) {
        float f = c->fog_table ? fog_factor(c->fog_table, in->pos[2], c->fog_start, c->fog_end, c->fog_density) : sat01f(in->v[10][0]);
        for (int k = 0; k < 3; k++) out[k] = out[k] * f + c->fog_color[k] * (1 - f);
    }
    (void)c;
    return 1;
}

// ---- configurations ---------------------------------------------------------------------------------------------------------------
enum { K_GL, K_TSS, K_PS };
typedef int (*FsFn)(const SgslCEnv *, const SgslFragIn *, const SgslUniforms *, float *, float *);
typedef void (*VsFn)(unsigned, const SgVertex *, const SgslUniforms *, SgslVSOut *);
typedef struct Cfg {
    char name[96];
    int kind, prim;
    SgState s;
    SgProgram prog;
    D3dShaderConsts consts;
    SgTexture glt[SG_UNITS];
    D3dShader *sh;
    D3dPsCtx pc;
    FfCtx ff;
    SgslCEnv env;
    SgslInfo info;
    char *csrc, *msrc;
    char err[256];
    int null_result;
    void *dl;
    FsFn fs; VsFn vs;
    int sprite;
    float sprite_size;
    int ndefi;
} Cfg;
static Cfg **g_cfg;
static int g_ncfg, g_capcfg;

static Cfg *cfg_new(const char *fmt, ...) {
    Cfg *c = calloc(1, sizeof *c);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(c->name, sizeof c->name, fmt, ap);
    va_end(ap);
    for (char *p = c->name; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) *p = '_';
    if (g_ncfg == g_capcfg) { g_capcfg = g_capcfg ? g_capcfg * 2 : 256; g_cfg = realloc(g_cfg, sizeof *g_cfg * (size_t)g_capcfg); }
    g_cfg[g_ncfg++] = c;
    c->prim = SG_PRIM_TRIANGLES;
    return c;
}

static void state_common(Cfg *c) {
    SgState *s = &c->s;
    s->vp_top_left = 1; s->clip_z_zero = 1;
    s->depth_near = 0.1f; s->depth_far = 0.9f;
    s->alpha_ref = 0.4f;
}
static void maybe_alpha(Cfg *c, int salt) {
    if (salt % 3 == 0) { c->s.alpha_test = 1; c->s.alpha_func = (int)(rnd() % 8); c->s.alpha_ref = rf(0.2f, 0.8f); }
}

// GL fixed function
static void make_gl(int env0, int fmt0, int env1, int fmt1, int two, int fog, int alpha_func, int flat, int sprite) {
    Cfg *c = cfg_new("gl_e%d_f%d%s_e%d_f%d_fog%d_a%d_flat%d_spr%d", env0, fmt0, two ? "" : "x", two ? env1 : 0, two ? fmt1 : 0, fog, alpha_func, flat, sprite);
    c->kind = K_GL;
    SgState *s = &c->s;
    s->fog = fog; s->flat = flat;
    s->fog_color[0] = rf(0, 1); s->fog_color[1] = rf(0, 1); s->fog_color[2] = rf(0, 1); s->fog_color[3] = 1;
    if (alpha_func >= 0) { s->alpha_test = 1; s->alpha_func = alpha_func; s->alpha_ref = rf(0.2f, 0.8f); }
    for (int u = 0; u < (two ? 2 : 1); u++) {
        SgTexture *t = &c->glt[u];
        *t = tex_make(SG_TEX_2D, u ? 4 : 8, u ? 8 : 4, 1, u == 0 && (rnd() & 1));
        t->format = u ? fmt1 : fmt0;
        t->min_linear = (int)(rnd() & 1); t->mag_linear = (int)(rnd() & 1); t->clamp_s = (int)(rnd() & 1); t->clamp_t = (int)(rnd() & 1);
        s->tex_on[u] = 1; s->tex[u] = t; s->tex_env[u] = u ? env1 : env0;
        for (int k = 0; k < 4; k++) s->env_color[u][k] = rf(0, 1);
        c->env.tex[u] = t;
    }
    if (sprite) { s->point_sprite = 1; c->prim = SG_PRIM_POINTS; c->sprite = 1; c->sprite_size = rf(2, 9); }
    state_common(c);
    s->alpha_ref = s->alpha_test ? s->alpha_ref : 0.4f;
}

// D3D9 texture stages
static void make_tss(const char *tag, int nst, const int ops[][2], const int args[][6], const int *results, int specular, int fog, int tex_mask, int proj) {
    Cfg *c = cfg_new("tss_%s", tag);
    c->kind = K_TSS;
    SgProgram *p = &c->prog;
    p->kind = SGP_D3D9_TSS;
    SgpTss *t = &p->tss;
    t->nstages = nst; t->specular = specular;
    for (int k = 0; k < 4; k++) t->tfactor[k] = rf(0, 1);
    for (int i = 0; i < nst; i++) {
        SgpTssStage *st = &t->stage[i];
        st->colorop = ops[i][0]; st->alphaop = ops[i][1];
        for (int k = 0; k < 3; k++) { st->color_arg[k] = args[i][k]; st->alpha_arg[k] = args[i][3 + k]; }
        st->result_arg = results[i];
        st->slot = i;
        st->ncoord = proj ? 2 + (int)(rnd() % 3) : (int)(rnd() % 3 ? 2 : 0);
        st->projected = proj && st->ncoord >= 2;
        if (!st->ncoord) st->projected = 0;
        for (int k = 0; k < 4; k++) { st->bump_mat[k] = rf(-0.5f, 0.5f); st->constant[k] = rf(0, 1); }
        st->bump_lscale = rf(0.5f, 2); st->bump_loffset = rf(-0.3f, 0.3f);
        if (tex_mask & (1 << i)) {
            int kind = (rnd() % 7 == 0) ? SG_TEX_CUBE : (rnd() % 9 == 0 ? SG_TEX_VOLUME : SG_TEX_2D);
            p->samplers[i].tex = pool_tex(kind, (int)rnd());
            rand_sampler(&p->samplers[i].samp);
            st->tex_kind = kind;
            c->env.tex[i] = p->samplers[i].tex;
        } else st->tex_kind = -1;
    }
    p->nsamplers = nst;
    if (fog) {
        p->fog.enable = 1; p->fog.table = fog - 1;
        p->fog.start = rf(0, 0.4f); p->fog.end = rf(0.5f, 1); p->fog.density = rf(0.5f, 3);
        for (int k = 0; k < 3; k++) p->fog.color[k] = rf(0, 1);
    }
    c->s.program = p; c->s.nvary = 11;
    p->nvary = 11;
    state_common(c);
    maybe_alpha(c, (int)rnd());
}

// D3D9 pixel shader
enum { PV_CUBE = 1, PV_VOL = 2, PV_UNBOUND = 4, PV_DCLAMP = 8, PV_FOG1 = 16, PV_FOG2 = 32, PV_FOG3 = 64, PV_FOGV = 128, PV_ALPHA = 256, PV_SPRITE = 512, PV_DUP = 4096 };
static void make_ps(const char *name, const uint32_t *t, unsigned n, int variant) {
    char err[200];
    D3dShader *sh = d3dsh_create(t, n, err, sizeof err);
    if (!sh) { printf("FAIL create %s: %s\n", name, err); g_fail++; return; }
    if (!d3dsh_is_pixel(sh)) { d3dsh_free(sh); return; }
    Cfg *c = cfg_new("ps_%s_v%d", name, variant);
    c->kind = K_PS; c->sh = sh;
    SgProgram *p = &c->prog;
    p->kind = SGP_D3D9_PS;
    SgpPs *ps = &p->ps;
    ps->shader = sh; ps->tokens = d3dsh_tokens(sh); ps->ntokens = (int)d3dsh_token_count(sh);
    ps->major = d3dsh_major(sh); ps->minor = d3dsh_minor(sh);
    ps->consts = &c->consts;
    ps->input_mask = d3dsh_ps_input_mask(sh); ps->sampler_mask = d3dsh_ps_sampler_mask(sh);
    for (int i = 0; i < SGP_MAX_SAMPLERS; i++) ps->sampler_type[i] = d3dsh_ps_sampler_type(sh, i);
    ps->writes_depth = d3dsh_ps_writes_depth(sh);
    uint32_t misc = d3dsh_ps_misc_mask(sh);
    ps->uses_vpos = (misc & 1) != 0; ps->uses_vface = (misc & 2) != 0;
    int forced = (variant & PV_CUBE) ? SG_TEX_CUBE : (variant & PV_VOL) ? SG_TEX_VOLUME : SG_TEX_2D;
    int first = 1;
    c->pc.ps = sh; c->pc.c = &c->consts;
    for (int i = 0; i < SGP_MAX_SAMPLERS; i++) {
        for (int k = 0; k < 4; k++) ps->bump_mat[i][k] = rf(-0.5f, 0.5f);
        ps->bump_lscale[i] = rf(0.5f, 2); ps->bump_loffset[i] = rf(-0.3f, 0.3f);
        if (!(ps->sampler_mask & (1u << i))) continue;
        if ((variant & PV_UNBOUND) && first) { first = 0; continue; }
        first = 0;
        int dk = ps->sampler_type[i];
        int kind = dk >= 0 ? dk : forced;
        p->samplers[i].tex = pool_tex(kind, (int)rnd());
        rand_sampler(&p->samplers[i].samp);
        p->nsamplers = i + 1;
        c->env.tex[i] = p->samplers[i].tex;
    }
    int nv = d3dsh_ps_nvary(sh);
    if (variant & (PV_FOG1 | PV_FOG2 | PV_FOG3 | PV_FOGV)) {
        p->fog.enable = 1;
        p->fog.table = (variant & PV_FOG1) ? 1 : (variant & PV_FOG2) ? 2 : (variant & PV_FOG3) ? 3 : 0;
        p->fog.start = rf(0, 0.4f); p->fog.end = rf(0.5f, 1); p->fog.density = rf(0.5f, 3);
        for (int k = 0; k < 3; k++) p->fog.color[k] = rf(0, 1);
        if (nv < 11) nv = 11;
    }
    SgState *s = &c->s;
    s->program = p; s->nvary = nv; p->nvary = nv;
    s->pixel_writes_depth = ps->writes_depth;
    state_common(c);
    if (variant & PV_ALPHA) { s->alpha_test = 1; s->alpha_func = (int)(rnd() % 8); s->alpha_ref = rf(0.2f, 0.8f); }
    if (variant & PV_DCLAMP) s->depth_clamp = 1;
    if (variant & PV_SPRITE) { s->point_sprite = 1; s->sprite_slot = nv > 0 ? (int)(rnd() % (unsigned)nv) : 0; c->prim = SG_PRIM_POINTS; c->sprite = 1; c->sprite_size = rf(2, 9); }
}

// ---- input generation --------------------------------------------------------------------------------------------------------------------
static float rand_comp(void) {
    switch (rnd() % 10) { case 0: return 0; case 1: return 1; case 2: return rf(0, 1); case 3: return rf(0.45f, 0.55f); default: return rf(-1.5f, 1.5f); }
}
static void rand_in(SgPixelIn *in, int nv) {
    memset(in, 0, sizeof *in);
    for (int k = 0; k < nv; k++)
        for (int q = 0; q < 4; q++) {
            in->v[k][q] = rand_comp();
            in->dx[k][q] = rnd() % 5 == 0 ? 0 : rf(-0.08f, 0.08f);
            in->dy[k][q] = rnd() % 5 == 0 ? 0 : rf(-0.08f, 0.08f);
        }
    in->pos[0] = (float)(rnd() % 200) + 0.5f; in->pos[1] = (float)(rnd() % 200) + 0.5f;
    in->pos[2] = rf(0, 1); in->pos[3] = rf(0.2f, 3);
    in->front_facing = (int)(rnd() & 1);
}
static void rand_consts(Cfg *c) {
    D3dShaderConsts *k = &c->consts;
    for (int i = 0; i < 256; i++) for (int q = 0; q < 4; q++) k->f[i][q] = rand_comp() * (rnd() % 8 == 0 ? 3.0f : 1.0f);
    for (int i = 0; i < 16; i++) { k->i[i][0] = (int)(rnd() % 6); k->i[i][1] = (int)(rnd() % 3); k->i[i][2] = 1 + (int)(rnd() % 2); k->i[i][3] = 0; k->b[i] = (int)(rnd() & 1); }
}

// ---- comparison --------------------------------------------------------------------------------------------------------------------------
typedef struct Ref { int pass; float c[4]; float depth; } Ref;

static void ref_final(const SgState *s, int pass, float c[4], Ref *r) {
    r->pass = pass;
    if (pass && s->alpha_test && !depth_pass(s->alpha_func, c[3], s->alpha_ref)) r->pass = 0;
    for (int i = 0; i < 4; i++) r->c[i] = sat01f(c[i]);
}

static void ref_eval(Cfg *c, const SgPixelIn *in, const float sprite[2], Ref *r) {
    const SgState *s = &c->s;
    SgPixelIn lin = *in;
    int slot = s->program ? s->sprite_slot : 1;
    if (c->sprite && slot >= 0 && slot < (s->program ? s->nvary : 4)) {
        lin.v[slot][0] = sprite[0]; lin.v[slot][1] = sprite[1];
        if (s->program) { lin.v[slot][2] = 0; lin.v[slot][3] = 1; }
    }
    float col[4] = { 0, 0, 0, 0 };
    r->depth = 0;
    if (c->kind == K_GL) {
        float cc[4] = { lin.v[0][0], lin.v[0][1], lin.v[0][2], lin.v[0][3] };
        for (int u = 0; u < SG_UNITS; u++) {
            if (!s->tex_on[u] || !s->tex[u] || !s->tex[u]->rgba) continue;
            float tx[4];
            sample(s->tex[u], lin.v[1 + u][0], lin.v[1 + u][1], 0, tx);
            tex_combine(s->tex_env[u], s->tex[u]->format, s->env_color[u], cc, tx);
        }
        if (s->fog) {
            float f = lin.v[3][0];
            if (f < 0) f = 0;
            if (f > 1) f = 1;
            for (int i = 0; i < 3; i++) cc[i] = cc[i] * f + s->fog_color[i] * (1 - f);
        }
        ref_final(s, 1, cc, r);
        return;
    }
    if (c->kind == K_TSS) {
        ff_pixel(&c->ff, &lin, col);
        for (int i = 0; i < 4; i++) col[i] = sat01f(col[i]);   // the rasteriser clamps
        ref_final(s, 1, col, r);
        return;
    }
    float zd = lin.pos[2];
    int ok = d3dsh_ps_pixel(&c->pc, &lin, col, &zd);
    if (ok && c->prog.fog.enable) {
        const SgpFog *fg = &c->prog.fog;
        float f = fg->table ? fog_factor(fg->table, lin.pos[2], fg->start, fg->end, fg->density) : sat01f(lin.v[10][0]);
        for (int k = 0; k < 3; k++) col[k] = col[k] * f + fg->color[k] * (1 - f);
    }
    for (int i = 0; i < 4; i++) col[i] = col[i] > 0 ? (col[i] < 1 ? col[i] : 1) : 0;
    if (s->depth_clamp) { float lo = s->depth_near < s->depth_far ? s->depth_near : s->depth_far, hi = s->depth_near < s->depth_far ? s->depth_far : s->depth_near; zd = zd < lo ? lo : zd > hi ? hi : zd; }
    ref_final(s, ok, col, r);
    r->depth = zd;
}

static void ff_from_program(Cfg *c) {
    const SgProgram *p = &c->prog;
    FfCtx *f = &c->ff;
    memset(f, 0, sizeof *f);
    f->nstages = p->tss.nstages;
    memcpy(f->tfactor, p->tss.tfactor, 16);
    f->specular = p->tss.specular;
    for (int i = 0; i < f->nstages; i++) {
        const SgpTssStage *o = &p->tss.stage[i];
        FfStage *s = &f->st[i];
        s->colorop = o->colorop; s->alphaop = o->alphaop;
        for (int k = 0; k < 3; k++) { s->carg[k] = o->color_arg[k]; s->aarg[k] = o->alpha_arg[k]; }
        s->resultarg = o->result_arg; s->slot = o->slot; s->count = o->ncoord; s->proj = o->projected;
        s->tex = p->samplers[i].tex; s->samp = p->samplers[i].samp;
        memcpy(s->bm, o->bump_mat, 16); s->lscale = o->bump_lscale; s->loffset = o->bump_loffset; memcpy(s->constant, o->constant, 16);
        s->is_cube = o->tex_kind == SG_TEX_CUBE;
    }
    f->fog_mode = p->fog.enable; f->fog_table = p->fog.table;
    f->fog_start = p->fog.start; f->fog_end = p->fog.end; f->fog_density = p->fog.density;
    memcpy(f->fog_color, p->fog.color, 12);
}

static void ps_ctx_from_program(Cfg *c) {
    const SgProgram *p = &c->prog;
    c->pc.ps = c->sh; c->pc.c = &c->consts;
    for (int i = 0; i < SGP_MAX_SAMPLERS; i++) {
        c->pc.tex[i] = p->samplers[i].tex; c->pc.samp[i] = p->samplers[i].samp;
        memcpy(c->pc.bump_mat[i], p->ps.bump_mat[i], 16);
        c->pc.bump_lscale[i] = p->ps.bump_lscale[i]; c->pc.bump_loffset[i] = p->ps.bump_loffset[i];
    }
}

static int g_nsamples = 300;
static long g_total_samples, g_passed_samples, g_interesting;

static int close_enough(float a, float b) { return fabsf(a - b) <= 1e-4f + 1e-4f * fabsf(b); }

static int run_cfg(Cfg *c) {
    const SgState *s = &c->s;
    int nv = s->program ? s->nvary : 4;
    if (c->kind == K_TSS) ff_from_program(c);
    if (c->kind == K_PS) ps_ctx_from_program(c);
    for (int i = 0; i < 16; i++) { c->env.samp[i] = c->kind == K_GL ? NULL : &c->prog.samplers[i].samp; }
    if (c->kind == K_GL) for (int u = 0; u < SG_UNITS; u++) { static SgSampler gs[SG_UNITS]; if (c->glt[u].rgba) { sgsl_gl_sampler(&c->glt[u], &gs[u]); c->env.samp[u] = &gs[u]; } }
    c->env.sample = sg_sample;
    if (c->kind == K_GL) for (int u = 0; u < SG_UNITS; u++) c->env.tex[u] = s->tex[u];
    else for (int i = 0; i < 16; i++) c->env.tex[i] = c->prog.samplers[i].tex;
    int bad = 0;
    for (int n = 0; n < g_nsamples && bad < 3; n++) {
        if (n % 40 == 0) {
            if (c->kind == K_PS) rand_consts(c);
        }
        SgPixelIn in;
        rand_in(&in, nv);
        float sp[2] = { rf(0, 1), rf(0, 1) };
        if (c->sprite) {
            int slot = s->program ? s->sprite_slot : 1;
            if (slot >= 0 && slot < nv) { in.dx[slot][0] = 1.0f / c->sprite_size; in.dx[slot][1] = 0; in.dy[slot][0] = 0; in.dy[slot][1] = 1.0f / c->sprite_size; in.dx[slot][2] = in.dx[slot][3] = in.dy[slot][2] = in.dy[slot][3] = 0; }
        }
        Ref ref;
        ref_eval(c, &in, sp, &ref);
        SgslUniforms u;
        sgsl_fill_uniforms(s, &u);
        SgslFragIn fin;
        memset(&fin, 0, sizeof fin);
        fin.px = in; fin.point_coord[0] = sp[0]; fin.point_coord[1] = sp[1];
        float col[4] = { -7, -7, -7, -7 }, depth = -7;
        int pass = c->fs(&c->env, &fin, &u, col, &depth);
        int ok = pass == ref.pass;
        if (ok && pass) {
            for (int k = 0; k < 4; k++) ok &= close_enough(col[k], ref.c[k]);
            if (c->info.writes_depth) ok &= close_enough(depth, ref.depth);
        }
        g_total_samples++;
        if (pass) { g_passed_samples++; if (col[0] != col[1] || col[1] != col[2] || (col[0] > 0.01f && col[0] < 0.99f)) g_interesting++; }
        if (!ok) {
            bad++;
            printf("   %s sample %d: pass ref %d got %d; colour ref (%g %g %g %g) got (%g %g %g %g)", c->name, n, ref.pass, pass, ref.c[0], ref.c[1], ref.c[2], ref.c[3], col[0], col[1], col[2], col[3]);
            if (c->info.writes_depth) printf(" depth ref %g got %g", ref.depth, depth);
            printf("\n");
        }
    }
    return bad == 0;
}

// ---- MSL structure checks ----------------------------------------------------------------------------------------------------------------------
static int msl_check(const char *src, char *why, size_t whylen) {
    int paren = 0, brace = 0, brack = 0, line_comment = 0;
    for (const char *p = src; *p; p++) {
        if (line_comment) { if (*p == '\n') line_comment = 0; continue; }
        if (p[0] == '/' && p[1] == '/') { line_comment = 1; continue; }
        switch (*p) {
        case '(': paren++; break; case ')': paren--; break; case '{': brace++; break; case '}': brace--; break; case '[': brack++; break; case ']': brack--; break;
        }
        if (paren < 0 || brace < 0 || brack < 0) { snprintf(why, whylen, "unbalanced brackets"); return 0; }
    }
    if (paren || brace || brack) { snprintf(why, whylen, "unbalanced brackets at the end"); return 0; }
    // MSL requires attributes before array bounds (float clip [[clip_distance]] [N]); "] [[" is an attribute after array bounds
    if (strstr(src, "] [[") || strstr(src, "][[")) { snprintf(why, whylen, "attribute after array bounds"); return 0; }
    if (!strstr(src, "vertex SgVSOut sg_vs(")) { snprintf(why, whylen, "no vertex sg_vs"); return 0; }
    if (!strstr(src, "fragment SgFSOut sg_fs(")) { snprintf(why, whylen, "no fragment sg_fs"); return 0; }
    // stage link: every SgFSIn member must have an identical member (type, name, [[user()]] and interpolation qualifier) in SgVSOut,
    // [[point_size]] exists in SgVSOut for point primitives only, and nothing but position/user members is in SgFSIn
    {
        const char *vo = strstr(src, "struct SgVSOut {"), *fi = strstr(src, "struct SgFSIn {");
        if (!vo || !fi) { snprintf(why, whylen, "no SgVSOut/SgFSIn"); return 0; }
        const char *vo_end = strstr(vo, "};"), *fi_end = strstr(fi, "};");
        if (!vo_end || !fi_end) { snprintf(why, whylen, "unterminated SgVSOut/SgFSIn"); return 0; }
        int prim = -1;
        const char *hp = strstr(src, ", primitive ");
        if (hp) prim = atoi(hp + 12);
        const char *ps = strstr(vo, "[[point_size]]");
        int has_ps = ps && ps < vo_end;
        if (has_ps != (prim == SG_PRIM_POINTS)) { snprintf(why, whylen, "[[point_size]] must be in SgVSOut exactly for points"); return 0; }
        const char *pos = strstr(vo, "float4 position [[position]];");
        if (!pos || pos > vo_end) { snprintf(why, whylen, "SgVSOut without [[position]]"); return 0; }
        for (const char *q = fi; q < fi_end; ) {
            const char *eol = strchr(q, '\n');
            if (!eol || eol > fi_end) break;
            char line[160];
            size_t n = (size_t)(eol - q);
            if (n >= sizeof line) n = sizeof line - 1;
            memcpy(line, q, n); line[n] = 0;
            q = eol + 1;
            if (!strstr(line, "[[user(")) {
                if (strstr(line, "struct SgFSIn") || strstr(line, "float4 position [[position]];") || !strchr(line, '[')) continue;
                snprintf(why, whylen, "unexpected SgFSIn member: %s", line); return 0;
            }
            const char *b = line;
            while (*b == ' ') b++;
            const char *f = vo;
            int found = 0;
            while ((f = strstr(f, b)) && f < vo_end) { if (f[-1] == ' ') { found = 1; break; } f += 1; }
            if (!found) { snprintf(why, whylen, "SgFSIn member without identical SgVSOut member: %s", b); return 0; }
        }
    }
    // textures / samplers used must be declared as parameters of sg_fs, texture types must not clash
    const char *fs = strstr(src, "fragment SgFSOut sg_fs(");
    for (const char *p = fs; (p = strstr(p, "tex")); p += 3) {
        if (p > src && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) continue;
        if (!isdigit((unsigned char)p[3])) continue;
        int idx = atoi(p + 3);
        char decl[64];
        snprintf(decl, sizeof decl, "tex%d [[texture(%d)]]", idx, idx);
        if (!strstr(fs, decl)) { snprintf(why, whylen, "texture %d used but not declared", idx); return 0; }
    }
    // no C-only constructs
    if (strstr(src, "sgc_") || strstr(src, "(env_") || strstr(src, "SgslCEnv")) { snprintf(why, whylen, "C-only construct in MSL"); return 0; }
    return 1;
}

static int has_helper_defs(const char *src, char *why, size_t whylen) {
    // every helper identifier used (sg_/d3_/d_/ff_/gl_ prefix followed by '(') must be defined in the text
    const char *prefixes[] = { "sg_", "d3_", "d_", "ff_", "gl_" };
    for (const char *p = src; *p; p++) {
        if (p > src && (isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.')) continue;
        int match = 0;
        for (int k = 0; k < 5; k++) if (!strncmp(p, prefixes[k], strlen(prefixes[k]))) match = 1;
        if (!match) continue;
        const char *e = p;
        while (isalnum((unsigned char)*e) || *e == '_') e++;
        if (*e != '(') continue;
        char name[64];
        size_t n = (size_t)(e - p);
        if (n >= sizeof name) continue;
        memcpy(name, p, n); name[n] = 0;
        if (!strcmp(name, "sg_vs") || !strcmp(name, "sg_fs")) { p = e - 1; continue; }
        // definition forms: "SG_FN <type> name(" , "SG_M1(name," , "float4 name(" at line start
        char pat1[80], pat2[80], pat3[80], pat4[80];
        snprintf(pat1, sizeof pat1, " %s(", name);
        snprintf(pat2, sizeof pat2, "(%s,", name);
        snprintf(pat3, sizeof pat3, "%s(float", name);
        snprintf(pat4, sizeof pat4, "%s(int", name);
        int found = 0;
        char pat5[80];
        snprintf(pat5, sizeof pat5, "#define %s", name);
        if (strstr(src, pat5)) found = 1;
        for (const char *q = src; !found && (q = strstr(q, name)); q += n) {
            if (q > src && (isalnum((unsigned char)q[-1]) || q[-1] == '_')) continue;
            if (q[n] != '(' && q[n] != ',') continue;
            // preceded by a type word ("float4 ", "float ", "int ", "bool ") => definition
            const char *b = q;
            while (b > src && b[-1] == ' ') b--;
            if (b - 6 >= src && !strncmp(b - 6, "float4", 6)) { found = 1; break; }
            if (b - 5 >= src && !strncmp(b - 5, "float", 5)) { found = 1; break; }
            if (b - 3 >= src && !strncmp(b - 3, "int", 3)) { found = 1; break; }
            if (b - 4 >= src && !strncmp(b - 4, "bool", 4)) { found = 1; break; }
            if (b > src && b[-1] == '(' && q[n] == ',') { found = 1; break; }   // SG_M1(name, ...)
        }
        if (!found) { snprintf(why, whylen, "helper '%s' used but not defined", name); return 0; }
        p = e - 1;
    }
    return 1;
}

// ---- driver ----------------------------------------------------------------------------------------------------------------------------------------
static const char *CDIR = "../../build/unit/sgsl_c", *MDIR = "../../build/unit/sgsl_msl";

static void write_file(const char *dir, const char *name, const char *ext, const char *text) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s%s", dir, name, ext);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void build_all_configs(void);

static void generate_all(void) {
    FILE *idx = NULL;
    char path[512];
    snprintf(path, sizeof path, "%s/index.txt", MDIR);
    idx = fopen(path, "w");
    int msl_ok = 1, msl_bad = 0;
    double total_us = 0; int ngen = 0;
    for (int i = 0; i < g_ncfg; i++) {
        Cfg *c = g_cfg[i];
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        c->csrc = sgsl_generate_prim(&c->s, c->prim, SGSL_C, &c->info, c->err, sizeof c->err);
        c->msrc = sgsl_generate_prim(&c->s, c->prim, SGSL_MSL, NULL, c->err, sizeof c->err);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (c->csrc && c->msrc) { total_us += (double)(t1.tv_sec - t0.tv_sec) * 1e6 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e3; ngen += 2; }
        if (!c->csrc || !c->msrc) { c->null_result = 1; printf("   NULL for %s: %s\n", c->name, c->err); continue; }
        write_file(CDIR, c->name, ".c", c->csrc);
        write_file(MDIR, c->name, ".metal", c->msrc);
        if (idx) fprintf(idx, "%s.metal %016llx %s\n", c->name, (unsigned long long)c->info.key, c->kind == K_GL ? "gl" : c->kind == K_TSS ? "tss" : "ps");
        char why[160] = "";
        if (!msl_check(c->msrc, why, sizeof why) || !has_helper_defs(c->msrc, why, sizeof why) || !has_helper_defs(c->csrc, why, sizeof why)) {
            msl_ok = 0; msl_bad++;
            if (msl_bad < 6) printf("   MSL check %s: %s\n", c->name, why);
        }
    }
    if (idx) fclose(idx);
    report("MSL sources have balanced structure, entry points and defined helpers", msl_ok);
    printf("   benchmark: %d sources generated, %.1f us average per source (MSL+C pair counted as two)\n", ngen, ngen ? total_us / ngen : 0.0);
}

static void compile_all(void) {
    // one gcc per configuration, in parallel through xargs
    char list[512];
    snprintf(list, sizeof list, "%s/list.txt", CDIR);
    FILE *f = fopen(list, "w");
    for (int i = 0; i < g_ncfg; i++) if (g_cfg[i]->csrc) fprintf(f, "%s\n", g_cfg[i]->name);
    fclose(f);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "cd %s && xargs -P 8 -I{} sh -c 'gcc -std=gnu11 -shared -fPIC -O1 -w -ffp-contract=off -I../../../src {}.c -o {}.so -lm || echo COMPILE-ERROR {}' < list.txt", CDIR);
    int rc = system(cmd);
    (void)rc;
}

static int load_all(void) {
    int bad = 0;
    for (int i = 0; i < g_ncfg; i++) {
        Cfg *c = g_cfg[i];
        if (!c->csrc) continue;
        char path[640];
        char cwd[400];
        if (!getcwd(cwd, sizeof cwd)) cwd[0] = 0;
        snprintf(path, sizeof path, "%s/%s/%s.so", cwd, CDIR, c->name);
        c->dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!c->dl) { printf("   dlopen %s: %s\n", c->name, dlerror()); bad++; continue; }
        c->fs = (FsFn)dlsym(c->dl, "sg_fs");
        c->vs = (VsFn)dlsym(c->dl, "sg_vs");
        if (!c->fs || !c->vs) { printf("   missing symbols in %s\n", c->name); bad++; c->fs = NULL; }
    }
    return bad;
}


// ---- MSL syntax / type check against a C++ mock of <metal_stdlib> (no Metal compiler on Linux) ------------------------------------------
// The generated MSL is plain C++ apart from attributes, address spaces and swizzles. The mock supplies float2/3/4 with every swizzle as
// a method (the check rewrites `).xyzw` to `).sw_xyzw()` first), textures with the sample overloads the translator emits, and empty
// `constant` / `device` / `vertex` / `fragment` keywords.
static void write_mock(const char *dir) {
    char path[512];
    snprintf(path, sizeof path, "%s/metal_stdlib", dir);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("#pragma once\n#include <cmath>\n#include <cstdint>\n#define vertex\n#define fragment\n#define constant\n#define device\n"
          "using namespace std;\n"
          "namespace metal {\n"
          "typedef unsigned uint;\n"
          "struct float2 { float x, y; float2() : x(0), y(0) {} float2(float a, float b) : x(a), y(b) {} float2 operator*(float k) const { return float2(x * k, y * k); } };\n"
          "struct float3 { float x, y, z; float3() : x(0), y(0), z(0) {} float3(float a, float b, float c) : x(a), y(b), z(c) {} float3 operator*(float k) const { return float3(x * k, y * k, z * k); } };\n"
          "struct float4;\n"
          "struct int4 { int x, y, z, w; };\n"
          "struct float4 {\n    float x, y, z, w;\n    float4() : x(0), y(0), z(0), w(0) {}\n    float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}\n", f);
    static const char L[4] = { 'x', 'y', 'z', 'w' };
    for (int n = 2; n <= 4; n++) {
        int total = 1;
        for (int i = 0; i < n; i++) total *= 4;
        for (int k = 0; k < total; k++) {
            char name[8], args[64];
            int kk = k, an = 0;
            for (int i = 0; i < n; i++) { name[i] = L[kk & 3]; an += snprintf(args + an, sizeof args - (size_t)an, "%s%c", i ? ", " : "", L[kk & 3]); kk >>= 2; }
            name[n] = 0;
            fprintf(f, "    float%d sw_%s() const { return float%d(%s); }\n", n, name, n, args);
        }
    }
    fputs("};\n"
          "struct sampler {};\n"
          "struct level { float v; level(float f) : v(f) {} };\n"
          "struct gradient2d { gradient2d(float2, float2) {} };\n"
          "struct gradientcube { gradientcube(float3, float3) {} };\n"
          "struct gradient3d { gradient3d(float3, float3) {} };\n"
          "template <class T> struct texture2d { float4 sample(sampler, float2) const { return float4(); } float4 sample(sampler, float2, level) const { return float4(); } float4 sample(sampler, float2, gradient2d) const { return float4(); } };\n"
          "template <class T> struct texturecube { float4 sample(sampler, float3) const { return float4(); } float4 sample(sampler, float3, level) const { return float4(); } float4 sample(sampler, float3, gradientcube) const { return float4(); } };\n"
          "template <class T> struct texture3d { float4 sample(sampler, float3) const { return float4(); } float4 sample(sampler, float3, level) const { return float4(); } float4 sample(sampler, float3, gradient3d) const { return float4(); } };\n"
          "inline void discard_fragment() {}\n"
          "inline float4 dfdx(float4 a) { return a; }\ninline float4 dfdy(float4 a) { return a; }\n"
          "}\n", f);
    fclose(f);
}

static int mock_check_all(void) {
    write_mock(MDIR);
    char list[512];
    snprintf(list, sizeof list, "%s/list.txt", MDIR);
    FILE *f = fopen(list, "w");
    for (int i = 0; i < g_ncfg; i++) if (g_cfg[i]->msrc) fprintf(f, "%s\n", g_cfg[i]->name);
    fclose(f);
    char cmd[1400];
    snprintf(cmd, sizeof cmd,
             "cd %s && rm -f mock_errors.txt && xargs -P 8 -I{} sh -c 'sed -E \"s/\\)\\.([xyzw]{2,4})\\b/).sw_\\1()/g\" {}.metal | %s -std=c++14 -fsyntax-only -w -I. -x c++ - 2>>mock_errors.txt || echo MOCK-ERROR {} >> mock_errors.txt' < list.txt",
             MDIR, getenv("SGSL_CXX") ? getenv("SGSL_CXX") : "g++");
    int rc = system(cmd);
    (void)rc;
    char errp[512];
    snprintf(errp, sizeof errp, "%s/mock_errors.txt", MDIR);
    FILE *e = fopen(errp, "r");
    int bad = 0;
    if (e) {
        char line[400];
        while (fgets(line, sizeof line, e)) { if (bad < 12) printf("   %s", line); bad++; }
        fclose(e);
    }
    return bad == 0;
}

// ---- vertex function checks ------------------------------------------------------------------------------------------------------------------------------
static int check_vertex(Cfg *c) {
    const SgState *s = &c->s;
    int nv = s->program ? s->nvary : 4;
    enum { NV = 12 };
    SgVertex vb[NV];
    memset(vb, 0, sizeof vb);
    for (int i = 0; i < NV; i++) {
        vb[i].x = rf(-2, 2); vb[i].y = rf(-2, 2); vb[i].z = rf(-2, 2); vb[i].w = rf(0.5f, 2);
        for (int k = 0; k < 4; k++) vb[i].c[k] = rf(0, 1);
        for (int u = 0; u < 2; u++) for (int k = 0; k < 2; k++) vb[i].t[u][k] = rf(-1, 2);
        vb[i].fog = rf(0, 1); vb[i].psize = rf(0.2f, 12);
        for (int k = 0; k < SG_VARYINGS; k++) for (int q = 0; q < 4; q++) vb[i].v[k][q] = rf(-3, 3);
    }
    SgslUniforms u;
    sgsl_fill_uniforms(s, &u);
    int per = c->prim == SG_PRIM_TRIANGLES ? 3 : c->prim == SG_PRIM_LINES ? 2 : 1;
    int ok = 1;
    for (unsigned vid = 0; vid < NV && ok; vid++) {
        SgslVSOut o;
        memset(&o, 0x55, sizeof o);
        c->vs(vid, vb, &u, &o);
        float zc = s->clip_z_zero ? vb[vid].z : (vb[vid].z + vb[vid].w) * 0.5f;
        ok &= close_enough(o.position.x, vb[vid].x) && close_enough(o.position.y, vb[vid].y) && close_enough(o.position.z, zc) && close_enough(o.position.w, vb[vid].w);
        int base = (int)vid / per * per, prov = s->flat_first ? base : base + per - 1;
        if (!s->program) {
            int ci = s->flat ? prov : (int)vid;
            for (int k = 0; k < 4; k++) ok &= o.v[0].x == o.v[0].x && close_enough(((float *)&o.v[0])[k], vb[ci].c[k]);
            ok &= close_enough(o.v[1].x, vb[vid].t[0][0]) && close_enough(o.v[1].y, vb[vid].t[0][1]) && close_enough(o.v[2].x, vb[vid].t[1][0]) && close_enough(o.v[3].x, vb[vid].fog);
        } else {
            for (int k = 0; k < nv; k++) {
                int vi = ((s->flat_mask >> k) & 1) ? prov : (int)vid;
                for (int q = 0; q < 4; q++) ok &= close_enough(((float *)&o.v[k])[q], vb[vi].v[k][q]);
            }
        }
        for (int i = 0; i < SG_MAX_CLIP_PLANES; i++) {
            if (!(s->clip_mask & (1 << i))) continue;
            float d = s->clip_plane[i][0] * vb[vid].x + s->clip_plane[i][1] * vb[vid].y + s->clip_plane[i][2] * vb[vid].z + s->clip_plane[i][3] * vb[vid].w;
            ok &= close_enough(o.clip[i], d);
        }
        if (c->prim == SG_PRIM_POINTS) {
            float sz = s->point_size_vertex ? vb[vid].psize : s->point_size;
            if (s->point_size_max > 0) { if (sz < s->point_size_min) sz = s->point_size_min; if (sz > s->point_size_max) sz = s->point_size_max; }
            if (s->point_sprite) { if (sz < 1) sz = 1; if (sz > 4096) sz = 4096; }
            else { sz = sz > 1.5f ? (float)(int)(sz + 0.5f) : 1.0f; if (sz > 4096) sz = 4096; }
            ok &= close_enough(o.point_size, sz);
        }
    }
    return ok;
}

// ---- configuration lists ------------------------------------------------------------------------------------------------------------------------------------
static void build_all_configs(void) {
    // GL fixed function: every environment x format (one unit), two-unit combinations, fog, alpha functions, flat, sprites
    for (int env = 0; env < 5; env++) for (int fmt = 0; fmt < 6; fmt++) make_gl(env, fmt, 0, 0, 0, env & 1, -1, 0, 0);
    for (int i = 0; i < 40; i++) make_gl((int)(rnd() % 5), (int)(rnd() % 6), (int)(rnd() % 5), (int)(rnd() % 6), 1, (int)(rnd() % 2), rnd() % 3 == 0 ? (int)(rnd() % 8) : -1, (int)(rnd() % 2), 0);
    for (int a = 0; a < 8; a++) make_gl(SG_ENV_MODULATE, SG_FMT_RGBA, 0, 0, 0, 0, a, 0, 0);
    make_gl(SG_ENV_MODULATE, SG_FMT_RGBA, 0, 0, 0, 1, -1, 1, 0);
    make_gl(SG_ENV_REPLACE, SG_FMT_RGB, 0, 0, 0, 0, -1, 1, 1);
    make_gl(SG_ENV_MODULATE, SG_FMT_RGBA, SG_ENV_ADD, SG_FMT_LUMINANCE_ALPHA, 1, 1, SG_GREATER, 0, 1);
    {   // no texture at all: colour only
        Cfg *c = cfg_new("gl_plain"); c->kind = K_GL; state_common(c); c->s.fog = 1;
        Cfg *d = cfg_new("gl_plain_flat_alpha"); d->kind = K_GL; state_common(d); d->s.flat = 1; d->s.alpha_test = 1; d->s.alpha_func = SG_LESS; d->s.alpha_ref = 0.5f;
    }
    // TSS: every colour operation (and a few alpha operations) with random arguments, 1-3 stages
    static const int argsel[] = { 0, 1, 2, 3, 4, 5, 6 };
    int count = 0;
    for (int op = 2; op <= 26; op++) {
        if (op == 22 || op == 23) continue;
        for (int rep = 0; rep < 6; rep++) {
            int nst = 1 + (rep % 3);
            int ops[3][2], args[3][6], res[3];
            for (int i = 0; i < nst; i++) {
                ops[i][0] = i == 0 ? op : 2 + (int)(rnd() % 25);
                if (ops[i][0] == 22 || ops[i][0] == 23) ops[i][0] = 4;
                ops[i][1] = rnd() % 4 == 0 ? 1 : (int)(2 + rnd() % 25);
                if (ops[i][1] == 22 || ops[i][1] == 23 || ops[i][1] == 24) ops[i][1] = 4;
                for (int k = 0; k < 6; k++) { args[i][k] = argsel[rnd() % 7]; if (rnd() % 4 == 0) args[i][k] |= 0x10; if (rnd() % 6 == 0) args[i][k] |= 0x20; }
                res[i] = (i < nst - 1 && rnd() % 3 == 0) ? 5 : 1;
            }
            char tag[64];
            snprintf(tag, sizeof tag, "op%d_r%d_n%d", op, rep, nst);
            make_tss(tag, nst, ops, args, res, (int)(rnd() % 3 == 0), (int)(rnd() % 5 == 0 ? 1 + rnd() % 4 : 0), (int)(rnd() % 8), rep == 5);
            count++;
        }
    }
    // TSS alpha operations against a fixed colour operation
    for (int aop = 1; aop <= 26; aop++) {
        if (aop >= 22 && aop <= 24) continue;
        int ops[1][2] = { { 4, aop } }, args[1][6] = { { 2, 2, 0, 2, 2, 0 } }, res[1] = { 1 };
        char tag[32];
        snprintf(tag, sizeof tag, "aop%d", aop);
        make_tss(tag, 1, ops, args, res, 0, 0, 1, 0);
    }
    // TSS bump mapping (BUMPENVMAP / BUMPENVMAPLUMINANCE followed by a textured stage), the cascade stopping at an unbound texture, dot3, temp registers
    for (int lumi = 0; lumi < 2; lumi++) for (int rep = 0; rep < 4; rep++) {
        int ops[3][2] = { { 22 + lumi, 1 }, { 4, 4 }, { 7, 2 } };
        int args[3][6] = { { 2, 2, 1, 2, 2, 1 }, { 2, 0, 1, 2, 0, 1 }, { 1, 1, 0, 1, 1, 0 } };
        int res[3] = { 1, 1, 1 };
        char tag[48];
        snprintf(tag, sizeof tag, "bump%d_%d", lumi, rep);
        make_tss(tag, 2 + (rep & 1), ops, args, res, rep == 3, rep == 2 ? 2 : 0, 7, 0);
    }
    for (int mask = 0; mask < 4; mask++) {
        int ops[3][2] = { { 4, 4 }, { 4, 4 }, { 4, 4 } };
        int args[3][6] = { { 2, 2, 0, 2, 2, 0 }, { 2, 1, 0, 2, 1, 0 }, { 2, 1, 0, 1, 1, 0 } };
        int res[3] = { 1, 1, 1 };
        char tag[48];
        snprintf(tag, sizeof tag, "stop%d", mask);
        make_tss(tag, 3, ops, args, res, 0, 0, mask, 0);
    }
    {
        int ops[3][2] = { { 24, 4 }, { 4, 4 }, { 24, 1 } };
        int args[3][6] = { { 0, 2, 0, 0, 2, 0 }, { 0, 1, 0, 0, 1, 0 }, { 0, 5, 3, 0, 5, 3 } };
        int res[3] = { 5, 1, 1 };
        make_tss("dot3_temp", 3, ops, args, res, 1, 1, 7, 1);
    }
    // pixel shaders
    int nps = 0;
    for (int i = 0; i < NTAB_U; i++) {
        const D3dshTab *t = &SH_TABLE[i];
        char err[100];
        D3dShader *probe = d3dsh_create(t->t, t->n, err, sizeof err);
        if (!probe) continue;
        int is_ps = d3dsh_is_pixel(probe), major = d3dsh_major(probe);
        uint32_t smask = d3dsh_ps_sampler_mask(probe);
        int ps1x = major == 1;
        int wd = d3dsh_ps_writes_depth(probe);
        d3dsh_free(probe);
        if (!is_ps) continue;
        make_ps(t->name, t->t, t->n, 0);
        if (i % 7 == 0) make_ps(t->name, t->t, t->n, PV_DUP);   // same configuration, different random uniforms / samplers: must share the key and the source
        nps++;
        if (ps1x && smask) { make_ps(t->name, t->t, t->n, PV_CUBE); make_ps(t->name, t->t, t->n, PV_VOL); }
        if (major < 3 && i % 2 == 0) make_ps(t->name, t->t, t->n, PV_FOG1 << (i % 3 == 0 ? 0 : i % 3 == 1 ? 1 : 3));   // 16, 32 or 128 (vertex fog)
        if (major < 3 && i % 5 == 1) make_ps(t->name, t->t, t->n, PV_FOG3 | PV_ALPHA);
        if (i % 3 == 0) make_ps(t->name, t->t, t->n, PV_ALPHA);
        if (smask && i % 4 == 2) make_ps(t->name, t->t, t->n, PV_UNBOUND);
        if (wd) make_ps(t->name, t->t, t->n, PV_DCLAMP);
        if (i % 17 == 3) make_ps(t->name, t->t, t->n, PV_SPRITE);
    }
    for (int i = 0; i < NTAB_W; i++) {
        const D3dshTabW *t = &SH_TABLE_W[i];
        char err[100];
        D3dShader *probe = d3dsh_create(t->t, t->n, err, sizeof err);
        if (!probe) continue;
        int is_ps = d3dsh_is_pixel(probe), major = d3dsh_major(probe);
        d3dsh_free(probe);
        if (!is_ps) continue;
        char nm[64];
        snprintf(nm, sizeof nm, "w32_%s", t->name);
        make_ps(nm, t->t, t->n, 0);
        if (major < 3) make_ps(nm, t->t, t->n, PV_FOG2);
        nps++;
    }
    printf("   configurations: %d (pixel shaders: %d, TSS cascades: %d+, GL: see names)\n", g_ncfg, nps, count);
}

// vertex-function variants (flat shading, clip planes, point rules) on a few configurations
static void vertex_variants(void) {
    static const int prims[3] = { SG_PRIM_TRIANGLES, SG_PRIM_LINES, SG_PRIM_POINTS };
    int base = g_ncfg;
    for (int pi = 0; pi < 3; pi++)
        for (int flatfirst = 0; flatfirst < 2; flatfirst++) {
            Cfg *c = cfg_new("vtx_gl_p%d_ff%d", pi, flatfirst);
            c->kind = K_GL; c->prim = prims[pi]; state_common(c);
            c->s.flat = 1; c->s.flat_first = flatfirst; c->s.clip_z_zero = pi & 1; c->s.clip_mask = 0x15;
            for (int i = 0; i < 6; i++) for (int k = 0; k < 4; k++) c->s.clip_plane[i][k] = rf(-1, 1);
            c->s.point_size = 3.4f; c->s.point_size_vertex = pi == 2 && flatfirst; c->s.point_sprite = pi == 2 && !flatfirst; c->s.point_size_min = 1.5f; c->s.point_size_max = rnd() & 1 ? 8.0f : 0;
            if (c->s.point_sprite) { c->sprite = 1; c->sprite_size = 4; }
        }
    for (int pi = 0; pi < 3; pi++) {
        const uint32_t *t = SH_TABLE[0].t; (void)t;
    }
    // programs with flat varyings
    for (int i = 0; i < NTAB_U; i++) {
        const D3dshTab *tb = &SH_TABLE[i];
        if (strcmp(tb->name, "p30_basic") && strcmp(tb->name, "p20_tex")) continue;
        for (int pi = 0; pi < 3; pi++) {
            make_ps(tb->name, tb->t, tb->n, 0);
            Cfg *c = g_cfg[g_ncfg - 1];
            snprintf(c->name + strlen(c->name), sizeof c->name - strlen(c->name), "_vtx%d", pi);
            c->prim = prims[pi]; c->s.flat_mask = 0x105; c->s.flat_first = pi & 1; c->s.clip_mask = 0x3; c->s.clip_plane[0][0] = 1; c->s.clip_plane[1][1] = -1; c->s.clip_plane[1][3] = 0.5f;
            c->s.point_size_vertex = 1; c->s.point_size_max = 6;
            if (c->s.nvary < 9) { c->s.nvary = 9; c->prog.nvary = 9; }
        }
    }
    (void)base;
}

int main(void) {
    const char *env = getenv("SGSL_SAMPLES");
    if (env && atoi(env) > 0) g_nsamples = atoi(env);
    mkdir("../../build", 0777); mkdir("../../build/unit", 0777); mkdir(CDIR, 0777); mkdir(MDIR, 0777);
    if (system("rm -f ../../build/unit/sgsl_c/*.c ../../build/unit/sgsl_c/*.so ../../build/unit/sgsl_msl/*.metal") != 0) {}
    pool_init();
    build_all_configs();
    vertex_variants();
    // static: key behaviour
    {
        SgState a; memset(&a, 0, sizeof a);
        SgTexture t = tex_make(SG_TEX_2D, 4, 4, 1, 0);
        a.tex_on[0] = 1; a.tex[0] = &t; a.alpha_test = 1; a.alpha_func = SG_LESS;
        SgState b = a;
        SgslInfo ia, ib; char err[200];
        char *sa = sgsl_generate(&a, SGSL_MSL, &ia, err, sizeof err);
        b.alpha_ref = 0.7f; b.fog_color[0] = 0.2f; b.env_color[0][1] = 0.5f;
        char *sb2 = sgsl_generate(&b, SGSL_MSL, &ib, err, sizeof err);
        report("equal keys for states differing only in uniform values, identical source", sa && sb2 && ia.key == ib.key && !strcmp(sa, sb2));
        SgState c2 = a; c2.alpha_func = SG_GREATER;
        SgslInfo ic; char *sc = sgsl_generate(&c2, SGSL_MSL, &ic, err, sizeof err);
        report("different alpha function -> different key and source", sc && ic.key != ia.key && strcmp(sa, sc) != 0);
        SgState d2 = a; d2.flat = 1;
        SgslInfo id; char *sd = sgsl_generate(&d2, SGSL_MSL, &id, err, sizeof err);
        report("flat shading changes the key", sd && id.key != ia.key);
        SgState e2 = a; e2.pixel_fn = (SgPixelFn)0x1;
        report("pixel_fn without program is refused", sgsl_generate(&e2, SGSL_MSL, NULL, err, sizeof err) == NULL && err[0]);
        free(sa); free(sb2); free(sc); free(sd);
        free(t.rgba);
    }
    generate_all();
    int nnull = 0;
    for (int i = 0; i < g_ncfg; i++) if (g_cfg[i]->null_result) nnull++;
    report("every configuration translates (no NULL results)", nnull == 0);
    compile_all();
    if (!getenv("SGSL_NO_MOCK")) report("every generated MSL source compiles as C++ against a mock of the Metal standard library", mock_check_all());
    else printf("   (MSL mock compile skipped)\n");
    {   // equal keys must mean identical source
        int same_ok = 1, pairs = 0;
        for (int i = 0; i < g_ncfg && same_ok; i++)
            for (int j = i + 1; j < g_ncfg; j++) {
                if (!g_cfg[i]->csrc || !g_cfg[j]->csrc || g_cfg[i]->info.key != g_cfg[j]->info.key) continue;
                pairs++;
                if (strcmp(g_cfg[i]->csrc, g_cfg[j]->csrc) != 0) { same_ok = 0; printf("   same key, different source: %s / %s\n", g_cfg[i]->name, g_cfg[j]->name); break; }
            }
        report("equal keys imply identical source (all configuration pairs)", same_ok);
        printf("   %d pairs share a key\n", pairs);
    }
    int dl_bad = load_all();
    report("every generated C rendering compiles and loads", dl_bad == 0);
    int ok_cfg = 0, tested = 0, bad_cfg = 0, vtx_ok = 0, vtx_n = 0;
    for (int i = 0; i < g_ncfg; i++) {
        Cfg *c = g_cfg[i];
        if (!c->fs) continue;
        int ok = run_cfg(c);
        tested++;
        if (ok) ok_cfg++; else { bad_cfg++; if (bad_cfg <= 10) printf("   FAILED configuration %s\n", c->name); }
        if (strncmp(c->name, "vtx_", 4) == 0 || strstr(c->name, "_vtx")) {
            vtx_n++;
            int v = check_vertex(c);
            if (v) vtx_ok++; else printf("   vertex function mismatch in %s\n", c->name);
        }
    }
    printf("   %d configurations compared, %ld samples (%ld not discarded, %ld with non-trivial colours)\n", tested, g_total_samples, g_passed_samples, g_interesting);
    report("C rendering matches the CPU implementations (all configurations, all samples)", tested > 0 && bad_cfg == 0);
    report("vertex functions: clip z conversion, flat provoking vertex, clip distances, point sizes", vtx_n > 0 && vtx_ok == vtx_n);
    printf("passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
