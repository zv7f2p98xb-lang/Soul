// SoulCore - x86-64 instruction decoder.
#include "decode.h"
#include <string.h>

extern int g_mode32;

enum { I_NONE, I_B, I_W, I_Z, I_V, I_ENTER, I_MOFFS, I_REL8, I_REL32 };

static int map1_modrm(uint8_t op) {
    if (op < 0x40) return (op & 7) < 4;
    switch (op) {
    case 0x62: case 0x63: case 0x69: case 0x6B:
    case 0xC0: case 0xC1: case 0xC4: case 0xC5: case 0xC6: case 0xC7:
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xF6: case 0xF7: case 0xFE: case 0xFF:
        return 1;
    default:
        break;
    }
    if (op >= 0x80 && op <= 0x8F) return 1;
    if (op >= 0xD8 && op <= 0xDF) return 1;
    return 0;
}

static int map1_imm(uint8_t op, uint8_t reg3) {
    if (op < 0x40) {
        if ((op & 7) == 4) return I_B;
        if ((op & 7) == 5) return I_Z;
        return I_NONE;
    }
    if (op >= 0x70 && op <= 0x7F) return I_REL8;
    if (op >= 0xB0 && op <= 0xB7) return I_B;
    if (op >= 0xB8 && op <= 0xBF) return I_V;
    if (op >= 0xA0 && op <= 0xA3) return I_MOFFS;
    switch (op) {
    case 0x68: case 0x69: case 0x81: case 0xA9: case 0xC7: return I_Z;
    case 0x6A: case 0x6B: case 0x80: case 0x82: case 0x83: case 0xA8:
    case 0xC0: case 0xC1: case 0xC6: case 0xCD: case 0xD4: case 0xD5:
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
        return I_B;
    case 0xC2: case 0xCA: return I_W;
    case 0xC8: return I_ENTER;
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xEB: return I_REL8;
    case 0xE8: case 0xE9: return I_REL32;
    case 0xF6: return (reg3 < 2) ? I_B : I_NONE;
    case 0xF7: return (reg3 < 2) ? I_Z : I_NONE;
    default: return I_NONE;
    }
}

static int map2_modrm(uint8_t op) {
    switch (op) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B: case 0x0E:
    case 0x77: case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
        return 0;
    default:
        break;
    }
    if (op >= 0x30 && op <= 0x37) return 0;
    if (op >= 0x80 && op <= 0x8F) return 0;
    if (op >= 0xC8 && op <= 0xCF) return 0;
    return 1;
}

static int map2_imm(uint8_t op) {
    if (op >= 0x80 && op <= 0x8F) return I_REL32;
    switch (op) {
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0xA4: case 0xAC: case 0xBA: case 0xC2: case 0xC4: case 0xC5: case 0xC6:
        return I_B;
    default:
        return I_NONE;
    }
}

