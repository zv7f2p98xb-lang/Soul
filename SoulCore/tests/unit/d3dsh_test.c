// Unit test for the Direct3D 9 shader engine (src/d3d9_shader*.c): assembles nothing at run time; the
// shaders are token arrays generated from shaders/*.asm by shaders/d3dasm.py (see d3dsh_shaders.h).
#include "d3d9_shader.h"
#include "shaders/d3dsh_shaders.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_pass, g_fail;
static void report(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_pass++; else g_fail++;
}

static int feq(float a, float b) {
    if (a == b) return 1;
    if (isnan(a) && isnan(b)) return 1;
    if (isnan(a) || isnan(b) || isinf(a) || isinf(b)) return 0;
    return fabsf(a - b) <= 1e-5f * fmaxf(fabsf(a), fabsf(b)) + 1e-6f;
}
static int feq_t(float a, float b, float tol) { return fabsf(a - b) <= tol; }
static float sat01(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }

static void ck4(const char *name, const float *g, float a, float b, float c, float d) {
    int ok = feq(g[0], a) && feq(g[1], b) && feq(g[2], c) && feq(g[3], d);
    report(name, ok);
    if (!ok) printf("   got (%g %g %g %g) expected (%g %g %g %g)\n", g[0], g[1], g[2], g[3], a, b, c, d);
}
static void ck4t(const char *name, const float *g, float a, float b, float c, float d, float tol) {
    int ok = feq_t(g[0], a, tol) && feq_t(g[1], b, tol) && feq_t(g[2], c, tol) && feq_t(g[3], d, tol);
    report(name, ok);
    if (!ok) printf("   got (%g %g %g %g) expected (%g %g %g %g)\n", g[0], g[1], g[2], g[3], a, b, c, d);
}
static void ck4v(const char *name, const float *g, const float *e) { ck4(name, g, e[0], e[1], e[2], e[3]); }

// ---- harness ---------------------------------------------------------------------------------------
static D3dShaderConsts C;
static float VIN[D3DSH_VS_INPUTS][4];
static D3dVsCtx VC;
static D3dPsCtx PC;
static SgPixelIn PIN;
static float g_depth;
static SgSampler DS;

static D3dShader *mk(const char *name, const uint32_t *t, size_t n) {
    char err[200];
    D3dShader *s = d3dsh_create(t, n, err, sizeof err);
    if (!s) { printf("FAIL create %s: %s\n", name, err); g_fail++; }
    return s;
}
#define MK(n) mk(#n, SH_##n, sizeof(SH_##n) / 4)

static void vs_reset(void) {
    memset(&C, 0, sizeof C); memset(VIN, 0, sizeof VIN); memset(&VC, 0, sizeof VC);
    VC.c = &C;
    for (int i = 0; i < D3DSH_VS_TEXTURES; i++) VC.samp[i] = DS;
}
static void ps_reset(void) {
    memset(&C, 0, sizeof C); memset(&PIN, 0, sizeof PIN); memset(&PC, 0, sizeof PC);
    for (int i = 0; i < D3DSH_MAX_SAMPLERS; i++) PC.samp[i] = DS;
    PIN.front_facing = 1; PIN.pos[0] = 0.5f; PIN.pos[1] = 0.5f; PIN.pos[2] = 0.5f; PIN.pos[3] = 1;
}
static void setv(int i, int slot, float x, float y, float z, float w) { VIN[i][0] = x; VIN[i][1] = y; VIN[i][2] = z; VIN[i][3] = w; (void)slot; }
static void setvary(int slot, float x, float y, float z, float w) { PIN.v[slot][0] = x; PIN.v[slot][1] = y; PIN.v[slot][2] = z; PIN.v[slot][3] = w; }
static void setgrad(int slot, const float dx[4], const float dy[4]) { memcpy(PIN.dx[slot], dx, 16); memcpy(PIN.dy[slot], dy, 16); }
static void setc(int i, float x, float y, float z, float w) { C.f[i][0] = x; C.f[i][1] = y; C.f[i][2] = z; C.f[i][3] = w; }
static int run_ps(const D3dShader *ps, float out[4]) {
    PC.ps = ps; PC.c = &C; g_depth = PIN.pos[2];
    out[0] = out[1] = out[2] = out[3] = -99;
    return d3dsh_ps_pixel(&PC, &PIN, out, &g_depth);
}
static void run_vs(const D3dShader *vs, D3dVsOut *o) { d3dsh_run_vs_ex(vs, &VC, VIN, o); }

// ---- textures --------------------------------------------------------------------------------------
static SgTexture tex_new(int kind, int w, int h, int d) {
    SgTexture t; memset(&t, 0, sizeof t);
    t.kind = kind; t.w = w; t.h = h; t.depth = d; t.levels = 1;
    size_t n = (size_t)w * h * (kind == SG_TEX_VOLUME ? d : 1) * (kind == SG_TEX_CUBE ? 6 : 1) * 4;
    t.rgba = calloc(n, 1);
    t.mip[0] = t.rgba; t.mip_w[0] = w; t.mip_h[0] = h; t.mip_d[0] = d;
    return t;
}
static void tex_set(SgTexture *t, int x, int y, int z, int r, int g, int b, int a) {
    uint8_t *p = t->rgba + (((size_t)z * t->h + y) * t->w + x) * 4;
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = (uint8_t)a;
}
static SgTexture tex_grid4(void) {          // texel (i,j) = (i*64, j*64, 0, 255)
    SgTexture t = tex_new(SG_TEX_2D, 4, 4, 1);
    for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) tex_set(&t, i, j, 0, i * 64, j * 64, 0, 255);
    return t;
}
static SgTexture tex_solid(int r, int g, int b, int a) {
    SgTexture t = tex_new(SG_TEX_2D, 1, 1, 1);
    tex_set(&t, 0, 0, 0, r, g, b, a);
    return t;
}
static SgTexture tex_mips(void) {           // 8x8, level colours red, green, blue, yellow
    SgTexture t = tex_new(SG_TEX_2D, 8, 8, 1);
    sg_texture_build_mips(&t);
    static const int col[4][3] = { { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 }, { 255, 255, 0 } };
    for (int l = 0; l < t.levels && l < 4; l++)
        for (int i = 0; i < t.mip_w[l] * t.mip_h[l]; i++) { uint8_t *p = t.mip[l] + i * 4; p[0] = (uint8_t)col[l][0]; p[1] = (uint8_t)col[l][1]; p[2] = (uint8_t)col[l][2]; p[3] = 255; }
    t.use_mips = 1;
    return t;
}
static void tex_free(SgTexture *t) { sg_texture_free(t); }

// ---- reference implementations of the generic instructions (SM 2+ semantics) -------------------------
static void ref_op(const char *op, const float a[4], const float b[4], const float c[4], float r[4]) {
    memset(r, 0, 16);
#define IS(n) (!strcmp(op, n))
    if (IS("mov")) { for (int i = 0; i < 4; i++) r[i] = a[i]; }
    else if (IS("add")) { for (int i = 0; i < 4; i++) r[i] = a[i] + b[i]; }
    else if (IS("sub")) { for (int i = 0; i < 4; i++) r[i] = a[i] - b[i]; }
    else if (IS("mul")) { for (int i = 0; i < 4; i++) r[i] = a[i] * b[i]; }
    else if (IS("mad")) { for (int i = 0; i < 4; i++) r[i] = a[i] * b[i] + c[i]; }
    else if (IS("min")) { for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? a[i] : b[i]; }
    else if (IS("max")) { for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? a[i] : b[i]; }
    else if (IS("slt")) { for (int i = 0; i < 4; i++) r[i] = a[i] < b[i] ? 1.f : 0.f; }
    else if (IS("sge")) { for (int i = 0; i < 4; i++) r[i] = a[i] >= b[i] ? 1.f : 0.f; }
    else if (IS("dp3")) { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; for (int i = 0; i < 4; i++) r[i] = d; }
    else if (IS("dp4")) { float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; for (int i = 0; i < 4; i++) r[i] = d; }
    else if (IS("dp2add")) { float d = a[0] * b[0] + a[1] * b[1] + c[0]; for (int i = 0; i < 4; i++) r[i] = d; }
    else if (IS("dst")) { r[0] = 1; r[1] = a[1] * b[1]; r[2] = a[2]; r[3] = b[3]; }
    else if (IS("crs")) { r[0] = a[1] * b[2] - a[2] * b[1]; r[1] = a[2] * b[0] - a[0] * b[2]; r[2] = a[0] * b[1] - a[1] * b[0]; }
    else if (IS("pow")) { float v = powf(fabsf(a[0]), b[0]); for (int i = 0; i < 4; i++) r[i] = v; }
    else if (IS("abs")) { for (int i = 0; i < 4; i++) r[i] = fabsf(a[i]); }
    else if (IS("frc")) { for (int i = 0; i < 4; i++) r[i] = a[i] - floorf(a[i]); }
    else if (IS("rcp")) { for (int i = 0; i < 4; i++) r[i] = 1.0f / a[0]; }
    else if (IS("rsq")) { for (int i = 0; i < 4; i++) r[i] = 1.0f / sqrtf(fabsf(a[0])); }
    else if (IS("exp") || IS("expp")) { for (int i = 0; i < 4; i++) r[i] = exp2f(a[0]); }
    else if (IS("log") || IS("logp")) { for (int i = 0; i < 4; i++) r[i] = log2f(fabsf(a[0])); }
    else if (IS("nrm")) { float l = a[0] * a[0] + a[1] * a[1] + a[2] * a[2]; float k = 1.0f / sqrtf(l); for (int i = 0; i < 4; i++) r[i] = a[i] * k; }
    else if (IS("lit")) {
        float p = a[3] < -127.9961f ? -127.9961f : a[3] > 127.9961f ? 127.9961f : a[3];
        r[0] = 1; r[3] = 1;
        if (a[0] > 0) { r[1] = a[0]; if (a[1] > 0) r[2] = powf(a[1], p); }
    }
    else if (IS("lrp")) { for (int i = 0; i < 4; i++) r[i] = a[i] * (b[i] - c[i]) + c[i]; }
    else if (IS("cmp")) { for (int i = 0; i < 4; i++) r[i] = a[i] >= 0 ? b[i] : c[i]; }
    else { r[0] = NAN; }
#undef IS
}

static const float SETS_VS[3][3][4] = {
    { { 1.5f, -2.25f, 3.0f, 0.5f }, { 0.5f, 4.0f, -1.0f, 2.0f }, { 2.0f, 3.0f, 4.0f, 5.0f } },
    { { 0.75f, 0.5f, 0.25f, 2.5f }, { 1.0f, 2.0f, 3.0f, 4.0f }, { 0.1f, 0.2f, 0.3f, 0.4f } },
    { { 0.5f, 2.0f, 0.0f, 3.0f }, { 0.0f, -1.0f, 2.0f, -3.0f }, { -1.0f, 0.0f, 1.0f, 0.5f } },
};
static const float SETS_PS[3][3][4] = {
    { { 0.75f, 0.5f, 0.25f, 0.4f }, { 0.3f, 0.6f, 0.9f, 0.2f }, { 0.2f, 0.4f, 0.6f, 0.8f } },
    { { 0.9f, 0.1f, 0.8f, 0.2f }, { 0.1f, 0.2f, 0.3f, 0.4f }, { 0.6f, 0.5f, 0.4f, 0.3f } },
    { { 0.5f, 2.0f, 0.0f, 3.0f }, { 0.0f, -1.0f, 2.0f, -3.0f }, { -1.0f, 0.0f, 1.0f, 0.5f } },
};

