// SoulCore - OpenGL 1.x (opengl32 / glu32 / WGL) front-end of the sgpu pipeline.
//
// Implements the fixed-function GL: matrix stacks, immediate mode and vertex
// arrays, per-vertex lighting, fog, texture objects with mipmaps, multitexture,
// display lists, blending/depth/alpha tests and WGL contexts. Frames are
// rendered into a per-window back buffer and copied into the window's pixels by
// SwapBuffers.
#include "gui.h"
#include "sgpu.h"
#include <math.h>

// ---- argument access ----------------------------------------------------------------------------
// Win64: the first four arguments travel in XMM0-3 when they are float/double. 32-bit stdcall: all
// arguments are on the stack, a float in one 4-byte slot and a double in two. fa(i) assumes every
// earlier argument is 4 bytes wide (ints, enums, floats, pointers) and da(i) that they are all
// doubles; functions that mix doubles with other arguments use DAS / IAS with explicit slots.
static inline double slot_double(X86State *st, int slot) { uint64_t u = hle_arg(st, slot) | (hle_arg(st, slot + 1) << 32); double d; memcpy(&d, &u, 8); return d; }
static inline float fa(X86State *st, int i) {
    if (!g_mode32 && i < 4) return st->xmm[i].f[0];
    uint32_t u = (uint32_t)hle_arg(st, i);
    float f; memcpy(&f, &u, 4); return f;
}
static inline double da(X86State *st, int i) {
    if (g_mode32) return slot_double(st, 2 * i);
    if (i < 4) return st->xmm[i].fd[0];
    uint64_t u = hle_arg(st, i);
    double d; memcpy(&d, &u, 8); return d;
}
#define DAS(i64, s32) (g_mode32 ? slot_double(st, (s32)) : da(st, (i64)))
#define IAS(i64, s32) ((int32_t)hle_arg(st, g_mode32 ? (s32) : (i64)))
#define PAS(i64, s32) G2H(hle_arg(st, g_mode32 ? (s32) : (i64)))
#define IA(i) ((int32_t)hle_arg(st, (i)))
#define UA(i) ((uint32_t)hle_arg(st, (i)))
#define PA(i) G2H(hle_arg(st, (i)))
// Constant strings returned to the program: guest-memory copies (made once per process).
static uint64_t gl_str(const char *s) {
    if (!g_mode32) return (uint64_t)(uintptr_t)s;
    static const char *keys[16];
    static uint64_t vals[16];
    static SoulProcess *owner;
    if (owner != g_proc) { memset(keys, 0, sizeof keys); owner = g_proc; }
    for (int i = 0; i < 16; i++) if (keys[i] == s) return vals[i];
    uint64_t g = H2G(g_strdup(s));
    for (int i = 0; i < 16; i++) if (!keys[i]) { keys[i] = s; vals[i] = g; break; }
    return g;
}

// ---- GL enums used here ------------------------------------------------------------------------
enum {
    GL_POINTS = 0, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP, GL_POLYGON,
    GL_BYTE = 0x1400, GL_UNSIGNED_BYTE = 0x1401, GL_SHORT = 0x1402, GL_UNSIGNED_SHORT = 0x1403, GL_INT = 0x1404, GL_UNSIGNED_INT = 0x1405,
    GL_FLOAT = 0x1406, GL_DOUBLE = 0x140A,
    GL_MODELVIEW = 0x1700, GL_PROJECTION = 0x1701, GL_TEXTURE = 0x1702,
    GL_COMPILE = 0x1300, GL_COMPILE_AND_EXECUTE = 0x1301,
    GL_FRONT = 0x404, GL_BACK = 0x405, GL_FRONT_AND_BACK = 0x408,
    GL_CW = 0x900, GL_CCW = 0x901,
    GL_TEXTURE_2D = 0x0DE1, GL_DEPTH_TEST = 0x0B71, GL_BLEND = 0x0BE2, GL_ALPHA_TEST = 0x0BC0, GL_CULL_FACE = 0x0B44,
    GL_LIGHTING = 0x0B50, GL_LIGHT0 = 0x4000, GL_FOG = 0x0B60, GL_COLOR_MATERIAL = 0x0B57, GL_NORMALIZE = 0x0BA1,
    GL_SCISSOR_TEST = 0x0C11, GL_POLYGON_OFFSET_FILL = 0x8037, GL_TEXTURE_GEN_S = 0x0C60, GL_TEXTURE_GEN_T = 0x0C61,
    GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078,
    GL_AMBIENT = 0x1200, GL_DIFFUSE = 0x1201, GL_SPECULAR = 0x1202, GL_POSITION = 0x1203, GL_SPOT_DIRECTION = 0x1204,
    GL_SPOT_EXPONENT = 0x1205, GL_SPOT_CUTOFF = 0x1206, GL_CONSTANT_ATTENUATION = 0x1207, GL_LINEAR_ATTENUATION = 0x1208,
    GL_QUADRATIC_ATTENUATION = 0x1209, GL_EMISSION = 0x1600, GL_SHININESS = 0x1601, GL_AMBIENT_AND_DIFFUSE = 0x1602,
    GL_LIGHT_MODEL_AMBIENT = 0x0B53, GL_LIGHT_MODEL_TWO_SIDE = 0x0B52,
    GL_FOG_MODE = 0x0B65, GL_FOG_DENSITY = 0x0B62, GL_FOG_START = 0x0B63, GL_FOG_END = 0x0B64, GL_FOG_COLOR = 0x0B66,
    GL_EXP = 0x0800, GL_EXP2 = 0x0801, GL_LINEAR = 0x2601, GL_NEAREST = 0x2600,
    GL_FLAT = 0x1D00, GL_SMOOTH = 0x1D01, GL_LINE = 0x1B01, GL_FILL = 0x1B02,
    GL_TEXTURE_ENV_MODE = 0x2200, GL_TEXTURE_ENV_COLOR = 0x2201, GL_MODULATE = 0x2100, GL_DECAL = 0x2101, GL_REPLACE = 0x1E01, GL_ADD = 0x0104,
    GL_TEXTURE_MAG_FILTER = 0x2800, GL_TEXTURE_MIN_FILTER = 0x2801, GL_TEXTURE_WRAP_S = 0x2802, GL_TEXTURE_WRAP_T = 0x2803,
    GL_CLAMP = 0x2900, GL_REPEAT = 0x2901, GL_CLAMP_TO_EDGE = 0x812F, GL_CLAMP_TO_BORDER = 0x812D, GL_GENERATE_MIPMAP = 0x8191,
    GL_ALPHA = 0x1906, GL_RGB = 0x1907, GL_RGBA = 0x1908, GL_LUMINANCE = 0x1909, GL_LUMINANCE_ALPHA = 0x190A, GL_RED = 0x1903,
    GL_BGR = 0x80E0, GL_BGRA = 0x80E1, GL_INTENSITY = 0x8049,
    GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033, GL_UNSIGNED_SHORT_5_5_5_1 = 0x8034, GL_UNSIGNED_SHORT_5_6_5 = 0x8363,
    GL_UNSIGNED_INT_8_8_8_8 = 0x8035, GL_UNSIGNED_INT_8_8_8_8_REV = 0x8367,
    GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x0100,
    GL_TEXTURE0 = 0x84C0,
    GL_VENDOR = 0x1F00, GL_RENDERER = 0x1F01, GL_VERSION = 0x1F02, GL_EXTENSIONS = 0x1F03,
    GL_INVALID_ENUM = 0x500, GL_INVALID_VALUE = 0x501, GL_INVALID_OPERATION = 0x502, GL_STACK_OVERFLOW = 0x503, GL_STACK_UNDERFLOW = 0x504,
};

// ---- state -----------------------------------------------------------------------------------

typedef struct Vtx { float p[4], c[4], n[3], t[SG_UNITS][4]; } Vtx;

typedef struct GlLight {
    int on;
    float amb[4], dif[4], spec[4], pos[4], dir[3];
    float spot_exp, spot_cut, att[3];
} GlLight;
typedef struct GlMat { float amb[4], dif[4], spec[4], emi[4], shin; } GlMat;
typedef struct GlArr { int on, size, type, stride; uint64_t ptr; } GlArr;

typedef struct Cmd { int op; int i[4]; float f[16]; } Cmd;
typedef struct GlList { int used; Cmd *cmds; int n, cap; } GlList;

typedef struct GlTex { int used; SgTexture tex; int dirty_mips, gen_mips; } GlTex;

#define MAX_TEX 4096
#define MAX_LISTS 8192

typedef struct GlCtx GlCtx;
typedef struct GlAttr { SgState sg; int lighting, color_mat, fog_on, tex2d[SG_UNITS], normalize, polygon_mode, shade_flat; float color[4]; } GlAttr;

struct GlCtx {
    int used;
    uint64_t hwnd;
    uint32_t tid;            // thread the context is current on (0 = none)
    int double_buffer;
    SgTarget fb;
    SgState sg;
    float mv[32][16], pj[8][16], tm[SG_UNITS][8][16];
    int mv_top, pj_top, tm_top[SG_UNITS], mode;
    int active_tex, client_tex;
    float color[4], normal[3], tc[SG_UNITS][4];
    int in_begin, prim;
    Vtx *imm;
    int imm_n, imm_cap;
    int lighting, color_mat, cm_face, cm_mode, normalize;
    float light_ambient[4];
    GlLight lights[8];
    GlMat mat[2];
    int fog_on, fog_mode;
    float fog_start, fog_end, fog_density;
    int tex2d[SG_UNITS];
    uint32_t bound[SG_UNITS];
    GlArr va, ca, na, ta[SG_UNITS];
    uint32_t list_id;
    int list_mode, list_base;
    int polygon_mode, cull_mode;
    float clear_color[4], clear_depth;
    int unpack_align, unpack_row, pack_align;
    int shade_flat;
    uint32_t err;
    float raster[4];
    int raster_valid;
    GlAttr attr_stack[16];
    int attr_top;
    double last_swap;
    int swap_interval;
    int viewport_set;
};

static GlCtx g_ctx[16];
static GlTex g_tex[MAX_TEX];
static GlList g_lists[MAX_LISTS];
static struct { uint64_t hwnd; int double_buffer; } g_pfd[64];

#define CTX_HANDLE_BASE 0x70000
static GlCtx *current(void) {
    SoulThread *t = soul_cur_thread();
    uint32_t tid = t ? t->tid : 0;
    for (int i = 0; i < 16; i++) if (g_ctx[i].used && g_ctx[i].tid == tid) return &g_ctx[i];
    return NULL;
}
static GlCtx *ctx_of(uint64_t h) {
    if (h < CTX_HANDLE_BASE || h >= CTX_HANDLE_BASE + 16) return NULL;
    GlCtx *c = &g_ctx[h - CTX_HANDLE_BASE];
    return c->used ? c : NULL;
}
static void seterr(GlCtx *c, uint32_t e) { if (c && !c->err) c->err = e; }

// ---- matrices (column-major, like GL) ----------------------------------------------------------
static void m_ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void m_mul(float *r, const float *a, const float *b) { // r = a * b
    float t[16];
    for (int c = 0; c < 4; c++) for (int rr = 0; rr < 4; rr++) {
        float s = 0;
        for (int k = 0; k < 4; k++) s += a[k * 4 + rr] * b[c * 4 + k];
        t[c * 4 + rr] = s;
    }
    memcpy(r, t, 64);
}
static void m_xform(const float *m, const float *v, float *o) {
    float x = v[0], y = v[1], z = v[2], w = v[3];
    for (int i = 0; i < 4; i++) o[i] = m[i] * x + m[4 + i] * y + m[8 + i] * z + m[12 + i] * w;
}
static float *cur_mat(GlCtx *c) {
    if (c->mode == GL_PROJECTION) return c->pj[c->pj_top];
    if (c->mode == GL_TEXTURE) return c->tm[c->active_tex][c->tm_top[c->active_tex]];
    return c->mv[c->mv_top];
}
static void m_rotate(float *m, float ang, float x, float y, float z) {
    float len = sqrtf(x * x + y * y + z * z);
    m_ident(m);
    if (len < 1e-12f) return;
    x /= len; y /= len; z /= len;
    float a = ang * (float)M_PI / 180.0f, cs = cosf(a), sn = sinf(a), ic = 1 - cs;
    m[0] = x * x * ic + cs;     m[4] = x * y * ic - z * sn; m[8] = x * z * ic + y * sn;
    m[1] = y * x * ic + z * sn; m[5] = y * y * ic + cs;     m[9] = y * z * ic - x * sn;
    m[2] = x * z * ic - y * sn; m[6] = y * z * ic + x * sn; m[10] = z * z * ic + cs;
}

// ---- framebuffer -------------------------------------------------------------------------------
static void fb_sync(GlCtx *c) {
    Win *w = win_get(c->hwnd);
    if (!w) return;
    int W = w->w > 0 ? w->w : 1, H = w->h > 0 ? w->h : 1;
    if (W > 4096) W = 4096;
    if (H > 4096) H = 4096;
    if (c->fb.color && c->fb.w == W && c->fb.h == H) return;
    free(c->fb.color); free(c->fb.depth);
    c->fb.w = W; c->fb.h = H;
    c->fb.color = calloc((size_t)W * (size_t)H, 4);
    c->fb.depth = malloc((size_t)W * (size_t)H * sizeof(float));
    for (size_t i = 0; i < (size_t)W * (size_t)H; i++) { c->fb.color[i] = 0xFF000000u; c->fb.depth[i] = 1.0f; }
    if (!c->viewport_set) { c->sg.vp_x = 0; c->sg.vp_y = 0; c->sg.vp_w = W; c->sg.vp_h = H; }
}

static void present(GlCtx *c) {
    Win *w = win_get(c->hwnd);
    if (!w || !c->fb.color) return;
    Win *top = win_top(w);
    if (!top || !top->pixels) return;
    int ox, oy, x0, y0, x1, y1;
    win_client_origin(w, &ox, &oy);
    if (!win_visible_rect(w, &x0, &y0, &x1, &y1)) return;
    for (int y = y0; y < y1; y++) {
        int sy = y - oy;
        if (sy < 0 || sy >= c->fb.h) continue;
        uint32_t *dst = top->pixels + (size_t)y * (size_t)top->fb_w;
        const uint32_t *src = c->fb.color + (size_t)sy * (size_t)c->fb.w;
        for (int x = x0; x < x1; x++) {
            int sx = x - ox;
            if (sx >= 0 && sx < c->fb.w) dst[x] = src[sx] | 0xFF000000u;
        }
    }
    win_touch(top);
}

// ---- textures ----------------------------------------------------------------------------------
static GlTex *tex_obj(uint32_t id) {
    if (id >= MAX_TEX) return NULL;
    g_tex[id].used = 1;
    return &g_tex[id];
}
static GlTex *bound_tex(GlCtx *c) { return tex_obj(c->bound[c->active_tex]); }

static int sg_format_for(int internal) {
    switch (internal) {
    case 1: case GL_LUMINANCE: case 0x8040: case 0x8041: case GL_RED: return SG_FMT_LUMINANCE;
    case 2: case GL_LUMINANCE_ALPHA: case 0x8045: return SG_FMT_LUMINANCE_ALPHA;
    case GL_ALPHA: case 0x803C: return SG_FMT_ALPHA;
    case GL_INTENSITY: case 0x804B: return SG_FMT_INTENSITY;
    case 3: case GL_RGB: case 0x8051: case 0x8050: case 0x8052: case 0x8053: case GL_BGR: return SG_FMT_RGB;
    default: return SG_FMT_RGBA;
    }
}

