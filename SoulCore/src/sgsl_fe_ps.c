// SoulCore - sgsl front end for Direct3D 9 pixel shaders (ps_1_1 .. ps_3_0). A line-by-line translation of the
// interpreter d3dsh_ps_pixel (d3d9_shader_exec.c): every register access, source modifier, destination
// modifier, instruction and the forward-mode derivative propagation (the CPU engine derives texture gradients
// analytically; so does the generated code) are reproduced.
#include "sgsl_priv.h"
#include "d3d9_shader_priv.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct Ps {
    Gb *g;
    const SgState *s;
    const SgProgram *p;
    const D3dShader *sh;
    int ps1x, minor, dfl, ntemps;
    SgVar *r, *tr, *pr, *aL, *colv, *depthv, *depth_val, *dr[2];
    SgVar *row[4];
    SgVar *va, *vb, *vc, *res, *da[2], *db[2], *dc[2], *dres[2], *cc, *ddx, *ddy, *ix;
    int npad, depth_set;
    int call_stack[40], ncall;
} Ps;

#define G (P->g)
#define EV(v) ex_var(G, v)
#define V4F(fn, ...) V4C(G, fn, __VA_ARGS__)
#define FF(fn, ...) FC(G, fn, __VA_ARGS__)

static const int LIMF[RF_COUNT] = { [RF_TEMP] = 32, [RF_INPUT] = 16, [RF_VARY] = SG_VARYINGS, [RF_CONST] = 256, [RF_DEFC] = 256, [RF_TREG] = 8, [RF_MISC] = 2, [RF_ZERO] = 1 };

static SgExpr *zero4(Ps *P) { return ex_v4(G, 0, 0, 0, 0); }

// constant register row `idx` as the interpreter's cptr() sees it
static SgExpr *const_row(Ps *P, int idx) {
    if (idx < 0 || idx >= 256) return zero4(P);
    const D3dShader *sh = P->sh;
    if (sh->deff && (sh->defmask[idx >> 5] & (1u << (idx & 31)))) {
        const float *f = sh->deff[idx];
        return ex_v4(G, f[0], f[1], f[2], f[3]);
    }
    if (idx + 1 > G->max_const) G->max_const = idx + 1;
    return ex_raw(G, TY_V4, "u.c[%d]", idx);
}

static SgExpr *pos_ex(Ps *P) { G->uses_pos = 1; return EV(G->pos); }

static SgExpr *reg_static(Ps *P, int file, int idx) {
    if (idx < 0 || idx >= LIMF[file]) return zero4(P);
    switch (file) {
    case RF_TEMP: return idx < P->ntemps ? ex_elem(G, P->r, idx) : zero4(P);
    case RF_VARY: return ex_elem(G, G->vy, idx);
    case RF_TREG: return ex_elem(G, P->tr, idx);
    case RF_MISC:
        if (idx == 0) return V4F("d3_vpos", pos_ex(P));
        G->uses_front = 1;
        return V4F("d3_vface", EV(G->front));
    case RF_DEFC: { const float *f = P->sh->deff[idx]; return ex_v4(G, f[0], f[1], f[2], f[3]); }
    case RF_CONST: return const_row(P, idx);
    default: return zero4(P);
    }
}

// Raw (unswizzled, unmodified) register value at idx + off (+ aL when relative). A relative operand stores its index
// in the variable `ix`; the caller must emit the statement that consumes the expression immediately.
static SgExpr *operand_raw(Ps *P, const Operand *o, int off) {
    int file = o->file, idx = o->idx + off;
    if (o->rel) {
        if (o->rel != 2) { gb_fail(G, "a0 relative addressing in a pixel shader"); return zero4(P); }
        st_assign(G, EV(P->ix), IC(G, "sg_relidx", ex_i(G, idx), EV(P->aL)));
        SgExpr *e = EV(P->ix);
        if (file == RF_CONST) {
            G->max_const = 256; G->rel_const = 1; G->need_defc_fn = 1;
            return V4F("sg_cfetch", e, ex_raw(G, TY_V4, "u.c[sg_clampi(%s, 0, 255)]", P->ix->name));
        }
        if (file == RF_INPUT && P->sh->pixel) {
            G->in_slot_table = 1;
            SgExpr *inr = ex_bin(G, TY_B, "&&", ex_bin(G, TY_B, ">=", e, ex_i(G, 0)), ex_bin(G, TY_B, "<", e, ex_i(G, 16)));
            SgExpr *slot = ex_raw(G, TY_I, "sg_in_slot[%s]", P->ix->name);
            SgExpr *ok = ex_bin(G, TY_B, "&&", inr, ex_bin(G, TY_B, ">=", slot, ex_i(G, 0)));
            return ex_tern(G, ok, ex_index(G, G->vy, ex_raw(G, TY_I, "sg_in_slot[%s]", P->ix->name)), zero4(P));
        }
        SgExpr *inr = ex_bin(G, TY_B, "&&", ex_bin(G, TY_B, ">=", e, ex_i(G, 0)), ex_bin(G, TY_B, "<", e, ex_i(G, file == RF_TEMP ? P->ntemps : SG_VARYINGS)));
        if (file == RF_TEMP) return ex_tern(G, inr, ex_index(G, P->r, e), zero4(P));
        if (file == RF_VARY) return ex_tern(G, inr, ex_index(G, G->vy, e), zero4(P));
        gb_fail(G, "relative addressing of register file %d", file);
        return zero4(P);
    }
    if (file == RF_CONST || (file == RF_DEFC && off)) return const_row(P, idx);
    return reg_static(P, file, idx);
}

