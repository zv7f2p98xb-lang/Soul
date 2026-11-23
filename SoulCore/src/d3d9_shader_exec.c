// SoulCore - Direct3D 9 shader engine: interpreter (see d3d9_shader.h, d3d9_shader_priv.h).
// Runs the pre-decoded Instr array. All state lives on the stack (reentrant, allocation free).
#include "d3d9_shader_priv.h"
#include <float.h>
#include <math.h>
#include <string.h>

#define MAX_LOOPS 24
#define MAX_CALLS 32
#define BUDGET 2000000

typedef struct Frame { int count, step, aL_saved, is_loop; } Frame;

typedef struct St {
    const D3dShader *sh;
    float *rb[RF_COUNT];                 // register file base pointers (4 floats per register)
    const float *dbx[RF_COUNT], *dby[RF_COUNT];   // derivative bases (NULL = zero)
    float r[32][4], t[8][4], misc[2][4], zero[4], sink[4], psz[4], depth[4], col[4][4];
    float dr[2][32][4], dt[2][8][4];
    float dres[2][4];
    int a0, aL, p[4];
    const D3dShaderConsts *c;
    const SgTexture *const *tex;
    const SgSampler *samp;
    int ntex;
    const D3dPsCtx *pc;
    const SgPixelIn *pin;
    int dflag, ps1x, lim_clamp;
    float row[4];
    int npad;
    float depth_val;
    int depth_set;
} St;

static const int LIM[RF_COUNT] = { [RF_TEMP] = 32, [RF_INPUT] = 16, [RF_VARY] = SG_VARYINGS, [RF_CONST] = 256, [RF_DEFC] = 256,
                                   [RF_TREG] = 8, [RF_MISC] = 2, [RF_ZERO] = 1 };

static inline float sat01(float v) { return v > 0 ? (v < 1 ? v : 1) : 0; }
static inline float d3mul(float a, float b) { float m = a * b; if (m != m && (a == 0 || b == 0)) m = 0; return m; }

static inline const float *cptr(const St *s, int idx) {
    if (idx < 0 || idx >= 256) return s->zero;
    const D3dShader *sh = s->sh;
    if (sh->deff && (sh->defmask[idx >> 5] & (1u << (idx & 31)))) return sh->deff[idx];
    return s->c->f[idx];
}

static inline const float *op_ptr(const St *s, const Operand *o, int off) {
    int file = o->file, idx = o->idx + off;
    if (o->rel) {
        { long long k = (long long)idx + (o->rel == 1 ? s->a0 : s->aL); idx = k > 1000000 ? 1000000 : k < -1000000 ? -1000000 : (int)k; }
        if (file == RF_CONST) return cptr(s, idx);
        if (file == RF_INPUT && s->sh->pixel) {
            int sl = (idx >= 0 && idx < 16) ? s->sh->in_slot[idx] : -1;
            return sl < 0 ? s->zero : s->rb[RF_VARY] + sl * 4;
        }
    } else if (file == RF_CONST || (file == RF_DEFC && off)) {
        if (off) return cptr(s, idx);
        return s->c->f[idx];
    }
    if (file == RF_DEFC) return s->sh->deff[idx];
    if ((unsigned)idx >= (unsigned)LIM[file]) return s->zero;
    return s->rb[file] + idx * 4;
}

static inline void apply_mod(int mod, float v[4]) {
    switch (mod) {
    case SM_NEG: for (int i = 0; i < 4; i++) v[i] = -v[i]; break;
    case SM_BIAS: for (int i = 0; i < 4; i++) v[i] -= 0.5f; break;
    case SM_BIASNEG: for (int i = 0; i < 4; i++) v[i] = -(v[i] - 0.5f); break;
    case SM_SIGN: for (int i = 0; i < 4; i++) v[i] = 2 * (v[i] - 0.5f); break;
    case SM_SIGNNEG: for (int i = 0; i < 4; i++) v[i] = -2 * (v[i] - 0.5f); break;
    case SM_COMP: for (int i = 0; i < 4; i++) v[i] = 1 - v[i]; break;
    case SM_X2: for (int i = 0; i < 4; i++) v[i] *= 2; break;
    case SM_X2NEG: for (int i = 0; i < 4; i++) v[i] *= -2; break;
    case SM_DZ: { float z = v[2]; for (int i = 0; i < 4; i++) v[i] = z != 0 ? v[i] / z : v[i]; break; }
    case SM_DW: { float w = v[3]; for (int i = 0; i < 4; i++) v[i] = w != 0 ? v[i] / w : v[i]; break; }
    case SM_ABS: for (int i = 0; i < 4; i++) v[i] = fabsf(v[i]); break;
    case SM_ABSNEG: for (int i = 0; i < 4; i++) v[i] = -fabsf(v[i]); break;
    default: break;
    }
}

