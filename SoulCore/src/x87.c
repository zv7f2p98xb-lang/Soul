// SoulCore - minimal x87 FPU (values kept as host doubles).
#include "interp.h"
#include "soul_internal.h"
#include <math.h>

void x87_reset(X86State *st) {
    st->ftop = 0;
    st->fcw = 0x037F;
    st->fsw = 0;
    for (int i = 0; i < 8; i++) { st->fpr[i] = 0; st->ftag[i] = 1; }
}

static inline double *sti(X86State *st, int i) { return &st->fpr[(st->ftop + (unsigned)i) & 7]; }
static inline void fpush(X86State *st, double v) {
    st->ftop = (st->ftop - 1) & 7;
    st->fpr[st->ftop] = v;
    st->ftag[st->ftop] = 0;
}
static inline double fpop(X86State *st) {
    double v = st->fpr[st->ftop];
    st->ftag[st->ftop] = 1;
    st->ftop = (st->ftop + 1) & 7;
    return v;
}
static inline uint16_t fsw_value(X86State *st) { return (uint16_t)((st->fsw & ~0x3800u) | ((st->ftop & 7) << 11)); }

static double f80_to_double(const uint8_t *p) {
    uint64_t mant; uint16_t se;
    memcpy(&mant, p, 8); memcpy(&se, p + 8, 2);
    int sign = se >> 15;
    int exp = se & 0x7FFF;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7FFF) v = (mant << 1) ? NAN : INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
