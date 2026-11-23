// SoulCore - Direct3D 9 shader interface (SM 1.1 - 3.0), the contract between
//   * the Direct3D 9 API front end ("G2": devices, state, vertex processing, draw calls), and
//   * the shader engine ("G3": bytecode parser + interpreter, implemented in d3d9_shader.c).
// Both sides can be written against this header alone.
//
// ---------------------------------------------------------------------------------------------
// Overview of a draw call with shaders (G2 side)
//   1. For each vertex: gather the vertex-declaration elements into in[16][4] (see "Vertex
//      inputs"), call d3dsh_run_vs_ex(), take out.pos as the clip-space position (D3D clip volume
//      0 <= z <= w: set SgState.clip_z_zero = 1) and out.v[] as SgVertex.v[]; out.psize is
//      SgVertex.psize.  Apply the D3D9 half-pixel offset in the viewport transform (G2 side).
//   2. Set SgState.pixel_fn = d3dsh_ps_pixel, pixel_ctx = &D3dPsCtx, nvary = d3dsh_ps_nvary(ps),
//      pixel_writes_depth = d3dsh_ps_writes_depth(ps), vp_top_left = 1, depth range from the
//      viewport; then sg_triangles/sg_lines/sg_points.  Fog is NOT applied by the pixel function
//      (the FF fog blend after a pixel shader is done by G2 by wrapping the function).
//   3. Shaders are immutable once created and may be shared by any number of devices/threads.
//      Constants and contexts are per draw and owned by the caller; no function in this header
//      keeps global state.
//
// Constant registers
//   The front end owns one D3dShaderConsts for the vertex stage and one for the pixel stage and
//   copies SetVertexShaderConstantF/I/B and SetPixelShader... into it. Unset entries are zero.
//   Constants declared inside the shader (def c#, defi i#, defb b#) are applied by the engine
//   and take precedence over the caller's values; the caller never needs to know about them.
//   Float constants: vs up to 256 (c0..c255), ps up to 224 (ps_3_0) / 32 (ps_2_0) / 8 (ps_1_x);
//   the array always has 256 rows. Integer constants i#: loop counter {count, start, step, _}.
//   Booleans are 0/non-zero.
//
// Linkage by semantic (vertex output -> pixel input), the "varying slot" table:
//     TEXCOORD0..7 -> 0..7    COLOR0 -> 8   COLOR1 -> 9   FOG -> 10
//     other SM3 semantics (NORMAL, TANGENT, BINORMAL, BLENDWEIGHT, BLENDINDICES, DEPTH, SAMPLE,
//     TESSFACTOR, PSIZE as a varying, TEXCOORD8+ / COLOR2+ ...) -> slot 11, shared; if more than
//     one distinct such semantic is used in one shader pair the extra ones return -1 (unsupported).
//     POSITION / POSITIONT / PSIZE as the vs output position/size are not varyings (-1).
//   ps_1_x/ps_2_x with fixed registers use the same table: v0/v1 = COLOR0/1 (slots 8/9), t#
//   = TEXCOORD# (slot #), vs oD0/oD1 -> 8/9, oT# -> #, oFog -> 10 (x component), oPts -> psize.
//   SM3 vs outputs declared with dcl_<usage><n> oN and ps inputs dcl_<usage><n> vN are linked by
//   that mapping, NOT by register number, so any vs/ps pair with matching semantics works.
//   A pixel input without a matching vs output reads zero (D3D leaves it undefined); unwritten vs
//   outputs are zero.
//   Every varying is a full vec4; the engine masks/swizzles as the instructions ask.
//
// Vertex inputs (vs): the engine reads in[reg] for reg 0..15. For vs_2_0+ the dcl_<usage> tokens
// bind register -> (usage, usage_index) and the front end maps vertex-declaration elements to
// registers with d3dsh_vs_inputs(). For vs_1_x there are no dcl tokens: the register meaning
// comes from the vertex declaration (FVF or D3DVERTEXELEMENT9 stream: the front end assigns
// registers itself, v0.. in the order of the DECLARATION USAGE as documented for SetFVF/vs_1_1
// stock mapping: POSITION=v0, BLENDWEIGHT=v1, BLENDINDICES=v2, NORMAL=v3, PSIZE=v4, COLOR0=v5,
// COLOR1=v6, TEXCOORD0..7=v7..v14, or the declaration's own register order when a declaration
// is used with explicit register binding) and just fills in[]. Missing components default to
// (0,0,0,1) as in D3D. Colours must already be unpacked to floats 0..1 (D3DCOLOR is BGRA ->
// converted to RGBA order x=R y=G z=B w=A) and UBYTE4/SHORT/etc. expanded by the front end.
//
// Pixel stage details
//   * ps_1_x: texture coordinate registers t# are taken from varying slot # (TEXCOORD#), colours
//     v0/v1 from slots 8/9, texture stage # is tex[#] / samp[#]; tex* instructions sample
//     implicitly (texld, texbem, texm3x3*, texkill, texreg2*, ...). Result is r0 -> color_out.
//   * ps_2_0+: s# registers are samplers tex[#]/samp[#]; texld/texldb/texldp/texldl; the engine
//     chooses 2D/cube/volume by the SgTexture.kind of the bound texture (dcl_2d/cube/volume gives
//     the expected type; d3dsh_ps_sampler_type() exposes it). oC0 -> color_out; oC1..3 (MRT) and
//     oDepth: oDepth -> *depth_out (flag pixel_writes_depth). texkill/discard -> return 0.
//   * ps_3_0: inputs from varying slots as above, vPos (VPOS) = floor(in->pos.xy) as integer
//     pixel coordinates in the render target, vFace (VFACE) = +1 / -1 from front_facing,
//     loops/rep/if/call per spec.
//   * Implicit derivatives (texld with computed coordinates, dsx/dsy): the engine may propagate
//     derivatives of the sgpu inputs (in->dx/in->dy) through arithmetic; a legal minimal
//     implementation uses the varying's derivative when the coordinate is a direct copy of a
//     pixel input, and LOD 0 (or the lod of the source varying) otherwise.
//   * Sampling uses sg_sample() with samp[s] (min/mag/mip filter, address modes, border, lod
//     bias, max mip, anisotropy, sRGB); an unbound texture (NULL) samples as (0,0,0,1).
//   * Vertex texture fetch (SM3 vs, texldl on s0..s3): D3dVsCtx.tex/samp, explicit LOD.
// ---------------------------------------------------------------------------------------------
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "sgpu.h"

