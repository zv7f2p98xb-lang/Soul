// SoulCore - sgsl IR builder and the shared MSL / C statement printer (see sgsl_priv.h).
#include "sgsl_priv.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- arena ---------------------------------------------------------------------------------------------------------
static void *gb_alloc(Gb *g, size_t n) {
    n = (n + 15) & ~(size_t)15;
    SgChunk *c = g->chunks;
    if (!c || c->used + n > c->cap) {
        size_t cap = n + sizeof(SgChunk) > 65536 ? n + sizeof(SgChunk) : 65536;
        c = (SgChunk *)calloc(1, cap);
        if (!c) { g->failed = 1; snprintf(g->err, sizeof g->err, "out of memory"); static char dummy[1024]; return dummy; }
        c->cap = cap; c->used = (sizeof(SgChunk) + 15) & ~(size_t)15;
        c->next = g->chunks; g->chunks = c;
    }
    void *p = (char *)c + c->used;
    c->used += n;
    return p;
}

void gb_init(Gb *g) {
    memset(g, 0, sizeof *g);
    g->cur = &g->root;
    memset(g->in_slot, -1, sizeof g->in_slot);
    for (int i = 0; i < 16; i++) g->texture_kind[i] = -1;
}
void gb_free(Gb *g) {
    SgChunk *c = g->chunks;
    while (c) { SgChunk *n = c->next; free(c); c = n; }
    g->chunks = NULL;
}
void gb_fail(Gb *g, const char *fmt, ...) {
    if (g->failed) return;
    g->failed = 1;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->err, sizeof g->err, fmt, ap);
    va_end(ap);
}
static char *gb_strdup(Gb *g, const char *s) { size_t n = strlen(s) + 1; char *p = (char *)gb_alloc(g, n); memcpy(p, s, n); return p; }

SgVar *gb_var(Gb *g, SgTy ty, const char *name, int n) {
    SgVar *v = (SgVar *)gb_alloc(g, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->ty = ty; v->n = n;
    if (g->vars_tail) g->vars_tail->next = v; else g->vars = v;
    g->vars_tail = v;
    return v;
}
SgVar *gb_tmp(Gb *g, SgTy ty) {
    char nm[24];
    snprintf(nm, sizeof nm, "t%d", g->tmp_n++);
    return gb_var(g, ty, nm, 0);
}

// ---- expressions -----------------------------------------------------------------------------------------------------
static SgExpr *ex_new(Gb *g, SgEK k, SgTy ty) { SgExpr *e = (SgExpr *)gb_alloc(g, sizeof *e); e->k = k; e->ty = ty; return e; }
SgExpr *ex_f(Gb *g, float v) { SgExpr *e = ex_new(g, EK_F, TY_F); e->f[0] = v; return e; }
SgExpr *ex_i(Gb *g, int v) { SgExpr *e = ex_new(g, EK_I, TY_I); e->i = v; return e; }
SgExpr *ex_b(Gb *g, int v) { SgExpr *e = ex_new(g, EK_B, TY_B); e->i = v != 0; return e; }
SgExpr *ex_v4(Gb *g, float x, float y, float z, float w) { SgExpr *e = ex_new(g, EK_V4, TY_V4); e->f[0] = x; e->f[1] = y; e->f[2] = z; e->f[3] = w; return e; }
SgExpr *ex_var(Gb *g, const SgVar *v) { SgExpr *e = ex_new(g, EK_VAR, v->ty); e->var = v; e->s = v->name; return e; }
SgExpr *ex_elem(Gb *g, const SgVar *v, int idx) { SgExpr *e = ex_new(g, EK_ELEM, v->ty); e->var = v; e->s = v->name; e->i = idx; return e; }
SgExpr *ex_index(Gb *g, const SgVar *v, SgExpr *idx) { SgExpr *e = ex_new(g, EK_INDEX, v->ty); e->var = v; e->s = v->name; e->a[0] = idx; e->na = 1; return e; }
SgExpr *ex_comp(Gb *g, SgExpr *a, int c) { SgExpr *e = ex_new(g, EK_COMP, TY_F); e->a[0] = a; e->na = 1; e->i = c; return e; }
SgExpr *ex_swz(Gb *g, SgExpr *a, unsigned packed) {
    if ((packed & 255) == 0xE4) return a;
    SgExpr *e = ex_new(g, EK_SWZ, TY_V4); e->a[0] = a; e->na = 1; e->i = (int)(packed & 255); return e;
}
SgExpr *ex_call(Gb *g, SgTy ty, const char *fn, int n, ...) {
    SgExpr *e = ex_new(g, EK_CALL, ty);
    e->s = fn; e->na = n;
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n && i < 8; i++) e->a[i] = va_arg(ap, SgExpr *);
    va_end(ap);
    return e;
}
SgExpr *ex_raw(Gb *g, SgTy ty, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SgExpr *e = ex_new(g, EK_RAW, ty); e->s = gb_strdup(g, buf); return e;
}
SgExpr *ex_bin(Gb *g, SgTy ty, const char *op, SgExpr *a, SgExpr *b) { SgExpr *e = ex_new(g, EK_BIN, ty); e->s = op; e->a[0] = a; e->a[1] = b; e->na = 2; return e; }
SgExpr *ex_un(Gb *g, SgTy ty, const char *op, SgExpr *a) { SgExpr *e = ex_new(g, EK_UN, ty); e->s = op; e->a[0] = a; e->na = 1; return e; }
SgExpr *ex_tern(Gb *g, SgExpr *c, SgExpr *a, SgExpr *b) { SgExpr *e = ex_new(g, EK_TERN, a->ty); e->a[0] = c; e->a[1] = a; e->a[2] = b; e->na = 3; return e; }