// source operand: raw value, constant clamp (ps 1.x), swizzle, modifier
static SgExpr *src_expr(Ps *P, const Operand *o, int off) {
    SgExpr *e = operand_raw(P, o, off);
    if (o->clampc) e = V4F("sg_clamp114", e);
    e = ex_swz(G, e, o->swz);
    if (o->mod) e = V4F("d3_mod", ex_i(G, o->mod), e);
    return e;
}

// derivative of a source operand (forward mode): dx/dy expressions
static void src_deriv(Ps *P, const Operand *o, SgExpr **dx, SgExpr **dy) {
    *dx = zero4(P); *dy = zero4(P);
    if (!(o->file == RF_VARY || o->file == RF_TEMP) || o->rel || o->clampc) return;
    int idx = o->idx;
    if (idx >= LIMF[o->file]) return;
    SgExpr *sx, *sy;
    if (o->file == RF_VARY) { sx = ex_elem(G, G->dvx, idx); sy = ex_elem(G, G->dvy, idx); G->uses_deriv = 1; }
    else {
        if (idx >= P->ntemps) return;
        sx = ex_elem(G, P->dr[0], idx); sy = ex_elem(G, P->dr[1], idx);
    }
    sx = ex_swz(G, sx, o->swz); sy = ex_swz(G, sy, o->swz);
    switch (o->mod) {
    case SM_NEG: case SM_BIASNEG: case SM_COMP: sx = V4F("sg_scale4", sx, ex_f(G, -1.f)); sy = V4F("sg_scale4", sy, ex_f(G, -1.f)); break;
    case SM_SIGN: case SM_X2: sx = V4F("sg_scale4", sx, ex_f(G, 2.f)); sy = V4F("sg_scale4", sy, ex_f(G, 2.f)); break;
    case SM_SIGNNEG: case SM_X2NEG: sx = V4F("sg_scale4", sx, ex_f(G, -2.f)); sy = V4F("sg_scale4", sy, ex_f(G, -2.f)); break;
    case SM_ABS: case SM_ABSNEG: {
        SgExpr *raw = ex_swz(G, reg_static(P, o->file, idx), o->swz);
        SgExpr *neg = ex_i(G, o->mod == SM_ABSNEG);
        sx = V4F("d_absd", raw, sx, neg); sy = V4F("d_absd", raw, sy, neg);
        break; }
    default: break;
    }
    *dx = sx; *dy = sy;
}

static SgExpr *pcond(Ps *P, const Instr *I, int c) {   // component c of the predicate mask
    int k = (I->predswz >> (2 * c)) & 3;
    return ex_bin(G, TY_B, I->pred == 2 ? "==" : "!=", ex_elem(G, P->pr, k), ex_i(G, 0));
}

static SgExpr *dst_reg(Ps *P, const DestOp *d) {
    switch (d->file) {
    case RF_TEMP: return d->idx < P->ntemps ? ex_elem(G, P->r, d->idx) : NULL;
    case RF_TREG: return d->idx < 8 ? ex_elem(G, P->tr, d->idx) : NULL;
    case RF_OUT_COLOR: return d->idx < 4 ? ex_elem(G, P->colv, d->idx) : NULL;
    case RF_OUT_DEPTH: return EV(P->depthv);
    default: return NULL;
    }
}

// store res (V4 variable) and its derivatives into the destination register
static void store(Ps *P, const Instr *I, int with_deriv) {
    const DestOp *d = &I->dst;
    SgExpr *reg = dst_reg(P, d);
    if (!reg) return;
    int shift = d->shift & 15; if (shift & 8) shift -= 16;
    float scale = shift ? ldexpf(1.0f, shift) : 1.0f;
    float lim = P->ps1x ? (P->minor >= 4 ? 8.0f : 1.0f) : 0;
    for (int c = 0; c < 4; c++) {
        if (!(d->mask & (1 << c))) continue;
        if (I->pred) st_if(G, pcond(P, I, c));
        SgExpr *v = ex_comp(G, EV(P->res), c);
        if (shift) v = ex_bin(G, TY_F, "*", v, ex_f(G, scale));
        if (d->sat) v = FF("sg_sat1", v);
        else if (lim) v = FF("sg_clampf", v, ex_f(G, -lim), ex_f(G, lim));
        st_assign(G, ex_comp(G, reg, c), v);
        if (with_deriv && d->file == RF_TEMP && d->idx < P->ntemps)
            for (int k = 0; k < 2; k++) {
                SgExpr *dv = ex_comp(G, EV(P->dres[k]), c);
                if (shift) dv = ex_bin(G, TY_F, "*", dv, ex_f(G, scale));
                st_assign(G, ex_comp(G, ex_elem(G, P->dr[k], d->idx), c), dv);
            }
        if (I->pred) st_endif(G);
    }
}