// Converts client pixel data to RGBA8. Returns malloc'ed buffer.
static uint8_t *unpack(GlCtx *c, int w, int h, int format, int type, const uint8_t *src) {
    uint8_t *out = malloc((size_t)w * (size_t)h * 4 + 4);
    int comps = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB || format == GL_BGR ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1;
    int packed = type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1;
    int packed32 = type == GL_UNSIGNED_INT_8_8_8_8_REV || type == GL_UNSIGNED_INT_8_8_8_8;
    int bpc = type == GL_FLOAT ? 4 : (type == GL_UNSIGNED_SHORT || type == GL_SHORT) ? 2 : 1;
    int pixel = packed ? 2 : packed32 ? 4 : comps * bpc;
    int rowlen = c->unpack_row > 0 ? c->unpack_row : w;
    size_t stride = (size_t)rowlen * (size_t)pixel;
    int al = c->unpack_align > 0 ? c->unpack_align : 4;
    stride = (stride + (size_t)al - 1) / (size_t)al * (size_t)al;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = src ? src + (size_t)y * stride : NULL;
        for (int x = 0; x < w; x++) {
            uint8_t *o = out + ((size_t)y * (size_t)w + (size_t)x) * 4;
            if (!row) { o[0] = o[1] = o[2] = 0; o[3] = 255; continue; }
            const uint8_t *p = row + (size_t)x * (size_t)pixel;
            float v[4] = { 0, 0, 0, 1 };
            if (packed) {
                uint16_t s; memcpy(&s, p, 2);
                if (type == GL_UNSIGNED_SHORT_5_6_5) { v[0] = ((s >> 11) & 31) / 31.0f; v[1] = ((s >> 5) & 63) / 63.0f; v[2] = (s & 31) / 31.0f; }
                else if (type == GL_UNSIGNED_SHORT_4_4_4_4) { v[0] = ((s >> 12) & 15) / 15.0f; v[1] = ((s >> 8) & 15) / 15.0f; v[2] = ((s >> 4) & 15) / 15.0f; v[3] = (s & 15) / 15.0f; }
                else { v[0] = ((s >> 11) & 31) / 31.0f; v[1] = ((s >> 6) & 31) / 31.0f; v[2] = ((s >> 1) & 31) / 31.0f; v[3] = (float)(s & 1); }
                if (format == GL_BGRA || format == GL_BGR) { float t = v[0]; v[0] = v[2]; v[2] = t; }
            } else if (packed32) {
                uint32_t s; memcpy(&s, p, 4);
                if (type == GL_UNSIGNED_INT_8_8_8_8) s = __builtin_bswap32(s);
                for (int k = 0; k < 4; k++) v[k] = ((s >> (8 * k)) & 255) / 255.0f;
                if (format == GL_BGRA) { float t = v[0]; v[0] = v[2]; v[2] = t; }
            } else {
                float ch[4] = { 0, 0, 0, 1 };
                for (int k = 0; k < comps; k++) {
                    if (type == GL_FLOAT) { float f; memcpy(&f, p + 4 * k, 4); ch[k] = f; }
                    else if (bpc == 2) { uint16_t s; memcpy(&s, p + 2 * k, 2); ch[k] = s / 65535.0f; }
                    else ch[k] = p[k] / 255.0f;
                }
                switch (format) {
                case GL_RGBA: memcpy(v, ch, 16); break;
                case GL_RGB: v[0] = ch[0]; v[1] = ch[1]; v[2] = ch[2]; break;
                case GL_BGRA: v[0] = ch[2]; v[1] = ch[1]; v[2] = ch[0]; v[3] = ch[3]; break;
                case GL_BGR: v[0] = ch[2]; v[1] = ch[1]; v[2] = ch[0]; break;
                case GL_LUMINANCE: case GL_RED: v[0] = v[1] = v[2] = ch[0]; break;
                case GL_LUMINANCE_ALPHA: v[0] = v[1] = v[2] = ch[0]; v[3] = ch[1]; break;
                case GL_ALPHA: v[3] = ch[0]; break;
                case GL_INTENSITY: v[0] = v[1] = v[2] = v[3] = ch[0]; break;
                default: memcpy(v, ch, 16); break;
                }
            }
            for (int k = 0; k < 4; k++) { float f = v[k] < 0 ? 0 : v[k] > 1 ? 1 : v[k]; o[k] = (uint8_t)(f * 255.0f + 0.5f); }
        }
    }
    return out;
}

static void tex_image(GlCtx *c, int level, int internal, int w, int h, int format, int type, const void *data, int mips) {
    GlTex *t = bound_tex(c);
    if (!t || w <= 0 || h <= 0 || w > 8192 || h > 8192) { seterr(c, GL_INVALID_VALUE); return; }
    if (level > 0) return; // mipmaps are generated from level 0
    uint8_t *px = unpack(c, w, h, format, type, data);
    int fmt = sg_format_for(internal);
    if (fmt == SG_FMT_INTENSITY) for (int i = 0; i < w * h; i++) px[i * 4 + 3] = px[i * 4];
    for (int i = 1; i < t->tex.levels; i++) free(t->tex.mip[i]);
    t->tex.levels = 0;
    free(t->tex.rgba);
    t->tex.rgba = px; t->tex.w = w; t->tex.h = h; t->tex.format = fmt;
    t->dirty_mips = 1;
    if (mips) t->gen_mips = 1;
}

static void tex_prepare(GlTex *t) {
    if (!t || !t->tex.rgba) return;
    if (t->tex.use_mips && t->dirty_mips) { sg_texture_build_mips(&t->tex); t->dirty_mips = 0; }
}

// ---- lighting --------------------------------------------------------------------------------
static void v3norm(float *v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-12f) { v[0] /= l; v[1] /= l; v[2] /= l; } }

static void light_vertex(GlCtx *c, const float *eye, const float *n, const float *col, float *out) {
    const GlMat *m = &c->mat[0];
    float amb[4], dif[4];
    memcpy(amb, m->amb, 16); memcpy(dif, m->dif, 16);
    if (c->color_mat) {
        if (c->cm_mode == GL_AMBIENT || c->cm_mode == GL_AMBIENT_AND_DIFFUSE) memcpy(amb, col, 16);
        if (c->cm_mode == GL_DIFFUSE || c->cm_mode == GL_AMBIENT_AND_DIFFUSE) memcpy(dif, col, 16);
    }
    float emi[4], spec_m[4];
    memcpy(emi, m->emi, 16); memcpy(spec_m, m->spec, 16);
    if (c->color_mat && c->cm_mode == GL_EMISSION) memcpy(emi, col, 16);
    if (c->color_mat && c->cm_mode == GL_SPECULAR) memcpy(spec_m, col, 16);
    float r[3];
    for (int i = 0; i < 3; i++) r[i] = emi[i] + c->light_ambient[i] * amb[i];
    for (int li = 0; li < 8; li++) {
        const GlLight *L = &c->lights[li];
        if (!L->on) continue;
        float l[3], att = 1;
        if (L->pos[3] == 0) { l[0] = L->pos[0]; l[1] = L->pos[1]; l[2] = L->pos[2]; v3norm(l); }
        else {
            l[0] = L->pos[0] / L->pos[3] - eye[0]; l[1] = L->pos[1] / L->pos[3] - eye[1]; l[2] = L->pos[2] / L->pos[3] - eye[2];
            float d = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            if (d > 1e-12f) { l[0] /= d; l[1] /= d; l[2] /= d; }
            float k = L->att[0] + L->att[1] * d + L->att[2] * d * d;
            att = k > 1e-12f ? 1.0f / k : 1.0f;
            if (L->spot_cut < 179.9f) {
                float sd[3] = { L->dir[0], L->dir[1], L->dir[2] };
                v3norm(sd);
                float cs = -(l[0] * sd[0] + l[1] * sd[1] + l[2] * sd[2]);
                if (cs < cosf(L->spot_cut * (float)M_PI / 180.0f)) att = 0;
                else att *= powf(cs, L->spot_exp);
            }
        }
        if (att == 0) continue;
        float ndl = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
        if (ndl < 0) ndl = 0;
        float sp = 0;
        if (ndl > 0 && m->shin >= 0 && (L->spec[0] > 0 || L->spec[1] > 0 || L->spec[2] > 0)) {
            float h[3] = { l[0], l[1], l[2] + 1 };
            v3norm(h);
            float ndh = n[0] * h[0] + n[1] * h[1] + n[2] * h[2];
            if (ndh > 0) sp = m->shin > 0 ? powf(ndh, m->shin) : 1;
        }
        for (int i = 0; i < 3; i++) r[i] += att * (L->amb[i] * amb[i] + ndl * L->dif[i] * dif[i] + sp * L->spec[i] * spec_m[i]);
    }
    for (int i = 0; i < 3; i++) out[i] = r[i] < 0 ? 0 : r[i] > 1 ? 1 : r[i];
    out[3] = dif[3];
}

// ---- vertex processing and primitive assembly ------------------------------------------------
static void sync_state(GlCtx *c) {
    for (int u = 0; u < SG_UNITS; u++) {
        GlTex *t = tex_obj(c->bound[u]);
        c->sg.tex_on[u] = c->tex2d[u] && t && t->tex.rgba;
        if (c->sg.tex_on[u]) { tex_prepare(t); c->sg.tex[u] = &t->tex; }
        else c->sg.tex[u] = NULL;
    }
    c->sg.fog = c->fog_on;
    c->sg.flat = c->shade_flat;
}

static void transform(GlCtx *c, const Vtx *in, int n, SgVertex *out) {
    float mvp[16];
    const float *mv = c->mv[c->mv_top];
    m_mul(mvp, c->pj[c->pj_top], mv);
    // normal matrix: inverse transpose of the upper 3x3 of the modelview
    float nm[9];
    {
        float a = mv[0], b = mv[4], cc = mv[8], d = mv[1], e = mv[5], f = mv[9], g = mv[2], h = mv[6], k = mv[10];
        float det = a * (e * k - f * h) - b * (d * k - f * g) + cc * (d * h - e * g);
        float id = fabsf(det) > 1e-20f ? 1.0f / det : 0;
        // inverse (row-major), then transposed -> applied as n' = invT * n
        float inv[9] = { (e * k - f * h) * id, (cc * h - b * k) * id, (b * f - cc * e) * id,
                         (f * g - d * k) * id, (a * k - cc * g) * id, (cc * d - a * f) * id,
                         (d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id };
        for (int r = 0; r < 3; r++) for (int col = 0; col < 3; col++) nm[r * 3 + col] = inv[col * 3 + r];
    }
    int need_eye = c->lighting || c->fog_on;
    for (int i = 0; i < n; i++) {
        const Vtx *v = &in[i];
        SgVertex *o = &out[i];
        float clip[4];
        m_xform(mvp, v->p, clip);
        o->x = clip[0]; o->y = clip[1]; o->z = clip[2]; o->w = clip[3];
        float eye[4] = { 0, 0, 0, 1 };
        if (need_eye) m_xform(mv, v->p, eye);
        if (c->lighting) {
            float nn[3] = { nm[0] * v->n[0] + nm[1] * v->n[1] + nm[2] * v->n[2],
                            nm[3] * v->n[0] + nm[4] * v->n[1] + nm[5] * v->n[2],
                            nm[6] * v->n[0] + nm[7] * v->n[1] + nm[8] * v->n[2] };
            v3norm(nn);
            float e3[3] = { eye[0] / eye[3], eye[1] / eye[3], eye[2] / eye[3] };
            light_vertex(c, e3, nn, v->c, o->c);
        } else {
            for (int k = 0; k < 4; k++) o->c[k] = v->c[k] < 0 ? 0 : v->c[k] > 1 ? 1 : v->c[k];
        }
        for (int u = 0; u < SG_UNITS; u++) {
            if (!c->sg.tex_on[u]) { o->t[u][0] = o->t[u][1] = 0; continue; }
            float tt[4];
            m_xform(c->tm[u][c->tm_top[u]], v->t[u], tt);
            float q = tt[3] != 0 ? tt[3] : 1;
            o->t[u][0] = tt[0] / q; o->t[u][1] = tt[1] / q;
        }
        o->fog = 1;
        if (c->fog_on) {
            float d = fabsf(eye[2] / eye[3]);
            float f;
            if (c->fog_mode == GL_LINEAR) f = c->fog_end != c->fog_start ? (c->fog_end - d) / (c->fog_end - c->fog_start) : 1;
            else if (c->fog_mode == GL_EXP) f = expf(-c->fog_density * d);
            else f = expf(-(c->fog_density * d) * (c->fog_density * d));
            o->fog = f < 0 ? 0 : f > 1 ? 1 : f;
        }
    }
}

static void draw_prims(GlCtx *c, int mode, const Vtx *v, int n) {
    if (n <= 0 || !c->fb.color) return;
    sync_state(c);
    SgVertex *sv = malloc(sizeof(SgVertex) * (size_t)n);
    transform(c, v, n, sv);
    int cap = n * 6 + 8;
    SgVertex *out = malloc(sizeof(SgVertex) * (size_t)cap);
    int m = 0;
    int lines = mode == GL_LINES || mode == GL_LINE_STRIP || mode == GL_LINE_LOOP;
    if (mode == GL_POINTS) { sg_points(&c->fb, &c->sg, sv, n); free(sv); free(out); return; }
    if (lines) {
        if (mode == GL_LINES) for (int i = 0; i + 1 < n; i += 2) { out[m++] = sv[i]; out[m++] = sv[i + 1]; }
        else {
            for (int i = 0; i + 1 < n; i++) { out[m++] = sv[i]; out[m++] = sv[i + 1]; }
            if (mode == GL_LINE_LOOP && n > 2) { out[m++] = sv[n - 1]; out[m++] = sv[0]; }
        }
        sg_lines(&c->fb, &c->sg, out, m);
        free(sv); free(out);
        return;
    }
    int wire = c->polygon_mode == GL_LINE;
    if (wire) {
        // outline each polygon of the primitive
        #define EDGE(a, b) do { out[m++] = sv[a]; out[m++] = sv[b]; } while (0)
        switch (mode) {
        case GL_TRIANGLES: for (int i = 0; i + 2 < n; i += 3) { EDGE(i, i + 1); EDGE(i + 1, i + 2); EDGE(i + 2, i); } break;
        case GL_QUADS: for (int i = 0; i + 3 < n; i += 4) { EDGE(i, i + 1); EDGE(i + 1, i + 2); EDGE(i + 2, i + 3); EDGE(i + 3, i); } break;
        case GL_TRIANGLE_STRIP: for (int i = 0; i + 2 < n; i++) { EDGE(i, i + 1); EDGE(i + 1, i + 2); EDGE(i + 2, i); } break;
        case GL_QUAD_STRIP: for (int i = 0; i + 3 < n; i += 2) { EDGE(i, i + 1); EDGE(i + 1, i + 3); EDGE(i + 3, i + 2); EDGE(i + 2, i); } break;
        default: for (int i = 0; i < n; i++) EDGE(i, (i + 1) % n); break;
        }
        #undef EDGE
        sg_lines(&c->fb, &c->sg, out, m);
        free(sv); free(out);
        return;
    }
    #define TRI(a, b, cc) do { out[m++] = sv[a]; out[m++] = sv[b]; out[m++] = sv[cc]; } while (0)
    switch (mode) {
    case GL_TRIANGLES: for (int i = 0; i + 2 < n; i += 3) TRI(i, i + 1, i + 2); break;
    case GL_TRIANGLE_STRIP: for (int i = 0; i + 2 < n; i++) { if (i & 1) TRI(i + 1, i, i + 2); else TRI(i, i + 1, i + 2); } break;
    case GL_TRIANGLE_FAN: for (int i = 1; i + 1 < n; i++) TRI(0, i, i + 1); break;
    case GL_QUADS: for (int i = 0; i + 3 < n; i += 4) { TRI(i, i + 1, i + 3); TRI(i + 1, i + 2, i + 3); } break;
    case GL_QUAD_STRIP: for (int i = 0; i + 3 < n; i += 2) { TRI(i, i + 1, i + 3); TRI(i, i + 3, i + 2); } break;
    case GL_POLYGON: for (int i = 1; i + 1 < n; i++) TRI(i, i + 1, 0); break;
    default: break;
    }
    #undef TRI
    sg_triangles(&c->fb, &c->sg, out, m);
    free(sv); free(out);
}

// ---- display-list aware command execution ------------------------------------------------------
enum {
    OP_BEGIN = 1, OP_END, OP_VERTEX, OP_COLOR, OP_NORMAL, OP_TEXCOORD, OP_MATRIX_MODE, OP_LOAD_IDENTITY, OP_LOAD_MATRIX,
    OP_MULT_MATRIX, OP_PUSH, OP_POP, OP_ENABLE, OP_DISABLE, OP_BIND_TEXTURE, OP_MATERIAL, OP_LIGHT, OP_LIGHT_MODEL,
    OP_BLEND_FUNC, OP_DEPTH_FUNC, OP_DEPTH_MASK, OP_ALPHA_FUNC, OP_SHADE_MODEL, OP_CALL_LIST, OP_TEX_ENV, OP_COLOR_MATERIAL,
    OP_FOG, OP_LINE_WIDTH, OP_POINT_SIZE, OP_CULL_FACE, OP_FRONT_FACE, OP_ACTIVE_TEXTURE, OP_POLYGON_MODE, OP_RASTER_POS,
    OP_COLOR_MASK, OP_TRANSLATE, OP_SCALE,
};
static void exec(GlCtx *c, const Cmd *k);

static void submit(GlCtx *c, const Cmd *k) {
    if (c->list_id) {
        GlList *l = &g_lists[c->list_id];
        if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->cmds = realloc(l->cmds, sizeof(Cmd) * (size_t)l->cap); }
        l->cmds[l->n++] = *k;
        if (c->list_mode != GL_COMPILE_AND_EXECUTE) return;
    }
    exec(c, k);
}

