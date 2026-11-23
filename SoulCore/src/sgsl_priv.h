// SoulCore - sgsl private header: the small typed IR shared by the front ends (GL fixed function, D3D9 texture
// stages, D3D9 pixel shaders) and the two printers (MSL, C). See sgsl.h for the public interface.
//
// The IR is deliberately simple: expression trees over four types (V4 = float4, F = float, I = int, B = bool),
// function-level variables, and structured statements (assign, if/else, counted loop, break, kill, texture sample).
// All arithmetic is expressed as calls of helper functions from the shared prelude (sgsl_prelude.c), which is the
// same text for MSL and C; only structure printing (types, sampling, discard, entry points) differs per language.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "sgsl.h"

typedef enum { TY_V4, TY_F, TY_I, TY_B } SgTy;

typedef struct SgVar {
    char name[24];
    SgTy ty;
    int n;                       // 0 = scalar, >0 = array of n
    struct SgVar *next;
} SgVar;

typedef enum { EK_F, EK_I, EK_B, EK_V4, EK_VAR, EK_ELEM, EK_INDEX, EK_COMP, EK_SWZ, EK_CALL, EK_RAW, EK_BIN, EK_UN, EK_TERN } SgEK;

typedef struct SgExpr {
    SgEK k;
    SgTy ty;
    const char *s;               // function / operator / raw text / variable name
    float f[4];
    int i;                       // int constant, element index, component, packed swizzle
    int na;
    struct SgExpr *a[8];
    const SgVar *var;
} SgExpr;

typedef enum { ST_ASSIGN, ST_IF, ST_LOOP, ST_BREAK, ST_KILL, ST_COMMENT, ST_SAMPLE } SgSK;
// sample forms: GRAD explicit gradients, LEVEL explicit level of detail, BASE zero gradients (base level, magnification
// filter), HW hardware derivatives (GL fixed function: the C rendering uses zero gradients)
enum { SMP_GRAD = 1, SMP_LEVEL = 2, SMP_BASE = 3, SMP_HW = 4 };

typedef struct SgStmt {
    SgSK k;
    SgExpr *lhs, *rhs;           // ASSIGN: lhs = rhs; IF: rhs = condition; LOOP: rhs = iteration count (I), s = counter name
    const char *s;
    struct SgStmt *a, *b;        // blocks (IF: then / else, LOOP: body)
    struct SgStmt *next;
    // SAMPLE: lhs = destination (V4), rhs = coordinate (V4); ddx/ddy gradients (SMP_GRAD), lod (SMP_LEVEL, F)
    int slot, tex_kind, form;
    SgExpr *ddx, *ddy, *lod;
} SgStmt;

typedef struct SgBlk { SgStmt *head, *tail; } SgBlk;

typedef struct SgChunk { struct SgChunk *next; size_t used, cap; } SgChunk;

typedef struct SgBuf { char *p; size_t n, cap; } SgBuf;

typedef struct Gb {
    SgChunk *chunks;
    SgVar *vars, *vars_tail;
    SgBlk root;
    SgBlk *cur;
    SgBlk *stack[32];            // enclosing blocks
    SgStmt *open[32];            // the IF / LOOP statements being filled
    int depth;
    int nsym;
    int failed;
    char err[256];
    // fragment properties collected while building
    uint32_t texture_mask;
    int texture_kind[16];
    int uses_deriv, uses_front, uses_pos, uses_pcoord, uses_discard;
    int writes_depth;
    int max_const;               // highest constant register read + 1 (256 when relative addressing is used)
    int rel_const;
    int tmp_n;
    int in_slot_table;           // the table sg_in_slot[] is needed
    int8_t in_slot[16];
    int need_defc_fn;            // generated function sg_cfetch() (shader-declared constants by relative index)
    const struct D3dShader *shader;
    SgVar *vy, *dvx, *dvy, *pos, *front, *ocol, *odepth;
} Gb;

