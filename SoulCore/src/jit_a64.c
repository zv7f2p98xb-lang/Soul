// SoulCore - x86-64 -> ARM64 dynamic binary translator.
//
// Guest code is translated one basic block at a time. Guest registers live in
// the X86State structure (x19 points at it); each translated instruction loads
// its operands, computes with native ARM64 instructions and stores results back.
// EFLAGS are kept lazily (lf_* fields) and conditional branches use the native
// NZCV flags whenever the producing instruction is in the same block.
// Instructions the translator does not know end the block; the dispatcher then
// runs them through the interpreter.
#include "soul_internal.h"
#include "decode.h"
#include "interp.h"
#include "a64_emit.h"
#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__) || defined(__arm64__)

#define MAX_BLOCK_INSNS 64
#define CODE_RESERVE (64 * 1024)
#define FAST_SIZE 8192

typedef void (*BlockFn)(X86State *);

typedef struct Block {
    uint64_t rip;
    BlockFn code;
    struct Block *next;
} Block;

struct JitCache {
    SoulProcess *p;
    uint8_t *rw, *rx;
    size_t size, used;
    Block **table;
    size_t table_mask;
    Block *pool;
    size_t pool_used, pool_cap;
    struct { uint64_t rip; BlockFn code; } fast[FAST_SIZE];
    X86Insn *insns;          // decoded instructions referenced by interpreter callouts
    size_t insn_used, insn_cap;
    uint64_t blocks;
    uint64_t flushes;
};

// x86 flag producer kinds that map onto native NZCV
enum { NK_NONE = 0, NK_SUB, NK_ADD, NK_LOGIC, NK_INC, NK_SHIFT, NK_FCMP };

// host register roles
enum { R_ST = 19, T0 = 0, T1 = 1, T2 = 2, T3 = 3, T4 = 4, R_EA = 6, S14 = 14, S15 = 15, R_CALL = 16 };

typedef struct {
    Emit e;
    X86State *st;
    struct JitCache *jc;
    int nat;          // current native flag kind
    int fail;         // translation of the current instruction is not possible
} J;

static int jit_h_cond(X86State *st, int cc) { return x86_cond(st, cc); }
static void jit_h_flags(X86State *st) { x86_flags(st); }
static void jit_h_exec(X86State *st, const X86Insn *in) { x86_exec(st, in); }

// ---- small emit helpers ---------------------------------------------------------------------

static inline uint32_t gpr_off(int r) { return ST_OFF(gpr) + 8u * (uint32_t)r; }
static inline uint32_t xmm_off(int r) { return ST_OFF(xmm) + 16u * (uint32_t)r; }

static void mov_imm(J *j, int rd, uint64_t v) {
    Emit *e = &j->e;
    if (v == 0) { em(e, a_movz(1, rd, 0, 0)); return; }
    if ((int64_t)v < 0 && (int64_t)v >= -65536) { em(e, a_movn(1, rd, (uint32_t)(~v & 0xFFFF), 0)); return; }
    int first = 1;
    for (int hw = 0; hw < 4; hw++) {
        uint32_t part = (uint32_t)(v >> (16 * hw)) & 0xFFFF;
        if (!part) continue;
        em(e, first ? a_movz(1, rd, part, hw) : a_movk(1, rd, part, hw));
        first = 0;
    }
}

static void add_imm(J *j, int rd, int rn, int64_t imm) {
    Emit *e = &j->e;
    if (imm == 0) { if (rd != rn) em(e, a_mov(1, rd, rn)); return; }
    int sub = imm < 0;
    uint64_t a = sub ? (uint64_t)(-imm) : (uint64_t)imm;
    if (a < 4096) { em(e, a_addsub_imm(1, sub, 0, rd, rn, (uint32_t)a, 0)); return; }
    if ((a & 0xFFF) == 0 && a < (1u << 24)) { em(e, a_addsub_imm(1, sub, 0, rd, rn, (uint32_t)(a >> 12), 1)); return; }
    mov_imm(j, S14, (uint64_t)imm);
    em(e, a_addsub_reg(1, 0, 0, rd, rn, S14, 0, 0));
}

static void call_helper(J *j, void *fn) {
    mov_imm(j, R_CALL, (uint64_t)(uintptr_t)fn);
    em(&j->e, a_blr(R_CALL));
    j->nat = NK_NONE;
}

static int size_log2(int size) { return size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3; }

// Byte register offset in X86State (handles AH/CH/DH/BH).
static uint32_t reg_off(const X86Insn *in, int r, int size) {
    if (size == 1 && !in->rex && r >= 4 && r < 8) return gpr_off(r - 4) + 1;
    return gpr_off(r);
}
static void load_reg(J *j, const X86Insn *in, int hr, int r, int size) {
    em(&j->e, a_ldr_imm(size_log2(size), hr, R_ST, reg_off(in, r, size)));
}
// Stores to a guest register with x86 semantics: 32-bit writes zero-extend, 8/16-bit merge.
static void store_reg(J *j, const X86Insn *in, int hr, int r, int size) {
    if (size == 4) {
        em(&j->e, a_mov(0, hr, hr)); // zero-extend
        em(&j->e, a_str_imm(3, hr, R_ST, gpr_off(r)));
        return;
    }
    em(&j->e, a_str_imm(size_log2(size), hr, R_ST, reg_off(in, r, size)));
}

// Effective address of the memory operand into R_EA.
static void emit_ea(J *j, const X86Insn *in, int with_seg) {
    Emit *e = &j->e;
    if (in->ea_base == EA_RIP) {
        mov_imm(j, R_EA, in->addr + in->len + (int64_t)in->disp);
    } else {
        int have = 0;
        if (in->ea_base != EA_NONE) { em(e, a_ldr_imm(3, R_EA, R_ST, gpr_off(in->ea_base))); have = 1; }
        if (in->ea_index != EA_NONE) {
            em(e, a_ldr_imm(3, S15, R_ST, gpr_off(in->ea_index)));
            if (have) em(e, a_addsub_reg(1, 0, 0, R_EA, R_EA, S15, 0, in->ea_scale));
            else em(e, a_lsl(1, R_EA, S15, in->ea_scale));
            have = 1;
        }
        if (!have) mov_imm(j, R_EA, (uint64_t)(int64_t)in->disp);
        else add_imm(j, R_EA, R_EA, in->disp);
    }
    if (in->adsize32) em(e, a_mov(0, R_EA, R_EA));
    if (with_seg && in->seg) {
        em(e, a_ldr_imm(3, S15, R_ST, in->seg == SEG_FS ? ST_OFF(fs_base) : ST_OFF(gs_base)));
        em(e, a_addsub_reg(1, 0, 0, R_EA, R_EA, S15, 0, 0));
    }
}

// Load the r/m operand (register or memory, address must already be in R_EA for memory).
static void load_rm(J *j, const X86Insn *in, int hr, int size) {
    if (in->mod == 3) load_reg(j, in, hr, in->rm, size);
    else em(&j->e, a_ldr_imm(size_log2(size), hr, R_EA, 0));
}
static void store_rm(J *j, const X86Insn *in, int hr, int size) {
    if (in->mod == 3) store_reg(j, in, hr, in->rm, size);
    else em(&j->e, a_str_imm(size_log2(size), hr, R_EA, 0));
}

