// SoulCore - sgsl: shader translator driver (see sgsl.h). Assembles the complete MSL / C source of a draw
// configuration: language shim + shared prelude, uniform / vertex / varying structs, the vertex function and
// the fragment function whose body comes from one of the front ends (sgsl_fe_*.c) through the IR (sgsl_ir.c).
#include "sgsl_priv.h"
#include "d3d9_shader_priv.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- uniform block layout -----------------------------------------------------------------------------------------------
enum { UF_F4, UF_F, UF_I4, UF_I };
typedef struct { const char *name; int ty, count; size_t off; } UField;
#define UFLD(n, t, c) { #n, t, c, offsetof(SgslUniforms, n) }
static const UField UF[] = {
    UFLD(fog_color, UF_F4, 1), UFLD(tfactor, UF_F4, 1), UFLD(env_color, UF_F4, SG_UNITS), UFLD(clip_plane, UF_F4, SG_MAX_CLIP_PLANES),
    UFLD(tss_constant, UF_F4, SGP_MAX_STAGES), UFLD(bump_mat, UF_F4, SGP_MAX_SAMPLERS), UFLD(bump_lscale, UF_F, SGP_MAX_SAMPLERS),
    UFLD(bump_loffset, UF_F, SGP_MAX_SAMPLERS), UFLD(lod_bias, UF_F, SGP_MAX_SAMPLERS), UFLD(alpha_ref, UF_F, 1), UFLD(fog_start, UF_F, 1),
    UFLD(fog_end, UF_F, 1), UFLD(fog_density, UF_F, 1), UFLD(depth_near, UF_F, 1), UFLD(depth_far, UF_F, 1), UFLD(point_size, UF_F, 1),
    UFLD(point_size_min, UF_F, 1), UFLD(point_size_max, UF_F, 1), UFLD(pad0, UF_F, 1),
    UFLD(i, UF_I4, D3DSH_MAX_CONST_I), UFLD(b, UF_I, D3DSH_MAX_CONST_B), UFLD(c, UF_F4, D3DSH_MAX_CONST_F),
};
#define NUF ((int)(sizeof UF / sizeof UF[0]))

static int uf_first_ps(void) { for (int k = 0; k < NUF; k++) if (!strcmp(UF[k].name, "i")) return k; return NUF; }

// Prints the MSL declaration of the uniform block: fields [0, upto), the constant array `c` truncated to nc rows
// (omitted when nc == 0). Returns the byte size of the declared struct, or 0 on a layout mismatch with the C struct.
static size_t print_msl_uniforms(SgBuf *b, int ps, int nc) {
    size_t off = 0;
    sb_put(b, "struct SgslUniforms {\n");
    int last = ps ? NUF : uf_first_ps();
    for (int k = 0; k < last; k++) {
        const UField *f = &UF[k];
        int count = f->count;
        if (!strcmp(f->name, "c")) { if (nc <= 0) break; count = nc; }
        size_t al = (f->ty == UF_F4 || f->ty == UF_I4) ? 16 : 4, sz = al;
        off = (off + al - 1) & ~(al - 1);
        if (off != f->off) return 0;
        const char *ty = f->ty == UF_F4 ? "float4" : f->ty == UF_F ? "float" : f->ty == UF_I4 ? "int4" : "int";
        if (count > 1 || !strcmp(f->name, "c") || !strcmp(f->name, "i") || !strcmp(f->name, "b")) sb_put(b, "    %s %s[%d];\n", ty, f->name, count);
        else sb_put(b, "    %s %s;\n", ty, f->name);
        off += sz * (size_t)count;
    }
    sb_put(b, "};\n");
    return (off + 15) & ~(size_t)15;
}

// ---- hashing ---------------------------------------------------------------------------------------------------------------
static void kmix(uint64_t *h, uint64_t v) { *h = (*h ^ v) * 0x100000001b3ULL; *h ^= *h >> 29; }

static int tex_state(const SgTexture *t) {
    if (!t) return 0;
    if (!t->rgba || t->w <= 0 || t->h <= 0) return 1;
    return 2 + t->kind;
}

static int nvary_out(const SgState *s) {
    if (!s->program) return 4;
    int n = s->program->nvary > 0 ? s->program->nvary : s->nvary;
    if (s->nvary > n) n = s->nvary;
    return n < 0 ? 0 : n > SG_VARYINGS ? SG_VARYINGS : n;
}

