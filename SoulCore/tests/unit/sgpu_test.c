// Unit test for src/sgpu.c: renders small scenes into a 64x64 target and checks pixels.
#include "sgpu.h"
#include "sgpu_program.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
static void report(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_pass++; else g_fail++;
}

#define W 64
#define H 64
#define BG 0xFF101010u

static SgTarget T;
static SgState S;
static uint32_t cbuf[W * H];
static float zbuf[W * H];
static uint8_t sbuf[W * H];

static void reset(void) {
    memset(&S, 0, sizeof S);
    S.vp_w = W; S.vp_h = H; S.depth_near = 0; S.depth_far = 1;
    S.depth_func = SG_LESS; S.depth_write = 1;
    S.blend_src = SG_ONE; S.blend_dst = SG_ZERO; S.alpha_func = SG_ALWAYS;
    S.front_ccw = 1; S.mask_r = S.mask_g = S.mask_b = S.mask_a = 1;
    S.line_width = 1; S.point_size = 1;
    T.color = cbuf; T.depth = zbuf; T.stencil = sbuf; T.w = W; T.h = H;
    for (int i = 0; i < W * H; i++) { cbuf[i] = BG; zbuf[i] = 1; sbuf[i] = 0; }
}
static uint32_t px(int x, int y) { return cbuf[y * W + x]; }
static int near_col(uint32_t c, int r, int g, int b, int a, int tol) {
    return abs((int)((c >> 16) & 255) - r) <= tol && abs((int)((c >> 8) & 255) - g) <= tol && abs((int)(c & 255) - b) <= tol && abs((int)(c >> 24) - a) <= tol;
}
static int ch(uint32_t c, int i) { return i == 0 ? (c >> 16) & 255 : i == 1 ? (c >> 8) & 255 : i == 2 ? c & 255 : c >> 24; }

static void vtx(SgVertex *v, float x, float y, float z, float w) {
    memset(v, 0, sizeof *v);
    v->x = x * w; v->y = y * w; v->z = z * w; v->w = w; // x,y,z given in NDC
    v->c[0] = v->c[1] = v->c[2] = v->c[3] = 1; v->fog = 1;
}
// quad over NDC rect (counter-clockwise), z in NDC units, 6 vertices
static void quad(SgVertex *v, float x0, float y0, float x1, float y1, float z) {
    vtx(&v[0], x0, y0, z, 1); vtx(&v[1], x1, y0, z, 1); vtx(&v[2], x1, y1, z, 1);
    vtx(&v[3], x0, y0, z, 1); vtx(&v[4], x1, y1, z, 1); vtx(&v[5], x0, y1, z, 1);
}
static void colour(SgVertex *v, int n, float r, float g, float b, float a) { for (int i = 0; i < n; i++) { v[i].c[0] = r; v[i].c[1] = g; v[i].c[2] = b; v[i].c[3] = a; } }

// ---- pixel functions ----
static int fn_v0(void *ctx, const SgPixelIn *in, float *c, float *d) { memcpy(c, in->v[0], 16); return 1; }
static int fn_discard(void *ctx, const SgPixelIn *in, float *c, float *d) { if (in->v[0][0] > 0.5f) return 0; c[0] = 0; c[1] = 1; c[2] = 0; c[3] = 1; return 1; }
static float g_dx[H][W], g_v[H][W], g_dy[H][W];
static int fn_record(void *ctx, const SgPixelIn *in, float *c, float *d) {
    int x = (int)in->pos[0], y = (int)in->pos[1];
    g_v[y][x] = in->v[0][0]; g_dx[y][x] = in->dx[0][0]; g_dy[y][x] = in->dy[0][1];
    c[0] = c[1] = c[2] = c[3] = 1; return 1;
}
static int fn_red(void *ctx, const SgPixelIn *in, float *c, float *d) { c[0] = 1; c[1] = 0; c[2] = 0; c[3] = 1; return 1; }
static int fn_face(void *ctx, const SgPixelIn *in, float *c, float *d) { c[0] = in->front_facing; c[1] = !in->front_facing; c[2] = 0; c[3] = 1; return 1; }
static int fn_depth(void *ctx, const SgPixelIn *in, float *c, float *d) { c[0] = 0; c[1] = 1; c[2] = 0; c[3] = 1; *d = 0.25f; return 1; }

// textured quad with varyings 0 = (u in x, 0, 0, 1) going 0 (left) .. 1 (right); right edge has w = wr
static void varying_quad(SgVertex *v, float wr) {
    float xs[6] = { -1, 1, 1, -1, 1, -1 }, ys[6] = { -1, -1, 1, -1, 1, 1 };
    for (int i = 0; i < 6; i++) {
        float w = xs[i] > 0 ? wr : 1;
        vtx(&v[i], xs[i], ys[i], 0, w);
        v[i].v[0][0] = xs[i] > 0 ? 1.f : 0.f; v[i].v[0][3] = 1;
    }
}

static void test_varyings(void) {
    SgVertex v[6];
    reset();
    S.pixel_fn = fn_v0; S.nvary = 1;
    varying_quad(v, 4);
    sg_triangles(&T, &S, v, 6);
    int ok = 1;
    for (int x = 0; x < W; x += 7) {
        float t = (x + 0.5f) / 64.0f;
        float u = (t / 4) / ((1 - t) + t / 4);
        if (!near_col(px(x, 31), (int)(u * 255 + 0.5f), 0, 0, 255, 1)) ok = 0;
    }
    report("varyings: perspective-correct interpolation", ok);
    report("varyings: perspective differs from affine", ch(px(31, 31), 0) < 70 && ch(px(31, 31), 0) > 40);
    reset();
    S.pixel_fn = fn_v0; S.nvary = 1;
    varying_quad(v, 1);
    sg_triangles(&T, &S, v, 6);
    report("varyings: w=1 is linear (affine == perspective)", near_col(px(15, 20), 62, 0, 0, 255, 1) && near_col(px(47, 20), 189, 0, 0, 255, 1));
    // discard
    reset();
    S.pixel_fn = fn_discard; S.nvary = 1;
    varying_quad(v, 4);
    sg_triangles(&T, &S, v, 6);
    report("pixel_fn: discard", px(40, 30) == 0xFF00FF00u && px(55, 30) == BG && px(2, 2) == 0xFF00FF00u);
    // varying clipped by the far plane keeps interpolation
    reset();
    S.pixel_fn = fn_v0; S.nvary = 1; S.clip_z_zero = 1;
    {
        float xs[6] = { -1, 1, 1, -1, 1, -1 }, ys[6] = { -1, -1, 1, -1, 1, 1 };
        for (int i = 0; i < 6; i++) { vtx(&v[i], xs[i], ys[i], xs[i] > 0 ? 1.5f : 0.5f, 1); v[i].v[0][0] = xs[i] > 0 ? 1.f : 0.f; v[i].v[0][3] = 1; }
    }
    sg_triangles(&T, &S, v, 6);
    report("clipping: far plane interpolates varyings", near_col(px(16, 32), 64, 0, 0, 255, 2) && px(40, 32) == BG);
    // depth range of D3D clip space: z < 0 is clipped
    reset();
    S.clip_z_zero = 1; S.pixel_fn = fn_red;
    quad(v, -1, -1, 1, 1, -0.5f);
    sg_triangles(&T, &S, v, 6);
    report("clipping: D3D near plane z>=0", px(32, 32) == BG);
}

