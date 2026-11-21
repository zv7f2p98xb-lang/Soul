// SoulCore - SSE / SSE2 interpreter (scalar + packed float, 128-bit integer).
#include "interp.h"
#include "soul_internal.h"
#include <math.h>
#include <stdlib.h>

// Mandatory prefix: 0 none, 1 = 66, 2 = F3, 3 = F2
static inline int sse_pfx(const X86Insn *in) {
    if (in->rep == 0xF3) return 2;
    if (in->rep == 0xF2) return 3;
    return in->p66 ? 1 : 0;
}

// Read the r/m operand. For memory forms `bytes` are loaded (rest zero).
static inline Xmm src_rm(X86State *st, const X86Insn *in, int bytes) {
    Xmm v;
    if (in->mod == 3) return st->xmm[in->rm];
    memset(&v, 0, sizeof(v));
    memcpy(&v, (void *)G2H(ea_addr(st, in)), (size_t)bytes);
    return v;
}
static inline void store_rm(X86State *st, const X86Insn *in, const Xmm *v, int bytes) {
    if (in->mod == 3) {
        if (bytes == 16) st->xmm[in->rm] = *v;
        else memcpy(&st->xmm[in->rm], v, (size_t)bytes);
    } else {
        memcpy((void *)G2H(ea_addr(st, in)), v, (size_t)bytes);
    }
}

static inline int64_t cvt_trunc_i(double v, int size) {
    if (size == 8) {
        if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return INT64_MIN;
        return (int64_t)v;
    }
    if (!(v >= -2147483648.0 && v < 2147483648.0)) return INT32_MIN;
    return (int32_t)v;
}
static inline int64_t cvt_round_i(double v, int size) { return cvt_trunc_i(nearbyint(v), size); }

static void set_fcmp_flags(X86State *st, double a, double b) {
    uint64_t f;
    if (isnan(a) || isnan(b)) f = F_ZF | F_PF | F_CF;
    else if (a < b) f = F_CF;
    else if (a == b) f = F_ZF;
    else f = 0;
    st->eflags = (st->eflags & ~(uint64_t)F_ARITH) | f;
    st->lf_op = LF_NONE;
}

static inline int fcmp_pred(double a, double b, int pred) {
    int un = isnan(a) || isnan(b);
    switch (pred & 7) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    default: return !un;
    }
}