static void set_material(GlCtx *c, int face, int pname, const float *f) {
    for (int s = 0; s < 2; s++) {
        if ((s == 0 && face == GL_BACK) || (s == 1 && face == GL_FRONT)) continue;
        GlMat *m = &c->mat[s];
        switch (pname) {
        case GL_AMBIENT: memcpy(m->amb, f, 16); break;
        case GL_DIFFUSE: memcpy(m->dif, f, 16); break;
        case GL_SPECULAR: memcpy(m->spec, f, 16); break;
        case GL_EMISSION: memcpy(m->emi, f, 16); break;
        case GL_SHININESS: m->shin = f[0]; break;
        case GL_AMBIENT_AND_DIFFUSE: memcpy(m->amb, f, 16); memcpy(m->dif, f, 16); break;
        default: break;
        }
    }
}

static void set_light(GlCtx *c, int light, int pname, const float *f) {
    if (light < GL_LIGHT0 || light >= GL_LIGHT0 + 8) { seterr(c, GL_INVALID_ENUM); return; }
    GlLight *L = &c->lights[light - GL_LIGHT0];
    switch (pname) {
    case GL_AMBIENT: memcpy(L->amb, f, 16); break;
    case GL_DIFFUSE: memcpy(L->dif, f, 16); break;
    case GL_SPECULAR: memcpy(L->spec, f, 16); break;
    case GL_POSITION: m_xform(c->mv[c->mv_top], f, L->pos); break; // stored in eye coordinates
    case GL_SPOT_DIRECTION: {
        const float *m = c->mv[c->mv_top];
        for (int i = 0; i < 3; i++) L->dir[i] = m[i] * f[0] + m[4 + i] * f[1] + m[8 + i] * f[2];
        break;
    }
    case GL_SPOT_EXPONENT: L->spot_exp = f[0]; break;
    case GL_SPOT_CUTOFF: L->spot_cut = f[0]; break;
    case GL_CONSTANT_ATTENUATION: L->att[0] = f[0]; break;
    case GL_LINEAR_ATTENUATION: L->att[1] = f[0]; break;
    case GL_QUADRATIC_ATTENUATION: L->att[2] = f[0]; break;
    default: break;
    }
}

static int sg_blend(int f) {
    switch (f) {
    case 0: return SG_ZERO;
    case 1: return SG_ONE;
    case 0x300: return SG_SRC_COLOR;
    case 0x301: return SG_ONE_MINUS_SRC_COLOR;
    case 0x302: return SG_SRC_ALPHA;
    case 0x303: return SG_ONE_MINUS_SRC_ALPHA;
    case 0x304: return SG_DST_ALPHA;
    case 0x305: return SG_ONE_MINUS_DST_ALPHA;
    case 0x306: return SG_DST_COLOR;
    case 0x307: return SG_ONE_MINUS_DST_COLOR;
    case 0x308: return SG_SRC_ALPHA_SATURATE;
    default: return SG_ONE;
    }
}

static void enable(GlCtx *c, int cap, int on) {
    switch (cap) {
    case GL_TEXTURE_2D: c->tex2d[c->active_tex] = on; break;
    case GL_DEPTH_TEST: c->sg.depth_test = on; break;
    case GL_BLEND: c->sg.blend = on; break;
    case GL_ALPHA_TEST: c->sg.alpha_test = on; break;
    case GL_CULL_FACE: c->sg.cull = on; break;
    case GL_LIGHTING: c->lighting = on; break;
    case GL_FOG: c->fog_on = on; break;
    case GL_COLOR_MATERIAL: c->color_mat = on; break;
    case GL_NORMALIZE: case 0x803A: c->normalize = on; break;
    case GL_SCISSOR_TEST: c->sg.scissor = on; break;
    case GL_POLYGON_OFFSET_FILL: if (!on) c->sg.polygon_offset = 0; break;
    default:
        if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) c->lights[cap - GL_LIGHT0].on = on;
        break;
    }
}

static void imm_vertex(GlCtx *c, const float *p) {
    if (!c->in_begin) return;
    if (c->imm_n == c->imm_cap) { c->imm_cap = c->imm_cap ? c->imm_cap * 2 : 256; c->imm = realloc(c->imm, sizeof(Vtx) * (size_t)c->imm_cap); }
    Vtx *v = &c->imm[c->imm_n++];
    memcpy(v->p, p, 16);
    memcpy(v->c, c->color, 16);
    memcpy(v->n, c->normal, 12);
    memcpy(v->t, c->tc, sizeof v->t);
}

static void exec(GlCtx *c, const Cmd *k) {
    const float *f = k->f;
    switch (k->op) {
    case OP_BEGIN: c->in_begin = 1; c->prim = k->i[0]; c->imm_n = 0; break;
    case OP_END:
        if (!c->in_begin) { seterr(c, GL_INVALID_OPERATION); break; }
        c->in_begin = 0;
        draw_prims(c, c->prim, c->imm, c->imm_n);
        break;
    case OP_VERTEX: imm_vertex(c, f); break;
    case OP_COLOR: memcpy(c->color, f, 16); break;
    case OP_NORMAL: memcpy(c->normal, f, 12); break;
    case OP_TEXCOORD: if (k->i[0] >= 0 && k->i[0] < SG_UNITS) memcpy(c->tc[k->i[0]], f, 16); break;
    case OP_MATRIX_MODE: c->mode = k->i[0]; break;
    case OP_LOAD_IDENTITY: m_ident(cur_mat(c)); break;
    case OP_LOAD_MATRIX: memcpy(cur_mat(c), f, 64); break;
    case OP_MULT_MATRIX: { float *m = cur_mat(c); m_mul(m, m, f); break; }
    case OP_TRANSLATE: { float *m = cur_mat(c); for (int i = 0; i < 4; i++) m[12 + i] += m[i] * f[0] + m[4 + i] * f[1] + m[8 + i] * f[2]; break; }
    case OP_SCALE: { float *m = cur_mat(c); for (int i = 0; i < 4; i++) { m[i] *= f[0]; m[4 + i] *= f[1]; m[8 + i] *= f[2]; } break; }
    case OP_PUSH:
        if (c->mode == GL_PROJECTION) { if (c->pj_top + 1 >= 8) { seterr(c, GL_STACK_OVERFLOW); break; } memcpy(c->pj[c->pj_top + 1], c->pj[c->pj_top], 64); c->pj_top++; }
        else if (c->mode == GL_TEXTURE) { int u = c->active_tex; if (c->tm_top[u] + 1 >= 8) { seterr(c, GL_STACK_OVERFLOW); break; } memcpy(c->tm[u][c->tm_top[u] + 1], c->tm[u][c->tm_top[u]], 64); c->tm_top[u]++; }
        else { if (c->mv_top + 1 >= 32) { seterr(c, GL_STACK_OVERFLOW); break; } memcpy(c->mv[c->mv_top + 1], c->mv[c->mv_top], 64); c->mv_top++; }
        break;
    case OP_POP:
        if (c->mode == GL_PROJECTION) { if (c->pj_top) c->pj_top--; else seterr(c, GL_STACK_UNDERFLOW); }
        else if (c->mode == GL_TEXTURE) { if (c->tm_top[c->active_tex]) c->tm_top[c->active_tex]--; else seterr(c, GL_STACK_UNDERFLOW); }
        else { if (c->mv_top) c->mv_top--; else seterr(c, GL_STACK_UNDERFLOW); }
        break;
    case OP_ENABLE: enable(c, k->i[0], 1); break;
    case OP_DISABLE: enable(c, k->i[0], 0); break;
    case OP_BIND_TEXTURE: if (k->i[0] == GL_TEXTURE_2D) c->bound[c->active_tex] = (uint32_t)k->i[1]; break;
    case OP_MATERIAL: set_material(c, k->i[0], k->i[1], f); break;
    case OP_LIGHT: set_light(c, k->i[0], k->i[1], f); break;
    case OP_LIGHT_MODEL: if (k->i[0] == GL_LIGHT_MODEL_AMBIENT) memcpy(c->light_ambient, f, 16); break;
    case OP_BLEND_FUNC: c->sg.blend_src = sg_blend(k->i[0]); c->sg.blend_dst = sg_blend(k->i[1]); break;
    case OP_DEPTH_FUNC: c->sg.depth_func = (k->i[0] - 0x200) & 7; break;
    case OP_DEPTH_MASK: c->sg.depth_write = k->i[0] != 0; break;
    case OP_ALPHA_FUNC: c->sg.alpha_func = (k->i[0] - 0x200) & 7; c->sg.alpha_ref = f[0]; break;
    case OP_SHADE_MODEL: c->shade_flat = k->i[0] == GL_FLAT; break;
    case OP_CALL_LIST: {
        uint32_t id = (uint32_t)k->i[0];
        if (id == 0 || id >= MAX_LISTS || !g_lists[id].used || id == c->list_id) break;
        GlList *l = &g_lists[id];
        for (int i = 0; i < l->n; i++) exec(c, &l->cmds[i]);
        break;
    }
    case OP_TEX_ENV: {
        int u = c->active_tex;
        if (k->i[0] == GL_TEXTURE_ENV_MODE) {
            int m = k->i[1];
            c->sg.tex_env[u] = m == GL_REPLACE ? SG_ENV_REPLACE : m == GL_DECAL ? SG_ENV_DECAL : m == GL_ADD ? SG_ENV_ADD : m == GL_BLEND ? SG_ENV_BLEND : SG_ENV_MODULATE;
        } else if (k->i[0] == GL_TEXTURE_ENV_COLOR) memcpy(c->sg.env_color[u], f, 16);
        break;
    }
    case OP_COLOR_MATERIAL: c->cm_face = k->i[0]; c->cm_mode = k->i[1]; break;
    case OP_FOG:
        switch (k->i[0]) {
        case GL_FOG_MODE: c->fog_mode = k->i[1]; break;
        case GL_FOG_DENSITY: c->fog_density = f[0]; break;
        case GL_FOG_START: c->fog_start = f[0]; break;
        case GL_FOG_END: c->fog_end = f[0]; break;
        case GL_FOG_COLOR: memcpy(c->sg.fog_color, f, 16); break;
        default: break;
        }
        break;
    case OP_LINE_WIDTH: c->sg.line_width = f[0]; break;
    case OP_POINT_SIZE: c->sg.point_size = f[0]; break;
    case OP_CULL_FACE: c->cull_mode = k->i[0]; c->sg.cull_back = k->i[0] != GL_FRONT; c->sg.cull_front = k->i[0] != GL_BACK; break;
    case OP_FRONT_FACE: c->sg.front_ccw = k->i[0] != GL_CW; break;
    case OP_ACTIVE_TEXTURE: { int u = k->i[0] - GL_TEXTURE0; if (u >= 0 && u < SG_UNITS) c->active_tex = u; break; }
    case OP_POLYGON_MODE: c->polygon_mode = k->i[1]; break;
    case OP_RASTER_POS: {
        float clip[4], eye[4];
        m_xform(c->mv[c->mv_top], f, eye);
        m_xform(c->pj[c->pj_top], eye, clip);
        if (clip[3] == 0) { c->raster_valid = 0; break; }
        c->raster[0] = c->sg.vp_x + (clip[0] / clip[3] + 1) * 0.5f * c->sg.vp_w;
        c->raster[1] = c->sg.vp_y + (clip[1] / clip[3] + 1) * 0.5f * c->sg.vp_h;
        c->raster[2] = (clip[2] / clip[3] + 1) * 0.5f;
        c->raster_valid = 1;
        break;
    }
    case OP_COLOR_MASK: c->sg.mask_r = k->i[0]; c->sg.mask_g = k->i[1]; c->sg.mask_b = k->i[2]; c->sg.mask_a = k->i[3]; break;
    default: break;
    }
}

#define CTX() GlCtx *c = current(); if (!c) { RET(0); return; }
static void op0(GlCtx *c, int op) { Cmd k = { op, { 0 }, { 0 } }; submit(c, &k); }
static void opi(GlCtx *c, int op, int a, int b) { Cmd k = { op, { a, b }, { 0 } }; submit(c, &k); }
static void opf(GlCtx *c, int op, int a, int b, const float *f, int nf) { Cmd k = { op, { a, b }, { 0 } }; memcpy(k.f, f, sizeof(float) * (size_t)nf); submit(c, &k); }