// ---- builder -----------------------------------------------------------------------------------------------------
void gb_init(Gb *g);
void gb_free(Gb *g);
void gb_fail(Gb *g, const char *fmt, ...);
SgVar *gb_var(Gb *g, SgTy ty, const char *name, int n);      // function-level variable, zero initialised
SgVar *gb_tmp(Gb *g, SgTy ty);                               // fresh scalar temp
SgExpr *ex_f(Gb *g, float v);
SgExpr *ex_i(Gb *g, int v);
SgExpr *ex_b(Gb *g, int v);
SgExpr *ex_v4(Gb *g, float x, float y, float z, float w);
SgExpr *ex_var(Gb *g, const SgVar *v);
SgExpr *ex_elem(Gb *g, const SgVar *v, int idx);             // v[idx], idx constant
SgExpr *ex_index(Gb *g, const SgVar *v, SgExpr *idx);        // v[idx] (idx runtime int, no bounds check)
SgExpr *ex_comp(Gb *g, SgExpr *a, int c);                    // a.x / .y / .z / .w (a is V4) -> F
SgExpr *ex_swz(Gb *g, SgExpr *a, unsigned packed);           // 2 bits per component, comp i = (packed >> 2i) & 3
SgExpr *ex_call(Gb *g, SgTy ty, const char *fn, int n, ...); // n SgExpr* arguments
SgExpr *ex_raw(Gb *g, SgTy ty, const char *fmt, ...);
SgExpr *ex_bin(Gb *g, SgTy ty, const char *op, SgExpr *a, SgExpr *b);
SgExpr *ex_un(Gb *g, SgTy ty, const char *op, SgExpr *a);
SgExpr *ex_tern(Gb *g, SgExpr *c, SgExpr *a, SgExpr *b);
#define V4C(g, fn, ...) ex_call(g, TY_V4, fn, (int)(sizeof((SgExpr *[]){ __VA_ARGS__ }) / sizeof(SgExpr *)), __VA_ARGS__)
#define FC(g, fn, ...) ex_call(g, TY_F, fn, (int)(sizeof((SgExpr *[]){ __VA_ARGS__ }) / sizeof(SgExpr *)), __VA_ARGS__)
#define BC(g, fn, ...) ex_call(g, TY_B, fn, (int)(sizeof((SgExpr *[]){ __VA_ARGS__ }) / sizeof(SgExpr *)), __VA_ARGS__)
#define IC(g, fn, ...) ex_call(g, TY_I, fn, (int)(sizeof((SgExpr *[]){ __VA_ARGS__ }) / sizeof(SgExpr *)), __VA_ARGS__)

void st_assign(Gb *g, SgExpr *lhs, SgExpr *rhs);
void st_comment(Gb *g, const char *fmt, ...);
void st_break(Gb *g);
void st_kill(Gb *g);
void st_if(Gb *g, SgExpr *cond);                             // opens the then block
void st_else(Gb *g);
void st_endif(Gb *g);
void st_loop(Gb *g, SgExpr *count);                          // opens the body of "repeat count times" (count: I)
void st_endloop(Gb *g);
// texture sample: dst = sample(slot, coord, ...). The caller handles unbound textures (constant) itself.
void st_sample(Gb *g, SgExpr *dst, int slot, int kind, int form, SgExpr *coord, SgExpr *ddx, SgExpr *ddy, SgExpr *lod);

// ---- printing ------------------------------------------------------------------------------------------------------
void sb_put(SgBuf *b, const char *fmt, ...);
void sb_putn(SgBuf *b, const char *s, size_t n);
void sg_print_decls(const Gb *g, SgBuf *b, int lang, const char *indent);   // function-level variable declarations
void sg_print_body(const Gb *g, SgBuf *b, int lang, int indent);
const char *sgsl_prelude_text(void);   // shared helper functions (valid in MSL and C once the language shim is in place)
const char *sgsl_shim_text(int lang);

// ---- front ends ----------------------------------------------------------------------------------------------------
// Conventions of the fragment function that every front end builds on (declared by the assembler in sgsl.c):
//   V4 vy[12]    varyings v0 .. v11 (zero beyond nvary), V4 dvx[12], dvy[12] screen-space derivatives of vy
//   V4 pos       window position (x, y, z, 1/w)            I front (1 = front face)
//   V4 ocol      result colour,   F odepth  result depth (programs that write depth)
// GL fixed function inputs: v0 = colour, v1 = (t0.s, t0.t), v2 = (t1.s, t1.t), v3.x = fog factor.
int fe_gl(Gb *g, const SgState *s);
int fe_tss(Gb *g, const SgState *s);
int fe_ps(Gb *g, const SgState *s);
void fe_d3d_fog(Gb *g, const SgProgram *p, SgExpr *rgba);
void fe_finish(Gb *g, const SgState *s, int clamp_first);   // alpha test + final clamp