static inline int8_t sat8(int v) { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }
static inline uint8_t satu8(int v) { return (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }
static inline int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
static inline uint16_t satu16(int v) { return (uint16_t)(v > 65535 ? 65535 : v < 0 ? 0 : v); }

static int packed_int(X86State *st, const X86Insn *in) {
    uint8_t op = in->op;
    Xmm *d = &st->xmm[in->reg];
    Xmm s = src_rm(st, in, 16);
    Xmm r = *d;
    int i;
    switch (op) {
    case 0x60: for (i = 0; i < 8; i++) { r.b[2 * i] = d->b[i]; r.b[2 * i + 1] = s.b[i]; } break;
    case 0x61: for (i = 0; i < 4; i++) { r.w[2 * i] = d->w[i]; r.w[2 * i + 1] = s.w[i]; } break;
    case 0x62: for (i = 0; i < 2; i++) { r.d[2 * i] = d->d[i]; r.d[2 * i + 1] = s.d[i]; } break;
    case 0x63: for (i = 0; i < 8; i++) { r.sb[i] = sat8(d->sw[i]); r.sb[8 + i] = sat8(s.sw[i]); } break;
    case 0x64: for (i = 0; i < 16; i++) r.b[i] = d->sb[i] > s.sb[i] ? 0xFF : 0; break;
    case 0x65: for (i = 0; i < 8; i++) r.w[i] = d->sw[i] > s.sw[i] ? 0xFFFF : 0; break;
    case 0x66: for (i = 0; i < 4; i++) r.d[i] = d->sd[i] > s.sd[i] ? 0xFFFFFFFFu : 0; break;
    case 0x67: for (i = 0; i < 8; i++) { r.b[i] = satu8(d->sw[i]); r.b[8 + i] = satu8(s.sw[i]); } break;
    case 0x68: for (i = 0; i < 8; i++) { r.b[2 * i] = d->b[8 + i]; r.b[2 * i + 1] = s.b[8 + i]; } break;
    case 0x69: for (i = 0; i < 4; i++) { r.w[2 * i] = d->w[4 + i]; r.w[2 * i + 1] = s.w[4 + i]; } break;
    case 0x6A: for (i = 0; i < 2; i++) { r.d[2 * i] = d->d[2 + i]; r.d[2 * i + 1] = s.d[2 + i]; } break;
    case 0x6B: for (i = 0; i < 4; i++) { r.sw[i] = sat16(d->sd[i]); r.sw[4 + i] = sat16(s.sd[i]); } break;
    case 0x6C: r.q[0] = d->q[0]; r.q[1] = s.q[0]; break;
    case 0x6D: r.q[0] = d->q[1]; r.q[1] = s.q[1]; break;
    case 0x74: for (i = 0; i < 16; i++) r.b[i] = d->b[i] == s.b[i] ? 0xFF : 0; break;
    case 0x75: for (i = 0; i < 8; i++) r.w[i] = d->w[i] == s.w[i] ? 0xFFFF : 0; break;
    case 0x76: for (i = 0; i < 4; i++) r.d[i] = d->d[i] == s.d[i] ? 0xFFFFFFFFu : 0; break;
    case 0xD4: r.q[0] = d->q[0] + s.q[0]; r.q[1] = d->q[1] + s.q[1]; break;
    case 0xD5: for (i = 0; i < 8; i++) r.w[i] = (uint16_t)(d->sw[i] * s.sw[i]); break;
    case 0xD8: for (i = 0; i < 16; i++) r.b[i] = satu8(d->b[i] - s.b[i]); break;
    case 0xD9: for (i = 0; i < 8; i++) r.w[i] = satu16(d->w[i] - s.w[i]); break;
    case 0xDA: for (i = 0; i < 16; i++) r.b[i] = d->b[i] < s.b[i] ? d->b[i] : s.b[i]; break;
    case 0xDB: r.q[0] = d->q[0] & s.q[0]; r.q[1] = d->q[1] & s.q[1]; break;
    case 0xDC: for (i = 0; i < 16; i++) r.b[i] = satu8(d->b[i] + s.b[i]); break;
    case 0xDD: for (i = 0; i < 8; i++) r.w[i] = satu16(d->w[i] + s.w[i]); break;
    case 0xDE: for (i = 0; i < 16; i++) r.b[i] = d->b[i] > s.b[i] ? d->b[i] : s.b[i]; break;
    case 0xDF: r.q[0] = ~d->q[0] & s.q[0]; r.q[1] = ~d->q[1] & s.q[1]; break;
    case 0xE0: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)((d->b[i] + s.b[i] + 1) >> 1); break;
    case 0xE3: for (i = 0; i < 8; i++) r.w[i] = (uint16_t)((d->w[i] + s.w[i] + 1) >> 1); break;
    case 0xE4: for (i = 0; i < 8; i++) r.w[i] = (uint16_t)(((uint32_t)d->w[i] * s.w[i]) >> 16); break;
    case 0xE5: for (i = 0; i < 8; i++) r.w[i] = (uint16_t)(((int32_t)d->sw[i] * s.sw[i]) >> 16); break;
    case 0xE8: for (i = 0; i < 16; i++) r.sb[i] = sat8(d->sb[i] - s.sb[i]); break;
    case 0xE9: for (i = 0; i < 8; i++) r.sw[i] = sat16(d->sw[i] - s.sw[i]); break;
    case 0xEA: for (i = 0; i < 8; i++) r.sw[i] = d->sw[i] < s.sw[i] ? d->sw[i] : s.sw[i]; break;
    case 0xEB: r.q[0] = d->q[0] | s.q[0]; r.q[1] = d->q[1] | s.q[1]; break;
    case 0xEC: for (i = 0; i < 16; i++) r.sb[i] = sat8(d->sb[i] + s.sb[i]); break;
    case 0xED: for (i = 0; i < 8; i++) r.sw[i] = sat16(d->sw[i] + s.sw[i]); break;
    case 0xEE: for (i = 0; i < 8; i++) r.sw[i] = d->sw[i] > s.sw[i] ? d->sw[i] : s.sw[i]; break;
    case 0xEF: r.q[0] = d->q[0] ^ s.q[0]; r.q[1] = d->q[1] ^ s.q[1]; break;
    case 0xF4: r.q[0] = (uint64_t)d->d[0] * s.d[0]; r.q[1] = (uint64_t)d->d[2] * s.d[2]; break;
    case 0xF5: for (i = 0; i < 4; i++) r.sd[i] = d->sw[2 * i] * s.sw[2 * i] + d->sw[2 * i + 1] * s.sw[2 * i + 1]; break;
    case 0xF6: {
        uint32_t a = 0, b = 0;
        for (i = 0; i < 8; i++) a += (uint32_t)abs(d->b[i] - s.b[i]);
        for (i = 8; i < 16; i++) b += (uint32_t)abs(d->b[i] - s.b[i]);
        r.q[0] = a; r.q[1] = b;
        break;
    }
    case 0xF8: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)(d->b[i] - s.b[i]); break;
    case 0xF9: for (i = 0; i < 8; i++) r.w[i] = (uint16_t)(d->w[i] - s.w[i]); break;
    case 0xFA: for (i = 0; i < 4; i++) r.d[i] = d->d[i] - s.d[i]; break;
    case 0xFB: r.q[0] = d->q[0] - s.q[0]; r.q[1] = d->q[1] - s.q[1]; break;
    case 0xFC: for (i = 0; i < 16; i++) r.b[i] = (uint8_t)(d->b[i] + s.b[i]); break;
    case 0xFD: for (i = 0; i < 8; i++) r.w[i] = (uint16_t)(d->w[i] + s.w[i]); break;
    case 0xFE: for (i = 0; i < 4; i++) r.d[i] = d->d[i] + s.d[i]; break;
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: {
        uint64_t c = s.q[0];
        for (i = 0; i < 8 && (op == 0xD1 || op == 0xE1 || op == 0xF1); i++) {
            if (op == 0xD1) r.w[i] = c > 15 ? 0 : (uint16_t)(d->w[i] >> c);
            else if (op == 0xE1) r.sw[i] = (int16_t)(d->sw[i] >> (c > 15 ? 15 : c));
            else r.w[i] = c > 15 ? 0 : (uint16_t)(d->w[i] << c);
        }
        for (i = 0; i < 4 && (op == 0xD2 || op == 0xE2 || op == 0xF2); i++) {
            if (op == 0xD2) r.d[i] = c > 31 ? 0 : d->d[i] >> c;
            else if (op == 0xE2) r.sd[i] = d->sd[i] >> (c > 31 ? 31 : c);
            else r.d[i] = c > 31 ? 0 : d->d[i] << c;
        }
        for (i = 0; i < 2 && (op == 0xD3 || op == 0xF3); i++) {
            if (op == 0xD3) r.q[i] = c > 63 ? 0 : d->q[i] >> c;
            else r.q[i] = c > 63 ? 0 : d->q[i] << c;
        }
        break;
    }
    default: return 0;
    }
    *d = r;
    return 1;
}

