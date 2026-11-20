// SoulCore - minimal AArch64 instruction encoder used by the JIT.
#pragma once
#include <stdint.h>

typedef struct Emit {
    uint32_t *start;
    uint32_t *p;
    uint32_t *end;
    int overflow;
} Emit;

static inline void em(Emit *e, uint32_t ins) {
    if (e->p >= e->end) { e->overflow = 1; return; }
    *e->p++ = ins;
}

enum { XZR = 31, SP_REG = 31 };
enum { C_EQ = 0, C_NE, C_HS, C_LO, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL };

#define SF(sf) ((uint32_t)((sf) ? 1u : 0u) << 31)

// ---- data processing (register) --------------------------------------------------------------
// shift: 0 LSL, 1 LSR, 2 ASR
static inline uint32_t a_addsub_reg(int sf, int sub, int setf, int rd, int rn, int rm, int shift, int amount) {
    return SF(sf) | ((uint32_t)sub << 30) | ((uint32_t)setf << 29) | 0x0B000000u | ((uint32_t)shift << 22) |
           ((uint32_t)rm << 16) | ((uint32_t)(amount & 63) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
// opc: 0 AND, 1 ORR, 2 EOR, 3 ANDS ; n = invert rm (BIC/ORN/EON/BICS)
static inline uint32_t a_logic_reg(int sf, int opc, int n, int rd, int rn, int rm, int shift, int amount) {
    return SF(sf) | ((uint32_t)opc << 29) | 0x0A000000u | ((uint32_t)shift << 22) | ((uint32_t)n << 21) |
           ((uint32_t)rm << 16) | ((uint32_t)(amount & 63) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a_addsub_imm(int sf, int sub, int setf, int rd, int rn, uint32_t imm12, int sh12) {
    return SF(sf) | ((uint32_t)sub << 30) | ((uint32_t)setf << 29) | 0x11000000u | ((uint32_t)sh12 << 22) |
           ((imm12 & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a_movz(int sf, int rd, uint32_t imm16, int hw) { return SF(sf) | 0x52800000u | ((uint32_t)hw << 21) | ((imm16 & 0xFFFF) << 5) | (uint32_t)rd; }
static inline uint32_t a_movk(int sf, int rd, uint32_t imm16, int hw) { return SF(sf) | 0x72800000u | ((uint32_t)hw << 21) | ((imm16 & 0xFFFF) << 5) | (uint32_t)rd; }
static inline uint32_t a_movn(int sf, int rd, uint32_t imm16, int hw) { return SF(sf) | 0x12800000u | ((uint32_t)hw << 21) | ((imm16 & 0xFFFF) << 5) | (uint32_t)rd; }
static inline uint32_t a_mov(int sf, int rd, int rm) { return a_logic_reg(sf, 1, 0, rd, XZR, rm, 0, 0); }
// bitfield: opc 0 SBFM, 1 BFM, 2 UBFM
static inline uint32_t a_bfm(int sf, int opc, int rd, int rn, int immr, int imms) {
    return SF(sf) | ((uint32_t)opc << 29) | 0x13000000u | ((uint32_t)(sf ? 1 : 0) << 22) | ((uint32_t)immr << 16) | ((uint32_t)imms << 10) |
           ((uint32_t)rn << 5) | (uint32_t)rd;
}
static inline uint32_t a_lsl(int sf, int rd, int rn, int s) { int w = sf ? 64 : 32; return a_bfm(sf, 2, rd, rn, (w - s) % w, w - 1 - s); }
static inline uint32_t a_lsr(int sf, int rd, int rn, int s) { return a_bfm(sf, 2, rd, rn, s, sf ? 63 : 31); }
static inline uint32_t a_asr(int sf, int rd, int rn, int s) { return a_bfm(sf, 0, rd, rn, s, sf ? 63 : 31); }
static inline uint32_t a_sxt(int rd, int rn, int bits) { return a_bfm(1, 0, rd, rn, 0, bits - 1); }      // sign extend to 64
static inline uint32_t a_uxt(int rd, int rn, int bits) { return a_bfm(0, 2, rd, rn, 0, bits - 1); }      // zero extend (w)
static inline uint32_t a_bfi(int sf, int rd, int rn, int lsb, int width) { int w = sf ? 64 : 32; return a_bfm(sf, 1, rd, rn, (w - lsb) % w, width - 1); }
// variable shifts: op2 0 LSLV, 1 LSRV, 2 ASRV
static inline uint32_t a_shiftv(int sf, int op2, int rd, int rn, int rm) { return SF(sf) | 0x1AC02000u | ((uint32_t)rm << 16) | ((uint32_t)op2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_mul(int sf, int rd, int rn, int rm) { return SF(sf) | 0x1B007C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_msub(int sf, int rd, int rn, int rm, int ra) { return SF(sf) | 0x1B008000u | ((uint32_t)rm << 16) | ((uint32_t)ra << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_udiv(int sf, int rd, int rn, int rm) { return SF(sf) | 0x1AC00800u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_sdiv(int sf, int rd, int rn, int rm) { return SF(sf) | 0x1AC00C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_smulh(int rd, int rn, int rm) { return 0x9B407C00u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_csel(int sf, int rd, int rn, int rm, int cond) { return SF(sf) | 0x1A800000u | ((uint32_t)rm << 16) | ((uint32_t)cond << 12) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_cset(int sf, int rd, int cond) { return SF(sf) | 0x1A9F07E0u | ((uint32_t)(cond ^ 1) << 12) | (uint32_t)rd; }
static inline uint32_t a_rev(int sf, int rd, int rn) { return sf ? (0xDAC00C00u | ((uint32_t)rn << 5) | (uint32_t)rd) : (0x5AC00800u | ((uint32_t)rn << 5) | (uint32_t)rd); }

// ---- loads / stores ------------------------------------------------------------------------
// size: 0 byte, 1 half, 2 word, 3 dword. Unsigned scaled immediate offset.
static inline uint32_t a_ldr_imm(int size, int rt, int rn, uint32_t off) {
    return ((uint32_t)size << 30) | 0x39400000u | (((off >> size) & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a_str_imm(int size, int rt, int rn, uint32_t off) {
    return ((uint32_t)size << 30) | 0x39000000u | (((off >> size) & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
// sign-extending loads into X: size 0 LDRSB, 1 LDRSH, 2 LDRSW
static inline uint32_t a_ldrs_imm(int size, int rt, int rn, uint32_t off) {
    return ((uint32_t)size << 30) | 0x39800000u | (((off >> size) & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
// SIMD&FP: size 2 = S, 3 = D, 4 = Q
static inline uint32_t a_ldr_fp(int size, int rt, int rn, uint32_t off) {
    if (size == 4) return 0x3DC00000u | (((off >> 4) & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
    return ((uint32_t)size << 30) | 0x3D400000u | (((off >> size) & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a_str_fp(int size, int rt, int rn, uint32_t off) {
    if (size == 4) return 0x3D800000u | (((off >> 4) & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
    return ((uint32_t)size << 30) | 0x3D000000u | (((off >> size) & 0xFFF) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static inline uint32_t a_stp_pre(int rt, int rt2, int rn, int imm) { return 0xA9800000u | (((uint32_t)(imm / 8) & 0x7F) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }
static inline uint32_t a_ldp_post(int rt, int rt2, int rn, int imm) { return 0xA8C00000u | (((uint32_t)(imm / 8) & 0x7F) << 15) | ((uint32_t)rt2 << 10) | ((uint32_t)rn << 5) | (uint32_t)rt; }

// ---- branches -------------------------------------------------------------------------------
static inline uint32_t a_b(int32_t words) { return 0x14000000u | ((uint32_t)words & 0x3FFFFFF); }
static inline uint32_t a_bcond(int cond, int32_t words) { return 0x54000000u | (((uint32_t)words & 0x7FFFF) << 5) | (uint32_t)cond; }
static inline uint32_t a_cbnz(int sf, int rt, int32_t words) { return SF(sf) | 0x35000000u | (((uint32_t)words & 0x7FFFF) << 5) | (uint32_t)rt; }
static inline uint32_t a_cbz(int sf, int rt, int32_t words) { return SF(sf) | 0x34000000u | (((uint32_t)words & 0x7FFFF) << 5) | (uint32_t)rt; }
static inline uint32_t a_blr(int rn) { return 0xD63F0000u | ((uint32_t)rn << 5); }
static inline uint32_t a_br(int rn) { return 0xD61F0000u | ((uint32_t)rn << 5); }
static inline uint32_t a_ret(void) { return 0xD65F03C0u; }

// ---- floating point (scalar). type 0 = single, 1 = double ---------------------------------------
// opcode: 0 FMUL, 1 FDIV, 2 FADD, 3 FSUB, 4 FMAX, 5 FMIN
static inline uint32_t a_fp2(int dbl, int op, int rd, int rn, int rm) { return 0x1E200800u | ((uint32_t)dbl << 22) | ((uint32_t)rm << 16) | ((uint32_t)op << 12) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_fsqrt(int dbl, int rd, int rn) { return 0x1E21C000u | ((uint32_t)dbl << 22) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_fcmp(int dbl, int rn, int rm) { return 0x1E202000u | ((uint32_t)dbl << 22) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5); }
static inline uint32_t a_scvtf(int sf, int dbl, int rd, int rn) { return SF(sf) | 0x1E220000u | ((uint32_t)dbl << 22) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_fcvtzs(int sf, int dbl, int rd, int rn) { return SF(sf) | 0x1E380000u | ((uint32_t)dbl << 22) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_fcvt_s2d(int rd, int rn) { return 0x1E22C000u | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_fcvt_d2s(int rd, int rn) { return 0x1E624000u | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_fmov_to_gpr(int dbl, int rd, int rn) { return dbl ? (0x9E660000u | ((uint32_t)rn << 5) | (uint32_t)rd) : (0x1E260000u | ((uint32_t)rn << 5) | (uint32_t)rd); }
static inline uint32_t a_fmov_from_gpr(int dbl, int rd, int rn) { return dbl ? (0x9E670000u | ((uint32_t)rn << 5) | (uint32_t)rd) : (0x1E270000u | ((uint32_t)rn << 5) | (uint32_t)rd); }
// vector integer: size 0 B, 1 H, 2 S, 3 D (Q = 1)
static inline uint32_t a_vadd(int size, int rd, int rn, int rm) { return 0x4E208400u | ((uint32_t)size << 22) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_vsub(int size, int rd, int rn, int rm) { return 0x6E208400u | ((uint32_t)size << 22) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_vcmeq(int size, int rd, int rn, int rm) { return 0x6E208C00u | ((uint32_t)size << 22) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_vcmgt(int size, int rd, int rn, int rm) { return 0x4E203400u | ((uint32_t)size << 22) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_vzip1(int size, int rd, int rn, int rm) { return 0x4E003800u | ((uint32_t)size << 22) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static inline uint32_t a_vzip2(int size, int rd, int rn, int rm) { return 0x4E007800u | ((uint32_t)size << 22) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
// vector FP: dbl 0 = 4S, 1 = 2D ; op 0 FADD, 1 FSUB, 2 FMUL, 3 FDIV
static inline uint32_t a_vfp(int dbl, int op, int rd, int rn, int rm) {
    static const uint32_t base[4] = { 0x4E20D400u, 0x4EA0D400u, 0x6E20DC00u, 0x6E20FC00u };
    return base[op] | ((uint32_t)dbl << 22) | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
// vector 16B logic: opc 0 AND, 1 BIC, 2 ORR, 3 EOR
static inline uint32_t a_vlogic(int op, int rd, int rn, int rm) {
    static const uint32_t base[4] = { 0x4E201C00u, 0x4E601C00u, 0x4EA01C00u, 0x6E201C00u };
    return base[op] | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
