// SoulCore - lazy EFLAGS evaluation.
#include "cpu.h"

static inline uint64_t parity_flag(uint64_t res) {
    return (__builtin_parity((unsigned)(res & 0xFF)) == 0) ? F_PF : 0;
}

static inline uint64_t szp(uint64_t res, int size) {
    uint64_t m = size_mask(size);
    uint64_t f = 0;
    if ((res & m) == 0) f |= F_ZF;
    if (res & sign_bit(size)) f |= F_SF;
    f |= parity_flag(res);
    return f;
}

void x86_set_flags(X86State *st, uint64_t res, int size, int cf, int of, int af) {
    uint64_t f = szp(res, size);
    if (cf) f |= F_CF;
    if (of) f |= F_OF;
    if (af) f |= F_AF;
    st->eflags = (st->eflags & ~(uint64_t)F_ARITH) | f;
    st->lf_op = LF_NONE;
}

uint64_t x86_flags(X86State *st) {
    if (st->lf_op == LF_NONE) return st->eflags;
    int size = (int)st->lf_size;
    uint64_t m = size_mask(size);
    uint64_t sb = sign_bit(size);
    uint64_t d = st->lf_dst & m, s = st->lf_src & m, r = st->lf_res & m;
    uint64_t f = szp(r, size);
    switch (st->lf_op) {
    case LF_ADD:
        if (r < d) f |= F_CF;
        if ((d ^ r) & (s ^ r) & sb) f |= F_OF;
        if ((d ^ s ^ r) & 0x10) f |= F_AF;
        break;
    case LF_SUB:
        if (d < s) f |= F_CF;
        if ((d ^ s) & (d ^ r) & sb) f |= F_OF;
        if ((d ^ s ^ r) & 0x10) f |= F_AF;
        break;
    case LF_LOGIC:
        break;
    case LF_INC:
        f |= st->eflags & F_CF;
        if (r == sb) f |= F_OF;
        if ((r & 0xF) == 0) f |= F_AF;
        break;
    case LF_DEC:
        f |= st->eflags & F_CF;
        if (r == sb - 1) f |= F_OF;
        if ((r & 0xF) == 0xF) f |= F_AF;
        break;
    case LF_SHL: {
        unsigned bits = (unsigned)size * 8, c = (unsigned)s;
        uint64_t cf = (c <= bits) ? ((d >> (bits - c)) & 1) : 0;
        if (cf) f |= F_CF;
        if (((r & sb) ? 1 : 0) ^ cf) f |= F_OF;
        break;
    }
    case LF_SHR: {
        unsigned c = (unsigned)s;
        if ((d >> (c - 1)) & 1) f |= F_CF;
        if (c == 1 && (d & sb)) f |= F_OF;
        break;
    }
    case LF_SAR: {
        unsigned c = (unsigned)s;
        int64_t sd = sext(d, size);
        if (c > 63) c = 63;
        if (((uint64_t)(sd >> (c - 1))) & 1) f |= F_CF;
        break;
    }
    default:
        break;
    }
    st->eflags = (st->eflags & ~(uint64_t)F_ARITH) | f;
    st->lf_op = LF_NONE;
    return st->eflags;
}

int x86_cond(X86State *st, int cc) {
    // Fast path for CMP/SUB producers.
    if (st->lf_op == LF_SUB) {
        int size = (int)st->lf_size;
        uint64_t m = size_mask(size);
        uint64_t d = st->lf_dst & m, s = st->lf_src & m;
        int64_t sd = sext(d, size), ss = sext(s, size);
        switch (cc) {
        case 2: return d < s;
        case 3: return d >= s;
        case 4: return d == s;
        case 5: return d != s;
        case 6: return d <= s;
        case 7: return d > s;
        case 12: return sd < ss;
        case 13: return sd >= ss;
        case 14: return sd <= ss;
        case 15: return sd > ss;
        default: break;
        }
    }
    uint64_t f = x86_flags(st);
    int r;
    switch (cc >> 1) {
    case 0: r = (f & F_OF) != 0; break;
    case 1: r = (f & F_CF) != 0; break;
    case 2: r = (f & F_ZF) != 0; break;
    case 3: r = (f & (F_CF | F_ZF)) != 0; break;
    case 4: r = (f & F_SF) != 0; break;
    case 5: r = (f & F_PF) != 0; break;
    case 6: r = ((f & F_SF) != 0) != ((f & F_OF) != 0); break;
    default: r = (f & F_ZF) || (((f & F_SF) != 0) != ((f & F_OF) != 0)); break;
    }
    return (cc & 1) ? !r : r;
}