// ---- context creation and WGL ------------------------------------------------------------------
static void ctx_init(GlCtx *c, uint64_t hwnd) {
    memset(c, 0, sizeof *c);
    c->used = 1;
    c->hwnd = hwnd;
    c->double_buffer = 1;
    for (int i = 0; i < 64; i++) if (g_pfd[i].hwnd == hwnd) c->double_buffer = g_pfd[i].double_buffer;
    m_ident(c->mv[0]); m_ident(c->pj[0]);
    for (int u = 0; u < SG_UNITS; u++) { m_ident(c->tm[u][0]); c->tc[u][3] = 1; }
    c->mode = GL_MODELVIEW;
    c->color[0] = c->color[1] = c->color[2] = c->color[3] = 1;
    c->normal[2] = 1;
    c->sg.depth_near = 0; c->sg.depth_far = 1;
    c->sg.depth_func = SG_LESS; c->sg.depth_write = 1;
    c->sg.blend_src = SG_ONE; c->sg.blend_dst = SG_ZERO;
    c->sg.alpha_func = SG_ALWAYS;
    c->sg.front_ccw = 1; c->sg.cull_back = 1;
    c->sg.mask_r = c->sg.mask_g = c->sg.mask_b = c->sg.mask_a = 1;
    c->sg.line_width = 1; c->sg.point_size = 1;
    c->polygon_mode = GL_FILL;
    c->clear_depth = 1;
    c->unpack_align = 4; c->pack_align = 4;
    c->cm_face = GL_FRONT_AND_BACK; c->cm_mode = GL_AMBIENT_AND_DIFFUSE;
    float la[4] = { 0.2f, 0.2f, 0.2f, 1 };
    memcpy(c->light_ambient, la, 16);
    for (int i = 0; i < 8; i++) {
        GlLight *L = &c->lights[i];
        L->amb[3] = 1; L->pos[2] = 1; L->dir[2] = -1; L->spot_cut = 180; L->att[0] = 1;
        L->dif[3] = 1; L->spec[3] = 1;
        if (i == 0) { L->dif[0] = L->dif[1] = L->dif[2] = 1; L->spec[0] = L->spec[1] = L->spec[2] = 1; }
    }
    for (int s = 0; s < 2; s++) {
        GlMat *m = &c->mat[s];
        m->amb[0] = m->amb[1] = m->amb[2] = 0.2f; m->amb[3] = 1;
        m->dif[0] = m->dif[1] = m->dif[2] = 0.8f; m->dif[3] = 1;
        m->spec[3] = 1; m->emi[3] = 1;
    }
    c->fog_mode = GL_EXP; c->fog_density = 1; c->fog_end = 1;
    c->swap_interval = 1;
}

static uint64_t hwnd_of_dc(uint64_t hdc) {
    Dc *dc = dc_get(hdc);
    return dc ? dc->hwnd : 0;
}

HLE(w_wglCreateContext) {
    uint64_t hwnd = hwnd_of_dc(ARG(0));
    if (!hwnd) { RET(0); return; }
    for (int i = 0; i < 16; i++) if (!g_ctx[i].used) { ctx_init(&g_ctx[i], hwnd); RET(CTX_HANDLE_BASE + (uint64_t)i); return; }
    RET(0);
}
HLE(w_wglDeleteContext) {
    GlCtx *c = ctx_of(ARG(0));
    if (!c) { RET(0); return; }
    free(c->fb.color); free(c->fb.depth); free(c->imm);
    memset(c, 0, sizeof *c);
    RET(1);
}
HLE(w_wglMakeCurrent) {
    SoulThread *t = soul_cur_thread();
    uint32_t tid = t ? t->tid : 0;
    GlCtx *old = current();
    if (old) old->tid = 0;
    GlCtx *c = ctx_of(ARG(1));
    if (!c) { RET(ARG(1) == 0); return; }
    uint64_t hwnd = hwnd_of_dc(ARG(0));
    if (hwnd) c->hwnd = hwnd;
    c->tid = tid;
    fb_sync(c);
    RET(1);
}
HLE(w_wglGetCurrentContext) { GlCtx *c = current(); RET(c ? CTX_HANDLE_BASE + (uint64_t)(c - g_ctx) : 0); }
HLE(w_wglGetCurrentDC) { GlCtx *c = current(); RET(c ? gdi_create_dc(DC_WINDOW, c->hwnd) : 0); }
HLE_STUB_RET(w_wglShareLists, 1)
static const char *k_extensions =
    "GL_ARB_multitexture GL_EXT_bgra GL_EXT_texture_edge_clamp GL_SGIS_texture_edge_clamp GL_ARB_texture_non_power_of_two "
    "GL_EXT_texture_env_add GL_ARB_texture_env_add GL_EXT_compiled_vertex_array GL_SGIS_generate_mipmap GL_EXT_packed_pixels "
    "GL_ARB_transpose_matrix WGL_EXT_swap_control WGL_ARB_extensions_string WGL_EXT_extensions_string";
static const char *k_wgl_extensions = "WGL_EXT_swap_control WGL_ARB_extensions_string WGL_EXT_extensions_string";
HLE(w_wglGetExtensionsString) { RET(gl_str(k_wgl_extensions)); }
HLE(w_wglGetProcAddress) {
    const char *name = (const char *)PA(0);
    if (!name) { RET(0); return; }
    char base[128];
    snprintf(base, sizeof base, "%s", name);
    uint64_t a = hle_get_proc(g_proc, "opengl32.dll", base);
    if (!a) { // glFooARB / glFooEXT -> glFoo
        size_t n = strlen(base);
        if (n > 3 && (!strcmp(base + n - 3, "ARB") || !strcmp(base + n - 3, "EXT"))) { base[n - 3] = 0; a = hle_get_proc(g_proc, "opengl32.dll", base); }
    }
    RET(a);
}
HLE(w_wglSwapIntervalEXT) { GlCtx *c = current(); if (c) c->swap_interval = IA(0); RET(1); }
HLE(w_wglGetSwapIntervalEXT) { GlCtx *c = current(); RET(c ? c->swap_interval : 1); }
HLE_STUB_RET(w_wglUseFontBitmaps, 1)
HLE_STUB_RET(w_wglCreateContextAttribsARB, 0)

static void fill_pfd(uint8_t *pfd, int double_buffer) {
    memset(pfd, 0, 40);
    uint16_t size = 40, ver = 1;
    memcpy(pfd, &size, 2); memcpy(pfd + 2, &ver, 2);
    uint32_t flags = 0x4 | 0x20 | (double_buffer ? 0x1 : 0); // DRAW_TO_WINDOW | SUPPORT_OPENGL | DOUBLEBUFFER
    memcpy(pfd + 4, &flags, 4);
    pfd[8] = 0;   // PFD_TYPE_RGBA
    pfd[9] = 32;  // color bits
    pfd[10] = 8; pfd[11] = 16; pfd[12] = 8; pfd[13] = 8; pfd[14] = 8; pfd[15] = 0; pfd[16] = 8; pfd[17] = 24;
    pfd[23] = 24; // depth bits
    pfd[24] = 8;  // stencil bits
}
HLE(w_ChoosePixelFormat) { RET(1); }
HLE(w_SetPixelFormat) {
    uint64_t hwnd = hwnd_of_dc(ARG(0));
    const uint8_t *pfd = PA(2);
    int db = 1;
    if (pfd) { uint32_t fl; memcpy(&fl, pfd + 4, 4); db = (fl & 1) != 0; }
    for (int i = 0; i < 64; i++) if (!g_pfd[i].hwnd || g_pfd[i].hwnd == hwnd) { g_pfd[i].hwnd = hwnd; g_pfd[i].double_buffer = db; break; }
    RET(1);
}
HLE(w_DescribePixelFormat) { if (PA(3) && UA(2) >= 40) fill_pfd(PA(3), 1); RET(1); }
HLE(w_GetPixelFormat) { RET(1); }
HLE(w_SwapBuffers) {
    GlCtx *c = current();
    uint64_t hwnd = hwnd_of_dc(ARG(0));
    if (!c || (hwnd && c->hwnd != hwnd)) { for (int i = 0; i < 16; i++) if (g_ctx[i].used && g_ctx[i].hwnd == hwnd) c = &g_ctx[i]; }
    if (!c) { RET(0); return; }
    present(c);
    fb_sync(c);
    // pace to the display (vsync) so busy render loops do not burn the battery
    double now = soul_now();
    if (c->swap_interval > 0 && c->last_swap > 0) {
        double next = c->last_swap + (double)c->swap_interval / 60.0;
        if (now < next) { gil_wait((int64_t)((next - now) * 1000.0)); now = soul_now(); }
    }
    c->last_swap = now;
    RET(1);
}

// ---- GL entry points -----------------------------------------------------------------------------
HLE(gl_Begin) { CTX(); if (c->in_begin) { seterr(c, GL_INVALID_OPERATION); return; } opi(c, OP_BEGIN, IA(0), 0); }
HLE(gl_End) { CTX(); op0(c, OP_END); }

static void vtx(GlCtx *c, float x, float y, float z, float w) { float f[4] = { x, y, z, w }; opf(c, OP_VERTEX, 0, 0, f, 4); }
HLE(gl_Vertex2f) { CTX(); vtx(c, fa(st, 0), fa(st, 1), 0, 1); }
HLE(gl_Vertex3f) { CTX(); vtx(c, fa(st, 0), fa(st, 1), fa(st, 2), 1); }
HLE(gl_Vertex4f) { CTX(); vtx(c, fa(st, 0), fa(st, 1), fa(st, 2), fa(st, 3)); }
HLE(gl_Vertex2d) { CTX(); vtx(c, (float)da(st, 0), (float)da(st, 1), 0, 1); }
HLE(gl_Vertex3d) { CTX(); vtx(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2), 1); }
HLE(gl_Vertex4d) { CTX(); vtx(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2), (float)da(st, 3)); }
HLE(gl_Vertex2i) { CTX(); vtx(c, (float)IA(0), (float)IA(1), 0, 1); }
HLE(gl_Vertex3i) { CTX(); vtx(c, (float)IA(0), (float)IA(1), (float)IA(2), 1); }
HLE(gl_Vertex2s) { CTX(); vtx(c, (float)(int16_t)IA(0), (float)(int16_t)IA(1), 0, 1); }
HLE(gl_Vertex3s) { CTX(); vtx(c, (float)(int16_t)IA(0), (float)(int16_t)IA(1), (float)(int16_t)IA(2), 1); }
HLE(gl_Vertex2fv) { CTX(); const float *p = PA(0); vtx(c, p[0], p[1], 0, 1); }
HLE(gl_Vertex3fv) { CTX(); const float *p = PA(0); vtx(c, p[0], p[1], p[2], 1); }
HLE(gl_Vertex4fv) { CTX(); const float *p = PA(0); vtx(c, p[0], p[1], p[2], p[3]); }
HLE(gl_Vertex2dv) { CTX(); const double *p = PA(0); vtx(c, (float)p[0], (float)p[1], 0, 1); }
HLE(gl_Vertex3dv) { CTX(); const double *p = PA(0); vtx(c, (float)p[0], (float)p[1], (float)p[2], 1); }
HLE(gl_Vertex2iv) { CTX(); const int32_t *p = PA(0); vtx(c, (float)p[0], (float)p[1], 0, 1); }
HLE(gl_Vertex3iv) { CTX(); const int32_t *p = PA(0); vtx(c, (float)p[0], (float)p[1], (float)p[2], 1); }

static void col(GlCtx *c, float r, float g, float b, float a) { float f[4] = { r, g, b, a }; opf(c, OP_COLOR, 0, 0, f, 4); }
HLE(gl_Color3f) { CTX(); col(c, fa(st, 0), fa(st, 1), fa(st, 2), 1); }
HLE(gl_Color4f) { CTX(); col(c, fa(st, 0), fa(st, 1), fa(st, 2), fa(st, 3)); }
HLE(gl_Color3d) { CTX(); col(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2), 1); }
HLE(gl_Color4d) { CTX(); col(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2), (float)da(st, 3)); }
HLE(gl_Color3ub) { CTX(); col(c, (uint8_t)UA(0) / 255.0f, (uint8_t)UA(1) / 255.0f, (uint8_t)UA(2) / 255.0f, 1); }
HLE(gl_Color4ub) { CTX(); col(c, (uint8_t)UA(0) / 255.0f, (uint8_t)UA(1) / 255.0f, (uint8_t)UA(2) / 255.0f, (uint8_t)UA(3) / 255.0f); }
HLE(gl_Color3i) { CTX(); col(c, IA(0) / 2147483647.0f, IA(1) / 2147483647.0f, IA(2) / 2147483647.0f, 1); }
HLE(gl_Color3fv) { CTX(); const float *p = PA(0); col(c, p[0], p[1], p[2], 1); }
HLE(gl_Color4fv) { CTX(); const float *p = PA(0); col(c, p[0], p[1], p[2], p[3]); }
HLE(gl_Color3dv) { CTX(); const double *p = PA(0); col(c, (float)p[0], (float)p[1], (float)p[2], 1); }
HLE(gl_Color4dv) { CTX(); const double *p = PA(0); col(c, (float)p[0], (float)p[1], (float)p[2], (float)p[3]); }
HLE(gl_Color3ubv) { CTX(); const uint8_t *p = PA(0); col(c, p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, 1); }
HLE(gl_Color4ubv) { CTX(); const uint8_t *p = PA(0); col(c, p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f); }

static void nrm(GlCtx *c, float x, float y, float z) { float f[3] = { x, y, z }; opf(c, OP_NORMAL, 0, 0, f, 3); }
HLE(gl_Normal3f) { CTX(); nrm(c, fa(st, 0), fa(st, 1), fa(st, 2)); }
HLE(gl_Normal3d) { CTX(); nrm(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2)); }
HLE(gl_Normal3fv) { CTX(); const float *p = PA(0); nrm(c, p[0], p[1], p[2]); }
HLE(gl_Normal3dv) { CTX(); const double *p = PA(0); nrm(c, (float)p[0], (float)p[1], (float)p[2]); }
HLE(gl_Normal3i) { CTX(); nrm(c, (float)IA(0), (float)IA(1), (float)IA(2)); }

static void tcoord(GlCtx *c, int unit, float s, float t, float r, float q) { float f[4] = { s, t, r, q }; opf(c, OP_TEXCOORD, unit, 0, f, 4); }
HLE(gl_TexCoord1f) { CTX(); tcoord(c, 0, fa(st, 0), 0, 0, 1); }
HLE(gl_TexCoord2f) { CTX(); tcoord(c, 0, fa(st, 0), fa(st, 1), 0, 1); }
HLE(gl_TexCoord3f) { CTX(); tcoord(c, 0, fa(st, 0), fa(st, 1), fa(st, 2), 1); }
HLE(gl_TexCoord4f) { CTX(); tcoord(c, 0, fa(st, 0), fa(st, 1), fa(st, 2), fa(st, 3)); }
HLE(gl_TexCoord2d) { CTX(); tcoord(c, 0, (float)da(st, 0), (float)da(st, 1), 0, 1); }
HLE(gl_TexCoord2i) { CTX(); tcoord(c, 0, (float)IA(0), (float)IA(1), 0, 1); }
HLE(gl_TexCoord2fv) { CTX(); const float *p = PA(0); tcoord(c, 0, p[0], p[1], 0, 1); }
HLE(gl_TexCoord2dv) { CTX(); const double *p = PA(0); tcoord(c, 0, (float)p[0], (float)p[1], 0, 1); }
HLE(gl_MultiTexCoord2f) { CTX(); tcoord(c, IA(0) - GL_TEXTURE0, fa(st, 1), fa(st, 2), 0, 1); }
HLE(gl_MultiTexCoord2fv) { CTX(); const float *p = PA(1); tcoord(c, IA(0) - GL_TEXTURE0, p[0], p[1], 0, 1); }
HLE(gl_ActiveTexture) { CTX(); opi(c, OP_ACTIVE_TEXTURE, IA(0), 0); }
HLE(gl_ClientActiveTexture) { CTX(); int u = IA(0) - GL_TEXTURE0; if (u >= 0 && u < SG_UNITS) c->client_tex = u; }

static void rect(GlCtx *c, float x0, float y0, float x1, float y1) {
    opi(c, OP_BEGIN, GL_POLYGON, 0);
    vtx(c, x0, y0, 0, 1); vtx(c, x1, y0, 0, 1); vtx(c, x1, y1, 0, 1); vtx(c, x0, y1, 0, 1);
    op0(c, OP_END);
}
HLE(gl_Rectf) { CTX(); rect(c, fa(st, 0), fa(st, 1), fa(st, 2), fa(st, 3)); }
HLE(gl_Rectd) { CTX(); rect(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2), (float)da(st, 3)); }
HLE(gl_Recti) { CTX(); rect(c, (float)IA(0), (float)IA(1), (float)IA(2), (float)IA(3)); }

