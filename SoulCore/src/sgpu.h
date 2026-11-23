// SoulCore - "sgpu": the 3D pipeline behind Soul's graphics APIs (OpenGL, Direct3D 9).
//
// Front-ends transform vertices to clip space and hand batches of primitives plus a
// render state to this backend. The current backend rasterises on the CPU
// (perspective-correct, depth/stencil-buffered, textured, blended, optionally running a
// programmable pixel stage); the interface is deliberately GPU-shaped so that a Metal
// backend can replace it without touching the front-ends.
//
// Conventions
//  * All SgState/SgVertex/SgTarget fields added after the original fixed-function ones are
//    designed so that an all-zero value means "behave like the OpenGL fixed-function pipe".
//  * Colour target rows are top-down. Window coordinates in SgPixelIn are top-left origin,
//    pixel centres at integer + 0.5.
//  * Clip space: GL style -w <= z <= w by default, D3D style 0 <= z <= w with clip_z_zero.
//  * Triangle orientation: front_ccw/cull_* are expressed for a y-up NDC (as GL). A D3D
//    front end maps D3DCULL_CW (cull clockwise = cull back when clockwise is front) with
//    front_ccw = 0 (clockwise triangles are front faces) and cull_back/cull_front as needed;
//    this holds for top-left origin viewports too (NDC y up always maps to screen y down).
#pragma once
#include <stdint.h>

#define SG_UNITS 2
#define SG_VARYINGS 12          // generic vec4 attributes per vertex
#define SG_MAX_CLIP_PLANES 6
#define SG_MAX_MIPS 14

enum { SG_NEVER, SG_LESS, SG_EQUAL, SG_LEQUAL, SG_GREATER, SG_NOTEQUAL, SG_GEQUAL, SG_ALWAYS };
enum {
    SG_ZERO, SG_ONE, SG_SRC_COLOR, SG_ONE_MINUS_SRC_COLOR, SG_SRC_ALPHA, SG_ONE_MINUS_SRC_ALPHA,
    SG_DST_ALPHA, SG_ONE_MINUS_DST_ALPHA, SG_DST_COLOR, SG_ONE_MINUS_DST_COLOR, SG_SRC_ALPHA_SATURATE,
    SG_CONSTANT_COLOR, SG_ONE_MINUS_CONSTANT_COLOR, SG_CONSTANT_ALPHA, SG_ONE_MINUS_CONSTANT_ALPHA,
};
enum { SG_BLEND_ADD, SG_BLEND_SUBTRACT, SG_BLEND_REV_SUBTRACT, SG_BLEND_MIN, SG_BLEND_MAX };
enum { SG_SOP_KEEP, SG_SOP_ZERO, SG_SOP_REPLACE, SG_SOP_INCR_SAT, SG_SOP_DECR_SAT, SG_SOP_INVERT, SG_SOP_INCR, SG_SOP_DECR };
enum { SG_ENV_MODULATE, SG_ENV_REPLACE, SG_ENV_DECAL, SG_ENV_ADD, SG_ENV_BLEND };
enum { SG_FMT_RGBA, SG_FMT_RGB, SG_FMT_ALPHA, SG_FMT_LUMINANCE, SG_FMT_LUMINANCE_ALPHA, SG_FMT_INTENSITY };
enum { SG_TEX_2D, SG_TEX_CUBE, SG_TEX_VOLUME };
enum { SG_FILTER_NONE, SG_FILTER_POINT, SG_FILTER_LINEAR };
enum { SG_ADDR_WRAP, SG_ADDR_MIRROR, SG_ADDR_CLAMP, SG_ADDR_BORDER, SG_ADDR_MIRROR_ONCE }; // D3DTADDRESS_x minus 1
// cube faces: +X, -X, +Y, -Y, +Z, -Z (GL / D3D order)

