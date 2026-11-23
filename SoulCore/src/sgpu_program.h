// SoulCore - sgpu "program": a GPU-translatable description of a draw's pixel stage.
//
// Why: the CPU rasteriser runs the pixel stage as a C callback (SgState.pixel_fn). A GPU backend cannot
// call it, so the front ends ALSO describe the same pixel stage declaratively. A shader translator
// (src/sgsl.c) turns the description into Metal Shading Language; the description is complete enough
// to reproduce the pixel_fn result.
//
// Three kinds (SgProgram.kind):
//
//  SGP_NONE      SgState.program == NULL and SgState.pixel_fn == NULL: the OpenGL fixed-function pixel
//                stage. SgState IS the description:
//                   per unit u < SG_UNITS: tex_on[u], tex[u] (SgTexture: format/min_linear/mag_linear/
//                   clamp_s/clamp_t/use_mips/levels), tex_env[u] (SG_ENV_*), env_color[u];
//                   colour = interpolated SgVertex.c (flat -> provoking vertex), texture coordinates
//                   SgVertex.t[u]; fog (SgState.fog, fog_color) with SgVertex.fog as the factor
//                   (1 = no fog); alpha test; blending; masks. nvary may be 0.
//                The key for it is sg_program_key(s).
//
//  SGP_D3D9_TSS  Direct3D 9 fixed-function texture stage cascade (no pixel shader). The varyings are
//                produced by the CPU vertex stage:  v[8] = diffuse, v[9] = specular, v[10].x = vertex fog
//                factor (1 = no fog), v[s] = texture coordinate set of STAGE s (already generated /
//                transformed per TEXCOORDINDEX, TCI flags and TEXTURETRANSFORMFLAGS; per pixel only the
//                projective divide remains).
//  SGP_D3D9_PS   Direct3D 9 pixel shader 1.x .. 3.0.
//
// In both D3D kinds SgState.pixel_fn / pixel_ctx are set as well (CPU reference implementation) and
// SgState.nvary covers the slots read. Colour written by the pixel stage is clamped to 0..1 by the
// rasteriser; alpha test, depth/stencil, blending, colour mask are the fixed back end described by
// SgState (identical for all kinds). Pixel shaders do not write MRT 1..3 (ignored).
//
// ---- TSS semantics (reference: ff_pixel in d3d9_draw.c) ---------------------------------------------------
//   cur = diffuse (v[8]), tmp = (0,0,0,0).
//   for stage s in 0 .. ntss-1 (the list ends before the first stage whose COLOROP is DISABLE):
//     texture T = (0,0,0,1);  the stage needs its texture when any ARG0/1/2 (colour or alpha) selects
//       TEXTURE, or colorop/alphaop is BLENDTEXTUREALPHA(13) / BLENDTEXTUREALPHAPM(15), or colorop is
//       BUMPENVMAP(22) / BUMPENVMAPLUMINANCE(23).
//     if it needs the texture:
//        no texture bound (samplers[s].tex == NULL)  -> the cascade STOPS here (result = cur so far);
//        else coordinate c = v[slot] (4 floats; dx/dy are the screen derivatives of the varying);
//             if projected && ncoord >= 2 && texture is not a cube map:  c = c / c[ncoord-1] (divide by 0 -> 1/1),
//             if the previous stage was a bump stage: c.xy += perturbation of that stage,
//             T = sg_sample(tex, samp, c, dx, dy, NaN)   (derivatives follow the projective divide).
//     if the previous stage was BUMPENVMAPLUMINANCE: T.rgb *= its luminance factor. (flags reset afterwards)
//     BUMPENVMAP / BUMPENVMAPLUMINANCE: du,dv = signed 8-bit decode of T.rg (sraw: v=round(255*b); v>127 -> v-256;
//        v/127 clamped to -1), pert = (m00*du + m10*dv, m01*du + m11*dv) with bump_mat = (m00 m01 m10 m11) as
//        stored in the TSS slots 7..10; luminance = sat(T.b * lscale + loffset); the stage produces no colour,
//        `continue` with the next stage.
//     otherwise:  A_i = arg(color_arg[i]), B_i = arg(alpha_arg[i]) with D3DTA selector in the low 4 bits
//        (0 DIFFUSE, 1 CURRENT, 2 TEXTURE, 3 TFACTOR, 4 SPECULAR, 5 TEMP, 6 CONSTANT, others = CURRENT),
//        0x10 = COMPLEMENT (1 - x), 0x20 = ALPHAREPLICATE (colour args only: rgba = alpha).
//        colorop DOTPRODUCT3(24): d = sat(dot(2*A1.rgb-1, 2*A2.rgb-1)) into all four channels (alpha op ignored);
//        else rgb = op(color ops, A0, A1, A2), alpha = (alphaop == SELECTARG-CURRENT(1)? "disable" : op(B...)).
//        Ops (D3DTOP): 2 SELECTARG1, 3 SELECTARG2, 4 MODULATE, 5 MODULATE2X, 6 MODULATE4X, 7 ADD, 8 ADDSIGNED,
//        9 ADDSIGNED2X, 10 SUBTRACT, 11 ADDSMOOTH, 12 BLENDDIFFUSEALPHA, 13 BLENDTEXTUREALPHA, 14 BLENDFACTORALPHA,
//        15 BLENDTEXTUREALPHAPM, 16 BLENDCURRENTALPHA, 17 PREMODULATE(= SELECTARG1 here), 18 MODULATEALPHA_ADDCOLOR,
//        19 MODULATECOLOR_ADDALPHA, 20 MODULATEINVALPHA_ADDCOLOR, 21 MODULATEINVCOLOR_ADDALPHA (the four
//        MODULATE*_ADD* are colour-only, alpha ops behave like SELECTARG1), 25 MULTIPLYADD, 26 LERP; any
//        other value = SELECTARG1. Results are saturated. (See ff_op for the exact blend factors; the blend
//        factor of BLENDTEXTUREALPHA is T.a, of BLENDDIFFUSEALPHA diffuse.a, of BLENDFACTORALPHA tfactor.a,
//        of BLENDCURRENTALPHA cur.a.) alphaop 1 = DISABLE: alpha = cur.a (colour computed normally).
//        The result goes to tmp when result_arg == 5 (TEMP), else to cur.
//   out = cur; if specular: out.rgb = sat(out.rgb + v[9].rgb); if fog.enable: f = (fog.table ? fog_factor(table,
//   depth = pixel z in 0..1, start, end, density) : sat(v[10].x)); out.rgb = out.rgb * f + fog.color * (1 - f).
//   fog_factor: 1 EXP: e^(-d*density); 2 EXP2: e^(-(d*density)^2); 3 LINEAR: (end-d)/(end-start) (start==end:
//   d<start ? 1 : 0); result saturated.
//
// ---- PS semantics -----------------------------------------------------------------------------------------
//   The bytecode (tokens/ntokens) with the semantics of d3d9_shader.h: constants float/int/bool from `consts`
//   (shader-declared def/defi/defb override them), sampler s = samplers[s] (ps_1_x: stage s; texture type is
//   dcl'd by the shader for 2.0+, or taken from the bound texture's kind), bump_mat/lscale/loffset for
//   texbem/texbeml/bem, input linkage by semantic: TEXCOORD0..7 -> slots 0..7, COLOR0 -> 8, COLOR1 -> 9, FOG ->
//   10 (x), other SM3 semantics -> slot 11 (d3dsh_varying_slot(usage, index); for ps_3_0 the dcl tokens map
//   v# to a semantic; ps_1_x/2_x: t# = slot #, v0/v1 = slots 8/9); vPos = floor(window x, y), vFace = +1 front / -1.
//   oC0 -> colour, oDepth -> depth (writes_depth). Unwritten inputs read zero. texkill/discard drop the pixel.
//   Fog (ps < 3.0 only): when fog.enable is set the front end wraps the shader: after oC0,
//   f = (fog.table ? fog_factor(...) : sat(v[10].x)); rgb = rgb * f + fog.color * (1 - f)   (nvary >= 11).
//   For ps_3_0 fog.enable is 0 (the shader does its own fog).
//
// ---- Cached pipelines: the key -----------------------------------------------------------------------------
//   sg_program_key() (sgpu.h) hashes exactly what changes generated code: kind, nvary, per-stage ops / args /
//   result / slot / ncoord / projected / texture kind / bump_signed, specular, fog.enable + fog.table, shader
//   tokens, writes_depth, vpos/vface use, the texture KIND (2D/cube/volume, or unbound) per sampler, and for
//   SGP_NONE the fixed-function pixel parameters of SgState. It does NOT hash values that can be uniforms:
//   constants, tfactor, bump matrices, lscale/loffset, constant colour, fog start/end/density/colour, texture
//   identity, sampler state (filters, address modes, border, lod bias, anisotropy, sRGB - use sampler objects;
//   sRGB decoding must be a runtime flag or part of the sampler binding).
#pragma once
#include "sgpu.h"
#include "d3d9_shader.h"

