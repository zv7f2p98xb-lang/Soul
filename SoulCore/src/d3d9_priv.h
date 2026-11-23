// SoulCore - Direct3D 9 (d3d9.dll) internal declarations shared by d3d9*.c.
//
// COM objects are blocks of guest memory whose first pointer-sized field is the vtable (a table of HLE
// thunks). The block IS the host structure (D9Obj is its first member); pointers inside are host
// pointers, so only what the program reads (locked memory, the objects themselves) lives in guest memory.
#pragma once
#include "hle.h"
#include "gui.h"
#include "sgpu.h"
#include "d3d9_shader.h"
#include <math.h>

// ---- HRESULTs ------------------------------------------------------------------------------------------
#define D3D_OK 0u
#define S_FALSE_ 1u
#define E_FAIL_ 0x80004005u
#define E_NOINTERFACE_ 0x80004002u
#define E_POINTER_ 0x80004003u
#define E_OUTOFMEMORY_ 0x8007000Eu
#define E_INVALIDARG_ 0x80070057u
#define D3DERR_WRONGTEXTUREFORMAT 0x88760818u
#define D3DERR_UNSUPPORTEDCOLOROPERATION 0x88760819u
#define D3DERR_NOTFOUND 0x88760866u
#define D3DERR_MOREDATA 0x88760867u
#define D3DERR_DEVICELOST 0x88760868u
#define D3DERR_DEVICENOTRESET 0x88760869u
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#define D3DERR_INVALIDDEVICE 0x8876086Bu
#define D3DERR_WASSTILLDRAWING 0x8876021Cu

// ---- formats -----------------------------------------------------------------------------------------------
enum {
    F_UNKNOWN = 0, F_R8G8B8 = 20, F_A8R8G8B8 = 21, F_X8R8G8B8 = 22, F_R5G6B5 = 23, F_X1R5G5B5 = 24, F_A1R5G5B5 = 25,
    F_A4R4G4B4 = 26, F_R3G3B2 = 27, F_A8 = 28, F_A8R3G3B2 = 29, F_X4R4G4B4 = 30, F_A2B10G10R10 = 31, F_A8B8G8R8 = 32,
    F_X8B8G8R8 = 33, F_G16R16 = 34, F_A2R10G10B10 = 35, F_A16B16G16R16 = 36, F_A8P8 = 40, F_P8 = 41, F_L8 = 50,
    F_A8L8 = 51, F_A4L4 = 52, F_V8U8 = 60, F_L6V5U5 = 61, F_X8L8V8U8 = 62, F_Q8W8V8U8 = 63, F_V16U16 = 64,
    F_A2W10V10U10 = 67, F_D16_LOCKABLE = 70, F_D32 = 71, F_D15S1 = 73, F_D24S8 = 75, F_D24X8 = 77, F_D24X4S4 = 79,
    F_D16 = 80, F_L16 = 81, F_D32F_LOCKABLE = 82, F_D24FS8 = 83, F_D32_LOCKABLE = 84, F_S8_LOCKABLE = 85,
    F_VERTEXDATA = 100, F_INDEX16 = 101, F_INDEX32 = 102, F_Q16W16V16U16 = 110, F_R16F = 111, F_G16R16F = 112,
    F_A16B16G16R16F = 113, F_R32F = 114, F_G32R32F = 115, F_A32B32G32R32F = 116,
    F_DXT1 = 0x31545844, F_DXT2 = 0x32545844, F_DXT3 = 0x33545844, F_DXT4 = 0x34545844, F_DXT5 = 0x35545844,
};
// usage / pool / lock flags
#define USAGE_RENDERTARGET 0x1u
#define USAGE_DEPTHSTENCIL 0x2u
#define USAGE_DYNAMIC 0x200u
#define USAGE_AUTOGENMIPMAP 0x400u
#define POOL_DEFAULT 0
#define POOL_MANAGED 1
#define POOL_SYSTEMMEM 2
#define POOL_SCRATCH 3
#define LOCK_READONLY 0x10u
#define LOCK_DISCARD 0x2000u
#define LOCK_NOOVERWRITE 0x1000u
// D3DRESOURCETYPE
enum { RT_SURFACE = 1, RT_VOLUME, RT_TEXTURE, RT_VOLUMETEXTURE, RT_CUBETEXTURE, RT_VERTEXBUFFER, RT_INDEXBUFFER };

