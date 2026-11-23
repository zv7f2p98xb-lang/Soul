// SoulCore - "sgsl": the sgpu shader translator.
//
// Turns the pixel stage of one sgpu draw (SgState + SgProgram, see sgpu.h / sgpu_program.h) into complete
// Metal Shading Language source (`vertex sg_vs`, `fragment sg_fs`), or into an equivalent piece of C that the unit
// tests compile and compare against the CPU rasteriser (tests/unit/sgsl_test.c). Both languages are printed from the
// same small typed IR (sgsl_priv.h), so what the tests prove about the C rendering holds for the MSL text.
//
// Three front ends feed the IR:
//   * SgState.program == NULL : the OpenGL fixed-function pixel stage (texture environments, fog, alpha test),
//   * SGP_D3D9_TSS            : the Direct3D 9 texture-stage cascade,
//   * SGP_D3D9_PS             : Direct3D 9 pixel shader bytecode, ps_1_1 .. ps_3_0.
// Alpha test and the clamp of the colour to what an 8-bit target stores are part of every program. Depth/stencil
// tests, blending, colour masks, cull and the fill rule belong to the backend's fixed-function pipeline state.
//
// ===================================================================================================================
// Usage (backend)
// ===================================================================================================================
//   SgslInfo info; char err[256];
//   char *src = sgsl_generate_prim(state, SG_PRIM_TRIANGLES, SGSL_MSL, &info, err, sizeof err);
//   if (!src) -> draw this on the CPU (return -1 from SgBackend.draw)
//   cache the compiled MTLLibrary / pipeline by info.key (+ the render target formats and blend state you use);
//   per draw: SgslUniforms u; sgsl_fill_uniforms(state, &u); upload the first info.uniform_bytes bytes of u.
// sgsl_generate(s, ...) is sgsl_generate_prim(s, SG_PRIM_TRIANGLES, ...) (lines use the same code as triangles; only
// points differ, see "Primitive kinds").  Equal info.key => identical source, so the source need not be regenerated
// or recompiled; the key covers every static input (everything that is not a uniform or a texture/sampler object).
//
// ===================================================================================================================
// Binding conventions (MSL)
// ===================================================================================================================
//   vertex function   sg_vs(uint vid [[vertex_id]], const device SgVertex *vb [[buffer(0)]],
//                           constant SgslUniforms &u [[buffer(1)]])
//   fragment function sg_fs(SgFSIn in [[stage_in]], constant SgslUniforms &u [[buffer(1)]], [bool [[front_facing]]],
//                           [float2 [[point_coord]]], texture<i> [[texture(i)]] + sampler [[sampler(i)]] per slot)
//   * buffer(0): the draw's SgVertex array exactly as given to SgBackend.draw (struct SgVertex, 248 bytes per vertex,
//     tightly packed floats, same layout as the C struct; no index buffer: draw non-indexed with vertex_id = array
//     index, vertex count n; triangles = MTLPrimitiveTypeTriangle, lines = MTLPrimitiveTypeLine, points = ...Point).
//   * buffer(1): SgslUniforms (declared below), for BOTH stages. The MSL declaration of the struct is truncated after
//     the constant registers actually used, so bind a buffer (or setXBytes data) of at least info.uniform_bytes bytes;
//     the first info.uniform_bytes bytes of a filled C struct are the right content. (GL and texture-stage programs need 784 bytes;
//     pixel shader programs 1104 + 16 * (highest constant register read + 1), at most 5200 bytes: setBytes is limited to
//     4096 bytes, so use an MTLBuffer / a ring of them for those.)
//   * texture(i)/sampler(i): D3D sampler / stage slot i, or GL texture unit i. The MSL texture type follows
//     info.texture_kind[i]: SG_TEX_2D -> texture2d<float>, SG_TEX_CUBE -> texturecube<float>, SG_TEX_VOLUME ->
//     texture3d<float>. Only slots in info.texture_mask are declared. The texture is the SgTexture of slot i
//     (SgState.tex[i] for GL, SgState.program->samplers[i].tex otherwise) uploaded as RGBA8 (SgTexture rules); a slot
//     that is unbound (or has no storage) is not declared and the shader uses the constant (0,0,0,1).
//   * The sampler object for slot i is built from the SgSampler of the slot (program->samplers[i].samp), or for GL
//     from sgsl_gl_sampler(): filters (min/mag/mip), address modes, max_mip_level (lodMinClamp), max_anisotropy.
//     NOT in the sampler object and handled by the shader: lod_bias (uniform lod_bias[i]; the shader scales gradients by
//     2^bias or adds the bias to explicit levels). NOT expressible in Metal samplers: SG_ADDR_BORDER with a border colour
//     other than transparent/opaque black/white, SG_ADDR_MIRROR_ONCE (use mirror_clamp_to_edge), and sRGB (bind
//     the texture as MTLPixelFormatRGBA8Unorm_sRGB: decoding then happens before filtering like on the CPU). A
//     backend that cannot honour a sampler setting should decline the draw (return -1).
//   * Fragment stage_in is SgFSIn: [[position]] plus user varyings; the vertex function's SgVSOut carries the same
//     user varyings (matched by [[user(name)]]).  [[position]].xy are window coordinates, pixel centres at +0.5,
//     origin top-left; .z is the depth in 0..1 of the render pass viewport; .w is 1/w: exactly SgPixelIn.pos.
//     (Depth bias is applied by the rasteriser after the fragment function, so pos.z used for table fog excludes
//     SgState.depth_bias / slope bias / polygon_offset; the CPU includes them. Negligible, documented.)
//
// ===================================================================================================================
// Coordinate conventions: what the backend must do (MSL)
// ===================================================================================================================
//   * Clip space in: the SgVertex x,y,z,w are the clip coordinates used by the CPU rasteriser. sg_vs writes
//     position = (x, y, z', w) with z' = z when state.clip_z_zero (D3D: 0 <= z <= w), else z' = (z + w) / 2 (GL: -w <= z
//     <= w); Metal's clip volume is 0 <= z <= w, so this reproduces the CPU clipping exactly.
//   * Viewport. The CPU maps NDC to window coordinates as
//       x_win = vp_x + (nx + 1) / 2 * vp_w
//       y_win (top-down row) = vp_top_left ? vp_y + (1 - ny) / 2 * vp_h
//                                          : target_h - (vp_y + (ny + 1) / 2 * vp_h)
//       depth = depth_near + zn * (depth_far - depth_near)         zn = z' / w
//     Metal's viewport maps NDC +y to the TOP of the viewport rectangle (origin top-left, y down), so use
//       MTLViewport { originX = vp_x, originY = vp_top_left ? vp_y : target_h - vp_y - vp_h, width = vp_w, height = vp_h,
//                     znear = depth_near, zfar = depth_far }.
//     The scissor rectangle converts the same way (scissor origin = same origin as the viewport). depth_clamp =
//     MTLDepthClipModeClamp. Direct3D's half-pixel offset is already folded into the vertex positions by the front end.
//   * Front faces: Metal's [[front_facing]] and culling follow MTLWinding evaluated on the NDC (y up) triangle, like the
//     CPU's "front_ccw/cull_*" fields (see sgpu.h): front when counter-clockwise == (front_ccw != 0).
//   * Pixel centres/fill rule: the CPU uses the top-left rule for D3D (vp_top_left) and a bottom-left tie rule for GL;
//     Metal's rule is the top-left rule. Bit-exact edge coverage is not promised.
//   * Colour output: the fragment writes the clamped (0..1) colour of the CPU path. The CPU's GL fixed-function path
//     blends the unclamped colour; the shader clamps first (indistinguishable for 8-bit targets except for extreme blend
//     factors). Fast-math must be OFF when compiling (MTLCompileOptions.fastMathEnabled = NO; MSL 'preserve' semantics):
//     the D3D arithmetic relies on 0 * inf = 0 and on exact NaN propagation in a few places.
//
// ===================================================================================================================
// Primitive kinds, flat shading, points
// ===================================================================================================================
//   * Flat shading never relies on Metal's provoking vertex. The vertex function reads every flat attribute from the
//     provoking vertex of the vertex's own primitive (index from vertex_id: first vertex when state.flat_first, else
//     the last one) and the varying is declared [[flat]], so every vertex of the primitive carries the same value.
//     This requires the vertex buffer to be a plain list (n = 3 * triangles, 2 * lines, 1 * points) exactly as passed to
//     the backend: do not reorder or index it. Flat attributes: the GL colour when state.flat, and varyings in
//     state.flat_mask (below nvary) for programs.
//   * Points (SG_PRIM_POINTS): sg_vs writes [[point_size]] with the CPU rules (psize/point_size, min/max clamp, sprite
//     clamp 1..4096, non-sprite rounding). With state.point_sprite the fragment function replaces varying sprite_slot
//     by (point_coord.x, point_coord.y, 0, 1) (GL fixed function: texture coordinate 0), exactly the CPU's value;
//     info.uses_point_coord is set. For triangles/lines point sprites are inactive (the CPU ignores them too): generate
//     with the draw's real primitive kind.
//   * User clip planes (state.clip_mask): sg_vs writes [[clip_distance]] = dot(plane, (x, y, z, w)) (clip space, before
//     the z conversion) for planes 0..info.clip_distances-1 (disabled ones inside that range write 1).
//   * Line width / line smoothing / stipple: not part of the shaders (a backend declines wide lines).
//
// ===================================================================================================================
// What is bit-for-bit / what is approximate
// ===================================================================================================================
//   Colour arithmetic follows the CPU implementations operation by operation (the C rendering is tested to ~1e-4 against
//   the CPU functions). Differences by design: GPU texture filtering vs sg_sample (hardware filtering precision), the
//   GL fixed-function path uses per-pixel mip selection instead of one level per triangle, implicit derivatives of
//   varyings are computed by the hardware (the CPU uses analytic plane gradients), the fragment function runs for
//   helper lanes after discard_fragment().
//   Not translated (sgsl_generate returns NULL with a message, the backend draws on the CPU): pixel shader
//   subroutines that `ret` from inside nested control flow or with a predicate, recursive subroutine calls,
//   SgState.pixel_fn without a program (opaque CPU callback), a program whose kind/shader is malformed. Every
//   shader of the repository's test suites translates.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "sgpu.h"
#include "sgpu_program.h"