static int tex_usable(Ps *P, int stage) {
    if (stage < 0 || stage >= 16) return 0;
    const SgTexture *t = P->p->samplers[stage].tex;
    return t && t->rgba && t->w > 0 && t->h > 0;
}

// dst = sample(stage, coord, ...) with the interpreter's sample_tex(): unbound textures give (0,0,0,1)
static void emit_sample(Ps *P, SgVar *dst, int stage, SgExpr *coord, SgExpr *dx, SgExpr *dy, SgExpr *lod) {
    if (!tex_usable(P, stage)) { st_assign(G, EV(dst), ex_v4(G, 0, 0, 0, 1)); return; }
    int kind = P->p->samplers[stage].tex->kind;
    if (lod) st_sample(G, EV(dst), stage, kind, SMP_LEVEL, coord, NULL, NULL, lod);
    else if (dx) st_sample(G, EV(dst), stage, kind, SMP_GRAD, coord, dx, dy, NULL);
    else st_sample(G, EV(dst), stage, kind, SMP_BASE, coord, NULL, NULL, NULL);
}

static SgExpr *cmp_expr(Ps *P, int code, SgExpr *a, SgExpr *b) {
    const char *op = NULL;
    switch (code) {
    case CMP_GT: op = ">"; break; case CMP_EQ: op = "=="; break; case CMP_GE: op = ">="; break;
    case CMP_LT: op = "<"; break; case CMP_NE: op = "!="; break; case CMP_LE: op = "<="; break;
    default: return ex_b(G, 0);
    }
    return ex_bin(G, TY_B, op, a, b);
}

static SgExpr *cond_bool(Ps *P, const Operand *o) {
    SgExpr *v;
    if (o->file == RF_PRED) v = ex_bin(G, TY_B, "!=", ex_elem(G, P->pr, o->swz & 3), ex_i(G, 0));
    else if (o->idx < 16 && ((P->sh->defb_mask >> o->idx) & 1)) v = ex_b(G, P->sh->defb[o->idx]);
    else v = ex_bin(G, TY_B, "!=", ex_raw(G, TY_I, "u.b[%d]", o->idx & 15), ex_i(G, 0));
    return o->mod == SM_NOT ? ex_un(G, TY_B, "!", v) : v;
}