int d9_fmt_bytes(int fmt);               // bytes per pixel (0 = block compressed / unknown)
int d9_fmt_is_dxt(int fmt);
int d9_fmt_is_depth(int fmt);
int d9_fmt_known(int fmt);               // a colour format Soul can store
int d9_fmt_texture_ok(int fmt);
int d9_fmt_rt_ok(int fmt);
int d9_fmt_is_argb32(int fmt);           // A8R8G8B8 / X8R8G8B8 (the render storage format)
int d9_fmt_is_bump(int fmt);
size_t d9_level_pitch(int fmt, int w);
size_t d9_level_rows(int fmt, int h);
void d9_unpack_row(int fmt, const uint8_t *src, uint32_t *dst, int n, const uint32_t *pal);
int d9_pack_row(int fmt, const uint32_t *src, uint8_t *dst, int n);   // 0 = format cannot be written
void d9_decode_dxt(int fmt, const uint8_t *src, int w, int h, size_t pitch, uint32_t *dst);
float d9_half_to_float(uint16_t h);
uint16_t d9_float_to_half(float f);

// ---- objects -----------------------------------------------------------------------------------------------
enum { OT_D3D = 1, OT_DEV, OT_SWAP, OT_SURF, OT_TEX, OT_VOLUME, OT_VB, OT_IB, OT_DECL, OT_VS, OT_PS, OT_SB, OT_QUERY };
enum { VT_D3D, VT_D3DEX, VT_DEV, VT_DEVEX, VT_SWAP, VT_SURF, VT_TEX, VT_CUBE, VT_VOLTEX, VT_VOLUME, VT_VB, VT_IB,
       VT_DECL, VT_VS, VT_PS, VT_SB, VT_QUERY, VT_COUNT };

typedef struct D9Priv { struct D9Priv *next; uint8_t guid[16]; uint32_t size, flags; uint8_t *data; } D9Priv;
struct D9Dev;
typedef struct D9Obj {
    uint64_t vtbl;                 // first: what the program sees
    int type, kind;
    uint32_t refs, irefs;          // external (COM) references, internal holds
    struct D9Dev *dev;
    struct D9Obj *container;       // child objects (texture levels, back buffers): the owner
    struct D9Obj *reg_next;        // per-process registry
    D9Priv *priv;
    uint32_t priority;
    int dead;
} D9Obj;

typedef struct D9Surf D9Surf;
typedef struct D9Tex D9Tex;
typedef struct D9Vol D9Vol;
typedef struct D9Buf D9Buf;
typedef struct D9Decl D9Decl;
typedef struct D9Shader D9Shader;
typedef struct D9SB D9SB;
typedef struct D9Swap D9Swap;
typedef struct D9Query D9Query;
typedef struct D9Dev D9Dev;

struct D9Surf {
    D9Obj o;
    int w, h, fmt, usage, pool, ms, depth_fmt;
    int pitch;
    uint8_t *data;                 // guest memory in `fmt` (colour surfaces)
    uint32_t *argb;                // render storage (ARGB8): == data for 32-bit formats, else a shadow of data
    int shadow;                    // argb is a separate buffer
    int native_stale, argb_stale;  // shadow / data need refreshing from the other
    float *depth; uint8_t *stencil; int dw, dh;   // depth-stencil surfaces
    float *sdepth; uint8_t *sstencil; int sw_, sh_; // scratch depth for render targets smaller than the depth buffer
    D9Tex *tex; int level, face;   // owning texture
    int lockable;
    int lock_count, lock_flags;
    int modified;
    int is_back;
    // GetDC
    uint64_t dc_hdc, dc_bmp; uint32_t *dc_buf;
};

struct D9Vol {
    D9Obj o;
    int w, h, d, fmt, level;
    uint8_t *data;
    int rowpitch, slicepitch;
    D9Tex *tex;
    int lock_count;
    int modified;
};

struct D9Tex {
    D9Obj o;                       // o.kind: SG_TEX_2D / CUBE / VOLUME
    int w, h, d, levels, fmt, usage, pool;
    D9Surf *surf[6][SG_MAX_MIPS];
    D9Vol *vol[SG_MAX_MIPS];
    uint32_t lod, autogen_filter;
    SgTexture sg; int sg_valid;
    int dirty;
    int is_rt;
};

struct D9Buf {
    D9Obj o;                       // o.kind: 0 vertex buffer, 1 index buffer
    uint32_t size, usage, pool, fvf, fmt;
    uint8_t *data;
    int locked; uint32_t lock_off, lock_size, lock_flags;
};

