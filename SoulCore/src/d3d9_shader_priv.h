// SoulCore - Direct3D 9 shader engine, private definitions shared by the decoder (d3d9_shader.c)
// and the interpreter (d3d9_shader_exec.c).
#pragma once
#include "d3d9_shader.h"

// D3D9 opcodes (D3DSIO_*)
enum {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT, OP_SGE,
    OP_EXP, OP_LOG, OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2, OP_CALL, OP_CALLNZ,
    OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM, OP_SINCOS, OP_REP, OP_ENDREP,
    OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB, OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL, OP_TEX, OP_TEXBEM, OP_TEXBEML, OP_TEXREG2AR, OP_TEXREG2GB, OP_TEXM3x2PAD, OP_TEXM3x2TEX,
    OP_TEXM3x3PAD, OP_TEXM3x3TEX, OP_RESERVED0, OP_TEXM3x3SPEC, OP_TEXM3x3VSPEC, OP_EXPP, OP_LOGP, OP_CND, OP_DEF,
    OP_TEXREG2RGB, OP_TEXDP3TEX, OP_TEXM3x2DEPTH, OP_TEXDP3, OP_TEXM3x3, OP_TEXDEPTH, OP_CMP, OP_BEM, OP_DP2ADD,
    OP_DSX, OP_DSY, OP_TEXLDD, OP_SETP, OP_TEXLDL, OP_BREAKP,
    OP_MAX_CODE = 97,
    OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};

// decoded register files
enum {
    RF_TEMP, RF_INPUT, RF_VARY, RF_CONST, RF_DEFC, RF_TREG, RF_MISC, RF_ZERO,      // readable via rb[] tables
    RF_OUT_POS, RF_OUT_VARY, RF_OUT_PSIZE, RF_OUT_COLOR, RF_OUT_DEPTH, RF_SINK,    // write only
    RF_ADDR, RF_LOOP, RF_PRED, RF_CONSTI, RF_CONSTB, RF_SAMP, RF_LABEL,            // special
    RF_COUNT
};

enum { SM_NEG = 1, SM_BIAS, SM_BIASNEG, SM_SIGN, SM_SIGNNEG, SM_COMP, SM_X2, SM_X2NEG, SM_DZ, SM_DW, SM_ABS, SM_ABSNEG, SM_NOT };
enum { CMP_GT = 1, CMP_EQ, CMP_GE, CMP_LT, CMP_NE, CMP_LE };

typedef struct Operand {
    uint8_t file;        // RF_*
    uint8_t swz;         // packed swizzle, comp i = (swz >> 2i) & 3
    uint8_t mod;         // SM_*
    uint8_t rel;         // 0 none, 1 a0.<relc>, 2 aL
    uint8_t relc;
    uint8_t clampc;      // ps 1.x constant: clamp to [-1,1]
    uint16_t idx;
} Operand;

typedef struct DestOp {
    uint8_t file, mask, sat, shift;    // shift: signed 4 bit
    uint16_t idx;
} DestOp;

typedef struct Instr {
    uint16_t op;
    uint8_t ctl;         // comparison code / texld control
    uint8_t nsrc;
    uint8_t pred;        // 0 none, 1 p0, 2 !p0
    uint8_t predswz;
    uint8_t hasdst;
    uint8_t coissue;
    int32_t target;      // jump target (flow control) / label pc
    DestOp dst;
    Operand src[4];
} Instr;

#define MAX_LABELS 2048

struct D3dShader {
    int pixel, major, minor;
    uint32_t *tokens;
    size_t ntok;
    Instr *ins;
    int nins;
    float (*deff)[4];                    // defined float constants (256) or NULL
    uint32_t defmask[8];
    int defi[D3DSH_MAX_CONST_I][4];
    uint32_t defi_mask;
    int defb[D3DSH_MAX_CONST_B];
    uint32_t defb_mask;
    D3dShaderInput vsin[D3DSH_VS_INPUTS];
    int nvsin;
    int8_t in_slot[16];                  // ps_3_0 v# -> varying slot (-1 undeclared)
    uint32_t vs_out_mask, ps_in_mask, sampler_mask;
    int ps_nvary, writes_depth;
    int8_t sampler_type[16];
    int ntemps;                          // temp registers used (max index + 1)
    int deriv;                           // pixel shader needs derivative tracking
    int ps1x;                            // ps_1_x
    int *label_pc;                       // label number -> pc (or -1), MAX_LABELS entries, or NULL
};