static inline void fetch(const St *s, const Operand *o, int off, float v[4]) {
    const float *p = op_ptr(s, o, off);
    float x[4] = { p[0], p[1], p[2], p[3] };
    if (o->clampc) for (int i = 0; i < 4; i++) x[i] = x[i] < -1 ? -1 : x[i] > 1 ? 1 : x[i];
    unsigned sw = o->swz;
    v[0] = x[sw & 3]; v[1] = x[(sw >> 2) & 3]; v[2] = x[(sw >> 4) & 3]; v[3] = x[(sw >> 6) & 3];
    if (o->mod) apply_mod(o->mod, v);
}

// derivative of a source operand (forward mode): d[0] = d/dx, d[1] = d/dy
static void fetch_d(const St *s, const Operand *o, float d[2][4]) {
    const float *px = s->dbx[o->file], *py = s->dby[o->file];
    if (!px || o->rel) { memset(d, 0, sizeof(float) * 8); return; }
    int idx = o->idx;
    if (idx >= LIM[o->file]) { memset(d, 0, sizeof(float) * 8); return; }
    const float *raw = s->rb[o->file] + idx * 4;
    float f[4] = {1, 1, 1, 1};
    switch (o->mod) {
    case SM_NEG: case SM_BIASNEG: case SM_COMP: for (int i = 0; i < 4; i++) f[i] = -1; break;
    case SM_SIGN: case SM_X2: for (int i = 0; i < 4; i++) f[i] = 2; break;
    case SM_SIGNNEG: case SM_X2NEG: for (int i = 0; i < 4; i++) f[i] = -2; break;
    case SM_ABS: case SM_ABSNEG: for (int i = 0; i < 4; i++) { float sg = raw[(o->swz >> (2 * i)) & 3] < 0 ? -1.f : 1.f; f[i] = o->mod == SM_ABS ? sg : -sg; } break;
    default: break;
    }
    px += idx * 4; py += idx * 4;
    for (int i = 0; i < 4; i++) {
        int c = (o->swz >> (2 * i)) & 3;
        d[0][i] = px[c] * f[i]; d[1][i] = py[c] * f[i];
    }
    if (o->clampc) memset(d, 0, sizeof(float) * 8);
}

static inline int pmask(const St *s, const Instr *I) {
    int m = 0;
    for (int c = 0; c < 4; c++) {
        int v = s->p[(I->predswz >> (2 * c)) & 3] != 0;
        if (I->pred == 2) v = !v;
        m |= v << c;
    }
    return m;
}

static inline void store(St *s, const Instr *I, const float res[4]) {
    const DestOp *d = &I->dst;
    if (d->file == RF_ADDR) {
        if (!I->pred || pmask(s, I)) {
            float v = res[0];
            if (v != v) v = 0;
            v = v > 1e6f ? 1e6f : v < -1e6f ? -1e6f : v;
            s->a0 = I->op == OP_MOVA ? (int)floorf(v + 0.5f) : (int)floorf(v);
        }
        return;
    }
    int mask = d->mask;
    if (I->pred) mask &= pmask(s, I);
    if (!mask) return;
    float *p = s->rb[d->file] + d->idx * 4;
    int shift = d->shift & 15; if (shift & 8) shift -= 16;
    float scale = shift ? ldexpf(1.0f, shift) : 1.0f;
    float lim = s->ps1x ? (s->sh->minor >= 4 ? 8.0f : 1.0f) : 0;
    for (int c = 0; c < 4; c++) {
        if (!(mask & (1 << c))) continue;
        float v = res[c];
        if (shift) v *= scale;
        if (d->sat) v = sat01(v);
        else if (lim) v = v < -lim ? -lim : v > lim ? lim : v;
        p[c] = v;
    }
    if (s->dflag && (d->file == RF_TEMP || d->file == RF_TREG)) {
        float (*dx)[4] = d->file == RF_TEMP ? s->dr[0] : s->dt[0], (*dy)[4] = d->file == RF_TEMP ? s->dr[1] : s->dt[1];
        for (int c = 0; c < 4; c++) if (mask & (1 << c)) { dx[d->idx][c] = s->dres[0][c] * scale; dy[d->idx][c] = s->dres[1][c] * scale; }
    }
}

static inline int cmp_op(int code, float a, float b) {
    switch (code) {
    case CMP_GT: return a > b;
    case CMP_EQ: return a == b;
    case CMP_GE: return a >= b;
    case CMP_LT: return a < b;
    case CMP_NE: return a != b;
    case CMP_LE: return a <= b;
    default: return 0;
    }
}

static inline int cond_bool(const St *s, const Operand *o) {
    if (o->file == RF_PRED) { int v = s->p[o->swz & 3] != 0; return o->mod == SM_NOT ? !v : v; }
    const D3dShader *sh = s->sh;
    int v = (sh->defb_mask >> o->idx & 1) ? sh->defb[o->idx] : s->c->b[o->idx] != 0;
    return o->mod == SM_NOT ? !v : v;
}

static inline const int *iconst(const St *s, int idx) {
    const D3dShader *sh = s->sh;
    return (sh->defi_mask >> idx & 1) ? sh->defi[idx] : s->c->i[idx];
}