// RGBA8 texture. Storage rules (all kinds):
//  * level 0 is `rgba`; levels 1.. (if any) are in mip[i] with sizes mip_w/mip_h/mip_d.
//    `levels` = number of levels (0 or 1 = no mip chain). sg_texture_build_mips() fills them,
//    or a front end may fill mip[]/levels itself (then mip[0] must equal rgba).
//  * 2D: w*h*4 bytes per level.
//  * CUBE (w == h): all 6 faces of a level contiguous, face f at offset f*w*h*4.
//  * VOLUME: `depth` slices of w*h*4 per level 0 (mip_d[i] slices for level i).
// The OpenGL path only uses the 2D fields up to use_mips; zero `kind` = 2D.
typedef struct SgTexture {
    int w, h;
    int format;          // SG_FMT_* (how the texel is combined, GL fixed-function only)
    uint8_t *rgba;       // R G B A
    int min_linear, mag_linear;
    int clamp_s, clamp_t;
    int levels;          // mipmaps: rgba of level i at mip[i] (level 0 = rgba)
    uint8_t *mip[SG_MAX_MIPS];
    int mip_w[SG_MAX_MIPS], mip_h[SG_MAX_MIPS];
    int use_mips;
    int kind;            // SG_TEX_*
    int depth;           // volume slices at level 0
    int mip_d[SG_MAX_MIPS];
} SgTexture;

// Sampler state for sg_sample (D3D9 sampler states map 1:1; values are D3DTEXF_x / D3DTADDRESS_x
// converted as noted in the enums above).
typedef struct SgSampler {
    int min_filter, mag_filter, mip_filter;      // SG_FILTER_*
    int address_u, address_v, address_w;         // SG_ADDR_*
    uint32_t border_argb;
    float lod_bias;                              // added to the (derived or overridden) LOD
    int max_mip_level;                           // finest level allowed (D3D MAXMIPLEVEL)
    int max_anisotropy;                          // >1 with linear min filter = anisotropic (2D only)
    int srgb;                                    // texels are sRGB encoded (RGB decoded before filtering)
} SgSampler;

// Samples a texture. coord: 2D uses xy, cube xyz (direction), volume xyz. ddx/ddy: screen-space
// derivatives of coord (may be NULL = zero). lod_override: NaN = derive the LOD from ddx/ddy,
// otherwise the explicit LOD (sampler lod_bias is still added; to emulate a D3D "bias" lookup scale
// ddx/ddy by 2^bias). out = RGBA 0..1; an unbound/empty texture yields (0,0,0,1).
void sg_sample(const SgTexture *t, const SgSampler *s, const float coord[4], const float ddx[4], const float ddy[4], float lod_override, float out[4]);

typedef struct SgVertex {
    float x, y, z, w;            // clip space
    float c[4];                  // colour 0..1 (fixed-function path only)
    float t[SG_UNITS][2];        // texture coordinates (fixed-function path only)
    float fog;                   // 1 = no fog, 0 = fully fogged (fixed-function path only)
    float psize;                 // point size when SgState.point_size_vertex
    float v[SG_VARYINGS][4];     // generic varyings; only the first SgState.nvary are read
} SgVertex;

// Pixel stage input. Only the first SgState.nvary entries of v/dx/dy are valid.
typedef struct SgPixelIn {
    float pos[4];                // window x, y (pixel centre, top-left origin), z (0..1 depth, after bias), 1/w
    float v[SG_VARYINGS][4];     // perspective-correct interpolated varyings
    float dx[SG_VARYINGS][4], dy[SG_VARYINGS][4]; // exact screen-space derivatives of v (zero for lines)
    int front_facing;
} SgPixelIn;
// Returns 0 to discard the pixel. *depth_out is preset to the interpolated z and may be changed
// (set SgState.pixel_writes_depth then). color_out is RGBA, clamped to 0..1 by the rasteriser.
typedef int (*SgPixelFn)(void *ctx, const SgPixelIn *in, float color_out[4], float *depth_out);

typedef struct SgStencilFace {
    int func;                    // SG_NEVER..SG_ALWAYS: passes if (ref & read_mask) func (stencil & read_mask)
    int ref;
    int read_mask, write_mask;   // 0..255
    int fail_op, zfail_op, zpass_op; // SG_SOP_*
} SgStencilFace;

