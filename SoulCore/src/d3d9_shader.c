// SoulCore - Direct3D 9 shader engine: bytecode decoder (see d3d9_shader.h, d3d9_shader_priv.h).
// Pre-decodes SM 1.1 - 3.0 token streams into a compact Instr array; execution is in d3d9_shader_exec.c.
#include "d3d9_shader_priv.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int d3dsh_varying_slot(int usage, int usage_index) {
    switch (usage) {
    case D3DSU_TEXCOORD: return usage_index >= 0 && usage_index < 8 ? usage_index : (usage_index == 8 ? 11 : -1);
    case D3DSU_COLOR: return usage_index == 0 ? 8 : usage_index == 1 ? 9 : -1;
    case D3DSU_FOG: return usage_index == 0 ? 10 : -1;
    case D3DSU_NORMAL: case D3DSU_TANGENT: case D3DSU_BINORMAL: case D3DSU_BLENDWEIGHT: case D3DSU_BLENDINDICES:
    case D3DSU_TESSFACTOR: case D3DSU_DEPTH: case D3DSU_SAMPLE:
        return 11; // shared slot: only one such semantic per shader pair (the engine reports -1 for a second)
    default: return -1;
    }
}

// ---- opcode table -----------------------------------------------------------------------------------
typedef struct { uint8_t valid, hd, ns; } OpInfo;
static const OpInfo OPI[OP_MAX_CODE] = {
    [OP_NOP] = {1, 0, 0}, [OP_MOV] = {1, 1, 1}, [OP_ADD] = {1, 1, 2}, [OP_SUB] = {1, 1, 2}, [OP_MAD] = {1, 1, 3},
    [OP_MUL] = {1, 1, 2}, [OP_RCP] = {1, 1, 1}, [OP_RSQ] = {1, 1, 1}, [OP_DP3] = {1, 1, 2}, [OP_DP4] = {1, 1, 2},
    [OP_MIN] = {1, 1, 2}, [OP_MAX] = {1, 1, 2}, [OP_SLT] = {1, 1, 2}, [OP_SGE] = {1, 1, 2}, [OP_EXP] = {1, 1, 1},
    [OP_LOG] = {1, 1, 1}, [OP_LIT] = {1, 1, 1}, [OP_DST] = {1, 1, 2}, [OP_LRP] = {1, 1, 3}, [OP_FRC] = {1, 1, 1},
    [OP_M4x4] = {1, 1, 2}, [OP_M4x3] = {1, 1, 2}, [OP_M3x4] = {1, 1, 2}, [OP_M3x3] = {1, 1, 2}, [OP_M3x2] = {1, 1, 2},
    [OP_CALL] = {1, 0, 1}, [OP_CALLNZ] = {1, 0, 2}, [OP_LOOP] = {1, 0, 2}, [OP_RET] = {1, 0, 0},
    [OP_ENDLOOP] = {1, 0, 0}, [OP_LABEL] = {1, 0, 1}, [OP_DCL] = {1, 0, 0}, [OP_POW] = {1, 1, 2}, [OP_CRS] = {1, 1, 2},
    [OP_SGN] = {1, 1, 3}, [OP_ABS] = {1, 1, 1}, [OP_NRM] = {1, 1, 1}, [OP_SINCOS] = {1, 1, 1}, [OP_REP] = {1, 0, 1},
    [OP_ENDREP] = {1, 0, 0}, [OP_IF] = {1, 0, 1}, [OP_IFC] = {1, 0, 2}, [OP_ELSE] = {1, 0, 0}, [OP_ENDIF] = {1, 0, 0},
    [OP_BREAK] = {1, 0, 0}, [OP_BREAKC] = {1, 0, 2}, [OP_MOVA] = {1, 1, 1}, [OP_DEFB] = {1, 0, 0}, [OP_DEFI] = {1, 0, 0},
    [OP_TEXCOORD] = {1, 1, 0}, [OP_TEXKILL] = {1, 1, 0}, [OP_TEX] = {1, 1, 0}, [OP_TEXBEM] = {1, 1, 1},
    [OP_TEXBEML] = {1, 1, 1}, [OP_TEXREG2AR] = {1, 1, 1}, [OP_TEXREG2GB] = {1, 1, 1}, [OP_TEXM3x2PAD] = {1, 1, 1},
    [OP_TEXM3x2TEX] = {1, 1, 1}, [OP_TEXM3x3PAD] = {1, 1, 1}, [OP_TEXM3x3TEX] = {1, 1, 1}, [OP_TEXM3x3SPEC] = {1, 1, 2},
    [OP_TEXM3x3VSPEC] = {1, 1, 1}, [OP_EXPP] = {1, 1, 1}, [OP_LOGP] = {1, 1, 1}, [OP_CND] = {1, 1, 3}, [OP_DEF] = {1, 0, 0},
    [OP_TEXREG2RGB] = {1, 1, 1}, [OP_TEXDP3TEX] = {1, 1, 1}, [OP_TEXM3x2DEPTH] = {1, 1, 1}, [OP_TEXDP3] = {1, 1, 1},
    [OP_TEXM3x3] = {1, 1, 1}, [OP_TEXDEPTH] = {1, 1, 0}, [OP_CMP] = {1, 1, 3}, [OP_BEM] = {1, 1, 2},
    [OP_DP2ADD] = {1, 1, 3}, [OP_DSX] = {1, 1, 1}, [OP_DSY] = {1, 1, 1}, [OP_TEXLDD] = {1, 1, 4}, [OP_SETP] = {1, 1, 2},
    [OP_TEXLDL] = {1, 1, 2}, [OP_BREAKP] = {1, 0, 1},
};