static void sample_tex(const St *s, int stage, const float c[4], const float *ddx, const float *ddy, float lod, float out[4]) {
    const SgTexture *t = (stage >= 0 && stage < s->ntex) ? s->tex[stage] : NULL;
    if (!t) { out[0] = out[1] = out[2] = 0; out[3] = 1; return; }
    sg_sample(t, &s->samp[stage], c, ddx, ddy, lod, out);
}

// ---- derivative propagation for the arithmetic instructions ----------------------------------------
static void deriv_alu(const St *s, const Instr *I, const float a[4], const float b[4], const float c[4],
                      float da[2][4], float db[2][4], float dc[2][4], const float *res, float dres[2][4]) {
    (void)s;
    for (int k = 0; k < 2; k++) {
        float *o = dres[k];
        const float *x = da[k], *y = db[k], *z = dc[k];
        switch (I->op) {
        case OP_MOV: case OP_FRC: for (int i = 0; i < 4; i++) o[i] = x[i]; break;
        case OP_ABS: for (int i = 0; i < 4; i++) o[i] = a[i] < 0 ? -x[i] : x[i]; break;
        case OP_ADD: for (int i = 0; i < 4; i++) o[i] = x[i] + y[i]; break;
        case OP_SUB: for (int i = 0; i < 4; i++) o[i] = x[i] - y[i]; break;
        case OP_MUL: for (int i = 0; i < 4; i++) o[i] = x[i] * b[i] + a[i] * y[i]; break;
        case OP_MAD: for (int i = 0; i < 4; i++) o[i] = x[i] * b[i] + a[i] * y[i] + z[i]; break;
        case OP_LRP: for (int i = 0; i < 4; i++) o[i] = x[i] * (b[i] - c[i]) + a[i] * (y[i] - z[i]) + z[i]; break;
        case OP_DP2ADD: { float v = x[0] * b[0] + a[0] * y[0] + x[1] * b[1] + a[1] * y[1] + z[0]; for (int i = 0; i < 4; i++) o[i] = v; break; }
        case OP_DP3: { float v = 0; for (int i = 0; i < 3; i++) v += x[i] * b[i] + a[i] * y[i]; for (int i = 0; i < 4; i++) o[i] = v; break; }
        case OP_DP4: { float v = 0; for (int i = 0; i < 4; i++) v += x[i] * b[i] + a[i] * y[i]; for (int i = 0; i < 4; i++) o[i] = v; break; }
        case OP_RCP: for (int i = 0; i < 4; i++) o[i] = -res[i] * res[i] * x[0]; break;
        case OP_RSQ: for (int i = 0; i < 4; i++) o[i] = -0.5f * res[i] * res[i] * res[i] * x[0]; break;
        case OP_MIN: for (int i = 0; i < 4; i++) o[i] = a[i] < b[i] ? x[i] : y[i]; break;
        case OP_MAX: for (int i = 0; i < 4; i++) o[i] = a[i] >= b[i] ? x[i] : y[i]; break;
        case OP_CMP: for (int i = 0; i < 4; i++) o[i] = a[i] >= 0 ? y[i] : z[i]; break;
        case OP_CND: for (int i = 0; i < 4; i++) o[i] = a[i] > 0.5f ? y[i] : z[i]; break;
        case OP_POW: {
            float xa = fabsf(a[0]), r = res[0], v = 0;
            if (xa > 0 && isfinite(r)) v = b[0] * r / xa * (a[0] < 0 ? -x[0] : x[0]) + r * logf(xa) * y[0];
            for (int i = 0; i < 4; i++) o[i] = v;
            break; }
        case OP_EXP: case OP_EXPP: for (int i = 0; i < 4; i++) o[i] = 0.69314718f * res[i] * x[0]; break;
        case OP_LOG: case OP_LOGP: { float v = a[0] != 0 ? x[0] / (fabsf(a[0]) * 0.69314718f) * (a[0] < 0 ? -1.f : 1.f) : 0; for (int i = 0; i < 4; i++) o[i] = v; break; }
        case OP_SINCOS: o[0] = -sinf(a[0]) * x[0]; o[1] = cosf(a[0]) * x[0]; o[2] = o[3] = 0; break;
        case OP_DSX: case OP_DSY: case OP_NRM: default: for (int i = 0; i < 4; i++) o[i] = 0; break;
        }
    }
}

typedef enum { CL_NONE, CL_ALU, CL_FLOW, CL_MAT } OpClass;
static OpClass classify(int op) {
    switch (op) {
    case OP_CALL: case OP_CALLNZ: case OP_LOOP: case OP_RET: case OP_ENDLOOP: case OP_LABEL: case OP_REP: case OP_ENDREP:
    case OP_IF: case OP_IFC: case OP_ELSE: case OP_ENDIF: case OP_BREAK: case OP_BREAKC: case OP_BREAKP: return CL_FLOW;
    case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: return CL_MAT;
    default: return CL_ALU;
    }
}

static inline float dp3f(const float *a, const float *b) { return d3mul(a[0], b[0]) + d3mul(a[1], b[1]) + d3mul(a[2], b[2]); }