static uint64_t make_key(const SgState *s, int prim, int lang) {
    uint64_t h = sg_program_key(s);
    kmix(&h, 0x5367736cULL); kmix(&h, (uint64_t)lang); kmix(&h, (uint64_t)prim);
    int nv = nvary_out(s);
    kmix(&h, (uint64_t)nv);
    kmix(&h, (uint64_t)s->flat_first);
    kmix(&h, (uint64_t)(s->program ? (s->flat_mask & ((nv >= 32 ? 0xFFFFFFFFu : (1u << nv) - 1u))) : (s->flat != 0)));
    kmix(&h, (uint64_t)(s->clip_mask & ((1u << SG_MAX_CLIP_PLANES) - 1)));
    kmix(&h, (uint64_t)(s->clip_z_zero != 0)); kmix(&h, (uint64_t)(s->depth_clamp != 0));
    int sprite = s->point_sprite && prim == SG_PRIM_POINTS;
    kmix(&h, (uint64_t)sprite); if (sprite) kmix(&h, (uint64_t)(unsigned)s->sprite_slot);
    kmix(&h, (uint64_t)(s->point_size_vertex != 0)); kmix(&h, (uint64_t)(s->point_size_max > 0));
    kmix(&h, (uint64_t)(s->alpha_test != 0)); if (s->alpha_test) kmix(&h, (uint64_t)s->alpha_func);
    kmix(&h, (uint64_t)(s->fog != 0));
    if (!s->program) for (int u = 0; u < SG_UNITS; u++) kmix(&h, (uint64_t)(s->tex_on[u] ? tex_state(s->tex[u]) : 0));
    else for (int i = 0; i < SGP_MAX_SAMPLERS; i++) kmix(&h, (uint64_t)tex_state(s->program->samplers[i].tex));
    return h;
}

// ---- uniforms ----------------------------------------------------------------------------------------------------------------
static void setf4(SgslF4 *d, const float *v) { d->x = v[0]; d->y = v[1]; d->z = v[2]; d->w = v[3]; }

void sgsl_fill_uniforms(const SgState *s, SgslUniforms *u) {
    memset(u, 0, sizeof *u);
    const SgProgram *p = s->program;
    if (!p) {
        setf4(&u->fog_color, s->fog_color);
        for (int k = 0; k < SG_UNITS; k++) setf4(&u->env_color[k], s->env_color[k]);
    } else {
        u->fog_color.x = p->fog.color[0]; u->fog_color.y = p->fog.color[1]; u->fog_color.z = p->fog.color[2]; u->fog_color.w = 1.0f;
        u->fog_start = p->fog.start; u->fog_end = p->fog.end; u->fog_density = p->fog.density;
        for (int i = 0; i < SGP_MAX_SAMPLERS; i++) u->lod_bias[i] = p->samplers[i].samp.lod_bias;
        if (p->kind == SGP_D3D9_TSS) {
            setf4(&u->tfactor, p->tss.tfactor);
            for (int i = 0; i < SGP_MAX_STAGES; i++) {
                setf4(&u->tss_constant[i], p->tss.stage[i].constant);
                setf4(&u->bump_mat[i], p->tss.stage[i].bump_mat);
                u->bump_lscale[i] = p->tss.stage[i].bump_lscale; u->bump_loffset[i] = p->tss.stage[i].bump_loffset;
            }
        } else if (p->kind == SGP_D3D9_PS) {
            for (int i = 0; i < SGP_MAX_SAMPLERS; i++) {
                setf4(&u->bump_mat[i], p->ps.bump_mat[i]);
                u->bump_lscale[i] = p->ps.bump_lscale[i]; u->bump_loffset[i] = p->ps.bump_loffset[i];
            }
            const D3dShaderConsts *c = p->ps.consts;
            if (c) {
                for (int i = 0; i < D3DSH_MAX_CONST_F; i++) setf4(&u->c[i], c->f[i]);
                for (int i = 0; i < D3DSH_MAX_CONST_I; i++) { u->i[i].x = c->i[i][0]; u->i[i].y = c->i[i][1]; u->i[i].z = c->i[i][2]; u->i[i].w = c->i[i][3]; }
                for (int i = 0; i < D3DSH_MAX_CONST_B; i++) u->b[i] = c->b[i] != 0;
            }
        }
    }
    for (int i = 0; i < SG_MAX_CLIP_PLANES; i++) setf4(&u->clip_plane[i], s->clip_plane[i]);
    u->alpha_ref = s->alpha_ref;
    u->depth_near = s->depth_near; u->depth_far = s->depth_far;
    u->point_size = s->point_size; u->point_size_min = s->point_size_min; u->point_size_max = s->point_size_max;
}