// matrices
HLE(gl_MatrixMode) { CTX(); opi(c, OP_MATRIX_MODE, IA(0), 0); }
HLE(gl_LoadIdentity) { CTX(); op0(c, OP_LOAD_IDENTITY); }
HLE(gl_LoadMatrixf) { CTX(); opf(c, OP_LOAD_MATRIX, 0, 0, PA(0), 16); }
HLE(gl_LoadMatrixd) { CTX(); const double *d = PA(0); float f[16]; for (int i = 0; i < 16; i++) f[i] = (float)d[i]; opf(c, OP_LOAD_MATRIX, 0, 0, f, 16); }
HLE(gl_MultMatrixf) { CTX(); opf(c, OP_MULT_MATRIX, 0, 0, PA(0), 16); }
HLE(gl_MultMatrixd) { CTX(); const double *d = PA(0); float f[16]; for (int i = 0; i < 16; i++) f[i] = (float)d[i]; opf(c, OP_MULT_MATRIX, 0, 0, f, 16); }
HLE(gl_LoadTransposeMatrixf) { CTX(); const float *s = PA(0); float f[16]; for (int i = 0; i < 16; i++) f[i] = s[(i % 4) * 4 + i / 4]; opf(c, OP_LOAD_MATRIX, 0, 0, f, 16); }
HLE(gl_MultTransposeMatrixf) { CTX(); const float *s = PA(0); float f[16]; for (int i = 0; i < 16; i++) f[i] = s[(i % 4) * 4 + i / 4]; opf(c, OP_MULT_MATRIX, 0, 0, f, 16); }
HLE(gl_PushMatrix) { CTX(); op0(c, OP_PUSH); }
HLE(gl_PopMatrix) { CTX(); op0(c, OP_POP); }
static void translate(GlCtx *c, float x, float y, float z) { float f[3] = { x, y, z }; opf(c, OP_TRANSLATE, 0, 0, f, 3); }
static void scale(GlCtx *c, float x, float y, float z) { float f[3] = { x, y, z }; opf(c, OP_SCALE, 0, 0, f, 3); }
static void rotate(GlCtx *c, float a, float x, float y, float z) { float m[16]; m_rotate(m, a, x, y, z); opf(c, OP_MULT_MATRIX, 0, 0, m, 16); }
HLE(gl_Translatef) { CTX(); translate(c, fa(st, 0), fa(st, 1), fa(st, 2)); }
HLE(gl_Translated) { CTX(); translate(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2)); }
HLE(gl_Scalef) { CTX(); scale(c, fa(st, 0), fa(st, 1), fa(st, 2)); }
HLE(gl_Scaled) { CTX(); scale(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2)); }
HLE(gl_Rotatef) { CTX(); rotate(c, fa(st, 0), fa(st, 1), fa(st, 2), fa(st, 3)); }
HLE(gl_Rotated) { CTX(); rotate(c, (float)da(st, 0), (float)da(st, 1), (float)da(st, 2), (float)da(st, 3)); }
static void ortho(GlCtx *c, double l, double r, double b, double t, double n, double f) {
    float m[16];
    m_ident(m);
    if (r == l || t == b || f == n) { seterr(c, GL_INVALID_VALUE); return; }
    m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
    m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n));
    opf(c, OP_MULT_MATRIX, 0, 0, m, 16);
}
static void frustum(GlCtx *c, double l, double r, double b, double t, double n, double f) {
    float m[16];
    memset(m, 0, 64);
    if (r == l || t == b || f == n || n <= 0) { seterr(c, GL_INVALID_VALUE); return; }
    m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b));
    m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b)); m[10] = (float)(-(f + n) / (f - n)); m[11] = -1;
    m[14] = (float)(-2 * f * n / (f - n));
    opf(c, OP_MULT_MATRIX, 0, 0, m, 16);
}
HLE(gl_Ortho) { CTX(); ortho(c, da(st, 0), da(st, 1), da(st, 2), da(st, 3), da(st, 4), da(st, 5)); }
HLE(gl_Frustum) { CTX(); frustum(c, da(st, 0), da(st, 1), da(st, 2), da(st, 3), da(st, 4), da(st, 5)); }

// state
HLE(gl_Enable) { CTX(); opi(c, OP_ENABLE, IA(0), 0); }
HLE(gl_Disable) { CTX(); opi(c, OP_DISABLE, IA(0), 0); }
HLE(gl_IsEnabled) {
    CTX();
    int cap = IA(0), r = 0;
    switch (cap) {
    case GL_TEXTURE_2D: r = c->tex2d[c->active_tex]; break;
    case GL_DEPTH_TEST: r = c->sg.depth_test; break;
    case GL_BLEND: r = c->sg.blend; break;
    case GL_ALPHA_TEST: r = c->sg.alpha_test; break;
    case GL_CULL_FACE: r = c->sg.cull; break;
    case GL_LIGHTING: r = c->lighting; break;
    case GL_FOG: r = c->fog_on; break;
    case GL_SCISSOR_TEST: r = c->sg.scissor; break;
    default: if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + 8) r = c->lights[cap - GL_LIGHT0].on; break;
    }
    RET(r);
}
HLE(gl_EnableClientState) {
    CTX();
    switch (IA(0)) {
    case GL_VERTEX_ARRAY: c->va.on = 1; break;
    case GL_COLOR_ARRAY: c->ca.on = 1; break;
    case GL_NORMAL_ARRAY: c->na.on = 1; break;
    case GL_TEXTURE_COORD_ARRAY: c->ta[c->client_tex].on = 1; break;
    default: break;
    }
}
HLE(gl_DisableClientState) {
    CTX();
    switch (IA(0)) {
    case GL_VERTEX_ARRAY: c->va.on = 0; break;
    case GL_COLOR_ARRAY: c->ca.on = 0; break;
    case GL_NORMAL_ARRAY: c->na.on = 0; break;
    case GL_TEXTURE_COORD_ARRAY: c->ta[c->client_tex].on = 0; break;
    default: break;
    }
}
HLE(gl_BlendFunc) { CTX(); opi(c, OP_BLEND_FUNC, IA(0), IA(1)); }
HLE(gl_DepthFunc) { CTX(); opi(c, OP_DEPTH_FUNC, IA(0), 0); }
HLE(gl_DepthMask) { CTX(); opi(c, OP_DEPTH_MASK, IA(0) & 0xFF, 0); }
HLE(gl_DepthRange) { CTX(); c->sg.depth_near = (float)da(st, 0); c->sg.depth_far = (float)da(st, 1); }
HLE(gl_AlphaFunc) { CTX(); float f = fa(st, 1); opf(c, OP_ALPHA_FUNC, IA(0), 0, &f, 1); }
HLE(gl_ShadeModel) { CTX(); opi(c, OP_SHADE_MODEL, IA(0), 0); }
HLE(gl_CullFace) { CTX(); opi(c, OP_CULL_FACE, IA(0), 0); }
HLE(gl_FrontFace) { CTX(); opi(c, OP_FRONT_FACE, IA(0), 0); }
HLE(gl_PolygonMode) { CTX(); opi(c, OP_POLYGON_MODE, IA(0), IA(1)); }
HLE(gl_PolygonOffset) { CTX(); c->sg.polygon_offset = (fa(st, 0) * 0.0001f + fa(st, 1) * 1e-6f); }
HLE(gl_LineWidth) { CTX(); float f = fa(st, 0); opf(c, OP_LINE_WIDTH, 0, 0, &f, 1); }
HLE(gl_PointSize) { CTX(); float f = fa(st, 0); opf(c, OP_POINT_SIZE, 0, 0, &f, 1); }
HLE(gl_ColorMask) { CTX(); Cmd k = { OP_COLOR_MASK, { IA(0) & 0xFF, IA(1) & 0xFF, IA(2) & 0xFF, IA(3) & 0xFF }, { 0 } }; submit(c, &k); }
HLE(gl_Scissor) { CTX(); c->sg.sc_x = IA(0); c->sg.sc_y = IA(1); c->sg.sc_w = IA(2); c->sg.sc_h = IA(3); }
HLE(gl_Viewport) { CTX(); fb_sync(c); c->sg.vp_x = IA(0); c->sg.vp_y = IA(1); c->sg.vp_w = IA(2); c->sg.vp_h = IA(3); c->viewport_set = 1; }
HLE(gl_ClearColor) { CTX(); c->clear_color[0] = fa(st, 0); c->clear_color[1] = fa(st, 1); c->clear_color[2] = fa(st, 2); c->clear_color[3] = fa(st, 3); }
HLE(gl_ClearDepth) { CTX(); c->clear_depth = (float)da(st, 0); }
HLE(gl_ClearDepthf) { CTX(); c->clear_depth = fa(st, 0); }
static uint8_t c8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255 + 0.5f); }
HLE(gl_Clear) {
    CTX();
    fb_sync(c);
    uint32_t m = UA(0);
    uint32_t argb = ((uint32_t)c8(c->clear_color[3]) << 24) | ((uint32_t)c8(c->clear_color[0]) << 16) | ((uint32_t)c8(c->clear_color[1]) << 8) | c8(c->clear_color[2]);
    sg_clear(&c->fb, &c->sg, (m & GL_COLOR_BUFFER_BIT) != 0, argb, (m & GL_DEPTH_BUFFER_BIT) != 0, c->clear_depth);
}
HLE(gl_Flush) { CTX(); if (!c->double_buffer) present(c); }
HLE(gl_Finish) { CTX(); if (!c->double_buffer) present(c); }
HLE(gl_GetError) { GlCtx *c = current(); uint32_t e = c ? c->err : 0; if (c) c->err = 0; RET(e); }
static const char *k_vendor = "Soul", *k_renderer = "Soul sgpu (Apple Silicon)", *k_version = "1.5.0 Soul";
HLE(gl_GetString) {
    switch (UA(0)) {
    case GL_VENDOR: RET(gl_str(k_vendor)); break;
    case GL_RENDERER: RET(gl_str(k_renderer)); break;
    case GL_VERSION: RET(gl_str(k_version)); break;
    case GL_EXTENSIONS: RET(gl_str(k_extensions)); break;
    default: RET(0); break;
    }
}
HLE_STUB_RET(gl_Noop, 0)

// light / material / fog / texture environment
static void fv4(const float *p, int scalar, float *o) { o[0] = p[0]; o[1] = scalar ? 0 : p[1]; o[2] = scalar ? 0 : p[2]; o[3] = scalar ? 0 : p[3]; }
static int is_scalar_light(int pname) { return pname >= GL_SPOT_EXPONENT && pname <= GL_QUADRATIC_ATTENUATION; }
HLE(gl_Lightf) { CTX(); float f[4] = { fa(st, 2), 0, 0, 0 }; opf(c, OP_LIGHT, IA(0), IA(1), f, 4); }
HLE(gl_Lighti) { CTX(); float f[4] = { (float)IA(2), 0, 0, 0 }; opf(c, OP_LIGHT, IA(0), IA(1), f, 4); }
HLE(gl_Lightfv) {
    CTX();
    int pn = IA(1);
    float f[4];
    if (pn == GL_SPOT_DIRECTION) { const float *p = PA(2); f[0] = p[0]; f[1] = p[1]; f[2] = p[2]; f[3] = 0; }
    else fv4(PA(2), is_scalar_light(pn), f);
    opf(c, OP_LIGHT, IA(0), pn, f, 4);
}
HLE(gl_Lightiv) { CTX(); const int32_t *p = PA(2); int pn = IA(1); float f[4] = { (float)p[0], 0, 0, 0 }; if (!is_scalar_light(pn)) { f[1] = (float)p[1]; f[2] = (float)p[2]; f[3] = pn == GL_SPOT_DIRECTION ? 0 : (float)p[3]; } opf(c, OP_LIGHT, IA(0), pn, f, 4); }
HLE(gl_LightModelf) { CTX(); (void)c; }
HLE(gl_LightModelfv) { CTX(); float f[4]; fv4(PA(1), IA(0) != GL_LIGHT_MODEL_AMBIENT, f); opf(c, OP_LIGHT_MODEL, IA(0), 0, f, 4); }
HLE(gl_Materialf) { CTX(); float f[4] = { fa(st, 2), 0, 0, 0 }; opf(c, OP_MATERIAL, IA(0), IA(1), f, 4); }
HLE(gl_Materialfv) { CTX(); float f[4]; fv4(PA(2), IA(1) == GL_SHININESS, f); opf(c, OP_MATERIAL, IA(0), IA(1), f, 4); }
HLE(gl_Materiali) { CTX(); float f[4] = { (float)IA(2), 0, 0, 0 }; opf(c, OP_MATERIAL, IA(0), IA(1), f, 4); }
HLE(gl_Materialiv) { CTX(); const int32_t *p = PA(2); float f[4] = { (float)p[0], (float)p[1], (float)p[2], (float)p[3] }; if (IA(1) == GL_SHININESS) f[1] = f[2] = f[3] = 0; opf(c, OP_MATERIAL, IA(0), IA(1), f, 4); }
HLE(gl_ColorMaterial) { CTX(); opi(c, OP_COLOR_MATERIAL, IA(0), IA(1)); }
HLE(gl_Fogf) { CTX(); float f = fa(st, 1); opf(c, OP_FOG, IA(0), (int)f, &f, 1); }
HLE(gl_Fogi) { CTX(); float f = (float)IA(1); opf(c, OP_FOG, IA(0), IA(1), &f, 1); }
HLE(gl_Fogfv) { CTX(); const float *p = PA(1); int pn = IA(0); float f[4]; fv4(p, pn != GL_FOG_COLOR, f); opf(c, OP_FOG, pn, (int)p[0], f, 4); }
HLE(gl_Fogiv) { CTX(); const int32_t *p = PA(1); float f[4] = { (float)p[0], 0, 0, 0 }; opf(c, OP_FOG, IA(0), p[0], f, 4); }
HLE(gl_TexEnvf) { CTX(); float f = fa(st, 2); opf(c, OP_TEX_ENV, IA(1), (int)f, &f, 1); }
HLE(gl_TexEnvi) { CTX(); float f = (float)IA(2); opf(c, OP_TEX_ENV, IA(1), IA(2), &f, 1); }
HLE(gl_TexEnvfv) { CTX(); const float *p = PA(2); float f[4]; fv4(p, 0, f); opf(c, OP_TEX_ENV, IA(1), (int)p[0], f, 4); }
HLE(gl_TexEnviv) { CTX(); const int32_t *p = PA(2); float f[4] = { (float)p[0], 0, 0, 0 }; opf(c, OP_TEX_ENV, IA(1), p[0], f, 4); }