static void run(St *s) {
    const D3dShader *sh = s->sh;
    const Instr *ins = sh->ins;
    const int n = sh->nins;
    int pc = 0, nframes = 0, ncalls = 0;
    long budget = BUDGET;
    Frame fr[MAX_LOOPS + 1];
    struct { int ret, nframes; } cs[MAX_CALLS];
    const int vs1x = !sh->pixel && sh->major == 1;
    const int dfl = s->dflag;

    while (pc < n) {
        const Instr *I = &ins[pc++];
        const int op = I->op;
        OpClass cl = classify(op);
        if (cl == CL_FLOW) {
            switch (op) {
            case OP_IF: if (!cond_bool(s, &I->src[0])) pc = I->target + 1; break;
            case OP_IFC: {
                float a[4], b[4]; fetch(s, &I->src[0], 0, a); fetch(s, &I->src[1], 0, b);
                if (!cmp_op(I->ctl, a[0], b[0])) pc = I->target + 1;
                break; }
            case OP_ELSE: pc = I->target + 1; break;
            case OP_ENDIF: break;
            case OP_LOOP: case OP_REP: {
                if (nframes >= MAX_LOOPS) return;
                const int *ci = iconst(s, I->src[op == OP_LOOP ? 1 : 0].idx);
                int count = ci[0];
                if (count <= 0) { pc = I->target + 1; break; }
                if (count > 4096) count = 4096;
                Frame *f = &fr[nframes++];
                f->count = count; f->is_loop = op == OP_LOOP; f->aL_saved = s->aL; f->step = 0;
                if (op == OP_LOOP) { f->step = ci[2]; s->aL = ci[1]; }
                break; }
            case OP_ENDLOOP: case OP_ENDREP: {
                if (nframes <= 0) return;
                Frame *f = &fr[nframes - 1];
                if (--budget <= 0) return;
                if (--f->count > 0) { if (f->is_loop) s->aL = (int)((unsigned)s->aL + (unsigned)f->step); pc = I->target + 1; }
                else { if (f->is_loop) s->aL = f->aL_saved; nframes--; }
                break; }
            case OP_BREAK: case OP_BREAKC: case OP_BREAKP: {
                int go = 1;
                if (op == OP_BREAKC) { float a[4], b[4]; fetch(s, &I->src[0], 0, a); fetch(s, &I->src[1], 0, b); go = cmp_op(I->ctl, a[0], b[0]); }
                else if (op == OP_BREAKP) go = cond_bool(s, &I->src[0]);
                if (go && I->pred && !(pmask(s, I) & 1)) go = 0;
                if (go) {
                    if (nframes <= 0) return;
                    Frame *f = &fr[--nframes];
                    if (f->is_loop) s->aL = f->aL_saved;
                    pc = I->target + 1;
                }
                break; }
            case OP_CALL: case OP_CALLNZ: {
                int go = op == OP_CALL || cond_bool(s, &I->src[1]);
                if (go && I->pred && !(pmask(s, I) & 1)) go = 0;
                if (go) {
                    if (ncalls >= MAX_CALLS || --budget <= 0) return;
                    cs[ncalls].ret = pc; cs[ncalls].nframes = nframes; ncalls++;
                    pc = I->target + 1;
                }
                break; }
            case OP_RET: case OP_LABEL:
                if (op == OP_RET && I->pred && !(pmask(s, I) & 1)) break;
                if (ncalls <= 0) return;
                ncalls--; pc = cs[ncalls].ret; nframes = cs[ncalls].nframes; break;
            default: break;
            }
            continue;
        }
        float a[4] = {0}, b[4] = {0}, c[4] = {0}, res[4] = {0, 0, 0, 0};
        float da[2][4], db[2][4], dc[2][4];
        const int ns = I->nsrc;
        if (cl == CL_ALU) {
            if (ns > 0 && I->src[0].file < RF_OUT_POS) fetch(s, &I->src[0], 0, a);
            if (ns > 1 && I->src[1].file < RF_OUT_POS) fetch(s, &I->src[1], 0, b);
            if (ns > 2 && I->src[2].file < RF_OUT_POS) fetch(s, &I->src[2], 0, c);
            if (dfl) {
                if (ns > 0 && I->src[0].file < RF_OUT_POS) fetch_d(s, &I->src[0], da); else memset(da, 0, sizeof da);
                if (ns > 1 && I->src[1].file < RF_OUT_POS) fetch_d(s, &I->src[1], db); else memset(db, 0, sizeof db);
                if (ns > 2 && I->src[2].file < RF_OUT_POS) fetch_d(s, &I->src[2], dc); else memset(dc, 0, sizeof dc);
                memset(s->dres, 0, sizeof s->dres);
            }
        }
        int nostore = 0;
        switch (op) {
        case OP_MOV: case OP_MOVA: memcpy(res, a, sizeof res); break;
        case OP_ADD: for (int i = 0; i < 4; i++) res[i] = a[i] + b[i]; break;
        case OP_SUB: for (int i = 0; i < 4; i++) res[i] = a[i] - b[i]; break;
        case OP_MUL: for (int i = 0; i < 4; i++) res[i] = d3mul(a[i], b[i]); break;
        case OP_MAD: for (int i = 0; i < 4; i++) res[i] = d3mul(a[i], b[i]) + c[i]; break;
        case OP_RCP: { float x = a[0], r = 1.0f / x; if (vs1x && x == 0) r = x == 0 && signbit(x) ? -FLT_MAX : FLT_MAX; res[0] = res[1] = res[2] = res[3] = r; break; }
        case OP_RSQ: { float x = fabsf(a[0]), r = x == 0 ? (vs1x ? FLT_MAX : INFINITY) : 1.0f / sqrtf(x); res[0] = res[1] = res[2] = res[3] = r; break; }
        case OP_DP3: res[0] = res[1] = res[2] = res[3] = dp3f(a, b); break;
        case OP_DP4: res[0] = res[1] = res[2] = res[3] = dp3f(a, b) + d3mul(a[3], b[3]); break;
        case OP_MIN: for (int i = 0; i < 4; i++) res[i] = a[i] < b[i] ? a[i] : b[i]; break;
        case OP_MAX: for (int i = 0; i < 4; i++) res[i] = a[i] >= b[i] ? a[i] : b[i]; break;
        case OP_SLT: for (int i = 0; i < 4; i++) res[i] = a[i] < b[i] ? 1.f : 0.f; break;
        case OP_SGE: for (int i = 0; i < 4; i++) res[i] = a[i] >= b[i] ? 1.f : 0.f; break;
        case OP_EXP: case OP_EXPP: {
            float x = a[0];
            if (vs1x) { float fl = floorf(x); res[0] = exp2f(fl); res[1] = x - fl; res[2] = exp2f(x); res[3] = 1; }
            else res[0] = res[1] = res[2] = res[3] = exp2f(x);
            break; }
        case OP_LOG: case OP_LOGP: {
            float x = fabsf(a[0]);
            float l = x == 0 ? (vs1x ? -FLT_MAX : -INFINITY) : log2f(x);
            if (vs1x) {
                if (x == 0) { res[0] = -FLT_MAX; res[1] = 1; res[2] = -FLT_MAX; res[3] = 1; }
                else { float e = floorf(l); res[0] = e; res[1] = x / exp2f(e); res[2] = l; res[3] = 1; }
            } else res[0] = res[1] = res[2] = res[3] = l;
            break; }
        case OP_LIT: {
            float p = a[3]; p = p < -127.9961f ? -127.9961f : p > 127.9961f ? 127.9961f : p;
            res[0] = 1; res[1] = 0; res[2] = 0; res[3] = 1;
            if (a[0] > 0) { res[1] = a[0]; if (a[1] > 0) res[2] = powf(a[1], p); }
            break; }
        case OP_DST: res[0] = 1; res[1] = d3mul(a[1], b[1]); res[2] = a[2]; res[3] = b[3]; break;
        case OP_LRP: for (int i = 0; i < 4; i++) res[i] = d3mul(a[i], b[i] - c[i]) + c[i]; break;
        case OP_FRC: for (int i = 0; i < 4; i++) res[i] = a[i] - floorf(a[i]); break;
        case OP_POW: {
            float x = fabsf(a[0]), y = b[0], r = y == 0 ? 1.0f : powf(x, y);
            res[0] = res[1] = res[2] = res[3] = r; break; }
        case OP_CRS:
            res[0] = d3mul(a[1], b[2]) - d3mul(a[2], b[1]); res[1] = d3mul(a[2], b[0]) - d3mul(a[0], b[2]);
            res[2] = d3mul(a[0], b[1]) - d3mul(a[1], b[0]); res[3] = 0; break;
        case OP_SGN: for (int i = 0; i < 4; i++) res[i] = a[i] > 0 ? 1.f : a[i] < 0 ? -1.f : 0.f; break;
        case OP_ABS: for (int i = 0; i < 4; i++) res[i] = fabsf(a[i]); break;
        case OP_NRM: {
            float l = dp3f(a, a);
            if (l > 0 && isfinite(l)) { float inv = 1.0f / sqrtf(l); for (int i = 0; i < 4; i++) res[i] = a[i] * inv; }
            break; }
        case OP_SINCOS: res[0] = cosf(a[0]); res[1] = sinf(a[0]); break;
        case OP_DP2ADD: res[0] = res[1] = res[2] = res[3] = d3mul(a[0], b[0]) + d3mul(a[1], b[1]) + c[0]; break;
        case OP_CND: for (int i = 0; i < 4; i++) res[i] = a[i] > 0.5f ? b[i] : c[i]; break;
        case OP_CMP: for (int i = 0; i < 4; i++) res[i] = a[i] >= 0 ? b[i] : c[i]; break;
        case OP_SETP: {
            if (ns < 2) break;
            for (int k = 0; k < 4; k++) if (I->dst.mask & (1 << k)) s->p[k] = cmp_op(I->ctl, a[k], b[k]);
            nostore = 1; break; }
        case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
            int rows = op == OP_M4x4 || op == OP_M3x4 ? 4 : op == OP_M4x3 || op == OP_M3x3 ? 3 : 2;
            int dim = op == OP_M4x4 || op == OP_M4x3 ? 4 : 3;
            fetch(s, &I->src[0], 0, a);
            for (int r = 0; r < rows; r++) {
                fetch(s, &I->src[1], r, b);
                res[r] = dp3f(a, b) + (dim == 4 ? d3mul(a[3], b[3]) : 0.0f);
            }
            if (dfl) {
                float dd[2][4]; fetch_d(s, &I->src[0], dd);
                memset(s->dres, 0, sizeof s->dres);
                for (int k = 0; k < 2; k++) for (int r = 0; r < rows; r++) { fetch(s, &I->src[1], r, b); float v = 0; for (int j = 0; j < dim; j++) v += dd[k][j] * b[j]; s->dres[k][r] = v; }
            }
            break; }
        case OP_DSX: case OP_DSY: {
            if (dfl) for (int i = 0; i < 4; i++) res[i] = da[op == OP_DSX ? 0 : 1][i];
            break; }
        case OP_TEXKILL: {
            const float *p = s->rb[I->dst.file] + (I->dst.idx < LIM[I->dst.file] ? I->dst.idx : 0) * 4;
            if (I->dst.file == RF_ZERO) p = s->zero;
            if (p[0] < 0 || p[1] < 0 || p[2] < 0) { if (!I->pred || (pmask(s, I) & 1)) { s->depth_set = -1; return; } }
            nostore = 1; break; }
        case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: {
            if (sh->ps1x) {
                const SgPixelIn *pin = s->pin;
                int stage = I->dst.idx;
                if (sh->minor < 4) {
                    sample_tex(s, stage, pin->v[stage], pin->dx[stage], pin->dy[stage], NAN, res);
                } else {
                    const float *dx = NULL, *dy = NULL;
                    if (I->src[0].file == RF_TREG) { dx = pin->dx[I->src[0].idx]; dy = pin->dy[I->src[0].idx]; }
                    sample_tex(s, stage, a, dx, dy, NAN, res);
                }
                break;
            }
            int stage = I->src[1].idx;
            float cc[4] = { a[0], a[1], a[2], a[3] };
            float dx[4] = {0}, dy[4] = {0};
            if (op == OP_TEXLDD) {
                fetch(s, &I->src[2], 0, dx); fetch(s, &I->src[3], 0, dy);
                sample_tex(s, stage, cc, dx, dy, NAN, res); break;
            }
            if (dfl) { memcpy(dx, da[0], 16); memcpy(dy, da[1], 16); }
            if (op == OP_TEXLDL) { sample_tex(s, stage, cc, NULL, NULL, a[3], res); break; }
            if (I->ctl == 1 && a[3] != 0) {           // texldp
                float w = a[3], iw = 1.0f / w;
                for (int k = 0; k < 3; k++) {
                    cc[k] = a[k] * iw;
                    dx[k] = (da[0][k] * w - a[k] * da[0][3]) * iw * iw;
                    dy[k] = (da[1][k] * w - a[k] * da[1][3]) * iw * iw;
                }
                cc[3] = 1;
            }
            if (I->ctl == 2) {                         // texldb: bias in w
                float bias = a[3];
                int has = dx[0] != 0 || dx[1] != 0 || dx[2] != 0 || dy[0] != 0 || dy[1] != 0 || dy[2] != 0;
                if (has) { float sc = exp2f(bias); for (int k = 0; k < 4; k++) { dx[k] *= sc; dy[k] *= sc; } sample_tex(s, stage, cc, dx, dy, NAN, res); }
                else sample_tex(s, stage, cc, NULL, NULL, bias, res);
                break;
            }
            sample_tex(s, stage, cc, dx, dy, NAN, res);
            break; }
        // ---- ps 1.x texture addressing ----
        case OP_TEXCOORD: {
            if (sh->minor < 4) { const float *v = s->pin->v[I->dst.idx]; res[0] = sat01(v[0]); res[1] = sat01(v[1]); res[2] = sat01(v[2]); res[3] = 1; }
            else { res[0] = a[0]; res[1] = a[1]; res[2] = a[2]; res[3] = 1; }
            break; }
        case OP_TEXBEM: case OP_TEXBEML: {
            int n = I->dst.idx; const float *cv = s->pin->v[n], *m = s->pc->bump_mat[n];
            float cc[4] = { cv[0] + m[0] * a[0] + m[2] * a[1], cv[1] + m[1] * a[0] + m[3] * a[1], cv[2], cv[3] };
            sample_tex(s, n, cc, s->pin->dx[n], s->pin->dy[n], NAN, res);
            if (op == OP_TEXBEML) { float L = sat01(a[2] * s->pc->bump_lscale[n] + s->pc->bump_loffset[n]); res[0] *= L; res[1] *= L; res[2] *= L; }
            break; }
        case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXREG2RGB: {
            int n = I->dst.idx;
            float cc[4] = { 0, 0, 0, 1 };
            if (op == OP_TEXREG2AR) { cc[0] = a[3]; cc[1] = a[0]; } else if (op == OP_TEXREG2GB) { cc[0] = a[1]; cc[1] = a[2]; }
            else { cc[0] = a[0]; cc[1] = a[1]; cc[2] = a[2]; }
            sample_tex(s, n, cc, NULL, NULL, NAN, res); break; }
        case OP_TEXM3x2PAD: case OP_TEXM3x3PAD:
            s->row[s->npad & 3] = dp3f(s->pin->v[I->dst.idx], a); s->npad++;
            nostore = 1; break;
        case OP_TEXM3x2TEX: case OP_TEXM3x3TEX: case OP_TEXM3x3: case OP_TEXM3x3SPEC: case OP_TEXM3x3VSPEC: {
            int n = I->dst.idx;
            float v = dp3f(s->pin->v[n], a);
            float cc[4] = { s->row[0], s->row[1], 0, 1 };
            if (op == OP_TEXM3x2TEX) { cc[0] = s->row[0]; cc[1] = v; cc[2] = 0; }
            else { cc[0] = s->row[0]; cc[1] = s->row[1]; cc[2] = v; }
            s->npad = 0;
            if (op == OP_TEXM3x3) { res[0] = cc[0]; res[1] = cc[1]; res[2] = cc[2]; res[3] = 1; break; }
            if (op == OP_TEXM3x3SPEC || op == OP_TEXM3x3VSPEC) {
                float e[3];
                if (op == OP_TEXM3x3SPEC) { e[0] = b[0]; e[1] = b[1]; e[2] = b[2]; }
                else { e[0] = s->pin->v[(n - 2) & 7][3]; e[1] = s->pin->v[(n - 1) & 7][3]; e[2] = s->pin->v[n & 7][3]; }
                float nn = cc[0] * cc[0] + cc[1] * cc[1] + cc[2] * cc[2], ne = cc[0] * e[0] + cc[1] * e[1] + cc[2] * e[2];
                float k = nn != 0 ? 2 * ne / nn : 0;
                cc[0] = cc[0] * k - e[0]; cc[1] = cc[1] * k - e[1]; cc[2] = cc[2] * k - e[2];
            }
            sample_tex(s, n, cc, NULL, NULL, NAN, res); break; }
        case OP_TEXDP3TEX: {
            float cc[4] = { dp3f(s->pin->v[I->dst.idx], a), 0, 0, 1 };
            sample_tex(s, I->dst.idx, cc, NULL, NULL, NAN, res); break; }
        case OP_TEXDP3: res[0] = res[1] = res[2] = res[3] = dp3f(s->pin->v[I->dst.idx], a); break;
        case OP_TEXM3x2DEPTH: {
            float v = dp3f(s->pin->v[I->dst.idx], a);
            s->depth_val = v != 0 ? s->row[0] / v : 1.0f; s->depth_set = 1; s->npad = 0; nostore = 1; break; }
        case OP_TEXDEPTH: {
            const float *p = s->rb[I->dst.file] + I->dst.idx * 4;
            s->depth_val = p[1] != 0 ? p[0] / p[1] : 1.0f; s->depth_set = 1; nostore = 1; break; }
        case OP_BEM: {
            int n = I->dst.idx < 16 ? I->dst.idx : 0; const float *m = s->pc ? s->pc->bump_mat[n] : s->zero;
            res[0] = a[0] + m[0] * b[0] + m[2] * b[1]; res[1] = a[1] + m[1] * b[0] + m[3] * b[1]; break; }
        default: nostore = 1; break;
        }
        if (nostore) continue;
        if (dfl) {
            if (op != OP_M4x4 && op != OP_M4x3 && op != OP_M3x4 && op != OP_M3x3 && op != OP_M3x2 && op != OP_TEX && op != OP_TEXLDL && op != OP_TEXLDD)
                deriv_alu(s, I, a, b, c, da, db, dc, res, s->dres);
            else if (op == OP_TEX || op == OP_TEXLDL || op == OP_TEXLDD) memset(s->dres, 0, sizeof s->dres);
        }
        store(s, I, res);
    }
}