void sgsl_gl_sampler(const SgTexture *t, SgSampler *o) {
    memset(o, 0, sizeof *o);
    o->min_filter = t->min_linear ? SG_FILTER_LINEAR : SG_FILTER_POINT;
    o->mag_filter = t->mag_linear ? SG_FILTER_LINEAR : SG_FILTER_POINT;
    o->mip_filter = (t->use_mips && t->levels > 1) ? SG_FILTER_POINT : SG_FILTER_NONE;
    o->address_u = t->clamp_s ? SG_ADDR_CLAMP : SG_ADDR_WRAP;
    o->address_v = t->clamp_t ? SG_ADDR_CLAMP : SG_ADDR_WRAP;
    o->address_w = SG_ADDR_CLAMP;
    o->max_anisotropy = 1;
}

// ---- language shims --------------------------------------------------------------------------------------------------------------
static const char *const SHIM_MSL =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define SG_FN inline\n"
    "#define SG_ATTR(x) x\n"
    "#define sg_floor floor\n#define sg_fabs fabs\n#define sg_sqrt sqrt\n#define sg_sin sin\n#define sg_cos cos\n"
    "#define sg_exp2f exp2\n#define sg_expf exp\n#define sg_log2f log2\n#define sg_logf log\n#define sg_powf pow\n"
    "#define sg_isfinite isfinite\n#define SG_INF INFINITY\n"
    "inline float4 mk4(float x, float y, float z, float w) { return float4(x, y, z, w); }\n";

static const char *const SHIM_C =
    "#include <math.h>\n#include <float.h>\n#include <string.h>\n#include <stdint.h>\n#include <stdbool.h>\n#include \"sgsl.h\"\n"
    "typedef SgslF4 float4;\ntypedef unsigned uint;\n"
    "#define SG_FN static inline\n"
    "#define SG_ATTR(x)\n"
    "#define sg_floor floorf\n#define sg_fabs fabsf\n#define sg_sqrt sqrtf\n#define sg_sin sinf\n#define sg_cos cosf\n"
    "#define sg_exp2f exp2f\n#define sg_expf expf\n#define sg_log2f log2f\n#define sg_logf logf\n#define sg_powf powf\n"
    "#define sg_isfinite isfinite\n#define SG_INF INFINITY\n"
    "static inline float4 mk4(float x, float y, float z, float w) { float4 r; r.x = x; r.y = y; r.z = z; r.w = w; return r; }\n"
    "static inline float4 sg_swz(float4 a, int i, int j, int k, int l) { float t[4]; t[0] = a.x; t[1] = a.y; t[2] = a.z; t[3] = a.w; return mk4(t[i], t[j], t[k], t[l]); }\n";

static const char *const SHIM_C_TAIL =
    "static inline float4 sgc_call(const SgslCEnv *e, int i, float4 c, const float *dx, const float *dy, float lod) {\n"
    "    float cd[4], o[4];\n"
    "    cd[0] = c.x; cd[1] = c.y; cd[2] = c.z; cd[3] = c.w;\n"
    "    e->sample(e->tex[i], e->samp[i], cd, dx, dy, lod, o);\n"
    "    return mk4(o[0], o[1], o[2], o[3]);\n"
    "}\n"
    "static inline float4 sgc_smp(const SgslCEnv *e, int i, float4 c, float4 dx, float4 dy) {\n"
    "    float a[4], b[4];\n"
    "    a[0] = dx.x; a[1] = dx.y; a[2] = dx.z; a[3] = dx.w; b[0] = dy.x; b[1] = dy.y; b[2] = dy.z; b[3] = dy.w;\n"
    "    return sgc_call(e, i, c, a, b, NAN);\n"
    "}\n"
    "static inline float4 sgc_smp0(const SgslCEnv *e, int i, float4 c, float lod) { return sgc_call(e, i, c, NULL, NULL, lod); }\n";