enum { SGP_NONE = 0, SGP_D3D9_TSS = 1, SGP_D3D9_PS = 2 };

#define SGP_MAX_STAGES 8
#define SGP_MAX_SAMPLERS 16        // == D3DSH_MAX_SAMPLERS

// varying slots (same table as d3d9_shader.h)
#define SGP_VARY_DIFFUSE 8
#define SGP_VARY_SPECULAR 9
#define SGP_VARY_FOG 10

// One sampler slot: the bound texture (NULL = unbound: samples (0,0,0,1)) and its sampler state.
// The SgTexture pointer is valid for the draw call; identify the texture by tex->id / tex->version.
typedef struct SgpSampler {
    const SgTexture *tex;
    SgSampler samp;
} SgpSampler;

// Pixel fog (D3D9). Applied to the pixel stage output as described above.
typedef struct SgpFog {
    int enable;                    // 0: no pixel fog in the program
    int table;                     // 0: factor = sat(v[10].x) (vertex fog); 1 EXP, 2 EXP2, 3 LINEAR evaluated per pixel from depth
    float start, end, density;
    float color[3];                // rgb 0..1
} SgpFog;

typedef struct SgpTssStage {
    int colorop, alphaop;          // D3DTOP_*
    int color_arg[3];              // ARG0, ARG1, ARG2 (D3DTA_* with 0x10 complement / 0x20 alpha replicate)
    int alpha_arg[3];
    int result_arg;                // D3DTA_CURRENT (1) or D3DTA_TEMP (5)
    int slot;                      // varying slot holding this stage's texture coordinates (== stage index)
    int ncoord;                    // TEXTURETRANSFORMFLAGS & 0xFF: coordinate count 0..4 (0 = disabled)
    int projected;                 // TEXTURETRANSFORMFLAGS PROJECTED (already 0 when ncoord == 0)
    int tex_kind;                  // SG_TEX_* of the bound texture, -1 = none bound
    int bump_signed;               // texture format is a signed bump format (informational, decoded by the 8-bit rule)
    uint32_t tci;                  // raw TEXCOORDINDEX (informational: applied by the vertex stage)
    uint32_t ttf;                  // raw TEXTURETRANSFORMFLAGS (informational)
    float bump_mat[4];             // BUMPENVMAT00, 01, 10, 11
    float bump_lscale, bump_loffset;
    float constant[4];             // TEXTURESTAGE CONSTANT as rgba 0..1
} SgpTssStage;