// textures
static void tex_param(GlCtx *c, int pname, int v) {
    GlTex *t = bound_tex(c);
    if (!t) return;
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
        t->tex.min_linear = v == GL_LINEAR || v == 0x2701 || v == 0x2703;
        t->tex.use_mips = v >= 0x2700 && v <= 0x2703;
        if (t->tex.use_mips) t->dirty_mips = 1;
        break;
    case GL_TEXTURE_MAG_FILTER: t->tex.mag_linear = v == GL_LINEAR; break;
    case GL_TEXTURE_WRAP_S: t->tex.clamp_s = v != GL_REPEAT; break;
    case GL_TEXTURE_WRAP_T: t->tex.clamp_t = v != GL_REPEAT; break;
    case GL_GENERATE_MIPMAP: t->gen_mips = v != 0; break;
    default: break;
    }
}
HLE(gl_TexParameteri) { CTX(); tex_param(c, IA(1), IA(2)); }
HLE(gl_TexParameterf) { CTX(); tex_param(c, IA(1), (int)fa(st, 2)); }
HLE(gl_TexParameteriv) { CTX(); tex_param(c, IA(1), *(const int32_t *)PA(2)); }
HLE(gl_TexParameterfv) { CTX(); tex_param(c, IA(1), (int)*(const float *)PA(2)); }
HLE(gl_GenTextures) {
    int n = IA(0);
    uint32_t *out = PA(1);
    for (int i = 0, id = 1; i < n && id < MAX_TEX; id++) if (!g_tex[id].used) { g_tex[id].used = 1; memset(&g_tex[id].tex, 0, sizeof g_tex[id].tex); g_tex[id].tex.min_linear = 0; out[i++] = (uint32_t)id; }
    RET(0);
}
HLE(gl_DeleteTextures) {
    int n = IA(0);
    const uint32_t *ids = PA(1);
    for (int i = 0; i < n; i++) if (ids[i] && ids[i] < MAX_TEX && g_tex[ids[i]].used) { sg_texture_free(&g_tex[ids[i]].tex); memset(&g_tex[ids[i]], 0, sizeof g_tex[0]); }
    RET(0);
}
HLE(gl_BindTexture) { CTX(); if (UA(1) < MAX_TEX) { tex_obj(UA(1)); opi(c, OP_BIND_TEXTURE, IA(0), IA(1)); } }
HLE(gl_IsTexture) { RET(UA(0) && UA(0) < MAX_TEX && g_tex[UA(0)].used); }
HLE(gl_AreTexturesResident) { RET(1); }
HLE(gl_TexImage2D) {
    // (target, level, internalformat, width, height, border, format, type, pixels)
    CTX();
    if (IA(0) != GL_TEXTURE_2D) return;
    tex_image(c, IA(1), IA(2), IA(3), (int)(int32_t)hle_arg(st, 4), (int)(int32_t)hle_arg(st, 6), (int)(int32_t)hle_arg(st, 7), PA(8), 0);
}
HLE(gl_TexSubImage2D) {
    // (target, level, x, y, w, h, format, type, pixels)
    CTX();
    GlTex *t = bound_tex(c);
    int lv = IA(1), x = IA(2), y = IA(3), w = (int)(int32_t)hle_arg(st, 4), h = (int)(int32_t)hle_arg(st, 5);
    if (!t || !t->tex.rgba || lv > 0 || w <= 0 || h <= 0) return;
    uint8_t *px = unpack(c, w, h, (int)(int32_t)hle_arg(st, 6), (int)(int32_t)hle_arg(st, 7), PA(8));
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        int tx = x + xx, ty = y + yy;
        if (tx < 0 || ty < 0 || tx >= t->tex.w || ty >= t->tex.h) continue;
        memcpy(t->tex.rgba + ((size_t)ty * (size_t)t->tex.w + (size_t)tx) * 4, px + ((size_t)yy * (size_t)w + (size_t)xx) * 4, 4);
    }
    free(px);
    t->dirty_mips = 1;
}
HLE(gl_CopyTexImage2D) {
    // (target, level, internalformat, x, y, w, h, border): copy from the frame buffer
    CTX();
    int x = IA(3), y = (int)(int32_t)hle_arg(st, 4), w = (int)(int32_t)hle_arg(st, 5), h = (int)(int32_t)hle_arg(st, 6);
    if (IA(1) > 0 || w <= 0 || h <= 0 || !c->fb.color) return;
    uint8_t *px = malloc((size_t)w * (size_t)h * 4);
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        int sx = x + xx, sy = c->fb.h - 1 - (y + yy);
        uint32_t p = (sx >= 0 && sy >= 0 && sx < c->fb.w && sy < c->fb.h) ? c->fb.color[(size_t)sy * c->fb.w + sx] : 0;
        uint8_t *o = px + ((size_t)yy * w + xx) * 4;
        o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p; o[3] = (uint8_t)(p >> 24);
    }
    int saved_align = c->unpack_align, saved_row = c->unpack_row;
    c->unpack_align = 1; c->unpack_row = 0;
    tex_image(c, 0, IA(2), w, h, GL_RGBA, GL_UNSIGNED_BYTE, px, 0);
    c->unpack_align = saved_align; c->unpack_row = saved_row;
    free(px);
}
HLE(gl_GetTexLevelParameteriv) {
    CTX();
    GlTex *t = bound_tex(c);
    int32_t *out = PA(3);
    int lv = IA(1);
    int w = t && t->tex.rgba ? t->tex.w >> lv : 0, h = t && t->tex.rgba ? t->tex.h >> lv : 0;
    switch (IA(2)) {
    case 0x1000: *out = w; break; // GL_TEXTURE_WIDTH
    case 0x1001: *out = h; break; // GL_TEXTURE_HEIGHT
    default: *out = 0; break;
    }
}
HLE(gl_PixelStorei) {
    CTX();
    switch (IA(0)) {
    case 0x0CF5: c->unpack_align = IA(1); break;  // GL_UNPACK_ALIGNMENT
    case 0x0CF2: c->unpack_row = IA(1); break;    // GL_UNPACK_ROW_LENGTH
    case 0x0D05: c->pack_align = IA(1); break;    // GL_PACK_ALIGNMENT
    default: break;
    }
}
HLE(gl_PixelStoref) { CTX(); (void)c; }
HLE(gl_ReadPixels) {
    // (x, y, w, h, format, type, pixels)
    CTX();
    int x = IA(0), y = IA(1), w = IA(2), h = IA(3);
    int fmt = (int)(int32_t)hle_arg(st, 4);
    uint8_t *out = PA(6);
    if (!out || !c->fb.color) return;
    int comps = fmt == GL_RGB || fmt == GL_BGR ? 3 : fmt == GL_RGBA || fmt == GL_BGRA ? 4 : 1;
    size_t stride = ((size_t)w * (size_t)comps + (size_t)c->pack_align - 1) / (size_t)c->pack_align * (size_t)c->pack_align;
    for (int yy = 0; yy < h; yy++) for (int xx = 0; xx < w; xx++) {
        int sx = x + xx, sy = c->fb.h - 1 - (y + yy);
        uint32_t p = (sx >= 0 && sy >= 0 && sx < c->fb.w && sy < c->fb.h) ? c->fb.color[(size_t)sy * c->fb.w + sx] : 0;
        uint8_t r = (uint8_t)(p >> 16), g = (uint8_t)(p >> 8), b = (uint8_t)p, a = (uint8_t)(p >> 24);
        uint8_t *o = out + (size_t)yy * stride + (size_t)xx * (size_t)comps;
        switch (fmt) {
        case GL_RGB: o[0] = r; o[1] = g; o[2] = b; break;
        case GL_BGR: o[0] = b; o[1] = g; o[2] = r; break;
        case GL_BGRA: o[0] = b; o[1] = g; o[2] = r; o[3] = a; break;
        case GL_RGBA: o[0] = r; o[1] = g; o[2] = b; o[3] = a; break;
        default: o[0] = (uint8_t)((r + g + b) / 3); break;
        }
    }
}

// queries
static int get_values(GlCtx *c, int pname, double *v) {
    switch (pname) {
    case 0x0BA2: v[0] = c->sg.vp_x; v[1] = c->sg.vp_y; v[2] = c->sg.vp_w; v[3] = c->sg.vp_h; return 4; // GL_VIEWPORT
    case 0x0BA6: for (int i = 0; i < 16; i++) v[i] = c->mv[c->mv_top][i]; return 16;
    case 0x0BA7: for (int i = 0; i < 16; i++) v[i] = c->pj[c->pj_top][i]; return 16;
    case 0x0BA8: for (int i = 0; i < 16; i++) v[i] = c->tm[c->active_tex][c->tm_top[c->active_tex]][i]; return 16;
    case 0x0D33: v[0] = 4096; return 1;          // GL_MAX_TEXTURE_SIZE
    case 0x84E2: v[0] = SG_UNITS; return 1;      // GL_MAX_TEXTURE_UNITS
    case 0x0BA0: v[0] = c->mode; return 1;       // GL_MATRIX_MODE
    case 0x0D56: v[0] = 24; return 1;            // GL_DEPTH_BITS
    case 0x0D57: v[0] = 8; return 1;             // GL_STENCIL_BITS
    case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: v[0] = 8; return 1; // red/green/blue/alpha bits
    case 0x0C32: v[0] = c->double_buffer; return 1;
    case 0x0D31: v[0] = 8; return 1;             // GL_MAX_LIGHTS
    case 0x0D36: v[0] = 32; return 1;            // GL_MAX_MODELVIEW_STACK_DEPTH
    case 0x0D38: v[0] = 8; return 1;             // GL_MAX_PROJECTION_STACK_DEPTH
    case 0x8069: v[0] = c->bound[c->active_tex]; return 1;
    case 0x0CF5: v[0] = c->unpack_align; return 1;
    case 0x0D05: v[0] = c->pack_align; return 1;
    case 0x0B00: for (int i = 0; i < 4; i++) v[i] = c->color[i]; return 4;
    case 0x0C22: for (int i = 0; i < 4; i++) v[i] = c->clear_color[i]; return 4;
    case 0x0D3A: v[0] = 4096; v[1] = 4096; return 2;
    case 0x0C10: v[0] = c->sg.sc_x; v[1] = c->sg.sc_y; v[2] = c->sg.sc_w; v[3] = c->sg.sc_h; return 4;
    case 0x84E0: v[0] = GL_TEXTURE0 + c->active_tex; return 1;
    case 0x0B21: v[0] = c->sg.line_width; return 1;
    case 0x0B11: v[0] = c->sg.point_size; return 1;
    case 0x0BA3: v[0] = c->mv_top + 1; return 1;  // GL_MODELVIEW_STACK_DEPTH
    case 0x0BA4: v[0] = c->pj_top + 1; return 1;
    case 0x0B71: v[0] = c->sg.depth_test; return 1;
    case 0x0BE2: v[0] = c->sg.blend; return 1;
    case 0x0DE1: v[0] = c->tex2d[c->active_tex]; return 1;
    case 0x0B50: v[0] = c->lighting; return 1;
    case 0x80E8: v[0] = 65536; return 1;          // GL_MAX_ELEMENTS_VERTICES
    case 0x80E9: v[0] = 65536; return 1;
    case 0x0D32: v[0] = 6; return 1;              // GL_MAX_CLIP_PLANES
    case 0x0B32: v[0] = c->sg.depth_write; return 1; // not exact enum, harmless
    case 0x0B72: v[0] = c->sg.depth_write; return 1; // GL_DEPTH_WRITEMASK
    default: v[0] = 0; return 1;
    }
}
HLE(gl_GetIntegerv) { CTX(); double v[16]; int n = get_values(c, IA(0), v); int32_t *o = PA(1); for (int i = 0; i < n; i++) o[i] = (int32_t)v[i]; }
HLE(gl_GetFloatv) { CTX(); double v[16]; int n = get_values(c, IA(0), v); float *o = PA(1); for (int i = 0; i < n; i++) o[i] = (float)v[i]; }
HLE(gl_GetDoublev) { CTX(); double v[16]; int n = get_values(c, IA(0), v); double *o = PA(1); for (int i = 0; i < n; i++) o[i] = v[i]; }
HLE(gl_GetBooleanv) { CTX(); double v[16]; int n = get_values(c, IA(0), v); uint8_t *o = PA(1); for (int i = 0; i < n; i++) o[i] = v[i] != 0; }

// attribute stack (whole-state snapshot of the parts that matter)
HLE(gl_PushAttrib) {
    CTX();
    if (c->attr_top >= 16) { seterr(c, GL_STACK_OVERFLOW); return; }
    GlAttr *a = &c->attr_stack[c->attr_top++];
    a->sg = c->sg; a->lighting = c->lighting; a->color_mat = c->color_mat; a->fog_on = c->fog_on;
    memcpy(a->tex2d, c->tex2d, sizeof a->tex2d); a->normalize = c->normalize; a->polygon_mode = c->polygon_mode;
    a->shade_flat = c->shade_flat; memcpy(a->color, c->color, 16);
}
HLE(gl_PopAttrib) {
    CTX();
    if (!c->attr_top) { seterr(c, GL_STACK_UNDERFLOW); return; }
    GlAttr *a = &c->attr_stack[--c->attr_top];
    c->sg = a->sg; c->lighting = a->lighting; c->color_mat = a->color_mat; c->fog_on = a->fog_on;
    memcpy(c->tex2d, a->tex2d, sizeof a->tex2d); c->normalize = a->normalize; c->polygon_mode = a->polygon_mode;
    c->shade_flat = a->shade_flat; memcpy(c->color, a->color, 16);
}

