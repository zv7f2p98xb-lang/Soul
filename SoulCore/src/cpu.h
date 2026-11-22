// SoulCore - x86-64 guest CPU state shared by the interpreter and the ARM64 JIT.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };

#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_TF 0x0100u
#define F_IF 0x0200u
#define F_DF 0x0400u
#define F_OF 0x0800u
#define F_ARITH (F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)

// Lazy flag producers. When lf_op != LF_NONE the arithmetic bits of
// eflags must be derived from lf_dst/lf_src/lf_res/lf_size.
enum {
    LF_NONE = 0,
    LF_ADD,
    LF_SUB,
    LF_LOGIC,
    LF_INC, // CF comes from eflags
    LF_DEC, // CF comes from eflags
    LF_SHL,
    LF_SHR,
    LF_SAR,
};

typedef union {
    uint8_t b[16];
    uint16_t w[8];
    uint32_t d[4];
    uint64_t q[2];
    int8_t sb[16];
    int16_t sw[8];
    int32_t sd[4];
    int64_t sq[2];
    float f[4];
    double fd[2];
} Xmm;

typedef struct SoulThread SoulThread;

typedef struct X86State {
    uint64_t gpr[16];
    uint64_t rip;
    uint64_t eflags;
    uint64_t lf_dst, lf_src, lf_res;
    uint32_t lf_op, lf_size;
    uint64_t fs_base, gs_base;
    uint64_t gbase;          // host address of guest address 0 (32-bit programs; 0 for 64-bit ones)
    uint64_t icount_jit;
    uint64_t icount_interp;
    uint64_t _pad0;
    Xmm xmm[16] __attribute__((aligned(16)));
    uint32_t mxcsr;
    uint32_t ftop;
    double fpr[8];
    uint16_t fcw, fsw;
    uint8_t ftag[8];
    volatile int32_t stop_req;
    int32_t exit_code;
    SoulThread *thread;
} __attribute__((aligned(16))) X86State;

#define ST_OFF(field) ((uint32_t)offsetof(X86State, field))

static inline uint64_t size_mask(int size) {
    return size == 8 ? ~0ull : ((1ull << (size * 8)) - 1);
}
static inline uint64_t sign_bit(int size) { return 1ull << (size * 8 - 1); }
static inline int64_t sext(uint64_t v, int size) {
    switch (size) {
    case 1: return (int8_t)v;
    case 2: return (int16_t)v;
    case 4: return (int32_t)v;
    default: return (int64_t)v;
    }
}

// Guest memory: 64-bit programs are identity-mapped (g_gbase = 0). 32-bit programs live
// in a 4 GB window of the host address space starting at g_gbase (iOS keeps the low
// 4 GB of every process unmapped), so guest address a is host address g_gbase + a.
extern uint64_t g_gbase;
extern int g_mode32;
// (branch-free so that a NULL guest pointer stays NULL without tripping -Wnonnull)
static inline void *G2H(uint64_t a) { return (void *)(uintptr_t)(a + (g_gbase & (0 - (uint64_t)(a != 0)))); }
#define H2G(p) ((p) ? (uint64_t)(uintptr_t)(p) - g_gbase : 0)
static inline uint64_t mem_rd(uint64_t a, int size) {
    a += g_gbase;
    switch (size) {
    case 1: return *(volatile uint8_t *)(uintptr_t)a;
    case 2: { uint16_t v; memcpy(&v, (void *)(uintptr_t)a, 2); return v; }
    case 4: { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
    default: { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
    }
}
static inline void mem_wr(uint64_t a, int size, uint64_t v) {
    a += g_gbase;
    switch (size) {
    case 1: *(volatile uint8_t *)(uintptr_t)a = (uint8_t)v; break;
    case 2: { uint16_t x = (uint16_t)v; memcpy((void *)(uintptr_t)a, &x, 2); break; }
    case 4: { uint32_t x = (uint32_t)v; memcpy((void *)(uintptr_t)a, &x, 4); break; }
    default: memcpy((void *)(uintptr_t)a, &v, 8); break;
    }
}

// flags.c
uint64_t x86_flags(X86State *st);          // resolve lazy flags, returns eflags
int x86_cond(X86State *st, int cc);        // evaluate Jcc/SETcc/CMOVcc condition
static inline void set_lazy(X86State *st, uint32_t op, uint64_t dst, uint64_t src, uint64_t res, int size) {
    st->lf_op = op;
    st->lf_size = (uint32_t)size;
    st->lf_dst = dst;
    st->lf_src = src;
    st->lf_res = res;
}
void x86_set_flags(X86State *st, uint64_t res, int size, int cf, int of, int af); // eager full update