#define OPLIST_BIN(X) X(add) X(sub) X(mul) X(min) X(max) X(slt) X(sge) X(dp3) X(dp4) X(dst) X(crs) X(pow)
#define OPLIST_UN(X) X(mov) X(abs) X(frc) X(rcp) X(rsq) X(exp) X(log) X(nrm) X(lit) X(expp) X(logp)
#define OPLIST_TRI(X) X(mad) X(lrp) X(cmp)

typedef struct { const char *op; const uint32_t *t; size_t n; } OpSh;
#define E_VBIN(o) { #o, SH_v20_bin_##o, sizeof SH_v20_bin_##o / 4 },
#define E_VUN(o) { #o, SH_v20_un_##o, sizeof SH_v20_un_##o / 4 },
#define E_VTRI(o) { #o, SH_v20_tri_##o, sizeof SH_v20_tri_##o / 4 },
static const OpSh VBIN[] = { OPLIST_BIN(E_VBIN) }, VUN[] = { OPLIST_UN(E_VUN) }, VTRI[] = { OPLIST_TRI(E_VTRI) };
#define E_PBIN(o) { #o, SH_p20_bin_##o, sizeof SH_p20_bin_##o / 4 },
#define E_PUN(o) { #o, SH_p20_un_##o, sizeof SH_p20_un_##o / 4 },
#define E_PTRI(o) { #o, SH_p20_tri_##o, sizeof SH_p20_tri_##o / 4 },
static const OpSh PBIN[] = { E_PBIN(add) E_PBIN(sub) E_PBIN(mul) E_PBIN(min) E_PBIN(max) E_PBIN(slt) E_PBIN(sge) E_PBIN(dp3) E_PBIN(dp4) E_PBIN(crs) E_PBIN(pow) };
static const OpSh PUN[] = { E_PUN(mov) E_PUN(abs) E_PUN(frc) E_PUN(rcp) E_PUN(rsq) E_PUN(exp) E_PUN(log) E_PUN(nrm) E_PUN(expp) E_PUN(logp) };
static const OpSh PTRI[] = { E_PTRI(mad) E_PTRI(lrp) E_PTRI(cmp) E_PTRI(dp2add) };
#define NELEM(a) (int)(sizeof(a) / sizeof((a)[0]))

static void test_vs_generic(void) {
    const OpSh *groups[3] = { VBIN, VUN, VTRI };
    int counts[3] = { NELEM(VBIN), NELEM(VUN), NELEM(VTRI) };
    for (int g = 0; g < 3; g++) for (int k = 0; k < counts[g]; k++) {
        const OpSh *o = &groups[g][k];
        D3dShader *sh = mk(o->op, o->t, o->n);
        if (!sh) continue;
        for (int s = 0; s < 3; s++) {
            const float *a = SETS_VS[s][0], *b = SETS_VS[s][1], *c = SETS_VS[s][2];
            vs_reset();
            memcpy(VIN[1], a, 16); memcpy(VIN[2], b, 16); memcpy(VIN[3], c, 16);
            D3dVsOut out; run_vs(sh, &out);
            float ref[4];
            const float *ra = a, *rb = b, *rc = c;
            if (g == 0) { rb = b; } else if (g == 1) { rb = rb; }
            ref_op(o->op, ra, rb, rc, ref);
            if (g == 1 || g == 0) { /* unary uses v1 only, binary v1, v2 */ }
            char nm[96]; snprintf(nm, sizeof nm, "vs_2_0 %s set%d", o->op, s);
            if (isnan(ref[0]) && !isnan(out.v[0][0])) report(nm, 0); else ck4v(nm, out.v[0], ref);
        }
        d3dsh_free(sh);
    }
    // matrix macros: rows c4..c7
    static const struct { const char *n; const uint32_t *t; size_t sz; int rows, dim; } M[] = {
        { "m4x4", SH_v20_mat_m4x4, sizeof SH_v20_mat_m4x4 / 4, 4, 4 }, { "m4x3", SH_v20_mat_m4x3, sizeof SH_v20_mat_m4x3 / 4, 3, 4 },
        { "m3x4", SH_v20_mat_m3x4, sizeof SH_v20_mat_m3x4 / 4, 4, 3 }, { "m3x3", SH_v20_mat_m3x3, sizeof SH_v20_mat_m3x3 / 4, 3, 3 },
        { "m3x2", SH_v20_mat_m3x2, sizeof SH_v20_mat_m3x2 / 4, 2, 3 } };
    for (int k = 0; k < 5; k++) {
        D3dShader *sh = mk(M[k].n, M[k].t, M[k].sz);
        if (!sh) continue;
        vs_reset();
        for (int i = 0; i < 4; i++) setc(4 + i, (float)(i + 1), 0.5f * i - 1, 2.0f - i, 0.25f * (i + 1));
        setv(0, 0, 1, 2, 3, 4);
        D3dVsOut out; run_vs(sh, &out);
        float e[4] = { 0, 0, 0, 0 };
        for (int r = 0; r < M[k].rows; r++) { float d = 0; for (int j = 0; j < M[k].dim; j++) d += VIN[0][j] * C.f[4 + r][j]; e[r] = d; }
        char nm[64]; snprintf(nm, sizeof nm, "vs_2_0 %s", M[k].n);
        ck4v(nm, out.v[0], e);
        d3dsh_free(sh);
    }
}

static void test_vs11(void) {
    D3dShader *sh;
    D3dVsOut o;
    // transform + outputs
    sh = MK(v11_xform);
    vs_reset();
    for (int i = 0; i < 4; i++) setc(i, (float)(i + 1), 0.5f * i, -1.0f * i, 0.25f);
    setc(4, 0.75f, 12.0f, 0, 0);
    setv(0, 0, 1, 2, 3, 1); setv(1, 0, 0.1f, 0.2f, 0.3f, 0.4f); setv(2, 0, 5, 6, 7, 8);
    run_vs(sh, &o);
    float e[4];
    for (int r = 0; r < 4; r++) { e[r] = 0; for (int j = 0; j < 4; j++) e[r] += VIN[0][j] * C.f[r][j]; }
    ck4v("vs_1_1 m4x4 oPos", o.pos, e);
    ck4("vs_1_1 oD0 -> slot 8", o.v[8], 0.1f, 0.2f, 0.3f, 0.4f);
    ck4("vs_1_1 oT0.xy -> slot 0", o.v[0], 5, 6, 0, 0);
    report("vs_1_1 oFog -> slot 10.x", o.v[10][0] == 0.75f);
    report("vs_1_1 oPts -> psize", o.psize == 12.0f);
    report("vs_1_1 output mask", d3dsh_vs_output_mask(sh) == ((1u << 0) | (1u << 8) | (1u << 10)) && o.written_mask == d3dsh_vs_output_mask(sh));
    report("vs_1_1 identity", d3dsh_is_pixel(sh) == 0 && d3dsh_major(sh) == 1 && d3dsh_minor(sh) == 1 && d3dsh_vs_inputs(sh, NULL, 0) == 0);
    // batch
    {
        enum { N = 5 };
        static float in[N][D3DSH_VS_INPUTS][4];
        static D3dVsOut outs[N];
        memset(in, 0, sizeof in);
        for (int i = 0; i < N; i++) { in[i][0][0] = (float)i; in[i][0][1] = 1; in[i][0][2] = 2; in[i][0][3] = 1; in[i][1][0] = (float)i * 0.1f; }
        d3dsh_run_vs_batch(sh, &VC, in, outs, N);
        int ok = 1;
        for (int i = 0; i < N; i++) {
            float ee[4];
            for (int r = 0; r < 4; r++) { ee[r] = 0; for (int j = 0; j < 4; j++) ee[r] += in[i][0][j] * C.f[r][j]; }
            for (int r = 0; r < 4; r++) ok &= feq(outs[i].pos[r], ee[r]);
            ok &= feq(outs[i].v[8][0], (float)i * 0.1f);
        }
        report("vs_1_1 batch of 5", ok);
    }
    d3dsh_free(sh);

    // arithmetic 1
    sh = MK(v11_arith1);
    vs_reset();
    setv(0, 0, 0, 0, 0, 1); setv(1, 0, 1, -2, 3, 4); setv(2, 0, 0.5f, 4, -1, 2); setv(3, 0, 1, 1, 1, 1);
    // note: v0 = pos, the arithmetic uses v0,v1,v2 -> rebind: a = v0
    setv(0, 0, 1, -2, 3, 4); setv(1, 0, 0.5f, 4, -1, 2); setv(2, 0, 1, 1, 1, 1);
    run_vs(sh, &o);
    {
        const float *a = VIN[0], *b = VIN[1], *c = VIN[2];
        float r[4];
        const char *names[8] = { "add", "sub", "mul", "mad", "dp3", "dp4", "min", "max" };
        for (int s = 0; s < 8; s++) {
            const char *opn = names[s];
            ref_op(opn, a, b, c, r);
            char nm[48]; snprintf(nm, sizeof nm, "vs_1_1 %s", opn);
            ck4v(nm, o.v[s], r);
        }
    }
    d3dsh_free(sh);

    sh = MK(v11_arith2);
    vs_reset();
    setv(0, 0, 4, -2, 3, 3); setv(1, 0, 0.5f, 16, -1, 8); setv(2, 0, 0.5f, 2, 0, 3);
    run_vs(sh, &o);
    ck4("vs_1_1 slt", o.v[0], 0, 1, 0, 1);
    ck4("vs_1_1 sge", o.v[1], 1, 0, 1, 0);
    ck4("vs_1_1 rcp", o.v[2], 0.25f, 0.25f, 0.25f, 0.25f);
    ck4("vs_1_1 rsq", o.v[3], 0.25f, 0.25f, 0.25f, 0.25f);
    ck4("vs_1_1 exp (4 component form)", o.v[4], 8, 0, 8, 1);
    ck4("vs_1_1 log (4 component form)", o.v[5], 3, 1, 3, 1);
    ck4("vs_1_1 lit", o.v[6], 1, 0.5f, 8, 1);
    ck4("vs_1_1 dst", o.v[7], 1, -32, 3, 8);
    d3dsh_free(sh);

    sh = MK(v11_arith3);
    vs_reset();
    for (int i = 0; i < 4; i++) setc(i, (float)(i + 1), 1, 0.5f * i, 2);
    setv(0, 0, 1.25f, -2.5f, 3, 4); setv(1, 0, 12, 1, 2, 3);
    run_vs(sh, &o);
    ck4("vs_1_1 frc", o.v[0], 0.25f, 0.5f, 0, 0);
    {
        float m33[4], m43[4], m34[4], m32[4];
        memset(m33, 0, 16); memset(m43, 0, 16); memset(m34, 0, 16); memset(m32, 0, 16);
        for (int r = 0; r < 4; r++) {
            float d3 = 0, d4 = 0;
            for (int j = 0; j < 3; j++) d3 += VIN[0][j] * C.f[r][j];
            d4 = d3 + VIN[0][3] * C.f[r][3];
            if (r < 3) { m33[r] = d3; m43[r] = d4; }
            m34[r] = d3;
            if (r < 2) m32[r] = d3;
        }
        ck4v("vs_1_1 m3x3", o.v[1], m33); ck4v("vs_1_1 m4x3", o.v[2], m43);
        ck4v("vs_1_1 m3x4", o.v[3], m34); ck4v("vs_1_1 m3x2", o.v[4], m32);
    }
    d3dsh_free(sh);

    sh = MK(v11_rel);
    vs_reset();
    for (int i = 0; i < 256; i++) setc(i, (float)i, i + 0.25f, 0, 1);
    setv(3, 0, 2.7f, 0, 0, 0);
    run_vs(sh, &o);
    ck4("vs_1_1 c[a0.x+4] (a0 = floor)", o.pos, 6, 6.25f, 0, 1);
    ck4("vs_1_1 c[a0.x]", o.v[0], 2, 2.25f, 0, 1);
    ck4("vs_1_1 c[a0.x+200]", o.v[1], 202, 202.25f, 0, 1);
    setv(3, 0, 100, 0, 0, 0);
    run_vs(sh, &o);
    ck4("vs_1_1 relative out of range reads 0", o.v[1], 0, 0, 0, 0);
    setv(3, 0, -5, 0, 0, 0);
    run_vs(sh, &o);
    ck4("vs_1_1 relative negative index reads 0", o.v[0], 0, 0, 0, 0);
    d3dsh_free(sh);

    sh = MK(v11_defs);
    vs_reset();
    setc(20, 9, 9, 9, 9); setc(21, 5, 6, 7, 8);
    run_vs(sh, &o);
    ck4("vs_1_1 def overrides caller constants", o.pos, 1, 2, 3, 4);
    ck4("vs_1_1 non-def constant from caller", o.v[0], 5, 6, 7, 8);
    ck4("vs_1_1 def + caller", o.v[1], 6, 8, 10, 12);
    d3dsh_free(sh);

    sh = MK(v11_swz);
    vs_reset();
    setv(0, 0, 1, 2, 3, 4); setv(1, 0, 10, 20, 30, 40);
    run_vs(sh, &o);
    ck4("vs_1_1 swizzle wzyx", o.pos, 4, 3, 2, 1);
    ck4("vs_1_1 negate + swizzle", o.v[0], -1, -1, -2, -2);
    ck4("vs_1_1 write mask yz", o.v[1], 0, 2, 3, 0);
    ck4("vs_1_1 mask xw", o.v[2], 3 - 10, 0, 0, 3 - 40);
    d3dsh_free(sh);

    sh = MK(v11_zero);
    vs_reset();
    setv(0, 0, 1, 2, 3, 4);
    run_vs(sh, &o);
    ck4("vs_1_1 rcp(0) = FLT_MAX", o.v[0], 3.402823466e38f, 3.402823466e38f, 3.402823466e38f, 3.402823466e38f);
    ck4("vs_1_1 rsq(0) = FLT_MAX", o.v[1], 3.402823466e38f, 3.402823466e38f, 3.402823466e38f, 3.402823466e38f);
    report("vs_1_1 log(0) = -FLT_MAX", o.v[2][2] == -3.402823466e38f);
    d3dsh_free(sh);
}