enum { SGSL_MSL = 1, SGSL_C = 2 };

typedef struct SgslInfo {
    uint64_t key;              // sg_program_key(state) combined with every other static input (primitive kind, flat mask, point sprite, clip mask, alpha func, fog mode, ...): equal keys => identical source
    uint32_t texture_mask;     // texture/sampler slots the fragment function samples (declared as arguments)
    int texture_kind[16];      // SG_TEX_2D / SG_TEX_CUBE / SG_TEX_VOLUME per used slot (others -1)
    int writes_depth, uses_point_coord, uses_front_facing, uses_position;
    int clip_distances;        // number of [[clip_distance]] outputs the vertex function writes (0 = none)
    // ---- additions ----
    uint32_t uniform_bytes;    // size of the SgslUniforms prefix the generated code declares/reads (multiple of 16)
    int nvary;                 // varyings passed from the vertex to the fragment function (v0 .. v(nvary-1), SgState.nvary)
    int prim;                  // SG_PRIM_* the source was generated for
    int uses_derivatives;      // fragment function calls dfdx/dfdy
    int uses_discard;          // fragment function may discard (alpha test, texkill/discard)
} SgslInfo;

// Complete source for one draw configuration: shared structs, `vertex sg_vs(...)` and `fragment sg_fs(...)` (MSL),
// or the equivalent C (see "C rendering" below). Returns malloc'ed text (free()), or NULL with err set when the
// configuration cannot be translated. info may be NULL.
char *sgsl_generate(const SgState *s, int lang, SgslInfo *info, char *err, size_t errlen);
char *sgsl_generate_prim(const SgState *s, int prim, int lang, SgslInfo *info, char *err, size_t errlen);