typedef struct SgpTss {
    int nstages;                   // active stages (cascade length)
    SgpTssStage stage[SGP_MAX_STAGES];
    float tfactor[4];              // D3DRS_TEXTUREFACTOR as rgba 0..1
    int specular;                  // D3DRS_SPECULARENABLE: add v[9].rgb after the cascade
} SgpTss;

typedef struct SgpPs {
    const D3dShader *shader;       // opaque handle (CPU interpreter)
    const uint32_t *tokens;        // bytecode: version token .. end token (aliases the shader's copy)
    int ntokens;
    int major, minor;              // shader model
    const D3dShaderConsts *consts; // float / int / bool constants; valid during the draw call only - copy
    uint32_t input_mask;           // varying slots read (bit n = slot n)
    uint32_t sampler_mask;         // samplers used
    int sampler_type[SGP_MAX_SAMPLERS]; // SG_TEX_* declared by the shader, -1 = take it from the bound texture
    int writes_depth;              // oDepth written
    int uses_vpos, uses_vface;     // ps_3_0 VPOS / VFACE inputs are read
    float bump_mat[SGP_MAX_SAMPLERS][4];   // TSS BUMPENVMAT00..11 of the stage with that number (stages 0..7; slots >= 8 use stage 0's)
    float bump_lscale[SGP_MAX_SAMPLERS], bump_loffset[SGP_MAX_SAMPLERS];
} SgpPs;

typedef struct SgProgram {
    int kind;                      // SGP_D3D9_TSS or SGP_D3D9_PS (a NULL SgState.program is SGP_NONE)
    int nvary;                     // = SgState.nvary
    int nsamplers;                 // valid entries of samplers[] (highest bound slot + 1)
    SgpSampler samplers[SGP_MAX_SAMPLERS];   // TSS: stage s uses samplers[s]; PS: sampler register s
    SgpFog fog;
    SgpTss tss;                    // kind == SGP_D3D9_TSS (zero otherwise)
    SgpPs ps;                      // kind == SGP_D3D9_PS (zero otherwise)
} SgProgram;