typedef struct SgState {
    int vp_x, vp_y, vp_w, vp_h;  // viewport in window coordinates (origin bottom-left, or top-left with vp_top_left)
    float depth_near, depth_far;
    int scissor, sc_x, sc_y, sc_w, sc_h;   // same origin as the viewport
    int depth_test, depth_write, depth_func;
    int blend, blend_src, blend_dst;
    int alpha_test, alpha_func;
    float alpha_ref;
    int cull, cull_back, cull_front, front_ccw;
    int tex_on[SG_UNITS];
    SgTexture *tex[SG_UNITS];
    int tex_env[SG_UNITS];
    float env_color[SG_UNITS][4];
    int fog;
    float fog_color[4];
    int mask_r, mask_g, mask_b, mask_a;
    int flat;                    // flat shading: colour of the provoking vertex
    float line_width, point_size;
    float polygon_offset;        // depth bias in depth units

    // ---- additions (zero = OpenGL fixed-function behaviour) ----
    int vp_top_left;             // viewport and scissor origin is top-left (D3D)
    int clip_z_zero;             // clip volume is 0 <= z <= w (D3D) and NDC z maps directly to depth
    int depth_clamp;             // do not clip against near/far; clamp depth to [near, far] instead
    float depth_bias;            // constant bias added to depth (depth units)
    float slope_scale_bias;      // times max(|dz/dx|, |dz/dy|)
    int nvary;                   // varyings used (0..SG_VARYINGS); others are neither clipped nor interpolated
    int flat_first;              // provoking vertex is the first (D3D) instead of the last (GL)
    uint32_t flat_mask;          // varyings that are constant over a primitive (provoking vertex value)
    SgPixelFn pixel_fn;          // programmable pixel stage replaces texture/colour/fog when set
    void *pixel_ctx;
    int pixel_writes_depth;      // pixel_fn may change depth (disables early depth rejection)
    // stencil (needs SgTarget.stencil; with no stencil buffer the test always passes)
    int stencil_test, stencil_two_sided;
    SgStencilFace stencil[2];    // [0] front faces (and all faces unless two sided), [1] back faces
    // blending
    int blend_sep_alpha;         // use blend_*_a for the alpha channel
    int blend_src_a, blend_dst_a;
    int blend_op, blend_op_a;    // SG_BLEND_*
    float blend_color[4];
    // user clip planes in clip space: inside when dot(plane, (x,y,z,w)) >= 0
    int clip_mask;
    float clip_plane[SG_MAX_CLIP_PLANES][4];
    // points
    int point_sprite;            // square point with texture coordinates 0..1 (top-left origin)
    int sprite_slot;             // varying receiving the sprite coordinate (xy = s,t; zw = 0,1); also fixed unit 0 t
    int point_size_vertex;       // size comes from SgVertex.psize
    float point_size_min, point_size_max; // clamp when point_size_max > 0
} SgState;

typedef struct SgTarget {
    uint32_t *color;             // 0xAARRGGBB, top-down rows
    float *depth;
    int w, h;
    uint8_t *stencil;            // optional, w*h bytes
} SgTarget;

void sg_triangles(SgTarget *t, const SgState *s, const SgVertex *v, int n); // n = 3 * triangles
void sg_lines(SgTarget *t, const SgState *s, const SgVertex *v, int n);     // n = 2 * lines
void sg_points(SgTarget *t, const SgState *s, const SgVertex *v, int n);
void sg_clear(SgTarget *t, const SgState *s, int color, uint32_t argb, int depth, float z);

enum { SG_CLEAR_COLOR = 1, SG_CLEAR_DEPTH = 2, SG_CLEAR_STENCIL = 4, SG_CLEAR_NOMASK = 8 };
// Clears the selected buffers inside the scissor rectangle (if enabled). Colour respects the colour
// masks, depth respects depth_write, stencil respects stencil[0].write_mask, unless SG_CLEAR_NOMASK.
void sg_clear_ex(SgTarget *t, const SgState *s, int flags, uint32_t argb, float z, uint8_t stencil);

void sg_texture_build_mips(SgTexture *tex);   // 2D, cube (6 faces) and volume
void sg_texture_free(SgTexture *tex);
uint64_t sg_pixels_drawn(void);