// ---- statements -------------------------------------------------------------------------------------------------------
static SgStmt *st_new(Gb *g, SgSK k) {
    SgStmt *s = (SgStmt *)gb_alloc(g, sizeof *s);
    s->k = k;
    SgBlk *b = g->cur;
    if (b->tail) b->tail->next = s; else b->head = s;
    b->tail = s;
    return s;
}
void st_assign(Gb *g, SgExpr *lhs, SgExpr *rhs) { SgStmt *s = st_new(g, ST_ASSIGN); s->lhs = lhs; s->rhs = rhs; }
void st_comment(Gb *g, const char *fmt, ...) {
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SgStmt *s = st_new(g, ST_COMMENT); s->s = gb_strdup(g, buf);
}
void st_break(Gb *g) { st_new(g, ST_BREAK); }
void st_kill(Gb *g) { st_new(g, ST_KILL); g->uses_discard = 1; }
static void open_block(Gb *g, SgStmt *s, SgBlk **which_out) {
    if (g->depth >= 31) { gb_fail(g, "control flow nesting too deep"); return; }
    SgBlk *nb = (SgBlk *)gb_alloc(g, sizeof *nb);
    g->stack[g->depth] = g->cur; g->open[g->depth] = s; g->depth++;
    g->cur = nb;
    *which_out = nb;
}
void st_if(Gb *g, SgExpr *cond) {
    SgStmt *s = st_new(g, ST_IF); s->rhs = cond;
    SgBlk *nb; open_block(g, s, &nb);
}
void st_else(Gb *g) {
    if (g->depth <= 0) { gb_fail(g, "else without if"); return; }
    SgStmt *s = g->open[g->depth - 1];
    s->a = g->cur->head;
    SgBlk *nb = (SgBlk *)gb_alloc(g, sizeof *nb);
    g->cur = nb;
    s->lhs = (SgExpr *)1;   // marks "then part stored, now filling else"
}
void st_endif(Gb *g) {
    if (g->depth <= 0) { gb_fail(g, "endif without if"); return; }
    SgStmt *s = g->open[g->depth - 1];
    if (s->lhs == (SgExpr *)1) { s->b = g->cur->head; s->lhs = NULL; } else s->a = g->cur->head;
    g->depth--;
    g->cur = g->stack[g->depth];
}
void st_loop(Gb *g, SgExpr *count) {
    SgStmt *s = st_new(g, ST_LOOP); s->rhs = count;
    char nm[24];
    snprintf(nm, sizeof nm, "lc%d", g->nsym++);
    s->s = gb_strdup(g, nm);
    SgBlk *nb; open_block(g, s, &nb);
}
void st_endloop(Gb *g) {
    if (g->depth <= 0) { gb_fail(g, "endloop without loop"); return; }
    SgStmt *s = g->open[g->depth - 1];
    s->a = g->cur->head;
    g->depth--;
    g->cur = g->stack[g->depth];
}
void st_sample(Gb *g, SgExpr *dst, int slot, int kind, int form, SgExpr *coord, SgExpr *ddx, SgExpr *ddy, SgExpr *lod) {
    SgStmt *s = st_new(g, ST_SAMPLE);
    s->lhs = dst; s->rhs = coord; s->slot = slot; s->tex_kind = kind; s->form = form; s->ddx = ddx; s->ddy = ddy; s->lod = lod;
    if (slot >= 0 && slot < 16) { g->texture_mask |= 1u << slot; g->texture_kind[slot] = kind; }
}