static int shift_imm(X86State *st, const X86Insn *in) {
    Xmm *d = &st->xmm[in->rm];
    unsigned c = (unsigned)(in->imm & 0xFF);
    int sub = in->reg & 7, i;
    Xmm r = *d;
    if (in->op == 0x71) {
        for (i = 0; i < 8; i++) {
            if (sub == 2) r.w[i] = c > 15 ? 0 : (uint16_t)(d->w[i] >> c);
            else if (sub == 4) r.sw[i] = (int16_t)(d->sw[i] >> (c > 15 ? 15 : c));
            else if (sub == 6) r.w[i] = c > 15 ? 0 : (uint16_t)(d->w[i] << c);
            else return 0;
        }
    } else if (in->op == 0x72) {
        for (i = 0; i < 4; i++) {
            if (sub == 2) r.d[i] = c > 31 ? 0 : d->d[i] >> c;
            else if (sub == 4) r.sd[i] = d->sd[i] >> (c > 31 ? 31 : c);
            else if (sub == 6) r.d[i] = c > 31 ? 0 : d->d[i] << c;
            else return 0;
        }
    } else {
        switch (sub) {
        case 2: for (i = 0; i < 2; i++) r.q[i] = c > 63 ? 0 : d->q[i] >> c; break;
        case 6: for (i = 0; i < 2; i++) r.q[i] = c > 63 ? 0 : d->q[i] << c; break;
        case 3: memset(&r, 0, 16); if (c < 16) memcpy(&r.b[0], &d->b[c], 16 - c); break;
        case 7: memset(&r, 0, 16); if (c < 16) memcpy(&r.b[c], &d->b[0], 16 - c); break;
        default: return 0;
        }
    }
    *d = r;
    return 1;
}