const char *sgsl_shim_text(int lang) { return lang == SGSL_MSL ? SHIM_MSL : SHIM_C; }

// ---- vertex function -----------------------------------------------------------------------------------------------------------------
static void vname(SgBuf *b, int lang, int k) { if (lang == SGSL_MSL) sb_put(b, "o.v%d", k); else sb_put(b, "o.v[%d]", k); }

static void print_vertex(SgBuf *b, const SgState *s, int prim, int lang, int nv, uint32_t flatmask, int nclip) {
    int gl = s->program == NULL;
    if (lang == SGSL_MSL)
        sb_put(b, "vertex SgVSOut sg_vs(uint vid [[vertex_id]], const device SgVertex *vb [[buffer(0)]], constant SgslUniforms &u [[buffer(1)]]) {\n    SgVSOut o;\n");
    else
        sb_put(b, "void sg_vs(unsigned vid, const SgVertex *vb, const SgslUniforms *u_, SgslVSOut *out_) {\n    SgslVSOut o;\n    memset(&o, 0, sizeof o);\n");
    sb_put(b, "    float4 p = mk4(vb[vid].x, vb[vid].y, vb[vid].z, vb[vid].w);\n");
    if (s->clip_z_zero) sb_put(b, "    o.position = mk4(p.x, p.y, p.z, p.w);\n");
    else sb_put(b, "    o.position = mk4(p.x, p.y, (p.z + p.w) * 0.5f, p.w);\n");
    // provoking vertex of the primitive this vertex belongs to
    int need_prov = gl ? (s->flat != 0) : (flatmask != 0);
    if (need_prov) {
        if (prim == SG_PRIM_TRIANGLES) sb_put(b, "    uint sg_pi = (vid / 3u) * 3u + %du;\n", s->flat_first ? 0 : 2);
        else if (prim == SG_PRIM_LINES) sb_put(b, "    uint sg_pi = (vid / 2u) * 2u + %du;\n", s->flat_first ? 0 : 1);
        else sb_put(b, "    uint sg_pi = vid;\n");
    }
    // point size
    if (prim == SG_PRIM_POINTS) {
        sb_put(b, "    float sz = %s;\n", s->point_size_vertex ? "vb[vid].psize" : "u.point_size");
        sb_put(b, "    if (sz != sz) o.position = mk4(0.0f, 0.0f, 2.0f, 1.0f);\n");
        if (s->point_size_max > 0) sb_put(b, "    if (sz < u.point_size_min) sz = u.point_size_min;\n    if (sz > u.point_size_max) sz = u.point_size_max;\n");
        if (s->point_sprite) sb_put(b, "    if (sz < 1.0f) sz = 1.0f;\n    if (sz > 4096.0f) sz = 4096.0f;\n");
        else sb_put(b, "    sz = sz > 1.5f ? sg_floor(sz + 0.5f) : 1.0f;\n    if (sz > 4096.0f) sz = 4096.0f;\n");
        sb_put(b, "    o.point_size = sz;\n");
    } else sb_put(b, "    o.point_size = 1.0f;\n");
    for (int i = 0; i < nclip; i++) {
        if (s->clip_mask & (1 << i)) sb_put(b, "    o.clip[%d] = u.clip_plane[%d].x * p.x + u.clip_plane[%d].y * p.y + u.clip_plane[%d].z * p.z + u.clip_plane[%d].w * p.w;\n", i, i, i, i, i);
        else sb_put(b, "    o.clip[%d] = 1.0f;\n", i);
    }
    if (gl) {
        const char *ci = s->flat ? "sg_pi" : "vid";
        sb_put(b, "    "); vname(b, lang, 0); sb_put(b, " = mk4(vb[%s].c[0], vb[%s].c[1], vb[%s].c[2], vb[%s].c[3]);\n", ci, ci, ci, ci);
        for (int u = 0; u < SG_UNITS; u++) { sb_put(b, "    "); vname(b, lang, 1 + u); sb_put(b, " = mk4(vb[vid].t[%d][0], vb[vid].t[%d][1], 0.0f, 0.0f);\n", u, u); }
        sb_put(b, "    "); vname(b, lang, 3); sb_put(b, " = mk4(vb[vid].fog, 0.0f, 0.0f, 0.0f);\n");
    } else {
        for (int k = 0; k < nv; k++) {
            const char *vi = ((flatmask >> k) & 1) ? "sg_pi" : "vid";
            sb_put(b, "    "); vname(b, lang, k);
            sb_put(b, " = mk4(vb[%s].v[%d][0], vb[%s].v[%d][1], vb[%s].v[%d][2], vb[%s].v[%d][3]);\n", vi, k, vi, k, vi, k, vi, k);
        }
    }
    if (lang == SGSL_MSL) sb_put(b, "    return o;\n}\n"); else sb_put(b, "    *out_ = o;\n}\n");
}