static void store_lazy(J *j, uint32_t op, int size, int rdst, int rsrc, int rres) {
    Emit *e = &j->e;
    em(e, a_str_imm(3, rdst, R_ST, ST_OFF(lf_dst)));
    em(e, a_str_imm(3, rsrc, R_ST, ST_OFF(lf_src)));
    em(e, a_str_imm(3, rres, R_ST, ST_OFF(lf_res)));
    mov_imm(j, S14, ((uint64_t)(uint32_t)size << 32) | op);
    em(e, a_str_imm(3, S14, R_ST, ST_OFF(lf_op)));
}

// ---- condition codes -------------------------------------------------------------------------

static int map_cond(int nk, int cc) {
    static const int8_t sub[16] = { C_VS, C_VC, C_LO, C_HS, C_EQ, C_NE, C_LS, C_HI, C_MI, C_PL, -1, -1, C_LT, C_GE, C_LE, C_GT };
    static const int8_t add[16] = { C_VS, C_VC, C_HS, C_LO, C_EQ, C_NE, -1, -1, C_MI, C_PL, -1, -1, C_LT, C_GE, C_LE, C_GT };
    static const int8_t logic[16] = { C_VS, C_VC, C_HS, C_LO, C_EQ, C_NE, C_EQ, C_NE, C_MI, C_PL, -1, -1, C_LT, C_GE, C_LE, C_GT };
    static const int8_t inc[16] = { C_VS, C_VC, -1, -1, C_EQ, C_NE, -1, -1, C_MI, C_PL, -1, -1, C_LT, C_GE, C_LE, C_GT };
    static const int8_t shift[16] = { -1, -1, -1, -1, C_EQ, C_NE, -1, -1, C_MI, C_PL, -1, -1, -1, -1, -1, -1 };
    static const int8_t fcmp[16] = { -1, -1, C_LT, C_GE, -1, -1, C_LE, C_GT, -1, -1, C_VS, C_VC, -1, -1, -1, -1 };
    switch (nk) {
    case NK_SUB: return sub[cc];
    case NK_ADD: return add[cc];
    case NK_LOGIC: return logic[cc];
    case NK_INC: return inc[cc];
    case NK_SHIFT: return shift[cc];
    case NK_FCMP: return fcmp[cc];
    default: return -1;
    }
}

// Leaves an ARM condition in the returned code; may call a helper (result in w0, cond NE).
static int emit_cond(J *j, int cc) {
    int c = map_cond(j->nat, cc);
    if (c >= 0) return c;
    em(&j->e, a_mov(1, 0, R_ST));
    mov_imm(j, 1, (uint64_t)cc);
    call_helper(j, (void *)jit_h_cond);
    em(&j->e, a_addsub_imm(0, 1, 1, XZR, 0, 0, 0)); // cmp w0, #0
    j->nat = NK_NONE;
    return C_NE;
}

// ---- instruction classification for flag liveness -----------------------------------------------

typedef struct { X86Insn in; int fwrite; int fread; int need; } Slot;

static int is_cc_reader(const X86Insn *in) {
    if (in->map == MAP_1) {
        if (in->op >= 0x70 && in->op <= 0x7F) return 1;
        if (in->op >= 0x10 && in->op <= 0x1D && (in->op & 7) < 6) return 1; // ADC/SBB forms
        if ((in->op == 0x80 || in->op == 0x81 || in->op == 0x83) && ((in->reg & 7) == 2 || (in->reg & 7) == 3)) return 1;
        if (in->op == 0x9C || in->op == 0x9F || in->op == 0xF5) return 1;
        if ((in->op == 0xC0 || in->op == 0xC1 || (in->op >= 0xD0 && in->op <= 0xD3)) && ((in->reg & 7) == 2 || (in->reg & 7) == 3)) return 1;
        if ((in->op == 0xDA || in->op == 0xDB) && in->mod == 3) return 1;
        return 0;
    }
    if (in->map == MAP_0F) {
        if (in->op >= 0x40 && in->op <= 0x4F) return 1;
        if (in->op >= 0x80 && in->op <= 0x9F) return 1;
    }
    return 0;
}

// ---- translation of single instructions ------------------------------------------------------------

static int alu_op_supported(int aop) { return aop == 0 || aop == 1 || aop == 4 || aop == 5 || aop == 6 || aop == 7; }

// T0 = dst value, T1 = src value -> T2 = result, flags as requested.
static void emit_alu(J *j, int aop, int size, int need) {
    Emit *e = &j->e;
    int sf = size == 8;
    int kind = (aop == 0) ? NK_ADD : (aop == 5 || aop == 7) ? NK_SUB : NK_LOGIC;
    if (size >= 4) {
        switch (aop) {
        case 0: em(e, a_addsub_reg(sf, 0, need, T2, T0, T1, 0, 0)); break;
        case 5: case 7: em(e, a_addsub_reg(sf, 1, need, T2, T0, T1, 0, 0)); break;
        case 4: case 8: em(e, a_logic_reg(sf, need ? 3 : 0, 0, T2, T0, T1, 0, 0)); break;
        case 1: em(e, a_logic_reg(sf, 1, 0, T2, T0, T1, 0, 0)); if (need) em(e, a_logic_reg(sf, 3, 0, XZR, T2, T2, 0, 0)); break;
        case 6: em(e, a_logic_reg(sf, 2, 0, T2, T0, T1, 0, 0)); if (need) em(e, a_logic_reg(sf, 3, 0, XZR, T2, T2, 0, 0)); break;
        }
    } else {
        int sh = 32 - 8 * size;
        switch (aop) {
        case 0: case 5: case 7:
            if (need) {
                em(e, a_lsl(0, T3, T0, sh));
                em(e, a_addsub_reg(0, aop != 0, 1, T2, T3, T1, 0, sh));
                em(e, a_lsr(0, T2, T2, sh));
            } else {
                em(e, a_addsub_reg(0, aop != 0, 0, T2, T0, T1, 0, 0));
                em(e, a_uxt(T2, T2, 8 * size));
            }
            break;
        default: {
            int opc = (aop == 1) ? 1 : (aop == 6) ? 2 : 0;
            em(e, a_logic_reg(0, opc, 0, T2, T0, T1, 0, 0));
            if (need) { em(e, a_lsl(0, T3, T2, sh)); em(e, a_logic_reg(0, 3, 0, XZR, T3, T3, 0, 0)); }
            break;
        }
        }
    }
    if (need) {
        store_lazy(j, kind == NK_ADD ? LF_ADD : kind == NK_SUB ? LF_SUB : LF_LOGIC, size, T0, T1, T2);
        j->nat = kind;
    }
}

static void emit_epilogue(J *j) {
    Emit *e = &j->e;
    em(e, a_ldp_post(19, 20, SP_REG, 16));
    em(e, a_ldp_post(29, 30, SP_REG, 16));
    em(e, a_ret());
}

#define PROLOGUE_INSNS 3