static void double_to_f80(double v, uint8_t *p) {
    uint64_t mant = 0; uint16_t se = 0;
    if (signbit(v)) se = 0x8000;
    double a = fabs(v);
    if (a == 0.0) {
    } else if (isinf(a)) { se |= 0x7FFF; mant = 0x8000000000000000ull; }
    else if (isnan(a)) { se |= 0x7FFF; mant = 0xC000000000000000ull; }
    else {
        int e;
        double m = frexp(a, &e); // a = m * 2^e, m in [0.5,1)
        mant = (uint64_t)ldexp(m, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8); memcpy(p + 8, &se, 2);
}

static double fround(X86State *st, double v) {
    switch ((st->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

static void fcom_flags(X86State *st, double a, double b) {
    uint16_t c = 0; // C0 = 0x100, C2 = 0x400, C3 = 0x4000
    if (isnan(a) || isnan(b)) c = 0x4500;
    else if (a < b) c = 0x0100;
    else if (a == b) c = 0x4000;
    st->fsw = (uint16_t)((st->fsw & ~0x4500u) | c);
}
static void fcomi_flags(X86State *st, double a, double b) {
    uint64_t f;
    if (isnan(a) || isnan(b)) f = F_ZF | F_PF | F_CF;
    else if (a < b) f = F_CF;
    else if (a == b) f = F_ZF;
    else f = 0;
    st->eflags = (st->eflags & ~(uint64_t)F_ARITH) | f;
    st->lf_op = LF_NONE;
}

static double arith(int op, double a, double b) {
    switch (op) {
    case 0: return a + b;
    case 1: return a * b;
    case 4: return a - b;
    case 5: return b - a;
    case 6: return a / b;
    case 7: return b / a;
    default: return a;
    }
}

int x87_exec(X86State *st, const X86Insn *in) {
    uint8_t op = in->op;
    int r = in->reg & 7;
    if (in->mod != 3) {
        uint64_t a = ea_addr(st, in);
        switch (op) {
        case 0xD8: case 0xDC: { // arith with m32 / m64
            double v = (op == 0xD8) ? (double)({ float f; memcpy(&f, (void *)(uintptr_t)a, 4); f; })
                                    : ({ double dd; memcpy(&dd, (void *)(uintptr_t)a, 8); dd; });
            if (r == 2 || r == 3) { fcom_flags(st, *sti(st, 0), v); if (r == 3) fpop(st); return 1; }
            *sti(st, 0) = arith(r, *sti(st, 0), v);
            return 1;
        }
        case 0xDA: case 0xDE: { // arith with m32int / m16int
            double v = (op == 0xDA) ? (double)(int32_t)mem_rd(a, 4) : (double)(int16_t)mem_rd(a, 2);
            if (r == 2 || r == 3) { fcom_flags(st, *sti(st, 0), v); if (r == 3) fpop(st); return 1; }
            *sti(st, 0) = arith(r, *sti(st, 0), v);
            return 1;
        }
        case 0xD9:
            switch (r) {
            case 0: { float f; memcpy(&f, (void *)(uintptr_t)a, 4); fpush(st, f); return 1; }
            case 2: case 3: { float f = (float)*sti(st, 0); memcpy((void *)(uintptr_t)a, &f, 4); if (r == 3) fpop(st); return 1; }
            case 4: return 1; // FLDENV
            case 5: st->fcw = (uint16_t)mem_rd(a, 2); return 1;
            case 6: { uint8_t buf[28]; memset(buf, 0, sizeof buf); memcpy(buf, &st->fcw, 2); uint16_t sw = fsw_value(st); memcpy(buf + 4, &sw, 2); memcpy((void *)(uintptr_t)a, buf, 28); return 1; }
            case 7: mem_wr(a, 2, st->fcw); return 1;
            default: return 0;
            }
        case 0xDB:
            switch (r) {
            case 0: fpush(st, (double)(int32_t)mem_rd(a, 4)); return 1;
            case 1: mem_wr(a, 4, (uint32_t)(int32_t)trunc(fpop(st))); return 1;
            case 2: case 3: { double v = fround(st, *sti(st, 0)); mem_wr(a, 4, (uint32_t)(int32_t)v); if (r == 3) fpop(st); return 1; }
            case 5: fpush(st, f80_to_double((const uint8_t *)(uintptr_t)a)); return 1;
            case 7: double_to_f80(fpop(st), (uint8_t *)(uintptr_t)a); return 1;
            default: return 0;
            }
        case 0xDD:
            switch (r) {
            case 0: { double dd; memcpy(&dd, (void *)(uintptr_t)a, 8); fpush(st, dd); return 1; }
            case 1: mem_wr(a, 8, (uint64_t)(int64_t)trunc(fpop(st))); return 1;
            case 2: case 3: { double dd = *sti(st, 0); memcpy((void *)(uintptr_t)a, &dd, 8); if (r == 3) fpop(st); return 1; }
            case 4: return 1; // FRSTOR
            case 6: memset((void *)(uintptr_t)a, 0, 108); x87_reset(st); return 1; // FNSAVE
            case 7: mem_wr(a, 2, fsw_value(st)); return 1;
            default: return 0;
            }
        case 0xDF:
            switch (r) {
            case 0: fpush(st, (double)(int16_t)mem_rd(a, 2)); return 1;
            case 1: mem_wr(a, 2, (uint16_t)(int16_t)trunc(fpop(st))); return 1;
            case 2: case 3: { double v = fround(st, *sti(st, 0)); mem_wr(a, 2, (uint16_t)(int16_t)v); if (r == 3) fpop(st); return 1; }
            case 5: fpush(st, (double)(int64_t)mem_rd(a, 8)); return 1;
            case 7: { double v = fround(st, fpop(st)); mem_wr(a, 8, (uint64_t)(int64_t)v); return 1; }
            default: return 0;
            }
        default:
            return 0;
        }
    }
    int i = in->rm & 7;
    switch (op) {
    case 0xD8:
        if (r == 2 || r == 3) { fcom_flags(st, *sti(st, 0), *sti(st, i)); if (r == 3) fpop(st); return 1; }
        *sti(st, 0) = arith(r, *sti(st, 0), *sti(st, i));
        return 1;
    case 0xDC:
    case 0xDE:
        if (op == 0xDC && (r == 2 || r == 3)) { fcom_flags(st, *sti(st, 0), *sti(st, i)); if (r == 3) fpop(st); return 1; }
        if (op == 0xDE && r == 3 && i == 1) { fcom_flags(st, *sti(st, 0), *sti(st, 1)); fpop(st); fpop(st); return 1; }
        {
            // FADDP, FMULP, FSUBRP(4), FSUBP(5), FDIVRP(6), FDIVP(7): ST(i) = ST(i) op ST(0)
            double a0 = *sti(st, 0), ai = *sti(st, i), v;
            switch (r) {
            case 0: v = ai + a0; break;
            case 1: v = ai * a0; break;
            case 4: v = a0 - ai; break;
            case 5: v = ai - a0; break;
            case 6: v = a0 / ai; break;
            case 7: v = ai / a0; break;
            default: return 0;
            }
            *sti(st, i) = v;
            if (op == 0xDE) fpop(st);
        }
        return 1;
    case 0xD9:
        if (r == 0) { fpush(st, *sti(st, i)); return 1; }
        if (r == 1) { double t = *sti(st, 0); *sti(st, 0) = *sti(st, i); *sti(st, i) = t; return 1; }
        switch ((in->rm & 7) | (r << 3)) {
        case 0x10: return 1; // FNOP
        case 0x20: *sti(st, 0) = -*sti(st, 0); return 1;
        case 0x21: *sti(st, 0) = fabs(*sti(st, 0)); return 1;
        case 0x24: fcom_flags(st, *sti(st, 0), 0.0); return 1;
        case 0x25: { // FXAM
            double v = *sti(st, 0);
            uint16_t c = 0;
            if (st->ftag[st->ftop]) c = 0x4100;
            else if (isnan(v)) c = 0x0100;
            else if (isinf(v)) c = 0x0500;
            else if (v == 0) c = 0x4000;
            else c = 0x0400;
            if (signbit(v)) c |= 0x0200;
            st->fsw = (uint16_t)((st->fsw & ~0x4700u) | c);
            return 1;
        }
        case 0x28: fpush(st, 1.0); return 1;
        case 0x29: fpush(st, 3.321928094887362347870319429489390175864831393); return 1;
        case 0x2A: fpush(st, 1.442695040888963407359924681001892137426645954); return 1;
        case 0x2B: fpush(st, 3.141592653589793238462643383279502884197169399); return 1;
        case 0x2C: fpush(st, 0.301029995663981195213738894724493026768189881); return 1;
        case 0x2D: fpush(st, 0.693147180559945309417232121458176568075500134); return 1;
        case 0x2E: fpush(st, 0.0); return 1;
        case 0x30: { double v = fpop(st); fpush(st, exp2(v) - 1.0); return 1; } // F2XM1
        case 0x31: { double x = fpop(st); *sti(st, 0) = *sti(st, 0) * log2(x); return 1; } // FYL2X
        case 0x32: { double v = fpop(st); fpush(st, tan(v)); fpush(st, 1.0); return 1; }
        case 0x33: { double x = fpop(st); *sti(st, 0) = atan2(*sti(st, 0), x); return 1; }
        case 0x34: { double v = fpop(st); int e; double m = frexp(v, &e); fpush(st, (double)(e - 1)); fpush(st, m * 2); return 1; }
        case 0x35: case 0x38: { *sti(st, 0) = remainder(*sti(st, 0), *sti(st, 1)); st->fsw &= ~0x0400; return 1; }
        case 0x36: st->ftop = (st->ftop - 1) & 7; return 1;
        case 0x37: st->ftop = (st->ftop + 1) & 7; return 1;
        case 0x39: { double x = fpop(st); *sti(st, 0) = *sti(st, 0) * log2(x + 1.0); return 1; }
        case 0x3A: *sti(st, 0) = sqrt(*sti(st, 0)); return 1;
        case 0x3B: { double v = fpop(st); fpush(st, sin(v)); fpush(st, cos(v)); return 1; }
        case 0x3C: *sti(st, 0) = fround(st, *sti(st, 0)); return 1;
        case 0x3D: *sti(st, 0) = ldexp(*sti(st, 0), (int)trunc(*sti(st, 1))); return 1;
        case 0x3E: *sti(st, 0) = sin(*sti(st, 0)); return 1;
        case 0x3F: *sti(st, 0) = cos(*sti(st, 0)); return 1;
        default: return 0;
        }
    case 0xDA:
        if (r == 5 && i == 1) { fcom_flags(st, *sti(st, 0), *sti(st, 1)); fpop(st); fpop(st); return 1; }
        { // FCMOVB/E/BE/U
            uint64_t f = x86_flags(st);
            int c = r == 0 ? (f & F_CF) != 0 : r == 1 ? (f & F_ZF) != 0 : r == 2 ? (f & (F_CF | F_ZF)) != 0 : r == 3 ? (f & F_PF) != 0 : -1;
            if (c < 0) return 0;
            if (c) *sti(st, 0) = *sti(st, i);
            return 1;
        }
    case 0xDB:
        if (r == 4) {
            if (i == 2) { st->fsw &= 0x7F00; return 1; } // FNCLEX
            if (i == 3) { x87_reset(st); return 1; }      // FNINIT
            return 1;
        }
        if (r == 5 || r == 6) { fcomi_flags(st, *sti(st, 0), *sti(st, i)); return 1; }
        {
            uint64_t f = x86_flags(st);
            int c = r == 0 ? !(f & F_CF) : r == 1 ? !(f & F_ZF) : r == 2 ? !(f & (F_CF | F_ZF)) : r == 3 ? !(f & F_PF) : -1;
            if (c < 0) return 0;
            if (c) *sti(st, 0) = *sti(st, i);
            return 1;
        }
    case 0xDD:
        if (r == 0) { st->ftag[(st->ftop + (unsigned)i) & 7] = 1; return 1; } // FFREE
        if (r == 2) { *sti(st, i) = *sti(st, 0); return 1; }
        if (r == 3) { *sti(st, i) = *sti(st, 0); fpop(st); return 1; }
        if (r == 4 || r == 5) { fcom_flags(st, *sti(st, 0), *sti(st, i)); if (r == 5) fpop(st); return 1; }
        return 0;
    case 0xDF:
        if (r == 4 && i == 0) { st->gpr[RAX] = (st->gpr[RAX] & ~0xFFFFull) | fsw_value(st); return 1; }
        if (r == 5 || r == 6) { fcomi_flags(st, *sti(st, 0), *sti(st, i)); fpop(st); return 1; }
        return 0;
    default:
        return 0;
    }
}