static void test_derivs(void) {
    SgVertex v[6];
    reset();
    S.pixel_fn = fn_record; S.nvary = 1;
    // u = 0..1 left to right, v (component 1) not used; w = 1
    varying_quad(v, 1);
    memset(g_dx, 0, sizeof g_dx); memset(g_dy, 0, sizeof g_dy);
    sg_triangles(&T, &S, v, 6);
    report("derivatives: constant gradient dx = 1/64", fabsf(g_dx[20][10] - 1.0f / 64) < 1e-5f && fabsf(g_dx[50][60] - 1.0f / 64) < 1e-5f);
    reset();
    S.pixel_fn = fn_record; S.nvary = 1;
    varying_quad(v, 4);
    sg_triangles(&T, &S, v, 6);
    int ok = 1;
    for (int x = 4; x < 60; x += 11) {
        float fd = (g_v[30][x + 1] - g_v[30][x - 1]) * 0.5f;
        if (fabsf(g_dx[30][x] - fd) > fd * 0.02f + 1e-5f) ok = 0;
    }
    report("derivatives: perspective-correct analytic dx matches finite differences", ok);
}

static void stencil_setup(int func, int ref, int zpass) {
    S.stencil_test = 1;
    S.stencil[0].func = func; S.stencil[0].ref = ref; S.stencil[0].read_mask = 255; S.stencil[0].write_mask = 255;
    S.stencil[0].zpass_op = zpass;
}

static void test_stencil(void) {
    SgVertex v[6], full[6];
    quad(full, -1, -1, 1, 1, 0);
    colour(full, 6, 1, 0, 0, 1);
    reset();
    quad(v, -1, -1, 0, 1, 0);
    // write mask: stencil <- 5 on the left half, no colour
    stencil_setup(SG_ALWAYS, 5, SG_SOP_REPLACE);
    S.mask_r = S.mask_g = S.mask_b = S.mask_a = 0;
    sg_triangles(&T, &S, v, 6);
    report("stencil: REPLACE writes ref (colour masked)", sbuf[10 * W + 10] == 5 && sbuf[10 * W + 50] == 0 && px(10, 10) == BG);
    S.mask_r = S.mask_g = S.mask_b = S.mask_a = 1;
    stencil_setup(SG_EQUAL, 5, SG_SOP_KEEP);
    sg_triangles(&T, &S, full, 6);
    report("stencil: draw only where stencil == ref", px(10, 10) == 0xFFFF0000u && px(50, 10) == BG);
    // INCR / DECR / INCR_SAT / DECR_SAT / INVERT / ZERO
    int ops[] = { SG_SOP_INCR, SG_SOP_DECR, SG_SOP_INCR_SAT, SG_SOP_DECR_SAT, SG_SOP_INVERT, SG_SOP_ZERO };
    int init[] = { 255, 0, 255, 0, 0x0F, 77 };
    int want[] = { 0, 255, 255, 0, 0xF0, 0 };
    int ok = 1;
    for (int i = 0; i < 6; i++) {
        reset();
        memset(sbuf, init[i], sizeof sbuf);
        stencil_setup(SG_ALWAYS, 0, ops[i]);
        sg_triangles(&T, &S, full, 6);
        if (sbuf[5 * W + 5] != want[i]) ok = 0;
    }
    report("stencil: INCR/DECR (wrap), INCRSAT/DECRSAT, INVERT, ZERO", ok);
    // write mask and read mask
    reset();
    memset(sbuf, 0xF0, sizeof sbuf);
    stencil_setup(SG_ALWAYS, 0x0F, SG_SOP_REPLACE);
    S.stencil[0].write_mask = 0x3C;
    sg_triangles(&T, &S, full, 6);
    report("stencil: write mask", sbuf[0] == ((0xF0 & ~0x3C) | (0x0F & 0x3C)));
    reset();
    memset(sbuf, 0xA5, sizeof sbuf);
    stencil_setup(SG_EQUAL, 0x05, SG_SOP_KEEP);
    S.stencil[0].read_mask = 0x0F;
    sg_triangles(&T, &S, full, 6);
    report("stencil: read mask", px(3, 3) == 0xFFFF0000u);
    // fail op and depth-fail op
    reset();
    stencil_setup(SG_NEVER, 9, SG_SOP_KEEP);
    S.stencil[0].fail_op = SG_SOP_REPLACE;
    sg_triangles(&T, &S, full, 6);
    report("stencil: fail op, colour not written", sbuf[0] == 9 && px(0, 0) == BG);
    reset();
    for (int i = 0; i < W * H; i++) zbuf[i] = 0.2f;
    S.depth_test = 1;
    stencil_setup(SG_ALWAYS, 3, SG_SOP_KEEP);
    S.stencil[0].zfail_op = SG_SOP_REPLACE;
    sg_triangles(&T, &S, full, 6); // quad z=0.5 > 0.2 fails the depth test
    report("stencil: depth-fail op", sbuf[0] == 3 && px(0, 0) == BG);
    // two-sided
    reset();
    {
        SgVertex tri[6];
        vtx(&tri[0], -1, -1, 0, 1); vtx(&tri[1], 0, -1, 0, 1); vtx(&tri[2], -1, 1, 0, 1); // CCW = front
        vtx(&tri[3], 0, -1, 0, 1); vtx(&tri[4], 1, -1, 0, 1); vtx(&tri[5], 0, 1, 0, 1);
        SgVertex t2[3] = { tri[3], tri[5], tri[4] };   // clockwise = back, right part
        stencil_setup(SG_ALWAYS, 7, SG_SOP_REPLACE);
        S.stencil_two_sided = 1;
        S.stencil[1] = S.stencil[0];
        S.stencil[1].ref = 9;
        sg_triangles(&T, &S, tri, 3);
        sg_triangles(&T, &S, t2, 3);
    }
    report("stencil: two-sided (front uses [0], back uses [1])", sbuf[40 * W + 5] == 7 && sbuf[50 * W + 40] == 9);
    reset();
    S.pixel_fn = fn_face;
    {
        SgVertex tri[3], bk[3];
        vtx(&tri[0], -1, -1, 0, 1); vtx(&tri[1], 0, -1, 0, 1); vtx(&tri[2], -1, 1, 0, 1);
        bk[0] = tri[0]; bk[1] = tri[2]; bk[2] = tri[1];
        sg_triangles(&T, &S, tri, 3);
        report("pixel_fn: front_facing", near_col(px(3, 40), 255, 0, 0, 255, 0));
        reset(); S.pixel_fn = fn_face;
        sg_triangles(&T, &S, bk, 3);
        report("pixel_fn: back_facing", near_col(px(3, 40), 0, 255, 0, 255, 0));
    }
}

