// SoulCore - "sgpu": the fixed-function 3D pipeline behind Soul's graphics APIs.
//
// Front-ends (OpenGL today, Direct3D later) transform vertices to clip space and
// hand batches of primitives plus a render state to this backend. The current
// backend rasterises on the CPU (perspective-correct, depth-buffered, textured,
// blended); the interface is deliberately GPU-shaped so that a Metal backend can
// replace it without touching the front-ends.
#pragma once
#include <stdint.h>

#define SG_UNITS 2

enum { SG_NEVER, SG_LESS, SG_EQUAL, SG_LEQUAL, SG_GREATER, SG_NOTEQUAL, SG_GEQUAL, SG_ALWAYS };
enum {
    SG_ZERO, SG_ONE, SG_SRC_COLOR, SG_ONE_MINUS_SRC_COLOR, SG_SRC_ALPHA, SG_ONE_MINUS_SRC_ALPHA,
    SG_DST_ALPHA, SG_ONE_MINUS_DST_ALPHA, SG_DST_COLOR, SG_ONE_MINUS_DST_COLOR, SG_SRC_ALPHA_SATURATE,
};
enum { SG_ENV_MODULATE, SG_ENV_REPLACE, SG_ENV_DECAL, SG_ENV_ADD, SG_ENV_BLEND };
enum { SG_FMT_RGBA, SG_FMT_RGB, SG_FMT_ALPHA, SG_FMT_LUMINANCE, SG_FMT_LUMINANCE_ALPHA, SG_FMT_INTENSITY };

typedef struct SgTexture {
    int w, h;
    int format;          // SG_FMT_* (how the texel is combined)
    uint8_t *rgba;       // w*h*4 bytes, R G B A
    int min_linear, mag_linear;
    int clamp_s, clamp_t;
    int levels;          // mipmaps: rgba of level i at mip[i] (level 0 = rgba)
    uint8_t *mip[14];
    int mip_w[14], mip_h[14];
    int use_mips;
} SgTexture;

typedef struct SgVertex {
    float x, y, z, w;            // clip space
    float c[4];                  // colour 0..1
    float t[SG_UNITS][2];        // texture coordinates
    float fog;                   // 1 = no fog, 0 = fully fogged
} SgVertex;

typedef struct SgState {
    int vp_x, vp_y, vp_w, vp_h;  // viewport in window coordinates (origin bottom-left)
    float depth_near, depth_far;
    int scissor, sc_x, sc_y, sc_w, sc_h;
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
    int flat;                    // flat shading: colour of the provoking (last) vertex
    float line_width, point_size;
    float polygon_offset;        // depth bias in depth units
} SgState;

typedef struct SgTarget {
    uint32_t *color;             // 0xAARRGGBB, top-down rows
    float *depth;
    int w, h;
} SgTarget;

void sg_triangles(SgTarget *t, const SgState *s, const SgVertex *v, int n); // n = 3 * triangles
void sg_lines(SgTarget *t, const SgState *s, const SgVertex *v, int n);     // n = 2 * lines
void sg_points(SgTarget *t, const SgState *s, const SgVertex *v, int n);
void sg_clear(SgTarget *t, const SgState *s, int color, uint32_t argb, int depth, float z);
void sg_texture_build_mips(SgTexture *tex);
void sg_texture_free(SgTexture *tex);
uint64_t sg_pixels_drawn(void);
