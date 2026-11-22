// SoulCore - x86-64 integer interpreter.
#include "interp.h"
#include "soul_internal.h"
#include <time.h>

typedef struct { int mem; int reg; uint64_t addr; } Loc;

static inline Loc loc_e(X86State *st, const X86Insn *in) {
    Loc l;
    if (in->mod == 3) { l.mem = 0; l.reg = in->rm; l.addr = 0; }
    else { l.mem = 1; l.reg = 0; l.addr = ea_addr(st, in); }
    return l;
}
static inline uint64_t rd_loc(X86State *st, const X86Insn *in, Loc l, int size) {
    return l.mem ? mem_rd(l.addr, size) : reg_rd(st, in, l.reg, size);
}
static inline void wr_loc(X86State *st, const X86Insn *in, Loc l, int size, uint64_t v) {
    if (l.mem) mem_wr(l.addr, size, v);
    else reg_wr(st, in, l.reg, size, v);
}

// #DE: 32-bit programs get EXCEPTION_INT_DIVIDE_BY_ZERO / _OVERFLOW through their SEH chain.
static int divide_error(X86State *st, const X86Insn *in, int overflow) {
    if (soul_seh_raise(st, overflow ? 0xC0000095u : 0xC0000094u, 0, in->addr, st->gpr[RSP], 0, NULL)) return 1;
    if (overflow) soul_fatal(st, "Bölme taşması (#DE) @%llx", (unsigned long long)in->addr);
    soul_fatal(st, "Tamsayı sıfıra bölme (#DE) @%llx", (unsigned long long)in->addr);
}

__attribute__((noreturn)) static void unsupported(X86State *st, const X86Insn *in) {
    const uint8_t *b = (const uint8_t *)G2H(in->addr);
    soul_fatal(st, "Desteklenmeyen x86 komutu @%016llx: %02x %02x %02x %02x %02x %02x (map %d op %02x)",
               (unsigned long long)in->addr, b[0], b[1], b[2], b[3], b[4], b[5], in->map, in->op);
}

static uint64_t do_alu(X86State *st, int op, uint64_t d, uint64_t s, int size) {
    uint64_t m = size_mask(size), r;
    d &= m;
    s &= m;
    switch (op) {
    case 0: r = (d + s) & m; set_lazy(st, LF_ADD, d, s, r, size); return r;
    case 1: r = d | s; set_lazy(st, LF_LOGIC, d, s, r, size); return r;
    case 2: {
        uint64_t c = x86_flags(st) & F_CF;
        r = (d + s + c) & m;
        int cf = c ? (r <= d) : (r < d);
        int of = ((d ^ r) & (s ^ r) & sign_bit(size)) != 0;
        x86_set_flags(st, r, size, cf, of, ((d ^ s ^ r) & 0x10) != 0);
        return r;
    }
    case 3: {
        uint64_t c = x86_flags(st) & F_CF;
        r = (d - s - c) & m;
        int cf = c ? (d <= s) : (d < s);
        int of = ((d ^ s) & (d ^ r) & sign_bit(size)) != 0;
        x86_set_flags(st, r, size, cf, of, ((d ^ s ^ r) & 0x10) != 0);
        return r;
    }
    case 4: r = d & s; set_lazy(st, LF_LOGIC, d, s, r, size); return r;
    case 6: r = d ^ s; set_lazy(st, LF_LOGIC, d, s, r, size); return r;
    default: r = (d - s) & m; set_lazy(st, LF_SUB, d, s, r, size); return r;
    }
}

