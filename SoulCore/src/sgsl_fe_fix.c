// SoulCore - sgsl front ends for the fixed-function pixel stages: the OpenGL texture environment / fog path of
// sgpu.c (fragment()) and the Direct3D 9 texture stage cascade of d3d9_draw.c (ff_pixel()), plus the steps every
// program shares (fog factor, alpha test, colour clamp).
#include "sgsl_priv.h"
#include <string.h>

static SgExpr *vyk(Gb *g, int k) { return ex_elem(g, g->vy, k); }
static SgExpr *uni(Gb *g, SgTy ty, const char *fmt, int idx) { return ex_raw(g, ty, fmt, idx); }

// ---- GL fixed function -------------------------------------------------------------------------------------------
// Varyings (see the vertex function in sgsl.c): v0 = colour, v1 = t0.xy, v2 = t1.xy, v3.x = fog factor.
int fe_gl(Gb *g, const SgState *s) {
    SgExpr *col = ex_var(g, g->ocol);
    st_assign(g, col, vyk(g, 0));
    for (int u = 0; u < SG_UNITS; u++) {
        if (!s->tex_on[u] || !s->tex[u] || !s->tex[u]->rgba) continue;
        const SgTexture *t = s->tex[u];
        st_comment(g, "GL texture unit %d: env %d, format %d", u, s->tex_env[u], t->format);
        SgVar *T = gb_tmp(g, TY_V4);
        st_sample(g, ex_var(g, T), u, SG_TEX_2D, SMP_HW, vyk(g, 1 + u), NULL, NULL, NULL);
        st_assign(g, col, V4C(g, "gl_combine", ex_i(g, s->tex_env[u]), ex_i(g, t->format), col, ex_var(g, T), uni(g, TY_V4, "u.env_color[%d]", u)));
    }
    if (s->fog) {
        st_comment(g, "GL fog (vertex fog factor)");
        SgVar *f = gb_tmp(g, TY_F);
        st_assign(g, ex_var(g, f), FC(g, "gl_clamp01", ex_comp(g, vyk(g, 3), 0)));
        st_assign(g, col, V4C(g, "sg_fogmix", col, ex_var(g, f), ex_raw(g, TY_V4, "u.fog_color")));
    }
    return 1;
}

// ---- fog factor of D3D pixel fog -----------------------------------------------------------------------------------
// mode 0: vertex fog factor sat(v[10].x); 1 EXP, 2 EXP2, 3 LINEAR from depth = window z.
void fe_d3d_fog(Gb *g, const SgProgram *p, SgExpr *rgba) {
    if (!p->fog.enable) return;
    SgVar *f = gb_tmp(g, TY_F);
    if (p->fog.table)
        st_assign(g, ex_var(g, f), FC(g, "sg_fogf", ex_i(g, p->fog.table), ex_comp(g, ex_var(g, g->pos), 2), ex_raw(g, TY_F, "u.fog_start"), ex_raw(g, TY_F, "u.fog_end"), ex_raw(g, TY_F, "u.fog_density")));
    else
        st_assign(g, ex_var(g, f), FC(g, "sg_sat1", ex_comp(g, vyk(g, 10), 0)));
    st_assign(g, rgba, V4C(g, "sg_fogmix", rgba, ex_var(g, f), ex_raw(g, TY_V4, "u.fog_color")));
}

// ---- D3D9 texture stage cascade --------------------------------------------------------------------------------------
static int arg_tex(int a) { return (a & 15) == 2; }