// Block exit to a target held in register r (T2..T4 allowed). The guest rip is
// stored, then the fast lookup table is probed inline: on a hit we jump straight
// into the next block's body (direct chaining) instead of returning to C.
static void emit_chain(J *j, int r, int known, uint64_t target) {
    Emit *e = &j->e;
    em(e, a_str_imm(3, r, R_ST, ST_OFF(rip)));
    em(e, a_ldr_imm(2, S15, R_ST, ST_OFF(stop_req)));
    uint32_t *stop_br = e->p;
    em(e, a_cbnz(0, S15, 0));
    if (known) {
        mov_imm(j, S14, (uint64_t)(uintptr_t)&j->jc->fast[(size_t)(target >> 1) & (FAST_SIZE - 1)]);
    } else {
        mov_imm(j, S14, (uint64_t)(uintptr_t)&j->jc->fast[0]);
        em(e, a_bfm(1, 2, S15, r, 1, 13));                        // ubfx x15, r, #1, #13
        em(e, a_addsub_reg(1, 0, 0, S14, S14, S15, 0, 4));        // x14 += idx * 16
    }
    em(e, 0xA9400000u | ((uint32_t)17 << 10) | ((uint32_t)S14 << 5) | (uint32_t)S15); // ldp x15, x17, [x14]
    em(e, a_addsub_reg(1, 1, 1, XZR, S15, r, 0, 0));          // cmp x15, r
    uint32_t *miss_br = e->p;
    em(e, a_bcond(C_NE, 0));
    uint32_t *nul_br = e->p;
    em(e, a_cbz(1, 17, 0));
    em(e, a_addsub_imm(1, 0, 0, 17, 17, PROLOGUE_INSNS * 4, 0));
    em(e, a_br(17));
    uint32_t *exit = e->p;
    if (!e->overflow) {
        *stop_br = a_cbnz(0, S15, (int32_t)(exit - stop_br));
        *miss_br = a_bcond(C_NE, (int32_t)(exit - miss_br));
        *nul_br = a_cbz(1, 17, (int32_t)(exit - nul_br));
    }
    emit_epilogue(j);
}

static void emit_exit_imm(J *j, uint64_t target) {
    mov_imm(j, T2, target);
    emit_chain(j, T2, 1, target);
}
static void emit_exit_reg(J *j, int r) {
    emit_chain(j, r, 0, 0);
}

static void emit_push(J *j, int hr_val) {
    Emit *e = &j->e;
    em(e, a_ldr_imm(3, T4, R_ST, gpr_off(RSP)));
    em(e, a_addsub_imm(1, 1, 0, T4, T4, 8, 0));
    em(e, a_str_imm(3, hr_val, T4, 0));
    em(e, a_str_imm(3, T4, R_ST, gpr_off(RSP)));
}