// ---- text buffer ----------------------------------------------------------------------------------------------------
void sb_putn(SgBuf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + n + 1) nc *= 2;
        char *p = (char *)realloc(b->p, nc);
        if (!p) return;
        b->p = p; b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
void sb_put(SgBuf *b, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof buf) { sb_putn(b, buf, (size_t)n); return; }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb_putn(b, big, (size_t)n);
    free(big);
}

// ---- printing -------------------------------------------------------------------------------------------------------
static void put_float(SgBuf *b, float v) {
    if (v != v) { sb_put(b, "NAN"); return; }
    if (isinf(v)) { sb_put(b, v < 0 ? "(-INFINITY)" : "INFINITY"); return; }
    char buf[40];
    snprintf(buf, sizeof buf, "%.9g", (double)v);
    int plain = 1;
    for (char *p = buf; *p; p++) if (*p == '.' || *p == 'e' || *p == 'E') plain = 0;
    if (v < 0) sb_put(b, "(%s%sf)", buf, plain ? ".0" : ""); else sb_put(b, "%s%sf", buf, plain ? ".0" : "");
}

static void put_expr(SgBuf *b, const SgExpr *e, int lang) {
    switch (e->k) {
    case EK_F: put_float(b, e->f[0]); break;
    case EK_I: if (e->i < 0) sb_put(b, "(%d)", e->i); else sb_put(b, "%d", e->i); break;
    case EK_B: sb_put(b, e->i ? "true" : "false"); break;
    case EK_V4:
        sb_put(b, "mk4(");
        for (int i = 0; i < 4; i++) { if (i) sb_put(b, ", "); put_float(b, e->f[i]); }
        sb_put(b, ")");
        break;
    case EK_VAR: sb_put(b, "%s", e->s); break;
    case EK_ELEM: sb_put(b, "%s[%d]", e->s, e->i); break;
    case EK_INDEX: sb_put(b, "%s[", e->s); put_expr(b, e->a[0], lang); sb_put(b, "]"); break;
    case EK_COMP: sb_put(b, "("); put_expr(b, e->a[0], lang); sb_put(b, ").%c", "xyzw"[e->i & 3]); break;
    case EK_SWZ:
        if (lang == SGSL_MSL) {
            sb_put(b, "("); put_expr(b, e->a[0], lang);
            sb_put(b, ").%c%c%c%c", "xyzw"[e->i & 3], "xyzw"[(e->i >> 2) & 3], "xyzw"[(e->i >> 4) & 3], "xyzw"[(e->i >> 6) & 3]);
        } else {
            sb_put(b, "sg_swz("); put_expr(b, e->a[0], lang);
            sb_put(b, ", %d, %d, %d, %d)", e->i & 3, (e->i >> 2) & 3, (e->i >> 4) & 3, (e->i >> 6) & 3);
        }
        break;
    case EK_CALL:
        sb_put(b, "%s(", e->s);
        for (int i = 0; i < e->na; i++) { if (i) sb_put(b, ", "); put_expr(b, e->a[i], lang); }
        sb_put(b, ")");
        break;
    case EK_RAW: sb_put(b, "%s", e->s); break;
    case EK_BIN: sb_put(b, "("); put_expr(b, e->a[0], lang); sb_put(b, " %s ", e->s); put_expr(b, e->a[1], lang); sb_put(b, ")"); break;
    case EK_UN: sb_put(b, "(%s", e->s); put_expr(b, e->a[0], lang); sb_put(b, ")"); break;
    case EK_TERN: sb_put(b, "("); put_expr(b, e->a[0], lang); sb_put(b, " ? "); put_expr(b, e->a[1], lang); sb_put(b, " : "); put_expr(b, e->a[2], lang); sb_put(b, ")"); break;
    }
}

static void pad(SgBuf *b, int n) { for (int i = 0; i < n; i++) sb_put(b, "  "); }