static void test_blend(void) {
    SgVertex v[6];
    quad(v, -1, -1, 1, 1, 0);
    colour(v, 6, 0.5f, 0.25f, 0.9f, 0.5f);
    const float s[4] = { 0.5f, 0.25f, 0.9f, 0.5f };
    const float d[4] = { 0.2f, 0.4f, 0.6f, 0.8f };
    const uint32_t dst = 0xCC336699u; // a=204 r=51 g=102 b=153
    struct { int op; const char *name; } ops[] = { { SG_BLEND_ADD, "ADD" }, { SG_BLEND_SUBTRACT, "SUBTRACT" }, { SG_BLEND_REV_SUBTRACT, "REVSUBTRACT" }, { SG_BLEND_MIN, "MIN" }, { SG_BLEND_MAX, "MAX" } };
    for (int k = 0; k < 5; k++) {
        reset();
        for (int i = 0; i < W * H; i++) cbuf[i] = dst;
        S.blend = 1; S.blend_src = SG_ONE; S.blend_dst = SG_ONE; S.blend_op = ops[k].op;
        sg_triangles(&T, &S, v, 6);
        int e[4];
        for (int i = 0; i < 4; i++) {
            float r = ops[k].op == SG_BLEND_ADD ? s[i] + d[i] : ops[k].op == SG_BLEND_SUBTRACT ? s[i] - d[i] : ops[k].op == SG_BLEND_REV_SUBTRACT ? d[i] - s[i] : ops[k].op == SG_BLEND_MIN ? fminf(s[i], d[i]) : fmaxf(s[i], d[i]);
            e[i] = r <= 0 ? 0 : r >= 1 ? 255 : (int)(r * 255 + 0.5f);
        }
        char nm[64]; snprintf(nm, sizeof nm, "blend op %s", ops[k].name);
        report(nm, near_col(px(20, 20), e[0], e[1], e[2], e[3], 1));
    }
    reset();
    for (int i = 0; i < W * H; i++) cbuf[i] = dst;
    S.blend = 1; S.blend_src = SG_SRC_ALPHA; S.blend_dst = SG_ONE_MINUS_SRC_ALPHA; S.blend_sep_alpha = 1; S.blend_src_a = SG_ONE; S.blend_dst_a = SG_ZERO;
    sg_triangles(&T, &S, v, 6);
    report("blend: separate alpha factors", near_col(px(9, 9), (int)((0.5f * 0.5f + 0.2f * 0.5f) * 255 + 0.5f), (int)((0.25f * 0.5f + 0.4f * 0.5f) * 255 + 0.5f), (int)((0.9f * 0.5f + 0.6f * 0.5f) * 255 + 0.5f), 128, 1));
    reset();
    for (int i = 0; i < W * H; i++) cbuf[i] = dst;
    S.blend = 1; S.blend_src = SG_CONSTANT_COLOR; S.blend_dst = SG_ONE_MINUS_CONSTANT_ALPHA;
    S.blend_color[0] = 1; S.blend_color[1] = 0.5f; S.blend_color[2] = 0; S.blend_color[3] = 0.25f;
    sg_triangles(&T, &S, v, 6);
    {
        float bc[4] = { 1, 0.5f, 0, 0.25f };
        int e[4];
        for (int i = 0; i < 4; i++) { float r = s[i] * bc[i] + d[i] * (1 - bc[3]); e[i] = r >= 1 ? 255 : (int)(r * 255 + 0.5f); }
        report("blend: constant colour / constant alpha factors", near_col(px(9, 9), e[0], e[1], e[2], e[3], 1));
    }
    reset();
    for (int i = 0; i < W * H; i++) cbuf[i] = dst;
    S.blend = 1; S.blend_src = SG_SRC_ALPHA_SATURATE; S.blend_dst = SG_ONE;
    sg_triangles(&T, &S, v, 6);
    {
        float f = fminf(0.5f, 1 - 0.8f);
        report("blend: SRCALPHASAT", near_col(px(9, 9), (int)((0.5f * f + 0.2f) * 255 + 0.5f), (int)((0.25f * f + 0.4f) * 255 + 0.5f), (int)((0.9f * f + 0.6f) * 255 + 0.5f), 255, 1));
    }
    reset();
    S.mask_g = 0;
    colour(v, 6, 1, 1, 1, 1);
    sg_triangles(&T, &S, v, 6);
    report("blend: colour write mask", near_col(px(9, 9), 255, 0x10, 255, 255, 0));
}

static void test_depth(void) {
    SgVertex v[6];
    reset();
    S.clip_z_zero = 1; S.depth_test = 1;
    quad(v, -1, -1, 1, 1, 0.5f);
    colour(v, 6, 1, 0, 0, 1);
    sg_triangles(&T, &S, v, 6);
    colour(v, 6, 0, 1, 0, 1);
    sg_triangles(&T, &S, v, 6);
    report("depth: equal depth fails LESS", px(5, 5) == 0xFFFF0000u);
    S.depth_bias = -0.1f;
    sg_triangles(&T, &S, v, 6);
    report("depth: constant bias", px(5, 5) == 0xFF00FF00u && fabsf(zbuf[5 * W + 5] - 0.4f) < 1e-5f);
    // slope-scaled bias
    reset();
    S.clip_z_zero = 1; S.depth_test = 1;
    {
        float xs[6] = { -1, 1, 1, -1, 1, -1 }, ys[6] = { -1, -1, 1, -1, 1, 1 };
        for (int i = 0; i < 6; i++) { vtx(&v[i], xs[i], ys[i], xs[i] > 0 ? 0.8f : 0.2f, 1); v[i].c[0] = 1; v[i].c[1] = v[i].c[2] = 0; }
    }
    sg_triangles(&T, &S, v, 6);
    colour(v, 6, 0, 0, 1, 1);
    sg_triangles(&T, &S, v, 6);
    int same = px(30, 30) == 0xFFFF0000u;
    S.slope_scale_bias = -1;
    sg_triangles(&T, &S, v, 6);
    report("depth: slope-scaled bias", same && px(30, 30) == 0xFF0000FFu);
    // depth clamp
    reset();
    S.clip_z_zero = 1; S.depth_test = 1; S.depth_clamp = 1; S.depth_func = SG_LEQUAL;
    quad(v, -1, -1, 1, 1, 1.5f);
    colour(v, 6, 1, 1, 0, 1);
    sg_triangles(&T, &S, v, 6);
    report("depth: clamp instead of far clipping", px(5, 5) == 0xFFFFFF00u && zbuf[5 * W + 5] == 1.0f);
    // clear with a depth value
    reset();
    S.stencil[0].write_mask = 255;
    sg_clear_ex(&T, &S, SG_CLEAR_DEPTH | SG_CLEAR_STENCIL | SG_CLEAR_COLOR, 0xFF123456u, 0.25f, 0x7F);
    report("clear: colour, depth value, stencil", px(1, 1) == 0xFF123456u && zbuf[63 * W + 63] == 0.25f && sbuf[0] == 0x7F);
    // pixel_fn depth output
    reset();
    S.depth_test = 1; S.pixel_fn = fn_depth; S.pixel_writes_depth = 1; S.clip_z_zero = 1;
    quad(v, -1, -1, 1, 1, 0.9f);
    sg_triangles(&T, &S, v, 6);
    report("pixel_fn: depth output", zbuf[10 * W + 10] == 0.25f);
}