// Returns: 0 = translated (continue), 1 = translated and block ends, -1 = not translatable.
static int translate(J *j, const Slot *s) {
    const X86Insn *in = &s->in;
    Emit *e = &j->e;
    int need = s->need;
    uint8_t op = in->op;
    int osz = in->opsize;
    uint64_t next = in->addr + in->len;

    // LOCK prefixes are irrelevant: guest code runs on a single host thread.
    if (in->seg && in->seg != SEG_GS && in->seg != SEG_FS) return -1;

    if (in->map == MAP_1) {
        // ---- ALU forms -------------------------------------------------------------------
        if (op < 0x40 && (op & 7) < 6) {
            int aop = op >> 3, form = op & 7;
            if (!alu_op_supported(aop)) return -1;
            int size = (form & 1) ? osz : 1;
            if (form == 4 || form == 5) {
                load_reg(j, in, T0, RAX, size);
                mov_imm(j, T1, in->imm & size_mask(size));
                emit_alu(j, aop, size, need);
                if (aop != 7) store_reg(j, in, T2, RAX, size);
                return 0;
            }
            if (in->mod != 3) emit_ea(j, in, 1);
            if (form <= 1) { load_rm(j, in, T0, size); load_reg(j, in, T1, in->reg, size); }
            else { load_reg(j, in, T0, in->reg, size); load_rm(j, in, T1, size); }
            // xor r,r zero idiom keeps the same flags semantics through emit_alu
            emit_alu(j, aop, size, need);
            if (aop != 7) { if (form <= 1) store_rm(j, in, T2, size); else store_reg(j, in, T2, in->reg, size); }
            return 0;
        }
        switch (op) {
        case 0x80: case 0x81: case 0x83: {
            int aop = in->reg & 7;
            if (!alu_op_supported(aop)) return -1;
            int size = op == 0x80 ? 1 : osz;
            if (in->mod != 3) emit_ea(j, in, 1);
            load_rm(j, in, T0, size);
            mov_imm(j, T1, in->imm & size_mask(size));
            emit_alu(j, aop, size, need);
            if (aop != 7) store_rm(j, in, T2, size);
            return 0;
        }
        case 0x84: case 0x85: {
            int size = op == 0x84 ? 1 : osz;
            if (in->mod != 3) emit_ea(j, in, 1);
            load_rm(j, in, T0, size);
            load_reg(j, in, T1, in->reg, size);
            emit_alu(j, 4, size, need);
            return 0;
        }
        case 0xA8: case 0xA9: {
            int size = op == 0xA8 ? 1 : osz;
            load_reg(j, in, T0, RAX, size);
            mov_imm(j, T1, in->imm & size_mask(size));
            emit_alu(j, 4, size, need);
            return 0;
        }
        case 0xF6: case 0xF7: {
            int sub = in->reg & 7, size = op == 0xF6 ? 1 : osz;
            if (sub == 0 || sub == 1) {
                if (in->mod != 3) emit_ea(j, in, 1);
                load_rm(j, in, T0, size);
                mov_imm(j, T1, in->imm & size_mask(size));
                emit_alu(j, 4, size, need);
                return 0;
            }
            if (sub == 2) { // NOT
                if (in->mod != 3) emit_ea(j, in, 1);
                load_rm(j, in, T0, size);
                em(e, a_logic_reg(size == 8, 1, 1, T2, XZR, T0, 0, 0)); // mvn
                store_rm(j, in, T2, size);
                return 0;
            }
            if ((sub == 6 || sub == 7) && size >= 4 && j->jc->insn_used < j->jc->insn_cap) { // DIV / IDIV
                int sf = size == 8, sgn = sub == 7;
                if (in->mod != 3) emit_ea(j, in, 1);
                load_rm(j, in, T1, size);                  // divisor
                load_reg(j, in, T0, RAX, 8);
                load_reg(j, in, T2, RDX, 8);
                uint32_t *slow[4];
                int ns = 0;
                if (sf) {
                    if (sgn) {
                        em(e, a_asr(1, T3, T0, 63));
                        em(e, a_addsub_reg(1, 1, 1, XZR, T3, T2, 0, 0)); // rdx must be the sign extension of rax
                        slow[ns++] = e->p; em(e, a_bcond(C_NE, 0));
                        em(e, a_addsub_imm(1, 0, 1, XZR, T1, 1, 0));    // cmn x1, #1 (divisor -1 may overflow)
                        slow[ns++] = e->p; em(e, a_bcond(C_EQ, 0));
                    } else {
                        slow[ns++] = e->p; em(e, a_cbnz(1, T2, 0));      // 128-bit dividend
                    }
                    slow[ns++] = e->p; em(e, a_cbz(1, T1, 0));           // #DE
                    em(e, sgn ? a_sdiv(1, T3, T0, T1) : a_udiv(1, T3, T0, T1));
                    em(e, a_msub(1, T4, T3, T1, T0));
                    em(e, a_str_imm(3, T3, R_ST, gpr_off(RAX)));
                    em(e, a_str_imm(3, T4, R_ST, gpr_off(RDX)));
                } else {
                    em(e, a_mov(0, T0, T0));
                    em(e, a_logic_reg(1, 1, 0, T0, T0, T2, 0, 32));    // x0 = edx:eax
                    slow[ns++] = e->p; em(e, a_cbz(0, T1, 0));
                    if (sgn) {
                        em(e, a_sxt(T1, T1, 32));
                        em(e, a_sdiv(1, T3, T0, T1));
                        em(e, a_sxt(5, T3, 32));
                        em(e, a_addsub_reg(1, 1, 1, XZR, 5, T3, 0, 0)); // quotient must fit in 32 bits
                        slow[ns++] = e->p; em(e, a_bcond(C_NE, 0));
                    } else {
                        em(e, a_mov(0, T1, T1));
                        em(e, a_udiv(1, T3, T0, T1));
                        em(e, a_lsr(1, 5, T3, 32));
                        slow[ns++] = e->p; em(e, a_cbnz(1, 5, 0));
                    }
                    em(e, a_msub(1, T4, T3, T1, T0));
                    store_reg(j, in, T3, RAX, 4);
                    store_reg(j, in, T4, RDX, 4);
                }
                uint32_t *skip = e->p;
                em(e, a_b(0));
                uint32_t *slow_at = e->p;
                X86Insn *copy = &j->jc->insns[j->jc->insn_used++];
                *copy = *in;
                em(e, a_mov(1, 0, R_ST));
                mov_imm(j, 1, (uint64_t)(uintptr_t)copy);
                call_helper(j, (void *)jit_h_exec);           // interpreter raises #DE or handles 128-bit cases
                uint32_t *done = e->p;
                if (!e->overflow) {
                    for (int k = 0; k < ns; k++) {
                        uint32_t ins = *slow[k];
                        int32_t off = (int32_t)(slow_at - slow[k]);
                        if ((ins & 0xFF000000u) == 0x54000000u) *slow[k] = a_bcond((int)(ins & 0xF), off);
                        else *slow[k] = (ins & 0xFF00001Fu) | (((uint32_t)off & 0x7FFFF) << 5);
                    }
                    *skip = a_b((int32_t)(done - skip));
                }
                j->nat = NK_NONE;
                return 0;
            }
            if (sub == 3 && size >= 4) { // NEG
                if (in->mod != 3) emit_ea(j, in, 1);
                load_rm(j, in, T1, size);
                em(e, a_movz(1, T0, 0, 0));
                emit_alu(j, 5, size, need);
                store_rm(j, in, T2, size);
                return 0;
            }
            return -1;
        }
        case 0xFE: case 0xFF: {
            int sub = in->reg & 7, size = op == 0xFE ? 1 : osz;
            if (sub == 0 || sub == 1) { // INC / DEC
                if (need) {
                    em(e, a_mov(1, 0, R_ST));
                    call_helper(j, (void *)jit_h_flags);
                }
                if (in->mod != 3) emit_ea(j, in, 1);
                load_rm(j, in, T0, size);
                em(e, a_movz(1, T1, 1, 0));
                if (size >= 4) {
                    em(e, a_addsub_imm(size == 8, sub, need, T2, T0, 1, 0));
                } else if (need) {
                    int sh = 32 - 8 * size;
                    em(e, a_lsl(0, T3, T0, sh));
                    em(e, a_addsub_reg(0, sub, 1, T2, T3, T1, 0, sh));
                    em(e, a_lsr(0, T2, T2, sh));
                } else {
                    em(e, a_addsub_imm(0, sub, 0, T2, T0, 1, 0));
                }
                if (need) { store_lazy(j, sub ? LF_DEC : LF_INC, size, T0, T1, T2); j->nat = NK_INC; }
                store_rm(j, in, T2, size);
                return 0;
            }
            if (op == 0xFF && (sub == 2 || sub == 4)) { // CALL / JMP indirect
                if (in->mod != 3) emit_ea(j, in, 1);
                load_rm(j, in, T2, 8);
                if (sub == 2) { mov_imm(j, T1, next); emit_push(j, T1); }
                emit_exit_reg(j, T2);
                return 1;
            }
            if (op == 0xFF && sub == 6) {
                if (in->mod != 3) emit_ea(j, in, 1);
                load_rm(j, in, T1, 8);
                emit_push(j, T1);
                return 0;
            }
            return -1;
        }
        case 0x88: case 0x89: case 0x8A: case 0x8B: {
            int size = (op & 1) ? osz : 1;
            if (in->mod != 3) emit_ea(j, in, 1);
            if (op <= 0x89) { load_reg(j, in, T0, in->reg, size); store_rm(j, in, T0, size); }
            else { load_rm(j, in, T0, size); store_reg(j, in, T0, in->reg, size); }
            return 0;
        }
        case 0xC6: case 0xC7: {
            if (in->reg & 7) return -1;
            int size = op == 0xC6 ? 1 : osz;
            if (in->mod != 3) emit_ea(j, in, 1);
            mov_imm(j, T0, in->imm & size_mask(size));
            store_rm(j, in, T0, size);
            return 0;
        }
        case 0x8D: {
            if (in->mod == 3) return -1;
            emit_ea(j, in, 0);
            if (osz == 2) return -1;
            store_reg(j, in, R_EA, in->reg, osz);
            return 0;
        }
        case 0x63: {
            if (in->mod != 3) emit_ea(j, in, 1);
            if (osz == 8) {
                if (in->mod == 3) { load_reg(j, in, T0, in->rm, 4); em(e, a_sxt(T0, T0, 32)); }
                else em(e, a_ldrs_imm(2, T0, R_EA, 0));
                store_reg(j, in, T0, in->reg, 8);
            } else {
                load_rm(j, in, T0, osz);
                store_reg(j, in, T0, in->reg, osz);
            }
            return 0;
        }
        case 0x98:
            load_reg(j, in, T0, RAX, 8);
            if (osz == 8) { em(e, a_sxt(T0, T0, 32)); store_reg(j, in, T0, RAX, 8); }
            else if (osz == 4) { em(e, a_sxt(T0, T0, 16)); store_reg(j, in, T0, RAX, 4); }
            else { em(e, a_sxt(T0, T0, 8)); store_reg(j, in, T0, RAX, 2); }
            return 0;
        case 0x99:
            load_reg(j, in, T0, RAX, 8);
            if (osz == 8) { em(e, a_asr(1, T0, T0, 63)); store_reg(j, in, T0, RDX, 8); }
            else if (osz == 4) { em(e, a_asr(0, T0, T0, 31)); store_reg(j, in, T0, RDX, 4); }
            else return -1;
            return 0;
        case 0x90:
            if (in->rex & 1) return -1;
            return 0;
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            if (in->p66) return -1;
            load_reg(j, in, T1, (op & 7) | ((in->rex & 1) << 3), 8);
            emit_push(j, T1);
            return 0;
        case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
            if (in->p66) return -1;
            int r = (op & 7) | ((in->rex & 1) << 3);
            em(e, a_ldr_imm(3, T4, R_ST, gpr_off(RSP)));
            em(e, a_ldr_imm(3, T1, T4, 0));
            em(e, a_addsub_imm(1, 0, 0, T4, T4, 8, 0));
            em(e, a_str_imm(3, T4, R_ST, gpr_off(RSP)));
            em(e, a_str_imm(3, T1, R_ST, gpr_off(r)));
            return 0;
        }
        case 0x68: case 0x6A:
            if (in->p66) return -1;
            mov_imm(j, T1, in->imm);
            emit_push(j, T1);
            return 0;
        case 0x69: case 0x6B: {
            if (need || osz == 2) return -1;
            if (in->mod != 3) emit_ea(j, in, 1);
            load_rm(j, in, T0, osz);
            mov_imm(j, T1, in->imm);
            em(e, a_mul(osz == 8, T2, T0, T1));
            store_reg(j, in, T2, in->reg, osz);
            return 0;
        }
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            mov_imm(j, T0, in->imm & 0xFF);
            store_reg(j, in, T0, (op & 7) | ((in->rex & 1) << 3), 1);
            return 0;
        case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            mov_imm(j, T0, in->imm & size_mask(osz));
            store_reg(j, in, T0, (op & 7) | ((in->rex & 1) << 3), osz);
            return 0;
        case 0xC0: case 0xC1: case 0xD0: case 0xD1: {
            int sub = in->reg & 7;
            if (sub != 4 && sub != 5 && sub != 7 && sub != 6) return -1;
            int size = (op & 1) ? osz : 1;
            unsigned cnt = (op <= 0xC1) ? (unsigned)(in->imm & 0xFF) : 1u;
            cnt &= size == 8 ? 63 : 31;
            if (cnt == 0) return 0; // no-op, flags untouched
            if (size < 4) return -1;
            if (in->mod != 3) emit_ea(j, in, 1);
            load_rm(j, in, T0, size);
            int sf = size == 8;
            if (sub == 4 || sub == 6) em(e, a_lsl(sf, T2, T0, (int)cnt));
            else if (sub == 5) em(e, a_lsr(sf, T2, T0, (int)cnt));
            else em(e, a_asr(sf, T2, T0, (int)cnt));
            store_rm(j, in, T2, size);
            if (need) {
                mov_imm(j, T1, cnt);
                store_lazy(j, (sub == 5) ? LF_SHR : (sub == 7) ? LF_SAR : LF_SHL, size, T0, T1, T2);
                em(e, a_logic_reg(sf, 3, 0, XZR, T2, T2, 0, 0)); // tst
                j->nat = NK_SHIFT;
            }
            return 0;
        }
        case 0xD3: { // SHL/SHR/SAR r/m, CL (only when the resulting flags are dead)
            int sub = in->reg & 7, size = osz;
            if (need || size < 4 || (sub != 4 && sub != 5 && sub != 6 && sub != 7)) return -1;
            if (in->mod != 3) emit_ea(j, in, 1);
            load_rm(j, in, T0, size);
            load_reg(j, in, T1, RCX, 8);   // LSLV/LSRV/ASRV mask the count like x86 does
            int sf = size == 8;
            em(e, a_shiftv(sf, sub == 5 ? 1 : sub == 7 ? 2 : 0, T2, T0, T1));
            store_rm(j, in, T2, size);
            return 0;
        }
        case 0xC3:
            em(e, a_ldr_imm(3, T4, R_ST, gpr_off(RSP)));
            em(e, a_ldr_imm(3, T2, T4, 0));
            em(e, a_addsub_imm(1, 0, 0, T4, T4, 8, 0));
            em(e, a_str_imm(3, T4, R_ST, gpr_off(RSP)));
            emit_exit_reg(j, T2);
            return 1;
        case 0xC2:
            em(e, a_ldr_imm(3, T4, R_ST, gpr_off(RSP)));
            em(e, a_ldr_imm(3, T2, T4, 0));
            add_imm(j, T4, T4, 8 + (int64_t)in->imm);
            em(e, a_str_imm(3, T4, R_ST, gpr_off(RSP)));
            emit_exit_reg(j, T2);
            return 1;
        case 0xC9: // LEAVE
            em(e, a_ldr_imm(3, T4, R_ST, gpr_off(RBP)));
            em(e, a_ldr_imm(3, T1, T4, 0));
            em(e, a_addsub_imm(1, 0, 0, T4, T4, 8, 0));
            em(e, a_str_imm(3, T4, R_ST, gpr_off(RSP)));
            em(e, a_str_imm(3, T1, R_ST, gpr_off(RBP)));
            return 0;
        case 0xE8:
            mov_imm(j, T1, next);
            emit_push(j, T1);
            emit_exit_imm(j, next + in->imm);
            return 1;
        case 0xE9: case 0xEB:
            emit_exit_imm(j, next + in->imm);
            return 1;
        default:
            break;
        }
        if (op >= 0x70 && op <= 0x7F) goto jcc;
        return -1;
    }

    if (in->map == MAP_0F) {
        if (op >= 0x80 && op <= 0x8F) goto jcc;
        if (op >= 0x40 && op <= 0x4F) { // CMOVcc
            if (osz == 2) return -1;
            int c = emit_cond(j, op & 0xF); // may call a helper: evaluate before loading operands
            if (in->mod != 3) emit_ea(j, in, 1);
            load_rm(j, in, T1, osz);
            load_reg(j, in, T0, in->reg, osz);
            em(e, a_csel(osz == 8, T2, T1, T0, c));
            store_reg(j, in, T2, in->reg, osz);
            return 0;
        }
        if (op >= 0x90 && op <= 0x9F) { // SETcc
            int c = emit_cond(j, op & 0xF);
            em(e, a_cset(0, T2, c));
            if (in->mod != 3) emit_ea(j, in, 1);
            store_rm(j, in, T2, 1);
            return 0;
        }
        switch (op) {
        case 0x1F: case 0x1E: case 0x18: case 0x0D:
            return 0;
        case 0xB6: case 0xB7: case 0xBE: case 0xBF: {
            int ssz = (op & 1) ? 2 : 1;
            if (osz == 2) return -1;
            if (in->mod != 3) emit_ea(j, in, 1);
            load_rm(j, in, T0, ssz);
            if (op >= 0xBE) em(e, a_sxt(T0, T0, 8 * ssz));
            store_reg(j, in, T0, in->reg, osz);
            return 0;
        }
        case 0xAF: {
            if (need || osz == 2) return -1;
            if (in->mod != 3) emit_ea(j, in, 1);
            load_rm(j, in, T1, osz);
            load_reg(j, in, T0, in->reg, osz);
            em(e, a_mul(osz == 8, T2, T0, T1));
            store_reg(j, in, T2, in->reg, osz);
            return 0;
        }
        case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
            int r = (op & 7) | ((in->rex & 1) << 3);
            if (osz == 2) return -1;
            load_reg(j, in, T0, r, 8);
            em(e, a_rev(osz == 8, T0, T0));
            store_reg(j, in, T0, r, osz);
            return 0;
        }
        default:
            break;
        }
        // ---- SSE subset -------------------------------------------------------------------
        int pfx = in->rep == 0xF3 ? 2 : in->rep == 0xF2 ? 3 : in->p66 ? 1 : 0;
        int xr = in->reg;
        switch (op) {
        case 0x10: case 0x11: case 0x28: case 0x29: case 0x6F: case 0x7F: {
            if ((op == 0x6F || op == 0x7F) && pfx != 1 && pfx != 2) return -1;
            if ((op == 0x28 || op == 0x29) && pfx > 1) return -1;
            int load = (op == 0x10 || op == 0x28 || op == 0x6F);
            int fsz = (op == 0x10 || op == 0x11) ? (pfx == 2 ? 2 : pfx == 3 ? 3 : 4) : 4;
            if (in->mod != 3) {
                emit_ea(j, in, 1);
                if (load) {
                    em(e, a_ldr_fp(fsz, 0, R_EA, 0));          // narrower loads zero the rest of v0
                    em(e, a_str_fp(4, 0, R_ST, xmm_off(xr)));
                } else {
                    em(e, a_ldr_fp(fsz, 0, R_ST, xmm_off(xr)));
                    em(e, a_str_fp(fsz, 0, R_EA, 0));
                }
            } else {
                int src = load ? in->rm : xr, dst = load ? xr : in->rm;
                em(e, a_ldr_fp(fsz, 0, R_ST, xmm_off(src)));
                em(e, a_str_fp(fsz, 0, R_ST, xmm_off(dst)));     // scalar reg moves keep upper bits
            }
            return 0;
        }
        case 0x57: case 0xEF: case 0x54: case 0xDB: case 0x56: case 0xEB: case 0x55: case 0xDF: {
            if (op >= 0xD0 && pfx != 1) return -1;
            if (op < 0x60 && pfx > 1) return -1;
            int lop = (op == 0x57 || op == 0xEF) ? 3 : (op == 0x54 || op == 0xDB) ? 0 : (op == 0x56 || op == 0xEB) ? 2 : 1;
            if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(4, 1, R_EA, 0)); }
            else em(e, a_ldr_fp(4, 1, R_ST, xmm_off(in->rm)));
            em(e, a_ldr_fp(4, 0, R_ST, xmm_off(xr)));
            if (lop == 1) em(e, a_vlogic(1, 0, 1, 0)); // ANDN: ~dst & src  => BIC src, dst
            else em(e, a_vlogic(lop, 0, 0, 1));
            em(e, a_str_fp(4, 0, R_ST, xmm_off(xr)));
            return 0;
        }
        case 0x58: case 0x59: case 0x5C: case 0x5E: case 0x51: case 0x5D: case 0x5F: {
            if (pfx < 2) {
                if (op == 0x51 || op == 0x5D || op == 0x5F) return -1;
                // packed single (pfx 0) / double (pfx 1)
                if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(4, 1, R_EA, 0)); }
                else em(e, a_ldr_fp(4, 1, R_ST, xmm_off(in->rm)));
                em(e, a_ldr_fp(4, 0, R_ST, xmm_off(xr)));
                int vop = op == 0x58 ? 0 : op == 0x5C ? 1 : op == 0x59 ? 2 : 3;
                em(e, a_vfp(pfx, vop, 0, 0, 1));
                em(e, a_str_fp(4, 0, R_ST, xmm_off(xr)));
                return 0;
            }
            int dbl = pfx == 3, fsz = dbl ? 3 : 2;
            if (op == 0x5D || op == 0x5F) return -1; // x86 MIN/MAX NaN semantics differ
            if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(fsz, 1, R_EA, 0)); }
            else em(e, a_ldr_fp(fsz, 1, R_ST, xmm_off(in->rm)));
            if (op == 0x51) em(e, a_fsqrt(dbl, 0, 1));
            else {
                em(e, a_ldr_fp(fsz, 0, R_ST, xmm_off(xr)));
                int fop = op == 0x58 ? 2 : op == 0x59 ? 0 : op == 0x5C ? 3 : 1;
                em(e, a_fp2(dbl, fop, 0, 0, 1));
            }
            em(e, a_str_fp(fsz, 0, R_ST, xmm_off(xr)));
            return 0;
        }
        case 0x5A: {
            if (pfx < 2) return -1;
            int from_dbl = pfx == 3;
            if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(from_dbl ? 3 : 2, 1, R_EA, 0)); }
            else em(e, a_ldr_fp(from_dbl ? 3 : 2, 1, R_ST, xmm_off(in->rm)));
            if (from_dbl) { em(e, a_fcvt_d2s(0, 1)); em(e, a_str_fp(2, 0, R_ST, xmm_off(xr))); }
            else { em(e, a_fcvt_s2d(0, 1)); em(e, a_str_fp(3, 0, R_ST, xmm_off(xr))); }
            return 0;
        }
        case 0x2A: {
            if (pfx < 2) return -1;
            int isz = (in->rex & 8) ? 8 : 4, dbl = pfx == 3;
            if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_imm(size_log2(isz), T0, R_EA, 0)); }
            else load_reg(j, in, T0, in->rm, isz);
            em(e, a_scvtf(isz == 8, dbl, 0, T0));
            em(e, a_str_fp(dbl ? 3 : 2, 0, R_ST, xmm_off(xr)));
            return 0;
        }
        case 0x2C: {
            if (pfx < 2) return -1;
            int dsz = (in->rex & 8) ? 8 : 4, dbl = pfx == 3;
            if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(dbl ? 3 : 2, 0, R_EA, 0)); }
            else em(e, a_ldr_fp(dbl ? 3 : 2, 0, R_ST, xmm_off(in->rm)));
            // x86 returns the "integer indefinite" value for NaN and out-of-range inputs,
            // ARM64 saturates: patch the result up.
            em(e, a_fcvtzs(1, dbl, T0, 0));
            em(e, a_movz(1, T2, 0x8000, 3));                       // INT64_MIN
            if (dsz == 4) {
                em(e, a_sxt(T1, T0, 32));
                em(e, a_addsub_reg(1, 1, 1, XZR, T1, T0, 0, 0));    // cmp x1, x0
                em(e, a_movz(0, T2, 0x8000, 1));                   // 0x80000000
                em(e, a_csel(0, T0, T2, T0, C_NE));
            } else {
                em(e, a_addsub_imm(1, 1, 0, T3, T2, 1, 0));        // INT64_MAX
                em(e, a_addsub_reg(1, 1, 1, XZR, T0, T3, 0, 0));
                em(e, a_csel(1, T0, T2, T0, C_EQ));
            }
            em(e, a_fcmp(dbl, 0, 0));
            em(e, a_csel(dsz == 8, T0, T2, T0, C_VS));
            j->nat = NK_NONE;
            store_reg(j, in, T0, xr, dsz);
            return 0;
        }
        case 0x2E: case 0x2F: {
            if (pfx > 1) return -1;
            int dbl = pfx == 1, fsz = dbl ? 3 : 2;
            if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(fsz, 1, R_EA, 0)); }
            else em(e, a_ldr_fp(fsz, 1, R_ST, xmm_off(in->rm)));
            em(e, a_ldr_fp(fsz, 0, R_ST, xmm_off(xr)));
            em(e, a_fcmp(dbl, 0, 1));
            // materialise CF/ZF/PF into eflags (NZCV is preserved for the next jcc)
            em(e, a_cset(0, T1, C_LT));
            em(e, a_cset(0, T2, C_EQ));
            em(e, a_cset(0, T3, C_VS));
            em(e, a_logic_reg(0, 1, 0, T2, T2, T3, 0, 0));
            em(e, a_ldr_imm(3, T4, R_ST, ST_OFF(eflags)));
            mov_imm(j, S14, ~(uint64_t)F_ARITH);
            em(e, a_logic_reg(1, 0, 0, T4, T4, S14, 0, 0));
            em(e, a_logic_reg(1, 1, 0, T4, T4, T1, 0, 0));
            em(e, a_logic_reg(1, 1, 0, T4, T4, T3, 0, 2));
            em(e, a_logic_reg(1, 1, 0, T4, T4, T2, 0, 6));
            em(e, a_str_imm(3, T4, R_ST, ST_OFF(eflags)));
            em(e, a_str_imm(2, XZR, R_ST, ST_OFF(lf_op)));
            j->nat = NK_FCMP;
            return 0;
        }
        case 0x6E: case 0x7E: {
            if (pfx == 2 && op == 0x7E) { // MOVQ xmm, xmm/m64
                if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(3, 0, R_EA, 0)); }
                else em(e, a_ldr_fp(3, 0, R_ST, xmm_off(in->rm)));
                em(e, a_str_fp(4, 0, R_ST, xmm_off(xr)));
                return 0;
            }
            if (pfx != 1) return -1;
            int sz = (in->rex & 8) ? 8 : 4;
            if (op == 0x6E) {
                if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(sz == 8 ? 3 : 2, 0, R_EA, 0)); }
                else { load_reg(j, in, T0, in->rm, sz); em(e, a_fmov_from_gpr(sz == 8, 0, T0)); }
                em(e, a_str_fp(4, 0, R_ST, xmm_off(xr)));
            } else {
                em(e, a_ldr_imm(size_log2(sz), T0, R_ST, xmm_off(xr)));
                if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_str_imm(size_log2(sz), T0, R_EA, 0)); }
                else store_reg(j, in, T0, in->rm, sz);
            }
            return 0;
        }
        case 0xFC: case 0xFD: case 0xFE: case 0xD4: case 0xF8: case 0xF9: case 0xFA: case 0xFB:
        case 0x74: case 0x75: case 0x76: case 0x64: case 0x65: case 0x66: case 0x14: case 0x15: case 0x6C: case 0x6D: {
            if (pfx != 1 && !((op == 0x14 || op == 0x15) && pfx == 0)) return -1;
            if (in->mod != 3) { emit_ea(j, in, 1); em(e, a_ldr_fp(4, 1, R_EA, 0)); }
            else em(e, a_ldr_fp(4, 1, R_ST, xmm_off(in->rm)));
            em(e, a_ldr_fp(4, 0, R_ST, xmm_off(xr)));
            uint32_t ins;
            switch (op) {
            case 0xFC: ins = a_vadd(0, 0, 0, 1); break;
            case 0xFD: ins = a_vadd(1, 0, 0, 1); break;
            case 0xFE: ins = a_vadd(2, 0, 0, 1); break;
            case 0xD4: ins = a_vadd(3, 0, 0, 1); break;
            case 0xF8: ins = a_vsub(0, 0, 0, 1); break;
            case 0xF9: ins = a_vsub(1, 0, 0, 1); break;
            case 0xFA: ins = a_vsub(2, 0, 0, 1); break;
            case 0xFB: ins = a_vsub(3, 0, 0, 1); break;
            case 0x74: ins = a_vcmeq(0, 0, 0, 1); break;
            case 0x75: ins = a_vcmeq(1, 0, 0, 1); break;
            case 0x76: ins = a_vcmeq(2, 0, 0, 1); break;
            case 0x64: ins = a_vcmgt(0, 0, 0, 1); break;
            case 0x65: ins = a_vcmgt(1, 0, 0, 1); break;
            case 0x66: ins = a_vcmgt(2, 0, 0, 1); break;
            case 0x14: ins = a_vzip1(pfx == 1 ? 3 : 2, 0, 0, 1); break;   // UNPCKLPD / UNPCKLPS
            case 0x15: ins = a_vzip2(pfx == 1 ? 3 : 2, 0, 0, 1); break;   // UNPCKHPD / UNPCKHPS
            case 0x6C: ins = a_vzip1(3, 0, 0, 1); break;                  // PUNPCKLQDQ
            default: ins = a_vzip2(3, 0, 0, 1); break;                    // PUNPCKHQDQ
            }
            em(e, ins);
            em(e, a_str_fp(4, 0, R_ST, xmm_off(xr)));
            return 0;
        }
        case 0xD6: {
            if (pfx != 1 || in->mod == 3) return -1;
            emit_ea(j, in, 1);
            em(e, a_ldr_fp(3, 0, R_ST, xmm_off(xr)));
            em(e, a_str_fp(3, 0, R_EA, 0));
            return 0;
        }
        default:
            return -1;
        }
    }
    return -1;