static SgExpr *iconst(Ps *P, int idx, int comp) {
    const D3dShader *sh = P->sh;
    if (idx < 16 && ((sh->defi_mask >> idx) & 1)) return ex_i(G, sh->defi[idx][comp]);
    return ex_raw(G, TY_I, "u.i[%d].%c", idx & 15, "xyzw"[comp]);
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

static SgExpr *dres_expr(Ps *P, const Instr *I, int k) {
    SgExpr *a = EV(P->va), *b = EV(P->vb), *c = EV(P->vc), *res = EV(P->res);
    SgExpr *x = EV(P->da[k]), *y = EV(P->db[k]), *z = EV(P->dc[k]);
    switch (I->op) {
    case OP_MOV: case OP_FRC: return x;
    case OP_ABS: return V4F("d_abs", a, x);
    case OP_ADD: return V4F("sg_add4", x, y);
    case OP_SUB: return V4F("sg_sub4", x, y);
    case OP_MUL: return V4F("d_mul", a, x, b, y);
    case OP_MAD: return V4F("d_mad", a, x, b, y, z);
    case OP_LRP: return V4F("d_lrp", a, x, b, y, c, z);
    case OP_DP2ADD: return V4F("d_dp2add", a, x, b, y, z);
    case OP_DP3: return V4F("d_dp3", a, x, b, y);
    case OP_DP4: return V4F("d_dp4", a, x, b, y);
    case OP_RCP: return V4F("d_rcp", res, x);
    case OP_RSQ: return V4F("d_rsq", res, x);
    case OP_MIN: return V4F("d_min", a, b, x, y);
    case OP_MAX: return V4F("d_max", a, b, x, y);
    case OP_CMP: return V4F("d_cmp", a, y, z);
    case OP_CND: return V4F("d_cnd", a, y, z);
    case OP_POW: return V4F("d_pow", a, b, res, x, y);
    case OP_EXP: case OP_EXPP: return V4F("d_exp", res, x);
    case OP_LOG: case OP_LOGP: return V4F("d_log", a, x);
    case OP_SINCOS: return V4F("d_sincos", a, x);
    default: return zero4(P);
    }
}

static void emit_instr(Ps *P, const Instr *I, int pc) {
    const int op = I->op, ns = I->nsrc;
    const D3dShader *sh = P->sh;
    OpClass cl = classify(op);
    st_comment(G, "pc %d: opcode %d", pc, op);
    SgExpr *res = NULL;                 // V4 result expression (assigned to P->res)
    SgExpr *dres[2] = { NULL, NULL };   // matrix ops / texture ops provide their own derivative
    int nostore = 0, special_deriv = 0;
    if (cl == CL_ALU) {
        for (int k = 0; k < 3 && k < ns; k++) {
            if (I->src[k].file >= RF_OUT_POS) continue;
            SgVar *v = k == 0 ? P->va : k == 1 ? P->vb : P->vc;
            st_assign(G, EV(v), src_expr(P, &I->src[k], 0));
        }
        if (P->dfl) {
            for (int k = 0; k < 3; k++) {
                SgVar *d0 = k == 0 ? P->da[0] : k == 1 ? P->db[0] : P->dc[0], *d1 = k == 0 ? P->da[1] : k == 1 ? P->db[1] : P->dc[1];
                SgExpr *dx = zero4(P), *dy = zero4(P);
                if (k < ns && I->src[k].file < RF_OUT_POS) src_deriv(P, &I->src[k], &dx, &dy);
                st_assign(G, EV(d0), dx); st_assign(G, EV(d1), dy);
            }
        }
    }
    SgExpr *a = EV(P->va), *b = EV(P->vb), *c = EV(P->vc);
    switch (op) {
    case OP_MOV: res = a; break;
    case OP_ADD: res = V4F("sg_add4", a, b); break;
    case OP_SUB: res = V4F("sg_sub4", a, b); break;
    case OP_MUL: res = V4F("d3_mul4", a, b); break;
    case OP_MAD: res = V4F("sg_add4", V4F("d3_mul4", a, b), c); break;
    case OP_RCP: res = V4F("d3_rcp", a); break;
    case OP_RSQ: res = V4F("d3_rsq", a); break;
    case OP_DP3: res = V4F("d3_dp3", a, b); break;
    case OP_DP4: res = V4F("d3_dp4", a, b); break;
    case OP_MIN: res = V4F("d3_min4", a, b); break;
    case OP_MAX: res = V4F("d3_max4", a, b); break;
    case OP_SLT: res = V4F("d3_slt4", a, b); break;
    case OP_SGE: res = V4F("d3_sge4", a, b); break;
    case OP_EXP: case OP_EXPP: res = V4F("d3_exp", a); break;
    case OP_LOG: case OP_LOGP: res = V4F("d3_log", a); break;
    case OP_LIT: res = V4F("d3_lit", a); break;
    case OP_DST: res = V4F("d3_dst", a, b); break;
    case OP_LRP: res = V4F("d3_lrp", a, b, c); break;
    case OP_FRC: res = V4F("d3_frc", a); break;
    case OP_POW: res = V4F("d3_pow", a, b); break;
    case OP_CRS: res = V4F("d3_crs", a, b); break;
    case OP_SGN: res = V4F("d3_sgn", a); break;
    case OP_ABS: res = V4F("d3_abs", a); break;
    case OP_NRM: res = V4F("d3_nrm", a); break;
    case OP_SINCOS: res = V4F("d3_sincos", a); break;
    case OP_DP2ADD: res = V4F("d3_dp2add", a, b, c); break;
    case OP_CND: res = V4F("d3_cnd", a, b, c); break;
    case OP_CMP: res = V4F("d3_cmp", a, b, c); break;
    case OP_SETP:
        if (ns < 2) break;
        for (int k = 0; k < 4; k++)
            if (I->dst.mask & (1 << k)) st_assign(G, ex_elem(G, P->pr, k), ex_tern(G, cmp_expr(P, I->ctl, ex_comp(G, a, k), ex_comp(G, b, k)), ex_i(G, 1), ex_i(G, 0)));
        nostore = 1; break;
    case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
        int rows = op == OP_M4x4 || op == OP_M3x4 ? 4 : op == OP_M4x3 || op == OP_M3x3 ? 3 : 2;
        int dim = op == OP_M4x4 || op == OP_M4x3 ? 4 : 3;
        st_assign(G, EV(P->va), src_expr(P, &I->src[0], 0));
        SgExpr *m[4] = { ex_f(G, 0), ex_f(G, 0), ex_f(G, 0), ex_f(G, 0) };
        for (int r = 0; r < rows; r++) {
            st_assign(G, EV(P->vb), src_expr(P, &I->src[1], r));
            m[r] = FF("d3_mrow", EV(P->va), EV(P->vb), ex_i(G, dim));
            SgVar *tv = gb_tmp(G, TY_F);
            st_assign(G, EV(tv), m[r]);
            m[r] = EV(tv);
        }
        res = V4F("mk4", m[0], m[1], m[2], m[3]);
        if (P->dfl) {
            SgExpr *dd0, *dd1;
            src_deriv(P, &I->src[0], &dd0, &dd1);
            st_assign(G, EV(P->da[0]), dd0); st_assign(G, EV(P->da[1]), dd1);
            SgExpr *dm[2][4];
            for (int k = 0; k < 2; k++) for (int r = 0; r < 4; r++) dm[k][r] = ex_f(G, 0);
            for (int r = 0; r < rows; r++) {
                st_assign(G, EV(P->vb), src_expr(P, &I->src[1], r));
                for (int k = 0; k < 2; k++) {
                    SgVar *tv = gb_tmp(G, TY_F);
                    st_assign(G, EV(tv), FF("d3_mdot", EV(P->da[k]), EV(P->vb), ex_i(G, dim)));
                    dm[k][r] = EV(tv);
                }
            }
            for (int k = 0; k < 2; k++) dres[k] = V4F("mk4", dm[k][0], dm[k][1], dm[k][2], dm[k][3]);
            special_deriv = 1;
        }
        break; }
    case OP_DSX: case OP_DSY:
        res = P->dfl ? EV(P->da[op == OP_DSX ? 0 : 1]) : zero4(P);
        break;
    case OP_TEXKILL: {
        SgExpr *p = I->dst.file == RF_ZERO ? zero4(P) : reg_static(P, I->dst.file, I->dst.idx < LIMF[I->dst.file] ? I->dst.idx : 0);
        SgExpr *kc = BC(G, "d3_killtest", p);
        if (I->pred) kc = ex_bin(G, TY_B, "&&", kc, pcond(P, I, 0));
        st_if(G, kc); st_kill(G); st_endif(G);
        nostore = 1; break; }
    case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: {
        if (P->ps1x) {
            int stage = I->dst.idx;
            if (P->minor < 4) {
                G->uses_deriv = 1;
                emit_sample(P, P->res, stage, ex_elem(G, G->vy, stage < SG_VARYINGS ? stage : 0), ex_elem(G, G->dvx, stage < SG_VARYINGS ? stage : 0), ex_elem(G, G->dvy, stage < SG_VARYINGS ? stage : 0), NULL);
            } else {
                SgExpr *dx = NULL, *dy = NULL;
                if (I->src[0].file == RF_TREG) { G->uses_deriv = 1; dx = ex_elem(G, G->dvx, I->src[0].idx); dy = ex_elem(G, G->dvy, I->src[0].idx); }
                st_assign(G, EV(P->cc), a);
                emit_sample(P, P->res, stage, EV(P->cc), dx, dy, NULL);
            }
            res = EV(P->res);
            break;
        }
        int stage = I->src[1].idx;
        st_assign(G, EV(P->cc), a);
        if (op == OP_TEXLDD) {
            st_assign(G, EV(P->ddx), src_expr(P, &I->src[2], 0));
            st_assign(G, EV(P->ddy), src_expr(P, &I->src[3], 0));
            emit_sample(P, P->res, stage, EV(P->cc), EV(P->ddx), EV(P->ddy), NULL);
            res = EV(P->res); break;
        }
        st_assign(G, EV(P->ddx), P->dfl ? EV(P->da[0]) : zero4(P));
        st_assign(G, EV(P->ddy), P->dfl ? EV(P->da[1]) : zero4(P));
        if (op == OP_TEXLDL) { emit_sample(P, P->res, stage, EV(P->cc), NULL, NULL, ex_comp(G, a, 3)); res = EV(P->res); break; }
        if (I->ctl == 1) {                                  // texldp
            st_assign(G, EV(P->ddx), V4F("d3_projd", a, EV(P->ddx)));
            st_assign(G, EV(P->ddy), V4F("d3_projd", a, EV(P->ddy)));
            st_assign(G, EV(P->cc), V4F("d3_projc", a));
        }
        if (I->ctl == 2) {                                  // texldb: bias in w
            SgExpr *bias = ex_comp(G, a, 3);
            st_if(G, BC(G, "d3_hasd", EV(P->ddx), EV(P->ddy)));
            SgVar *sc = gb_tmp(G, TY_F);
            st_assign(G, EV(sc), FF("sg_exp2", bias));
            st_assign(G, EV(P->ddx), V4F("sg_scale4", EV(P->ddx), EV(sc)));
            st_assign(G, EV(P->ddy), V4F("sg_scale4", EV(P->ddy), EV(sc)));
            emit_sample(P, P->res, stage, EV(P->cc), EV(P->ddx), EV(P->ddy), NULL);
            st_else(G);
            emit_sample(P, P->res, stage, EV(P->cc), NULL, NULL, bias);
            st_endif(G);
            res = EV(P->res); break;
        }
        emit_sample(P, P->res, stage, EV(P->cc), EV(P->ddx), EV(P->ddy), NULL);
        res = EV(P->res);
        break; }
    // ---- ps 1.x texture addressing ----
    case OP_TEXCOORD:
        if (P->minor < 4) res = V4F("d3_texcoord", ex_elem(G, G->vy, I->dst.idx < SG_VARYINGS ? I->dst.idx : 0));
        else res = V4F("d3_texcoord4", a);
        break;
    case OP_TEXBEM: case OP_TEXBEML: {
        int n = I->dst.idx;
        SgExpr *cv = ex_elem(G, G->vy, n < SG_VARYINGS ? n : 0);
        st_assign(G, EV(P->cc), V4F("d3_bemc", cv, ex_raw(G, TY_V4, "u.bump_mat[%d]", n & 15), a));
        G->uses_deriv = 1;
        emit_sample(P, P->res, n, EV(P->cc), ex_elem(G, G->dvx, n < SG_VARYINGS ? n : 0), ex_elem(G, G->dvy, n < SG_VARYINGS ? n : 0), NULL);
        res = EV(P->res);
        if (op == OP_TEXBEML)
            res = V4F("d3_lumscale", res, FF("sg_sat1", ex_bin(G, TY_F, "+", ex_bin(G, TY_F, "*", ex_comp(G, a, 2), ex_raw(G, TY_F, "u.bump_lscale[%d]", n & 15)), ex_raw(G, TY_F, "u.bump_loffset[%d]", n & 15))));
        break; }
    case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXREG2RGB: {
        int n = I->dst.idx;
        st_assign(G, EV(P->cc), op == OP_TEXREG2AR ? ex_call(G, TY_V4, "mk4", 4, ex_comp(G, a, 3), ex_comp(G, a, 0), ex_f(G, 0), ex_f(G, 1)) :
                                op == OP_TEXREG2GB ? ex_call(G, TY_V4, "mk4", 4, ex_comp(G, a, 1), ex_comp(G, a, 2), ex_f(G, 0), ex_f(G, 1)) :
                                                      ex_call(G, TY_V4, "mk4", 4, ex_comp(G, a, 0), ex_comp(G, a, 1), ex_comp(G, a, 2), ex_f(G, 1)));
        emit_sample(P, P->res, n, EV(P->cc), NULL, NULL, NULL);
        res = EV(P->res);
        break; }
    case OP_TEXM3x2PAD: case OP_TEXM3x3PAD:
        st_assign(G, EV(P->row[P->npad & 3]), FF("d3_dp3f", ex_elem(G, G->vy, I->dst.idx < SG_VARYINGS ? I->dst.idx : 0), a));
        P->npad++;
        nostore = 1; break;
    case OP_TEXM3x2TEX: case OP_TEXM3x3TEX: case OP_TEXM3x3: case OP_TEXM3x3SPEC: case OP_TEXM3x3VSPEC: {
        int n = I->dst.idx;
        SgVar *vv = gb_tmp(G, TY_F);
        st_assign(G, EV(vv), FF("d3_dp3f", ex_elem(G, G->vy, n < SG_VARYINGS ? n : 0), a));
        SgExpr *cc4;
        if (op == OP_TEXM3x2TEX) cc4 = ex_call(G, TY_V4, "mk4", 4, EV(P->row[0]), EV(vv), ex_f(G, 0), ex_f(G, 1));
        else cc4 = ex_call(G, TY_V4, "mk4", 4, EV(P->row[0]), EV(P->row[1]), EV(vv), ex_f(G, 1));
        P->npad = 0;
        st_assign(G, EV(P->cc), cc4);
        if (op == OP_TEXM3x3) { res = V4F("mk4", ex_comp(G, EV(P->cc), 0), ex_comp(G, EV(P->cc), 1), ex_comp(G, EV(P->cc), 2), ex_f(G, 1)); break; }
        if (op == OP_TEXM3x3SPEC || op == OP_TEXM3x3VSPEC) {
            SgExpr *e;
            if (op == OP_TEXM3x3SPEC) e = b;
            else e = ex_call(G, TY_V4, "mk4", 4, ex_comp(G, ex_elem(G, G->vy, (n - 2) & 7), 3), ex_comp(G, ex_elem(G, G->vy, (n - 1) & 7), 3), ex_comp(G, ex_elem(G, G->vy, n & 7), 3), ex_f(G, 0));
            st_assign(G, EV(P->cc), V4F("d3_refl", EV(P->cc), e));
        }
        emit_sample(P, P->res, n, EV(P->cc), NULL, NULL, NULL);
        res = EV(P->res);
        break; }
    case OP_TEXDP3TEX:
        st_assign(G, EV(P->cc), V4F("mk4", FF("d3_dp3f", ex_elem(G, G->vy, I->dst.idx < SG_VARYINGS ? I->dst.idx : 0), a), ex_f(G, 0), ex_f(G, 0), ex_f(G, 1)));
        emit_sample(P, P->res, I->dst.idx, EV(P->cc), NULL, NULL, NULL);
        res = EV(P->res);
        break;
    case OP_TEXDP3: res = V4F("sg_splat", FF("d3_dp3f", ex_elem(G, G->vy, I->dst.idx < SG_VARYINGS ? I->dst.idx : 0), a)); break;
    case OP_TEXM3x2DEPTH: {
        SgVar *vv = gb_tmp(G, TY_F);
        st_assign(G, EV(vv), FF("d3_dp3f", ex_elem(G, G->vy, I->dst.idx < SG_VARYINGS ? I->dst.idx : 0), a));
        st_assign(G, EV(P->depth_val), FF("d3_depthdiv", EV(P->row[0]), EV(vv)));
        P->depth_set = 1; P->npad = 0; nostore = 1; break; }
    case OP_TEXDEPTH: {
        SgExpr *p = reg_static(P, I->dst.file, I->dst.idx);
        st_assign(G, EV(P->depth_val), FF("d3_depthdiv", ex_comp(G, p, 0), ex_comp(G, p, 1)));
        P->depth_set = 1; nostore = 1; break; }
    case OP_BEM: {
        int n = I->dst.idx < 16 ? I->dst.idx : 0;
        res = V4F("d3_bem", a, b, ex_raw(G, TY_V4, "u.bump_mat[%d]", n));
        break; }
    default: nostore = 1; break;
    }
    if (nostore || !res) return;
    if (!(res->k == EK_VAR && res->var == P->res)) st_assign(G, EV(P->res), res);
    if (P->dfl) {
        if (!special_deriv) {
            if (op == OP_TEX || op == OP_TEXLDL || op == OP_TEXLDD) { dres[0] = dres[1] = zero4(P); }
            else { dres[0] = dres_expr(P, I, 0); dres[1] = dres_expr(P, I, 1); }
        }
        st_assign(G, EV(P->dres[0]), dres[0]);
        st_assign(G, EV(P->dres[1]), dres[1]);
    }
    store(P, I, P->dfl);
    (void)sh;
}