static void test_clip_and_viewport(void) {
    SgVertex v[6];
    reset();
    quad(v, -1, -1, 1, 1, 0);
    colour(v, 6, 1, 0, 0, 1);
    S.clip_mask = 1; S.clip_plane[0][0] = 1; // x >= 0
    sg_triangles(&T, &S, v, 6);
    report("user clip plane", px(10, 30) == BG && px(50, 30) == 0xFFFF0000u);
    reset();
    S.clip_mask = 2; S.clip_plane[1][1] = -1; S.clip_plane[1][3] = 0; // y <= 0
    sg_triangles(&T, &S, v, 6);
    report("user clip plane (second slot, y)", px(30, 10) == BG && px(30, 50) == 0xFFFF0000u);
    reset();
    S.vp_w = 32; S.vp_h = 32; S.vp_x = 0; S.vp_y = 0;
    sg_triangles(&T, &S, v, 6);
    report("viewport: GL origin is bottom-left", px(10, 50) == 0xFFFF0000u && px(10, 10) == BG && px(50, 50) == BG);
    reset();
    S.vp_w = 32; S.vp_h = 32; S.vp_top_left = 1;
    sg_triangles(&T, &S, v, 6);
    report("viewport: top-left origin", px(10, 10) == 0xFFFF0000u && px(10, 50) == BG && px(50, 10) == BG);
    reset();
    S.vp_w = 32; S.vp_h = 32; S.vp_x = 32; S.vp_y = 32; S.vp_top_left = 1;
    sg_triangles(&T, &S, v, 6);
    report("viewport: top-left origin with offset", px(40, 40) == 0xFFFF0000u && px(10, 10) == BG && px(10, 40) == BG);
    reset();
    S.scissor = 1; S.sc_x = 40; S.sc_y = 10; S.sc_w = 10; S.sc_h = 10; S.vp_top_left = 1;
    sg_triangles(&T, &S, v, 6);
    {
        int n = 0, ok = 1;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (px(x, y) != BG) { n++; if (x < 40 || x >= 50 || y < 10 || y >= 20) ok = 0; }
        report("scissor: top-left origin", ok && n == 100);
    }
    reset();
    S.scissor = 1; S.sc_x = 0; S.sc_y = 0; S.sc_w = 64; S.sc_h = 8;
    sg_triangles(&T, &S, v, 6);
    report("scissor: GL origin (bottom rows)", px(5, 63) == 0xFFFF0000u && px(5, 55) == BG);
    // robustness: huge, NaN and degenerate geometry
    reset();
    {
        SgVertex g[3];
        vtx(&g[0], -1e30f, -1e30f, 0, 1); vtx(&g[1], 1e30f, -1e30f, 0, 1); vtx(&g[2], 0, 1e30f, 0, 1);
        sg_triangles(&T, &S, g, 3); // extreme values: must not crash (precision is not defined there)
        reset();
        vtx(&g[0], -1e5f, -1e5f, 0, 1); vtx(&g[1], 1e5f, -1e5f, 0, 1); vtx(&g[2], 0, 1e5f, 0, 1);
        colour(g, 3, 0, 1, 0, 1);
        sg_triangles(&T, &S, g, 3);
        report("robust: huge triangle covers the target", px(32, 32) == 0xFF00FF00u && px(1, 1) == 0xFF00FF00u && px(62, 62) == 0xFF00FF00u && px(1, 62) == 0xFF00FF00u);
        reset();
        vtx(&g[0], NAN, 0, 0, 1); vtx(&g[1], 1, 1, 0, 1); vtx(&g[2], 0, 1, 0, 1);
        sg_triangles(&T, &S, g, 3);
        vtx(&g[0], 0, 0, 0, NAN);
        sg_triangles(&T, &S, g, 3);
        vtx(&g[0], 0, 0, 0, 0); vtx(&g[1], 0, 0, 0, 0); vtx(&g[2], 0, 0, 0, 0);
        sg_triangles(&T, &S, g, 3);
        sg_lines(&T, &S, g, 2);
        sg_points(&T, &S, g, 3);
        report("robust: NaN / degenerate input", px(32, 32) == BG);
        reset();
        vtx(&g[0], -1, -1, 0, -1); vtx(&g[1], 1, -1, 0, 1); vtx(&g[2], 0, 1, 0, 1);
        colour(g, 3, 1, 1, 1, 1);
        sg_triangles(&T, &S, g, 3);
        report("robust: vertex behind the eye", 1);
    }
}

// Fill rule with the top-left viewport origin (Direct3D): pixel centres on a top or left edge are covered, those on a
// bottom or right edge are not. Quads are given in sgpu window coordinates where pixel (i, j) has its centre at i+0.5.
static void win_quad(SgVertex *v, float x0, float y0, float x1, float y1) { // window coordinates, y down; NDC is exact in binary
    quad(v, x0 / 32.0f - 1.0f, 1.0f - y0 / 32.0f, x1 / 32.0f - 1.0f, 1.0f - y1 / 32.0f, 0);
}
static void test_fill_rule(void) {
    SgVertex v[6];
    int n, ok;
    // quad whose edges pass exactly through pixel centres: rows/columns 16..47 are covered and nothing else
    reset(); S.vp_top_left = 1;
    win_quad(v, 16.5f, 16.5f, 48.5f, 48.5f); // D3D coordinates (16, 16)-(48, 48) plus the half-pixel shift
    // counter-clockwise on screen with y down is the front face for front_ccw = 0
    S.front_ccw = 0;
    colour(v, 6, 1, 0, 0, 1);
    sg_triangles(&T, &S, v, 6);
    n = 0; ok = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int in = x >= 16 && x < 48 && y >= 16 && y < 48;
        if (px(x, y) == 0xFFFF0000u) n++;
        if ((px(x, y) == 0xFFFF0000u) != in) ok = 0;
    }
    report("fill rule (top-left origin): quad through pixel centres covers rows/columns 16..47", ok && n == 32 * 32);
    // the same, one pixel centre off: edges at y = 16.0 / 48.0 and x = 16.0 / 48.0 contain exactly the same pixels
    reset(); S.vp_top_left = 1; S.front_ccw = 0;
    win_quad(v, 16.0f, 16.0f, 48.0f, 48.0f);
    colour(v, 6, 1, 0, 0, 1);
    sg_triangles(&T, &S, v, 6);
    n = 0; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (px(x, y) == 0xFFFF0000u) n++;
    report("fill rule (top-left origin): edges between pixel centres", n == 32 * 32 && px(16, 16) == 0xFFFF0000u && px(47, 47) == 0xFFFF0000u && px(15, 16) == BG && px(16, 15) == BG);
    // a horizontal strip one pixel high and a vertical strip one pixel wide
    reset(); S.vp_top_left = 1; S.front_ccw = 0;
    win_quad(v, 8.5f, 20.5f, 40.5f, 21.5f);
    colour(v, 6, 1, 0, 0, 1);
    sg_triangles(&T, &S, v, 6);
    n = 0; for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (px(x, y) == 0xFFFF0000u) n++;
    report("fill rule (top-left origin): one-pixel strip lies on row 20 only", n == 32 && px(8, 20) == 0xFFFF0000u && px(39, 20) == 0xFFFF0000u && px(8, 21) == BG && px(8, 19) == BG);
    // shared edges of adjacent triangles are drawn exactly once (additive blending exposes double coverage);
    // a fan of triangles around an interior point with edges through pixel centres
    reset(); S.vp_top_left = 1; S.front_ccw = 0;
    S.blend = 1; S.blend_src = SG_ONE; S.blend_dst = SG_ONE; S.blend_op = SG_BLEND_ADD;
    {
        SgVertex f[12];
        float cx = 32.5f, cy = 32.5f, L = 16.5f, R = 48.5f, Tp = 16.5f, B = 48.5f;
        float X[5] = { L, R, R, L, L }, Y[5] = { Tp, Tp, B, B, Tp };
        for (int i = 0; i < 4; i++) {
            vtx(&f[3 * i], cx / 32.0f - 1, 1 - cy / 32.0f, 0, 1);
            vtx(&f[3 * i + 1], X[i] / 32.0f - 1, 1 - Y[i] / 32.0f, 0, 1);
            vtx(&f[3 * i + 2], X[i + 1] / 32.0f - 1, 1 - Y[i + 1] / 32.0f, 0, 1);
        }
        colour(f, 12, 0.25f, 0.25f, 0.25f, 1);
        sg_triangles(&T, &S, f, 12);
        n = 0; ok = 1;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            int in = x >= 16 && x < 48 && y >= 16 && y < 48;
            uint32_t want = in ? 0xFF505050u : BG;
            if (px(x, y) != want) ok = 0;
            n += px(x, y) != BG;
        }
        report("fill rule (top-left origin): shared edges of a triangle fan are drawn exactly once", ok && n == 32 * 32);
    }
    // two triangles sharing a diagonal through pixel centres
    reset(); S.vp_top_left = 1; S.front_ccw = 0;
    S.blend = 1; S.blend_src = SG_ONE; S.blend_dst = SG_ONE; S.blend_op = SG_BLEND_ADD;
    win_quad(v, 16.5f, 16.5f, 48.5f, 48.5f);
    colour(v, 6, 0.25f, 0.25f, 0.25f, 1);
    sg_triangles(&T, &S, v, 6);
    n = 0; ok = 1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int in = x >= 16 && x < 48 && y >= 16 && y < 48;
        if (px(x, y) != (in ? 0xFF505050u : BG)) ok = 0;
    }
    report("fill rule (top-left origin): shared diagonal drawn once", ok);
}