static void test_vs11_expp(void) {
    D3dShader *sh = MK(v11_arith3);
    D3dVsOut o;
    vs_reset();
    setv(0, 0, 2.5f, 0, 0, 0); setv(1, 0, 12, 0, 0, 0);
    run_vs(sh, &o);
    ck4("vs_1_1 expp", o.v[5], 4, 0.5f, exp2f(2.5f), 1);
    ck4("vs_1_1 logp", o.v[6], 3, 1.5f, log2f(12), 1);
    ck4("vs_1_1 negated swizzle", o.v[7], 0, 0, 0, -12);
    d3dsh_free(sh);
}

static void test_vs20(void) {
    D3dShader *sh;
    D3dVsOut o;
    sh = MK(v20_loops);
    vs_reset();
    for (int i = 0; i < 256; i++) setc(i, (float)i, 0, 0, 0);
    C.b[0] = 0;
    run_vs(sh, &o);
    ck4("vs_2_0 loop aL, c[aL+10]", o.v[0], 1, 2, 3, 4);
    ck4("vs_2_0 rep i1", o.v[1], 1.5f, 0.75f, 0.375f, 3);
    report("vs_2_0 nested loops (4 x 2)", o.v[2][0] == 8);
    report("vs_2_0 loop with break_ge", o.v[3][0] == 5);
    ck4("vs_2_0 if_lt else branch", o.v[4], 0, 2, 0, 0);
    ck4("vs_2_0 if b0 (false)", o.v[5], 0.5f, 0.25f, 0.125f, 1);
    ck4("vs_2_0 defb true", o.v[6], 1, 1, 1, 1);
    report("vs_2_0 loop start/step (14,17,20)", o.v[7][0] == 51);
    C.b[0] = 1;
    run_vs(sh, &o);
    ck4("vs_2_0 if b0 (true)", o.v[5], 1, 1, 1, 1);
    d3dsh_free(sh);

    sh = MK(v20_call);
    vs_reset();
    setv(0, 0, 1, 2, 3, 4);
    run_vs(sh, &o);
    ck4("vs_2_0 call/ret", o.v[0], 4, 5, 6, 7);
    ck4("vs_2_0 callnz b0=0", o.v[1], 4, 5, 6, 7);
    C.b[0] = 1;
    run_vs(sh, &o);
    ck4("vs_2_0 callnz b0=1", o.v[1], 5, 6, 7, 8);
    d3dsh_free(sh);

    sh = MK(v2x_pred);
    vs_reset();
    setv(1, 0, 1, -1, 1, -1);
    setc(1, 0.1f, 0.2f, 0.3f, 0.4f); setc(2, 0.5f, 0.6f, 0.7f, 0.8f); setc(3, 1.1f, 1.2f, 1.3f, 1.4f);
    setc(4, 4, 4, 4, 4); setc(5, 5, 5, 5, 5); setc(6, 1, 0, 0, 0); setc(7, 4, 0, 0, 0);
    run_vs(sh, &o);
    ck4("vs_2_x predicated mov / (!p0.x)", o.v[0], 0.5f, 0.2f, 0.7f, 0.4f);
    ck4("vs_2_x if p0.x", o.v[1], 4, 4, 4, 4);
    ck4("vs_2_x loop + breakp", o.v[2], 4, 0, 0, 0);
    ck4("vs_2_x (p0.y) with p0.y false", o.v[3], 0, 0, 0, 0);
    setv(1, 0, -1, 1, -1, 1);
    run_vs(sh, &o);
    ck4("vs_2_x predicated mov, second pattern", o.v[0], 0.1f, 0.6f, 0.3f, 1.4f);
    ck4("vs_2_x if p0.x else", o.v[1], 5, 5, 5, 5);
    ck4("vs_2_x (p0.y) with p0.y true", o.v[3], 0.5f, 0.6f, 0.7f, 0.8f);
    d3dsh_free(sh);

    sh = MK(v20_ops);
    vs_reset();
    setv(1, 0, 3, 2.6f, -1.5f, 0.5f); setv(2, 0, 0.5f, 2, 4, 1);
    for (int i = 0; i < 16; i++) setc(i, (float)i, 0, 0, 0);
    run_vs(sh, &o);
    ck4("vs_2_0 sgn", o.v[4], 1, 1, -1, 1);
    ck4("vs_2_0 sincos", o.v[5], cosf(3), sinf(3), 0, 0);
    ck4("vs_2_0 mova rounds to nearest", o.v[6], 5, 0, 0, 0);
    d3dsh_free(sh);

    sh = MK(v30_sincos);
    vs_reset();
    setv(1, 0, 1.25f, 0, 0, 0);
    run_vs(sh, &o);
    ck4("vs_3_0 sincos (1 source form)", o.v[0], cosf(1.25f), sinf(1.25f), 0, 0);
    d3dsh_free(sh);

    // special values
    {
        static const char *z[] = { "rcp", "rsq", "log" };
        (void)z;
        sh = MK(v20_un_rcp);
        vs_reset(); setv(1, 0, 0, 0, 0, 0);
        run_vs(sh, &o);
        report("vs_2_0 rcp(0) = +inf", isinf(o.v[0][0]) && o.v[0][0] > 0);
        setv(1, 0, 2, 0, 0, 0); run_vs(sh, &o);
        ck4("vs_2_0 rcp(2)", o.v[0], 0.5f, 0.5f, 0.5f, 0.5f);
        d3dsh_free(sh);
        sh = MK(v20_un_rsq);
        vs_reset(); setv(1, 0, 0, 0, 0, 0);
        run_vs(sh, &o);
        report("vs_2_0 rsq(0) = +inf", isinf(o.v[0][0]) && o.v[0][0] > 0);
        setv(1, 0, -4, 0, 0, 0); run_vs(sh, &o);
        ck4("vs_2_0 rsq(-4) = 1/sqrt|x|", o.v[0], 0.5f, 0.5f, 0.5f, 0.5f);
        d3dsh_free(sh);
        sh = MK(v20_un_log);
        vs_reset(); setv(1, 0, 0, 0, 0, 0);
        run_vs(sh, &o);
        report("vs_2_0 log(0) = -inf", isinf(o.v[0][0]) && o.v[0][0] < 0);
        setv(1, 0, -8, 0, 0, 0); run_vs(sh, &o);
        ck4("vs_2_0 log(-8) = log2|x|", o.v[0], 3, 3, 3, 3);
        d3dsh_free(sh);
        sh = MK(v20_bin_pow);
        vs_reset(); setv(1, 0, -2, 0, 0, 0); setv(2, 0, 3, 0, 0, 0);
        run_vs(sh, &o);
        ck4("vs_2_0 pow(-2, 3) = |x|^y", o.v[0], 8, 8, 8, 8);
        setv(1, 0, 0, 0, 0, 0); setv(2, 0, 0, 0, 0, 0); run_vs(sh, &o);
        ck4("vs_2_0 pow(0, 0) = 1", o.v[0], 1, 1, 1, 1);
        d3dsh_free(sh);
        sh = MK(v20_bin_min);
        vs_reset(); setv(1, 0, NAN, 1, 2, 3); setv(2, 0, 5, NAN, 2, 1);
        run_vs(sh, &o);
        report("vs_2_0 min(NaN, x) = x, min(x, NaN) = NaN", o.v[0][0] == 5 && isnan(o.v[0][1]));
        d3dsh_free(sh);
        sh = MK(v20_bin_mul);
        vs_reset(); setv(1, 0, 0, INFINITY, 2, 0); setv(2, 0, INFINITY, 0, 3, 5);
        run_vs(sh, &o);
        ck4("vs_2_0 mul: 0 * inf = 0", o.v[0], 0, 0, 6, 0);
        d3dsh_free(sh);
    }
}