static uint64_t do_shift(X86State *st, int op, uint64_t v, unsigned cnt, int size) {
    unsigned bits = (unsigned)size * 8;
    uint64_t m = size_mask(size);
    v &= m;
    cnt &= (size == 8) ? 63 : 31;
    if (cnt == 0) return v;
    uint64_t r;
    switch (op) {
    case 0: { // ROL
        unsigned c = cnt % bits;
        r = c ? ((v << c) | (v >> (bits - c))) & m : v;
        uint64_t f = x86_flags(st) & ~(uint64_t)(F_CF | F_OF);
        int cf = (int)(r & 1);
        int of = (int)((r >> (bits - 1)) & 1) ^ cf;
        st->eflags = f | (cf ? F_CF : 0) | (of ? F_OF : 0);
        return r;
    }
    case 1: { // ROR
        unsigned c = cnt % bits;
        r = c ? ((v >> c) | (v << (bits - c))) & m : v;
        uint64_t f = x86_flags(st) & ~(uint64_t)(F_CF | F_OF);
        int cf = (int)((r >> (bits - 1)) & 1);
        int of = (int)(((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1);
        st->eflags = f | (cf ? F_CF : 0) | (of ? F_OF : 0);
        return r;
    }
    case 2: { // RCL
        uint64_t f = x86_flags(st);
        unsigned c = cnt % (bits + 1);
        uint64_t cf = (f & F_CF) ? 1 : 0;
        r = v;
        for (unsigned i = 0; i < c; i++) {
            uint64_t nc = (r >> (bits - 1)) & 1;
            r = ((r << 1) | cf) & m;
            cf = nc;
        }
        int of = (int)(((r >> (bits - 1)) & 1) ^ cf);
        st->eflags = (f & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0) | (of ? F_OF : 0);
        return r;
    }
    case 3: { // RCR
        uint64_t f = x86_flags(st);
        unsigned c = cnt % (bits + 1);
        uint64_t cf = (f & F_CF) ? 1 : 0;
        r = v;
        for (unsigned i = 0; i < c; i++) {
            uint64_t nc = r & 1;
            r = (r >> 1) | (cf << (bits - 1));
            cf = nc;
        }
        int of = (int)(((r >> (bits - 1)) ^ (r >> (bits - 2))) & 1);
        st->eflags = (f & ~(uint64_t)(F_CF | F_OF)) | (cf ? F_CF : 0) | (of ? F_OF : 0);
        return r;
    }
    case 4: case 6: // SHL
        r = (cnt < bits) ? (v << cnt) & m : 0;
        set_lazy(st, LF_SHL, v, cnt, r, size);
        return r;
    case 5: // SHR
        r = (cnt < bits) ? (v >> cnt) : 0;
        set_lazy(st, LF_SHR, v, cnt, r, size);
        return r;
    default: // SAR
        r = (uint64_t)(sext(v, size) >> (cnt < bits ? cnt : bits - 1)) & m;
        set_lazy(st, LF_SAR, v, cnt, r, size);
        return r;
    }
}

static void do_inc_dec(X86State *st, const X86Insn *in, Loc l, int size, int dec) {
    uint64_t m = size_mask(size);
    uint64_t d = rd_loc(st, in, l, size);
    x86_flags(st); // make CF concrete
    uint64_t r = (dec ? d - 1 : d + 1) & m;
    set_lazy(st, dec ? LF_DEC : LF_INC, d, 1, r, size);
    wr_loc(st, in, l, size, r);
}

static uint64_t host_ticks(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 3000000000ull + (uint64_t)ts.tv_nsec * 3;
}

static void do_cpuid(X86State *st) {
    uint32_t leaf = (uint32_t)st->gpr[RAX], sub = (uint32_t)st->gpr[RCX];
    uint32_t a = 0, b = 0, c = 0, d = 0;
    (void)sub;
    static const char brand[48] = "Soul x86-64 on Apple Silicon (JIT)";
    switch (leaf) {
    case 0:
        a = 7;
        memcpy(&b, "Genu", 4); memcpy(&d, "ineI", 4); memcpy(&c, "ntel", 4);
        break;
    case 1:
        a = 0x000306C3; // family 6
        b = (1u << 16) | (8u << 8);
        // ECX: SSE3? no. CX16 (13), POPCNT (23)
        c = (1u << 13) | (1u << 23);
        // EDX: FPU, TSC, CX8, CMOV, FXSR, SSE, SSE2
        d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 24) | (1u << 25) | (1u << 26);
        break;
    case 0x80000000: a = 0x80000004; break;
    case 0x80000001:
        c = (1u << 0) | (1u << 5); // LAHF/SAHF, LZCNT
        d = (1u << 11) | (1u << 20) | (1u << 29);
        break;
    case 0x80000002: case 0x80000003: case 0x80000004: {
        const char *p = brand + (leaf - 0x80000002) * 16;
        memcpy(&a, p, 4); memcpy(&b, p + 4, 4); memcpy(&c, p + 8, 4); memcpy(&d, p + 12, 4);
        break;
    }
    default: break;
    }
    st->gpr[RAX] = a; st->gpr[RBX] = b; st->gpr[RCX] = c; st->gpr[RDX] = d;
}

// String instructions (MOVS/STOS/LODS/CMPS/SCAS) with optional REP prefixes.
static void do_string(X86State *st, const X86Insn *in) {
    uint8_t op = in->op;
    int size = (op & 1) ? in->opsize : 1;
    int64_t step = (x86_flags(st) & F_DF) ? -size : size;
    int rep = in->rep != 0;
    uint64_t seg_src = 0;
    if (in->seg == SEG_FS) seg_src = st->fs_base;
    else if (in->seg == SEG_GS) seg_src = st->gs_base;
    switch (op) {
    case 0xA4: case 0xA5: { // MOVS
        if (rep) {
            uint64_t n = st->gpr[RCX];
            uint64_t src = st->gpr[RSI] + seg_src, dst = st->gpr[RDI];
            uint64_t bytes = n * (uint64_t)size;
            if (step > 0 && (dst + bytes <= src || src + bytes <= dst)) {
                memcpy((void *)G2H(dst), (void *)G2H(src), bytes);
                st->gpr[RSI] += bytes; st->gpr[RDI] += bytes; st->gpr[RCX] = 0;
                return;
            }
            for (; st->gpr[RCX]; st->gpr[RCX]--) {
                mem_wr(st->gpr[RDI], size, mem_rd(st->gpr[RSI] + seg_src, size));
                st->gpr[RSI] += step; st->gpr[RDI] += step;
            }
        } else {
            mem_wr(st->gpr[RDI], size, mem_rd(st->gpr[RSI] + seg_src, size));
            st->gpr[RSI] += step; st->gpr[RDI] += step;
        }
        return;
    }
    case 0xAA: case 0xAB: { // STOS
        uint64_t v = st->gpr[RAX] & size_mask(size);
        if (rep) {
            uint64_t n = st->gpr[RCX];
            if (step > 0 && size == 1) {
                memset((void *)G2H(st->gpr[RDI]), (int)v, n);
                st->gpr[RDI] += n; st->gpr[RCX] = 0;
                return;
            }
            for (; st->gpr[RCX]; st->gpr[RCX]--) { mem_wr(st->gpr[RDI], size, v); st->gpr[RDI] += step; }
        } else {
            mem_wr(st->gpr[RDI], size, v); st->gpr[RDI] += step;
        }
        return;
    }
    case 0xAC: case 0xAD: { // LODS
        do {
            if (rep && st->gpr[RCX] == 0) break;
            uint64_t v = mem_rd(st->gpr[RSI] + seg_src, size);
            reg_wr(st, in, RAX, size, v);
            st->gpr[RSI] += step;
            if (rep) st->gpr[RCX]--;
        } while (rep);
        return;
    }
    case 0xA6: case 0xA7: { // CMPS
        do {
            if (rep && st->gpr[RCX] == 0) break;
            uint64_t a = mem_rd(st->gpr[RSI] + seg_src, size), b = mem_rd(st->gpr[RDI], size);
            do_alu(st, 7, a, b, size);
            st->gpr[RSI] += step; st->gpr[RDI] += step;
            if (rep) {
                st->gpr[RCX]--;
                int eq = (a == b);
                if (in->rep == 0xF3 && !eq) break;
                if (in->rep == 0xF2 && eq) break;
            }
        } while (rep);
        return;
    }
    default: { // SCAS
        uint64_t acc = st->gpr[RAX] & size_mask(size);
        do {
            if (rep && st->gpr[RCX] == 0) break;
            uint64_t b = mem_rd(st->gpr[RDI], size);
            do_alu(st, 7, acc, b, size);
            st->gpr[RDI] += step;
            if (rep) {
                st->gpr[RCX]--;
                int eq = (acc == b);
                if (in->rep == 0xF3 && !eq) break;
                if (in->rep == 0xF2 && eq) break;
            }
        } while (rep);
        return;
    }
    }
}

static void do_group3(X86State *st, const X86Insn *in, int size) {
    Loc l = loc_e(st, in);
    uint64_t m = size_mask(size);
    uint64_t v = rd_loc(st, in, l, size);
    switch (in->reg & 7) {
    case 0: case 1: { uint64_t r = v & in->imm & m; set_lazy(st, LF_LOGIC, v, in->imm & m, r, size); return; }
    case 2: wr_loc(st, in, l, size, ~v & m); return;
    case 3: { uint64_t r = (0 - v) & m; set_lazy(st, LF_SUB, 0, v, r, size); wr_loc(st, in, l, size, r); return; }
    case 4: { // MUL
        int cf;
        if (size == 1) {
            uint64_t r = (st->gpr[RAX] & 0xFF) * v;
            reg_wr(st, in, RAX, 2, r); cf = (r >> 8) != 0;
            x86_set_flags(st, r, 1, cf, cf, 0);
        } else if (size == 2) {
            uint64_t r = (st->gpr[RAX] & 0xFFFF) * v;
            reg_wr(st, in, RAX, 2, r); reg_wr(st, in, RDX, 2, r >> 16); cf = (r >> 16) != 0;
            x86_set_flags(st, r, 2, cf, cf, 0);
        } else if (size == 4) {
            uint64_t r = (st->gpr[RAX] & 0xFFFFFFFFull) * v;
            st->gpr[RAX] = (uint32_t)r; st->gpr[RDX] = r >> 32; cf = (r >> 32) != 0;
            x86_set_flags(st, r, 4, cf, cf, 0);
        } else {
            unsigned __int128 r = (unsigned __int128)st->gpr[RAX] * v;
            st->gpr[RAX] = (uint64_t)r; st->gpr[RDX] = (uint64_t)(r >> 64); cf = st->gpr[RDX] != 0;
            x86_set_flags(st, (uint64_t)r, 8, cf, cf, 0);
        }
        return;
    }
    case 5: { // IMUL (one operand)
        int cf;
        int64_t sv = sext(v, size);
        if (size == 1) {
            int64_t r = (int64_t)(int8_t)st->gpr[RAX] * sv;
            reg_wr(st, in, RAX, 2, (uint64_t)r); cf = r != (int8_t)r;
            x86_set_flags(st, (uint64_t)r, 1, cf, cf, 0);
        } else if (size == 2) {
            int64_t r = (int64_t)(int16_t)st->gpr[RAX] * sv;
            reg_wr(st, in, RAX, 2, (uint64_t)r); reg_wr(st, in, RDX, 2, (uint64_t)r >> 16); cf = r != (int16_t)r;
            x86_set_flags(st, (uint64_t)r, 2, cf, cf, 0);
        } else if (size == 4) {
            int64_t r = (int64_t)(int32_t)st->gpr[RAX] * sv;
            st->gpr[RAX] = (uint32_t)r; st->gpr[RDX] = (uint32_t)((uint64_t)r >> 32); cf = r != (int32_t)r;
            x86_set_flags(st, (uint64_t)r, 4, cf, cf, 0);
        } else {
            __int128 r = (__int128)(int64_t)st->gpr[RAX] * sv;
            st->gpr[RAX] = (uint64_t)r; st->gpr[RDX] = (uint64_t)((unsigned __int128)r >> 64);
            cf = r != (int64_t)r;
            x86_set_flags(st, (uint64_t)r, 8, cf, cf, 0);
        }
        return;
    }
    case 6: { // DIV
        if (v == 0) { divide_error(st, in, 0); return; }
        if (size == 1) {
            uint64_t n = st->gpr[RAX] & 0xFFFF, q = n / v, r = n % v;
            if (q > 0xFF) { divide_error(st, in, 1); return; }
            reg_wr(st, in, RAX, 2, (r << 8) | q);
        } else if (size == 2) {
            uint64_t n = ((st->gpr[RDX] & 0xFFFF) << 16) | (st->gpr[RAX] & 0xFFFF), q = n / v, r = n % v;
            if (q > 0xFFFF) { divide_error(st, in, 1); return; }
            reg_wr(st, in, RAX, 2, q); reg_wr(st, in, RDX, 2, r);
        } else if (size == 4) {
            uint64_t n = ((st->gpr[RDX] & 0xFFFFFFFFull) << 32) | (st->gpr[RAX] & 0xFFFFFFFFull), q = n / v, r = n % v;
            if (q > 0xFFFFFFFFull) { divide_error(st, in, 1); return; }
            st->gpr[RAX] = q; st->gpr[RDX] = r;
        } else {
            unsigned __int128 n = ((unsigned __int128)st->gpr[RDX] << 64) | st->gpr[RAX];
            unsigned __int128 q = n / v;
            if (q >> 64) { divide_error(st, in, 1); return; }
            st->gpr[RAX] = (uint64_t)q; st->gpr[RDX] = (uint64_t)(n % v);
        }
        return;
    }
    default: { // IDIV
        int64_t sv = sext(v, size);
        if (sv == 0) { divide_error(st, in, 0); return; }
        if (size == 1) {
            int64_t n = (int16_t)st->gpr[RAX], q = n / sv, r = n % sv;
            if (q != (int8_t)q) { divide_error(st, in, 1); return; }
            reg_wr(st, in, RAX, 2, ((uint64_t)(r & 0xFF) << 8) | (q & 0xFF));
        } else if (size == 2) {
            int64_t n = (int32_t)(((st->gpr[RDX] & 0xFFFF) << 16) | (st->gpr[RAX] & 0xFFFF)), q = n / sv, r = n % sv;
            if (q != (int16_t)q) { divide_error(st, in, 1); return; }
            reg_wr(st, in, RAX, 2, (uint64_t)q); reg_wr(st, in, RDX, 2, (uint64_t)r);
        } else if (size == 4) {
            int64_t n = (int64_t)(((st->gpr[RDX] & 0xFFFFFFFFull) << 32) | (st->gpr[RAX] & 0xFFFFFFFFull));
            if (sv == -1 && n == INT64_MIN) { divide_error(st, in, 1); return; }
            int64_t q = n / sv, r = n % sv;
            if (q != (int32_t)q) { divide_error(st, in, 1); return; }
            st->gpr[RAX] = (uint32_t)q; st->gpr[RDX] = (uint32_t)r;
        } else {
            __int128 n = (__int128)(((unsigned __int128)st->gpr[RDX] << 64) | st->gpr[RAX]);
            if (sv == -1 && (int64_t)st->gpr[RAX] == INT64_MIN && (int64_t)st->gpr[RDX] == -1)
                { divide_error(st, in, 1); return; }
            __int128 q = n / sv, r = n % sv;
            if (q != (int64_t)q) { divide_error(st, in, 1); return; }
            st->gpr[RAX] = (uint64_t)q; st->gpr[RDX] = (uint64_t)r;
        }
        return;
    }
    }
}

static int64_t imul_trunc(X86State *st, int64_t a, int64_t b, int size) {
    __int128 r = (__int128)a * b;
    uint64_t res = (uint64_t)r & size_mask(size);
    int of = r != sext(res, size);
    x86_set_flags(st, res, size, of, of, 0);
    return (int64_t)res;
}

// Bit test family: op 0=BT 1=BTS 2=BTR 3=BTC
static void do_bt(X86State *st, const X86Insn *in, int op, int64_t bitoff, int from_reg) {
    int size = in->opsize;
    int bits = size * 8;
    Loc l = loc_e(st, in);
    if (l.mem && from_reg) {
        int64_t idx = bitoff >> (bits == 64 ? 6 : bits == 32 ? 5 : 4);
        l.addr += (uint64_t)(idx * size);
        bitoff &= bits - 1;
    } else {
        bitoff &= bits - 1;
    }
    uint64_t v = rd_loc(st, in, l, size);
    uint64_t mask = 1ull << bitoff;
    uint64_t f = x86_flags(st) & ~(uint64_t)F_CF;
    if (v & mask) f |= F_CF;
    st->eflags = f;
    if (op == 1) v |= mask;
    else if (op == 2) v &= ~mask;
    else if (op == 3) v ^= mask;
    else return;
    wr_loc(st, in, l, size, v);
}

// Instructions that only exist in 32-bit mode (their opcodes are REX prefixes or invalid in 64-bit mode).
static int exec_legacy32(X86State *st, const X86Insn *in) {
    uint8_t op = in->op;
    if (op >= 0x40 && op <= 0x4F) { Loc l = { 0, op & 7, 0 }; do_inc_dec(st, in, l, in->opsize, op >= 0x48); return 1; }
    uint64_t f;
    uint32_t al;
    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: push64(st, op == 0x0E ? 0x23 : 0x2B); return 1; // push segment register
    case 0x07: case 0x17: case 0x1F: (void)pop64(st); return 1;                                   // pop segment register
    case 0x60: { // PUSHA(D)
        uint64_t sp = st->gpr[RSP];
        for (int r = 0; r < 8; r++) push64(st, r == RSP ? sp : st->gpr[r] & size_mask(in->opsize));
        return 1;
    }
    case 0x61: // POPA(D)
        for (int r = 7; r >= 0; r--) { uint64_t v = pop64(st); if (r != RSP) reg_wr(st, in, r, in->opsize, v); }
        return 1;
    case 0x27: case 0x2F: { // DAA / DAS
        f = x86_flags(st);
        al = (uint32_t)st->gpr[RAX] & 0xFF;
        uint32_t old = al;
        int cf = (f & F_CF) != 0, af = 0, ncf = 0;
        if ((al & 0x0F) > 9 || (f & F_AF)) { al = op == 0x27 ? al + 6 : al - 6; af = 1; ncf = cf || (op == 0x27 ? al > 0xFF : old < 6); }
        if (old > 0x99 || cf) { al = op == 0x27 ? al + 0x60 : al - 0x60; ncf = 1; }
        al &= 0xFF;
        reg_wr(st, in, RAX, 1, al);
        x86_set_flags(st, al, 1, ncf, 0, af);
        return 1;
    }
    case 0x37: case 0x3F: { // AAA / AAS
        f = x86_flags(st);
        uint32_t ax = (uint32_t)st->gpr[RAX] & 0xFFFF;
        int adj = (ax & 0x0F) > 9 || (f & F_AF);
        if (adj) ax = op == 0x37 ? ax + 0x106 : ax - 0x106;
        ax = (ax & 0xFF0F);
        reg_wr(st, in, RAX, 2, ax);
        x86_set_flags(st, ax & 0xFF, 1, adj, 0, adj);
        return 1;
    }
    case 0xD4: { // AAM imm8
        uint32_t base = (uint32_t)(in->imm & 0xFF);
        if (!base) soul_fatal(st, "AAM 0 ile bölme");
        al = (uint32_t)st->gpr[RAX] & 0xFF;
        uint32_t ax = ((al / base) << 8) | (al % base);
        reg_wr(st, in, RAX, 2, ax);
        x86_set_flags(st, ax & 0xFF, 1, 0, 0, 0);
        return 1;
    }
    case 0xD5: { // AAD imm8
        uint32_t ax = (uint32_t)st->gpr[RAX] & 0xFFFF;
        uint32_t r = ((ax & 0xFF) + (ax >> 8) * (uint32_t)(in->imm & 0xFF)) & 0xFF;
        reg_wr(st, in, RAX, 2, r);
        x86_set_flags(st, r, 1, 0, 0, 0);
        return 1;
    }
    case 0xD6: reg_wr(st, in, RAX, 1, (x86_flags(st) & F_CF) ? 0xFF : 0); return 1; // SALC
    case 0x63: case 0xCE: return 1;                                                   // ARPL / INTO
    default: return 0;
    }
}