static void test_points(void) {
    SgVertex p;
    reset();
    vtx(&p, 0, 0, 0, 1);
    S.pixel_fn = fn_v0; S.nvary = 2; S.point_sprite = 1; S.sprite_slot = 1; S.point_size = 8;
    // pixel fn reads varying 0; put the sprite coordinate there instead
    S.sprite_slot = 0; S.nvary = 1;
    sg_points(&T, &S, &p, 1);
    int n = 0;
    for (int i = 0; i < W * H; i++) if (cbuf[i] != BG) n++;
    report("point sprite: 8x8 pixels", n == 64 && px(28, 28) != BG && px(35, 35) != BG && px(27, 28) == BG && px(36, 28) == BG);
    report("point sprite: texture coordinates 0..1, top-left origin", near_col(px(28, 28), 16, 16, 0, 255, 1) && near_col(px(35, 35), 239, 239, 0, 255, 1) && near_col(px(35, 28), 239, 16, 0, 255, 1));
    reset();
    vtx(&p, 0, 0, 0, 1); p.psize = 4;
    S.pixel_fn = fn_v0; S.nvary = 1; S.point_sprite = 1; S.point_size_vertex = 1; S.sprite_slot = 0;
    sg_points(&T, &S, &p, 1);
    n = 0;
    for (int i = 0; i < W * H; i++) if (cbuf[i] != BG) n++;
    report("point sprite: size from the vertex", n == 16);
    reset();
    vtx(&p, 0, 0, 0, 1); p.c[0] = 0; p.c[1] = 1; p.c[2] = 0;
    S.point_size = 3;
    sg_points(&T, &S, &p, 1);
    n = 0;
    for (int i = 0; i < W * H; i++) if (cbuf[i] != BG) n++;
    report("point: plain square point", n == 9 && px(32, 32) == 0xFF00FF00u);
}

// ---- sg_sample ----
static uint8_t *mk(int n) { return calloc((size_t)n * 4, 1); }

static float samp1(const SgTexture *t, const SgSampler *s, float u, float v, float w, float *out_a) {
    float c[4] = { u, v, w, 0 }, o[4];
    sg_sample(t, s, c, NULL, NULL, 0.0f, o);
    if (out_a) *out_a = o[3];
    return o[0] * 255.0f;
}