// 16-byte aligned vector types used by the uniform block (MSL: float4 / int4).
typedef struct __attribute__((aligned(16))) SgslF4 { float x, y, z, w; } SgslF4;
typedef struct __attribute__((aligned(16))) SgslI4 { int x, y, z, w; } SgslI4;

// Per-draw uniform values. The field order is the memory order; the generated MSL struct has the same fields in the same
// order (the constant register array `c` is last and truncated to the registers the shader can read).
typedef struct SgslUniforms {
    SgslF4 fog_color;          // rgb: GL fog colour / D3D pixel fog colour
    SgslF4 tfactor;            // D3D TEXTUREFACTOR
    SgslF4 env_color[SG_UNITS];            // GL texture environment colours
    SgslF4 clip_plane[SG_MAX_CLIP_PLANES]; // user clip planes (clip space), enabled ones per state.clip_mask
    SgslF4 tss_constant[SGP_MAX_STAGES];   // D3D TEXTURESTAGE CONSTANT
    SgslF4 bump_mat[SGP_MAX_SAMPLERS];     // TSS stage / PS sampler: BUMPENVMAT00, 01, 10, 11
    float bump_lscale[SGP_MAX_SAMPLERS];
    float bump_loffset[SGP_MAX_SAMPLERS];
    float lod_bias[SGP_MAX_SAMPLERS];      // SgSampler.lod_bias of slot i (GL: 0)
    float alpha_ref;
    float fog_start, fog_end, fog_density; // D3D table fog
    float depth_near, depth_far;           // viewport depth range (depth clamp of shader depth)
    float point_size, point_size_min, point_size_max;
    float pad0;
    SgslI4 i[D3DSH_MAX_CONST_I];           // PS integer constants {count, start, step, _}
    int b[D3DSH_MAX_CONST_B];              // PS boolean constants (0 / non-zero)
    SgslF4 c[D3DSH_MAX_CONST_F];           // PS float constants (shader-declared `def` constants are compiled in)
} SgslUniforms;