static void exec_map1(X86State *st, const X86Insn *in) {
    uint8_t op = in->op;
    uint64_t next = in->addr + in->len;
    if (g_mode32) next = (uint32_t)next;
    st->rip = next;
    if (g_mode32 && exec_legacy32(st, in)) return;

    if (op < 0x40 && (op & 7) < 6) {
        int aop = op >> 3, form = op & 7;
        int size = (form & 1) ? in->opsize : 1;
        switch (form) {
        case 0: case 1: {
            Loc l = loc_e(st, in);
            uint64_t r = do_alu(st, aop, rd_loc(st, in, l, size), reg_rd(st, in, in->reg, size), size);
            if (aop != 7) wr_loc(st, in, l, size, r);
            return;
        }
        case 2: case 3: {
            Loc l = loc_e(st, in);
            uint64_t s = rd_loc(st, in, l, size);
            uint64_t r = do_alu(st, aop, reg_rd(st, in, in->reg, size), s, size);
            if (aop != 7) reg_wr(st, in, in->reg, size, r);
            return;
        }
        case 4: {
            uint64_t r = do_alu(st, aop, st->gpr[RAX], in->imm, 1);
            if (aop != 7) reg_wr(st, in, RAX, 1, r);
            return;
        }
        default: {
            uint64_t r = do_alu(st, aop, st->gpr[RAX], in->imm, in->opsize);
            if (aop != 7) reg_wr(st, in, RAX, in->opsize, r);
            return;
        }
        }
    }
    if (op >= 0x50 && op <= 0x57) {
        int r = (op & 7) | ((in->rex & 1) << 3);
        if (in->p66) { st->gpr[RSP] -= 2; mem_wr(st->gpr[RSP], 2, st->gpr[r]); }
        else push64(st, st->gpr[r]);
        return;
    }
    if (op >= 0x58 && op <= 0x5F) {
        int r = (op & 7) | ((in->rex & 1) << 3);
        if (in->p66) { uint64_t v = mem_rd(st->gpr[RSP], 2); st->gpr[RSP] += 2; reg_wr(st, in, r, 2, v); }
        else st->gpr[r] = pop64(st);
        return;
    }
    if (op >= 0x70 && op <= 0x7F) {
        if (x86_cond(st, op & 0xF)) st->rip = next + in->imm;
        return;
    }
    if (op >= 0x91 && op <= 0x97) {
        int r = (op & 7) | ((in->rex & 1) << 3);
        int size = in->opsize;
        uint64_t a = reg_rd(st, in, RAX, size), b = reg_rd(st, in, r, size);
        reg_wr(st, in, RAX, size, b); reg_wr(st, in, r, size, a);
        return;
    }
    if (op >= 0xB0 && op <= 0xB7) {
        int r = (op & 7) | ((in->rex & 1) << 3);
        reg_wr(st, in, r, 1, in->imm);
        return;
    }
    if (op >= 0xB8 && op <= 0xBF) {
        int r = (op & 7) | ((in->rex & 1) << 3);
        reg_wr(st, in, r, in->opsize, in->imm);
        return;
    }
    if (op >= 0xD8 && op <= 0xDF) {
        if (!x87_exec(st, in)) unsupported(st, in);
        return;
    }

    switch (op) {
    case 0x63: {
        Loc l = loc_e(st, in);
        if (in->opsize == 8) st->gpr[in->reg] = (uint64_t)(int64_t)(int32_t)rd_loc(st, in, l, 4);
        else reg_wr(st, in, in->reg, in->opsize, rd_loc(st, in, l, in->opsize));
        return;
    }
    case 0x68: case 0x6A:
        if (in->p66) { st->gpr[RSP] -= 2; mem_wr(st->gpr[RSP], 2, in->imm); }
        else push64(st, in->imm);
        return;
    case 0x69: case 0x6B: {
        int size = in->opsize;
        Loc l = loc_e(st, in);
        int64_t a = sext(rd_loc(st, in, l, size), size);
        int64_t r = imul_trunc(st, a, sext(in->imm, size), size);
        reg_wr(st, in, in->reg, size, (uint64_t)r);
        return;
    }
    case 0x80: case 0x81: case 0x83: {
        int size = (op == 0x80) ? 1 : in->opsize;
        int aop = in->reg & 7;
        Loc l = loc_e(st, in);
        uint64_t r = do_alu(st, aop, rd_loc(st, in, l, size), in->imm, size);
        if (aop != 7) wr_loc(st, in, l, size, r);
        return;
    }
    case 0x84: case 0x85: {
        int size = (op == 0x84) ? 1 : in->opsize;
        Loc l = loc_e(st, in);
        uint64_t d = rd_loc(st, in, l, size), s = reg_rd(st, in, in->reg, size);
        set_lazy(st, LF_LOGIC, d, s, d & s, size);
        return;
    }
    case 0x86: case 0x87: {
        int size = (op == 0x86) ? 1 : in->opsize;
        Loc l = loc_e(st, in);
        uint64_t a = rd_loc(st, in, l, size), b = reg_rd(st, in, in->reg, size);
        wr_loc(st, in, l, size, b);
        reg_wr(st, in, in->reg, size, a);
        return;
    }
    case 0x88: wr_loc(st, in, loc_e(st, in), 1, reg_rd(st, in, in->reg, 1)); return;
    case 0x89: wr_loc(st, in, loc_e(st, in), in->opsize, reg_rd(st, in, in->reg, in->opsize)); return;
    case 0x8A: reg_wr(st, in, in->reg, 1, rd_loc(st, in, loc_e(st, in), 1)); return;
    case 0x8B: reg_wr(st, in, in->reg, in->opsize, rd_loc(st, in, loc_e(st, in), in->opsize)); return;
    case 0x8C: wr_loc(st, in, loc_e(st, in), in->mod == 3 ? in->opsize : 2, (in->reg & 7) == 1 ? 0x33 : 0x2B); return;
    case 0x8D: reg_wr(st, in, in->reg, in->opsize, ea_noseg(st, in)); return;
    case 0x8E: return; // MOV Sreg, Ev: ignored in flat 64-bit mode
    case 0x8F: {
        uint64_t v = pop64(st);
        wr_loc(st, in, loc_e(st, in), stack_slot(), v);
        return;
    }
    case 0x90:
        if (in->rex & 1) { uint64_t t = st->gpr[R8]; reg_wr(st, in, R8, in->opsize, st->gpr[RAX]); reg_wr(st, in, RAX, in->opsize, t); }
        return;
    case 0x98:
        if (in->opsize == 8) st->gpr[RAX] = (uint64_t)(int64_t)(int32_t)st->gpr[RAX];
        else if (in->opsize == 4) st->gpr[RAX] = (uint32_t)(int32_t)(int16_t)st->gpr[RAX];
        else reg_wr(st, in, RAX, 2, (uint64_t)(int64_t)(int8_t)st->gpr[RAX]);
        return;
    case 0x99:
        if (in->opsize == 8) st->gpr[RDX] = (uint64_t)((int64_t)st->gpr[RAX] >> 63);
        else if (in->opsize == 4) st->gpr[RDX] = (uint32_t)((int32_t)st->gpr[RAX] >> 31);
        else reg_wr(st, in, RDX, 2, (uint64_t)((int16_t)st->gpr[RAX] >> 15));
        return;
    case 0x9B: return;
    case 0x9C: push64(st, (x86_flags(st) & ~(uint64_t)F_TF) | 0x202); return;
    case 0x9D: {
        uint64_t v = pop64(st);
        uint64_t keep = F_ARITH | F_DF | (1u << 18) | (1u << 21);
        x86_flags(st);
        st->eflags = (st->eflags & ~keep) | (v & keep);
        return;
    }
    case 0x9E: {
        uint64_t f = x86_flags(st);
        uint64_t ah = (st->gpr[RAX] >> 8) & 0xD5;
        st->eflags = (f & ~0xD5ull) | ah;
        return;
    }
    case 0x9F: {
        uint64_t f = x86_flags(st);
        st->gpr[RAX] = (st->gpr[RAX] & ~0xFF00ull) | (((f & 0xD5) | 2) << 8);
        return;
    }
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
        uint64_t a = in->imm2;
        if (in->seg == SEG_FS) a += st->fs_base;
        else if (in->seg == SEG_GS) a += st->gs_base;
        int size = (op & 1) ? in->opsize : 1;
        if (op < 0xA2) reg_wr(st, in, RAX, size, mem_rd(a, size));
        else mem_wr(a, size, st->gpr[RAX]);
        return;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        do_string(st, in);
        return;
    case 0xA8: { uint64_t d = st->gpr[RAX] & 0xFF; set_lazy(st, LF_LOGIC, d, in->imm & 0xFF, d & in->imm & 0xFF, 1); return; }
    case 0xA9: {
        uint64_t m = size_mask(in->opsize), d = st->gpr[RAX] & m;
        set_lazy(st, LF_LOGIC, d, in->imm & m, d & in->imm & m, in->opsize);
        return;
    }
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        int size = (op & 1) ? in->opsize : 1;
        unsigned cnt = (op <= 0xC1) ? (unsigned)in->imm : (op <= 0xD1) ? 1u : (unsigned)(st->gpr[RCX] & 0xFF);
        Loc l = loc_e(st, in);
        uint64_t v = rd_loc(st, in, l, size);
        uint64_t r = do_shift(st, in->reg & 7, v, cnt, size);
        wr_loc(st, in, l, size, r);
        return;
    }
    case 0xC2: st->rip = pop64(st); st->gpr[RSP] += in->imm; return;
    case 0xC3: st->rip = pop64(st); return;
    case 0xC6:
        if ((in->reg & 7) == 0) wr_loc(st, in, loc_e(st, in), 1, in->imm);
        return; // XABORT ignored
    case 0xC7:
        if ((in->reg & 7) == 0) wr_loc(st, in, loc_e(st, in), in->opsize, in->imm);
        else unsupported(st, in);
        return;
    case 0xC8: {
        push64(st, st->gpr[RBP]);
        uint64_t frame = st->gpr[RSP];
        unsigned level = (unsigned)(in->imm2 & 31);
        for (unsigned i = 1; i < level; i++) { st->gpr[RBP] -= (uint64_t)stack_slot(); push64(st, mem_rd(st->gpr[RBP], stack_slot())); }
        if (level) push64(st, frame);
        st->gpr[RBP] = frame;
        st->gpr[RSP] -= in->imm;
        return;
    }
    case 0xC9: st->gpr[RSP] = st->gpr[RBP]; st->gpr[RBP] = pop64(st); return;
    case 0xCC: {
        const uint8_t *b = (const uint8_t *)G2H(in->addr);
        if (b[1] == 0x53 && b[2] == 0x43) {
            uint32_t id; memcpy(&id, b + 3, 4);
            st->rip = in->addr;
            hle_invoke(st, id);
            return;
        }
        soul_fatal(st, "Program bir kesme noktasına (int3) ulaştı @%llx", (unsigned long long)in->addr);
    }
    case 0xCD:
        if ((in->imm & 0xFF) == 0x29)
            soul_fatal(st, "Program __fastfail ile sonlandı (kod %llu)", (unsigned long long)st->gpr[RCX]);
        soul_fatal(st, "Yazılım kesmesi int 0x%02llx desteklenmiyor", (unsigned long long)(in->imm & 0xFF));
    case 0xE0: case 0xE1: case 0xE2: {
        uint64_t c = in->adsize32 ? (uint32_t)(st->gpr[RCX] - 1) : st->gpr[RCX] - 1;
        if (in->adsize32) st->gpr[RCX] = (st->gpr[RCX] & ~0xFFFFFFFFull) | c;
        else st->gpr[RCX] = c;
        int take = c != 0;
        if (op == 0xE0) take = take && !(x86_flags(st) & F_ZF);
        if (op == 0xE1) take = take && (x86_flags(st) & F_ZF);
        if (take) st->rip = next + in->imm;
        return;
    }
    case 0xE3: {
        uint64_t c = in->adsize32 ? (uint32_t)st->gpr[RCX] : st->gpr[RCX];
        if (c == 0) st->rip = next + in->imm;
        return;
    }
    case 0xE8: push64(st, next); st->rip = next + in->imm; return;
    case 0xE9: case 0xEB: st->rip = next + in->imm; return;
    case 0xF4: soul_fatal(st, "HLT komutu çalıştırıldı");
    case 0xF5: x86_flags(st); st->eflags ^= F_CF; return;
    case 0xF8: x86_flags(st); st->eflags &= ~(uint64_t)F_CF; return;
    case 0xF9: x86_flags(st); st->eflags |= F_CF; return;
    case 0xFA: case 0xFB: return;
    case 0xFC: st->eflags &= ~(uint64_t)F_DF; return;
    case 0xFD: st->eflags |= F_DF; return;
    case 0xF6: do_group3(st, in, 1); return;
    case 0xF7: do_group3(st, in, in->opsize); return;
    case 0xFE: {
        int r = in->reg & 7;
        if (r > 1) unsupported(st, in);
        do_inc_dec(st, in, loc_e(st, in), 1, r);
        return;
    }
    case 0xFF: {
        int r = in->reg & 7;
        switch (r) {
        case 0: case 1: do_inc_dec(st, in, loc_e(st, in), in->opsize, r); return;
        case 2: { uint64_t t = rd_loc(st, in, loc_e(st, in), stack_slot()); push64(st, next); st->rip = t; return; }
        case 4: st->rip = rd_loc(st, in, loc_e(st, in), stack_slot()); return;
        case 6: push64(st, rd_loc(st, in, loc_e(st, in), stack_slot())); return;
        default: unsupported(st, in);
        }
    }
    default:
        unsupported(st, in);
    }
}