typedef struct D3dShader D3dShader;   // opaque, immutable after d3dsh_create

// D3DDECLUSAGE values (also used for shader semantics)
enum {
    D3DSU_POSITION = 0, D3DSU_BLENDWEIGHT = 1, D3DSU_BLENDINDICES = 2, D3DSU_NORMAL = 3, D3DSU_PSIZE = 4,
    D3DSU_TEXCOORD = 5, D3DSU_TANGENT = 6, D3DSU_BINORMAL = 7, D3DSU_TESSFACTOR = 8, D3DSU_POSITIONT = 9,
    D3DSU_COLOR = 10, D3DSU_FOG = 11, D3DSU_DEPTH = 12, D3DSU_SAMPLE = 13,
};

#define D3DSH_MAX_CONST_F 256
#define D3DSH_MAX_CONST_I 16
#define D3DSH_MAX_CONST_B 16
#define D3DSH_MAX_SAMPLERS 16
#define D3DSH_VS_TEXTURES 4          // vertex texture samplers s0..s3 (D3DVERTEXTEXTURESAMPLER0..3)
#define D3DSH_VS_INPUTS 16

typedef struct D3dShaderConsts {
    float f[D3DSH_MAX_CONST_F][4];
    int i[D3DSH_MAX_CONST_I][4];
    int b[D3DSH_MAX_CONST_B];
} D3dShaderConsts;

// ---- creation / inspection ----------------------------------------------------------------

// Parses and pre-decodes shader bytecode (vs_1_1 .. vs_3_0: version token 0xFFFE0000 | major<<8 | minor,
// ps_1_0 .. ps_3_0: 0xFFFF....), ending with the end token 0x0000FFFF; comment tokens are skipped.
// max_tokens bounds the read (the caller usually passes the size it can see; a shader without an
// end token inside max_tokens is an error). The tokens are copied. Returns NULL on error with a
// message in err (CreateVertexShader/CreatePixelShader then fail with D3DERR_INVALIDCALL).
// The decoder should be tolerant: unknown/unsupported instructions are an error only if reached
// at creation validation, not silently ignored.
D3dShader *d3dsh_create(const uint32_t *tokens, size_t max_tokens, char *err, size_t errlen);
void d3dsh_free(D3dShader *sh);
int d3dsh_is_pixel(const D3dShader *sh);
int d3dsh_major(const D3dShader *sh);
int d3dsh_minor(const D3dShader *sh);
size_t d3dsh_token_count(const D3dShader *sh);        // tokens including version and end (for GetFunction)
const uint32_t *d3dsh_tokens(const D3dShader *sh);    // the copy made at creation