// ---- entry points ---------------------------------------------------------------------------------------

static void setup_common(St *s, const D3dShader *sh, const D3dShaderConsts *c) {
    s->sh = sh; s->c = c;
    for (int i = 0; i < RF_COUNT; i++) s->rb[i] = s->sink;
    memset(s->zero, 0, sizeof s->zero); memset(s->sink, 0, sizeof s->sink);
    s->rb[RF_TEMP] = &s->r[0][0]; s->rb[RF_CONST] = (float *)c->f; s->rb[RF_DEFC] = sh->deff ? &sh->deff[0][0] : s->zero;
    s->rb[RF_ZERO] = s->zero; s->rb[RF_MISC] = &s->misc[0][0]; s->rb[RF_TREG] = &s->t[0][0];
    s->rb[RF_OUT_PSIZE] = s->psz; s->rb[RF_OUT_DEPTH] = s->depth;
    s->a0 = s->aL = 0; memset(s->p, 0, sizeof s->p);
    memset(s->r, 0, (size_t)sh->ntemps * 16);
    s->npad = 0; s->depth_set = 0; s->dflag = 0; s->ps1x = 0; s->pc = NULL; s->pin = NULL;
    memset(s->row, 0, sizeof s->row);
    memset(s->dbx, 0, sizeof s->dbx); memset(s->dby, 0, sizeof s->dby);
    memset(s->psz, 0, sizeof s->psz); memset(s->depth, 0, sizeof s->depth);
    s->tex = NULL; s->samp = NULL; s->ntex = 0;
}