static int float_arith(X86State *st, const X86Insn *in, int pfx) {
    uint8_t op = in->op;
    Xmm *d = &st->xmm[in->reg];
    int n, i;
    if (pfx == 0 || pfx == 1) n = 16; else if (pfx == 2) n = 4; else n = 8;
    Xmm s = src_rm(st, in, n);
    Xmm r = *d;
    int dbl = (pfx == 1 || pfx == 3);
    int cnt = (pfx == 0) ? 4 : (pfx == 1) ? 2 : 1;
    for (i = 0; i < cnt; i++) {
        double a = dbl ? d->fd[i] : d->f[i];
        double b = dbl ? s.fd[i] : s.f[i];
        double x;
        switch (op) {
        case 0x51: x = dbl ? sqrt(b) : (double)sqrtf((float)b); break;
        case 0x52: x = 1.0 / sqrt(b); break;
        case 0x53: x = 1.0 / b; break;
        case 0x58: x = a + b; break;
        case 0x59: x = a * b; break;
        case 0x5C: x = a - b; break;
        case 0x5D: x = (a < b) ? a : b; break;
        case 0x5E: x = a / b; break;
        case 0x5F: x = (a > b) ? a : b; break;
        default: return 0;
        }
        if (dbl) r.fd[i] = x;
        else {
            // Single precision must round like real float math.
            float fa = (float)a, fb = (float)b, fx;
            switch (op) {
            case 0x58: fx = fa + fb; break;
            case 0x59: fx = fa * fb; break;
            case 0x5C: fx = fa - fb; break;
            case 0x5E: fx = fa / fb; break;
            default: fx = (float)x; break;
            }
            r.f[i] = fx;
        }
    }
    *d = r;
    return 1;
}