int fe_tss(Gb *g, const SgState *s) {
    const SgProgram *p = s->program;
    const SgpTss *t = &p->tss;
    SgVar *cur = gb_var(g, TY_V4, "cur", 0), *tmp = gb_var(g, TY_V4, "tmpr", 0), *T = gb_var(g, TY_V4, "T", 0);
    SgVar *pert = gb_var(g, TY_V4, "pert", 0), *lum = gb_var(g, TY_F, "lum", 0);
    SgVar *a0 = gb_var(g, TY_V4, "a0", 0), *a1 = gb_var(g, TY_V4, "a1", 0), *a2 = gb_var(g, TY_V4, "a2", 0);
    SgVar *b0 = gb_var(g, TY_V4, "b0", 0), *b1 = gb_var(g, TY_V4, "b1", 0), *b2 = gb_var(g, TY_V4, "b2", 0);
    SgVar *rc = gb_var(g, TY_V4, "rc", 0), *cc = gb_var(g, TY_V4, "cc", 0), *cdx = gb_var(g, TY_V4, "cdx", 0), *cdy = gb_var(g, TY_V4, "cdy", 0);
    SgExpr *E_cur = ex_var(g, cur), *E_tmp = ex_var(g, tmp), *E_T = ex_var(g, T), *E_pert = ex_var(g, pert), *E_lum = ex_var(g, lum);
    SgExpr *E_dif = vyk(g, SGP_VARY_DIFFUSE), *E_spc = vyk(g, SGP_VARY_SPECULAR), *E_tf = ex_raw(g, TY_V4, "u.tfactor");
    st_assign(g, E_cur, vyk(g, SGP_VARY_DIFFUSE));
    int have_pert = 0, have_lum = 0;
    for (int si = 0; si < t->nstages && si < SGP_MAX_STAGES; si++) {
        const SgpTssStage *S = &t->stage[si];
        const SgTexture *tex = p->samplers[si].tex;
        int need_tex = arg_tex(S->color_arg[0]) || arg_tex(S->color_arg[1]) || arg_tex(S->color_arg[2]) ||
                       arg_tex(S->alpha_arg[0]) || arg_tex(S->alpha_arg[1]) || arg_tex(S->alpha_arg[2]) ||
                       S->colorop == 13 || S->colorop == 15 || S->alphaop == 13 || S->alphaop == 15 || S->colorop == 22 || S->colorop == 23;
        st_comment(g, "stage %d: colorop %d alphaop %d", si, S->colorop, S->alphaop);
        st_assign(g, E_T, ex_v4(g, 0, 0, 0, 1));
        if (need_tex) {
            if (!tex) { st_comment(g, "no texture bound: the cascade stops here"); break; }
            int usable = tex->rgba && tex->w > 0 && tex->h > 0;
            int cube = tex->kind == SG_TEX_CUBE;
            if (usable) {
                if (S->slot < 0 || S->slot >= SG_VARYINGS) { gb_fail(g, "stage %d: bad coordinate slot", si); return 0; }
                SgExpr *v = vyk(g, S->slot), *vx = ex_elem(g, g->dvx, S->slot), *vy = ex_elem(g, g->dvy, S->slot);
                g->uses_deriv = 1;
                if (S->projected && S->ncoord >= 2 && !cube) {
                    SgVar *iq = gb_tmp(g, TY_F);
                    int q = S->ncoord - 1;
                    st_assign(g, ex_var(g, iq), FC(g, "ff_rcpz", ex_comp(g, v, q)));
                    st_assign(g, ex_var(g, cc), V4C(g, "sg_scale4", v, ex_var(g, iq)));
                    st_assign(g, ex_var(g, cdx), V4C(g, "ff_pd", vx, ex_var(g, cc), ex_comp(g, vx, q), ex_var(g, iq)));
                    st_assign(g, ex_var(g, cdy), V4C(g, "ff_pd", vy, ex_var(g, cc), ex_comp(g, vy, q), ex_var(g, iq)));
                } else {
                    st_assign(g, ex_var(g, cc), v); st_assign(g, ex_var(g, cdx), vx); st_assign(g, ex_var(g, cdy), vy);
                }
                if (have_pert) st_assign(g, ex_var(g, cc), V4C(g, "ff_addxy", ex_var(g, cc), E_pert));
                st_sample(g, E_T, si, tex->kind, SMP_GRAD, ex_var(g, cc), ex_var(g, cdx), ex_var(g, cdy), NULL);
            } else {
                st_comment(g, "texture without storage samples (0,0,0,1)");
            }
        }
        if (have_lum) st_assign(g, E_T, V4C(g, "ff_lum", E_T, E_lum));
        have_pert = have_lum = 0;
        if (S->colorop == 22 || S->colorop == 23) {
            st_assign(g, E_pert, V4C(g, "ff_bump", E_T, uni(g, TY_V4, "u.bump_mat[%d]", si)));
            have_pert = 1;
            if (S->colorop == 23) {
                st_assign(g, E_lum, FC(g, "sg_sat1", ex_bin(g, TY_F, "+", ex_bin(g, TY_F, "*", ex_comp(g, E_T, 2), uni(g, TY_F, "u.bump_lscale[%d]", si)), uni(g, TY_F, "u.bump_loffset[%d]", si))));
                have_lum = 1;
            }
            continue;
        }
        SgExpr *cst = uni(g, TY_V4, "u.tss_constant[%d]", si);
        SgExpr *res = S->result_arg == 5 ? E_tmp : E_cur;
        st_assign(g, ex_var(g, rc), res);
        for (int k = 0; k < 3; k++) {
            SgVar *av = k == 0 ? a0 : k == 1 ? a1 : a2;
            st_assign(g, ex_var(g, av), V4C(g, "ff_arg", ex_i(g, S->color_arg[k]), E_T, E_cur, E_tmp, E_dif, E_spc, E_tf, cst));
        }
        if (S->colorop == 24) {
            st_assign(g, ex_var(g, rc), V4C(g, "ff_dot3", ex_var(g, a1), ex_var(g, a2)));
        } else {
            st_assign(g, ex_var(g, rc), V4C(g, "ff_setrgb", ex_var(g, rc), V4C(g, "ff_opc", ex_i(g, S->colorop), ex_var(g, a0), ex_var(g, a1), ex_var(g, a2), E_T, E_cur, E_dif, E_tf)));
            if (S->alphaop == 1) st_assign(g, ex_comp(g, ex_var(g, rc), 3), ex_comp(g, E_cur, 3));
            else {
                for (int k = 0; k < 3; k++) {
                    SgVar *bv = k == 0 ? b0 : k == 1 ? b1 : b2;
                    st_assign(g, ex_var(g, bv), V4C(g, "ff_arga", ex_i(g, S->alpha_arg[k]), E_T, E_cur, E_tmp, E_dif, E_spc, E_tf, cst));
                }
                st_assign(g, ex_comp(g, ex_var(g, rc), 3), FC(g, "ff_opa", ex_i(g, S->alphaop), ex_var(g, b0), ex_var(g, b1), ex_var(g, b2), E_T, E_cur, E_dif, E_tf));
            }
        }
        st_assign(g, S->result_arg == 5 ? E_tmp : E_cur, ex_var(g, rc));
    }
    SgExpr *out = ex_var(g, g->ocol);
    st_assign(g, out, E_cur);
    if (t->specular) st_assign(g, out, V4C(g, "ff_spec", out, E_spc));
    fe_d3d_fog(g, p, out);
    return 1;
}

// ---- common tail: alpha test, clamp --------------------------------------------------------------------------------------
void fe_finish(Gb *g, const SgState *s, int clamp_first) {
    SgExpr *col = ex_var(g, g->ocol);
    if (clamp_first) st_assign(g, col, V4C(g, "sg_sat4", col));
    if (s->alpha_test) {
        st_comment(g, "alpha test (func %d)", s->alpha_func);
        SgExpr *a = ex_comp(g, col, 3), *r = ex_raw(g, TY_F, "u.alpha_ref");
        const char *op = NULL;
        switch (s->alpha_func) {
        case SG_NEVER: st_kill(g); break;
        case SG_LESS: op = "<"; break;
        case SG_EQUAL: op = "=="; break;
        case SG_LEQUAL: op = "<="; break;
        case SG_GREATER: op = ">"; break;
        case SG_NOTEQUAL: op = "!="; break;
        case SG_GEQUAL: op = ">="; break;
        default: break;   // ALWAYS
        }
        if (op) { st_if(g, ex_un(g, TY_B, "!", ex_bin(g, TY_B, op, a, r))); st_kill(g); st_endif(g); }
    }
    st_assign(g, col, V4C(g, "sg_sat4", col));
}