typedef struct D9Elem { uint16_t stream, offset; uint8_t type, method, usage, index; } D9Elem;
struct D9Decl {
    D9Obj o;
    D9Elem e[17]; int n;
    uint32_t fvf;
};

struct D9Shader {
    D9Obj o;
    D3dShader *sh;
    int is_ps;
};

struct D9Query {
    D9Obj o;
    int qtype, issued, building;
    uint64_t pix0, value;
    double t;
};

// ---- device state ------------------------------------------------------------------------------------------
#define D9_NSAMP 21
#define D9_NLIGHT 64
#define D9_MAXSWAP 4
typedef struct D3DLight9 { uint32_t type; float diffuse[4], specular[4], ambient[4], pos[3], dir[3], range, falloff, att0, att1, att2, theta, phi; } D3DLight9;
typedef struct D3DMaterial9 { float diffuse[4], ambient[4], specular[4], emissive[4], power; } D3DMaterial9;
typedef struct D3DViewport9 { uint32_t x, y, w, h; float minz, maxz; } D3DViewport9;
typedef struct D9Stream { D9Buf *vb; uint32_t offset, stride, freq; } D9Stream;

typedef struct D9State {
    uint32_t rs[256];
    uint32_t ss[D9_NSAMP][16];
    uint32_t tss[8][33];
    D9Obj *tex[D9_NSAMP];
    float xf[25][16];              // 2 view, 3 projection, 16..23 texture, 24 world
    D3DViewport9 vp;
    D3DMaterial9 mat;
    D3DLight9 light[D9_NLIGHT];
    uint8_t light_set[D9_NLIGHT], light_on[D9_NLIGHT];
    float clip[6][4];
    int32_t scissor[4];            // left top right bottom
    D9Shader *vs, *ps;
    D9Decl *decl;
    uint32_t fvf;
    D9Stream stream[16];
    D9Buf *ib;
    D3dShaderConsts vsc, psc;
    int sw_vp;
    float npatch;
} D9State;

typedef struct D9Mask {
    uint8_t rs[256], ss[D9_NSAMP][16], tss[8][33], tex[D9_NSAMP], xf[25];
    uint8_t vp, mat, scissor, vs, ps, decl, fvf, ib, sw, npatch;
    uint8_t light[D9_NLIGHT], light_on[D9_NLIGHT], clip[6], stream[16], freq[16];
    uint8_t vsf[256], vsi[16], vsb[16], psf[256], psi[16], psb[16];
} D9Mask;

struct D9SB {
    D9Obj o;
    D9State *st;
    D9Mask m;
    int type;
};

struct D9Swap {
    D9Obj o;
    uint32_t w, h, fmt, nback, effect, interval, flags, depth_fmt, refresh;
    int windowed, autods;
    uint64_t hwnd;
    D9Surf *back[4];
    uint32_t present_count;
    int implicit;
};

struct D9Dev {
    D9Obj o;
    int ex;
    D9Obj *d3d;
    uint32_t adapter, devtype, behavior;
    uint64_t focus;
    D9Swap *swap[D9_MAXSWAP];
    int nswap;
    D9Surf *rt, *ds;
    D9State st;
    D9SB *rec;
    uint32_t (*pal)[256];
    uint32_t cur_pal;
    uint16_t gamma[3 * 256];
    int in_scene;
    uint32_t maxlatency;
    double last_present;
    // scratch
    SgVertex *vbuf; size_t vcap;
    SgVertex *obuf; size_t ocap;
    int32_t *vmap; size_t vmapcap;
    uint8_t *tmp; size_t tmpcap;
    D9Decl *fvfc[16]; int nfvfc;
    uint64_t draw_calls;
    int torn;
};