// ---- structured control flow --------------------------------------------------------------------------------------------
static int emit_range(Ps *P, int lo, int hi);

// end of the code region starting at `lo` (first top-level RET / LABEL, or the end); -1 on an unsupported RET
static int region_end(Ps *P, int lo) {
    const D3dShader *sh = P->sh;
    int depth = 0;
    for (int pc = lo; pc < sh->nins; pc++) {
        int op = sh->ins[pc].op;
        if (op == OP_IF || op == OP_IFC || op == OP_LOOP || op == OP_REP) depth++;
        else if (op == OP_ENDIF || op == OP_ENDLOOP || op == OP_ENDREP) depth--;
        else if (op == OP_RET) {
            if (depth > 0) { gb_fail(G, "ret inside nested control flow is not translatable"); return -1; }
            if (sh->ins[pc].pred) { gb_fail(G, "predicated ret is not translatable"); return -1; }
            return pc;
        } else if (op == OP_LABEL && depth == 0) return pc;
    }
    return sh->nins;
}

static int emit_call(Ps *P, const Instr *I) {
    int lbl = I->target;
    for (int i = 0; i < P->ncall; i++) if (P->call_stack[i] == lbl) { gb_fail(G, "recursive subroutine call"); return 0; }
    if (P->ncall >= 32) { gb_fail(G, "subroutine call nesting too deep"); return 0; }
    int end = region_end(P, lbl + 1);
    if (end < 0) return 0;
    P->call_stack[P->ncall++] = lbl;
    int ok = emit_range(P, lbl + 1, end);
    P->ncall--;
    return ok;
}