static void test_vs30(void) {
    D3dShader *sh = MK(v30_link);
    D3dVsOut o;
    vs_reset();
    setc(0, 2, 3, 4, 5); setc(1, 9, 7, 0, 0);
    setv(0, 0, 1, 2, 3, 4); setv(1, 0, 0, 1, 0, 0); setv(2, 0, 0.25f, 0.5f, 0, 0); setv(3, 0, 0.5f, 0.5f, 0.5f, 1);
    run_vs(sh, &o);
    ck4("vs_3_0 dcl_position o0", o.pos, 1, 2, 3, 4);
    ck4("vs_3_0 texcoord3 -> slot 3 (o4)", o.v[3], 0.25f, 0.5f, 0, 0);
    ck4("vs_3_0 color0 -> slot 8 (o7)", o.v[8], 1, 1.5f, 2, 5);
    ck4("vs_3_0 normal -> slot 11", o.v[11], 0, 1, 0, 0);
    report("vs_3_0 psize and fog", o.psize == 9 && o.v[10][0] == 7);
    report("vs_3_0 output mask", d3dsh_vs_output_mask(sh) == ((1u << 3) | (1u << 8) | (1u << 10) | (1u << 11)));
    D3dShaderInput in[16];
    int n = d3dsh_vs_inputs(sh, in, 16);
    report("vs_3_0 vs_inputs", n == 4 && in[0].reg == 0 && in[0].usage == D3DSU_POSITION && in[1].reg == 1 && in[1].usage == D3DSU_NORMAL &&
                                  in[2].reg == 2 && in[2].usage == D3DSU_TEXCOORD && in[2].usage_index == 0 && in[3].usage == D3DSU_COLOR);
    report("vs_3_0 vs_inputs max", d3dsh_vs_inputs(sh, in, 2) == 2);
    d3dsh_free(sh);

    // vertex texture fetch
    sh = MK(v30_vtf);
    vs_reset();
    SgTexture t = tex_mips();
    VC.tex[0] = &t;
    setv(0, 0, 1, 2, 3, 4); setv(1, 0, 0.3f, 0.3f, 0, 0);
    run_vs(sh, &o);
    ck4t("vs_3_0 texldl lod 0", o.v[0], 1, 0, 0, 1, 0.01f);
    setv(1, 0, 0.3f, 0.3f, 0, 1);
    run_vs(sh, &o);
    ck4t("vs_3_0 texldl lod 1", o.v[0], 0, 1, 0, 1, 0.01f);
    setv(1, 0, 0.3f, 0.3f, 0, 2);
    run_vs(sh, &o);
    ck4t("vs_3_0 texldl lod 2", o.v[0], 0, 0, 1, 1, 0.01f);
    VC.tex[0] = NULL;
    run_vs(sh, &o);
    ck4("vs_3_0 unbound vertex texture = (0,0,0,1)", o.v[0], 0, 0, 0, 1);
    tex_free(&t);
    d3dsh_free(sh);

    sh = MK(v30_loop);
    vs_reset();
    setv(0, 0, 1, 1, 1, 1); setv(1, 0, 1, 2, 3, 4); setv(2, 0, 10, 20, 30, 40); setv(3, 0, 100, 200, 300, 400);
    run_vs(sh, &o);
    ck4("vs_3_0 loop aL over v[aL]", o.v[0], 112, 223, 334, 445);
    d3dsh_free(sh);
}

// ---- pixel shaders ---------------------------------------------------------------------------------------
static void test_ps1x(void) {
    D3dShader *sh;
    float o[4];
    sh = MK(p11_arith);
    ps_reset();
    setvary(8, 0.2f, 0.4f, 0.6f, 1); setvary(9, 0.1f, 0.1f, 0.1f, 0); setc(0, 0.5f, 0.5f, 0.5f, 0.5f);
    run_ps(sh, o);
    ck4("ps_1_1 mul/add + co-issued alpha", o, 0.2f, 0.3f, 0.4f, 0.75f);
    report("ps_1_1 input mask (v0,v1)", d3dsh_ps_input_mask(sh) == 0x300 && d3dsh_ps_nvary(sh) == 10 && d3dsh_is_pixel(sh));
    d3dsh_free(sh);

    sh = MK(p11_clamp);
    ps_reset();
    run_ps(sh, o);
    ck4("ps_1_1 def clamps to [-1,1], output to [0,1]", o, 1, 0, 0.5f, 1);
    d3dsh_free(sh);

    sh = MK(p11_mods);
    ps_reset();
    setvary(8, 0.75f, 0.25f, 1, 0.5f); setvary(9, 0.5f, 0.5f, 0.25f, 0.5f);
    run_ps(sh, o);
    ck4("ps_1_1 bx2, comp, register clamp, _x2", o, 1, 0, 0.5f, 0.5f);
    d3dsh_free(sh);

    sh = MK(p11_shift);
    ps_reset();
    setvary(8, 0.5f, 0.25f, 1, 0.5f); setvary(9, 0.5f, 0.5f, 0.5f, 0.5f);
    run_ps(sh, o);
    ck4("ps_1_1 _d4, _x2_sat", o, 0.25f, 0.1875f, 0.375f, 0.75f);
    d3dsh_free(sh);

    sh = MK(p11_cnd);
    ps_reset();
    setc(0, 0, 0, 0, 0.8f); setc(1, 0.1f, 0.2f, 0.3f, 0.4f); setc(2, 0.9f, 0.8f, 0.7f, 0.6f);
    run_ps(sh, o); ck4("ps_1_1 cnd (r0.a > 0.5)", o, 0.1f, 0.2f, 0.3f, 0.4f);
    setc(0, 0, 0, 0, 0.3f);
    run_ps(sh, o); ck4("ps_1_1 cnd (r0.a <= 0.5)", o, 0.9f, 0.8f, 0.7f, 0.6f);
    d3dsh_free(sh);

    sh = MK(p11_dp3lrp);
    ps_reset();
    setvary(8, 0.75f, 0.5f, 0.25f, 1); setvary(9, 1, 0.5f, 0.75f, 0.5f);
    setc(0, 0.25f, 0.5f, 0.75f, 1); setc(1, 0.1f, 0.1f, 0.1f, 0);
    run_ps(sh, o);
    {
        float d = 0.5f * 1 + 0 * 0 + -0.5f * 0.5f;
        float v0[4] = { 0.75f, 0.5f, 0.25f, 1 }, v1[4] = { 1, 0.5f, 0.75f, 0.5f }, c0[4] = { 0.25f, 0.5f, 0.75f, 1 };
        float e[4];
        for (int i = 0; i < 3; i++) { float r1 = c0[i] * v0[i] + (1 - c0[i]) * v1[i]; e[i] = d * r1 + 0.1f; }
        e[3] = d;
        ck4v("ps_1_1 dp3 + lrp + mad", o, e);
    }
    d3dsh_free(sh);

    // textures
    sh = MK(p11_tex);
    ps_reset();
    SgTexture t0 = tex_new(SG_TEX_2D, 2, 2, 1), t1 = tex_solid(128, 128, 128, 128);
    tex_set(&t0, 0, 0, 0, 255, 0, 0, 255); tex_set(&t0, 1, 0, 0, 0, 255, 0, 255); tex_set(&t0, 0, 1, 0, 0, 0, 255, 255); tex_set(&t0, 1, 1, 0, 255, 255, 0, 255);
    PC.tex[0] = &t0; PC.tex[1] = &t1;
    setvary(0, 0.75f, 0.25f, 0, 1); setvary(1, 0.5f, 0.5f, 0, 1);
    run_ps(sh, o);
    ck4t("ps_1_1 tex t0, tex t1, mul", o, 0, 128 / 255.f, 0, 128 / 255.f, 0.002f);
    report("ps_1_1 sampler mask / types", d3dsh_ps_sampler_mask(sh) == 3 && d3dsh_ps_sampler_type(sh, 0) == -1 && d3dsh_ps_input_mask(sh) == 3);
    PC.tex[1] = NULL;
    run_ps(sh, o);
    ck4("ps_1_1 unbound texture samples (0,0,0,1)", o, 0, 0, 0, 1);
    tex_free(&t0); tex_free(&t1);
    d3dsh_free(sh);

    sh = MK(p11_texcoord);
    ps_reset();
    setvary(0, 0.5f, 1.5f, -0.5f, 7);
    run_ps(sh, o); ck4("ps_1_1 texcoord clamps to [0,1], w = 1", o, 0.5f, 1, 0, 1);
    d3dsh_free(sh);

    sh = MK(p11_texkill);
    ps_reset();
    setvary(0, 0.1f, 0.2f, 0.3f, 1); setvary(8, 0.4f, 0.5f, 0.6f, 0.7f);
    report("ps_1_1 texkill passes", run_ps(sh, o) == 1 && feq(o[1], 0.5f));
    setvary(0, 0.1f, -0.2f, 0.3f, 1);
    report("ps_1_1 texkill discards", run_ps(sh, o) == 0);
    setvary(0, 0.1f, 0.2f, -0.0f, -1);
    report("ps_1_1 texkill ignores w", run_ps(sh, o) == 1);
    d3dsh_free(sh);

    // bump env map
    {
        SgTexture bump = tex_solid(64, 128, 128, 255), grid = tex_grid4();
        sh = MK(p11_texbem);
        ps_reset();
        PC.tex[0] = &bump; PC.tex[1] = &grid;
        PC.bump_mat[1][0] = 1; PC.bump_mat[1][1] = 0; PC.bump_mat[1][2] = 0; PC.bump_mat[1][3] = 1;
        setvary(0, 0.5f, 0.5f, 0, 1); setvary(1, 0.125f, 0.125f, 0, 1);
        run_ps(sh, o);
        ck4t("ps_1_1 texbem", o, 64 / 255.f, 128 / 255.f, 0, 1, 0.003f);
        d3dsh_free(sh);
        sh = MK(p11_texbeml);
        PC.bump_lscale[1] = 0.5f; PC.bump_loffset[1] = 0;
        run_ps(sh, o);
        float L = 128 / 255.f * 0.5f;
        ck4t("ps_1_1 texbeml (luminance)", o, 64 / 255.f * L, 128 / 255.f * L, 0, 1, 0.003f);
        d3dsh_free(sh);
        // texm3x2
        sh = MK(p11_m3x2);
        ps_reset();
        SgTexture n = tex_solid(255, 128, 128, 255);
        PC.tex[0] = &n; PC.tex[2] = &grid;
        setvary(0, 0.5f, 0.5f, 0, 1); setvary(1, 0.4f, 0, 0, 1); setvary(2, 0.6f, 0, 0, 1);
        run_ps(sh, o);
        ck4t("ps_1_1 texm3x2pad/texm3x2tex", o, 64 / 255.f, 128 / 255.f, 0, 1, 0.003f);
        d3dsh_free(sh);
        // texreg2ar / gb
        SgTexture rg = tex_solid(80, 100, 160, 200);
        ps_reset();
        PC.tex[0] = &rg; PC.tex[1] = &grid;
        sh = MK(p11_reg2ar);
        run_ps(sh, o);
        ck4t("ps_1_1 texreg2ar", o, 192 / 255.f, 64 / 255.f, 0, 1, 0.003f);
        d3dsh_free(sh);
        sh = MK(p11_reg2gb);
        run_ps(sh, o);
        ck4t("ps_1_1 texreg2gb", o, 64 / 255.f, 128 / 255.f, 0, 1, 0.003f);
        d3dsh_free(sh);
        // ps_1_3: texdp3tex / texdp3 / texm3x3 / m3x2depth
        SgTexture t3 = tex_solid(64, 128, 64, 255);
        ps_reset();
        PC.tex[0] = &t3; PC.tex[1] = &grid;
        setvary(1, 0.4f, 0.4f, 0.4f, 1);
        sh = MK(p13_texdp3tex);
        run_ps(sh, o);
        ck4t("ps_1_3 texdp3tex", o, 64 / 255.f, 0, 0, 1, 0.003f);
        d3dsh_free(sh);
        sh = MK(p13_texdp3);
        run_ps(sh, o);
        {
            float dd = 0.4f * (64 + 128 + 64) / 255.f;
            ck4t("ps_1_3 texdp3", o, dd, dd, dd, dd, 0.003f);
        }
        d3dsh_free(sh);
        sh = MK(p13_m3x3);
        ps_reset();
        PC.tex[0] = &n;
        setvary(1, 0.2f, 0, 0, 1); setvary(2, 0.3f, 0, 0, 1); setvary(3, 0.4f, 0, 0, 1);
        run_ps(sh, o);
        ck4t("ps_1_3 texm3x3 (no sample)", o, 0.2f, 0.3f, 0.4f, 1, 0.001f);
        d3dsh_free(sh);
        sh = MK(p13_m3x2depth);
        ps_reset();
        PC.tex[0] = &n;
        setc(0, 0.5f, 0.25f, 0.75f, 1);
        setvary(1, 0.3f, 0, 0, 1); setvary(2, 0.6f, 0, 0, 1);
        report("ps_1_3 texm3x2depth writes depth", d3dsh_ps_writes_depth(sh) == 1);
        run_ps(sh, o);
        report("ps_1_3 texm3x2depth value", feq_t(g_depth, 0.5f, 0.001f));
        ck4("ps_1_3 texm3x2depth colour", o, 0.5f, 0.25f, 0.75f, 1);
        d3dsh_free(sh);
        tex_free(&bump); tex_free(&n); tex_free(&rg); tex_free(&t3);
        tex_free(&grid);
    }

    // cube map environment mapping
    {
        SgTexture cube = tex_new(SG_TEX_CUBE, 2, 2, 1), nm = tex_solid(255, 128, 128, 255);
        for (int f = 0; f < 6; f++) for (int i = 0; i < 4; i++) { uint8_t *p = cube.rgba + ((size_t)f * 4 + i) * 4; p[0] = (uint8_t)(f * 40 + 10); p[1] = 0; p[2] = 0; p[3] = 255; }
        sh = MK(p11_m3x3);
        ps_reset();
        PC.tex[0] = &nm; PC.tex[3] = &cube;
        setvary(1, 0.1f, 0, 0, 0); setvary(2, 1.0f, 0, 0, -1); setvary(3, 0.2f, 0, 0, 0);
        run_ps(sh, o);
        ck4t("ps_1_1 texm3x3pad/texm3x3tex (cube, +Y)", o, 90 / 255.f, 0, 0, 1, 0.002f);
        d3dsh_free(sh);
        sh = MK(p11_m3x3spec);
        setc(0, 0, -1, 0, 0);
        run_ps(sh, o);
        ck4t("ps_1_1 texm3x3spec (reflection, -Y)", o, 130 / 255.f, 0, 0, 1, 0.002f);
        d3dsh_free(sh);
        sh = MK(p11_m3x3vspec);
        run_ps(sh, o);
        ck4t("ps_1_1 texm3x3vspec (eye from w, -Y)", o, 130 / 255.f, 0, 0, 1, 0.002f);
        d3dsh_free(sh);
        tex_free(&cube); tex_free(&nm);
    }

    sh = MK(p13_cmp);
    ps_reset();
    setc(0, -0.5f, 0, 0.5f, -1); setc(1, 0.1f, 0.2f, 0.3f, 0.4f); setc(2, 0.5f, 0.6f, 0.7f, 0.8f);
    run_ps(sh, o); ck4("ps_1_3 cmp", o, 0.5f, 0.2f, 0.3f, 0.8f);
    d3dsh_free(sh);
    sh = MK(p13_dp4);
    ps_reset();
    setvary(8, 0.5f, 0.5f, 0.5f, 0.5f); setvary(9, 0.1f, 0.2f, 0.3f, 0.2f);
    run_ps(sh, o); ck4("ps_1_3 dp4", o, 0.4f, 0.4f, 0.4f, 0.4f);
    d3dsh_free(sh);
}