jcc: {
        int cc = op & 0xF;
        uint64_t target = next + in->imm;
        int c = emit_cond(j, cc);
        uint32_t *br = e->p;
        em(e, a_bcond(c, 0));
        emit_exit_imm(j, next);
        uint32_t *taken = e->p;
        if (!e->overflow) *br = a_bcond(c, (int32_t)(taken - br));
        emit_exit_imm(j, target);
        return 1;
    }
}

// Instructions that may be executed through an interpreter callout from inside a block:
// anything that does not transfer control.
static int callout_ok(const X86Insn *in) {
    if (in->map == MAP_1) {
        uint8_t op = in->op;
        if (op >= 0x70 && op <= 0x7F) return 0;
        switch (op) {
        case 0xC2: case 0xC3: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE8: case 0xE9: case 0xEA: case 0xEB: case 0xF4:
            return 0;
        case 0xFF:
            return (in->reg & 7) < 2 || (in->reg & 7) == 6;
        default:
            return 1;
        }
    }
    if (in->map == MAP_0F) {
        if (in->op >= 0x80 && in->op <= 0x8F) return 0;
        if (in->op == 0x05 || in->op == 0x0B || in->op == 0x07 || in->op == 0x34 || in->op == 0x35) return 0;
    }
    return 1;
}