// vertex arrays
static float arr_comp(const GlArr *a, int i, int k) {
    size_t esz = a->type == GL_DOUBLE ? 8 : (a->type == GL_FLOAT || a->type == GL_INT || a->type == GL_UNSIGNED_INT) ? 4 : (a->type == GL_SHORT || a->type == GL_UNSIGNED_SHORT) ? 2 : 1;
    size_t stride = a->stride ? (size_t)a->stride : esz * (size_t)a->size;
    const uint8_t *p = (const uint8_t *)G2H(a->ptr) + stride * (size_t)i + esz * (size_t)k;
    switch (a->type) {
    case GL_FLOAT: { float f; memcpy(&f, p, 4); return f; }
    case GL_DOUBLE: { double d; memcpy(&d, p, 8); return (float)d; }
    case GL_INT: { int32_t v; memcpy(&v, p, 4); return (float)v; }
    case GL_UNSIGNED_INT: { uint32_t v; memcpy(&v, p, 4); return (float)v; }
    case GL_SHORT: { int16_t v; memcpy(&v, p, 2); return (float)v; }
    case GL_UNSIGNED_SHORT: { uint16_t v; memcpy(&v, p, 2); return (float)v; }
    case GL_BYTE: return (float)(int8_t)*p;
    default: return (float)*p;
    }
}
static void fetch(GlCtx *c, int i, Vtx *v) {
    float p[4] = { 0, 0, 0, 1 };
    for (int k = 0; k < c->va.size && k < 4; k++) p[k] = arr_comp(&c->va, i, k);
    memcpy(v->p, p, 16);
    if (c->ca.on) {
        float cc[4] = { 0, 0, 0, 1 };
        float sc = c->ca.type == GL_UNSIGNED_BYTE ? 1 / 255.0f : c->ca.type == GL_BYTE ? 1 / 127.0f : c->ca.type == GL_UNSIGNED_SHORT ? 1 / 65535.0f : 1;
        for (int k = 0; k < c->ca.size && k < 4; k++) cc[k] = arr_comp(&c->ca, i, k) * sc;
        memcpy(v->c, cc, 16);
        memcpy(c->color, cc, 16);
    } else memcpy(v->c, c->color, 16);
    if (c->na.on) { for (int k = 0; k < 3; k++) v->n[k] = arr_comp(&c->na, i, k); }
    else memcpy(v->n, c->normal, 12);
    for (int u = 0; u < SG_UNITS; u++) {
        if (c->ta[u].on) { float t[4] = { 0, 0, 0, 1 }; for (int k = 0; k < c->ta[u].size && k < 4; k++) t[k] = arr_comp(&c->ta[u], i, k); memcpy(v->t[u], t, 16); }
        else memcpy(v->t[u], c->tc[u], 16);
    }
}
static void draw_indexed(GlCtx *c, int mode, int n, const uint32_t *idx, int first) {
    if (!c->va.on || n <= 0) return;
    Vtx *v = malloc(sizeof(Vtx) * (size_t)n);
    for (int i = 0; i < n; i++) fetch(c, idx ? (int)idx[i] : first + i, &v[i]);
    if (c->list_id) { // compile: record as immediate mode
        opi(c, OP_BEGIN, mode, 0);
        for (int i = 0; i < n; i++) {
            opf(c, OP_COLOR, 0, 0, v[i].c, 4);
            opf(c, OP_NORMAL, 0, 0, v[i].n, 3);
            for (int u = 0; u < SG_UNITS; u++) opf(c, OP_TEXCOORD, u, 0, v[i].t[u], 4);
            opf(c, OP_VERTEX, 0, 0, v[i].p, 4);
        }
        op0(c, OP_END);
    } else draw_prims(c, mode, v, n);
    free(v);
}
static void set_arr(GlArr *a, int size, int type, int stride, uint64_t ptr) { a->size = size; a->type = type; a->stride = stride; a->ptr = ptr; }
HLE(gl_VertexPointer) { CTX(); set_arr(&c->va, IA(0), IA(1), IA(2), ARG(3)); }
HLE(gl_ColorPointer) { CTX(); set_arr(&c->ca, IA(0), IA(1), IA(2), ARG(3)); }
HLE(gl_NormalPointer) { CTX(); set_arr(&c->na, 3, IA(0), IA(1), ARG(2)); }
HLE(gl_TexCoordPointer) { CTX(); set_arr(&c->ta[c->client_tex], IA(0), IA(1), IA(2), ARG(3)); }
HLE(gl_DrawArrays) { CTX(); draw_indexed(c, IA(0), IA(2), NULL, IA(1)); }
static void draw_elements(GlCtx *c, int mode, int n, int type, uint64_t ptr) {
    if (n <= 0 || !ptr) return;
    uint32_t *idx = malloc(sizeof(uint32_t) * (size_t)n);
    for (int i = 0; i < n; i++) {
        if (type == GL_UNSIGNED_BYTE) idx[i] = ((const uint8_t *)G2H(ptr))[i];
        else if (type == GL_UNSIGNED_SHORT) idx[i] = ((const uint16_t *)G2H(ptr))[i];
        else idx[i] = ((const uint32_t *)G2H(ptr))[i];
    }
    draw_indexed(c, mode, n, idx, 0);
    free(idx);
}
HLE(gl_DrawElements) { CTX(); draw_elements(c, IA(0), IA(1), IA(2), ARG(3)); }
HLE(gl_DrawRangeElements) { CTX(); draw_elements(c, IA(0), (int)(int32_t)hle_arg(st, 3), (int)(int32_t)hle_arg(st, 4), hle_arg(st, 5)); }
HLE(gl_ArrayElement) {
    CTX();
    if (!c->va.on) return;
    Vtx v;
    fetch(c, IA(0), &v);
    opf(c, OP_COLOR, 0, 0, v.c, 4);
    opf(c, OP_NORMAL, 0, 0, v.n, 3);
    for (int u = 0; u < SG_UNITS; u++) opf(c, OP_TEXCOORD, u, 0, v.t[u], 4);
    opf(c, OP_VERTEX, 0, 0, v.p, 4);
}
HLE(gl_InterleavedArrays) {
    // GL_V3F 0x2A21, GL_C4UB_V3F 0x2A23, GL_C3F_V3F 0x2A24, GL_N3F_V3F 0x2A25, GL_T2F_V3F 0x2A27, GL_T2F_C4UB_V3F 0x2A29, GL_T2F_N3F_V3F 0x2A2B, GL_T2F_C3F_V3F 0x2A2A, GL_T2F_C4F_N3F_V3F 0x2A2C
    CTX();
    int fmt = IA(0), stride = IA(1);
    uint64_t p = ARG(2);
    c->ca.on = c->na.on = c->ta[c->client_tex].on = 0;
    c->va.on = 1;
    int tsz = 0, csz = 0, nsz = 0, ctype = GL_FLOAT;
    switch (fmt) {
    case 0x2A23: csz = 4; ctype = GL_UNSIGNED_BYTE; break;
    case 0x2A24: csz = 3; break;
    case 0x2A25: nsz = 3; break;
    case 0x2A27: tsz = 2; break;
    case 0x2A29: tsz = 2; csz = 4; ctype = GL_UNSIGNED_BYTE; break;
    case 0x2A2A: tsz = 2; csz = 3; break;
    case 0x2A2B: tsz = 2; nsz = 3; break;
    case 0x2A2C: tsz = 2; csz = 4; nsz = 3; break;
    default: break;
    }
    int cbytes = ctype == GL_UNSIGNED_BYTE ? 4 : csz * 4;
    int total = tsz * 4 + (csz ? cbytes : 0) + nsz * 4 + 12;
    if (!stride) stride = total;
    uint64_t off = p;
    if (tsz) { set_arr(&c->ta[c->client_tex], tsz, GL_FLOAT, stride, off); c->ta[c->client_tex].on = 1; off += (uint64_t)tsz * 4; }
    if (csz) { set_arr(&c->ca, csz, ctype, stride, off); c->ca.on = 1; off += (uint64_t)cbytes; }
    if (nsz) { set_arr(&c->na, 3, GL_FLOAT, stride, off); c->na.on = 1; off += 12; }
    set_arr(&c->va, 3, GL_FLOAT, stride, off);
}

// display lists
HLE(gl_GenLists) {
    int n = IA(0);
    if (n <= 0) { RET(0); return; }
    for (int base = 1; base + n <= MAX_LISTS; base++) {
        int ok = 1;
        for (int i = 0; i < n && ok; i++) if (g_lists[base + i].used) ok = 0;
        if (!ok) continue;
        for (int i = 0; i < n; i++) { g_lists[base + i].used = 1; g_lists[base + i].n = 0; }
        RET(base);
        return;
    }
    RET(0);
}
HLE(gl_NewList) {
    CTX();
    uint32_t id = UA(0);
    if (!id || id >= MAX_LISTS) { seterr(c, GL_INVALID_VALUE); return; }
    g_lists[id].used = 1;
    g_lists[id].n = 0;
    c->list_id = id;
    c->list_mode = IA(1);
}
HLE(gl_EndList) { CTX(); c->list_id = 0; }
HLE(gl_CallList) { CTX(); opi(c, OP_CALL_LIST, IA(0), 0); }
HLE(gl_ListBase) { CTX(); c->list_base = IA(0); }
HLE(gl_CallLists) {
    CTX();
    int n = IA(0), type = IA(1);
    const uint8_t *p = PA(2);
    for (int i = 0; i < n; i++) {
        int id;
        if (type == GL_UNSIGNED_BYTE) id = p[i];
        else if (type == GL_BYTE) id = (int8_t)p[i];
        else if (type == GL_UNSIGNED_SHORT) id = ((const uint16_t *)p)[i];
        else if (type == GL_SHORT) id = ((const int16_t *)p)[i];
        else id = ((const int32_t *)p)[i];
        opi(c, OP_CALL_LIST, c->list_base + id, 0);
    }
}
HLE(gl_DeleteLists) {
    uint32_t id = UA(0);
    int n = IA(1);
    for (int i = 0; i < n; i++) if (id + (uint32_t)i < MAX_LISTS) { free(g_lists[id + i].cmds); memset(&g_lists[id + i], 0, sizeof g_lists[0]); }
    RET(0);
}
HLE(gl_IsList) { RET(UA(0) && UA(0) < MAX_LISTS && g_lists[UA(0)].used); }

// raster position and pixel drawing
static void raster_pos(GlCtx *c, float x, float y, float z) { float f[4] = { x, y, z, 1 }; opf(c, OP_RASTER_POS, 0, 0, f, 4); }
HLE(gl_RasterPos2f) { CTX(); raster_pos(c, fa(st, 0), fa(st, 1), 0); }
HLE(gl_RasterPos2i) { CTX(); raster_pos(c, (float)IA(0), (float)IA(1), 0); }
HLE(gl_RasterPos2d) { CTX(); raster_pos(c, (float)da(st, 0), (float)da(st, 1), 0); }
HLE(gl_RasterPos3f) { CTX(); raster_pos(c, fa(st, 0), fa(st, 1), fa(st, 2)); }
HLE(gl_WindowPos2i) { CTX(); c->raster[0] = (float)IA(0); c->raster[1] = (float)IA(1); c->raster[2] = 0; c->raster_valid = 1; }
HLE(gl_DrawPixels) {
    // (w, h, format, type, pixels) at the raster position (bottom-left origin)
    CTX();
    int w = IA(0), h = IA(1);
    if (!c->raster_valid || w <= 0 || h <= 0 || !c->fb.color) return;
    uint8_t *px = unpack(c, w, h, IA(2), IA(3), PA(4));
    int x0 = (int)c->raster[0], y0 = (int)c->raster[1];
    for (int y = 0; y < h; y++) {
        int sy = c->fb.h - 1 - (y0 + y);
        if (sy < 0 || sy >= c->fb.h) continue;
        for (int x = 0; x < w; x++) {
            int sx = x0 + x;
            if (sx < 0 || sx >= c->fb.w) continue;
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            uint32_t d = c->fb.color[(size_t)sy * c->fb.w + sx];
            if (c->sg.blend && c->sg.blend_src == SG_SRC_ALPHA) {
                uint32_t a = p[3];
                uint32_t r = (p[0] * a + ((d >> 16) & 255) * (255 - a)) / 255, g = (p[1] * a + ((d >> 8) & 255) * (255 - a)) / 255, b = (p[2] * a + (d & 255) * (255 - a)) / 255;
                c->fb.color[(size_t)sy * c->fb.w + sx] = 0xFF000000u | (r << 16) | (g << 8) | b;
            } else c->fb.color[(size_t)sy * c->fb.w + sx] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    }
    free(px);
}
HLE(gl_Bitmap) {
    // (w, h, xorig, yorig, xmove, ymove, bitmap)
    CTX();
    int w = IA(0), h = IA(1);
    float xo = fa(st, 2), yo = fa(st, 3), xm = fa(st, 4), ym = fa(st, 5);
    const uint8_t *bits = PA(6);
    if (c->raster_valid && bits && c->fb.color) {
        uint32_t argb = 0xFF000000u | ((uint32_t)c8(c->color[0]) << 16) | ((uint32_t)c8(c->color[1]) << 8) | c8(c->color[2]);
        int stride = (w + 7) / 8;
        stride = (stride + c->unpack_align - 1) / c->unpack_align * c->unpack_align;
        int x0 = (int)(c->raster[0] - xo), y0 = (int)(c->raster[1] - yo);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            if (!(bits[y * stride + x / 8] & (0x80 >> (x & 7)))) continue;
            int sx = x0 + x, sy = c->fb.h - 1 - (y0 + y);
            if (sx >= 0 && sy >= 0 && sx < c->fb.w && sy < c->fb.h) c->fb.color[(size_t)sy * c->fb.w + sx] = argb;
        }
    }
    c->raster[0] += xm; c->raster[1] += ym;
}