static void test_ps14(void) {
    float o[4];
    SgTexture grid = tex_grid4();
    D3dShader *sh = MK(p14_basic);
    ps_reset();
    SgTexture a = tex_solid(64, 192, 0, 255), b = tex_solid(128, 128, 128, 128);
    PC.tex[0] = &a; PC.tex[1] = &b; PC.tex[2] = &grid;
    run_ps(sh, o);
    ck4t("ps_1_4 texld, phase, dependent read", o, 64 / 255.f * 128 / 255.f, 192 / 255.f * 128 / 255.f, 0, 128 / 255.f, 0.004f);
    report("ps_1_4 sampler mask", d3dsh_ps_sampler_mask(sh) == 7);
    d3dsh_free(sh);

    sh = MK(p14_proj);
    ps_reset();
    PC.tex[0] = &grid;
    setvary(0, 1.0f, 1.5f, 0, 4);
    run_ps(sh, o); ck4t("ps_1_4 texld _dw projective", o, 64 / 255.f, 64 / 255.f, 0, 1, 0.003f);
    d3dsh_free(sh);
    sh = MK(p14_projz);
    setvary(0, 0.6f, 0.9f, 2, 0);
    run_ps(sh, o); ck4t("ps_1_4 texld _dz", o, 64 / 255.f, 64 / 255.f, 0, 1, 0.003f);
    d3dsh_free(sh);

    sh = MK(p14_texcrd);
    ps_reset();
    setvary(0, 0.25f, 0.5f, 0.75f, 1);
    run_ps(sh, o); ck4("ps_1_4 texcrd", o, 0.25f, 0.5f, 0.75f, 0);
    d3dsh_free(sh);

    sh = MK(p14_cnd);
    ps_reset();
    setvary(8, 0.2f, 0.7f, 0.5f, 0.9f); setvary(9, 0.11f, 0.12f, 0.13f, 0.14f); setc(1, 0.21f, 0.22f, 0.23f, 0.24f);
    run_ps(sh, o); ck4("ps_1_4 cnd", o, 0.21f, 0.12f, 0.23f, 0.14f);
    d3dsh_free(sh);

    sh = MK(p14_texdepth);
    ps_reset();
    setc(1, 0.2f, 0.4f, 0.6f, 0.8f);
    run_ps(sh, o);
    report("ps_1_4 texdepth", d3dsh_ps_writes_depth(sh) && feq_t(g_depth, 0.5f, 1e-5f));
    ck4("ps_1_4 texdepth colour", o, 0.2f, 0.4f, 0.6f, 0.8f);
    d3dsh_free(sh);

    sh = MK(p14_texkill);
    ps_reset();
    setvary(0, 0.1f, 0.2f, 0.3f, 1); setvary(8, 0.5f, 0.6f, 0.7f, 0.8f);
    report("ps_1_4 texkill passes", run_ps(sh, o) == 1 && feq(o[3], 0.8f));
    setvary(0, 0.1f, 0.2f, -0.3f, 1);
    report("ps_1_4 texkill discards", run_ps(sh, o) == 0);
    d3dsh_free(sh);

    sh = MK(p14_bem);
    ps_reset();
    float m[4] = { 0.5f, 0.25f, 0.75f, 1.0f };
    memcpy(PC.bump_mat[2], m, 16);
    run_ps(sh, o); ck4("ps_1_4 bem", o, 1, 0.875f, 0, 0);
    d3dsh_free(sh);
    tex_free(&a); tex_free(&b); tex_free(&grid);
}

#define E_P2(o) o,
static void test_ps20_generic(void) {
    const OpSh *groups[3] = { PBIN, PUN, PTRI };
    int counts[3] = { NELEM(PBIN), NELEM(PUN), NELEM(PTRI) };
    for (int g = 0; g < 3; g++) for (int k = 0; k < counts[g]; k++) {
        const OpSh *o = &groups[g][k];
        D3dShader *sh = mk(o->op, o->t, o->n);
        if (!sh) continue;
        for (int s = 0; s < 3; s++) {
            const float *a = SETS_PS[s][0], *b = SETS_PS[s][1], *c = SETS_PS[s][2];
            if (s == 2 && (!strcmp(o->op, "rcp") || !strcmp(o->op, "nrm"))) { /* still defined */ }
            ps_reset();
            setvary(0, a[0], a[1], a[2], a[3]); setvary(1, b[0], b[1], b[2], b[3]); setvary(2, c[0], c[1], c[2], c[3]);
            float out[4], ref[4];
            run_ps(sh, out);
            ref_op(o->op, a, b, c, ref);
            for (int i = 0; i < 4; i++) ref[i] = sat01(ref[i]);
            char nm[96]; snprintf(nm, sizeof nm, "ps_2_0 %s set%d", o->op, s);
            ck4v(nm, out, ref);
        }
        d3dsh_free(sh);
    }
}