typedef struct Dec {
    const uint32_t *t;
    size_t n, pos;
    char *err;
    size_t errlen;
    D3dShader *sh;
    uint8_t out_file[12], out_idx[12];   // vs_3_0 o# declarations
} Dec;

static int fail(Dec *d, const char *fmt, ...) {
    if (d->err && d->errlen) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(d->err, d->errlen, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static int reg_type(uint32_t t) { return (int)(((t >> 28) & 7) | ((t >> 8) & 0x18)); }

// maps a register (type, number) to a decoded file/index; dst = destination operand
static int map_reg(Dec *d, int type, int num, int dst, uint8_t *file, uint16_t *idx) {
    const D3dShader *sh = d->sh;
    *idx = (uint16_t)num;
    switch (type) {
    case 0: case 16:
        if (num >= 32) return fail(d, "temp register r%d out of range", num);
        *file = RF_TEMP; return 1;
    case 1:
        if (!sh->pixel) {
            if (num >= 16) return fail(d, "input register v%d out of range", num);
            *file = RF_INPUT; return 1;
        }
        if (dst) return fail(d, "ps input register written");
        if (sh->major < 3) {
            if (num >= 2) return fail(d, "colour register v%d out of range", num);
            *file = RF_VARY; *idx = (uint16_t)(8 + num); return 1;
        }
        if (num >= 16) return fail(d, "input register v%d out of range", num);
        if (sh->in_slot[num] < 0) { *file = RF_ZERO; *idx = 0; } else { *file = RF_VARY; *idx = (uint16_t)sh->in_slot[num]; }
        return 1;
    case 2:
        if (num >= 256) return fail(d, "constant register c%d out of range", num);
        *file = RF_CONST; return 1;
    case 3:
        if (!sh->pixel) { if (num != 0) return fail(d, "bad address register"); *file = RF_ADDR; return 1; }
        if (sh->major >= 3 || num >= 8) return fail(d, "texture register t%d invalid", num);
        if (sh->major == 1) { *file = RF_TREG; return 1; }
        if (dst) return fail(d, "texture register written");
        *file = RF_VARY; return 1;
    case 4:
        if (sh->pixel || num > 2) return fail(d, "bad rasterizer output register");
        if (num == 0) { *file = RF_OUT_POS; *idx = 0; } else if (num == 1) { *file = RF_OUT_VARY; *idx = 10; }
        else { *file = RF_OUT_PSIZE; *idx = 0; }
        return 1;
    case 5:
        if (sh->pixel || num > 1) return fail(d, "bad colour output register");
        *file = RF_OUT_VARY; *idx = (uint16_t)(8 + num); return 1;
    case 6:
        if (sh->pixel) return fail(d, "bad register type 6 in pixel shader");
        if (sh->major < 3) {
            if (num >= 8) return fail(d, "oT%d out of range", num);
            *file = RF_OUT_VARY; return 1;
        }
        if (num >= 12) return fail(d, "o%d out of range", num);
        *file = d->out_file[num]; *idx = d->out_idx[num]; return 1;
    case 7:
        if (num >= 16) return fail(d, "i%d out of range", num);
        *file = RF_CONSTI; return 1;
    case 8:
        if (!sh->pixel || num >= 4) return fail(d, "bad colour output oC%d", num);
        *file = RF_OUT_COLOR; return 1;
    case 9:
        if (!sh->pixel) return fail(d, "oDepth in vertex shader");
        *file = RF_OUT_DEPTH; *idx = 0; return 1;
    case 10:
        if (num >= (sh->pixel ? 16 : 4)) return fail(d, "sampler s%d out of range", num);
        *file = RF_SAMP; return 1;
    case 14:
        if (num >= 16) return fail(d, "b%d out of range", num);
        *file = RF_CONSTB; return 1;
    case 15: *file = RF_LOOP; *idx = 0; return 1;
    case 17:
        if (!sh->pixel || num >= 2) return fail(d, "bad misc register");
        *file = RF_MISC; return 1;
    case 18:
        if (num >= MAX_LABELS) return fail(d, "label l%d out of range", num);
        *file = RF_LABEL; return 1;
    case 19:
        if (num != 0) return fail(d, "bad predicate register");
        *file = RF_PRED; *idx = 0; return 1;
    default:
        return fail(d, "unsupported register type %d", type);
    }
}

static int parse_dst(Dec *d, size_t end, DestOp *o, int ro_ok) {
    if (d->pos >= end) return fail(d, "truncated instruction (destination)");
    uint32_t t = d->t[d->pos++];
    if (!(t & 0x80000000u)) return fail(d, "bad destination token %08x", t);
    if (t & 0x2000) return fail(d, "relative destination addressing unsupported");
    int type = reg_type(t);
    if (!map_reg(d, type, (int)(t & 0x7FF), !ro_ok, &o->file, &o->idx)) return 0;
    if (!ro_ok && (o->file == RF_CONSTI || o->file == RF_CONSTB || o->file == RF_SAMP || o->file == RF_LABEL || o->file == RF_LOOP ||
        o->file == RF_INPUT || o->file == RF_CONST || o->file == RF_MISC || o->file == RF_VARY))
        return fail(d, "register type %d is not writable", type);
    o->mask = (uint8_t)((t >> 16) & 0xF);
    o->sat = (uint8_t)(((t >> 20) & 1));
    o->shift = (uint8_t)((t >> 24) & 0xF);
    return 1;
}

static int parse_src(Dec *d, size_t end, Operand *o, int allow_out) {
    if (d->pos >= end) return fail(d, "truncated instruction (source)");
    uint32_t t = d->t[d->pos++];
    if (!(t & 0x80000000u)) return fail(d, "bad source token %08x", t);
    int type = reg_type(t);
    memset(o, 0, sizeof *o);
    o->swz = (uint8_t)((t >> 16) & 0xFF);
    int mod = (int)((t >> 24) & 0xF);
    if (mod > SM_NOT) return fail(d, "bad source modifier %d", mod);
    o->mod = (uint8_t)mod;
    if (t & 0x2000) {
        if (d->sh->major >= 2) {
            if (d->pos >= end) return fail(d, "truncated relative address token");
            uint32_t rt = d->t[d->pos++];
            int rtype = reg_type(rt);
            if (!(rt & 0x80000000u) || (rtype != 3 && rtype != 15)) return fail(d, "bad relative address register");
            if (rtype == 3 && d->sh->pixel) return fail(d, "a0 in pixel shader");
            o->rel = rtype == 3 ? 1 : 2;
            o->relc = (uint8_t)((rt >> 16) & 3);
        } else {
            if (d->sh->pixel) return fail(d, "relative addressing in ps 1.x");
            o->rel = 1; o->relc = 0;
        }
    }
    if (o->rel && type == 1 && d->sh->pixel && d->sh->major >= 3) {      // v[aL + n]: remapped through in_slot at run time
        if ((t & 0x7FF) >= 16) return fail(d, "input register v%d out of range", (int)(t & 0x7FF));
        o->file = RF_INPUT; o->idx = (uint16_t)(t & 0x7FF);
    } else if (!map_reg(d, type, (int)(t & 0x7FF), 0, &o->file, &o->idx)) return 0;
    if (!allow_out && o->file >= RF_OUT_POS && o->file <= RF_SINK) return fail(d, "write-only register read");
    if (o->rel && o->file != RF_CONST && o->file != RF_INPUT && o->file != RF_VARY && o->file != RF_TEMP)
        return fail(d, "relative addressing not valid for this register");
    if (o->rel && o->file == RF_ZERO) return fail(d, "relative addressing of undeclared input");
    return 1;
}

static void note_temp(D3dShader *sh, int idx) { if (idx + 1 > sh->ntemps) sh->ntemps = idx + 1; }

static int decode_dcl(Dec *d, size_t end) {
    D3dShader *sh = d->sh;
    if (d->pos + 2 > end) return fail(d, "truncated dcl");
    uint32_t t1 = d->t[d->pos++], t2 = d->t[d->pos++];
    if (!(t2 & 0x80000000u)) return fail(d, "bad dcl register token");
    int type = reg_type(t2), num = (int)(t2 & 0x7FF);
    int usage = (int)(t1 & 0x1F), uidx = (int)((t1 >> 16) & 0xF), ttype = (int)((t1 >> 27) & 0xF);
    if (type == 10) {
        if (num >= (sh->pixel ? 16 : 4)) return fail(d, "sampler s%d out of range", num);
        int k = ttype == 3 ? SG_TEX_CUBE : ttype == 4 ? SG_TEX_VOLUME : SG_TEX_2D;
        if (ttype < 1 || ttype > 4) return fail(d, "bad sampler type %d", ttype);
        sh->sampler_type[num] = (int8_t)k;
        sh->sampler_mask |= 1u << num;
        return 1;
    }
    if (!sh->pixel && type == 1) {
        if (num >= 16) return fail(d, "input register v%d out of range", num);
        if (sh->nvsin < D3DSH_VS_INPUTS) {
            sh->vsin[sh->nvsin].reg = num; sh->vsin[sh->nvsin].usage = usage; sh->vsin[sh->nvsin].usage_index = uidx;
            sh->nvsin++;
        }
        return 1;
    }
    if (!sh->pixel && type == 6 && sh->major >= 3) {
        if (num >= 12) return fail(d, "o%d out of range", num);
        if (usage == D3DSU_POSITION && uidx == 0) { d->out_file[num] = RF_OUT_POS; d->out_idx[num] = 0; }
        else if (usage == D3DSU_PSIZE && uidx == 0) { d->out_file[num] = RF_OUT_PSIZE; d->out_idx[num] = 0; }
        else {
            int s = d3dsh_varying_slot(usage, uidx);
            if (s < 0) { d->out_file[num] = RF_SINK; d->out_idx[num] = 0; }
            else { d->out_file[num] = RF_OUT_VARY; d->out_idx[num] = (uint8_t)s; }
        }
        return 1;
    }
    if (sh->pixel && type == 1 && sh->major >= 3) {
        if (num >= 16) return fail(d, "input register v%d out of range", num);
        sh->in_slot[num] = (int8_t)d3dsh_varying_slot(usage, uidx);
        return 1;
    }
    if (sh->pixel && (type == 1 || type == 3 || type == 17 || type == 0)) return 1;   // ps_2_x input declarations
    return fail(d, "unsupported dcl of register type %d", type);
}

D3dShader *d3dsh_create(const uint32_t *tokens, size_t max_tokens, char *err, size_t errlen) {
    Dec d;
    memset(&d, 0, sizeof d);
    d.err = err; d.errlen = errlen;
    if (err && errlen) err[0] = 0;
    if (!tokens || max_tokens < 2) { fail(&d, "shader too short"); return NULL; }
    uint32_t ver = tokens[0];
    if ((ver >> 16) != 0xFFFE && (ver >> 16) != 0xFFFF) { fail(&d, "bad version token %08x", ver); return NULL; }
    D3dShader *sh = (D3dShader *)calloc(1, sizeof *sh);
    if (!sh) { fail(&d, "out of memory"); return NULL; }
    d.sh = sh;
    sh->pixel = (ver >> 16) == 0xFFFF;
    sh->major = (int)((ver >> 8) & 0xFF); sh->minor = (int)(ver & 0xFF);
    if (sh->major < 1 || sh->major > 3) { fail(&d, "unsupported shader version %d.%d", sh->major, sh->minor); goto bad; }
    sh->ps1x = sh->pixel && sh->major == 1;
    memset(sh->in_slot, -1, sizeof sh->in_slot);
    memset(sh->sampler_type, -1, sizeof sh->sampler_type);
    for (int i = 0; i < 12; i++) { d.out_file[i] = RF_SINK; d.out_idx[i] = 0; }
    // full decode (the end token is found by the instruction walk)
    sh->tokens = NULL;
    sh->ins = (Instr *)calloc(max_tokens + 1, sizeof(Instr));
    if (!sh->ins) { fail(&d, "out of memory"); goto bad; }
    d.t = tokens; d.n = max_tokens;
    int sm2 = sh->major >= 2;
    struct { int kind, ifpc, elsepc, loop; } stk[32];
    int nstk = 0, ended = 0;
    d.pos = 1;
    while (d.pos < d.n) {
        uint32_t tok = tokens[d.pos];
        int op = (int)(tok & 0xFFFF);
        if (op == OP_COMMENT && !(tok & 0x80000000u)) {
            size_t sz = (tok >> 16) & 0x7FFF;
            if (d.pos + 1 + sz > d.n) { fail(&d, "comment runs past the end of the shader"); goto bad; }
            d.pos += 1 + sz; continue;
        }
        if (tok == 0x0000FFFFu) { d.pos++; ended = 1; break; }
        if (tok & 0x80000000u) { fail(&d, "expected instruction token, found %08x", tok); goto bad; }
        if (op == OP_PHASE) { if (!sh->ps1x || sh->minor < 4) { fail(&d, "phase outside ps_1_4"); goto bad; } d.pos++; continue; }
        if (op >= OP_MAX_CODE || !OPI[op].valid) { fail(&d, "unknown opcode %d", op); goto bad; }
        const OpInfo *oi = &OPI[op];
        size_t start = d.pos++;
        size_t end;
        Instr I;
        memset(&I, 0, sizeof I);
        I.op = (uint16_t)op;
        I.target = -1;
        if (sm2) {
            size_t len = (tok >> 24) & 0xF;
            if (op == OP_DCL && len != 2) { fail(&d, "bad dcl length"); goto bad; }
            end = d.pos + len;
        } else {
            end = d.n;   // lengths are implicit below
        }
        if (end > d.n) { fail(&d, "instruction runs past the end of the shader"); goto bad; }
        if (op == OP_DCL) {
            if (!sm2) { fail(&d, "dcl in a 1.x shader"); goto bad; }
            if (!decode_dcl(&d, end)) goto bad;
            d.pos = end; continue;
        }
        if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB) {
            DestOp dd;
            int want = op == OP_DEF ? 2 : op == OP_DEFI ? 7 : 14;
            size_t nvals = op == OP_DEFB ? 1 : 4;
            if (d.pos >= d.n) { fail(&d, "truncated def"); goto bad; }
            uint32_t rt = tokens[d.pos];
            if (!(rt & 0x80000000u) || reg_type(rt) != want) { fail(&d, "bad register type in def"); goto bad; }
            if (!parse_dst(&d, d.n, &dd, 1)) goto bad;
            if (d.pos + nvals > d.n) { fail(&d, "truncated def"); goto bad; }
            if (sm2 && d.pos + nvals != end) { fail(&d, "bad def length"); goto bad; }
            if (op == OP_DEF) {
                if (!sh->deff) { sh->deff = (float (*)[4])calloc(256, 16); if (!sh->deff) { fail(&d, "out of memory"); goto bad; } }
                for (int k = 0; k < 4; k++) {
                    float f; memcpy(&f, &tokens[d.pos + k], 4);
                    if (sh->ps1x) f = f < -1 ? -1 : f > 1 ? 1 : f;
                    sh->deff[dd.idx][k] = f;
                }
                sh->defmask[dd.idx >> 5] |= 1u << (dd.idx & 31);
            } else if (op == OP_DEFI) {
                for (int k = 0; k < 4; k++) sh->defi[dd.idx][k] = (int)tokens[d.pos + k];
                sh->defi_mask |= 1u << dd.idx;
            } else {
                sh->defb[dd.idx] = tokens[d.pos] != 0;
                sh->defb_mask |= 1u << dd.idx;
            }
            d.pos += nvals; continue;
        }
        if (op == OP_NOP) { d.pos = sm2 ? end : d.pos; continue; }
        // operand layout
        int hd = oi->hd, ns = oi->ns;
        int texop = op >= OP_TEXCOORD && op <= OP_TEXM3x3VSPEC;
        if (op == OP_TEXCOORD) ns = (sh->ps1x && sh->minor >= 4) ? 1 : 0;
        if (op == OP_TEX) ns = sh->ps1x ? (sh->minor >= 4 ? 1 : 0) : 2;
        if (op == OP_TEXKILL) ns = 0;
        if (!sh->pixel && (texop || op == OP_DSX || op == OP_DSY || op == OP_TEXLDD || op == OP_CND || op == OP_BEM || op == OP_TEXDEPTH ||
                           op == OP_TEXDP3 || op == OP_TEXM3x3 || op == OP_TEXDP3TEX || op == OP_TEXREG2RGB || op == OP_TEXM3x2DEPTH ||
                           op == OP_DP2ADD)) { fail(&d, "pixel-only instruction %d in vertex shader", op); goto bad; }
        if (sh->pixel && (op == OP_MOVA)) { fail(&d, "mova in pixel shader"); goto bad; }
        if (!sh->pixel && op == OP_TEXLDL && sh->major < 3) { fail(&d, "texldl needs vs_3_0"); goto bad; }
        if (sm2) {
            I.pred = (tok & 0x10000000u) ? 1 : 0;
            if (I.pred && !((sh->major == 2 && sh->minor >= 1) || sh->major == 3)) { fail(&d, "predication needs shader model 2.1+"); goto bad; }
        }
        I.coissue = (tok & 0x40000000u) ? 1 : 0;
        I.ctl = (uint8_t)((tok >> 16) & 0xFF);
        I.hasdst = (uint8_t)hd;
        size_t opend = end;
        if (sm2 && I.pred) { if (end - d.pos < 1) { fail(&d, "missing predicate token"); goto bad; } opend = end - 1; }
        if (hd) {
            if (!parse_dst(&d, opend, &I.dst, op == OP_TEXKILL)) goto bad;
        }
        if (sm2) {
            int n = 0;
            while (d.pos < opend) {
                if (n >= 4) { fail(&d, "too many operands"); goto bad; }
                if (!parse_src(&d, opend, &I.src[n], 0)) goto bad;
                n++;
            }
            ns = n;
            // arity checks
            int exp = oi->ns;
            if (op == OP_TEX) exp = 2;
            if (op == OP_TEXCOORD || op == OP_TEXKILL || op == OP_TEXDEPTH) exp = 0;
            if (op == OP_SINCOS) { if (n != 1 && n != 3) { fail(&d, "sincos needs 1 or 3 sources"); goto bad; } }
            else if (n != exp) { fail(&d, "opcode %d expects %d sources, got %d", op, exp, n); goto bad; }
        } else {
            for (int n = 0; n < ns; n++) if (!parse_src(&d, d.n, &I.src[n], 0)) goto bad;
        }
        I.nsrc = (uint8_t)ns;
        if (I.pred) {
            uint32_t pt = tokens[d.pos++];
            if (!(pt & 0x80000000u) || reg_type(pt) != 19 || (pt & 0x7FF)) { fail(&d, "bad predicate token"); goto bad; }
            int m = (int)((pt >> 24) & 0xF);
            if (m != 0 && m != SM_NOT) { fail(&d, "bad predicate modifier"); goto bad; }
            I.pred = m == SM_NOT ? 2 : 1;
            I.predswz = (uint8_t)((pt >> 16) & 0xFF);
        }
        if (sm2) d.pos = end;
        (void)start;
        // per-opcode validation and flow control matching
        int pc = sh->nins;
        switch (op) {
        case OP_IF:
            if (I.src[0].file != RF_CONSTB && I.src[0].file != RF_PRED) { fail(&d, "if needs b# or p0"); goto bad; }
            if (nstk >= 24) { fail(&d, "if nesting too deep"); goto bad; }
            stk[nstk].kind = OP_IF; stk[nstk].ifpc = pc; stk[nstk].elsepc = -1; nstk++; break;
        case OP_IFC:
            if (I.ctl < 1 || I.ctl > 6) { fail(&d, "bad comparison"); goto bad; }
            if (nstk >= 24) { fail(&d, "if nesting too deep"); goto bad; }
            stk[nstk].kind = OP_IF; stk[nstk].ifpc = pc; stk[nstk].elsepc = -1; nstk++; break;
        case OP_ELSE:
            if (nstk < 1 || stk[nstk - 1].kind != OP_IF || stk[nstk - 1].elsepc >= 0) { fail(&d, "else without if"); goto bad; }
            stk[nstk - 1].elsepc = pc;
            sh->ins[stk[nstk - 1].ifpc].target = pc; break;
        case OP_ENDIF:
            if (nstk < 1 || stk[nstk - 1].kind != OP_IF) { fail(&d, "endif without if"); goto bad; }
            if (stk[nstk - 1].elsepc >= 0) sh->ins[stk[nstk - 1].elsepc].target = pc; else sh->ins[stk[nstk - 1].ifpc].target = pc;
            nstk--; break;
        case OP_LOOP:
            if (I.src[0].file != RF_LOOP || I.src[1].file != RF_CONSTI) { fail(&d, "loop needs aL, i#"); goto bad; }
            if (nstk >= 24) { fail(&d, "loop nesting too deep"); goto bad; }
            stk[nstk].kind = OP_LOOP; stk[nstk].ifpc = pc; nstk++; break;
        case OP_REP:
            if (I.src[0].file != RF_CONSTI) { fail(&d, "rep needs i#"); goto bad; }
            if (nstk >= 24) { fail(&d, "rep nesting too deep"); goto bad; }
            stk[nstk].kind = OP_REP; stk[nstk].ifpc = pc; nstk++; break;
        case OP_ENDLOOP: case OP_ENDREP: {
            int want = op == OP_ENDLOOP ? OP_LOOP : OP_REP;
            if (nstk < 1 || stk[nstk - 1].kind != want) { fail(&d, "unmatched end of loop"); goto bad; }
            int lp = stk[nstk - 1].ifpc;
            sh->ins[lp].target = pc; I.target = lp;
            for (int k = lp + 1; k < pc; k++)
                if ((sh->ins[k].op == OP_BREAK || sh->ins[k].op == OP_BREAKC || sh->ins[k].op == OP_BREAKP) && sh->ins[k].target < 0) sh->ins[k].target = pc;
            nstk--; break; }
        case OP_BREAK: case OP_BREAKC: case OP_BREAKP: {
            int inloop = 0;
            for (int k = 0; k < nstk; k++) if (stk[k].kind != OP_IF) inloop = 1;
            if (!inloop) { fail(&d, "break outside a loop"); goto bad; }
            if (op == OP_BREAKC && (I.ctl < 1 || I.ctl > 6)) { fail(&d, "bad comparison"); goto bad; }
            if (op == OP_BREAKP && I.src[0].file != RF_PRED) { fail(&d, "breakp needs p0"); goto bad; }
            break; }
        case OP_CALL: case OP_CALLNZ:
            if (I.src[0].file != RF_LABEL) { fail(&d, "call needs a label"); goto bad; }
            if (op == OP_CALLNZ && I.src[1].file != RF_CONSTB && I.src[1].file != RF_PRED) { fail(&d, "callnz needs b# or p0"); goto bad; }
            I.target = I.src[0].idx; break;
        case OP_LABEL:
            if (I.src[0].file != RF_LABEL) { fail(&d, "label needs l#"); goto bad; }
            if (!sh->label_pc) {
                sh->label_pc = (int *)malloc(MAX_LABELS * sizeof(int));
                if (!sh->label_pc) { fail(&d, "out of memory"); goto bad; }
                for (int k = 0; k < MAX_LABELS; k++) sh->label_pc[k] = -1;
            }
            if (sh->label_pc[I.src[0].idx] >= 0) { fail(&d, "label l%d defined twice", I.src[0].idx); goto bad; }
            sh->label_pc[I.src[0].idx] = pc; break;
        case OP_SETP:
            if (I.ctl < 1 || I.ctl > 6 || I.dst.file != RF_PRED) { fail(&d, "bad setp"); goto bad; }
            break;
        case OP_MOVA:
            if (I.dst.file != RF_ADDR) { fail(&d, "mova needs a0"); goto bad; }
            break;
        case OP_TEX:
            if (sm2 && I.src[1].file != RF_SAMP) { fail(&d, "texld needs a sampler"); goto bad; }
            if (sm2 && I.ctl > 2) { fail(&d, "bad texld control"); goto bad; }
            break;
        case OP_TEXLDL: case OP_TEXLDD:
            if (I.src[1].file != RF_SAMP) { fail(&d, "texture instruction needs a sampler"); goto bad; }
            break;
        default: break;
        }
        if (I.dst.file == RF_PRED && op != OP_SETP) { fail(&d, "only setp writes p0"); goto bad; }
        if (I.dst.file == RF_ADDR && op != OP_MOVA && !(op == OP_MOV && !sm2)) { fail(&d, "bad write to a0"); goto bad; }
        if (op != OP_TEX && op != OP_TEXLDL && op != OP_TEXLDD && op != OP_LOOP) {
            for (int k = 0; k < I.nsrc; k++)
                if (I.src[k].file == RF_SAMP || I.src[k].file == RF_LABEL) {
                    if (op == OP_CALL || op == OP_CALLNZ || op == OP_LABEL) continue;
                    fail(&d, "sampler/label used as a value"); goto bad;
                }
        }
        sh->ins[sh->nins++] = I;
    }
    if (!ended) { fail(&d, "missing end token"); goto bad; }
    if (nstk) { fail(&d, "unterminated if/loop"); goto bad; }
    sh->ntok = d.pos;
    { Instr *ni = (Instr *)realloc(sh->ins, (sh->nins + 1) * sizeof(Instr)); if (ni) sh->ins = ni; }
    sh->tokens = (uint32_t *)malloc(sh->ntok * 4);
    if (!sh->tokens) { fail(&d, "out of memory"); goto bad; }
    memcpy(sh->tokens, tokens, sh->ntok * 4);
    // second pass: labels, defined constants, statistics
    for (int i = 0; i < sh->nins; i++) {
        Instr *I = &sh->ins[i];
        if (I->op == OP_CALL || I->op == OP_CALLNZ) {
            if (!sh->label_pc || sh->label_pc[I->target] < 0) { fail(&d, "call to undefined label l%d", I->target); goto bad; }
            I->target = sh->label_pc[I->target];
        }
        if (I->hasdst) {
            if (I->dst.file == RF_TEMP || (I->op == OP_TEXKILL && I->dst.file == RF_TEMP)) note_temp(sh, I->dst.idx);
            if (I->dst.file == RF_OUT_VARY) sh->vs_out_mask |= 1u << I->dst.idx;
            if (I->dst.file == RF_OUT_DEPTH) sh->writes_depth = 1;
            if (I->dst.file == RF_TREG || (I->op == OP_TEXKILL && I->dst.file == RF_VARY)) sh->ps_in_mask |= 1u << I->dst.idx;
        }
        if (I->op == OP_TEXDEPTH || I->op == OP_TEXM3x2DEPTH) sh->writes_depth = 1;
        if (sh->pixel && I->hasdst && sh->ps1x) {
            int stageop = I->op == OP_TEX || (I->op >= OP_TEXBEM && I->op <= OP_TEXREG2GB) || I->op == OP_TEXM3x2TEX || I->op == OP_TEXM3x3TEX ||
                          I->op == OP_TEXM3x3SPEC || I->op == OP_TEXM3x3VSPEC || I->op == OP_TEXREG2RGB || I->op == OP_TEXDP3TEX;
            if (stageop && I->dst.idx < 16) sh->sampler_mask |= 1u << I->dst.idx;
        }
        if (sh->pixel && (I->op == OP_DSX || I->op == OP_DSY || (I->op == OP_TEX && !sh->ps1x))) sh->deriv = 1;
        for (int k = 0; k < I->nsrc; k++) {
            Operand *o = &I->src[k];
            if (o->file == RF_TEMP) note_temp(sh, o->idx);
            if (o->file == RF_SAMP && o->idx < 16) sh->sampler_mask |= 1u << o->idx;
            if (o->file == RF_VARY) {
                if (o->rel) { for (int s = 0; s < 16; s++) if (sh->in_slot[s] >= 0) sh->ps_in_mask |= 1u << sh->in_slot[s]; }
                else sh->ps_in_mask |= 1u << o->idx;
            }
            if (o->file == RF_INPUT && o->rel && sh->pixel) {
                for (int s = 0; s < 16; s++) if (sh->in_slot[s] >= 0) sh->ps_in_mask |= 1u << sh->in_slot[s];
            }
            if (o->file == RF_TREG) sh->ps_in_mask |= 1u << o->idx;
            if (o->file == RF_CONST && !o->rel) {
                if (sh->defmask[o->idx >> 5] & (1u << (o->idx & 31))) o->file = RF_DEFC;
                else if (sh->ps1x) o->clampc = 1;
            } else if (o->file == RF_CONST && sh->ps1x) o->clampc = 1;
        }
    }
    if (sh->ps1x && sh->ntemps < 6) sh->ntemps = 6;
    if (sh->ntemps < 1) sh->ntemps = 1;
    sh->ps_nvary = sh->ps_in_mask ? 32 - __builtin_clz(sh->ps_in_mask) : 0;
    if (sh->ps_nvary > SG_VARYINGS) sh->ps_nvary = SG_VARYINGS;
    if (!sh->pixel) { sh->ps_in_mask = 0; sh->ps_nvary = 0; }
    else sh->vs_out_mask = 0;
    return sh;
bad:
    d3dsh_free(sh);
    return NULL;
}

void d3dsh_free(D3dShader *sh) {
    if (!sh) return;
    free(sh->tokens); free(sh->ins); free(sh->deff); free(sh->label_pc);
    free(sh);
}
int d3dsh_is_pixel(const D3dShader *sh) { return sh ? sh->pixel : 0; }
int d3dsh_major(const D3dShader *sh) { return sh ? sh->major : 0; }
int d3dsh_minor(const D3dShader *sh) { return sh ? sh->minor : 0; }
size_t d3dsh_token_count(const D3dShader *sh) { return sh ? sh->ntok : 0; }
const uint32_t *d3dsh_tokens(const D3dShader *sh) { return sh ? sh->tokens : NULL; }

int d3dsh_vs_inputs(const D3dShader *vs, D3dShaderInput *out, int max) {
    if (!vs || vs->pixel) return 0;
    int n = vs->nvsin < max ? vs->nvsin : max;
    for (int i = 0; i < n; i++) out[i] = vs->vsin[i];
    return n;
}
uint32_t d3dsh_vs_output_mask(const D3dShader *vs) { return vs && !vs->pixel ? vs->vs_out_mask : 0; }
uint32_t d3dsh_ps_input_mask(const D3dShader *ps) { return ps && ps->pixel ? ps->ps_in_mask : 0; }
int d3dsh_ps_nvary(const D3dShader *ps) { return ps && ps->pixel ? ps->ps_nvary : 0; }
int d3dsh_ps_writes_depth(const D3dShader *ps) { return ps && ps->pixel ? ps->writes_depth : 0; }
uint32_t d3dsh_ps_sampler_mask(const D3dShader *ps) { return ps && ps->pixel ? ps->sampler_mask : 0; }
int d3dsh_ps_sampler_type(const D3dShader *ps, int s) {
    if (!ps || !ps->pixel || s < 0 || s >= 16) return -1;
    return ps->sampler_type[s];
}