static void test_sample(void) {
    SgTexture t;
    SgSampler s;
    memset(&t, 0, sizeof t);
    t.w = t.h = 4; t.rgba = mk(16);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { uint8_t *p = t.rgba + (y * 4 + x) * 4; p[0] = (uint8_t)(x * 60); p[1] = (uint8_t)(y * 60); p[2] = 0; p[3] = 255; }
    memset(&s, 0, sizeof s);
    s.min_filter = s.mag_filter = SG_FILTER_POINT; s.address_u = s.address_v = SG_ADDR_CLAMP;
    float c[4] = { 0.3f, 0.6f, 0, 0 }, o[4];
    sg_sample(&t, &s, c, NULL, NULL, NAN, o);
    report("sample: nearest", fabsf(o[0] * 255 - 60) < 0.5f && fabsf(o[1] * 255 - 120) < 0.5f && o[3] == 1);
    s.mag_filter = s.min_filter = SG_FILTER_LINEAR;
    c[0] = 0.5f; c[1] = 0.375f;
    sg_sample(&t, &s, c, NULL, NULL, NAN, o);
    report("sample: bilinear midpoint", fabsf(o[0] * 255 - 90) < 0.5f && fabsf(o[1] * 255 - 60) < 0.5f);
    // address modes: u = 1.125 (texel 4) and -0.125 (texel -1) and -1.125 (texel -5)
    int modes[] = { SG_ADDR_WRAP, SG_ADDR_MIRROR, SG_ADDR_CLAMP, SG_ADDR_BORDER, SG_ADDR_MIRROR_ONCE };
    float want_hi[] = { 0, 180, 180, -1, 180 }, want_lo[] = { 180, 0, 0, -1, 0 }, want_far[] = { 180 - 180 + 60 * 3 - 0, 180, 0, -1, 180 };
    // -1.125 -> texel -5: wrap = (-5 mod 4) = 3 -> 180; mirror = 3 -> 180; clamp = 0; border; mirror-once = 4 -> clamp 3 -> 180
    want_far[0] = 180; want_far[1] = 180; want_far[2] = 0;
    s.min_filter = s.mag_filter = SG_FILTER_POINT;
    s.border_argb = 0xFF00FF00u;
    int ok = 1;
    for (int m = 0; m < 5; m++) {
        s.address_u = modes[m];
        float hi = samp1(&t, &s, 1.125f, 0.1f, 0, NULL), lo = samp1(&t, &s, -0.125f, 0.1f, 0, NULL), far_ = samp1(&t, &s, -1.125f, 0.1f, 0, NULL);
        if (m == SG_ADDR_BORDER) { if (hi != 0 || lo != 0 || far_ != 0) ok = 0; continue; }
        if (fabsf(hi - want_hi[m]) > 0.5f || fabsf(lo - want_lo[m]) > 0.5f || fabsf(far_ - want_far[m]) > 0.5f) { ok = 0; printf("  mode %d: %f %f %f\n", m, hi, lo, far_); }
    }
    report("sample: wrap / mirror / clamp / mirror-once addressing", ok);
    s.address_u = SG_ADDR_BORDER;
    c[0] = 1.2f; c[1] = 0.1f;
    sg_sample(&t, &s, c, NULL, NULL, NAN, o);
    report("sample: border colour", o[0] == 0 && o[1] == 1 && o[2] == 0 && o[3] == 1);
    s.mag_filter = s.min_filter = SG_FILTER_LINEAR; s.address_v = SG_ADDR_CLAMP;
    c[0] = 1.0f; c[1] = 0.1f; // half of the right tap is the border colour
    sg_sample(&t, &s, c, NULL, NULL, NAN, o);
    report("sample: bilinear blends in the border", fabsf(o[0] * 255 - 90) < 0.5f && fabsf(o[1] - 0.5f) < 0.01f);
    sg_texture_free(&t);

    // mips
    memset(&t, 0, sizeof t);
    t.w = t.h = 8; t.rgba = mk(64);
    sg_texture_build_mips(&t);
    for (int l = 0; l < t.levels; l++) for (int i = 0; i < t.mip_w[l] * t.mip_h[l]; i++) { t.mip[l][i * 4] = (uint8_t)(10 + 100 * l); t.mip[l][i * 4 + 3] = 255; }
    memset(&s, 0, sizeof s);
    s.min_filter = s.mag_filter = SG_FILTER_POINT; s.mip_filter = SG_FILTER_POINT; s.address_u = s.address_v = SG_ADDR_CLAMP;
    float cc[4] = { 0.5f, 0.5f, 0, 0 }, dx[4] = { 1.0f / 8, 0, 0, 0 }, dy[4] = { 0, 1.0f / 8, 0, 0 };
    ok = t.levels == 4;
    sg_sample(&t, &s, cc, dx, dy, NAN, o); ok &= fabsf(o[0] * 255 - 10) < 0.5f;
    dx[0] = 2.0f / 8; dy[1] = 2.0f / 8;
    sg_sample(&t, &s, cc, dx, dy, NAN, o); ok &= fabsf(o[0] * 255 - 110) < 0.5f;
    dx[0] = 4.0f / 8; dy[1] = 1.0f / 8;
    sg_sample(&t, &s, cc, dx, dy, NAN, o); ok &= fabsf(o[0] * 255 - 210) < 0.5f;
    report("sample: mip level from derivatives (point)", ok);
    s.mip_filter = SG_FILTER_LINEAR;
    dx[0] = 1.5f / 8; dy[1] = 1.5f / 8;
    sg_sample(&t, &s, cc, dx, dy, NAN, o);
    float fr = log2f(1.5f);
    report("sample: trilinear blend of two levels", fabsf(o[0] * 255 - (10 + 100 * fr)) < 1.0f);
    sg_sample(&t, &s, cc, dx, dy, 2.0f, o);
    report("sample: explicit LOD override", fabsf(o[0] * 255 - 210) < 0.5f);
    s.mip_filter = SG_FILTER_POINT; s.lod_bias = 1;
    dx[0] = 1.0f / 8; dy[1] = 1.0f / 8;
    sg_sample(&t, &s, cc, dx, dy, NAN, o);
    report("sample: LOD bias", fabsf(o[0] * 255 - 110) < 0.5f);
    s.lod_bias = 0; s.max_mip_level = 2;
    sg_sample(&t, &s, cc, dx, dy, NAN, o);
    report("sample: max mip level", fabsf(o[0] * 255 - 210) < 0.5f);
    s.max_mip_level = 0; s.mip_filter = SG_FILTER_NONE;
    dx[0] = 4.0f / 8; dy[1] = 4.0f / 8;
    sg_sample(&t, &s, cc, dx, dy, NAN, o);
    report("sample: mip filter none stays on level 0", fabsf(o[0] * 255 - 10) < 0.5f);
    sg_texture_free(&t);

    // cube
    memset(&t, 0, sizeof t);
    t.kind = SG_TEX_CUBE; t.w = t.h = 4; t.rgba = mk(16 * 6);
    for (int f = 0; f < 6; f++) for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { uint8_t *p = t.rgba + ((f * 16) + y * 4 + x) * 4; p[0] = (uint8_t)(x * 60); p[1] = (uint8_t)(y * 60); p[2] = (uint8_t)(f * 40); p[3] = 255; }
    sg_texture_build_mips(&t);
    memset(&s, 0, sizeof s);
    s.min_filter = s.mag_filter = SG_FILTER_POINT;
    static const float dirs[6][4] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    ok = 1;
    for (int f = 0; f < 6; f++) {
        sg_sample(&t, &s, dirs[f], NULL, NULL, 0.0f, o);
        if (fabsf(o[2] * 255 - f * 40) > 0.5f || fabsf(o[0] * 255 - 120) > 0.5f || fabsf(o[1] * 255 - 120) > 0.5f) { ok = 0; printf("  face %d: %f %f %f\n", f, o[0] * 255, o[1] * 255, o[2] * 255); }
    }
    report("sample: cube face selection and centre", ok);
    float d2[4] = { 1, 0, -0.5f, 0 }; // +X face, sc = -z = 0.5 -> s = 0.75 -> texel 3
    sg_sample(&t, &s, d2, NULL, NULL, 0.0f, o);
    float d3[4] = { 0.3f, 0.2f, 2.0f, 0 }; // +Z: s = 0.5*(0.15+1) = 0.575 -> texel 2; t = 0.5*(-0.1+1) = 0.45 -> texel 1
    float o3[4];
    sg_sample(&t, &s, d3, NULL, NULL, 0.0f, o3);
    report("sample: cube in-face coordinates", fabsf(o[0] * 255 - 180) < 0.5f && o[2] * 255 < 0.5f && fabsf(o3[0] * 255 - 120) < 0.5f && fabsf(o3[1] * 255 - 60) < 0.5f && fabsf(o3[2] * 255 - 160) < 0.5f);
    sg_texture_free(&t);

    // volume
    memset(&t, 0, sizeof t);
    t.kind = SG_TEX_VOLUME; t.w = t.h = 2; t.depth = 2; t.rgba = mk(8);
    for (int z = 0; z < 2; z++) for (int i = 0; i < 4; i++) { uint8_t *p = t.rgba + (z * 4 + i) * 4; p[0] = (uint8_t)(50 + 100 * z); p[3] = 255; }
    memset(&s, 0, sizeof s);
    s.min_filter = s.mag_filter = SG_FILTER_POINT; s.address_u = s.address_v = s.address_w = SG_ADDR_CLAMP;
    float v1[4] = { 0.5f, 0.5f, 0.25f, 0 }, v2[4] = { 0.5f, 0.5f, 0.75f, 0 };
    sg_sample(&t, &s, v1, NULL, NULL, 0.0f, o); sg_sample(&t, &s, v2, NULL, NULL, 0.0f, o3);
    int vp = fabsf(o[0] * 255 - 50) < 0.5f && fabsf(o3[0] * 255 - 150) < 0.5f;
    s.min_filter = s.mag_filter = SG_FILTER_LINEAR;
    float v3[4] = { 0.5f, 0.5f, 0.5f, 0 };
    sg_sample(&t, &s, v3, NULL, NULL, 0.0f, o);
    report("sample: volume point and trilinear", vp && fabsf(o[0] * 255 - 100) < 0.5f);
    sg_texture_build_mips(&t);
    report("sample: volume mips (2x2x2 -> 1x1x1)", t.levels == 2 && t.mip_d[1] == 1 && abs(t.mip[1][0] - 100) <= 1);
    sg_texture_free(&t);

    // robustness
    memset(&t, 0, sizeof t);
    t.w = t.h = 4; t.rgba = mk(16);
    memset(&s, 0, sizeof s);
    s.min_filter = s.mag_filter = SG_FILTER_LINEAR; s.max_anisotropy = 8;
    float nn[4] = { NAN, INFINITY, -INFINITY, 0 };
    sg_sample(&t, &s, nn, nn, nn, NAN, o);
    sg_sample(&t, &s, nn, NULL, NULL, 0.0f, o);
    float big[4] = { 1e30f, -1e30f, 0, 0 };
    sg_sample(&t, &s, big, NULL, NULL, NAN, o);
    sg_sample(NULL, &s, big, NULL, NULL, NAN, o);
    report("sample: NaN / infinite / huge coordinates are safe", o[3] == 1);
    sg_texture_free(&t);
}