// ---- linkage --------------------------------------------------------------------------------

// Varying slot for a semantic (D3DDECLUSAGE + index), see the table above; -1 = not a varying.
// Pure function, implemented in d3d9_shader.c.
int d3dsh_varying_slot(int usage, int usage_index);

typedef struct D3dShaderInput { int reg; int usage; int usage_index; } D3dShaderInput;
// vs_2_0+: the dcl'd input registers with their semantics. Returns the count written (<= max)
// or 0 for vs_1_x (no dcl tokens) and for pixel shaders.
int d3dsh_vs_inputs(const D3dShader *vs, D3dShaderInput *out, int max);

uint32_t d3dsh_vs_output_mask(const D3dShader *vs);   // bit n set = varying slot n is written
uint32_t d3dsh_ps_input_mask(const D3dShader *ps);    // bit n set = varying slot n is read
int d3dsh_ps_nvary(const D3dShader *ps);              // highest read slot + 1 = SgState.nvary
int d3dsh_ps_writes_depth(const D3dShader *ps);       // oDepth written (SgState.pixel_writes_depth)
uint32_t d3dsh_ps_sampler_mask(const D3dShader *ps);  // bit s set = sampler s is used
int d3dsh_ps_sampler_type(const D3dShader *ps, int s);// SG_TEX_* expected by dcl_2d/cube/volume, -1 unknown (ps_1_x: from bound texture)
uint32_t d3dsh_ps_misc_mask(const D3dShader *ps);     // ps_3_0 misc inputs read: bit 0 = vPos (VPOS), bit 1 = vFace (VFACE)

// ---- vertex execution -----------------------------------------------------------------------

typedef struct D3dVsOut {
    float pos[4];                       // oPos, clip space (D3D: 0 <= z <= w)
    float v[SG_VARYINGS][4];            // varyings by slot (zero if not written)
    float psize;                        // oPts / PSIZE output (0 if not written: caller uses POINTSIZE state)
    uint32_t written_mask;              // varying slots actually written by this shader
} D3dVsOut;

typedef struct D3dVsCtx {
    const D3dShaderConsts *c;                    // required
    const SgTexture *tex[D3DSH_VS_TEXTURES];     // optional vertex textures (SM3 texldl), NULL = unbound
    SgSampler samp[D3DSH_VS_TEXTURES];
} D3dVsCtx;

// Runs the vertex shader for one vertex. in[r] = input register r (see "Vertex inputs").
void d3dsh_run_vs(const D3dShader *vs, const D3dShaderConsts *c, const float in[D3DSH_VS_INPUTS][4], D3dVsOut *out);
void d3dsh_run_vs_ex(const D3dShader *vs, const D3dVsCtx *ctx, const float in[D3DSH_VS_INPUTS][4], D3dVsOut *out);
// Same for n vertices (in[i], out[i]); allows the engine to amortise setup.
void d3dsh_run_vs_batch(const D3dShader *vs, const D3dVsCtx *ctx, const float (*in)[D3DSH_VS_INPUTS][4], D3dVsOut *out, int n);

// ---- pixel execution --------------------------------------------------------------------------

typedef struct D3dPsCtx {
    const D3dShader *ps;                         // required
    const D3dShaderConsts *c;                    // required
    const SgTexture *tex[D3DSH_MAX_SAMPLERS];    // bound textures per stage/sampler (NULL = unbound)
    SgSampler samp[D3DSH_MAX_SAMPLERS];
    // Fixed-function side inputs for ps_1_x (texture transform flags, bump-env matrices) are the
    // front end's business: it folds them into the texture coordinates before the pixel stage and
    // supplies bem constants through these fields.
    float bump_mat[D3DSH_MAX_SAMPLERS][4];       // D3DTSS_BUMPENVMAT00,01,10,11 per stage (texbem/texbeml)
    float bump_lscale[D3DSH_MAX_SAMPLERS], bump_loffset[D3DSH_MAX_SAMPLERS];
    void *reserved[4];
} D3dPsCtx;

// Usable directly as SgPixelFn with ctx = (D3dPsCtx *). Returns 0 on discard (texkill/discard).
// Writes oC0 to color_out and oDepth (if the shader writes it) to *depth_out. Allocation-free and
// reentrant (state lives on the stack), so it may be called concurrently with different contexts.
int d3dsh_ps_pixel(void *ctx, const SgPixelIn *in, float color_out[4], float *depth_out);