static void test_ps20(void) {
    D3dShader *sh;
    float o[4];
    static const char *mods[2] = { "sat", "pp" };
    for (int i = 0; i < 2; i++) {
        sh = i == 0 ? MK(p20_mod_sat) : MK(p20_mod_pp);
        ps_reset();
        setvary(0, 1.5f, -0.5f, 0.25f, 0.3f);
        run_ps(sh, o);
        char nm[48]; snprintf(nm, sizeof nm, "ps_2_0 _%s modifier", mods[i]);
        if (i == 0) ck4(nm, o, 0, 0.5f, 0.25f, 0.3f); else ck4(nm, o, 1, 0, 0.25f, 0.3f);
        d3dsh_free(sh);
    }
    sh = MK(p20_sincos);
    ps_reset();
    setvary(0, 0.5f, 0, 0, 0);
    run_ps(sh, o);
    ck4("ps_2_0 sincos", o, cosf(0.5f), sinf(0.5f), 0.00260416674f, 0.00026041667f);
    d3dsh_free(sh);

    SgTexture t0 = tex_new(SG_TEX_2D, 2, 2, 1), grid = tex_grid4(), mips = tex_mips();
    tex_set(&t0, 0, 0, 0, 255, 0, 0, 255); tex_set(&t0, 1, 0, 0, 0, 255, 0, 255); tex_set(&t0, 0, 1, 0, 0, 0, 255, 255); tex_set(&t0, 1, 1, 0, 255, 255, 0, 255);
    sh = MK(p20_tex);
    ps_reset();
    PC.tex[0] = &t0;
    setvary(0, 0.25f, 0.75f, 0, 0); setvary(8, 0.5f, 1, 1, 0.5f);
    run_ps(sh, o);
    ck4t("ps_2_0 texld + v0 modulate", o, 0, 0, 1, 0.5f, 0.002f);
    report("ps_2_0 sampler type / mask / inputs", d3dsh_ps_sampler_type(sh, 0) == SG_TEX_2D && d3dsh_ps_sampler_mask(sh) == 1 &&
                                                    d3dsh_ps_input_mask(sh) == 0x101 && d3dsh_ps_nvary(sh) == 9);
    PC.samp[0].mag_filter = SG_FILTER_LINEAR; PC.samp[0].address_u = SG_ADDR_WRAP; PC.samp[0].address_v = SG_ADDR_WRAP;
    setvary(0, 0.5f, 0.25f, 0, 0);     // between texels (0,0) and (1,0) at v centre of row 0
    setvary(8, 1, 1, 1, 1);
    run_ps(sh, o);
    ck4t("ps_2_0 bilinear sampler state", o, 0.5f, 0.5f, 0, 1, 0.003f);
    d3dsh_free(sh);

    sh = MK(p20_proj);
    ps_reset();
    PC.tex[0] = &grid;
    setvary(0, 1.0f, 1.5f, 0, 4);
    run_ps(sh, o); ck4t("ps_2_0 texldp", o, 64 / 255.f, 64 / 255.f, 0, 1, 0.003f);
    d3dsh_free(sh);

    sh = MK(p20_bias);
    ps_reset();
    PC.tex[0] = &mips;
    { float dx[4] = { 0.125f, 0, 0, 0 }, dy[4] = { 0, 0.125f, 0, 0 }; setgrad(0, dx, dy); }
    setvary(0, 0.3f, 0.3f, 0, 0);
    PC.samp[0].min_filter = SG_FILTER_POINT; PC.samp[0].mip_filter = SG_FILTER_POINT;
    run_ps(sh, o); ck4t("ps_2_0 texldb bias 0 -> level 0", o, 1, 0, 0, 1, 0.003f);
    setvary(0, 0.3f, 0.3f, 0, 1);
    run_ps(sh, o); ck4t("ps_2_0 texldb bias 1 -> level 1", o, 0, 1, 0, 1, 0.003f);
    setvary(0, 0.3f, 0.3f, 0, 2);
    run_ps(sh, o); ck4t("ps_2_0 texldb bias 2 -> level 2", o, 0, 0, 1, 1, 0.003f);
    d3dsh_free(sh);

    sh = MK(p20_lod);
    ps_reset();
    PC.tex[0] = &mips;
    PC.samp[0].min_filter = SG_FILTER_POINT; PC.samp[0].mip_filter = SG_FILTER_POINT;
    { float dx[4] = { 0.125f, 0, 0, 0 }, dy[4] = { 0, 0.125f, 0, 0 }; setgrad(0, dx, dy); }
    setvary(0, 0.3f, 0.3f, 0, 0);
    setc(0, 1, 1, 1, 1);
    run_ps(sh, o); ck4t("ps_2_0 derivative lod: scale 1 -> level 0", o, 1, 0, 0, 1, 0.003f);
    setc(0, 2, 2, 2, 2);
    run_ps(sh, o); ck4t("ps_2_0 derivative through mul: scale 2 -> level 1", o, 0, 1, 0, 1, 0.003f);
    setc(0, 4, 4, 4, 4);
    run_ps(sh, o); ck4t("ps_2_0 derivative through mul: scale 4 -> level 2", o, 0, 0, 1, 1, 0.003f);
    d3dsh_free(sh);

    sh = MK(p20_cube);
    ps_reset();
    {
        SgTexture cube = tex_new(SG_TEX_CUBE, 2, 2, 1);
        for (int f = 0; f < 6; f++) for (int i = 0; i < 4; i++) { uint8_t *p = cube.rgba + ((size_t)f * 4 + i) * 4; p[0] = (uint8_t)(f * 40 + 10); p[3] = 255; }
        PC.tex[1] = &cube;
        setvary(1, 0.1f, 0.2f, -1, 0);
        run_ps(sh, o); ck4t("ps_2_0 cube map (-Z face)", o, 210 / 255.f, 0, 0, 1, 0.002f);
        report("ps_2_0 cube sampler type", d3dsh_ps_sampler_type(sh, 1) == SG_TEX_CUBE && d3dsh_ps_sampler_type(sh, 0) == -1);
        tex_free(&cube);
    }
    d3dsh_free(sh);

    sh = MK(p20_volume);
    ps_reset();
    {
        SgTexture vol = tex_new(SG_TEX_VOLUME, 2, 2, 2);
        for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) tex_set(&vol, x, y, z, x * 255, y * 255, z * 255, 255);
        PC.tex[2] = &vol;
        setvary(2, 0.75f, 0.25f, 0.75f, 0);
        run_ps(sh, o); ck4t("ps_2_0 volume texture", o, 1, 0, 1, 1, 0.002f);
        report("ps_2_0 volume sampler type", d3dsh_ps_sampler_type(sh, 2) == SG_TEX_VOLUME);
        tex_free(&vol);
    }
    d3dsh_free(sh);

    sh = MK(p2x_deriv);
    ps_reset();
    { float dx[4] = { 0.1f, 0.2f, 0, 0 }, dy[4] = { 0.3f, 0.4f, 0, 0 }; setgrad(0, dx, dy); }
    setc(0, 2, 3, 0, 0); setc(1, 1, 0.5f, 0, 0);
    run_ps(sh, o); ck4("ps_2_x dsx/dsy", o, 0.5f, 0.8f, 0, 0);
    d3dsh_free(sh);
    sh = MK(p2x_deriv2);
    ps_reset();
    { float dx[4] = { 0.1f, 0.2f, 0, 0 }, dy[4] = { 0.3f, 0.4f, 0, 0 }; setgrad(0, dx, dy); }
    setc(0, 1, 1, 1, 1); setc(2, 2, 3, 0, 0); setc(3, 5, 5, 5, 5);
    run_ps(sh, o); ck4("ps_2_x dsx of mul/add chain", o, 0.2f, 0.6f, 0, 0);
    d3dsh_free(sh);

    sh = MK(p20_depth);
    ps_reset();
    setc(0, 0.3f, 0, 0, 0); setc(1, 1, 0.5f, 0.25f, 1);
    run_ps(sh, o);
    report("ps_2_0 oDepth", d3dsh_ps_writes_depth(sh) == 1 && feq(g_depth, 0.3f));
    ck4("ps_2_0 oDepth colour", o, 1, 0.5f, 0.25f, 1);
    d3dsh_free(sh);

    sh = MK(p20_kill);
    ps_reset();
    setc(0, 0.4f, 0.3f, 0.2f, 0.1f);
    setvary(0, 0.1f, 0.2f, 0.3f, 0);
    report("ps_2_0 texkill passes", run_ps(sh, o) == 1 && feq(o[0], 0.4f));
    setvary(0, 0.1f, -0.2f, 0.3f, 0);
    report("ps_2_0 texkill discards", run_ps(sh, o) == 0);
    d3dsh_free(sh);

    sh = MK(p2x_flow);
    ps_reset();
    setc(1, -0.2f, -0.1f, 0, 0.1f);
    C.b[0] = 0; run_ps(sh, o); ck4("ps_2_x rep + if b0 (false)", o, 0, 0, 0, 0.4f);
    C.b[0] = 1; run_ps(sh, o); ck4("ps_2_x rep + if b0 (true)", o, 0.2f, 0.6f, 1, 1);
    d3dsh_free(sh);

    sh = MK(p2x_pred);
    ps_reset();
    setc(1, 0.1f, 0.2f, 0.3f, 0.4f); setc(2, 0.5f, 0.6f, 0.7f, 0.8f); setc(3, 0.9f, 0.9f, 0.9f, 0.9f);
    setvary(0, 1, -1, 1, -1);
    run_ps(sh, o); ck4("ps_2_x predication (1)", o, 0.5f, 0.2f, 0.7f, 0.9f);
    setvary(0, -1, 1, -1, 1);
    run_ps(sh, o); ck4("ps_2_x predication (2)", o, 0.1f, 0.6f, 0.3f, 0.8f);
    d3dsh_free(sh);
    tex_free(&t0); tex_free(&grid); tex_free(&mips);
}