// ---- backend interface, texture identity, program key ----
typedef struct Rec {
    int draws, clears, sync, wrote, wrote_whole, gone, tgone, frames;
    int ret;                 // value returned by draw / clear
    int last_prim, last_n;
    const void *gone_c, *gone_d;
    uint64_t gone_tex;
} Rec;
static int rb_draw(void *self, SgTarget *t, const SgState *s, int prim, const SgVertex *v, int n) { Rec *r = self; (void)t; (void)s; (void)v; r->draws++; r->last_prim = prim; r->last_n = n; return r->ret; }
static int rb_clear(void *self, SgTarget *t, const SgState *s, int f, uint32_t a, float z, int st) { Rec *r = self; (void)t; (void)s; (void)f; (void)a; (void)z; (void)st; r->clears++; return r->ret; }
static void rb_sync(void *self, SgTarget *t) { Rec *r = self; (void)t; r->sync++; }
static void rb_wrote(void *self, SgTarget *t, int x, int y, int w, int h) { Rec *r = self; (void)t; (void)x; (void)y; r->wrote++; if (!w || !h) r->wrote_whole++; }
static void rb_gone(void *self, const void *c, const void *d, const void *st) { Rec *r = self; (void)st; r->gone++; r->gone_c = c; r->gone_d = d; }
static void rb_tgone(void *self, SgTexture *t) { Rec *r = self; r->tgone++; r->gone_tex = t->id; }
static void rb_frame(void *self) { ((Rec *)self)->frames++; }