// ---- block compilation -------------------------------------------------------------------------------

static void cache_reset(JitCache *jc) {
    jc->used = 0;
    memset(jc->table, 0, sizeof(Block *) * (jc->table_mask + 1));
    jc->pool_used = 0;
    jc->insn_used = 0;
    memset(jc->fast, 0, sizeof jc->fast);
    jc->flushes++;
}

static Block *new_block(JitCache *jc, uint64_t rip) {
    if (jc->pool_used == jc->pool_cap) cache_reset(jc);
    Block *b = &jc->pool[jc->pool_used++];
    b->rip = rip;
    b->code = NULL;
    size_t h = (size_t)(rip >> 1) & jc->table_mask;
    b->next = jc->table[h];
    jc->table[h] = b;
    return b;
}

static Block *lookup(JitCache *jc, uint64_t rip) {
    for (Block *b = jc->table[(size_t)(rip >> 1) & jc->table_mask]; b; b = b->next)
        if (b->rip == rip) return b;
    return NULL;
}

static int decode_block(uint64_t rip, Slot *slots) {
    int n = 0;
    uint64_t pc = rip;
    while (n < MAX_BLOCK_INSNS) {
        Slot *s = &slots[n];
        if (x86_decode((const uint8_t *)(uintptr_t)pc, pc, &s->in) != 0) break;
        const X86Insn *in = &s->in;
        s->fread = is_cc_reader(in);
        s->fwrite = 0;
        if (in->map == MAP_1) {
            uint8_t op = in->op;
            if (op < 0x40 && (op & 7) < 6) s->fwrite = 1;
            else if (op == 0x80 || op == 0x81 || op == 0x83 || op == 0x84 || op == 0x85 || op == 0xA8 || op == 0xA9) s->fwrite = 1;
            else if ((op == 0xF6 || op == 0xF7) && ((in->reg & 7) != 2)) s->fwrite = 1;
            else if ((op == 0xFE || op == 0xFF) && (in->reg & 7) < 2) s->fwrite = 2;
            else if (op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3) s->fwrite = 1;
            else if (op == 0xC0 || op == 0xC1) s->fwrite = (in->imm & ((op == 0xC1 && in->opsize == 8) ? 63 : 31)) != 0;
            else if (op == 0x69 || op == 0x6B) s->fwrite = 1;
        } else if (in->map == MAP_0F) {
            if (in->op == 0xAF || in->op == 0x2E || in->op == 0x2F) s->fwrite = 1;
        }
        n++;
        pc += in->len;
        // stop after control flow
        if (in->map == MAP_1) {
            uint8_t op = in->op;
            if ((op >= 0x70 && op <= 0x7F) || op == 0xE8 || op == 0xE9 || op == 0xEB || op == 0xC3 || op == 0xC2 ||
                op == 0xCC || op == 0xCD || (op >= 0xE0 && op <= 0xE3) || (op == 0xFF && ((in->reg & 7) >= 2 && (in->reg & 7) <= 5)))
                break;
        } else if (in->map == MAP_0F && in->op >= 0x80 && in->op <= 0x8F) {
            break;
        } else if (in->map == MAP_0F && (in->op == 0x05 || in->op == 0x0B)) {
            break;
        }
    }
    // backwards flag liveness: flags are assumed live at the block exit
    int live = 1;
    for (int i = n - 1; i >= 0; i--) {
        Slot *s = &slots[i];
        if (s->fwrite == 1) { s->need = live; live = s->fread; }
        else if (s->fwrite == 2) { s->need = live; live = live ? 1 : s->fread; }
        else { s->need = 0; live = live || s->fread; }
    }
    return n;
}