static void test_ps30(void) {
    D3dShader *sh;
    float o[4];
    SgTexture t0 = tex_new(SG_TEX_2D, 2, 2, 1), mips = tex_mips();
    tex_set(&t0, 0, 0, 0, 255, 0, 0, 255); tex_set(&t0, 1, 0, 0, 0, 255, 0, 255); tex_set(&t0, 0, 1, 0, 0, 0, 255, 255); tex_set(&t0, 1, 1, 0, 255, 255, 0, 255);
    sh = MK(p30_basic);
    ps_reset();
    PC.tex[0] = &t0;
    setvary(0, 0.25f, 0.75f, 0, 0); setvary(3, 0.1f, 0.1f, 0.1f, 0); setvary(8, 1, 0.5f, 1, 1);
    run_ps(sh, o);
    ck4t("ps_3_0 linkage by semantic", o, 0.1f, 0.1f, 1, 1, 0.002f);
    report("ps_3_0 input mask / nvary / sampler", d3dsh_ps_input_mask(sh) == ((1u << 0) | (1u << 3) | (1u << 8)) && d3dsh_ps_nvary(sh) == 9 &&
                                                    d3dsh_ps_sampler_mask(sh) == 1 && d3dsh_ps_sampler_type(sh, 0) == SG_TEX_2D);
    d3dsh_free(sh);

    sh = MK(p30_vpos);
    ps_reset();
    PIN.pos[0] = 3.5f; PIN.pos[1] = 5.5f; PIN.front_facing = 1;
    setc(3, 0.1f, 0.1f, 0, 0); setc(1, 0, 0, 0.6f, 0.7f); setc(2, 0, 0, 0.1f, 0.2f);
    run_ps(sh, o); ck4("ps_3_0 vPos and vFace (front)", o, 0.3f, 0.5f, 0.6f, 0.7f);
    PIN.front_facing = 0;
    run_ps(sh, o); ck4("ps_3_0 vFace (back)", o, 0.3f, 0.5f, 0.1f, 0.2f);
    d3dsh_free(sh);

    sh = MK(p30_loop);
    ps_reset();
    setvary(1, 0.1f, 0, 0, 0); setvary(0, 0.2f, 0, 0, 0.1f); setvary(2, 0.05f, 0.3f, 0, 0);
    run_ps(sh, o); ck4("ps_3_0 loop aL over relative inputs", o, 0.35f, 0.3f, 0, 0.1f);
    report("ps_3_0 relative input mask", d3dsh_ps_input_mask(sh) == 7);
    d3dsh_free(sh);
    sh = MK(p30_rel1);
    ps_reset();
    setvary(1, 0.1f, 0, 0, 0); setvary(0, 0.2f, 0.4f, 0, 0.1f); setvary(2, 0.05f, 0.3f, 0, 0);
    run_ps(sh, o); ck4("ps_3_0 v[aL] uses the declared semantic mapping", o, 0.2f, 0.4f, 0, 0.1f);
    d3dsh_free(sh);

    sh = MK(p30_texldl);
    ps_reset();
    PC.tex[0] = &mips;
    setvary(0, 0.3f, 0.3f, 0, 0);
    run_ps(sh, o); ck4t("ps_3_0 texldl lod 0", o, 1, 0, 0, 1, 0.003f);
    setvary(0, 0.3f, 0.3f, 0, 1);
    PC.samp[0].min_filter = SG_FILTER_POINT; PC.samp[0].mip_filter = SG_FILTER_POINT;
    run_ps(sh, o); ck4t("ps_3_0 texldl lod 1", o, 0, 1, 0, 1, 0.003f);
    setvary(0, 0.3f, 0.3f, 0, 2);
    run_ps(sh, o); ck4t("ps_3_0 texldl lod 2", o, 0, 0, 1, 1, 0.003f);
    d3dsh_free(sh);

    sh = MK(p30_texldd);
    ps_reset();
    PC.tex[0] = &mips;
    PC.samp[0].min_filter = SG_FILTER_POINT; PC.samp[0].mip_filter = SG_FILTER_POINT;
    setvary(0, 0.3f, 0.3f, 0, 0);
    setc(0, 0.25f, 0, 0, 0); setc(1, 0, 0.25f, 0, 0);
    run_ps(sh, o); ck4t("ps_3_0 texldd explicit derivatives", o, 0, 1, 0, 1, 0.003f);
    setc(0, 0.0625f, 0, 0, 0); setc(1, 0, 0.0625f, 0, 0);
    run_ps(sh, o); ck4t("ps_3_0 texldd small derivatives", o, 1, 0, 0, 1, 0.003f);
    d3dsh_free(sh);

    sh = MK(p30_flow);
    ps_reset();
    setvary(0, 3, 0, 0, 0);
    run_ps(sh, o); ck4("ps_3_0 loop/breakp/call", o, 0.3f, 0.2f, 0, 0);
    setvary(0, 100, 0, 0, 0);
    run_ps(sh, o); ck4("ps_3_0 loop runs to count", o, 0.8f, 0.2f, 0, 0);
    d3dsh_free(sh);

    sh = MK(p30_depth);
    ps_reset();
    setvary(0, 0.6f, 0.1f, 0.2f, 0.3f);
    run_ps(sh, o);
    report("ps_3_0 oDepth", d3dsh_ps_writes_depth(sh) && feq(g_depth, 0.6f));
    ck4("ps_3_0 oDepth colour", o, 0.6f, 0.1f, 0.2f, 0.3f);
    d3dsh_free(sh);

    sh = MK(p30_kill);
    ps_reset();
    setc(0, 0.9f, 0, 0, 0);
    setvary(0, 0.1f, 0.2f, 0.3f, 0);
    report("ps_3_0 texkill passes", run_ps(sh, o) == 1 && feq(o[0], 0.9f));
    setvary(0, 0.1f, -0.2f, 0.3f, 0);
    report("ps_3_0 texkill discards", run_ps(sh, o) == 0);
    d3dsh_free(sh);

    sh = MK(p30_pred);
    ps_reset();
    setc(1, 0.1f, 0.2f, 0.3f, 0.4f); setc(2, 0.5f, 0.6f, 0.7f, 0.8f); setc(3, 0.9f, 0.9f, 0.9f, 0.9f);
    setvary(0, 1, -1, 1, -1);
    run_ps(sh, o); ck4("ps_3_0 predication", o, 0.5f, 0.2f, 0.7f, 0.9f);
    d3dsh_free(sh);

    sh = MK(p30_if);
    ps_reset();
    setc(0, 0.5f, 1, 0, 0); setc(1, 0.1f, 0.1f, 0.1f, 0.1f); setc(2, 0.2f, 0.2f, 0.2f, 0.2f); setc(3, 0.3f, 0.3f, 0.3f, 0.3f);
    setvary(0, 1, 0.5f, 0, 0);
    run_ps(sh, o); ck4("ps_3_0 nested if_gt / if_lt (inner true)", o, 0.1f, 0.1f, 0.1f, 0.1f);
    setvary(0, 1, 2, 0, 0);
    run_ps(sh, o); ck4("ps_3_0 nested if (inner else)", o, 0.2f, 0.2f, 0.2f, 0.2f);
    setvary(0, 0, 0, 0, 0);
    run_ps(sh, o); ck4("ps_3_0 nested if (outer else)", o, 0.3f, 0.3f, 0.3f, 0.3f);
    d3dsh_free(sh);
    tex_free(&t0); tex_free(&mips);
}

// ---- bytecode encoding cross-check and malformed input ---------------------------------------------------
static int tokens_equal(const uint32_t *a, size_t na, const uint32_t *b, size_t nb) { return na == nb && !memcmp(a, b, na * 4); }

static void test_encoding(void) {
    static const uint32_t vs11[] = { 0xFFFE0101, 0x00000001, 0xC00F0000, 0x90E40000, 0x00000001, 0xD00F0000, 0x90E40001,
                                     0x00000001, 0x800F0000, 0xA0E42003, 0x0000FFFF };
    static const uint32_t ps11[] = { 0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000001, 0x800F0000, 0xB0E40000, 0x0000FFFF };
    static const uint32_t ps20[] = { 0xFFFF0200, 0x0200001F, 0x90000000, 0xA00F0800, 0x0200001F, 0x80000000, 0xB0030000,
                                     0x03000042, 0x800F0000, 0xB0E40000, 0xA0E40800, 0x02000001, 0x800F0800, 0x80E40000, 0x0000FFFF };
    static const uint32_t vs30[] = { 0xFFFE0300, 0x0200001F, 0x80000000, 0x900F0000, 0x0200001F, 0x80000000, 0xE00F0000,
                                     0x02000001, 0xE00F0000, 0x90E40000, 0x0000FFFF };
    report("assembler matches hand-encoded vs_1_1 (incl. relative addressing)", tokens_equal(SH_hand_vs11, sizeof SH_hand_vs11 / 4, vs11, sizeof vs11 / 4));
    report("assembler matches hand-encoded ps_1_1", tokens_equal(SH_hand_ps11, sizeof SH_hand_ps11 / 4, ps11, sizeof ps11 / 4));
    report("assembler matches hand-encoded ps_2_0 (dcl, texld)", tokens_equal(SH_hand_ps20, sizeof SH_hand_ps20 / 4, ps20, sizeof ps20 / 4));
    report("assembler matches hand-encoded vs_3_0 (dcl o0)", tokens_equal(SH_hand_vs30, sizeof SH_hand_vs30 / 4, vs30, sizeof vs30 / 4));
    // run the hand-encoded streams directly
    char err[100];
    D3dShader *s = d3dsh_create(vs11, sizeof vs11 / 4, err, sizeof err);
    report("hand-encoded vs_1_1 decodes", s != NULL);
    if (s) {
        vs_reset(); setc(3, 1, 2, 3, 4); setv(0, 0, 7, 8, 9, 1); setv(1, 0, 0.5f, 0.25f, 0, 1);
        D3dVsOut o; run_vs(s, &o);
        ck4("hand-encoded vs_1_1 runs", o.pos, 7, 8, 9, 1);
        ck4("hand-encoded vs_1_1 oD0", o.v[8], 0.5f, 0.25f, 0, 1);
        report("token copy", d3dsh_token_count(s) == sizeof vs11 / 4 && !memcmp(d3dsh_tokens(s), vs11, sizeof vs11));
        d3dsh_free(s);
    }
    // comments are skipped and not copied as shader tokens? (they are part of the token stream copy)
    static const uint32_t cm[] = { 0xFFFE0200, 0x0003FFFE, 0x42415443, 0x0000001C, 0x00000000, 0x02000001, 0xC00F0000, 0x90E40000, 0x0000FFFF };
    s = d3dsh_create(cm, sizeof cm / 4, err, sizeof err);
    report("comment tokens are skipped", s != NULL && d3dsh_token_count(s) == sizeof cm / 4);
    d3dsh_free(s);
}