static int emit_range(Ps *P, int lo, int hi) {
    const D3dShader *sh = P->sh;
    int pc = lo;
    while (pc < hi && !G->failed) {
        const Instr *I = &sh->ins[pc];
        int op = I->op;
        if (classify(op) != CL_FLOW) { emit_instr(P, I, pc); pc++; continue; }
        switch (op) {
        case OP_IF: case OP_IFC: {
            SgExpr *cond;
            if (op == OP_IF) cond = cond_bool(P, &I->src[0]);
            else {
                st_assign(G, EV(P->va), src_expr(P, &I->src[0], 0));
                st_assign(G, EV(P->vb), src_expr(P, &I->src[1], 0));
                cond = cmp_expr(P, I->ctl, ex_comp(G, EV(P->va), 0), ex_comp(G, EV(P->vb), 0));
            }
            int t = I->target;
            st_comment(G, "if");
            st_if(G, cond);
            if (sh->ins[t].op == OP_ELSE) {
                int endif = sh->ins[t].target;
                if (!emit_range(P, pc + 1, t)) return 0;
                st_else(G);
                if (!emit_range(P, t + 1, endif)) return 0;
                st_endif(G);
                pc = endif + 1;
            } else {
                if (!emit_range(P, pc + 1, t)) return 0;
                st_endif(G);
                pc = t + 1;
            }
            break; }
        case OP_LOOP: case OP_REP: {
            int ci = I->src[op == OP_LOOP ? 1 : 0].idx;
            SgVar *saved = NULL;
            st_comment(G, op == OP_LOOP ? "loop aL, i%d" : "rep i%d", ci);
            if (op == OP_LOOP) {
                saved = gb_tmp(G, TY_I);
                st_assign(G, EV(saved), EV(P->aL));
                st_assign(G, EV(P->aL), iconst(P, ci, 1));
            }
            st_loop(G, IC(G, "sg_clampi", iconst(P, ci, 0), ex_i(G, 0), ex_i(G, 4096)));
            if (!emit_range(P, pc + 1, I->target)) return 0;
            if (op == OP_LOOP) st_assign(G, EV(P->aL), ex_bin(G, TY_I, "+", EV(P->aL), iconst(P, ci, 2)));
            st_endloop(G);
            if (op == OP_LOOP) st_assign(G, EV(P->aL), EV(saved));
            pc = I->target + 1;
            break; }
        case OP_BREAK: case OP_BREAKC: case OP_BREAKP: {
            SgExpr *go = ex_b(G, 1);
            if (op == OP_BREAKC) {
                st_assign(G, EV(P->va), src_expr(P, &I->src[0], 0));
                st_assign(G, EV(P->vb), src_expr(P, &I->src[1], 0));
                go = cmp_expr(P, I->ctl, ex_comp(G, EV(P->va), 0), ex_comp(G, EV(P->vb), 0));
            } else if (op == OP_BREAKP) go = cond_bool(P, &I->src[0]);
            if (I->pred) go = ex_bin(G, TY_B, "&&", go, pcond(P, I, 0));
            st_if(G, go); st_break(G); st_endif(G);
            pc++;
            break; }
        case OP_CALL: case OP_CALLNZ: {
            SgExpr *go = op == OP_CALL ? ex_b(G, 1) : cond_bool(P, &I->src[1]);
            if (I->pred) go = ex_bin(G, TY_B, "&&", go, pcond(P, I, 0));
            st_comment(G, "call l%d", I->src[0].idx);
            st_if(G, go);
            if (!emit_call(P, I)) return 0;
            st_endif(G);
            pc++;
            break; }
        case OP_RET: case OP_LABEL:
            gb_fail(G, "ret/label inside a code region");
            return 0;
        default:
            gb_fail(G, "unexpected control flow instruction %d", op);
            return 0;
        }
    }
    return !G->failed;
}