// ---- driver --------------------------------------------------------------------------------------------------------------------------------
static char *fail_out(char *err, size_t errlen, const char *fmt, ...) {
    if (err && errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, errlen, fmt, ap);
        va_end(ap);
    }
    return NULL;
}

char *sgsl_generate(const SgState *s, int lang, SgslInfo *info, char *err, size_t errlen) {
    return sgsl_generate_prim(s, SG_PRIM_TRIANGLES, lang, info, err, errlen);
}

char *sgsl_generate_prim(const SgState *s, int prim, int lang, SgslInfo *info, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (lang != SGSL_MSL && lang != SGSL_C) return fail_out(err, errlen, "unknown output language %d", lang);
    if (!s) return fail_out(err, errlen, "no state");
    const SgProgram *p = s->program;
    if (!p && s->pixel_fn) return fail_out(err, errlen, "pixel_fn without a program: opaque CPU callback");
    if (p && p->kind != SGP_D3D9_TSS && p->kind != SGP_D3D9_PS) return fail_out(err, errlen, "unknown program kind %d", p->kind);
    if (p && p->kind == SGP_D3D9_PS && (!p->ps.shader || !d3dsh_is_pixel(p->ps.shader))) return fail_out(err, errlen, "pixel program without a pixel shader");

    const int nv = nvary_out(s);
    uint32_t flatmask = 0;
    if (p) flatmask = s->flat_mask & (nv >= 32 ? 0xFFFFFFFFu : ((1u << nv) - 1u));
    else if (s->flat) flatmask = 1;
    const int sprite_on = s->point_sprite && prim == SG_PRIM_POINTS;
    const int sprite_slot = p ? s->sprite_slot : 1;
    const int sprite = sprite_on && sprite_slot >= 0 && sprite_slot < nv;
    int nclip = 0;
    for (int i = 0; i < SG_MAX_CLIP_PLANES; i++) if (s->clip_mask & (1 << i)) nclip = i + 1;

    Gb g;
    gb_init(&g);
    g.vy = gb_var(&g, TY_V4, "vy", SG_VARYINGS);
    g.dvx = gb_var(&g, TY_V4, "dvx", SG_VARYINGS);
    g.dvy = gb_var(&g, TY_V4, "dvy", SG_VARYINGS);
    g.pos = gb_var(&g, TY_V4, "pos", 0);
    g.front = gb_var(&g, TY_I, "front", 0);
    g.ocol = gb_var(&g, TY_V4, "ocol", 0);
    g.odepth = gb_var(&g, TY_F, "odepth", 0);
    int ok;
    if (!p) ok = fe_gl(&g, s);
    else if (p->kind == SGP_D3D9_TSS) ok = fe_tss(&g, s);
    else ok = fe_ps(&g, s);
    if (ok && !g.failed) fe_finish(&g, s, p != NULL);
    if (!ok || g.failed) {
        fail_out(err, errlen, "%s", g.err[0] ? g.err : "translation failed");
        gb_free(&g);
        return NULL;
    }
    const int ps = p && p->kind == SGP_D3D9_PS;
    SgBuf b = { 0, 0, 0 };
    sb_put(&b, "// sgsl generated %s, key %016llx, primitive %d\n", lang == SGSL_MSL ? "Metal Shading Language" : "C", (unsigned long long)make_key(s, prim, lang), prim);
    sb_put(&b, "%s", sgsl_shim_text(lang));
    sb_put(&b, "%s", sgsl_prelude_text());
    if (lang == SGSL_C) sb_put(&b, "%s", SHIM_C_TAIL);
    int nc = ps ? (g.max_const > 256 ? 256 : g.max_const) : 0;
    size_t ubytes;
    if (lang == SGSL_MSL) {
        ubytes = print_msl_uniforms(&b, ps, nc);
        if (!ubytes) { fail_out(err, errlen, "uniform layout mismatch"); gb_free(&g); free(b.p); return NULL; }
        sb_put(&b,
               "struct SgVertex { float x, y, z, w; float c[4]; float t[2][2]; float fog; float psize; float v[12][4]; };\n");
        sb_put(&b, "struct SgVSOut {\n    float4 position [[position]];\n    float point_size [[point_size]];\n");
        if (nclip) sb_put(&b, "    float clip[%d] [[clip_distance]];\n", nclip);
        for (int k = 0; k < nv; k++) sb_put(&b, "    float4 v%d [[user(v%d)]];\n", k, k);
        sb_put(&b, "};\nstruct SgFSIn {\n    float4 position [[position]];\n");
        for (int k = 0; k < nv; k++) sb_put(&b, "    float4 v%d [[user(v%d)%s]];\n", k, k, ((flatmask >> k) & 1) ? ", flat" : "");
        sb_put(&b, "};\nstruct SgFSOut {\n    float4 color [[color(0)]];\n");
        if (g.writes_depth) sb_put(&b, "    float depth [[depth(any)]];\n");
        sb_put(&b, "};\n");
    } else {
        ubytes = sizeof(SgslUniforms);
        sb_put(&b, "typedef SgslVSOut SgVSOut;\n");
    }
    if (g.in_slot_table) {
        sb_put(&b, "%s int sg_in_slot[16] = {", lang == SGSL_MSL ? "constant" : "static const");
        for (int i = 0; i < 16; i++) sb_put(&b, "%s%d", i ? ", " : "", g.in_slot[i]);
        sb_put(&b, "};\n");
    }
    if (lang == SGSL_C) sb_put(&b, "#define u (*u_)\n");
    if (g.need_defc_fn) {
        // shader-declared constants addressed relatively: float4 sg_cfetch(int idx, float4 fallback)
        const D3dShader *sh = g.shader;
        sb_put(&b, "SG_FN float4 sg_cfetch(int idx, float4 fb) {\n    if (idx < 0 || idx >= 256) return mk4(0.0f, 0.0f, 0.0f, 0.0f);\n");
        if (sh && sh->deff) {
            sb_put(&b, "    switch (idx) {\n");
            for (int i = 0; i < 256; i++)
                if (sh->defmask[i >> 5] & (1u << (i & 31))) {
                    sb_put(&b, "    case %d: return mk4(", i);
                    for (int k = 0; k < 4; k++) {
                        float f = sh->deff[i][k];
                        char tmp[40];
                        if (f != f) snprintf(tmp, sizeof tmp, "NAN"); else if (isinf(f)) snprintf(tmp, sizeof tmp, f < 0 ? "-INFINITY" : "INFINITY");
                        else { snprintf(tmp, sizeof tmp, "%.9g", (double)f); if (!strpbrk(tmp, ".eE")) strcat(tmp, ".0"); strcat(tmp, "f"); }
                        sb_put(&b, "%s%s", k ? ", " : "", tmp);
                    }
                    sb_put(&b, ");\n");
                }
            sb_put(&b, "    default: break;\n    }\n");
        }
        sb_put(&b, "    return fb;\n}\n");
    }
    print_vertex(&b, s, prim, lang, nv, flatmask, nclip);

    // ---- fragment function ----
    if (lang == SGSL_MSL) {
        sb_put(&b, "fragment SgFSOut sg_fs(SgFSIn in [[stage_in]], constant SgslUniforms &u [[buffer(1)]]");
        if (g.uses_front) sb_put(&b, ", bool sg_front [[front_facing]]");
        if (sprite) sb_put(&b, ", float2 sg_pc [[point_coord]]");
        for (int i = 0; i < 16; i++)
            if (g.texture_mask & (1u << i)) {
                const char *tt = g.texture_kind[i] == SG_TEX_CUBE ? "texturecube<float>" : g.texture_kind[i] == SG_TEX_VOLUME ? "texture3d<float>" : "texture2d<float>";
                sb_put(&b, ", %s tex%d [[texture(%d)]], sampler smp%d [[sampler(%d)]]", tt, i, i, i, i);
            }
        sb_put(&b, ") {\n    SgFSOut sg_o;\n    sg_o.color = mk4(0.0f, 0.0f, 0.0f, 0.0f);\n");
        if (g.writes_depth) sb_put(&b, "    sg_o.depth = 0.0f;\n");
    } else {
        sb_put(&b, "int sg_fs(const SgslCEnv *env_, const SgslFragIn *in_, const SgslUniforms *u_, float *out_color, float *out_depth) {\n");
    }
    sg_print_decls(&g, &b, lang, "    ");
    // prologue: inputs
    if (lang == SGSL_MSL) {
        sb_put(&b, "    pos = in.position;\n");
        for (int k = 0; k < nv; k++) sb_put(&b, "    vy[%d] = in.v%d;\n", k, k);
        sb_put(&b, "    front = %s;\n", g.uses_front ? "(sg_front ? 1 : 0)" : "1");
    } else {
        sb_put(&b, "    pos = mk4(in_->px.pos[0], in_->px.pos[1], in_->px.pos[2], in_->px.pos[3]);\n");
        for (int k = 0; k < nv; k++) sb_put(&b, "    vy[%d] = mk4(in_->px.v[%d][0], in_->px.v[%d][1], in_->px.v[%d][2], in_->px.v[%d][3]);\n", k, k, k, k, k);
        sb_put(&b, "    front = in_->px.front_facing ? 1 : 0;\n");
    }
    if (sprite) {
        if (lang == SGSL_MSL) sb_put(&b, "    vy[%d] = %s;\n", sprite_slot, p ? "mk4(sg_pc.x, sg_pc.y, 0.0f, 1.0f)" : "mk4(sg_pc.x, sg_pc.y, vy[1].z, vy[1].w)");
        else sb_put(&b, "    vy[%d] = %s;\n", sprite_slot, p ? "mk4(in_->point_coord[0], in_->point_coord[1], 0.0f, 1.0f)" : "mk4(in_->point_coord[0], in_->point_coord[1], vy[1].z, vy[1].w)");
    }
    if (g.uses_deriv)
        for (int k = 0; k < nv; k++) {
            if (lang == SGSL_MSL) sb_put(&b, "    dvx[%d] = dfdx(vy[%d]);\n    dvy[%d] = dfdy(vy[%d]);\n", k, k, k, k);
            else {
                sb_put(&b, "    dvx[%d] = mk4(in_->px.dx[%d][0], in_->px.dx[%d][1], in_->px.dx[%d][2], in_->px.dx[%d][3]);\n", k, k, k, k, k);
                sb_put(&b, "    dvy[%d] = mk4(in_->px.dy[%d][0], in_->px.dy[%d][1], in_->px.dy[%d][2], in_->px.dy[%d][3]);\n", k, k, k, k, k);
            }
        }
    sg_print_body(&g, &b, lang, 1);
    if (lang == SGSL_MSL) {
        sb_put(&b, "    sg_o.color = ocol;\n");
        if (g.writes_depth) sb_put(&b, "    sg_o.depth = odepth;\n");
        sb_put(&b, "    return sg_o;\n}\n");
    } else {
        sb_put(&b, "    out_color[0] = ocol.x; out_color[1] = ocol.y; out_color[2] = ocol.z; out_color[3] = ocol.w;\n");
        sb_put(&b, "    if (out_depth) *out_depth = odepth;\n    return 1;\n}\n");
        sb_put(&b, "#undef u\n");
    }
    if (!b.p) { gb_free(&g); return fail_out(err, errlen, "out of memory"); }
    if (info) {
        memset(info, 0, sizeof *info);
        info->key = make_key(s, prim, lang);
        info->texture_mask = g.texture_mask;
        for (int i = 0; i < 16; i++) info->texture_kind[i] = (g.texture_mask >> i) & 1 ? g.texture_kind[i] : -1;
        info->writes_depth = g.writes_depth;
        info->uses_point_coord = sprite;
        info->uses_front_facing = g.uses_front;
        info->uses_position = g.uses_pos;
        info->clip_distances = nclip;
        info->nvary = nv;
        info->prim = prim;
        info->uses_derivatives = g.uses_deriv;
        info->uses_discard = g.uses_discard;
        if (ps) {
            info->uniform_bytes = (uint32_t)(offsetof(SgslUniforms, c) + 16u * (unsigned)nc);
        } else info->uniform_bytes = (uint32_t)offsetof(SgslUniforms, i);
    }
    gb_free(&g);
    return b.p;
}
