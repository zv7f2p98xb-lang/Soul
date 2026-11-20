// SoulCore - x86-64 instruction decoder.
#pragma once
#include <stdint.h>

#define EA_NONE 0xFF
#define EA_RIP 0xFE

#define SEG_NONE 0
#define SEG_FS 4
#define SEG_GS 5

enum { MAP_1 = 0, MAP_0F = 1, MAP_0F38 = 2, MAP_0F3A = 3 };

typedef struct X86Insn {
    uint64_t addr;
    uint8_t len;
    uint8_t map;
    uint8_t op;
    uint8_t opsize;   // 2, 4 or 8 for "v" sized operands
    uint8_t rex;      // raw REX byte (0 if none)
    uint8_t p66;      // operand size prefix present
    uint8_t rep;      // 0, 0xF2 or 0xF3
    uint8_t lock;
    uint8_t seg;
    uint8_t adsize32; // 0x67 prefix
    uint8_t has_modrm;
    uint8_t mod;      // modrm.mod (3 = register)
    uint8_t reg;      // modrm.reg with REX.R
    uint8_t rm;       // modrm.rm with REX.B (register form)
    uint8_t ea_base;  // register, EA_RIP or EA_NONE
    uint8_t ea_index; // register or EA_NONE
    uint8_t ea_scale; // shift amount 0..3
    uint8_t imm_size;
    int32_t disp;
    uint64_t imm;     // sign-extended immediate (or rel displacement)
    uint64_t imm2;    // ENTER second operand / moffs
} X86Insn;

// Returns 0 on success, -1 on undecodable bytes.
int x86_decode(const uint8_t *code, uint64_t addr, X86Insn *in);

static inline int insn_is_mem(const X86Insn *in) { return in->has_modrm && in->mod != 3; }