static void vs_one(St *s, const D3dShader *sh, const D3dVsCtx *ctx, const float in[D3DSH_VS_INPUTS][4], D3dVsOut *out) {
    memset(out, 0, sizeof *out);
    setup_common(s, sh, ctx->c);
    s->rb[RF_INPUT] = (float *)in;
    s->rb[RF_OUT_POS] = out->pos;
    s->rb[RF_OUT_VARY] = &out->v[0][0];
    s->tex = ctx->tex; s->samp = ctx->samp; s->ntex = D3DSH_VS_TEXTURES;
    run(s);
    out->psize = s->psz[0];
    out->written_mask = sh->vs_out_mask;
}

void d3dsh_run_vs_ex(const D3dShader *vs, const D3dVsCtx *ctx, const float in[D3DSH_VS_INPUTS][4], D3dVsOut *out) {
    St s;
    if (!vs || vs->pixel || !ctx || !ctx->c) { memset(out, 0, sizeof *out); return; }
    vs_one(&s, vs, ctx, in, out);
}
void d3dsh_run_vs(const D3dShader *vs, const D3dShaderConsts *c, const float in[D3DSH_VS_INPUTS][4], D3dVsOut *out) {
    D3dVsCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.c = c;
    d3dsh_run_vs_ex(vs, &ctx, in, out);
}
void d3dsh_run_vs_batch(const D3dShader *vs, const D3dVsCtx *ctx, const float (*in)[D3DSH_VS_INPUTS][4], D3dVsOut *out, int n) {
    St s;
    if (!vs || vs->pixel || !ctx || !ctx->c) { if (n > 0) memset(out, 0, sizeof *out * (size_t)n); return; }
    for (int i = 0; i < n; i++) vs_one(&s, vs, ctx, in[i], &out[i]);
}