int x86_decode(const uint8_t *p, uint64_t addr, X86Insn *in) {
    const uint8_t *start = p;
    memset(in, 0, sizeof(*in));
    in->addr = addr;
    in->ea_base = EA_NONE;
    in->ea_index = EA_NONE;

    // Legacy prefixes and REX.
    for (int n = 0; n < 15; n++) {
        uint8_t b = *p;
        if (b == 0x66) { in->p66 = 1; in->rex = 0; p++; continue; }
        if (b == 0x67) { in->adsize32 = 1; in->rex = 0; p++; continue; }
        if (b == 0xF2 || b == 0xF3) { in->rep = b; in->rex = 0; p++; continue; }
        if (b == 0xF0) { in->lock = 1; in->rex = 0; p++; continue; }
        if (b == 0x64) { in->seg = SEG_FS; in->rex = 0; p++; continue; }
        if (b == 0x65) { in->seg = SEG_GS; in->rex = 0; p++; continue; }
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) { in->rex = 0; p++; continue; }
        if ((b & 0xF0) == 0x40 && !g_mode32) { in->rex = b; p++; continue; }
        break;
    }
    if (g_mode32) in->adsize32 = 1; // 32-bit addressing (0x67 16-bit addressing is not supported)
    uint8_t rexW = (in->rex >> 3) & 1, rexR = (in->rex >> 2) & 1, rexX = (in->rex >> 1) & 1, rexB = in->rex & 1;
    in->opsize = rexW ? 8 : (in->p66 ? 2 : 4);

    uint8_t op = *p++;
    int has_modrm, immk;
    if (op == 0x0F) {
        uint8_t op2 = *p++;
        if (op2 == 0x38) {
            in->map = MAP_0F38;
            in->op = *p++;
            has_modrm = 1;
            immk = I_NONE;
        } else if (op2 == 0x3A) {
            in->map = MAP_0F3A;
            in->op = *p++;
            has_modrm = 1;
            immk = I_B;
        } else if (op2 == 0x0F) {
            return -1; // 3DNow!
        } else {
            in->map = MAP_0F;
            in->op = op2;
            has_modrm = map2_modrm(op2);
            immk = map2_imm(op2);
        }
    } else {
        if (g_mode32 && (op == 0x9A || op == 0xEA)) return -1; // far call / jmp
        if (op == 0xC4 || op == 0xC5 || op == 0x62 || op == 0x8F) {
            // VEX / EVEX / XOP. 0x8F /0 is POP Ev, everything else is XOP.
            if (op == 0x8F && ((p[0] >> 3) & 7) == 0) {
                // regular POP Ev
            } else if (op != 0x8F) {
                return -1;
            } else {
                return -1;
            }
        }
        in->map = MAP_1;
        in->op = op;
        has_modrm = map1_modrm(op);
        immk = -1; // resolved after modrm
    }

    uint8_t reg3 = 0;
    if (has_modrm) {
        uint8_t m = *p++;
        in->has_modrm = 1;
        in->mod = m >> 6;
        reg3 = (m >> 3) & 7;
        in->reg = (uint8_t)(reg3 | (rexR << 3));
        uint8_t rm = m & 7;
        if (in->mod == 3) {
            in->rm = (uint8_t)(rm | (rexB << 3));
        } else {
            if (rm == 4) {
                uint8_t sib = *p++;
                in->ea_scale = sib >> 6;
                uint8_t idx = (uint8_t)(((sib >> 3) & 7) | (rexX << 3));
                in->ea_index = (idx == 4) ? EA_NONE : idx;
                uint8_t base = sib & 7;
                if (base == 5 && in->mod == 0) {
                    in->ea_base = EA_NONE;
                    int32_t d; memcpy(&d, p, 4); p += 4; in->disp = d;
                } else {
                    in->ea_base = (uint8_t)(base | (rexB << 3));
                }
            } else if (rm == 5 && in->mod == 0) {
                in->ea_base = g_mode32 ? EA_NONE : EA_RIP; // 32-bit mode: absolute disp32
                int32_t d; memcpy(&d, p, 4); p += 4; in->disp = d;
            } else {
                in->ea_base = (uint8_t)(rm | (rexB << 3));
            }
            if (in->mod == 1) { in->disp = (int8_t)*p++; }
            else if (in->mod == 2) { int32_t d; memcpy(&d, p, 4); p += 4; in->disp = d; }
            in->rm = (uint8_t)(rm | (rexB << 3));
        }
    }
    if (immk == -1) immk = map1_imm(op, reg3);

    switch (immk) {
    case I_NONE: break;
    case I_B: case I_REL8:
        in->imm = (uint64_t)(int64_t)(int8_t)*p++; in->imm_size = 1; break;
    case I_W: { uint16_t w; memcpy(&w, p, 2); p += 2; in->imm = w; in->imm_size = 2; break; }
    case I_Z:
        if (in->opsize == 2) { int16_t w; memcpy(&w, p, 2); p += 2; in->imm = (uint64_t)(int64_t)w; in->imm_size = 2; }
        else { int32_t d; memcpy(&d, p, 4); p += 4; in->imm = (uint64_t)(int64_t)d; in->imm_size = 4; }
        break;
    case I_REL32: { int32_t d; memcpy(&d, p, 4); p += 4; in->imm = (uint64_t)(int64_t)d; in->imm_size = 4; break; }
    case I_V:
        if (in->opsize == 8) { uint64_t q; memcpy(&q, p, 8); p += 8; in->imm = q; in->imm_size = 8; }
        else if (in->opsize == 2) { uint16_t w; memcpy(&w, p, 2); p += 2; in->imm = w; in->imm_size = 2; }
        else { uint32_t d; memcpy(&d, p, 4); p += 4; in->imm = d; in->imm_size = 4; }
        break;
    case I_ENTER: { uint16_t w; memcpy(&w, p, 2); p += 2; in->imm = w; in->imm2 = *p++; break; }
    case I_MOFFS:
        if (in->adsize32) { uint32_t d; memcpy(&d, p, 4); p += 4; in->imm2 = d; }
        else { uint64_t q; memcpy(&q, p, 8); p += 8; in->imm2 = q; }
        break;
    }
    long len = p - start;
    if (len > 15) return -1;
    in->len = (uint8_t)len;
    return 0;
}