int fe_ps(Gb *g, const SgState *s) {
    Ps PS, *P = &PS;
    memset(&PS, 0, sizeof PS);
    P->g = g; P->s = s; P->p = s->program; P->sh = s->program->ps.shader;
    const D3dShader *sh = P->sh;
    if (!sh || !sh->pixel) { gb_fail(g, "program has no pixel shader"); return 0; }
    g->shader = sh;
    P->ps1x = sh->ps1x; P->minor = sh->minor; P->dfl = sh->deriv; P->ntemps = sh->ntemps > 32 ? 32 : sh->ntemps;
    if (sh->major > 3 || sh->major < 1) { gb_fail(g, "unsupported shader model"); return 0; }
    P->r = gb_var(g, TY_V4, "r", P->ntemps);
    P->tr = gb_var(g, TY_V4, "tr", 8);
    P->pr = gb_var(g, TY_I, "pr", 4);
    P->aL = gb_var(g, TY_I, "aL", 0);
    P->ix = gb_var(g, TY_I, "ix", 0);
    P->colv = gb_var(g, TY_V4, "colv", 4);
    P->depthv = gb_var(g, TY_V4, "depthv", 0);
    P->depth_val = gb_var(g, TY_F, "depth_val", 0);
    for (int i = 0; i < 4; i++) { char nm[16]; snprintf(nm, sizeof nm, "row%d", i); P->row[i] = gb_var(g, TY_F, nm, 0); }
    P->va = gb_var(g, TY_V4, "a", 0); P->vb = gb_var(g, TY_V4, "b", 0); P->vc = gb_var(g, TY_V4, "c", 0); P->res = gb_var(g, TY_V4, "res", 0);
    P->cc = gb_var(g, TY_V4, "cc", 0); P->ddx = gb_var(g, TY_V4, "ddx", 0); P->ddy = gb_var(g, TY_V4, "ddy", 0);
    P->da[0] = gb_var(g, TY_V4, "da0", 0); P->da[1] = gb_var(g, TY_V4, "da1", 0);
    P->db[0] = gb_var(g, TY_V4, "db0", 0); P->db[1] = gb_var(g, TY_V4, "db1", 0);
    P->dc[0] = gb_var(g, TY_V4, "dc0", 0); P->dc[1] = gb_var(g, TY_V4, "dc1", 0);
    P->dres[0] = gb_var(g, TY_V4, "dres0", 0); P->dres[1] = gb_var(g, TY_V4, "dres1", 0);
    if (P->dfl) { P->dr[0] = gb_var(g, TY_V4, "dr0", P->ntemps); P->dr[1] = gb_var(g, TY_V4, "dr1", P->ntemps); }
    memcpy(g->in_slot, sh->in_slot, sizeof g->in_slot);
    if (sh->ps1x) st_comment(g, "ps_1_%d: texture coordinate registers", sh->minor);
    if (sh->ps1x) {
        // t# registers start as the interpolated coordinates (t0..t7 = varyings 0..7)
        for (int k = 0; k < 8; k++) st_assign(g, ex_elem(g, P->tr, k), ex_elem(g, g->vy, k));
    }
    int end = region_end(P, 0);
    if (end < 0) return 0;
    if (!emit_range(P, 0, end)) return 0;
    SgExpr *o = P->ps1x ? ex_elem(g, P->r, 0) : ex_elem(g, P->colv, 0);
    SgExpr *col = ex_var(g, g->ocol);
    st_assign(g, col, V4C(g, "sg_sat4", o));
    if (sh->writes_depth) {
        g->writes_depth = 1;
        SgExpr *dz = P->depth_set ? FC(g, "sg_sat1", EV(P->depth_val)) : FC(g, "sg_sat1", ex_comp(g, EV(P->depthv), 0));
        if (s->depth_clamp) dz = FC(g, "sg_clampf", dz, FC(g, "sg_minf", ex_raw(g, TY_F, "u.depth_near"), ex_raw(g, TY_F, "u.depth_far")), FC(g, "sg_maxf", ex_raw(g, TY_F, "u.depth_near"), ex_raw(g, TY_F, "u.depth_far")));
        st_assign(g, ex_var(g, g->odepth), dz);
    }
    fe_d3d_fog(g, s->program, col);
    return !g->failed;
}
