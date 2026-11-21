// SoulCore - x86-64 interpreter entry points.
#pragma once
#include "cpu.h"
#include "decode.h"

// Decode and execute a single instruction at st->rip.
void x86_interp_step(X86State *st);
// Execute an already decoded instruction (st->rip must equal in->addr).
void x86_exec(X86State *st, const X86Insn *in);

// sse.c / x87.c: return 1 when handled.
int sse_exec(X86State *st, const X86Insn *in);
int x87_exec(X86State *st, const X86Insn *in);
void x87_reset(X86State *st);

// Effective address (including FS/GS base).
static inline uint64_t ea_noseg(const X86State *st, const X86Insn *in) {
    uint64_t a;
    if (in->ea_base == EA_RIP) {
        a = in->addr + in->len + (int64_t)in->disp;
    } else {
        a = (uint64_t)(int64_t)in->disp;
        if (in->ea_base != EA_NONE) a += st->gpr[in->ea_base];
        if (in->ea_index != EA_NONE) a += st->gpr[in->ea_index] << in->ea_scale;
    }
    if (in->adsize32) a = (uint32_t)a;
    return a;
}
static inline uint64_t ea_addr(const X86State *st, const X86Insn *in) {
    uint64_t a = ea_noseg(st, in);
    if (in->seg == SEG_FS) a += st->fs_base;
    else if (in->seg == SEG_GS) a += st->gs_base;
    if (g_mode32) a = (uint32_t)a;
    return a;
}

static inline uint64_t reg_rd(const X86State *st, const X86Insn *in, int r, int size) {
    if (size == 1) {
        if (!in->rex && r >= 4 && r < 8) return (st->gpr[r - 4] >> 8) & 0xFF;
        return st->gpr[r] & 0xFF;
    }
    return st->gpr[r] & size_mask(size);
}
static inline void reg_wr(X86State *st, const X86Insn *in, int r, int size, uint64_t v) {
    switch (size) {
    case 1:
        if (!in->rex && r >= 4 && r < 8) st->gpr[r - 4] = (st->gpr[r - 4] & ~0xFF00ull) | ((v & 0xFF) << 8);
        else st->gpr[r] = (st->gpr[r] & ~0xFFull) | (v & 0xFF);
        break;
    case 2: st->gpr[r] = (st->gpr[r] & ~0xFFFFull) | (v & 0xFFFF); break;
    case 4: st->gpr[r] = (uint32_t)v; break;
    default: st->gpr[r] = v; break;
    }
}

// Stack slots are 8 bytes in 64-bit mode and 4 bytes in 32-bit mode.
static inline void push64(X86State *st, uint64_t v) {
    if (g_mode32) { st->gpr[RSP] = (uint32_t)(st->gpr[RSP] - 4); mem_wr(st->gpr[RSP], 4, v); return; }
    st->gpr[RSP] -= 8;
    mem_wr(st->gpr[RSP], 8, v);
}
static inline uint64_t pop64(X86State *st) {
    if (g_mode32) { uint64_t v = mem_rd(st->gpr[RSP], 4); st->gpr[RSP] = (uint32_t)(st->gpr[RSP] + 4); return v; }
    uint64_t v = mem_rd(st->gpr[RSP], 8);
    st->gpr[RSP] += 8;
    return v;
}
static inline int stack_slot(void) { return g_mode32 ? 4 : 8; }