// ---- GLU -----------------------------------------------------------------------------------------
HLE(glu_Perspective) {
    CTX();
    double fovy = da(st, 0), aspect = da(st, 1), zn = da(st, 2), zf = da(st, 3);
    double ymax = zn * tan(fovy * M_PI / 360.0), xmax = ymax * aspect;
    frustum(c, -xmax, xmax, -ymax, ymax, zn, zf);
}
HLE(glu_Ortho2D) { CTX(); ortho(c, da(st, 0), da(st, 1), da(st, 2), da(st, 3), -1, 1); }
HLE(glu_LookAt) {
    CTX();
    float e[3] = { (float)da(st, 0), (float)da(st, 1), (float)da(st, 2) };
    float ce[3] = { (float)da(st, 3), (float)da(st, 4), (float)da(st, 5) };
    float up[3] = { (float)da(st, 6), (float)da(st, 7), (float)da(st, 8) };
    float f[3] = { ce[0] - e[0], ce[1] - e[1], ce[2] - e[2] };
    v3norm(f);
    float s[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
    v3norm(s);
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    float m[16];
    m_ident(m);
    m[0] = s[0]; m[4] = s[1]; m[8] = s[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = -f[0]; m[6] = -f[1]; m[10] = -f[2];
    opf(c, OP_MULT_MATRIX, 0, 0, m, 16);
    translate(c, -e[0], -e[1], -e[2]);
}
HLE(glu_Build2DMipmaps) {
    // (target, internalformat, w, h, format, type, data)
    CTX();
    tex_image(c, 0, IA(1), IA(2), IA(3), (int)(int32_t)hle_arg(st, 4), (int)(int32_t)hle_arg(st, 5), PA(6), 1);
    GlTex *t = bound_tex(c);
    if (t) { t->tex.use_mips = 1; t->tex.min_linear = 1; t->dirty_mips = 1; }
    RET(0);
}
HLE(glu_ErrorString) {
    static const char *names[] = { "no error", "invalid enum", "invalid value", "invalid operation", "stack overflow", "stack underflow", "out of memory" };
    uint32_t e = UA(0);
    static const char *unknown = "unknown error";
    RET(gl_str(e == 0 ? names[0] : (e >= 0x500 && e <= 0x505) ? names[e - 0x500 + 1] : unknown));
}
typedef struct Quadric { int normals, texture, style; } Quadric;
HLE(glu_NewQuadric) { Quadric *q = g_calloc(1, sizeof *q); q->normals = 100000; RET(q); } // GLU_SMOOTH (the program holds the pointer)
HLE(glu_DeleteQuadric) { g_free(PA(0)); RET(0); }
HLE(glu_QuadricNormals) { Quadric *q = PA(0); if (q) q->normals = IA(1); RET(0); }
HLE(glu_QuadricTexture) { Quadric *q = PA(0); if (q) q->texture = IA(1) != 0; RET(0); }
HLE(glu_QuadricDrawStyle) { Quadric *q = PA(0); if (q) q->style = IA(1); RET(0); }
static void qv(GlCtx *c, const Quadric *q, float x, float y, float z, float nx, float ny, float nz, float s, float t) {
    if (q->texture) tcoord(c, 0, s, t, 0, 1);
    nrm(c, nx, ny, nz);
    vtx(c, x, y, z, 1);
}
HLE(glu_Sphere) {
    CTX();
    const Quadric *q = PA(0);
    float r = (float)DAS(1, 1);
    int slices = IAS(2, 3), stacks = IAS(3, 4);
    if (!q || slices < 3 || stacks < 2) return;
    int wire = q->style == 100011; // GLU_LINE
    for (int i = 0; i < stacks; i++) {
        float t0 = (float)M_PI * i / stacks, t1 = (float)M_PI * (i + 1) / stacks;
        opi(c, OP_BEGIN, wire ? GL_LINE_STRIP : GL_QUAD_STRIP, 0);
        for (int j = 0; j <= slices; j++) {
            float p = 2 * (float)M_PI * j / slices;
            float x0 = -sinf(t0) * sinf(p), y0 = sinf(t0) * cosf(p), z0 = cosf(t0); // same orientation as GLU
            float x1 = -sinf(t1) * sinf(p), y1 = sinf(t1) * cosf(p), z1 = cosf(t1);
            qv(c, q, x0 * r, y0 * r, z0 * r, x0, y0, z0, (float)j / slices, 1 - (float)i / stacks);
            qv(c, q, x1 * r, y1 * r, z1 * r, x1, y1, z1, (float)j / slices, 1 - (float)(i + 1) / stacks);
        }
        op0(c, OP_END);
    }
}
HLE(glu_Cylinder) {
    CTX();
    const Quadric *q = PA(0);
    float rb = (float)DAS(1, 1), rt = (float)DAS(2, 3), h = (float)DAS(3, 5);
    int slices = IAS(4, 7), stacks = IAS(5, 8);
    if (!q || slices < 3 || stacks < 1) return;
    float nz = (rb - rt) / h;
    for (int i = 0; i < stacks; i++) {
        float z0 = h * i / stacks, z1 = h * (i + 1) / stacks;
        float r0 = rb + (rt - rb) * i / stacks, r1 = rb + (rt - rb) * (i + 1) / stacks;
        opi(c, OP_BEGIN, GL_QUAD_STRIP, 0);
        for (int j = 0; j <= slices; j++) {
            float p = 2 * (float)M_PI * j / slices, sx = sinf(p), cy = cosf(p);
            qv(c, q, sx * r0, cy * r0, z0, sx, cy, nz, (float)j / slices, (float)i / stacks);
            qv(c, q, sx * r1, cy * r1, z1, sx, cy, nz, (float)j / slices, (float)(i + 1) / stacks);
        }
        op0(c, OP_END);
    }
}
HLE(glu_Disk) {
    CTX();
    const Quadric *q = PA(0);
    float ri = (float)DAS(1, 1), ro = (float)DAS(2, 3);
    int slices = IAS(3, 5), loops = IAS(4, 6);
    if (!q || slices < 3 || loops < 1) return;
    for (int l = 0; l < loops; l++) {
        float r0 = ri + (ro - ri) * l / loops, r1 = ri + (ro - ri) * (l + 1) / loops;
        opi(c, OP_BEGIN, GL_QUAD_STRIP, 0);
        for (int j = 0; j <= slices; j++) {
            float p = 2 * (float)M_PI * j / slices, sx = sinf(p), cy = cosf(p);
            qv(c, q, sx * r1, cy * r1, 0, 0, 0, 1, 0.5f + sx * r1 / (2 * ro), 0.5f + cy * r1 / (2 * ro));
            qv(c, q, sx * r0, cy * r0, 0, 0, 0, 1, 0.5f + sx * r0 / (2 * ro), 0.5f + cy * r0 / (2 * ro));
        }
        op0(c, OP_END);
    }
}
HLE(glu_Project) {
    // (objx, objy, objz, model[16] double*, proj[16] double*, view[4] int*, winx*, winy*, winz*)
    double o[4] = { da(st, 0), da(st, 1), da(st, 2), 1 };
    const double *mm = PAS(3, 6), *pm = PAS(4, 7);
    const int32_t *vp = PAS(5, 8);
    double e[4], cl[4];
    for (int i = 0; i < 4; i++) e[i] = mm[i] * o[0] + mm[4 + i] * o[1] + mm[8 + i] * o[2] + mm[12 + i] * o[3];
    for (int i = 0; i < 4; i++) cl[i] = pm[i] * e[0] + pm[4 + i] * e[1] + pm[8 + i] * e[2] + pm[12 + i] * e[3];
    if (cl[3] == 0) { RET(0); return; }
    for (int i = 0; i < 3; i++) cl[i] /= cl[3];
    double wx = vp[0] + (cl[0] + 1) * vp[2] / 2, wy = vp[1] + (cl[1] + 1) * vp[3] / 2, wz = (cl[2] + 1) / 2;
    memcpy(PAS(6, 9), &wx, 8); memcpy(PAS(7, 10), &wy, 8); memcpy(PAS(8, 11), &wz, 8);
    RET(1);
}

// ---- lifecycle -----------------------------------------------------------------------------------
void gl_reset(void) {
    for (int i = 0; i < 16; i++) { free(g_ctx[i].fb.color); free(g_ctx[i].fb.depth); free(g_ctx[i].imm); }
    memset(g_ctx, 0, sizeof g_ctx);
    for (int i = 0; i < MAX_TEX; i++) if (g_tex[i].used) sg_texture_free(&g_tex[i].tex);
    memset(g_tex, 0, sizeof g_tex);
    for (int i = 0; i < MAX_LISTS; i++) free(g_lists[i].cmds);
    memset(g_lists, 0, sizeof g_lists);
    memset(g_pfd, 0, sizeof g_pfd);
}

#define G(n, f) { n, f, NULL }
const HleEntry hle_opengl_table[] = {
    G("wglCreateContext", w_wglCreateContext), G("wglDeleteContext", w_wglDeleteContext), G("wglMakeCurrent", w_wglMakeCurrent),
    G("wglGetCurrentContext", w_wglGetCurrentContext), G("wglGetCurrentDC", w_wglGetCurrentDC), G("wglShareLists", w_wglShareLists),
    G("wglGetProcAddress", w_wglGetProcAddress), G("wglSwapIntervalEXT", w_wglSwapIntervalEXT), G("wglGetSwapIntervalEXT", w_wglGetSwapIntervalEXT),
    G("wglGetExtensionsStringARB", w_wglGetExtensionsString), G("wglGetExtensionsStringEXT", w_wglGetExtensionsString),
    G("wglUseFontBitmapsA", w_wglUseFontBitmaps), G("wglUseFontBitmapsW", w_wglUseFontBitmaps), G("wglCreateContextAttribsARB", w_wglCreateContextAttribsARB),
    G("wglSwapBuffers", w_SwapBuffers), G("ChoosePixelFormat", w_ChoosePixelFormat), G("SetPixelFormat", w_SetPixelFormat),
    G("DescribePixelFormat", w_DescribePixelFormat), G("GetPixelFormat", w_GetPixelFormat), G("SwapBuffers", w_SwapBuffers),
    G("wglChoosePixelFormat", w_ChoosePixelFormat), G("wglSetPixelFormat", w_SetPixelFormat), G("wglDescribePixelFormat", w_DescribePixelFormat),
    G("glBegin", gl_Begin), G("glEnd", gl_End),
    G("glVertex2f", gl_Vertex2f), G("glVertex3f", gl_Vertex3f), G("glVertex4f", gl_Vertex4f), G("glVertex2d", gl_Vertex2d), G("glVertex3d", gl_Vertex3d),
    G("glVertex4d", gl_Vertex4d), G("glVertex2i", gl_Vertex2i), G("glVertex3i", gl_Vertex3i), G("glVertex2s", gl_Vertex2s), G("glVertex3s", gl_Vertex3s),
    G("glVertex2fv", gl_Vertex2fv), G("glVertex3fv", gl_Vertex3fv), G("glVertex4fv", gl_Vertex4fv), G("glVertex2dv", gl_Vertex2dv),
    G("glVertex3dv", gl_Vertex3dv), G("glVertex2iv", gl_Vertex2iv), G("glVertex3iv", gl_Vertex3iv),
    G("glColor3f", gl_Color3f), G("glColor4f", gl_Color4f), G("glColor3d", gl_Color3d), G("glColor4d", gl_Color4d), G("glColor3ub", gl_Color3ub),
    G("glColor4ub", gl_Color4ub), G("glColor3i", gl_Color3i), G("glColor3fv", gl_Color3fv), G("glColor4fv", gl_Color4fv), G("glColor3dv", gl_Color3dv),
    G("glColor4dv", gl_Color4dv), G("glColor3ubv", gl_Color3ubv), G("glColor4ubv", gl_Color4ubv),
    G("glNormal3f", gl_Normal3f), G("glNormal3d", gl_Normal3d), G("glNormal3fv", gl_Normal3fv), G("glNormal3dv", gl_Normal3dv), G("glNormal3i", gl_Normal3i),
    G("glTexCoord1f", gl_TexCoord1f), G("glTexCoord2f", gl_TexCoord2f), G("glTexCoord3f", gl_TexCoord3f), G("glTexCoord4f", gl_TexCoord4f),
    G("glTexCoord2d", gl_TexCoord2d), G("glTexCoord2i", gl_TexCoord2i), G("glTexCoord2fv", gl_TexCoord2fv), G("glTexCoord2dv", gl_TexCoord2dv),
    G("glMultiTexCoord2f", gl_MultiTexCoord2f), G("glMultiTexCoord2fARB", gl_MultiTexCoord2f), G("glMultiTexCoord2fv", gl_MultiTexCoord2fv),
    G("glMultiTexCoord2fvARB", gl_MultiTexCoord2fv), G("glActiveTexture", gl_ActiveTexture), G("glActiveTextureARB", gl_ActiveTexture),
    G("glClientActiveTexture", gl_ClientActiveTexture), G("glClientActiveTextureARB", gl_ClientActiveTexture),
    G("glRectf", gl_Rectf), G("glRectd", gl_Rectd), G("glRecti", gl_Recti),
    G("glMatrixMode", gl_MatrixMode), G("glLoadIdentity", gl_LoadIdentity), G("glLoadMatrixf", gl_LoadMatrixf), G("glLoadMatrixd", gl_LoadMatrixd),
    G("glMultMatrixf", gl_MultMatrixf), G("glMultMatrixd", gl_MultMatrixd), G("glLoadTransposeMatrixf", gl_LoadTransposeMatrixf),
    G("glMultTransposeMatrixf", gl_MultTransposeMatrixf), G("glPushMatrix", gl_PushMatrix), G("glPopMatrix", gl_PopMatrix),
    G("glTranslatef", gl_Translatef), G("glTranslated", gl_Translated), G("glScalef", gl_Scalef), G("glScaled", gl_Scaled),
    G("glRotatef", gl_Rotatef), G("glRotated", gl_Rotated), G("glOrtho", gl_Ortho), G("glFrustum", gl_Frustum),
    G("glEnable", gl_Enable), G("glDisable", gl_Disable), G("glIsEnabled", gl_IsEnabled), G("glEnableClientState", gl_EnableClientState),
    G("glDisableClientState", gl_DisableClientState), G("glBlendFunc", gl_BlendFunc), G("glDepthFunc", gl_DepthFunc), G("glDepthMask", gl_DepthMask),
    G("glDepthRange", gl_DepthRange), G("glAlphaFunc", gl_AlphaFunc), G("glShadeModel", gl_ShadeModel), G("glCullFace", gl_CullFace),
    G("glFrontFace", gl_FrontFace), G("glPolygonMode", gl_PolygonMode), G("glPolygonOffset", gl_PolygonOffset), G("glLineWidth", gl_LineWidth),
    G("glPointSize", gl_PointSize), G("glColorMask", gl_ColorMask), G("glScissor", gl_Scissor), G("glViewport", gl_Viewport),
    G("glClearColor", gl_ClearColor), G("glClearDepth", gl_ClearDepth), G("glClearDepthf", gl_ClearDepthf), G("glClear", gl_Clear),
    G("glFlush", gl_Flush), G("glFinish", gl_Finish), G("glGetError", gl_GetError), G("glGetString", gl_GetString),
    G("glHint", gl_Noop), G("glClearStencil", gl_Noop), G("glStencilFunc", gl_Noop), G("glStencilOp", gl_Noop), G("glStencilMask", gl_Noop),
    G("glDrawBuffer", gl_Noop), G("glReadBuffer", gl_Noop), G("glTexGeni", gl_Noop), G("glTexGenf", gl_Noop), G("glTexGenfv", gl_Noop),
    G("glTexGeniv", gl_Noop), G("glClipPlane", gl_Noop), G("glLockArraysEXT", gl_Noop), G("glUnlockArraysEXT", gl_Noop),
    G("glPrioritizeTextures", gl_Noop), G("glLineStipple", gl_Noop), G("glPolygonStipple", gl_Noop), G("glTexImage1D", gl_Noop),
    G("glLogicOp", gl_Noop), G("glAccum", gl_Noop), G("glClearAccum", gl_Noop), G("glLightModeli", gl_Noop), G("glPixelZoom", gl_Noop),
    G("glPixelTransferf", gl_Noop), G("glPixelTransferi", gl_Noop), G("glBlendEquation", gl_Noop), G("glBlendColor", gl_Noop),
    G("glLightf", gl_Lightf), G("glLighti", gl_Lighti), G("glLightfv", gl_Lightfv), G("glLightiv", gl_Lightiv), G("glLightModelf", gl_LightModelf),
    G("glLightModelfv", gl_LightModelfv), G("glMaterialf", gl_Materialf), G("glMaterialfv", gl_Materialfv), G("glMateriali", gl_Materiali),
    G("glMaterialiv", gl_Materialiv), G("glColorMaterial", gl_ColorMaterial), G("glFogf", gl_Fogf), G("glFogi", gl_Fogi), G("glFogfv", gl_Fogfv),
    G("glFogiv", gl_Fogiv), G("glTexEnvf", gl_TexEnvf), G("glTexEnvi", gl_TexEnvi), G("glTexEnvfv", gl_TexEnvfv), G("glTexEnviv", gl_TexEnviv),
    G("glTexParameteri", gl_TexParameteri), G("glTexParameterf", gl_TexParameterf), G("glTexParameteriv", gl_TexParameteriv),
    G("glTexParameterfv", gl_TexParameterfv), G("glGenTextures", gl_GenTextures), G("glGenTexturesEXT", gl_GenTextures),
    G("glDeleteTextures", gl_DeleteTextures), G("glBindTexture", gl_BindTexture), G("glBindTextureEXT", gl_BindTexture), G("glIsTexture", gl_IsTexture),
    G("glAreTexturesResident", gl_AreTexturesResident), G("glTexImage2D", gl_TexImage2D), G("glTexSubImage2D", gl_TexSubImage2D),
    G("glCopyTexImage2D", gl_CopyTexImage2D), G("glGetTexLevelParameteriv", gl_GetTexLevelParameteriv), G("glPixelStorei", gl_PixelStorei),
    G("glPixelStoref", gl_PixelStoref), G("glReadPixels", gl_ReadPixels), G("glGetIntegerv", gl_GetIntegerv), G("glGetFloatv", gl_GetFloatv),
    G("glGetDoublev", gl_GetDoublev), G("glGetBooleanv", gl_GetBooleanv), G("glPushAttrib", gl_PushAttrib), G("glPopAttrib", gl_PopAttrib),
    G("glPushClientAttrib", gl_Noop), G("glPopClientAttrib", gl_Noop),
    G("glVertexPointer", gl_VertexPointer), G("glColorPointer", gl_ColorPointer), G("glNormalPointer", gl_NormalPointer),
    G("glTexCoordPointer", gl_TexCoordPointer), G("glDrawArrays", gl_DrawArrays), G("glDrawElements", gl_DrawElements),
    G("glDrawRangeElements", gl_DrawRangeElements), G("glDrawRangeElementsEXT", gl_DrawRangeElements), G("glArrayElement", gl_ArrayElement),
    G("glInterleavedArrays", gl_InterleavedArrays),
    G("glGenLists", gl_GenLists), G("glNewList", gl_NewList), G("glEndList", gl_EndList), G("glCallList", gl_CallList), G("glListBase", gl_ListBase),
    G("glCallLists", gl_CallLists), G("glDeleteLists", gl_DeleteLists), G("glIsList", gl_IsList),
    G("glRasterPos2f", gl_RasterPos2f), G("glRasterPos2i", gl_RasterPos2i), G("glRasterPos2d", gl_RasterPos2d), G("glRasterPos3f", gl_RasterPos3f),
    G("glWindowPos2i", gl_WindowPos2i), G("glDrawPixels", gl_DrawPixels), G("glBitmap", gl_Bitmap),
    G("gluPerspective", glu_Perspective), G("gluOrtho2D", glu_Ortho2D), G("gluLookAt", glu_LookAt), G("gluBuild2DMipmaps", glu_Build2DMipmaps),
    G("gluErrorString", glu_ErrorString), G("gluNewQuadric", glu_NewQuadric), G("gluDeleteQuadric", glu_DeleteQuadric),
    G("gluQuadricNormals", glu_QuadricNormals), G("gluQuadricTexture", glu_QuadricTexture), G("gluQuadricDrawStyle", glu_QuadricDrawStyle),
    G("gluSphere", glu_Sphere), G("gluCylinder", glu_Cylinder), G("gluDisk", glu_Disk), G("gluProject", glu_Project),
    { NULL, NULL, NULL }
};
