// SoulCore - Direct3D 9 (d3d9.dll): the IDirect3D9(Ex) factory, object infrastructure (reference counting,
// vtables of thunks, per-process cleanup) and the DLL exports. The device is in d3d9_dev.c, resources in
// d3d9_res.c, vertex/pixel processing and drawing in d3d9_draw.c; rendering is done by sgpu.
#include "d3d9_priv.h"

typedef struct D9D3D { D9Obj o; int ex; } D9D3D;

// ---- small guest-memory helpers -------------------------------------------------------------------------------
void d9_wr32(uint64_t a, uint32_t v) { mem_wr(a, 4, v); }
uint32_t d9_rd32(uint64_t a) { return (uint32_t)mem_rd(a, 4); }
void d9_wrf(uint64_t a, float v) { mem_wr(a, 4, f2u(v)); }
float d9_rdf(uint64_t a) { return u2f((uint32_t)mem_rd(a, 4)); }
uint64_t d9_h2g(const void *p) { return H2G(p); }
// A float argument: Win64 passes the first four in XMM registers, everything else is a stack slot.
float d9_farg(X86State *st, int n) {
    if (!g_mode32 && n < 4) return st->xmm[n].f[0];
    return u2f((uint32_t)hle_arg(st, n));
}
void d9_screen_size(int *w, int *h) {
    GuiState *g = gui();
    *w = g->screen_w > 0 ? g->screen_w : 1024;
    *h = g->screen_h > 0 ? g->screen_h : 768;
}

// ---- object registry and vtables -------------------------------------------------------------------------------
static D9Obj *g_reg;
SOUL_PROCESS_VAR(g_reg)
static uint64_t g_vts[VT_COUNT];
SOUL_PROCESS_VAR(g_vts)
static struct { const char *iface; const D9Method *m; int n; } g_vtdef[VT_COUNT];
int d9_exiting;

void d9_vt_register(int which, const char *iface, const D9Method *m, int n) { g_vtdef[which].iface = iface; g_vtdef[which].m = m; g_vtdef[which].n = n; }

void d9_m_QI(X86State *st) { D9Obj *o = (D9Obj *)G2H(ARG(0)); RET(d9_iunknown_qi(o, ARG(1), ARG(2))); }
void d9_m_AddRef(X86State *st) { RET(d9_addref((D9Obj *)G2H(ARG(0)))); }
void d9_m_Release(X86State *st) { RET(d9_release((D9Obj *)G2H(ARG(0)))); }