static void put_sample(SgBuf *b, const SgStmt *s, int lang) {
    int slot = s->slot;
    put_expr(b, s->lhs, lang);
    if (lang == SGSL_C) {
        sb_put(b, " = ");
        switch (s->form) {
        case SMP_GRAD: sb_put(b, "sgc_smp(env_, %d, ", slot); put_expr(b, s->rhs, lang); sb_put(b, ", "); put_expr(b, s->ddx, lang); sb_put(b, ", "); put_expr(b, s->ddy, lang); sb_put(b, ")"); break;
        case SMP_LEVEL: sb_put(b, "sgc_smp0(env_, %d, ", slot); put_expr(b, s->rhs, lang); sb_put(b, ", "); put_expr(b, s->lod, lang); sb_put(b, ")"); break;
        default: sb_put(b, "sgc_smp0(env_, %d, ", slot); put_expr(b, s->rhs, lang); sb_put(b, ", NAN)"); break;   // BASE and HW
        }
        sb_put(b, ";\n");
        return;
    }
    const char *dim = s->tex_kind == SG_TEX_2D ? "xy" : "xyz";
    const char *grad = s->tex_kind == SG_TEX_2D ? "gradient2d" : s->tex_kind == SG_TEX_CUBE ? "gradientcube" : "gradient3d";
    sb_put(b, " = tex%d.sample(smp%d, (", slot, slot);
    put_expr(b, s->rhs, lang);
    sb_put(b, ").%s", dim);
    switch (s->form) {
    case SMP_GRAD:
        sb_put(b, ", %s(((", grad); put_expr(b, s->ddx, lang); sb_put(b, ").%s) * exp2(u.lod_bias[%d]), ((", dim, slot);
        put_expr(b, s->ddy, lang); sb_put(b, ").%s) * exp2(u.lod_bias[%d]))", dim, slot);
        break;
    case SMP_LEVEL:
        sb_put(b, ", level(("); put_expr(b, s->lod, lang); sb_put(b, ") + u.lod_bias[%d])", slot);
        break;
    case SMP_HW: break;   // implicit derivatives
    default:
        sb_put(b, ", level(u.lod_bias[%d] - 30.0f)", slot);
        break;
    }
    sb_put(b, ");\n");
}

static void put_block(SgBuf *b, const SgStmt *s, int lang, int ind) {
    for (; s; s = s->next) {
        switch (s->k) {
        case ST_COMMENT: pad(b, ind); sb_put(b, "// %s\n", s->s); break;
        case ST_ASSIGN: pad(b, ind); put_expr(b, s->lhs, lang); sb_put(b, " = "); put_expr(b, s->rhs, lang); sb_put(b, ";\n"); break;
        case ST_BREAK: pad(b, ind); sb_put(b, "break;\n"); break;
        case ST_KILL:
            pad(b, ind);
            if (lang == SGSL_MSL) sb_put(b, "{ discard_fragment(); return sg_o; }\n"); else sb_put(b, "return 0;\n");
            break;
        case ST_IF:
            pad(b, ind); sb_put(b, "if ("); put_expr(b, s->rhs, lang); sb_put(b, ") {\n");
            put_block(b, s->a, lang, ind + 1);
            if (s->b) { pad(b, ind); sb_put(b, "} else {\n"); put_block(b, s->b, lang, ind + 1); }
            pad(b, ind); sb_put(b, "}\n");
            break;
        case ST_LOOP:
            pad(b, ind); sb_put(b, "for (int %s = ", s->s); put_expr(b, s->rhs, lang); sb_put(b, "; %s > 0; %s--) {\n", s->s, s->s);
            put_block(b, s->a, lang, ind + 1);
            pad(b, ind); sb_put(b, "}\n");
            break;
        case ST_SAMPLE: pad(b, ind); put_sample(b, s, lang); break;
        }
    }
}

void sg_print_body(const Gb *g, SgBuf *b, int lang, int indent) { put_block(b, g->root.head, lang, indent); }

void sg_print_decls(const Gb *g, SgBuf *b, int lang, const char *indent) {
    (void)lang;
    for (const SgVar *v = g->vars; v; v = v->next) {
        const char *ty = v->ty == TY_V4 ? "float4" : v->ty == TY_F ? "float" : v->ty == TY_I ? "int" : "bool";
        const char *zero = v->ty == TY_V4 ? "mk4(0.0f, 0.0f, 0.0f, 0.0f)" : v->ty == TY_F ? "0.0f" : v->ty == TY_I ? "0" : "false";
        if (v->n) sb_put(b, "%s%s %s[%d];\n%sfor (int i_ = 0; i_ < %d; i_++) %s[i_] = %s;\n", indent, ty, v->name, v->n, indent, v->n, v->name, zero);
        else sb_put(b, "%s%s %s = %s;\n", indent, ty, v->name, zero);
    }
}