static BlockFn compile(JitCache *jc, uint64_t rip, X86State *st) {
    Slot slots[MAX_BLOCK_INSNS];
    int n = decode_block(rip, slots);
    if (n == 0) return NULL;
    if (jc->size - jc->used < CODE_RESERVE || jc->insn_cap - jc->insn_used < MAX_BLOCK_INSNS) cache_reset(jc);

    J j;
    memset(&j, 0, sizeof j);
    j.st = st;
    j.jc = jc;
    jitmem_begin_write();
    uint32_t *start = (uint32_t *)(jc->rw + jc->used);
    j.e.start = j.e.p = start;
    j.e.end = (uint32_t *)(jc->rw + jc->size);
    Emit *e = &j.e;

    em(e, a_stp_pre(29, 30, SP_REG, -16));
    em(e, a_stp_pre(19, 20, SP_REG, -16));
    em(e, a_mov(1, R_ST, 0));
    uint32_t *count_at = e->p;
    em(e, a_ldr_imm(3, T0, R_ST, ST_OFF(icount_jit)));
    em(e, a_addsub_imm(1, 0, 0, T0, T0, 0, 0)); // patched with the instruction count
    em(e, a_str_imm(3, T0, R_ST, ST_OFF(icount_jit)));

    int done = 0, count = 0;
    for (int i = 0; i < n && !done; i++) {
        uint32_t *mark = e->p;
        int saved_nat = j.nat;
        int r = translate(&j, &slots[i]);
        if (r < 0 && !e->overflow && callout_ok(&slots[i].in) && jc->insn_used < jc->insn_cap) {
            // run this instruction through the interpreter without leaving the block
            e->p = mark;
            j.nat = saved_nat;
            X86Insn *copy = &jc->insns[jc->insn_used++];
            *copy = slots[i].in;
            em(e, a_mov(1, 0, R_ST));
            mov_imm(&j, 1, (uint64_t)(uintptr_t)copy);
            call_helper(&j, (void *)jit_h_exec);
            r = 0;
        }
        if (r < 0 || e->overflow) {
            e->p = mark;
            e->overflow = 0;
            j.nat = saved_nat;
            break;
        }
        count++;
        if (r == 1) done = 1;
    }
    if (count == 0) { jitmem_end_write(); return NULL; }
    count_at[1] = a_addsub_imm(1, 0, 0, T0, T0, (uint32_t)count, 0);
    if (!done) {
        uint64_t next = slots[count - 1].in.addr + slots[count - 1].in.len;
        emit_exit_imm(&j, next);
    }
    if (e->overflow) { jitmem_end_write(); cache_reset(jc); return NULL; }
    size_t bytes = (size_t)((uint8_t *)e->p - (uint8_t *)start);
    size_t off = jc->used;
    jc->used = (jc->used + bytes + 15) & ~(size_t)15;
    jitmem_end_write();
    jitmem_flush_icache(jc->rx + off, bytes);
    jc->blocks++;
    return (BlockFn)(void *)(jc->rx + off);
}