int sse_exec(X86State *st, const X86Insn *in) {
    int pfx = sse_pfx(in);
    uint8_t op = in->op;
    if (in->map == MAP_0F38 || in->map == MAP_0F3A) return 0;
    Xmm *d = &st->xmm[in->reg];

    switch (op) {
    case 0x10:
        if (pfx <= 1) { *d = src_rm(st, in, 16); return 1; }
        if (pfx == 2) {
            if (in->mod == 3) d->d[0] = st->xmm[in->rm].d[0];
            else *d = src_rm(st, in, 4);
            return 1;
        }
        if (in->mod == 3) d->q[0] = st->xmm[in->rm].q[0];
        else *d = src_rm(st, in, 8);
        return 1;
    case 0x11:
        store_rm(st, in, d, pfx <= 1 ? 16 : pfx == 2 ? 4 : 8);
        return 1;
    case 0x12:
        if (pfx == 3) { Xmm s = src_rm(st, in, 8); d->q[0] = s.q[0]; d->q[1] = s.q[0]; return 1; }
        if (pfx == 2) { Xmm s = src_rm(st, in, 16); d->d[0] = d->d[1] = s.d[0]; d->d[2] = d->d[3] = s.d[2]; return 1; }
        if (in->mod == 3) d->q[0] = st->xmm[in->rm].q[1];
        else d->q[0] = mem_rd(ea_addr(st, in), 8);
        return 1;
    case 0x13:
        mem_wr(ea_addr(st, in), 8, d->q[0]);
        return 1;
    case 0x14: {
        Xmm s = src_rm(st, in, 16), r;
        if (pfx == 1) { r.q[0] = d->q[0]; r.q[1] = s.q[0]; }
        else { r.d[0] = d->d[0]; r.d[1] = s.d[0]; r.d[2] = d->d[1]; r.d[3] = s.d[1]; }
        *d = r;
        return 1;
    }
    case 0x15: {
        Xmm s = src_rm(st, in, 16), r;
        if (pfx == 1) { r.q[0] = d->q[1]; r.q[1] = s.q[1]; }
        else { r.d[0] = d->d[2]; r.d[1] = s.d[2]; r.d[2] = d->d[3]; r.d[3] = s.d[3]; }
        *d = r;
        return 1;
    }
    case 0x16:
        if (pfx == 2) { Xmm s = src_rm(st, in, 16); d->d[0] = d->d[1] = s.d[1]; d->d[2] = d->d[3] = s.d[3]; return 1; }
        if (in->mod == 3) d->q[1] = st->xmm[in->rm].q[0];
        else d->q[1] = mem_rd(ea_addr(st, in), 8);
        return 1;
    case 0x17:
        mem_wr(ea_addr(st, in), 8, d->q[1]);
        return 1;
    case 0x28: *d = src_rm(st, in, 16); return 1;
    case 0x29: case 0x2B: store_rm(st, in, d, 16); return 1;
    case 0x2A: {
        if (pfx < 2) return 0;
        int isz = (in->rex & 8) ? 8 : 4;
        int64_t v = in->mod == 3 ? sext(st->gpr[in->rm], isz) : sext(mem_rd(ea_addr(st, in), isz), isz);
        if (pfx == 2) d->f[0] = (float)v;
        else d->fd[0] = (double)v;
        return 1;
    }
    case 0x2C: case 0x2D: {
        if (pfx < 2) return 0;
        int dsz = (in->rex & 8) ? 8 : 4;
        Xmm s = src_rm(st, in, pfx == 2 ? 4 : 8);
        double v = (pfx == 2) ? (double)s.f[0] : s.fd[0];
        int64_t r = (op == 0x2C) ? cvt_trunc_i(v, dsz) : cvt_round_i(v, dsz);
        reg_wr(st, in, in->reg, dsz, (uint64_t)r);
        return 1;
    }
    case 0x2E: case 0x2F: {
        Xmm s = src_rm(st, in, pfx == 1 ? 8 : 4);
        if (pfx == 1) set_fcmp_flags(st, d->fd[0], s.fd[0]);
        else set_fcmp_flags(st, d->f[0], s.f[0]);
        return 1;
    }
    case 0x50: {
        Xmm s = st->xmm[in->rm];
        uint32_t m = 0;
        if (pfx == 1) m = (uint32_t)((s.q[0] >> 63) | ((s.q[1] >> 63) << 1));
        else for (int i = 0; i < 4; i++) m |= (s.d[i] >> 31) << i;
        st->gpr[in->reg] = m;
        return 1;
    }
    case 0x51: case 0x52: case 0x53: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        return float_arith(st, in, pfx);
    case 0x54: case 0x55: case 0x56: case 0x57: {
        Xmm s = src_rm(st, in, 16);
        for (int i = 0; i < 2; i++) {
            if (op == 0x54) d->q[i] &= s.q[i];
            else if (op == 0x55) d->q[i] = ~d->q[i] & s.q[i];
            else if (op == 0x56) d->q[i] |= s.q[i];
            else d->q[i] ^= s.q[i];
        }
        return 1;
    }
    case 0x5A: {
        if (pfx == 0) { Xmm s = src_rm(st, in, 8); Xmm r; r.fd[0] = s.f[0]; r.fd[1] = s.f[1]; *d = r; return 1; }
        if (pfx == 1) { Xmm s = src_rm(st, in, 16); Xmm r; memset(&r, 0, 16); r.f[0] = (float)s.fd[0]; r.f[1] = (float)s.fd[1]; *d = r; return 1; }
        if (pfx == 2) { Xmm s = src_rm(st, in, 4); d->fd[0] = s.f[0]; return 1; }
        { Xmm s = src_rm(st, in, 8); d->f[0] = (float)s.fd[0]; return 1; }
    }
    case 0x5B: {
        Xmm s = src_rm(st, in, 16), r;
        for (int i = 0; i < 4; i++) {
            if (pfx == 0) r.f[i] = (float)s.sd[i];
            else if (pfx == 1) r.sd[i] = (int32_t)cvt_round_i(s.f[i], 4);
            else r.sd[i] = (int32_t)cvt_trunc_i(s.f[i], 4);
        }
        *d = r;
        return 1;
    }
    case 0x6E:
        if (pfx != 1) return 0;
        {
            int sz = (in->rex & 8) ? 8 : 4;
            uint64_t v = in->mod == 3 ? (st->gpr[in->rm] & size_mask(sz)) : mem_rd(ea_addr(st, in), sz);
            memset(d, 0, 16);
            d->q[0] = v;
        }
        return 1;
    case 0x6F:
        if (pfx != 1 && pfx != 2) return 0;
        *d = src_rm(st, in, 16);
        return 1;
    case 0x70: {
        Xmm s = src_rm(st, in, 16), r = s;
        unsigned im = (unsigned)(in->imm & 0xFF);
        if (pfx == 1) for (int i = 0; i < 4; i++) r.d[i] = s.d[(im >> (2 * i)) & 3];
        else if (pfx == 2) for (int i = 0; i < 4; i++) r.w[4 + i] = s.w[4 + ((im >> (2 * i)) & 3)];
        else if (pfx == 3) for (int i = 0; i < 4; i++) r.w[i] = s.w[(im >> (2 * i)) & 3];
        else return 0;
        *d = r;
        return 1;
    }
    case 0x71: case 0x72: case 0x73:
        if (pfx != 1 || in->mod != 3) return 0;
        return shift_imm(st, in);
    case 0x7E:
        if (pfx == 1) {
            int sz = (in->rex & 8) ? 8 : 4;
            if (in->mod == 3) st->gpr[in->rm] = d->q[0] & size_mask(sz);
            else mem_wr(ea_addr(st, in), sz, d->q[0]);
            return 1;
        }
        if (pfx == 2) { Xmm s = src_rm(st, in, 8); d->q[0] = s.q[0]; d->q[1] = 0; return 1; }
        return 0;
    case 0x7F:
        if (pfx != 1 && pfx != 2) return 0;
        store_rm(st, in, d, 16);
        return 1;
    case 0xC2: {
        int pred = (int)(in->imm & 7);
        if (pfx == 0 || pfx == 1) {
            Xmm s = src_rm(st, in, 16), r;
            if (pfx == 1) for (int i = 0; i < 2; i++) r.q[i] = fcmp_pred(d->fd[i], s.fd[i], pred) ? ~0ull : 0;
            else for (int i = 0; i < 4; i++) r.d[i] = fcmp_pred(d->f[i], s.f[i], pred) ? ~0u : 0;
            *d = r;
        } else if (pfx == 2) {
            Xmm s = src_rm(st, in, 4);
            d->d[0] = fcmp_pred(d->f[0], s.f[0], pred) ? ~0u : 0;
        } else {
            Xmm s = src_rm(st, in, 8);
            d->q[0] = fcmp_pred(d->fd[0], s.fd[0], pred) ? ~0ull : 0;
        }
        return 1;
    }
    case 0xC4: {
        if (pfx != 1) return 0;
        uint64_t v = in->mod == 3 ? st->gpr[in->rm] : mem_rd(ea_addr(st, in), 2);
        d->w[in->imm & 7] = (uint16_t)v;
        return 1;
    }
    case 0xC5:
        if (pfx != 1 || in->mod != 3) return 0;
        st->gpr[in->reg] = st->xmm[in->rm].w[in->imm & 7];
        return 1;
    case 0xC6: {
        Xmm s = src_rm(st, in, 16), r;
        unsigned im = (unsigned)(in->imm & 0xFF);
        if (pfx == 1) { r.q[0] = d->q[im & 1]; r.q[1] = s.q[(im >> 1) & 1]; }
        else {
            r.d[0] = d->d[im & 3]; r.d[1] = d->d[(im >> 2) & 3];
            r.d[2] = s.d[(im >> 4) & 3]; r.d[3] = s.d[(im >> 6) & 3];
        }
        *d = r;
        return 1;
    }
    case 0xD6:
        if (pfx != 1) return 0;
        if (in->mod == 3) { st->xmm[in->rm].q[0] = d->q[0]; st->xmm[in->rm].q[1] = 0; }
        else mem_wr(ea_addr(st, in), 8, d->q[0]);
        return 1;
    case 0xD7: {
        if (pfx != 1) return 0;
        Xmm s = st->xmm[in->rm];
        uint32_t m = 0;
        for (int i = 0; i < 16; i++) m |= (uint32_t)(s.b[i] >> 7) << i;
        st->gpr[in->reg] = m;
        return 1;
    }
    case 0xE6: {
        Xmm r;
        memset(&r, 0, 16);
        if (pfx == 2) { Xmm s = src_rm(st, in, 8); r.fd[0] = s.sd[0]; r.fd[1] = s.sd[1]; }
        else if (pfx == 1) { Xmm s = src_rm(st, in, 16); r.sd[0] = (int32_t)cvt_trunc_i(s.fd[0], 4); r.sd[1] = (int32_t)cvt_trunc_i(s.fd[1], 4); }
        else if (pfx == 3) { Xmm s = src_rm(st, in, 16); r.sd[0] = (int32_t)cvt_round_i(s.fd[0], 4); r.sd[1] = (int32_t)cvt_round_i(s.fd[1], 4); }
        else return 0;
        *d = r;
        return 1;
    }
    case 0xE7:
        if (pfx != 1) return 0;
        store_rm(st, in, d, 16);
        return 1;
    default:
        break;
    }
    if (pfx == 1 && ((op >= 0x60 && op <= 0x6D) || (op >= 0x74 && op <= 0x76) || op >= 0xD1))
        return packed_int(st, in);
    return 0;
}