int d3dsh_ps_pixel(void *vctx, const SgPixelIn *in, float color_out[4], float *depth_out) {
    const D3dPsCtx *pc = (const D3dPsCtx *)vctx;
    const D3dShader *sh = pc->ps;
    St s;
    if (!sh || !sh->pixel || !pc->c) { color_out[0] = color_out[1] = color_out[2] = 0; color_out[3] = 1; return 1; }
    setup_common(&s, sh, pc->c);
    s.pc = pc; s.pin = in; s.ps1x = sh->ps1x;
    s.rb[RF_VARY] = (float *)&in->v[0][0];
    s.rb[RF_OUT_COLOR] = &s.col[0][0];
    memset(s.col, 0, sizeof s.col);
    s.tex = pc->tex; s.samp = pc->samp; s.ntex = D3DSH_MAX_SAMPLERS;
    if (sh->ps1x) {
        int nv = sh->ps_nvary < 8 ? sh->ps_nvary : 8;
        memset(s.t, 0, sizeof s.t);
        for (int k = 0; k < nv; k++) memcpy(s.t[k], in->v[k], 16);
    }
    s.misc[0][0] = floorf(in->pos[0]); s.misc[0][1] = floorf(in->pos[1]); s.misc[0][2] = s.misc[0][3] = 0;
    s.misc[1][0] = s.misc[1][1] = s.misc[1][2] = s.misc[1][3] = in->front_facing ? 1.0f : -1.0f;
    s.depth_val = in->pos[2];
    if (sh->deriv) {
        s.dflag = 1;
        memset(s.dr, 0, (size_t)sh->ntemps * 16);
        memset(s.dr[1], 0, (size_t)sh->ntemps * 16);
        s.dbx[RF_VARY] = &in->dx[0][0]; s.dby[RF_VARY] = &in->dy[0][0];
        s.dbx[RF_TEMP] = &s.dr[0][0][0]; s.dby[RF_TEMP] = &s.dr[1][0][0];
    }
    run(&s);
    if (s.depth_set < 0) return 0;
    const float *o = sh->ps1x ? s.r[0] : s.col[0];
    for (int k = 0; k < 4; k++) color_out[k] = sat01(o[k]);
    if (sh->writes_depth) *depth_out = s.depth_set > 0 ? sat01(s.depth_val) : sat01(s.depth[0]);
    return 1;
}