void sgsl_fill_uniforms(const SgState *s, SgslUniforms *u);

// The SgSampler a GL fixed-function texture is sampled with (GL unit i uses it for slot i): linear/point from
// min_linear/mag_linear/use_mips, clamp or wrap from clamp_s/clamp_t. Backends use it to build sampler objects.
void sgsl_gl_sampler(const SgTexture *t, SgSampler *out);

// ===================================================================================================================
// C rendering (tests)
// ===================================================================================================================
// With lang == SGSL_C the text is one self-contained translation unit (it needs only <math.h>, <stdint.h> and this
// header) defining
//     int  sg_fs(const SgslCEnv *env, const SgslFragIn *in, const SgslUniforms *u, float out_color[4], float *out_depth);
//     void sg_vs(unsigned vid, const SgVertex *vb, const SgslUniforms *u, SgslVSOut *out);
// sg_fs returns 0 when the fragment is discarded (alpha test, texkill/discard), else writes the colour (0..1) and, for
// programs that write depth, *out_depth. Texture reads go through env->sample (sg_sample) with env->tex[i]/env->samp[i].
typedef struct SgslCEnv {
    const SgTexture *tex[16];
    const SgSampler *samp[16];
    void (*sample)(const SgTexture *t, const SgSampler *s, const float coord[4], const float ddx[4], const float ddy[4], float lod_override, float out[4]);
} SgslCEnv;
typedef struct SgslFragIn {
    SgPixelIn px;              // position, varyings, derivatives, front facing (as given to a SgPixelFn)
    float point_coord[2];      // sprite coordinate (points with point_sprite)
} SgslFragIn;
typedef struct SgslVSOut {
    SgslF4 position;          // after the clip z conversion
    float point_size;
    float clip[SG_MAX_CLIP_PLANES];
    SgslF4 v[SG_VARYINGS];    // varyings v0 .. v(nvary-1) (GL fixed function: v0 colour, v1 t0, v2 t1, v3.x fog)
} SgslVSOut;