// ---- shared helpers ------------------------------------------------------------------------------------------
typedef struct D9Method { const char *name; HleFn fn; int nargs; } D9Method;   // nargs includes "this"
uint64_t d9_vt(int which);               // vtable (built on first use) of the current process
void d9_vt_register(int which, const char *iface, const D9Method *m, int n);
void *d9_new(int type, size_t size);     // guest memory, registered for process cleanup
void d9_obj_setvt(D9Obj *o, int vt);
uint32_t d9_addref(D9Obj *o);
uint32_t d9_release(D9Obj *o);
void d9_hold(D9Obj *o);
void d9_unhold(D9Obj *o);
void d9_destroy(D9Obj *o);               // frees the object (dispatches on type)
void d9_process_exit(void);
void d9_free_block(D9Obj *o);
void d9_priv_free_all(D9Obj *o);
void d9_res_destroy(D9Obj *o);
void d9_sb_destroy(D9SB *sb);
void d9_dev_destroy(D9Dev *d);
void d9_dev_teardown(D9Dev *d);
void d9_shader_free(D9Shader *s);
void d9_swap_destroy(D9Swap *sw);
extern int d9_exiting;
uint64_t d9_h2g(const void *p);
uint32_t d9_iunknown_qi(D9Obj *o, uint64_t riid, uint64_t ppv);
float d9_farg(X86State *st, int n);
#define D9OBJ(T, n) ((T *)G2H(ARG(n)))
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
void d9_wr32(uint64_t a, uint32_t v);    // guest-memory u32 helpers
uint32_t d9_rd32(uint64_t a);
void d9_wrf(uint64_t a, float v);
float d9_rdf(uint64_t a);

// d3d9.c
extern const HleEntry hle_d3d9_table[];
void d9_screen_size(int *w, int *h);

// d3d9_res.c
void d9_register_res_vtables(void);
D9Surf *d9_surf_create(D9Dev *dev, int w, int h, int fmt, int usage, int pool, int rt, int depth, D9Obj *container);
void d9_surf_free_storage(D9Surf *s);
void d9_surf_to_native(D9Surf *s);       // render storage -> lockable memory
void d9_surf_from_native(D9Surf *s);
uint32_t *d9_surf_argb(D9Surf *s);       // ARGB8 render view (refreshes the shadow)
void d9_surf_touch(D9Surf *s);           // contents changed (draw, clear, copy)
int d9_surf_read_argb(D9Surf *s, const int32_t *rc, uint32_t *out);   // rc: left top right bottom (NULL = all), out w*h
int d9_surf_write_argb(D9Surf *s, const int32_t *rc, const uint32_t *in);
uint32_t d9_surf_copy(D9Surf *dst, const int32_t *drc, D9Surf *src, const int32_t *src_rc, int filter);
D9Tex *d9_tex_create(D9Dev *dev, int kind, int w, int h, int d, int levels, int usage, int fmt, int pool, uint32_t *hr);
SgTexture *d9_tex_prepare(D9Tex *t);     // rebuilt SgTexture if modified, else the cached one
D9Buf *d9_buf_create(D9Dev *dev, int is_ib, uint32_t size, uint32_t usage, uint32_t fvf_or_fmt, uint32_t pool);
D9Decl *d9_decl_create(D9Dev *dev, const D9Elem *e, int n, uint32_t fvf);
D9Decl *d9_decl_from_fvf(D9Dev *dev, uint32_t fvf);
int d9_decl_size(const D9Elem *e, int n, int stream);
D9Query *d9_query_create(D9Dev *dev, int type);

// d3d9_dev.c
void d9_register_dev_vtables(void);
D9Dev *d9_dev_create(D9Obj *d3d, uint32_t adapter, uint32_t devtype, uint64_t focus, uint32_t behavior, uint64_t pp, int ex, uint32_t *hr);
D9Swap *d9_swap_create(D9Dev *dev, uint64_t pp, int implicit, uint32_t *hr);
void d9_dev_default_state(D9Dev *dev);
uint32_t d9_present(D9Dev *dev, D9Swap *sw, const int32_t *src, const int32_t *dst, uint64_t hwnd);
void d9_caps(uint32_t *c);               // fills a D3DCAPS9 (76 DWORDs)
void d9_dev_release_state(D9Dev *dev);
uint32_t d9_bind_texture(D9Dev *dev, uint32_t stage, D9Obj *tex);

// d3d9_draw.c
uint32_t d9_clear(D9Dev *dev, uint32_t count, uint64_t rects, uint32_t flags, uint32_t color, float z, uint32_t stencil);
uint32_t d9_draw(D9Dev *dev, int ptype, int base_vertex, uint32_t min_vertex, uint32_t num_vertices, uint32_t start_index, uint32_t prim_count,
                 int indexed, const void *up_vertices, uint32_t up_stride, const void *up_indices, int up_index32);
uint32_t d9_process_vertices(D9Dev *dev, uint32_t src_start, uint32_t dest_index, uint32_t count, D9Buf *dst, D9Decl *decl, uint32_t flags);
void d9_surf_target(D9Dev *dev, D9Surf *rt, D9Surf *ds, SgTarget *t);