static void test_backend(void) {
    Rec rec;
    memset(&rec, 0, sizeof rec);
    SgBackend be = { "test", &rec, rb_draw, rb_clear, rb_sync, rb_wrote, rb_gone, rb_tgone, rb_frame };
    SgVertex v[6];
    reset();
    quad(v, -1, -1, 1, 1, 0);
    colour(v, 6, 1, 0, 0, 1);

    report("backend: none installed by default", sg_backend_get() == NULL);
    sg_backend_set(&be);
    report("backend: sg_backend_get returns the installed backend", sg_backend_get() == &be);

    // the backend takes the draw: no CPU rasterisation, no sync, no cpu_wrote
    rec.ret = 0;
    sg_triangles(&T, &S, v, 6);
    report("backend: draw dispatched (triangles, n=6)", rec.draws == 1 && rec.last_prim == SG_PRIM_TRIANGLES && rec.last_n == 6);
    report("backend: accepted draw leaves the CPU target untouched", px(10, 10) == BG && rec.sync == 0 && rec.wrote == 0);
    sg_lines(&T, &S, v, 2);
    sg_points(&T, &S, v, 1);
    report("backend: lines and points dispatched", rec.draws == 3 && rec.last_prim == SG_PRIM_POINTS && rec.last_n == 1);
    sg_clear_ex(&T, &S, SG_CLEAR_COLOR, 0xFF123456u, 1, 0);
    report("backend: clear dispatched and accepted", rec.clears == 1 && px(0, 0) == BG && rec.sync == 0);

    // declined: sync to CPU, rasterise on the CPU, report cpu_wrote (whole)
    memset(&rec, 0, sizeof rec);
    rec.ret = -1;
    sg_triangles(&T, &S, v, 6);
    report("backend: declined draw falls back to the CPU rasteriser", rec.draws == 1 && near_col(px(10, 10), 255, 0, 0, 255, 0) && near_col(px(63, 63), 255, 0, 0, 255, 0));
    report("backend: fallback syncs before and reports cpu_wrote after", rec.sync == 1 && rec.wrote == 1 && rec.wrote_whole == 1);
    sg_clear_ex(&T, &S, SG_CLEAR_COLOR, 0xFF123456u, 1, 0);
    report("backend: declined clear falls back (CPU result, sync, cpu_wrote)", px(5, 5) == 0xFF123456u && rec.clears == 1 && rec.sync == 2 && rec.wrote == 2);

    // optional hooks may be NULL
    SgBackend bare = { "bare", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
    sg_backend_set(&bare);
    reset();
    sg_triangles(&T, &S, v, 6);
    sg_target_sync(&T); sg_target_cpu_wrote(&T, 0, 0, 0, 0); sg_target_gone(cbuf, NULL, NULL); sg_end_frame();
    report("backend: a backend without hooks behaves like the CPU", near_col(px(10, 10), 255, 0, 0, 255, 0));

    // front-end hooks reach the backend
    memset(&rec, 0, sizeof rec);
    sg_backend_set(&be);
    sg_target_sync(&T);
    sg_target_cpu_wrote(&T, 1, 2, 3, 4);
    sg_target_cpu_wrote(&T, 0, 0, 0, 0);
    sg_target_gone(cbuf, zbuf, NULL);
    sg_target_gone(NULL, NULL, NULL);
    sg_end_frame();
    report("backend: sync / cpu_wrote (rect, whole) / target_gone / end_frame forwarded",
           rec.sync == 1 && rec.wrote == 2 && rec.wrote_whole == 1 && rec.gone == 1 && rec.gone_c == cbuf && rec.gone_d == zbuf && rec.frames == 1);

    // nothing to draw: not dispatched as work
    memset(&rec, 0, sizeof rec);
    SgTarget nt = T; nt.color = NULL;
    sg_triangles(&nt, &S, v, 6);
    report("backend: target without colour is a no-op (no dispatch, no sync)", rec.draws == 0 && rec.sync == 0 && rec.wrote == 0);

    // texture identity
    SgTexture tx;
    memset(&tx, 0, sizeof tx);
    report("texture: zero struct has no id", tx.id == 0 && tx.version == 0);
    uint64_t id = sg_texture_id(&tx);
    report("texture: id assigned lazily and stable", id != 0 && sg_texture_id(&tx) == id && tx.version == 0);
    sg_texture_touch(&tx);
    sg_texture_touch(&tx);
    report("texture: touch bumps the version, keeps the id", tx.version == 2 && tx.id == id);
    SgTexture t2;
    memset(&t2, 0, sizeof t2);
    report("texture: ids are unique", sg_texture_id(&t2) != id);
    tx.w = tx.h = 2; tx.rgba = calloc(16, 1);
    sg_texture_build_mips(&tx);
    report("texture: sg_texture_build_mips touches", tx.version == 3);
    sg_texture_free(&tx);
    report("texture: free notifies the backend with the id and clears id/version", rec.tgone == 1 && rec.gone_tex == id && tx.id == 0 && tx.version == 0);
    sg_texture_free(&tx);
    report("texture: freeing an id-less texture does not notify", rec.tgone == 1);
    uint64_t id2 = sg_texture_id(&tx);
    report("texture: a recycled struct gets a new id", id2 != id && id2 != 0);
    sg_texture_release(&tx);
    report("texture: sg_texture_release notifies and clears", rec.tgone == 2 && tx.id == 0);
    sg_texture_free(&t2);

    sg_backend_set(NULL);
    report("backend: NULL restores CPU only", sg_backend_get() == NULL);
    reset();
    sg_triangles(&T, &S, v, 6);
    report("backend: CPU-only draw works after removal", near_col(px(10, 10), 255, 0, 0, 255, 0));
}

static void test_program_key(void) {
    SgState a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    report("key: deterministic for GL fixed function", sg_program_key(&a) == sg_program_key(&b));
    uint8_t px4[16] = { 0 };
    SgTexture t0;
    memset(&t0, 0, sizeof t0);
    t0.w = t0.h = 2; t0.rgba = px4; t0.format = SG_FMT_RGBA;
    a.tex_on[0] = 1; a.tex[0] = &t0; a.tex_env[0] = SG_ENV_MODULATE;
    b = a;
    uint64_t k = sg_program_key(&a);
    report("key: GL texture on changes the key", k != sg_program_key(&(SgState){ 0 }));
    b.env_color[0][0] = 0.5f; b.fog_color[1] = 0.25f; b.alpha_ref = 0.3f; b.blend = 1; b.blend_src = SG_SRC_ALPHA; b.depth_test = 1;
    report("key: GL constants and non-pixel state do not change the key", sg_program_key(&b) == k);
    b.tex_env[0] = SG_ENV_ADD;
    report("key: GL tex_env changes the key", sg_program_key(&b) != k);
    b = a; b.fog = 1;
    report("key: GL fog changes the key", sg_program_key(&b) != k);
    b = a; b.alpha_test = 1; b.alpha_func = SG_GREATER;
    uint64_t kg = sg_program_key(&b);
    b.alpha_func = SG_LESS;
    report("key: alpha test function changes the key", kg != k && sg_program_key(&b) != kg);
    b = a; t0.format = SG_FMT_LUMINANCE;
    report("key: GL texture format changes the key", sg_program_key(&b) != k);
    t0.format = SG_FMT_RGBA;

    // D3D9 texture stage cascade
    SgProgram p1, p2;
    memset(&p1, 0, sizeof p1);
    p1.kind = SGP_D3D9_TSS; p1.nvary = 11;
    p1.tss.nstages = 2;
    p1.tss.stage[0] = (SgpTssStage){ .colorop = 4, .alphaop = 2, .color_arg = { 0, 2, 1 }, .alpha_arg = { 0, 2, 1 }, .result_arg = 1, .slot = 0, .ncoord = 2, .tex_kind = SG_TEX_2D };
    p1.tss.stage[1] = (SgpTssStage){ .colorop = 7, .alphaop = 1, .color_arg = { 0, 2, 1 }, .result_arg = 1, .slot = 1, .tex_kind = SG_TEX_2D };
    SgTexture tex2 = t0;
    p1.samplers[0].tex = &t0; p1.samplers[1].tex = &tex2;
    p2 = p1;
    SgState sa, sb;
    memset(&sa, 0, sizeof sa);
    sa.program = &p1; sa.nvary = 11;
    sb = sa; sb.program = &p2;
    uint64_t kt = sg_program_key(&sa);
    report("key: TSS identical programs give the same key", kt == sg_program_key(&sb) && kt != k);
    p2.tss.tfactor[0] = 0.7f; p2.tss.stage[0].constant[1] = 0.2f; p2.tss.stage[0].bump_mat[2] = 3; p2.tss.stage[1].bump_lscale = 2;
    p2.fog.start = 1; p2.fog.end = 9; p2.fog.density = 0.5f; p2.fog.color[0] = 1;
    p2.samplers[0].samp.lod_bias = 1.5f; p2.samplers[1].samp.min_filter = SG_FILTER_LINEAR; p2.samplers[0].samp.srgb = 1;
    SgTexture other = t0; other.version = 77; other.id = 99;
    p2.samplers[0].tex = &other;
    report("key: TSS constants, fog values, sampler state and texture identity do not change the key", sg_program_key(&sb) == kt);
    p2 = p1; p2.tss.stage[0].colorop = 5;
    report("key: TSS colour op changes the key", sg_program_key(&sb) != kt);
    p2 = p1; p2.tss.stage[1].alpha_arg[1] = 0x12;
    report("key: TSS argument modifier changes the key", sg_program_key(&sb) != kt);
    p2 = p1; p2.tss.stage[0].projected = 1;
    report("key: TSS projected flag changes the key", sg_program_key(&sb) != kt);
    p2 = p1; p2.tss.specular = 1;
    report("key: TSS specular changes the key", sg_program_key(&sb) != kt);
    p2 = p1; p2.fog.enable = 1; p2.fog.table = 3;
    uint64_t kf = sg_program_key(&sb);
    p2.fog.table = 2;
    report("key: TSS fog mode changes the key", kf != kt && sg_program_key(&sb) != kf);
    p2 = p1; p2.tss.nstages = 1;
    report("key: TSS stage count changes the key", sg_program_key(&sb) != kt);
    p2 = p1; p2.samplers[0].tex = NULL;
    report("key: TSS unbound texture changes the key", sg_program_key(&sb) != kt);
    p2 = p1; tex2.kind = SG_TEX_CUBE; p2.samplers[1].tex = &tex2;
    report("key: TSS texture kind changes the key", sg_program_key(&sb) != kt);
    tex2.kind = SG_TEX_2D;

    // D3D9 pixel shader
    static const uint32_t tok1[] = { 0xFFFF0300u, 0x0000FFFFu }, tok2[] = { 0xFFFF0200u, 0x0000FFFFu };
    SgProgram q1, q2;
    memset(&q1, 0, sizeof q1);
    q1.kind = SGP_D3D9_PS; q1.nvary = 4;
    q1.ps.tokens = tok1; q1.ps.ntokens = 2; q1.ps.major = 3; q1.ps.sampler_mask = 1; q1.ps.sampler_type[0] = SG_TEX_2D;
    q1.samplers[0].tex = &t0;
    q2 = q1;
    sa.program = &q1; sb.program = &q2;
    uint64_t kp = sg_program_key(&sa);
    D3dShaderConsts c1, c2;
    memset(&c1, 0, sizeof c1); memset(&c2, 0, sizeof c2);
    c2.f[0][0] = 5; c2.i[1][0] = 3; c2.b[2] = 1;
    q1.ps.consts = &c1; q2.ps.consts = &c2;
    q2.ps.bump_mat[0][0] = 1; q2.samplers[0].samp.max_anisotropy = 4; q2.samplers[0].tex = &other;
    report("key: PS constants, bump parameters, sampler state, texture identity do not change the key", kp == sg_program_key(&sb));
    q2.ps.tokens = tok2;
    report("key: PS bytecode changes the key", kp != sg_program_key(&sb));
    q2 = q1; q2.ps.uses_vpos = 1;
    report("key: PS vpos use changes the key", kp != sg_program_key(&sb));
    q2 = q1; q2.fog.enable = 1; q2.fog.table = 1;
    report("key: PS fog wrap changes the key", kp != sg_program_key(&sb));
    q2 = q1; q2.samplers[0].tex = NULL;
    report("key: PS unbound sampler changes the key", kp != sg_program_key(&sb));
    q2 = q1; q2.ps.sampler_type[0] = SG_TEX_CUBE;
    report("key: PS declared sampler type changes the key", kp != sg_program_key(&sb));
    sb.nvary = 5;
    report("key: nvary changes the key", sg_program_key(&sb) != sg_program_key(&sa));
    report("key: PS key differs from TSS key", kp != kt);
}

int main(void) {
    test_varyings();
    test_derivs();
    test_stencil();
    test_blend();
    test_depth();
    test_clip_and_viewport();
    test_fill_rule();
    test_points();
    test_sample();
    test_backend();
    test_program_key();
    printf("passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