static void exec_map0f(X86State *st, const X86Insn *in) {
    uint8_t op = in->op;
    uint64_t next = in->addr + in->len;
    st->rip = next;

    if (op >= 0x40 && op <= 0x4F) { // CMOVcc
        uint64_t v = rd_loc(st, in, loc_e(st, in), in->opsize);
        if (x86_cond(st, op & 0xF)) reg_wr(st, in, in->reg, in->opsize, v);
        else if (in->opsize == 4) st->gpr[in->reg] = (uint32_t)st->gpr[in->reg];
        return;
    }
    if (op >= 0x80 && op <= 0x8F) {
        if (x86_cond(st, op & 0xF)) st->rip = next + in->imm;
        return;
    }
    if (op >= 0x90 && op <= 0x9F) {
        wr_loc(st, in, loc_e(st, in), 1, (uint64_t)x86_cond(st, op & 0xF));
        return;
    }
    if (op >= 0xC8 && op <= 0xCF) {
        int r = (op & 7) | ((in->rex & 1) << 3);
        if (in->opsize == 8) st->gpr[r] = __builtin_bswap64(st->gpr[r]);
        else st->gpr[r] = __builtin_bswap32((uint32_t)st->gpr[r]);
        return;
    }
    switch (op) {
    case 0x05: soul_fatal(st, "Doğrudan sistem çağrısı (syscall) desteklenmiyor");
    case 0x0B: soul_fatal(st, "Program UD2 (geçersiz komut) çalıştırdı @%llx", (unsigned long long)in->addr);
    case 0x0D: case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        return; // prefetch / hint NOPs / ENDBR64
    case 0x31: { uint64_t t = host_ticks(); st->gpr[RAX] = (uint32_t)t; st->gpr[RDX] = t >> 32; return; }
    case 0xA2: do_cpuid(st); return;
    case 0xA3: do_bt(st, in, 0, sext(st->gpr[in->reg], in->opsize), 1); return;
    case 0xAB: do_bt(st, in, 1, sext(st->gpr[in->reg], in->opsize), 1); return;
    case 0xB3: do_bt(st, in, 2, sext(st->gpr[in->reg], in->opsize), 1); return;
    case 0xBB: do_bt(st, in, 3, sext(st->gpr[in->reg], in->opsize), 1); return;
    case 0xBA: {
        int r = in->reg & 7;
        if (r < 4) unsupported(st, in);
        do_bt(st, in, r - 4, (int64_t)(in->imm & 0xFF), 0);
        return;
    }
    case 0xA4: case 0xA5: case 0xAC: case 0xAD: { // SHLD / SHRD
        int size = in->opsize, bits = size * 8;
        uint64_t m = size_mask(size);
        unsigned cnt = (op == 0xA4 || op == 0xAC) ? (unsigned)(in->imm & 0xFF) : (unsigned)(st->gpr[RCX] & 0xFF);
        cnt &= (size == 8) ? 63 : 31;
        if (!cnt) return;
        Loc l = loc_e(st, in);
        uint64_t d = rd_loc(st, in, l, size), s = reg_rd(st, in, in->reg, size), r;
        int cf;
        if (op <= 0xA5) {
            r = ((d << cnt) | (cnt < (unsigned)bits ? (s >> (bits - cnt)) : 0)) & m;
            cf = (int)((d >> (bits - cnt)) & 1);
        } else {
            r = ((d >> cnt) | (s << (bits - cnt))) & m;
            cf = (int)((d >> (cnt - 1)) & 1);
        }
        int of = ((r ^ d) & sign_bit(size)) != 0;
        x86_set_flags(st, r, size, cf, of, 0);
        wr_loc(st, in, l, size, r);
        return;
    }
    case 0xAE:
        if (in->mod == 3) return; // LFENCE/MFENCE/SFENCE
        switch (in->reg & 7) {
        case 0: { // FXSAVE
            uint8_t *p = (uint8_t *)G2H(ea_addr(st, in));
            memset(p, 0, 512);
            memcpy(p, &st->fcw, 2);
            memcpy(p + 24, &st->mxcsr, 4);
            for (int i = 0; i < 16; i++) memcpy(p + 160 + 16 * i, &st->xmm[i], 16);
            return;
        }
        case 1: { // FXRSTOR
            uint8_t *p = (uint8_t *)G2H(ea_addr(st, in));
            memcpy(&st->fcw, p, 2);
            memcpy(&st->mxcsr, p + 24, 4);
            for (int i = 0; i < 16; i++) memcpy(&st->xmm[i], p + 160 + 16 * i, 16);
            return;
        }
        case 2: st->mxcsr = (uint32_t)mem_rd(ea_addr(st, in), 4); return;
        case 3: mem_wr(ea_addr(st, in), 4, st->mxcsr); return;
        case 7: return; // CLFLUSH
        default: unsupported(st, in);
        }
    case 0xAF: {
        int size = in->opsize;
        int64_t a = sext(st->gpr[in->reg], size);
        int64_t b = sext(rd_loc(st, in, loc_e(st, in), size), size);
        reg_wr(st, in, in->reg, size, (uint64_t)imul_trunc(st, a, b, size));
        return;
    }
    case 0xB0: case 0xB1: {
        int size = (op == 0xB0) ? 1 : in->opsize;
        Loc l = loc_e(st, in);
        uint64_t d = rd_loc(st, in, l, size);
        uint64_t acc = reg_rd(st, in, RAX, size);
        do_alu(st, 7, acc, d, size);
        if (acc == d) wr_loc(st, in, l, size, reg_rd(st, in, in->reg, size));
        else reg_wr(st, in, RAX, size, d);
        return;
    }
    case 0xB6: case 0xB7: {
        uint64_t v = rd_loc(st, in, loc_e(st, in), op == 0xB6 ? 1 : 2);
        reg_wr(st, in, in->reg, in->opsize, v);
        return;
    }
    case 0xBE: case 0xBF: {
        int ssz = op == 0xBE ? 1 : 2;
        uint64_t v = (uint64_t)sext(rd_loc(st, in, loc_e(st, in), ssz), ssz);
        reg_wr(st, in, in->reg, in->opsize, v);
        return;
    }
    case 0xB8: {
        if (in->rep != 0xF3) unsupported(st, in);
        uint64_t v = rd_loc(st, in, loc_e(st, in), in->opsize);
        uint64_t r = (uint64_t)__builtin_popcountll(v);
        reg_wr(st, in, in->reg, in->opsize, r);
        x86_flags(st);
        st->eflags = (st->eflags & ~(uint64_t)F_ARITH) | (v == 0 ? F_ZF : 0);
        return;
    }
    case 0xBC: case 0xBD: {
        int size = in->opsize, bits = size * 8;
        uint64_t v = rd_loc(st, in, loc_e(st, in), size);
        x86_flags(st);
        if (in->rep == 0xF3) { // TZCNT / LZCNT
            uint64_t r;
            if (op == 0xBC) r = v ? (uint64_t)__builtin_ctzll(v) : (uint64_t)bits;
            else r = v ? (uint64_t)(__builtin_clzll(v) - (64 - bits)) : (uint64_t)bits;
            reg_wr(st, in, in->reg, size, r);
            st->eflags = (st->eflags & ~(uint64_t)(F_CF | F_ZF)) | (v == 0 ? F_CF : 0) | (r == 0 ? F_ZF : 0);
        } else {
            if (v == 0) { st->eflags |= F_ZF; return; }
            uint64_t r = (op == 0xBC) ? (uint64_t)__builtin_ctzll(v) : (uint64_t)(63 - __builtin_clzll(v));
            reg_wr(st, in, in->reg, size, r);
            st->eflags &= ~(uint64_t)F_ZF;
        }
        return;
    }
    case 0xC0: case 0xC1: {
        int size = (op == 0xC0) ? 1 : in->opsize;
        Loc l = loc_e(st, in);
        uint64_t d = rd_loc(st, in, l, size), s = reg_rd(st, in, in->reg, size);
        uint64_t r = do_alu(st, 0, d, s, size);
        reg_wr(st, in, in->reg, size, d);
        wr_loc(st, in, l, size, r);
        return;
    }
    case 0xC3: mem_wr(ea_addr(st, in), in->opsize, st->gpr[in->reg]); return; // MOVNTI
    case 0xC7: {
        int r = in->reg & 7;
        if (r != 1 || in->mod == 3) unsupported(st, in);
        uint64_t a = ea_addr(st, in);
        x86_flags(st);
        if (in->rex & 8) { // CMPXCHG16B
            uint64_t lo = mem_rd(a, 8), hi = mem_rd(a + 8, 8);
            if (lo == st->gpr[RAX] && hi == st->gpr[RDX]) {
                mem_wr(a, 8, st->gpr[RBX]); mem_wr(a + 8, 8, st->gpr[RCX]);
                st->eflags |= F_ZF;
            } else {
                st->gpr[RAX] = lo; st->gpr[RDX] = hi;
                st->eflags &= ~(uint64_t)F_ZF;
            }
        } else { // CMPXCHG8B
            uint64_t v = mem_rd(a, 8);
            uint64_t cmp = (st->gpr[RAX] & 0xFFFFFFFFull) | (st->gpr[RDX] << 32);
            if (v == cmp) {
                mem_wr(a, 8, (st->gpr[RBX] & 0xFFFFFFFFull) | (st->gpr[RCX] << 32));
                st->eflags |= F_ZF;
            } else {
                st->gpr[RAX] = (uint32_t)v; st->gpr[RDX] = v >> 32;
                st->eflags &= ~(uint64_t)F_ZF;
            }
        }
        return;
    }
    default:
        if (sse_exec(st, in)) return;
        unsupported(st, in);
    }
}

void x86_exec(X86State *st, const X86Insn *in) {
    st->icount_interp++;
    switch (in->map) {
    case MAP_1: exec_map1(st, in); return;
    case MAP_0F: exec_map0f(st, in); return;
    default:
        st->rip = in->addr + in->len;
        if (sse_exec(st, in)) return;
        unsupported(st, in);
    }
}

uint64_t g_interp_hist[4 * 256];
int g_interp_profile;

void x86_interp_step(X86State *st) {
    X86Insn in;
    const uint8_t *code = (const uint8_t *)G2H(st->rip);
    if (x86_decode(code, st->rip, &in) != 0) {
        soul_fatal(st, "Çözülemeyen komut @%016llx: %02x %02x %02x %02x %02x (AVX/VEX desteklenmiyor olabilir)",
                   (unsigned long long)st->rip, code[0], code[1], code[2], code[3], code[4]);
    }
    if (__builtin_expect(g_interp_profile, 0)) g_interp_hist[in.map * 256 + in.op]++;
    x86_exec(st, &in);
    if (g_mode32) st->rip = (uint32_t)st->rip;
}