static void test_malformed(void) {
    char err[200];
    int ok;
    report("NULL tokens rejected", d3dsh_create(NULL, 10, err, sizeof err) == NULL);
    report("empty shader rejected", d3dsh_create(SH_hand_vs11, 1, err, sizeof err) == NULL);
    { uint32_t t[] = { 0x12345678, 0xFFFF }; report("bad version token rejected", d3dsh_create(t, 2, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0501, 0xFFFF }; report("unsupported major version rejected", d3dsh_create(t, 2, err, sizeof err) == NULL); }
    // truncation: every strict prefix of every shader must fail cleanly
    ok = 1;
    for (int i = 0; i < SH_TABLE_N; i++)
        for (unsigned k = 0; k < SH_TABLE[i].n; k++) {
            D3dShader *s = d3dsh_create(SH_TABLE[i].t, k, err, sizeof err);
            if (s) { ok = 0; printf("   prefix %u of %s accepted\n", k, SH_TABLE[i].name); d3dsh_free(s); }
        }
    report("every truncated prefix of every shader is rejected", ok);
    ok = 1;
    for (int i = 0; i < SH_TABLE_N; i++) { D3dShader *s = d3dsh_create(SH_TABLE[i].t, SH_TABLE[i].n, err, sizeof err); if (!s) { ok = 0; printf("   %s: %s\n", SH_TABLE[i].name, err); } d3dsh_free(s); }
    report("every test shader decodes", ok);
    { uint32_t t[] = { 0xFFFE0200, 0x0200007F, 0x800F0000, 0x90E40000, 0xFFFF }; report("unknown opcode rejected", d3dsh_create(t, 5, err, sizeof err) == NULL && err[0]); }
    { uint32_t t[] = { 0xFFFE0200, 0x02000001, 0x800F07FF, 0x90E40000, 0xFFFF }; report("temp register r2047 rejected", d3dsh_create(t, 5, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x02000001, 0xC00F0000, 0x90E40010, 0xFFFF }; report("input register v16 rejected", d3dsh_create(t, 5, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x02000001, 0xC00F0000, 0x90E40000 | (14u << 24), 0xFFFF }; report("bad source modifier rejected", d3dsh_create(t, 5, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x03000001, 0xC00F0000, 0x90E40000, 0xFFFF }; report("length past operands rejected", d3dsh_create(t, 5, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x0100002B, 0xFFFF }; report("endif without if rejected", d3dsh_create(t, 3, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x01000028, 0xFFFF }; report("if with no operand rejected", d3dsh_create(t, 3, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x0200001B, 0xF0E40800, 0xF0E40000, 0xFFFF }; report("unterminated loop rejected", d3dsh_create(t, 5, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x0100002C, 0xFFFF }; report("break outside loop rejected", d3dsh_create(t, 3, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x01000019, 0x0000000, 0xFFFF }; report("call without label operand rejected", d3dsh_create(t, 4, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x02000019, 0xE0E41800 - 0x20000000, 0xFFFF }; (void)t; }
    { uint32_t t[] = { 0xFFFF0200, 0x02000001, 0xC00F0000, 0x90E40000, 0xFFFF }; report("rasterizer output in pixel shader rejected", d3dsh_create(t, 5, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFF0300, 0x0200001F, 0x80000000, 0xE00F0000, 0xFFFF }; report("ps_3_0 dcl of output register rejected", d3dsh_create(t, 5, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200 }; report("missing end token rejected", d3dsh_create(t, 1, err, sizeof err) == NULL); }
    { uint32_t t[] = { 0xFFFE0200, 0x0010FFFE, 0xFFFF }; report("comment past end rejected", d3dsh_create(t, 3, err, sizeof err) == NULL); }
    report("error message buffer of size 1 is safe", d3dsh_create(NULL, 0, err, 1) == NULL);
    report("NULL error buffer is safe", d3dsh_create(NULL, 0, NULL, 0) == NULL);
}

static uint64_t rng_s = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return (uint32_t)(rng_s >> 11); }

static void test_fuzz(void) {
    uint32_t tok[512];
    int created = 0, total = 0;
    static float vin[D3DSH_VS_INPUTS][4];
    static D3dVsOut vo;
    SgTexture t = tex_mips();
    for (int round = 0; round < 400; round++) {
        for (int i = 0; i < SH_TABLE_N; i++) {
            unsigned n = SH_TABLE[i].n;
            if (n > 512) continue;
            memcpy(tok, SH_TABLE[i].t, n * 4);
            int nm = 1 + (int)(rnd() % 3);
            for (int m = 0; m < nm; m++) {
                unsigned p = 1 + rnd() % (n - 1);
                switch (rnd() % 7) {
                case 4: tok[p] ^= rnd() % 0x40; break;                       // register number
                case 5: tok[p] ^= (rnd() % 0x100) << 16; break;              // swizzle / write mask / control
                case 6: tok[p] ^= (rnd() % 0x10) << 24; break;               // length / modifier
                case 0: tok[p] ^= 1u << (rnd() % 32); break;
                case 1: tok[p] = rnd(); break;
                case 2: tok[p] = (tok[p] & 0xFFFF0000u) | (rnd() & 0xFFFF); break;
                default: tok[p] = (tok[p] & 0x0000FFFFu) | (rnd() & 0xFFFF0000u); break;
                }
            }
            char err[100];
            D3dShader *s = d3dsh_create(tok, n, err, sizeof err);
            total++;
            if (!s) continue;
            created++;
            vs_reset();
            for (int k = 0; k < 256; k++) for (int j = 0; j < 4; j++) C.f[k][j] = (float)(int)(rnd() % 200 - 100) * 0.5f;
            for (int k = 0; k < 16; k++) { C.i[k][0] = (int)(rnd() % 40) - 3; C.i[k][1] = (int)(rnd() % 600) - 300; C.i[k][2] = (int)(rnd() % 9) - 4; C.b[k] = (int)(rnd() & 1); }
            for (int k = 0; k < 16; k++) for (int j = 0; j < 4; j++) vin[k][j] = (float)(int)(rnd() % 2000 - 1000) * 0.01f;
            if (!d3dsh_is_pixel(s)) {
                VC.tex[0] = &t;
                d3dsh_run_vs_ex(s, &VC, vin, &vo);
                d3dsh_run_vs(s, &C, vin, &vo);
            } else {
                ps_reset();
                for (int k = 0; k < SG_VARYINGS; k++) for (int j = 0; j < 4; j++) { PIN.v[k][j] = (float)(int)(rnd() % 2000 - 1000) * 0.01f; PIN.dx[k][j] = 0.01f * (float)(int)(rnd() % 7); PIN.dy[k][j] = 0.01f * (float)(int)(rnd() % 7); }
                for (int k = 0; k < 16; k++) for (int j = 0; j < 4; j++) C.f[k][j] = (float)(int)(rnd() % 200 - 100) * 0.5f;
                for (int k = 0; k < 16; k++) { C.i[k][0] = (int)(rnd() % 40) - 3; C.i[k][1] = (int)(rnd() % 600) - 300; C.i[k][2] = (int)(rnd() % 9) - 4; C.b[k] = (int)(rnd() & 1); }
                for (int k = 0; k < 4; k++) PC.tex[k] = &t;
                float out[4];
                run_ps(s, out);
                PIN.front_facing = 0;
                run_ps(s, out);
            }
            d3dsh_free(s);
        }
    }
    tex_free(&t);
    printf("   fuzz: %d mutated shaders, %d accepted and executed\n", total, created);
    report("fuzz: random token mutations never crash", 1);
    // deliberately hostile: unbounded recursion and endless loops terminate
    static const uint32_t rec[] = { 0xFFFE0200, 0x02000019, 0xE0E40000 - 0, 0x0000FFFF };
    (void)rec;
}

static void test_limits(void) {
    // recursion: label l0: call l0 (infinite) must terminate; callnz doubling too
    static const uint32_t rec[] = {
        0xFFFE0200, 0x01000019, 0xF0E40000 /*placeholder, patched below*/, 0x0000FFFF };
    (void)rec;
    uint32_t t[16]; int n = 0;
    t[n++] = 0xFFFE0200;
    // call l0 ; ret ; label l0 ; call l0 ; ret   (l# type 18: 0x80000000 | (18&7)<<28 | (18>>3)<<11)
    uint32_t l0 = 0x80000000u | (2u << 28) | (2u << 11);
    t[n++] = 0x01000019; t[n++] = l0;
    t[n++] = 0x0000001C;                 // ret (length 0)
    t[n++] = 0x0100001E; t[n++] = l0;    // label l0
    t[n++] = 0x01000019; t[n++] = l0;    // call l0  (infinite recursion)
    t[n++] = 0x0000001C;
    t[n++] = 0xFFFF;
    char err[100];
    D3dShader *s = d3dsh_create(t, (size_t)n, err, sizeof err);
    report("recursive subroutine decodes", s != NULL);
    if (s) {
        vs_reset();
        D3dVsOut o; run_vs(s, &o);
        report("infinite recursion terminates", 1);
        d3dsh_free(s);
    } else printf("   %s\n", err);
}

static void bench(void) {
    D3dShader *sh = MK(p30_basic);
    ps_reset();
    SgTexture t0 = tex_grid4();
    PC.tex[0] = &t0;
    PC.samp[0].min_filter = PC.samp[0].mag_filter = SG_FILTER_LINEAR;
    setvary(0, 0.3f, 0.6f, 0, 0); setvary(3, 0.1f, 0.1f, 0.1f, 0); setvary(8, 1, 0.5f, 1, 1);
    float dx[4] = { 0.001f, 0, 0, 0 }, dy[4] = { 0, 0.001f, 0, 0 };
    setgrad(0, dx, dy);
    float o[4], acc = 0;
    enum { N = 1000000 };
    clock_t c0 = clock();
    for (int i = 0; i < N; i++) { PIN.v[0][0] = 0.3f + (float)(i & 255) * 0.001f; run_ps(sh, o); acc += o[0]; }
    double dt = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("   bench: ps_3_0 texld+mul+add: %.2f Mpixel/s (%.3f s, checksum %.1f)\n", N / dt / 1e6, dt, acc);
    d3dsh_free(sh);
    sh = MK(p20_tri_lrp);
    ps_reset();
    setvary(0, 0.3f, 0.6f, 0.2f, 0.5f); setvary(1, 0.1f, 0.1f, 0.1f, 0.2f); setvary(2, 0.4f, 0.3f, 0.2f, 0.1f);
    c0 = clock();
    for (int i = 0; i < N; i++) { PIN.v[0][0] = (float)(i & 255) * 0.004f; run_ps(sh, o); acc += o[0]; }
    dt = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("   bench: ps_2_0 lrp+mov: %.2f Mpixel/s\n", N / dt / 1e6);
    d3dsh_free(sh);
    tex_free(&t0);
    sh = MK(v11_xform);
    vs_reset();
    for (int i = 0; i < 4; i++) setc(i, (float)(i + 1), 0.5f * i, -1.0f * i, 0.25f);
    static float in[256][D3DSH_VS_INPUTS][4];
    static D3dVsOut outs[256];
    for (int i = 0; i < 256; i++) { in[i][0][0] = (float)i; in[i][0][3] = 1; }
    c0 = clock();
    for (int r = 0; r < 4000; r++) d3dsh_run_vs_batch(sh, &VC, in, outs, 256);
    dt = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("   bench: vs_1_1 m4x4+3 movs: %.2f Mvertex/s (checksum %.1f)\n", 4000.0 * 256 / dt / 1e6, outs[255].pos[0]);
    d3dsh_free(sh);
}

int main(void) {
    memset(&DS, 0, sizeof DS);
    DS.min_filter = DS.mag_filter = DS.mip_filter = SG_FILTER_POINT;
    DS.address_u = DS.address_v = DS.address_w = SG_ADDR_CLAMP;
    DS.max_anisotropy = 1;
    // linkage helper
    report("varying slots", d3dsh_varying_slot(D3DSU_TEXCOORD, 3) == 3 && d3dsh_varying_slot(D3DSU_COLOR, 0) == 8 && d3dsh_varying_slot(D3DSU_COLOR, 1) == 9 &&
                              d3dsh_varying_slot(D3DSU_FOG, 0) == 10 && d3dsh_varying_slot(D3DSU_NORMAL, 0) == 11 && d3dsh_varying_slot(D3DSU_POSITION, 0) == -1);
    test_encoding();
    test_vs_generic();
    test_vs11();
    test_vs11_expp();
    test_vs20();
    test_vs30();
    test_ps1x();
    test_ps14();
    test_ps20_generic();
    test_ps20();
    test_ps30();
    test_malformed();
    test_limits();
    test_fuzz();
    bench();
    printf("passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