// ---- IDirect3D9 ----------------------------------------------------------------------------------------------------
static int mode_list(int fmt, int *w, int *h, int max) {
    static const int cand[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 768 }, { 1280, 800 }, { 1280, 960 },
        { 1280, 1024 }, { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2048, 1536 },
        { 2560, 1440 }, { 2732, 2048 } };
    if (fmt != F_X8R8G8B8 && fmt != F_R5G6B5 && fmt != F_A2R10G10B10 && fmt != F_X1R5G5B5) return 0;
    int sw, sh, n = 0, have = 0;
    d9_screen_size(&sw, &sh);
    for (size_t i = 0; i < sizeof cand / sizeof cand[0] && n < max; i++) {
        if (cand[i][0] > sw || cand[i][1] > sh) continue;
        if (cand[i][0] == sw && cand[i][1] == sh) have = 1;
        w[n] = cand[i][0]; h[n] = cand[i][1]; n++;
    }
    if (!have && n < max) {
        int pos = n;
        while (pos > 0 && (w[pos - 1] > sw || (w[pos - 1] == sw && h[pos - 1] > sh))) { w[pos] = w[pos - 1]; h[pos] = h[pos - 1]; pos--; }
        w[pos] = sw; h[pos] = sh; n++;
    }
    return n;
}
static void d3d_RegisterSoftwareDevice(X86State *st) { RET(D3D_OK); }
static void d3d_GetAdapterCount(X86State *st) { RET(1); }
static void d3d_GetAdapterIdentifier(X86State *st) { // (this, adapter, flags, D3DADAPTER_IDENTIFIER9 *)
    if (ARG(1) != 0 || !ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
    uint8_t *p = G2H(ARG(3));
    memset(p, 0, 1100);   // MinGW's i686 D3DADAPTER_IDENTIFIER9 is 1100 bytes (MSVC pads to 1104): never write past the last field
    strcpy((char *)p, "soul_sgpu.dll");
    strcpy((char *)p + 512, "Soul sgpu (software 3D)");
    strcpy((char *)p + 1024, "\\\\.\\DISPLAY1");
    uint32_t v[4] = { 0x00010001, 0x000A0000, 0x106B, 0x0001 }; // DriverVersion (low, high), VendorId, DeviceId
    memcpy(p + 1056, v, 16);
    uint32_t rev = 1;
    memcpy(p + 1076, &rev, 4);
    static const uint8_t guid[16] = { 0x53, 0x6F, 0x75, 0x6C, 0x73, 0x67, 0x70, 0x75, 0xD3, 0xD9, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05 };
    memcpy(p + 1080, guid, 16);
    uint32_t whql = 1;
    memcpy(p + 1096, &whql, 4);
    RET(D3D_OK);
}
static void d3d_GetAdapterModeCount(X86State *st) {
    int w[32], h[32];
    RET(ARG(1) != 0 ? 0 : mode_list((int)ARG(2), w, h, 32));
}
static void d3d_EnumAdapterModes(X86State *st) { // (this, adapter, format, mode, D3DDISPLAYMODE *)
    int w[32], h[32];
    int n = ARG(1) != 0 ? 0 : mode_list((int)ARG(2), w, h, 32);
    uint32_t m = (uint32_t)ARG(3);
    if (!ARG(4) || m >= (uint32_t)n) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(4), (uint32_t)w[m]); d9_wr32(ARG(4) + 4, (uint32_t)h[m]); d9_wr32(ARG(4) + 8, 60); d9_wr32(ARG(4) + 12, (uint32_t)ARG(2));
    RET(D3D_OK);
}
static void d3d_GetAdapterDisplayMode(X86State *st) {
    int w, h;
    d9_screen_size(&w, &h);
    if (ARG(1) != 0 || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(2), (uint32_t)w); d9_wr32(ARG(2) + 4, (uint32_t)h); d9_wr32(ARG(2) + 8, 60); d9_wr32(ARG(2) + 12, F_X8R8G8B8);
    RET(D3D_OK);
}
static int adapter_fmt_ok(int f) { return f == F_X8R8G8B8 || f == F_R5G6B5 || f == F_X1R5G5B5 || f == F_A2R10G10B10; }
static int depth_fmt_ok(int f) {
    switch (f) { case F_D16: case F_D16_LOCKABLE: case F_D24S8: case F_D24X8: case F_D32: case F_D15S1: case F_D24X4S4: case F_D32F_LOCKABLE: case F_D24FS8: return 1; }
    return 0;
}
static void d3d_CheckDeviceType(X86State *st) { // (this, adapter, devtype, adapterfmt, backfmt, windowed)
    if (ARG(1) != 0) { RET(D3DERR_INVALIDCALL); return; }
    int bf = (int)ARG(4);
    if (ARG(2) != 1 && ARG(2) != 2) { RET(D3DERR_NOTAVAILABLE); return; }
    if (!adapter_fmt_ok((int)ARG(3))) { RET(D3DERR_NOTAVAILABLE); return; }
    if (!(bf == 0 && ARG(5)) && bf != F_A8R8G8B8 && bf != F_X8R8G8B8 && bf != F_R5G6B5 && bf != F_X1R5G5B5 && bf != F_A1R5G5B5 && bf != F_A2R10G10B10) { RET(D3DERR_NOTAVAILABLE); return; }
    RET(D3D_OK);
}
static void d3d_CheckDeviceFormat(X86State *st) { // (this, adapter, devtype, adapterfmt, usage, rtype, checkfmt)
    uint32_t usage = (uint32_t)ARG(4), rtype = (uint32_t)ARG(5);
    int fmt = (int)ARG(6);
    if (ARG(1) != 0 || !adapter_fmt_ok((int)ARG(3))) { RET(D3DERR_NOTAVAILABLE); return; }
    uint32_t hr = D3D_OK;
    if (rtype == RT_VERTEXBUFFER) hr = fmt == F_VERTEXDATA ? D3D_OK : D3DERR_NOTAVAILABLE;
    else if (rtype == RT_INDEXBUFFER) hr = (fmt == F_INDEX16 || fmt == F_INDEX32) ? D3D_OK : D3DERR_NOTAVAILABLE;
    else if (usage & USAGE_DEPTHSTENCIL) hr = (rtype == RT_SURFACE && depth_fmt_ok(fmt)) ? D3D_OK : D3DERR_NOTAVAILABLE;
    else if (d9_fmt_is_depth(fmt)) hr = D3DERR_NOTAVAILABLE;
    else if (rtype == RT_SURFACE && !(usage & USAGE_RENDERTARGET)) hr = d9_fmt_known(fmt) ? D3D_OK : D3DERR_NOTAVAILABLE;
    else if (usage & USAGE_RENDERTARGET) hr = d9_fmt_rt_ok(fmt) ? D3D_OK : D3DERR_NOTAVAILABLE;
    else hr = d9_fmt_texture_ok(fmt) && fmt != F_UNKNOWN ? D3D_OK : D3DERR_NOTAVAILABLE;
    if (hr == D3D_OK && (usage & USAGE_AUTOGENMIPMAP) && d9_fmt_is_dxt(fmt)) hr = D3DERR_NOTAVAILABLE;
    RET(hr);
}
static void d3d_CheckDeviceMultiSampleType(X86State *st) { // (this, adapter, devtype, fmt, windowed, mst, pQuality)
    if (ARG(1) != 0) { RET(D3DERR_INVALIDCALL); return; }
    if (ARG(5) != 0) { if (ARG(6)) d9_wr32(ARG(6), 0); RET(D3DERR_NOTAVAILABLE); return; }
    if (ARG(6)) d9_wr32(ARG(6), 1);
    RET(D3D_OK);
}
static void d3d_CheckDepthStencilMatch(X86State *st) { // (this, adapter, devtype, adapterfmt, rtfmt, dsfmt)
    if (ARG(1) != 0) { RET(D3DERR_INVALIDCALL); return; }
    RET(depth_fmt_ok((int)ARG(5)) && (d9_fmt_rt_ok((int)ARG(4)) || (int)ARG(4) == F_UNKNOWN) ? D3D_OK : D3DERR_NOTAVAILABLE);
}
static void d3d_CheckDeviceFormatConversion(X86State *st) { RET(ARG(1) == 0 ? D3D_OK : D3DERR_INVALIDCALL); }
static void d3d_GetDeviceCaps(X86State *st) { // (this, adapter, devtype, D3DCAPS9 *)
    if (ARG(1) != 0 || !ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t c[76];
    d9_caps(c);
    c[0] = (uint32_t)ARG(2);
    memcpy(G2H(ARG(3)), c, sizeof c);
    RET(D3D_OK);
}
static void d3d_GetAdapterMonitor(X86State *st) { RET(ARG(1) == 0 ? 0x10001 : 0); }
static void d3d_CreateDevice(X86State *st) { // (this, adapter, devtype, hFocus, behavior, pp, ppDevice)
    D9D3D *d = D9OBJ(D9D3D, 0);
    if (!ARG(5) || !ARG(6)) { RET(D3DERR_INVALIDCALL); return; }
    wr_ptr(ARG(6), 0);
    uint32_t hr;
    D9Dev *dev = d9_dev_create(&d->o, (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3), (uint32_t)ARG(4), ARG(5), 0, &hr);
    if (!dev) { RET(hr); return; }
    wr_ptr(ARG(6), H2G(dev));
    RET(D3D_OK);
}
static void d3d_GetAdapterModeCountEx(X86State *st) { // (this, adapter, D3DDISPLAYMODEFILTER *)
    int w[32], h[32];
    RET(ARG(1) != 0 ? 0 : mode_list(F_X8R8G8B8, w, h, 32));
}
static void d3d_EnumAdapterModesEx(X86State *st) { // (this, adapter, filter, mode, D3DDISPLAYMODEEX *)
    int w[32], h[32];
    int n = ARG(1) != 0 ? 0 : mode_list(F_X8R8G8B8, w, h, 32);
    uint32_t m = (uint32_t)ARG(3);
    if (!ARG(4) || m >= (uint32_t)n) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t p = ARG(4);
    d9_wr32(p, 24); d9_wr32(p + 4, (uint32_t)w[m]); d9_wr32(p + 8, (uint32_t)h[m]); d9_wr32(p + 12, 60); d9_wr32(p + 16, F_X8R8G8B8); d9_wr32(p + 20, 1);
    RET(D3D_OK);
}
static void d3d_GetAdapterDisplayModeEx(X86State *st) { // (this, adapter, D3DDISPLAYMODEEX *, D3DDISPLAYROTATION *)
    int w, h;
    d9_screen_size(&w, &h);
    if (ARG(1) != 0) { RET(D3DERR_INVALIDCALL); return; }
    if (ARG(2)) { uint64_t p = ARG(2); d9_wr32(p, 24); d9_wr32(p + 4, (uint32_t)w); d9_wr32(p + 8, (uint32_t)h); d9_wr32(p + 12, 60); d9_wr32(p + 16, F_X8R8G8B8); d9_wr32(p + 20, 1); }
    if (ARG(3)) d9_wr32(ARG(3), 1);
    RET(D3D_OK);
}
static void d3d_CreateDeviceEx(X86State *st) { // (this, adapter, devtype, hFocus, behavior, pp, fullscreen mode, ppDevice)
    D9D3D *d = D9OBJ(D9D3D, 0);
    if (!ARG(5) || !ARG(7)) { RET(D3DERR_INVALIDCALL); return; }
    wr_ptr(ARG(7), 0);
    uint32_t hr;
    D9Dev *dev = d9_dev_create(&d->o, (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(3), (uint32_t)ARG(4), ARG(5), 1, &hr);
    if (!dev) { RET(hr); return; }
    wr_ptr(ARG(7), H2G(dev));
    RET(D3D_OK);
}
static void d3d_GetAdapterLUID(X86State *st) { if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; } d9_wr32(ARG(2), 0x5001); d9_wr32(ARG(2) + 4, 0); RET(D3D_OK); }

#define U3 { "QueryInterface", d9_m_QI, 3 }, { "AddRef", d9_m_AddRef, 1 }, { "Release", d9_m_Release, 1 }
#define D3DM { "RegisterSoftwareDevice", d3d_RegisterSoftwareDevice, 2 }, { "GetAdapterCount", d3d_GetAdapterCount, 1 }, \
    { "GetAdapterIdentifier", d3d_GetAdapterIdentifier, 4 }, { "GetAdapterModeCount", d3d_GetAdapterModeCount, 3 }, \
    { "EnumAdapterModes", d3d_EnumAdapterModes, 5 }, { "GetAdapterDisplayMode", d3d_GetAdapterDisplayMode, 3 }, \
    { "CheckDeviceType", d3d_CheckDeviceType, 6 }, { "CheckDeviceFormat", d3d_CheckDeviceFormat, 7 }, \
    { "CheckDeviceMultiSampleType", d3d_CheckDeviceMultiSampleType, 7 }, { "CheckDepthStencilMatch", d3d_CheckDepthStencilMatch, 6 }, \
    { "CheckDeviceFormatConversion", d3d_CheckDeviceFormatConversion, 5 }, { "GetDeviceCaps", d3d_GetDeviceCaps, 4 }, \
    { "GetAdapterMonitor", d3d_GetAdapterMonitor, 2 }, { "CreateDevice", d3d_CreateDevice, 7 }
static const D9Method m_d3d[] = { U3, D3DM };
static const D9Method m_d3dex[] = { U3, D3DM, { "GetAdapterModeCountEx", d3d_GetAdapterModeCountEx, 3 }, { "EnumAdapterModesEx", d3d_EnumAdapterModesEx, 5 },
    { "GetAdapterDisplayModeEx", d3d_GetAdapterDisplayModeEx, 4 }, { "CreateDeviceEx", d3d_CreateDeviceEx, 8 }, { "GetAdapterLUID", d3d_GetAdapterLUID, 3 } };

static void register_all(void) {
    static int done;
    if (done) return;
    done = 1;
    d9_vt_register(VT_D3D, "IDirect3D9", m_d3d, (int)(sizeof m_d3d / sizeof m_d3d[0]));
    d9_vt_register(VT_D3DEX, "IDirect3D9Ex", m_d3dex, (int)(sizeof m_d3dex / sizeof m_d3dex[0]));
    d9_register_res_vtables();
    d9_register_dev_vtables();
}
uint64_t d9_vt(int which) {
    register_all();
    if (g_vts[which]) return g_vts[which];
    int n = g_vtdef[which].n;
    uint8_t *t = g_calloc((size_t)n, (size_t)PSZ);
    for (int i = 0; i < n; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s::%s", g_vtdef[which].iface, g_vtdef[which].m[i].name);
        uint32_t id = hle_thunk_for(g_proc, g_vtdef[which].m[i].fn, "d3d9", name);
        g_proc->thunks[id].argbytes = g_mode32 ? (uint16_t)(4 * g_vtdef[which].m[i].nargs) : 0;
        wr_ptr(H2G(t) + (uint64_t)i * (uint64_t)PSZ, g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE);
    }
    g_vts[which] = H2G(t);
    return g_vts[which];
}

void *d9_new(int type, size_t size) {
    D9Obj *o = g_calloc(1, size);
    if (!o) return NULL;
    o->type = type;
    o->refs = 0;
    o->reg_next = g_reg;
    g_reg = o;
    return o;
}
void d9_obj_setvt(D9Obj *o, int vt) { o->vtbl = d9_vt(vt); }
void d9_free_block(D9Obj *o) {
    D9Obj **pp = &g_reg;
    while (*pp && *pp != o) pp = &(*pp)->reg_next;
    if (*pp) *pp = o->reg_next;
    g_free(o);
}
void d9_hold(D9Obj *o) { o->irefs++; }
void d9_unhold(D9Obj *o) {
    if (o->irefs) o->irefs--;
    if (!o->refs && !o->irefs && !o->container && !d9_exiting) d9_destroy(o);
}
uint32_t d9_addref(D9Obj *o) {
    if (o->container && o->refs == 0) d9_addref(o->container);
    return ++o->refs;
}
uint32_t d9_release(D9Obj *o) {
    if (o->refs > 0) o->refs--;
    uint32_t r = o->refs;
    if (o->container) { if (r == 0) d9_release(o->container); return r; }
    if (r == 0) {
        if (o->type == OT_DEV) { d9_dev_teardown((D9Dev *)o); if (!o->irefs) d9_destroy(o); }
        else if (o->irefs == 0) d9_destroy(o);
    }
    return r;
}
void d9_destroy(D9Obj *o) {
    if (o->dead) return;
    o->dead = 1;
    D9Dev *dev = o->dev;
    int child = o->container != NULL;
    switch (o->type) {
    case OT_D3D: break;
    case OT_DEV: d9_dev_destroy((D9Dev *)o); dev = NULL; break;
    case OT_SB: d9_sb_destroy((D9SB *)o); break;
    case OT_SWAP: d9_swap_destroy((D9Swap *)o); break;
    default: d9_res_destroy(o); break;
    }
    d9_priv_free_all(o);
    if (dev && !child && o->type != OT_DEV && o->type != OT_SWAP) d9_unhold(&dev->o);
    d9_free_block(o);
}
void d9_process_exit(void) {
    d9_exiting = 1;
    for (;;) {
        D9Obj *o = g_reg;
        while (o && o->container) o = o->reg_next;   // children go with their owner
        if (!o) break;
        // objects freed by their owners were already unlinked; destroy the rest without reference logic
        switch (o->type) {
        case OT_DEV: d9_dev_destroy((D9Dev *)o); break;
        case OT_SB: d9_sb_destroy((D9SB *)o); break;
        case OT_SWAP: d9_swap_destroy((D9Swap *)o); break;
        case OT_D3D: break;
        default: d9_res_destroy(o); break;
        }
        d9_priv_free_all(o);
        d9_free_block(o);
    }
    for (int i = 0; i < VT_COUNT; i++) if (g_vts[i]) { g_free(G2H(g_vts[i])); g_vts[i] = 0; }
    d9_exiting = 0;
}

// ---- QueryInterface ----------------------------------------------------------------------------------------------
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Gd;
#define GD(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) { a, b, c, { d0, d1, d2, d3, d4, d5, d6, d7 } }
static int is_guid(const Gd *g, uint32_t a, uint16_t b, uint16_t c, uint8_t d0, uint8_t d1, uint8_t d2, uint8_t d3, uint8_t d4, uint8_t d5, uint8_t d6, uint8_t d7) {
    Gd x = GD(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7);
    return memcmp(g, &x, 16) == 0;
}
uint32_t d9_iunknown_qi(D9Obj *o, uint64_t riid, uint64_t ppv) {
    if (!ppv) return E_POINTER_;
    wr_ptr(ppv, 0);
    if (!riid) return E_INVALIDARG_;
    Gd g;
    memcpy(&g, G2H(riid), 16);
#define IS(...) is_guid(&g, __VA_ARGS__)
    int ok = 0;
    D9Dev *dev = o->type == OT_DEV ? (D9Dev *)o : NULL;
    if (IS(0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46)) ok = 1;
    else switch (o->type) {
    case OT_D3D:
        ok = IS(0x81BDCBCA, 0x64D4, 0x426d, 0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C) ||
             (((D9D3D *)o)->ex && IS(0x02177241, 0x69FC, 0x400C, 0x8F, 0xF1, 0x93, 0xA4, 0x4D, 0xF6, 0x86, 0x1D));
        break;
    case OT_DEV:
        ok = IS(0xD0223B96, 0xBF7A, 0x43fd, 0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB) ||
             (dev->ex && IS(0xB18B10CE, 0x2649, 0x405a, 0x87, 0x0F, 0x95, 0xF7, 0x77, 0xD4, 0x31, 0x3A));
        break;
    case OT_SWAP: ok = IS(0x794950F2, 0xADFC, 0x458a, 0x90, 0x5E, 0x10, 0xA1, 0x0B, 0x0B, 0x50, 0x3B) || (o->dev && o->dev->ex && IS(0x91886CAF, 0x1C3D, 0x4d2e, 0xA0, 0xAB, 0x3E, 0x4C, 0x7D, 0x8D, 0x33, 0x03)); break;
    case OT_SURF: ok = IS(0x0CFBAF3A, 0x9FF6, 0x429a, 0x99, 0xB3, 0xA2, 0x79, 0x6A, 0xF8, 0xB8, 0x9B) || IS(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04); break;
    case OT_VOLUME: ok = IS(0x24F416E6, 0x1F67, 0x4aa7, 0xB8, 0x8E, 0xD3, 0x3F, 0x6F, 0x31, 0x28, 0xA1); break;
    case OT_TEX:
        ok = IS(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) || IS(0x580CA87E, 0x1D3C, 0x4d54, 0x99, 0x1D, 0xB7, 0xD3, 0xE3, 0xC2, 0x98, 0xCE) ||
             (o->kind == SG_TEX_2D && IS(0x85C31227, 0x3DE5, 0x4f00, 0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5)) ||
             (o->kind == SG_TEX_CUBE && IS(0xFFF32F81, 0xD953, 0x473a, 0x92, 0x23, 0x93, 0xD6, 0x52, 0xAB, 0xA9, 0x3F)) ||
             (o->kind == SG_TEX_VOLUME && IS(0x2518526C, 0xE789, 0x4111, 0xA7, 0xB9, 0x47, 0xEF, 0x32, 0x8D, 0x13, 0xE6));
        break;
    case OT_VB: ok = IS(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) || IS(0xB64BB1B5, 0xFD70, 0x4df6, 0xBF, 0x91, 0x19, 0xD0, 0xA1, 0x24, 0x55, 0xE3); break;
    case OT_IB: ok = IS(0x05EEC05D, 0x8F7D, 0x4362, 0xB9, 0x99, 0xD1, 0xBA, 0xF3, 0x57, 0xC7, 0x04) || IS(0x7C9DD65E, 0xD3F7, 0x4529, 0xAC, 0xEE, 0x78, 0x58, 0x30, 0xAC, 0xDE, 0x35); break;
    case OT_DECL: ok = IS(0xDD13C59C, 0x36FA, 0x4098, 0xA8, 0xFB, 0xC7, 0xED, 0x39, 0xDC, 0x85, 0x46); break;
    case OT_VS: ok = IS(0xEFC5557E, 0x6265, 0x4613, 0x8A, 0x94, 0x43, 0x85, 0x78, 0x89, 0xEB, 0x36); break;
    case OT_PS: ok = IS(0x6D3BDBDC, 0x5B02, 0x4415, 0xB8, 0x52, 0xCE, 0x5E, 0x8B, 0xCC, 0xB2, 0x89); break;
    case OT_SB: ok = IS(0xB07C4FE5, 0x310D, 0x4ba8, 0xA2, 0x3C, 0x4F, 0x0F, 0x20, 0x6F, 0x21, 0x8B); break;
    case OT_QUERY: ok = IS(0xd9771460, 0xa695, 0x4f26, 0xbb, 0xd3, 0x27, 0xb8, 0x40, 0xb5, 0x41, 0xcc); break;
    }
#undef IS
    if (!ok) return E_NOINTERFACE_;
    d9_addref(o);
    wr_ptr(ppv, H2G(o));
    return D3D_OK;
}

// ---- caps ------------------------------------------------------------------------------------------------------------
void d9_caps(uint32_t *c) {
    memset(c, 0, 76 * 4);
    c[0] = 1;                       // HAL
    c[2] = 0x20000;                 // D3DCAPS_READ_SCANLINE
    c[3] = 0x40000000u | 0x20000000u | 0x10000000u | 0x20000u;
    c[4] = 0x20 | 0x80 | 0x100 | 0x200;
    c[5] = 0x80000000u | 0xF;
    c[6] = 3;
    c[7] = 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x8000 | 0x10000 | 0x20000 | 0x80000;
    c[8] = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x200 | 0x400 | 0x800 | 0x8000 | 0x10000 | 0x20000 | 0x100000;
    c[9] = 0x10 | 0x80 | 0x100 | 0x2000 | 0x10000 | 0x20000 | 0x100000 | 0x200000 | 0x400000 | 0x1000000 | 0x2000000 | 0x4000000;
    c[10] = 0xFF; c[11] = 0x3FFF; c[12] = 0x3FFF; c[13] = 0xFF;
    c[14] = 0x8 | 0x200 | 0x4000 | 0x80000;
    c[15] = 0x1 | 0x4 | 0x40 | 0x400 | 0x800 | 0x2000 | 0x4000 | 0x8000 | 0x10000;
    c[16] = 0x7030700; c[17] = 0x7030700; c[18] = 0x7030700;
    c[19] = 0x3F; c[20] = 0x3F; c[21] = 0x1F;
    c[22] = 4096; c[23] = 4096; c[24] = 2048; c[25] = 8192; c[26] = 4096; c[27] = 16;
    c[28] = f2u(1e10f); c[29] = f2u(-32768.0f); c[30] = f2u(-32768.0f); c[31] = f2u(32768.0f); c[32] = f2u(32768.0f); c[33] = f2u(0.0f);
    c[34] = 0x1FF;
    c[35] = 0x80000 | 0x100000 | 8;
    c[36] = 0x03FFFFFFu;
    c[37] = 8; c[38] = 8;
    c[39] = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x100;
    c[40] = 8; c[41] = 6; c[42] = 0; c[43] = 0;
    c[44] = f2u(256.0f);
    c[45] = 0xFFFFF; c[46] = 0xFFFFF; c[47] = 16; c[48] = 508;
    c[49] = 0xFFFE0300u; c[50] = 256; c[51] = 0xFFFF0300u;
    c[52] = f2u(1e10f);
    c[53] = 0x13;
    c[54] = f2u(0.0f);
    c[56] = 0; c[57] = 0; c[58] = 1;
    c[59] = 0x3FF; c[60] = 1; c[61] = 0x3000300;
    c[62] = 1; c[63] = 24; c[64] = 32; c[65] = 4;
    c[66] = 0x1F; c[67] = 24; c[68] = 32; c[69] = 4; c[70] = 512;
    c[71] = 0x3000300; c[72] = 65535; c[73] = 65535; c[74] = 32768; c[75] = 32768;
}

// ---- exports ----------------------------------------------------------------------------------------------------------
static D9D3D *d3d_new(int ex) {
    D9D3D *d = d9_new(OT_D3D, sizeof *d);
    if (!d) return NULL;
    d->ex = ex;
    d->o.kind = ex;
    d9_obj_setvt(&d->o, ex ? VT_D3DEX : VT_D3D);
    d->o.refs = 1;
    return d;
}
HLE(x_Direct3DCreate9) { D9D3D *d = d3d_new(0); RET(d ? H2G(d) : 0); }
HLE(x_Direct3DCreate9Ex) {
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    wr_ptr(ARG(1), 0);
    D9D3D *d = d3d_new(1);
    if (!d) { RET(E_OUTOFMEMORY_); return; }
    wr_ptr(ARG(1), H2G(d));
    RET(D3D_OK);
}
HLE_STUB_RET(x_zero, 0)
HLE(x_noop) { (void)st; }

const HleEntry hle_d3d9_table[] = {
    { "Direct3DCreate9", x_Direct3DCreate9, NULL }, { "Direct3DCreate9Ex", x_Direct3DCreate9Ex, NULL },
    { "Direct3DShaderValidatorCreate9", x_zero, NULL }, { "Direct3D9EnableMaximizedWindowedModeShim", x_zero, NULL },
    { "D3DPERF_BeginEvent", x_zero, NULL }, { "D3DPERF_EndEvent", x_zero, NULL }, { "D3DPERF_SetMarker", x_noop, NULL },
    { "D3DPERF_SetRegion", x_noop, NULL }, { "D3DPERF_QueryRepeatFrame", x_zero, NULL }, { "D3DPERF_SetOptions", x_noop, NULL },
    { "D3DPERF_GetStatus", x_zero, NULL }, { "DebugSetMute", x_noop, NULL },
    { NULL, NULL, NULL }
};