JitCache *jit_create(SoulProcess *p) {
    if (!jitmem_ready()) return NULL;
    JitCache *jc = calloc(1, sizeof *jc);
    jc->p = p;
    jc->rw = jitmem_rw();
    jc->rx = jitmem_rx();
    jc->size = jitmem_size();
    jc->table_mask = (1u << 16) - 1;
    jc->table = calloc(jc->table_mask + 1, sizeof(Block *));
    jc->pool_cap = 1u << 18;
    jc->pool = calloc(jc->pool_cap, sizeof(Block));
    jc->insn_cap = 1u << 16;
    jc->insns = calloc(jc->insn_cap, sizeof(X86Insn));
    return jc;
}

void jit_destroy(JitCache *jc) {
    if (!jc) return;
    free(jc->table);
    free(jc->pool);
    free(jc->insns);
    free(jc);
}

void jit_flush(JitCache *jc) { if (jc) cache_reset(jc); }

int jit_run(JitCache *jc, X86State *st) {
    uint64_t rip = st->rip;
    size_t fi = (size_t)(rip >> 1) & (FAST_SIZE - 1);
    BlockFn code;
    if (jc->fast[fi].rip == rip && jc->fast[fi].code) {
        code = jc->fast[fi].code;
    } else {
        Block *b = lookup(jc, rip);
        if (!b) {
            b = new_block(jc, rip);
            b->code = compile(jc, rip, st);
            // compile may have reset the cache; re-create the entry if so
            if (lookup(jc, rip) != b) { Block *nb = new_block(jc, rip); nb->code = b->code; b = nb; }
        }
        if (!b->code) return 0;
        jc->fast[fi].rip = rip;
        jc->fast[fi].code = b->code;
        code = b->code;
    }
    st->thread->block_rip = rip;
    code(st);
    st->thread->block_rip = 0;
    return 1;
}

void jit_stats(JitCache *jc, SoulStats *s) {
    if (!jc) return;
    s->jit_blocks = jc->blocks;
    s->jit_code_bytes = jc->used;
}

#else // non-ARM64 hosts: interpreter only

JitCache *jit_create(SoulProcess *p) { (void)p; return NULL; }
void jit_destroy(JitCache *j) { (void)j; }
void jit_flush(JitCache *j) { (void)j; }
int jit_run(JitCache *j, X86State *st) { (void)j; (void)st; return 0; }
void jit_stats(JitCache *j, SoulStats *s) { (void)j; (void)s; }

#endif
