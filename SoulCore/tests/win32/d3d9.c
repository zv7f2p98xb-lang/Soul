// Direct3D 9 on Soul's software 3D pipeline: every check prints "name: ok" (or FAIL with the values).
// Pixels are read back with GetRenderTargetData + LockRect, so the output is deterministic.
//   d3d9.exe          : the checks
//   d3d9.exe --gui    : draws a test pattern, presents it to the window and keeps it open (GUI script checks it)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "d3d9_shaders.h"

static IDirect3D9 *d3d;
static IDirect3DDevice9 *dev;
static HWND wnd;
static IDirect3DSurface9 *sysmem;
static unsigned char *snap_bits;
static int snap_pitch;
static int g_fail;

static void check(const char *name, int cond) {
    printf("%s: %s\n", name, cond ? "ok" : "FAIL");
    if (!cond) g_fail++;
}
static int near_(unsigned got, unsigned want, int tol) {
    for (int s = 0; s < 24; s += 8) {
        int a = (got >> s) & 255, b = (want >> s) & 255;
        if (a - b > tol || b - a > tol) return 0;
    }
    return 1;
}
static void check_px(const char *name, unsigned got, unsigned want, int tol) {
    int ok = near_(got & 0xFFFFFF, want & 0xFFFFFF, tol);
    if (ok) printf("%s: ok\n", name);
    else { printf("%s: FAIL got=%06x want=%06x\n", name, got & 0xFFFFFF, want & 0xFFFFFF); g_fail++; }
}

// reads the back buffer (or any 128x128 render target) into the snapshot
static int snapshot_rt(IDirect3DSurface9 *rt) {
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sysmem))) return 0;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(sysmem, &lr, NULL, D3DLOCK_READONLY))) return 0;
    snap_bits = lr.pBits;
    snap_pitch = lr.Pitch;
    return 1;
}
static void snap_end(void) { IDirect3DSurface9_UnlockRect(sysmem); snap_bits = NULL; }
static unsigned px(int x, int y) { return ((unsigned *)(snap_bits + (size_t)y * (size_t)snap_pitch))[x] & 0xFFFFFF; }
static IDirect3DSurface9 *backbuffer(void) {
    IDirect3DSurface9 *s = NULL;
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &s);
    return s;
}
// runs fn with the pixels of the current back buffer available through px()
static void with_snapshot(void (*fn)(void)) {
    IDirect3DSurface9 *bb = backbuffer();
    if (snapshot_rt(bb)) { fn(); snap_end(); } else { printf("snapshot: FAIL\n"); g_fail++; }
    IDirect3DSurface9_Release(bb);
}

typedef struct { float x, y, z, rhw; DWORD color; } VtxT;
#define FVF_T (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)
typedef struct { float x, y, z, rhw; DWORD color; float u, v; } VtxTex;
#define FVF_TEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)
typedef struct { float x, y, z, nx, ny, nz; } VtxL;
#define FVF_L (D3DFVF_XYZ | D3DFVF_NORMAL)
typedef struct { float x, y, z; DWORD color; } VtxD;
#define FVF_D (D3DFVF_XYZ | D3DFVF_DIFFUSE)

static DWORD fbits(float f) { DWORD d; memcpy(&d, &f, 4); return d; }
static void rs(D3DRENDERSTATETYPE t, DWORD v) { IDirect3DDevice9_SetRenderState(dev, t, v); }
static void ident(D3DMATRIX *m) { memset(m, 0, sizeof *m); m->_11 = m->_22 = m->_33 = m->_44 = 1; }

static void reset_states(void) {
    rs(D3DRS_LIGHTING, FALSE); rs(D3DRS_ZENABLE, FALSE); rs(D3DRS_ZWRITEENABLE, TRUE); rs(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    rs(D3DRS_CULLMODE, D3DCULL_NONE); rs(D3DRS_ALPHABLENDENABLE, FALSE); rs(D3DRS_ALPHATESTENABLE, FALSE); rs(D3DRS_STENCILENABLE, FALSE);
    rs(D3DRS_SCISSORTESTENABLE, FALSE); rs(D3DRS_FOGENABLE, FALSE); rs(D3DRS_COLORWRITEENABLE, 0xF); rs(D3DRS_SPECULARENABLE, FALSE);
    rs(D3DRS_FILLMODE, D3DFILL_SOLID); rs(D3DRS_SHADEMODE, D3DSHADE_GOURAUD); rs(D3DRS_POINTSIZE, fbits(1.0f));
    rs(D3DRS_BLENDOP, D3DBLENDOP_ADD); rs(D3DRS_SRCBLEND, D3DBLEND_ONE); rs(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    for (int i = 0; i < 4; i++) IDirect3DDevice9_SetTexture(dev, i, NULL);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_CURRENT);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    for (int i = 0; i < 2; i++) {
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, i, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    D3DMATRIX m; ident(&m);
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &m);
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &m);
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &m);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
}
static void clear(DWORD color) { IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, color, 1.0f, 0); }
static void quad_t(float x0, float y0, float x1, float y1, float z, DWORD c) {
    VtxT v[4] = { { x0, y0, z, 1, c }, { x1, y0, z, 1, c }, { x0, y1, z, 1, c }, { x1, y1, z, 1, c } };
    IDirect3DDevice9_SetFVF(dev, FVF_T);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(VtxT));
}
static void quad_tex(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, DWORD c) {
    VtxTex v[4] = { { x0, y0, 0, 1, c, u0, v0 }, { x1, y0, 0, 1, c, u1, v0 }, { x0, y1, 0, 1, c, u0, v1 }, { x1, y1, 0, 1, c, u1, v1 } };
    IDirect3DDevice9_SetFVF(dev, FVF_TEX);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(VtxTex));
}
static IDirect3DTexture9 *make_tex(int w, int h, D3DFORMAT fmt, const void *data, int bytes_pp) {
    IDirect3DTexture9 *t = NULL;
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, 0, fmt, D3DPOOL_MANAGED, &t, NULL))) return NULL;
    D3DLOCKED_RECT lr;
    IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
    for (int y = 0; y < h; y++) memcpy((char *)lr.pBits + (size_t)y * (size_t)lr.Pitch, (const char *)data + (size_t)y * (size_t)w * (size_t)bytes_pp, (size_t)w * (size_t)bytes_pp);
    IDirect3DTexture9_UnlockRect(t, 0);
    return t;
}

// ---- individual tests ----------------------------------------------------------------------------------------------------------------
static void t_caps(void) {
    D3DCAPS9 c;
    HRESULT hr = IDirect3DDevice9_GetDeviceCaps(dev, &c);
    printf("caps: hr=%d dev=%d vs=%x ps=%x tex=%u stages=%u simul=%u streams=%u vsconst=%u lights=%u\n", (int)(hr == D3D_OK), (int)c.DeviceType,
           (unsigned)(c.VertexShaderVersion & 0xFFFF), (unsigned)(c.PixelShaderVersion & 0xFFFF), (unsigned)c.MaxTextureWidth, (unsigned)c.MaxTextureBlendStages,
           (unsigned)c.MaxSimultaneousTextures, (unsigned)c.MaxStreams, (unsigned)c.MaxVertexShaderConst, (unsigned)c.MaxActiveLights);
    D3DADAPTER_IDENTIFIER9 id;
    IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id);
    printf("adapter: count=%u name=%d\n", IDirect3D9_GetAdapterCount(d3d), (int)(strlen(id.Description) > 3));
    printf("check: a8r8g8b8 tex %d, dxt1 tex %d, d24s8 %d, msaa %d\n",
           (int)SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8)),
           (int)SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, D3DFMT_DXT1)),
           (int)SUCCEEDED(IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, D3DFMT_D24S8)),
           (int)SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_4_SAMPLES, NULL)));
    D3DDISPLAYMODE dm;
    IDirect3D9_GetAdapterDisplayMode(d3d, 0, &dm);
    check("display mode", dm.Width >= 640 && dm.Height >= 480 && dm.Format == D3DFMT_X8R8G8B8);
    check("mode enum", IDirect3D9_GetAdapterModeCount(d3d, 0, D3DFMT_X8R8G8B8) > 0);
}

static void v_clear1(void) { check_px("clear color", px(10, 10), 0x336699, 0); check_px("clear color corner", px(127, 127), 0x336699, 0); }
static void t_clear(void) {
    reset_states();
    clear(0xFF336699);
    with_snapshot(v_clear1);
    // depth: clear z = 0.3, draw at z = 0.5 with LESS (rejected), then with clear z = 1.0 (accepted)
    rs(D3DRS_ZENABLE, TRUE); rs(D3DRS_ZFUNC, D3DCMP_LESS);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 0.3f, 0);
    quad_t(0, 0, 128, 128, 0.5f, 0xFFFF0000);
    IDirect3DSurface9 *bb = backbuffer();
    snapshot_rt(bb); check_px("depth reject", px(60, 60), 0x000000, 0); snap_end();
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
    quad_t(0, 0, 128, 128, 0.5f, 0xFFFF0000);
    snapshot_rt(bb); check_px("depth accept", px(60, 60), 0xFF0000, 0); snap_end();
    // clear with a rectangle
    rs(D3DRS_ZENABLE, FALSE);
    D3DRECT r = { 10, 20, 30, 40 };
    clear(0xFF000000);
    IDirect3DDevice9_Clear(dev, 1, &r, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0);
    snapshot_rt(bb);
    check("clear rect", px(10, 20) == 0x00FF00 && px(29, 39) == 0x00FF00 && px(30, 39) == 0 && px(9, 20) == 0 && px(10, 40) == 0);
    snap_end();
    IDirect3DSurface9_Release(bb);
}

static void v_halfpixel(void) {
    check_px("rhw quad inside", px(64, 64), 0xFF0000, 0);
    check("rhw quad corners", px(32, 32) == 0xFF0000 && px(95, 95) == 0xFF0000 && px(32, 95) == 0xFF0000 && px(95, 32) == 0xFF0000);
    check("rhw quad outside", px(31, 50) == 0x101010 && px(96, 50) == 0x101010 && px(50, 31) == 0x101010 && px(50, 96) == 0x101010 && px(31, 31) == 0x101010 && px(96, 96) == 0x101010);
}
static void t_halfpixel(void) {
    reset_states();
    clear(0xFF101010);
    quad_t(32, 32, 96, 96, 0, 0xFFFF0000);
    with_snapshot(v_halfpixel);
}

static void v_cube_a(void) {
    check_px("cube front face", px(64, 64), 0x8040FF, 3);
    check("cube silhouette", px(64, 40) != 0x202020 && px(10, 10) == 0x202020);
}
static void v_cube_b(void) { check_px("cube no-depth backface cull", px(64, 64), 0x8040FF, 3); }
static void v_cube_c(void) { check_px("cube depth hides back faces", px(64, 64), 0x8040FF, 3); }
static void v_cube_d(void) {
    unsigned l = px(54, 64), r = px(74, 64);
    int dark_l = near_(l, 0x000000, 3), dark_r = near_(r, 0x000000, 3);
    int lit_l = near_(l, 0x8040FF, 3), lit_r = near_(r, 0x8040FF, 3);
    check("cube lit/unlit faces", (dark_l && lit_r) || (dark_r && lit_l));
}
static void draw_cube(void) {
    // 24 vertices (4 per face) so every face has its own normal; faces: -Z, +Z, -X, +X, +Y, -Y (clockwise seen from outside)
    static const float n[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    static const float p[6][4][3] = {
        { { -1, 1, -1 }, { 1, 1, -1 }, { 1, -1, -1 }, { -1, -1, -1 } },
        { { 1, 1, 1 }, { -1, 1, 1 }, { -1, -1, 1 }, { 1, -1, 1 } },
        { { -1, 1, 1 }, { -1, 1, -1 }, { -1, -1, -1 }, { -1, -1, 1 } },
        { { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 }, { 1, -1, -1 } },
        { { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { -1, 1, -1 } },
        { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } },
    };
    VtxL v[24];
    unsigned short idx[36];
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++) { v[f * 4 + k].x = p[f][k][0]; v[f * 4 + k].y = p[f][k][1]; v[f * 4 + k].z = p[f][k][2]; v[f * 4 + k].nx = n[f][0]; v[f * 4 + k].ny = n[f][1]; v[f * 4 + k].nz = n[f][2]; }
        unsigned short b = (unsigned short)(f * 4), t[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) idx[f * 6 + k] = (unsigned short)(b + t[k]);
    }
    IDirect3DDevice9_SetFVF(dev, FVF_L);
    IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, D3DPT_TRIANGLELIST, 0, 24, 12, idx, D3DFMT_INDEX16, v, sizeof(VtxL));
}
static void t_cube(void) {
    reset_states();
    D3DMATRIX world, view, proj;
    ident(&world); ident(&view); ident(&proj);
    view._43 = 5.0f;                                 // camera at z = -5 looking along +z
    float fovy = 1.0f, zn = 1.0f, zf = 20.0f, h = 1.0f / tanf(fovy * 0.5f);
    proj._11 = h; proj._22 = h; proj._33 = zf / (zf - zn); proj._34 = 1.0f; proj._43 = -zn * zf / (zf - zn); proj._44 = 0;
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &view);
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &proj);
    rs(D3DRS_LIGHTING, TRUE); rs(D3DRS_AMBIENT, 0);
    D3DMATERIAL9 mat;
    memset(&mat, 0, sizeof mat);
    mat.Diffuse.r = 0.5f; mat.Diffuse.g = 0.25f; mat.Diffuse.b = 1.0f; mat.Diffuse.a = 1.0f;
    IDirect3DDevice9_SetMaterial(dev, &mat);
    D3DLIGHT9 lt;
    memset(&lt, 0, sizeof lt);
    lt.Type = D3DLIGHT_DIRECTIONAL; lt.Diffuse.r = lt.Diffuse.g = lt.Diffuse.b = 1.0f; lt.Direction.z = 1.0f;
    IDirect3DDevice9_SetLight(dev, 0, &lt);
    IDirect3DDevice9_LightEnable(dev, 0, TRUE);
    rs(D3DRS_COLORVERTEX, FALSE);
    // default cull mode: counter-clockwise triangles are culled (the faces above are clockwise from outside)
    rs(D3DRS_CULLMODE, D3DCULL_CCW);
    rs(D3DRS_ZENABLE, TRUE);
    clear(0xFF202020);
    draw_cube();
    with_snapshot(v_cube_a);
    // without depth test only culling can hide the back faces (+Z is drawn after -Z)
    rs(D3DRS_ZENABLE, FALSE);
    clear(0xFF202020);
    draw_cube();
    with_snapshot(v_cube_b);
    // no culling, depth test hides them
    rs(D3DRS_ZENABLE, TRUE); rs(D3DRS_CULLMODE, D3DCULL_NONE);
    clear(0xFF202020);
    draw_cube();
    with_snapshot(v_cube_c);
    // a rotated cube lit from the side
    float c = cosf(0.7853982f), s = sinf(0.7853982f);
    world._11 = c; world._13 = -s; world._31 = s; world._33 = c;
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &world);
    lt.Direction.x = -0.70710678f; lt.Direction.z = 0.70710678f;
    IDirect3DDevice9_SetLight(dev, 0, &lt);
    rs(D3DRS_CULLMODE, D3DCULL_CCW);
    clear(0xFF202020);
    draw_cube();
    with_snapshot(v_cube_d);
    rs(D3DRS_LIGHTING, FALSE);
}

static void v_tex_point(void) {
    check_px("tex point texel 0,0", px(8, 8), 0x000080, 0);
    check_px("tex point texel 1,0", px(24, 8), 0x400080, 0);
    check_px("tex point texel 3,3", px(56, 56), 0xC0C080, 0);
    check_px("tex point edge 15", px(15, 8), 0x000080, 0);
    check_px("tex point edge 16", px(16, 8), 0x400080, 0);
    check_px("tex point row 2", px(40, 40), 0x808080, 0);
}
static void v_tex_linear(void) {
    check_px("tex linear left", px(0, 5), 0x000000, 1);
    check_px("tex linear mid 31", px(31, 5), 0x7B7B7B, 3);
    check_px("tex linear mid 32", px(32, 5), 0x838383, 3);
    check_px("tex linear right", px(63, 5), 0xFFFFFF, 1);
}
static void t_texture(void) {
    reset_states();
    unsigned texels[16];
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) texels[y * 4 + x] = 0xFF000080u | ((unsigned)(x * 64) << 16) | ((unsigned)(y * 64) << 8);
    IDirect3DTexture9 *t = make_tex(4, 4, D3DFMT_A8R8G8B8, texels, 4);
    check("texture created", t != NULL);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    clear(0xFF000000);
    // texel (x,y) = R x*64, G y*64, B 0x80; a quad from (-0.5,-0.5) to (63.5,63.5) maps pixel i to texel i / 16
    quad_tex(-0.5f, -0.5f, 63.5f, 63.5f, 0, 0, 1, 1, 0xFFFFFFFF);
    with_snapshot(v_tex_point);
    IDirect3DTexture9_Release(t);
    // linear filtering over a 2x1 texture (black, white)
    unsigned two[2] = { 0xFF000000u, 0xFFFFFFFFu };
    t = make_tex(2, 1, D3DFMT_A8R8G8B8, two, 4);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    clear(0xFF000000);
    quad_tex(-0.5f, -0.5f, 63.5f, 9.5f, 0, 0, 1, 1, 0xFFFFFFFF);
    with_snapshot(v_tex_linear);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DTexture9_Release(t);
}

static void v_dxt(void) {
    check_px("dxt1 row0 red", px(8, 2), 0xFF0000, 0);
    check_px("dxt1 row1 blue", px(8, 6), 0x0000FF, 0);
    check_px("dxt1 row2 2/3", px(8, 10), 0xAA0055, 1);
    check_px("dxt1 row3 1/3", px(8, 14), 0x5500AA, 1);
}
static void v_fmt(void) {
    check_px("fmt r5g6b5", px(2, 2), 0x00FF00, 0);
    check_px("fmt l8", px(6, 2), 0x808080, 0);
    check_px("fmt x1r5g5b5", px(10, 2), 0xFF0000, 0);
    check_px("fmt a8l8", px(14, 2), 0x404040, 0);
    check_px("fmt a8", px(18, 2), 0x000000, 0);
}
static void t_formats(void) {
    reset_states();
    // DXT1: c0 = red, c1 = blue; the four index rows select c0, c1, 2/3 c0 + 1/3 c1 and 1/3 c0 + 2/3 c1
    unsigned char block[8] = { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x55, 0xAA, 0xFF };
    IDirect3DTexture9 *t = NULL;
    HRESULT hr = IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &t, NULL);
    check("dxt1 created", SUCCEEDED(hr));
    if (t) {
        D3DLOCKED_RECT lr;
        IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
        memcpy(lr.pBits, block, 8);
        IDirect3DTexture9_UnlockRect(t, 0);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
        clear(0xFF000000);
        quad_tex(-0.5f, -0.5f, 15.5f, 15.5f, 0, 0, 1, 1, 0xFFFFFFFF);
        with_snapshot(v_dxt);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        IDirect3DTexture9_Release(t);
    }
    // 16-bit and 8-bit formats, one texel each drawn as 4x4 pixel quads
    clear(0xFF000000);
    struct { D3DFORMAT f; unsigned v; int bpp; float x; } fm[5] = {
        { D3DFMT_R5G6B5, 0x07E0, 2, 0 }, { D3DFMT_L8, 0x80, 1, 4 }, { D3DFMT_X1R5G5B5, 0x7C00, 2, 8 }, { D3DFMT_A8L8, 0x8040, 2, 12 }, { D3DFMT_A8, 0x80, 1, 16 },
    };
    for (int i = 0; i < 5; i++) {
        IDirect3DTexture9 *ft = make_tex(1, 1, fm[i].f, &fm[i].v, fm[i].bpp);
        IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)ft);
        quad_tex(fm[i].x - 0.5f, -0.5f, fm[i].x + 3.5f, 3.5f, 0, 0, 1, 1, 0xFFFFFFFF);
        IDirect3DDevice9_SetTexture(dev, 0, NULL);
        if (ft) IDirect3DTexture9_Release(ft);
    }
    with_snapshot(v_fmt);
}

static void v_rtt(void) {
    check_px("rtt sampled green", px(10, 10), 0x00FF00, 0);
    check_px("rtt sampled red", px(40, 40), 0xFF0000, 0);
    check_px("rtt outside", px(80, 80), 0x000000, 0);
}
static void t_render_to_texture(void) {
    reset_states();
    IDirect3DTexture9 *rt = NULL;
    HRESULT hr = IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &rt, NULL);
    check("rt texture created", SUCCEEDED(hr));
    if (!rt) return;
    IDirect3DSurface9 *surf = NULL, *old = NULL;
    IDirect3DTexture9_GetSurfaceLevel(rt, 0, &surf);
    IDirect3DDevice9_GetRenderTarget(dev, 0, &old);
    IDirect3DDevice9_SetRenderTarget(dev, 0, surf);
    D3DVIEWPORT9 vp;
    IDirect3DDevice9_GetViewport(dev, &vp);
    check("viewport follows target", vp.Width == 64 && vp.Height == 64);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0);
    quad_t(32, 32, 64, 64, 0, 0xFFFF0000);
    IDirect3DDevice9_SetRenderTarget(dev, 0, old);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)rt);
    clear(0xFF000000);
    quad_tex(-0.5f, -0.5f, 63.5f, 63.5f, 0, 0, 1, 1, 0xFFFFFFFF);
    with_snapshot(v_rtt);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DSurface9_Release(old);
    IDirect3DSurface9_Release(surf);
    IDirect3DTexture9_Release(rt);
}

static void v_blend2(void) { check_px("blend srcalpha", px(20, 20), 0x80007F, 1); }
static void v_blend3(void) { check_px("blend one/one add", px(20, 20), 0x800000 + 0x0000FF, 1); }
static void t_blend(void) {
    reset_states();
    clear(0xFF0000FF);
    rs(D3DRS_ALPHABLENDENABLE, TRUE); rs(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); rs(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    quad_t(0, 0, 64, 64, 0, 0x80FF0000);
    with_snapshot(v_blend2);
    clear(0xFF0000FF);
    rs(D3DRS_SRCBLEND, D3DBLEND_ONE); rs(D3DRS_DESTBLEND, D3DBLEND_ONE);
    quad_t(0, 0, 64, 64, 0, 0xFF800000);
    with_snapshot(v_blend3);
    rs(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void v_atest(void) {
    check_px("alpha test reject", px(10, 10), 0x000000, 0);
    check_px("alpha test accept", px(70, 10), 0x00FF00, 0);
}
static void t_alphatest(void) {
    reset_states();
    clear(0xFF000000);
    rs(D3DRS_ALPHATESTENABLE, TRUE); rs(D3DRS_ALPHAFUNC, D3DCMP_GREATER); rs(D3DRS_ALPHAREF, 0x80);
    quad_t(0, 0, 32, 32, 0, 0x40FF0000);
    quad_t(64, 0, 96, 32, 0, 0xC000FF00);
    with_snapshot(v_atest);
}

static void v_stencil(void) {
    check_px("stencil marked", px(50, 50), 0x0000FF, 0);
    check_px("stencil unmarked", px(20, 20), 0x000000, 0);
    check_px("stencil unmarked 2", px(100, 100), 0x000000, 0);
}
static void t_stencil(void) {
    reset_states();
    clear(0xFF000000);
    rs(D3DRS_STENCILENABLE, TRUE); rs(D3DRS_STENCILFUNC, D3DCMP_ALWAYS); rs(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE); rs(D3DRS_STENCILREF, 1);
    rs(D3DRS_COLORWRITEENABLE, 0);
    quad_t(40, 40, 80, 80, 0, 0xFFFFFFFF);
    rs(D3DRS_COLORWRITEENABLE, 0xF);
    rs(D3DRS_STENCILFUNC, D3DCMP_EQUAL); rs(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
    quad_t(0, 0, 128, 128, 0, 0xFF0000FF);
    with_snapshot(v_stencil);
    rs(D3DRS_STENCILENABLE, FALSE);
}

static void v_scissor(void) {
    check_px("scissor inside", px(30, 30), 0xFFFFFF, 0);
    check("scissor edges", px(20, 20) == 0xFFFFFF && px(39, 39) == 0xFFFFFF && px(19, 30) == 0 && px(40, 30) == 0 && px(30, 19) == 0 && px(30, 40) == 0);
}
static void v_scissor_clear(void) { check("scissor clear", px(30, 30) == 0x00FF00 && px(10, 10) == 0xFFFFFF && px(45, 45) == 0xFFFFFF); }
static void t_scissor(void) {
    reset_states();
    clear(0xFF000000);
    RECT sc = { 20, 20, 40, 40 };
    IDirect3DDevice9_SetScissorRect(dev, &sc);
    rs(D3DRS_SCISSORTESTENABLE, TRUE);
    quad_t(0, 0, 128, 128, 0, 0xFFFFFFFF);
    with_snapshot(v_scissor);
    rs(D3DRS_SCISSORTESTENABLE, FALSE);
    clear(0xFFFFFFFF);
    rs(D3DRS_SCISSORTESTENABLE, TRUE);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF00FF00, 1.0f, 0);
    with_snapshot(v_scissor_clear);
    rs(D3DRS_SCISSORTESTENABLE, FALSE);
}

static void v_stages(void) { check_px("two stages modulate+add", px(20, 20), 0x906030, 2); }
static void t_stages(void) {
    reset_states();
    unsigned grey = 0xFF808080u;
    IDirect3DTexture9 *t = make_tex(1, 1, D3DFMT_A8R8G8B8, &grey, 4);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_ADD);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
    rs(D3DRS_TEXTUREFACTOR, 0xFF102030);
    clear(0xFF000000);
    quad_tex(0, 0, 64, 64, 0, 0, 1, 1, 0xFFFF8000);
    with_snapshot(v_stages);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DTexture9_Release(t);
}

static void v_fog(void) { check_px("vertex fog linear", px(64, 64), 0x808080, 3); check_px("vertex fog none at z=0", px(8, 8), 0xFFFFFF, 3); }
static void t_fog(void) {
    reset_states();
    clear(0xFF000000);
    rs(D3DRS_FOGENABLE, TRUE); rs(D3DRS_FOGCOLOR, 0x00000000); rs(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    rs(D3DRS_FOGSTART, fbits(0.0f)); rs(D3DRS_FOGEND, fbits(1.0f));
    // identity transforms: view-space depth = z; a quad at z = 0.5 is half fogged, one at z = 0 not at all
    VtxD a[4] = { { -0.5f, 0.5f, 0.5f, 0xFFFFFFFF }, { 0.5f, 0.5f, 0.5f, 0xFFFFFFFF }, { -0.5f, -0.5f, 0.5f, 0xFFFFFFFF }, { 0.5f, -0.5f, 0.5f, 0xFFFFFFFF } };
    VtxD b[4] = { { -1.0f, 1.0f, 0.0f, 0xFFFFFFFF }, { -0.8f, 1.0f, 0.0f, 0xFFFFFFFF }, { -1.0f, 0.8f, 0.0f, 0xFFFFFFFF }, { -0.8f, 0.8f, 0.0f, 0xFFFFFFFF } };
    IDirect3DDevice9_SetFVF(dev, FVF_D);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, a, sizeof(VtxD));
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, b, sizeof(VtxD));
    with_snapshot(v_fog);
    rs(D3DRS_FOGENABLE, FALSE);
}

static void v_points(void) {
    check("point size 5", px(62, 64) == 0xFFFFFF && px(66, 64) == 0xFFFFFF && px(64, 62) == 0xFFFFFF && px(64, 66) == 0xFFFFFF);
    check("point size outside", px(61, 64) == 0 && px(67, 64) == 0 && px(64, 61) == 0 && px(64, 67) == 0);
    check("point size 1", px(20, 20) == 0xFFFFFF && px(21, 20) == 0 && px(19, 20) == 0);
}
static void t_points(void) {
    reset_states();
    clear(0xFF000000);
    VtxT p = { 64, 64, 0, 1, 0xFFFFFFFF };
    rs(D3DRS_POINTSIZE, fbits(5.0f));
    IDirect3DDevice9_SetFVF(dev, FVF_T);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_POINTLIST, 1, &p, sizeof p);
    rs(D3DRS_POINTSIZE, fbits(1.0f));
    VtxT q = { 20, 20, 0, 1, 0xFFFFFFFF };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_POINTLIST, 1, &q, sizeof q);
    with_snapshot(v_points);
}

static void v_indexed(void) {
    check_px("indexed16 base vertex", px(20, 20), 0xFF0000, 0);
    check_px("indexed32", px(100, 20), 0x00FF00, 0);
    check("indexed no garbage", px(5, 5) == 0 && px(64, 100) == 0);
}
static void t_indexed(void) {
    reset_states();
    clear(0xFF000000);
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DDevice9_CreateVertexBuffer(dev, 12 * sizeof(VtxT), 0, FVF_T, D3DPOOL_DEFAULT, &vb, NULL);
    VtxT *v;
    IDirect3DVertexBuffer9_Lock(vb, 0, 0, (void **)&v, 0);
    for (int i = 0; i < 4; i++) { v[i].x = 64; v[i].y = 100; v[i].z = 0; v[i].rhw = 1; v[i].color = 0xFFFF00FF; }   // garbage at the base
    VtxT q1[4] = { { 10, 10, 0, 1, 0xFFFF0000 }, { 30, 10, 0, 1, 0xFFFF0000 }, { 10, 30, 0, 1, 0xFFFF0000 }, { 30, 30, 0, 1, 0xFFFF0000 } };
    VtxT q2[4] = { { 90, 10, 0, 1, 0xFF00FF00 }, { 110, 10, 0, 1, 0xFF00FF00 }, { 90, 30, 0, 1, 0xFF00FF00 }, { 110, 30, 0, 1, 0xFF00FF00 } };
    memcpy(v + 4, q1, sizeof q1);
    memcpy(v + 8, q2, sizeof q2);
    IDirect3DVertexBuffer9_Unlock(vb);
    IDirect3DIndexBuffer9 *ib16 = NULL, *ib32 = NULL;
    IDirect3DDevice9_CreateIndexBuffer(dev, 6 * 2, 0, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib16, NULL);
    IDirect3DDevice9_CreateIndexBuffer(dev, 6 * 4, 0, D3DFMT_INDEX32, D3DPOOL_DEFAULT, &ib32, NULL);
    unsigned short *i16; unsigned *i32;
    IDirect3DIndexBuffer9_Lock(ib16, 0, 0, (void **)&i16, 0);
    static const unsigned short ix16[6] = { 0, 1, 2, 2, 1, 3 };
    memcpy(i16, ix16, sizeof ix16);
    IDirect3DIndexBuffer9_Unlock(ib16);
    IDirect3DIndexBuffer9_Lock(ib32, 0, 0, (void **)&i32, 0);
    static const unsigned ix32[6] = { 0, 1, 2, 2, 1, 3 };
    memcpy(i32, ix32, sizeof ix32);
    IDirect3DIndexBuffer9_Unlock(ib32);
    IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, sizeof(VtxT));
    IDirect3DDevice9_SetFVF(dev, FVF_T);
    IDirect3DDevice9_SetIndices(dev, ib16);
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 4, 0, 4, 0, 2);
    IDirect3DDevice9_SetIndices(dev, ib32);
    IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 8, 0, 4, 0, 2);
    with_snapshot(v_indexed);
    // reference counts like Windows: the device holds what is bound
    IDirect3DDevice9_SetStreamSource(dev, 0, NULL, 0, 0);
    IDirect3DDevice9_SetIndices(dev, NULL);
    check("vb refcount", IDirect3DVertexBuffer9_Release(vb) == 0);
    check("ib refcount", IDirect3DIndexBuffer9_Release(ib16) == 0 && IDirect3DIndexBuffer9_Release(ib32) == 0);
}

static void v_up(void) {
    check_px("drawprimitiveup strip", px(20, 100), 0x0000FF, 0);
    check_px("drawprimitiveup fan", px(112, 100), 0xFFFF00, 0);
    check_px("drawprimitiveup lines", px(64, 64), 0xFFFFFF, 0);
}
static void t_up(void) {
    reset_states();
    clear(0xFF000000);
    VtxT s[4] = { { 10, 90, 0, 1, 0xFF0000FF }, { 40, 90, 0, 1, 0xFF0000FF }, { 10, 120, 0, 1, 0xFF0000FF }, { 40, 120, 0, 1, 0xFF0000FF } };
    IDirect3DDevice9_SetFVF(dev, FVF_T);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, s, sizeof(VtxT));
    VtxT f[4] = { { 100, 100, 0, 1, 0xFFFFFF00 }, { 80, 85, 0, 1, 0xFFFFFF00 }, { 120, 85, 0, 1, 0xFFFFFF00 }, { 120, 120, 0, 1, 0xFFFFFF00 } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLEFAN, 2, f, sizeof(VtxT));
    VtxT l[3] = { { 50, 64, 0, 1, 0xFFFFFFFF }, { 78, 64, 0, 1, 0xFFFFFFFF }, { 78, 64, 0, 1, 0xFFFFFFFF } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_LINESTRIP, 2, l, sizeof(VtxT));
    with_snapshot(v_up);
}

static void v_decl(void) {
    check_px("vertex declaration quad", px(40, 40), 0x336699, 3);
    check_px("vertex declaration clip-space", px(64, 64), 0x336699, 3);
}
static void t_decl(void) {
    reset_states();
    clear(0xFF000000);
    // float3 position + D3DCOLOR, drawn through the fixed pipeline with a declaration
    static const D3DVERTEXELEMENT9 el[] = { { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                                            { 0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    IDirect3DVertexDeclaration9 *vd = NULL;
    HRESULT hr = IDirect3DDevice9_CreateVertexDeclaration(dev, el, &vd);
    check("vertex declaration created", SUCCEEDED(hr));
    if (!vd) return;
    IDirect3DDevice9_SetVertexDeclaration(dev, vd);
    VtxD q[4] = { { -0.5f, 0.5f, 0.2f, 0xFF336699 }, { 0.5f, 0.5f, 0.2f, 0xFF336699 }, { -0.5f, -0.5f, 0.2f, 0xFF336699 }, { 0.5f, -0.5f, 0.2f, 0xFF336699 } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VtxD));
    with_snapshot(v_decl);
    IDirect3DDevice9_SetVertexDeclaration(dev, NULL);
    check("vertex declaration refcount", IDirect3DVertexDeclaration9_Release(vd) == 0);
}

static void v_cubetex(void) {
    check_px("cube map +x", px(8, 8), 0xFF0000, 0);
    check_px("cube map -y", px(24, 8), 0x00FFFF, 0);
    check_px("cube map +z", px(40, 8), 0x0000FF, 0);
}
static void t_cube_map(void) {
    reset_states();
    IDirect3DCubeTexture9 *ct = NULL;
    HRESULT hr = IDirect3DDevice9_CreateCubeTexture(dev, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &ct, NULL);
    check("cube texture created", SUCCEEDED(hr));
    if (!ct) return;
    static const unsigned col[6] = { 0xFFFF0000u, 0xFF00FF00u, 0xFFFFFF00u, 0xFF00FFFFu, 0xFF0000FFu, 0xFFFF00FFu };
    for (int f = 0; f < 6; f++) {
        D3DLOCKED_RECT lr;
        IDirect3DCubeTexture9_LockRect(ct, (D3DCUBEMAP_FACES)f, 0, &lr, NULL, 0);
        for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) ((unsigned *)((char *)lr.pBits + y * lr.Pitch))[x] = col[f];
        IDirect3DCubeTexture9_UnlockRect(ct, (D3DCUBEMAP_FACES)f, 0);
    }
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)ct);
    clear(0xFF000000);
    typedef struct { float x, y, z, rhw; float tx, ty, tz; } VC;
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
    float dirs[3][3] = { { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, 1 } };
    for (int i = 0; i < 3; i++) {
        float x0 = 16.0f * (float)i, x1 = x0 + 15.0f;
        VC v[4] = { { x0, 0, 0, 1, dirs[i][0], dirs[i][1], dirs[i][2] }, { x1, 0, 0, 1, dirs[i][0], dirs[i][1], dirs[i][2] },
                    { x0, 15, 0, 1, dirs[i][0], dirs[i][1], dirs[i][2] }, { x1, 15, 0, 1, dirs[i][0], dirs[i][1], dirs[i][2] } };
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, v, sizeof(VC));
    }
    with_snapshot(v_cubetex);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DCubeTexture9_Release(ct);
}

static void v_stretch(void) {
    check("stretchrect", px(15, 15) == 0xFF0000 && px(10, 10) == 0xFF0000 && px(19, 19) == 0xFF0000 && px(20, 15) == 0x101010 && px(9, 15) == 0x101010);
}
static void v_dc(void) { check_px("surface GetDC / GDI", px(60, 60), 0x0000FF, 0); }
static void t_surfaces(void) {
    reset_states();
    clear(0xFF101010);
    IDirect3DSurface9 *s = NULL;
    HRESULT hr = IDirect3DDevice9_CreateRenderTarget(dev, 2, 2, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s, NULL);
    check("render target surface", SUCCEEDED(hr));
    IDirect3DDevice9_ColorFill(dev, s, NULL, 0xFFFF0000);
    IDirect3DSurface9 *bb = backbuffer();
    RECT dr = { 10, 10, 20, 20 };
    hr = IDirect3DDevice9_StretchRect(dev, s, NULL, bb, &dr, D3DTEXF_POINT);
    check("stretchrect hr", SUCCEEDED(hr));
    with_snapshot(v_stretch);
    IDirect3DSurface9_Release(s);
    // GDI drawing into a surface
    IDirect3DSurface9 *os = NULL;
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 32, 32, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &os, NULL);
    IDirect3DDevice9_ColorFill(dev, os, NULL, 0xFF00FF00);
    HDC dc = NULL;
    hr = IDirect3DSurface9_GetDC(os, &dc);
    check("surface GetDC", SUCCEEDED(hr) && dc != NULL);
    if (dc) {
        HBRUSH br = CreateSolidBrush(RGB(0, 0, 255));
        RECT r = { 4, 4, 28, 28 };
        FillRect(dc, &r, br);
        DeleteObject(br);
        IDirect3DSurface9_ReleaseDC(os, dc);
    }
    RECT sr = { 0, 0, 32, 32 };
    POINT pt = { 44, 44 };
    IDirect3DSurface9 *sys2 = NULL;
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 32, 32, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys2, NULL);
    check("render target data copy", SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(dev, os, sys2)));
    hr = IDirect3DDevice9_UpdateSurface(dev, sys2, &sr, bb, &pt);
    check("update surface", SUCCEEDED(hr));
    with_snapshot(v_dc);
    IDirect3DSurface9_Release(sys2);
    IDirect3DSurface9_Release(os);
    // reference counting of device-held surfaces
    IDirect3DSurface9 *a = NULL, *b = NULL;
    IDirect3DDevice9_GetRenderTarget(dev, 0, &a);
    check("backbuffer is render target", a == bb);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &b);
    check("depth stencil exists", b != NULL);
    D3DSURFACE_DESC d;
    IDirect3DSurface9_GetDesc(b, &d);
    check("depth desc", d.Format == D3DFMT_D24S8 && d.Width == 128 && d.Height == 128 && (d.Usage & D3DUSAGE_DEPTHSTENCIL));
    IDirect3DSurface9_Release(b);
    IDirect3DSurface9_Release(a);
    IDirect3DSurface9_Release(bb);
}

static void t_refcounts(void) {
    IDirect3DTexture9 *t = NULL;
    IDirect3DDevice9_CreateTexture(dev, 8, 8, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL);
    check("texture levels", IDirect3DTexture9_GetLevelCount(t) == 4);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    ULONG r = IDirect3DTexture9_AddRef(t);
    check("texture bound ref", r == 3);
    IDirect3DTexture9_Release(t);
    IDirect3DBaseTexture9 *got = NULL;
    IDirect3DDevice9_GetTexture(dev, 0, &got);
    check("GetTexture identity", got == (IDirect3DBaseTexture9 *)t);
    IDirect3DBaseTexture9_Release(got);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    check("texture unbound ref", IDirect3DTexture9_Release(t) == 0);
    // state block
    reset_states();
    rs(D3DRS_ZENABLE, TRUE);
    IDirect3DStateBlock9 *sb = NULL;
    IDirect3DDevice9_BeginStateBlock(dev);
    rs(D3DRS_ZENABLE, FALSE);
    rs(D3DRS_CULLMODE, D3DCULL_CW);
    IDirect3DDevice9_EndStateBlock(dev, &sb);
    DWORD v = 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &v);
    check("stateblock recording does not apply", v == TRUE);
    IDirect3DStateBlock9_Apply(sb);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &v);
    DWORD c = 0;
    IDirect3DDevice9_GetRenderState(dev, D3DRS_CULLMODE, &c);
    check("stateblock apply", v == FALSE && c == D3DCULL_CW);
    rs(D3DRS_ZENABLE, TRUE);
    IDirect3DStateBlock9_Capture(sb);
    rs(D3DRS_ZENABLE, FALSE);
    IDirect3DStateBlock9_Apply(sb);
    IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &v);
    check("stateblock capture", v == TRUE);
    IDirect3DStateBlock9_Release(sb);
    // queries
    IDirect3DQuery9 *q = NULL;
    check("event query", SUCCEEDED(IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_EVENT, &q)) && q);
    if (q) { BOOL done = FALSE; IDirect3DQuery9_Issue(q, D3DISSUE_END); check("event signalled", IDirect3DQuery9_GetData(q, &done, sizeof done, D3DGETDATA_FLUSH) == S_OK && done); IDirect3DQuery9_Release(q); }
    check("unsupported query", IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_VERTEXSTATS, NULL) == D3DERR_NOTAVAILABLE);
    check("cooperative level", IDirect3DDevice9_TestCooperativeLevel(dev) == D3D_OK);
    check("scene state", SUCCEEDED(IDirect3DDevice9_BeginScene(dev)) && SUCCEEDED(IDirect3DDevice9_EndScene(dev)) && FAILED(IDirect3DDevice9_EndScene(dev)));
}


// ---- more fixed-function features ----------------------------------------------------------------------------------------------------
static void v_xform(void) {
    check_px("texture transform scroll left", px(4, 4), 0x0000FF, 0);
    check_px("texture transform scroll right", px(28, 4), 0xFF0000, 0);
}
static void t_texxform(void) {
    reset_states();
    unsigned two[2] = { 0xFFFF0000u, 0xFF0000FFu };
    IDirect3DTexture9 *t = make_tex(2, 1, D3DFMT_A8R8G8B8, two, 4);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    D3DMATRIX m; ident(&m);
    m._31 = 0.5f;                       // COUNT2: the third row translates (u, v, 1)
    IDirect3DDevice9_SetTransform(dev, D3DTS_TEXTURE0, &m);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    clear(0xFF000000);
    quad_tex(-0.5f, -0.5f, 31.5f, 7.5f, 0, 0, 1, 1, 0xFFFFFFFF);   // without the transform: red | blue
    with_snapshot(v_xform);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DTexture9_Release(t);
}

static void v_wire(void) {
    check("wireframe", px(20, 20) == 0xFFFFFF && px(60, 60) == 0 && px(40, 20) == 0xFFFFFF && px(25, 60) == 0);
}
static void v_flat(void) { check_px("flat shading", px(30, 50), 0xFF0000, 0); }
static void v_gouraud(void) { check("gouraud differs from flat", px(30, 50) != 0xFF0000); }
static void t_fill_shade(void) {
    reset_states();
    clear(0xFF000000);
    rs(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    VtxT tri[3] = { { 20, 20, 0, 1, 0xFFFFFFFF }, { 80, 20, 0, 1, 0xFFFFFFFF }, { 20, 80, 0, 1, 0xFFFFFFFF } };
    IDirect3DDevice9_SetFVF(dev, FVF_T);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof(VtxT));
    with_snapshot(v_wire);
    rs(D3DRS_FILLMODE, D3DFILL_SOLID);
    VtxT c[3] = { { 10, 10, 0, 1, 0xFFFF0000 }, { 90, 90, 0, 1, 0xFF00FF00 }, { 10, 90, 0, 1, 0xFF0000FF } };
    clear(0xFF000000);
    rs(D3DRS_SHADEMODE, D3DSHADE_FLAT);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, c, sizeof(VtxT));
    with_snapshot(v_flat);
    clear(0xFF000000);
    rs(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, c, sizeof(VtxT));
    with_snapshot(v_gouraud);
}

static void v_clipplane(void) { check("clip plane", px(20, 64) == 0 && px(100, 64) == 0xFFFFFF); }
static void t_clipplane(void) {
    reset_states();
    clear(0xFF000000);
    float plane[4] = { 1, 0, 0, 0 };    // keeps x >= 0
    IDirect3DDevice9_SetClipPlane(dev, 0, plane);
    rs(D3DRS_CLIPPLANEENABLE, 1);
    VtxD q[4] = { { -0.9f, 0.9f, 0.3f, 0xFFFFFFFF }, { 0.9f, 0.9f, 0.3f, 0xFFFFFFFF }, { -0.9f, -0.9f, 0.3f, 0xFFFFFFFF }, { 0.9f, -0.9f, 0.3f, 0xFFFFFFFF } };
    IDirect3DDevice9_SetFVF(dev, FVF_D);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VtxD));
    with_snapshot(v_clipplane);
    rs(D3DRS_CLIPPLANEENABLE, 0);
}

static void v_spec(void) { check_px("specular highlight", px(64, 64), 0xFFFFFF, 2); }
static void v_point_light(void) { check_px("point light attenuation", px(64, 64), 0x555555, 3); }
static void v_spot(void) { check_px("spot light inside inner cone", px(64, 64), 0xF0F0F0, 3); }
static void v_spot2(void) { check_px("spot light outside outer cone", px(64, 64), 0x000000, 0); }
static void t_lights(void) {
    reset_states();
    rs(D3DRS_LIGHTING, TRUE); rs(D3DRS_AMBIENT, 0); rs(D3DRS_COLORVERTEX, FALSE);
    typedef struct { float x, y, z, nx, ny, nz; } VL;
    VL q[4] = { { -1, 1, 0, 0, 0, -1 }, { 1, 1, 0, 0, 0, -1 }, { -1, -1, 0, 0, 0, -1 }, { 1, -1, 0, 0, 0, -1 } };
    D3DMATRIX view; ident(&view);
    view._43 = 0;   // the camera sits at the origin looking along +z; the quad is moved forward with the world matrix
    D3DMATRIX world; ident(&world); world._43 = 2.0f;
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &world);
    D3DMATRIX proj; ident(&proj);
    proj._11 = 1.0f / 4.0f; proj._22 = 1.0f / 4.0f; proj._33 = 1.0f / 100.0f; proj._44 = 1.0f;     // orthographic, z/100
    IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &proj);
    IDirect3DDevice9_SetFVF(dev, FVF_L);
    // specular: material diffuse black, specular white; directional light along +z towards the surface, viewer straight on
    D3DMATERIAL9 mat; memset(&mat, 0, sizeof mat);
    mat.Specular.r = mat.Specular.g = mat.Specular.b = 1; mat.Specular.a = 1; mat.Power = 10;
    IDirect3DDevice9_SetMaterial(dev, &mat);
    D3DLIGHT9 lt; memset(&lt, 0, sizeof lt);
    lt.Type = D3DLIGHT_DIRECTIONAL; lt.Diffuse.r = lt.Diffuse.g = lt.Diffuse.b = 1; lt.Specular.r = lt.Specular.g = lt.Specular.b = 1; lt.Direction.z = 1;
    IDirect3DDevice9_SetLight(dev, 0, &lt);
    IDirect3DDevice9_LightEnable(dev, 0, TRUE);
    rs(D3DRS_SPECULARENABLE, TRUE); rs(D3DRS_LOCALVIEWER, FALSE);
    clear(0xFF000000);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VL));
    with_snapshot(v_spec);
    // point light 2 units in front of the surface: per-vertex colour = (n.l = 2/d) * (1/d) with d = sqrt(6) at the corners
    rs(D3DRS_SPECULARENABLE, FALSE);
    mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = 1; mat.Diffuse.a = 1;
    IDirect3DDevice9_SetMaterial(dev, &mat);
    memset(&lt, 0, sizeof lt);
    lt.Type = D3DLIGHT_POINT; lt.Diffuse.r = lt.Diffuse.g = lt.Diffuse.b = 1; lt.Position.z = 0; lt.Range = 100; lt.Attenuation1 = 1.0f;
    IDirect3DDevice9_SetLight(dev, 0, &lt);
    clear(0xFF000000);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VL));
    with_snapshot(v_point_light);
    // spot light 4 units from the surface; lighting is per vertex (the corners are 0.34 rad off the axis)
    memset(&lt, 0, sizeof lt);
    lt.Type = D3DLIGHT_SPOT; lt.Diffuse.r = lt.Diffuse.g = lt.Diffuse.b = 1; lt.Position.z = -2.0f; lt.Direction.z = 1; lt.Range = 100;
    lt.Attenuation0 = 1; lt.Theta = 1.0f; lt.Phi = 1.2f; lt.Falloff = 1;
    IDirect3DDevice9_SetLight(dev, 0, &lt);
    clear(0xFF000000);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VL));
    with_snapshot(v_spot);
    lt.Theta = 0.1f; lt.Phi = 0.2f;
    IDirect3DDevice9_SetLight(dev, 0, &lt);
    clear(0xFF000000);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VL));
    with_snapshot(v_spot2);
    rs(D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_LightEnable(dev, 0, FALSE);
}

static void v_inst(void) { check_px("instancing", px(32, 32), 0xFFFF00, 0); }
static void t_instancing(void) {
    reset_states();
    clear(0xFF000000);
    typedef struct { float x, y, z, rhw; } P;
    P quad[4] = { { 10, 10, 0, 1 }, { 60, 10, 0, 1 }, { 10, 60, 0, 1 }, { 60, 60, 0, 1 } };
    DWORD cols[2] = { 0xFFFF0000, 0xFF00FF00 };
    IDirect3DVertexBuffer9 *vb0 = NULL, *vb1 = NULL;
    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof quad, 0, 0, D3DPOOL_DEFAULT, &vb0, NULL);
    IDirect3DDevice9_CreateVertexBuffer(dev, sizeof cols, 0, 0, D3DPOOL_DEFAULT, &vb1, NULL);
    void *p;
    IDirect3DVertexBuffer9_Lock(vb0, 0, 0, &p, 0); memcpy(p, quad, sizeof quad); IDirect3DVertexBuffer9_Unlock(vb0);
    IDirect3DVertexBuffer9_Lock(vb1, 0, 0, &p, 0); memcpy(p, cols, sizeof cols); IDirect3DVertexBuffer9_Unlock(vb1);
    static const D3DVERTEXELEMENT9 el[] = { { 0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITIONT, 0 }, { 1, 0, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    IDirect3DVertexDeclaration9 *vd = NULL;
    IDirect3DDevice9_CreateVertexDeclaration(dev, el, &vd);
    IDirect3DDevice9_SetVertexDeclaration(dev, vd);
    IDirect3DDevice9_SetStreamSource(dev, 0, vb0, 0, sizeof(P));
    IDirect3DDevice9_SetStreamSource(dev, 1, vb1, 0, 4);
    IDirect3DDevice9_SetStreamSourceFreq(dev, 0, D3DSTREAMSOURCE_INDEXEDDATA | 2);
    IDirect3DDevice9_SetStreamSourceFreq(dev, 1, D3DSTREAMSOURCE_INSTANCEDATA | 1);
    rs(D3DRS_ALPHABLENDENABLE, TRUE); rs(D3DRS_SRCBLEND, D3DBLEND_ONE); rs(D3DRS_DESTBLEND, D3DBLEND_ONE);
    IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2);
    with_snapshot(v_inst);
    IDirect3DDevice9_SetStreamSourceFreq(dev, 0, 1);
    IDirect3DDevice9_SetStreamSourceFreq(dev, 1, 1);
    IDirect3DDevice9_SetStreamSource(dev, 0, NULL, 0, 0);
    IDirect3DDevice9_SetStreamSource(dev, 1, NULL, 0, 0);
    IDirect3DDevice9_SetVertexDeclaration(dev, NULL);
    IDirect3DVertexDeclaration9_Release(vd);
    IDirect3DVertexBuffer9_Release(vb0);
    IDirect3DVertexBuffer9_Release(vb1);
}

static void v_mip(void) {
    check_px("mip level 0", px(4, 4), 0xFF0000, 0);
    check_px("mip level selected by minification", px(40, 4), 0x0000FF, 0);
}
static void t_mips(void) {
    reset_states();
    IDirect3DTexture9 *t = NULL;
    IDirect3DDevice9_CreateTexture(dev, 4, 4, 3, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, NULL);
    static const unsigned col[3] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu };
    for (int l = 0; l < 3; l++) {
        D3DLOCKED_RECT lr;
        int n = 4 >> l;
        IDirect3DTexture9_LockRect(t, l, &lr, NULL, 0);
        for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) ((unsigned *)((char *)lr.pBits + y * lr.Pitch))[x] = col[l];
        IDirect3DTexture9_UnlockRect(t, l);
    }
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
    clear(0xFF000000);
    quad_tex(-0.5f, -0.5f, 7.5f, 7.5f, 0, 0, 2, 2, 0xFFFFFFFF);   // 8x8 pixels show 2x2 repetitions: 1 texel = 2 pixels -> level 0... 
    quad_tex(31.5f, -0.5f, 47.5f, 15.5f, 0, 0, 16, 16, 0xFFFFFFFF); // 16 pixels cover 64 texels: level 2
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    with_snapshot(v_mip);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DTexture9_Release(t);
}

static void t_swapchains(void) {
    reset_states();
    check("swap chain count", IDirect3DDevice9_GetNumberOfSwapChains(dev) == 1);
    IDirect3DSwapChain9 *sc = NULL;
    HRESULT hr = IDirect3DDevice9_GetSwapChain(dev, 0, &sc);
    D3DPRESENT_PARAMETERS pp;
    check("get swap chain", SUCCEEDED(hr) && sc);
    IDirect3DSwapChain9_GetPresentParameters(sc, &pp);
    check("present parameters", pp.BackBufferWidth == 128 && pp.BackBufferHeight == 128 && pp.BackBufferFormat == D3DFMT_X8R8G8B8 && pp.Windowed);
    IDirect3DSurface9 *b1 = NULL, *b2 = NULL;
    IDirect3DSwapChain9_GetBackBuffer(sc, 0, D3DBACKBUFFER_TYPE_MONO, &b1);
    IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &b2);
    check("swap chain back buffer identity", b1 == b2);
    IDirect3DSurface9_Release(b1); IDirect3DSurface9_Release(b2);
    check("swap chain present", IDirect3DSwapChain9_Present(sc, NULL, NULL, NULL, NULL, 0) == D3D_OK);
    IDirect3DSwapChain9_Release(sc);
    // additional swap chain
    D3DPRESENT_PARAMETERS p2;
    memset(&p2, 0, sizeof p2);
    p2.BackBufferWidth = 64; p2.BackBufferHeight = 32; p2.BackBufferFormat = D3DFMT_X8R8G8B8; p2.BackBufferCount = 1;
    p2.SwapEffect = D3DSWAPEFFECT_DISCARD; p2.hDeviceWindow = wnd; p2.Windowed = TRUE;
    IDirect3DSwapChain9 *a = NULL;
    hr = IDirect3DDevice9_CreateAdditionalSwapChain(dev, &p2, &a);
    check("additional swap chain", SUCCEEDED(hr) && a && IDirect3DDevice9_GetNumberOfSwapChains(dev) == 2);
    if (a) {
        IDirect3DSurface9 *ab = NULL;
        IDirect3DSwapChain9_GetBackBuffer(a, 0, D3DBACKBUFFER_TYPE_MONO, &ab);
        D3DSURFACE_DESC d;
        IDirect3DSurface9_GetDesc(ab, &d);
        check("additional back buffer size", d.Width == 64 && d.Height == 32);
        IDirect3DDevice9_ColorFill(dev, ab, NULL, 0xFF123456);
        check("additional present", IDirect3DSwapChain9_Present(a, NULL, NULL, NULL, NULL, 0) == D3D_OK);
        IDirect3DSurface9_Release(ab);
        IDirect3DSwapChain9_Release(a);
    }
}

static void t_reset(void) {
    reset_states();
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 64; pp.BackBufferHeight = 48; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D16;
    HRESULT hr = IDirect3DDevice9_Reset(dev, &pp);
    check("reset", SUCCEEDED(hr));
    IDirect3DSurface9 *bb = backbuffer(), *ds = NULL;
    D3DSURFACE_DESC d;
    IDirect3DSurface9_GetDesc(bb, &d);
    check("reset back buffer", d.Width == 64 && d.Height == 48);
    IDirect3DDevice9_GetDepthStencilSurface(dev, &ds);
    IDirect3DSurface9_GetDesc(ds, &d);
    check("reset depth buffer", d.Format == D3DFMT_D16 && d.Width == 64);
    D3DVIEWPORT9 vp;
    IDirect3DDevice9_GetViewport(dev, &vp);
    check("reset viewport", vp.Width == 64 && vp.Height == 48);
    IDirect3DSurface9_Release(ds);
    IDirect3DSurface9_Release(bb);
    reset_states();
    clear(0xFF112233);
    check("draw after reset", SUCCEEDED(IDirect3DDevice9_BeginScene(dev)) && SUCCEEDED(IDirect3DDevice9_EndScene(dev)) && IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL) == D3D_OK);
    pp.BackBufferWidth = 128; pp.BackBufferHeight = 128; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    check("reset back", SUCCEEDED(IDirect3DDevice9_Reset(dev, &pp)));
}

// ---- fog variants, culling, volume / palette / auto-mip textures, occlusion ---------------------------------------------------------------------
static void v_tfog(void) { check_px("table fog linear (z = 0.5)", px(20, 20), 0x808080, 3); check_px("table fog with specular-alpha vertex fog", px(100, 20), 0x404040, 3); }
static void t_fog2(void) {
    reset_states();
    clear(0xFF000000);
    rs(D3DRS_FOGENABLE, TRUE); rs(D3DRS_FOGCOLOR, 0); rs(D3DRS_FOGSTART, fbits(0.0f)); rs(D3DRS_FOGEND, fbits(1.0f));
    rs(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
    quad_t(10, 10, 50, 50, 0.5f, 0xFFFFFFFF);
    rs(D3DRS_FOGTABLEMODE, D3DFOG_NONE); rs(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
    typedef struct { float x, y, z, rhw; DWORD c, spec; } VS_;
    VS_ q[4] = { { 90, 10, 0, 1, 0xFFFFFFFF, 0x40000000 }, { 110, 10, 0, 1, 0xFFFFFFFF, 0x40000000 }, { 90, 30, 0, 1, 0xFFFFFFFF, 0x40000000 }, { 110, 30, 0, 1, 0xFFFFFFFF, 0x40000000 } };
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VS_));
    with_snapshot(v_tfog);
    rs(D3DRS_FOGENABLE, FALSE);
}

static void v_cull(void) { check("cull modes", px(10, 10) == 0xFF0000 && px(40, 10) == 0 && px(10, 50) == 0 && px(40, 50) == 0x00FF00); }
static void t_cull(void) {
    reset_states();
    clear(0xFF000000);
    // clockwise on screen (y down): (0,0) -> (20,0) -> (0,20)
    VtxT cw[3] = { { 5, 5, 0, 1, 0xFFFF0000 }, { 25, 5, 0, 1, 0xFFFF0000 }, { 5, 25, 0, 1, 0xFFFF0000 } };
    IDirect3DDevice9_SetFVF(dev, FVF_T);
    rs(D3DRS_CULLMODE, D3DCULL_CCW);                       // default: counter-clockwise triangles are culled
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, cw, sizeof(VtxT));
    VtxT cw2[3] = { { 35, 5, 0, 1, 0xFF0000FF }, { 55, 5, 0, 1, 0xFF0000FF }, { 35, 25, 0, 1, 0xFF0000FF } };
    rs(D3DRS_CULLMODE, D3DCULL_CW);                        // clockwise triangles culled: this one disappears
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, cw2, sizeof(VtxT));
    VtxT ccw[3] = { { 5, 45, 0, 1, 0xFFFFFF00 }, { 5, 65, 0, 1, 0xFFFFFF00 }, { 25, 45, 0, 1, 0xFFFFFF00 } };
    rs(D3DRS_CULLMODE, D3DCULL_CCW);                       // counter-clockwise culled: disappears
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, ccw, sizeof(VtxT));
    VtxT ccw2[3] = { { 35, 45, 0, 1, 0xFF00FF00 }, { 35, 65, 0, 1, 0xFF00FF00 }, { 55, 45, 0, 1, 0xFF00FF00 } };
    rs(D3DRS_CULLMODE, D3DCULL_CW);                        // counter-clockwise drawn
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, ccw2, sizeof(VtxT));
    // the "no culling" case
    VtxT ccw3[3] = { { 70, 5, 0, 1, 0xFFFFFFFF }, { 70, 25, 0, 1, 0xFFFFFFFF }, { 90, 5, 0, 1, 0xFFFFFFFF } };
    rs(D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, ccw3, sizeof(VtxT));
    with_snapshot(v_cull);
}

static void v_voltex(void) { check_px("volume texture slice 0", px(4, 4), 0xFF0000, 0); check_px("volume texture slice 1", px(20, 4), 0x00FF00, 0); }
static void t_volume(void) {
    reset_states();
    IDirect3DVolumeTexture9 *vt = NULL;
    HRESULT hr = IDirect3DDevice9_CreateVolumeTexture(dev, 2, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vt, NULL);
    check("volume texture created", SUCCEEDED(hr));
    if (!vt) return;
    D3DLOCKED_BOX lb;
    IDirect3DVolumeTexture9_LockBox(vt, 0, &lb, NULL, 0);
    for (int z = 0; z < 2; z++) for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) ((unsigned *)((char *)lb.pBits + z * lb.SlicePitch + y * lb.RowPitch))[x] = z ? 0xFF00FF00u : 0xFFFF0000u;
    IDirect3DVolumeTexture9_UnlockBox(vt, 0);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)vt);
    clear(0xFF000000);
    typedef struct { float x, y, z, rhw; float u, v, w; } V3;
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0));
    V3 a[4] = { { 0, 0, 0, 1, 0.5f, 0.5f, 0.2f }, { 8, 0, 0, 1, 0.5f, 0.5f, 0.2f }, { 0, 8, 0, 1, 0.5f, 0.5f, 0.2f }, { 8, 8, 0, 1, 0.5f, 0.5f, 0.2f } };
    V3 b[4] = { { 16, 0, 0, 1, 0.5f, 0.5f, 0.8f }, { 24, 0, 0, 1, 0.5f, 0.5f, 0.8f }, { 16, 8, 0, 1, 0.5f, 0.5f, 0.8f }, { 24, 8, 0, 1, 0.5f, 0.5f, 0.8f } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, a, sizeof(V3));
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, b, sizeof(V3));
    with_snapshot(v_voltex);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DVolumeTexture9_Release(vt);
}

static void v_pal(void) { check_px("palettised texture", px(2, 2), 0x0000FF, 0); }
static void t_palette(void) {
    reset_states();
    PALETTEENTRY pe[256];
    memset(pe, 0, sizeof pe);
    pe[3].peRed = 0; pe[3].peGreen = 0; pe[3].peBlue = 255; pe[3].peFlags = 255;
    pe[1].peRed = 255; pe[1].peFlags = 255;
    HRESULT h1 = IDirect3DDevice9_SetPaletteEntries(dev, 0, pe);
    IDirect3DDevice9_SetCurrentTexturePalette(dev, 0);
    unsigned char idx = 3;
    IDirect3DTexture9 *t = make_tex(1, 1, D3DFMT_P8, &idx, 1);
    check("p8 texture", SUCCEEDED(h1) && t != NULL);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    clear(0xFF000000);
    quad_tex(0, 0, 4, 4, 0, 0, 1, 1, 0xFFFFFFFF);
    with_snapshot(v_pal);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    if (t) IDirect3DTexture9_Release(t);
}

static void v_autogen(void) { check_px("auto generated mip chain", px(2, 2), 0xFF8040, 1); }
static void t_autogen(void) {
    reset_states();
    IDirect3DTexture9 *t = NULL;
    HRESULT hr = IDirect3DDevice9_CreateTexture(dev, 8, 8, 0, D3DUSAGE_AUTOGENMIPMAP, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, NULL);
    check("autogen texture", SUCCEEDED(hr) && t);
    if (!t) return;
    D3DLOCKED_RECT lr;
    IDirect3DTexture9_LockRect(t, 0, &lr, NULL, 0);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) ((unsigned *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xFFFF8040u;
    IDirect3DTexture9_UnlockRect(t, 0);
    IDirect3DBaseTexture9_GenerateMipSubLevels((IDirect3DBaseTexture9 *)t);
    check("autogen level count", IDirect3DTexture9_GetLevelCount(t) == 1);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    clear(0xFF000000);
    quad_tex(0, 0, 4, 4, 0, 0, 1, 1, 0xFFFFFFFF);
    with_snapshot(v_autogen);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DTexture9_Release(t);
}

static void t_occlusion(void) {
    reset_states();
    IDirect3DQuery9 *q = NULL;
    HRESULT hr = IDirect3DDevice9_CreateQuery(dev, D3DQUERYTYPE_OCCLUSION, &q);
    check("occlusion query", SUCCEEDED(hr) && q);
    if (!q) return;
    clear(0xFF000000);
    IDirect3DQuery9_Issue(q, D3DISSUE_BEGIN);
    quad_t(10, 10, 20, 20, 0, 0xFFFFFFFF);
    IDirect3DQuery9_Issue(q, D3DISSUE_END);
    DWORD n = 0;
    hr = IDirect3DQuery9_GetData(q, &n, sizeof n, D3DGETDATA_FLUSH);
    check("occlusion pixels", hr == S_OK && n == 100);
    IDirect3DQuery9_Release(q);
}

static void v_sprite(void) {
    check("point sprite texture coordinates", px(29, 32) == 0xFF0000 && px(31, 32) == 0xFF0000 && px(32, 32) == 0x0000FF && px(35, 32) == 0x0000FF);
    check("point sprite extent", px(27, 32) == 0 && px(36, 32) == 0 && px(32, 27) == 0 && px(32, 36) == 0);
}
static void t_sprite(void) {
    reset_states();
    unsigned two[2] = { 0xFFFF0000u, 0xFF0000FFu };
    IDirect3DTexture9 *t = make_tex(2, 1, D3DFMT_A8R8G8B8, two, 4);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    clear(0xFF000000);
    rs(D3DRS_POINTSPRITEENABLE, TRUE); rs(D3DRS_POINTSIZE, fbits(8.0f));
    VtxT p = { 32, 32, 0, 1, 0xFFFFFFFF };
    IDirect3DDevice9_SetFVF(dev, FVF_T);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_POINTLIST, 1, &p, sizeof p);
    with_snapshot(v_sprite);
    rs(D3DRS_POINTSPRITEENABLE, FALSE); rs(D3DRS_POINTSIZE, fbits(1.0f));
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DTexture9_Release(t);
}

// Vertex and pixel shaders: vs_1_1 "mov oPos, v0; mov oD0, v5" (stock register mapping) + ps_1_1 "mov r0, v0", once the shader engine is available
static void t_shaders(void) {
    static const DWORD vs[] = { 0xFFFE0101, 0x00000001, 0xC00F0000, 0x90E40000, 0x00000001, 0xD00F0000, 0x90E40005, 0x0000FFFF };
    static const DWORD ps[] = { 0xFFFF0101, 0x00000001, 0x800F0000, 0x90E40000, 0x0000FFFF };
    IDirect3DVertexShader9 *v = NULL;
    IDirect3DPixelShader9 *p = NULL;
    HRESULT hv = IDirect3DDevice9_CreateVertexShader(dev, vs, &v);
    HRESULT hp = IDirect3DDevice9_CreatePixelShader(dev, ps, &p);
    if (FAILED(hv) || FAILED(hp)) { printf("shaders unavailable\n"); return; }
    reset_states();
    clear(0xFF000000);
    IDirect3DDevice9_SetVertexShader(dev, v);
    IDirect3DDevice9_SetPixelShader(dev, p);
    static const D3DVERTEXELEMENT9 el[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
    IDirect3DVertexDeclaration9 *vd = NULL;
    IDirect3DDevice9_CreateVertexDeclaration(dev, el, &vd);
    IDirect3DDevice9_SetVertexDeclaration(dev, vd);
    VtxD q[4] = { { -0.5f, 0.5f, 0.2f, 0xFFFF8000 }, { 0.5f, 0.5f, 0.2f, 0xFFFF8000 }, { -0.5f, -0.5f, 0.2f, 0xFFFF8000 }, { 0.5f, -0.5f, 0.2f, 0xFFFF8000 } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VtxD));
    IDirect3DSurface9 *bb = backbuffer();
    snapshot_rt(bb);
    check_px("shader pass-through", px(64, 64), 0xFF8000, 2);
    snap_end();
    IDirect3DSurface9_Release(bb);
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetVertexDeclaration(dev, NULL);
    IDirect3DVertexDeclaration9_Release(vd);
    IDirect3DVertexShader9_Release(v);
    IDirect3DPixelShader9_Release(p);
}

// ---- MultiplyTransform and shader scenes -----------------------------------------------------------------------------------------------
static void mat_mul(D3DMATRIX *r, const D3DMATRIX *a, const D3DMATRIX *b) { // row vectors: r = a * b (a is applied first)
    D3DMATRIX t;
    const float *A = &a->_11, *B = &b->_11;
    float *T = &t._11;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += A[i * 4 + k] * B[k * 4 + j]; T[i * 4 + j] = s; }
    *r = t;
}
static void mat_translate(D3DMATRIX *m, float x, float y, float z) { ident(m); m->_41 = x; m->_42 = y; m->_43 = z; }
static void mat_scale(D3DMATRIX *m, float x, float y, float z) { ident(m); m->_11 = x; m->_22 = y; m->_33 = z; }
static void mat_transpose(float *out, const D3DMATRIX *m) { const float *s = &m->_11; for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[j * 4 + i] = s[i * 4 + j]; }

static void t_multiply_transform(void) {
    reset_states();
    D3DMATRIX w, s, r;
    mat_translate(&w, 10, 0, 0);
    mat_scale(&s, 2, 2, 2);
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &w);
    HRESULT hr = IDirect3DDevice9_MultiplyTransform(dev, D3DTS_WORLD, &s);
    memset(&r, 0, sizeof r);
    IDirect3DDevice9_GetTransform(dev, D3DTS_WORLD, &r);
    // pMatrix x State (row vectors: the scale is applied first, then the translation): the translation stays 10
    check("MultiplyTransform order is pMatrix x State", hr == D3D_OK && r._41 == 10.0f && r._11 == 2.0f && r._22 == 2.0f && r._44 == 1.0f && r._14 == 0.0f);
    // a second one with a non-commuting pair: rotation by 90 degrees about z (x -> y) applied first, then the scale and the translation
    D3DMATRIX rot;
    ident(&rot); rot._11 = 0; rot._12 = 1; rot._21 = -1; rot._22 = 0;
    IDirect3DDevice9_MultiplyTransform(dev, D3DTS_WORLD, &rot);
    IDirect3DDevice9_GetTransform(dev, D3DTS_WORLD, &r);
    // rot x (scale x translate): rows (0,2,0,0), (-2,0,0,0), (0,0,2,0), (10,0,0,1); State x pMatrix would have put 10 into _42
    check("MultiplyTransform second product", r._12 == 2.0f && r._21 == -2.0f && r._11 == 0.0f && r._22 == 0.0f && r._33 == 2.0f && r._41 == 10.0f && r._42 == 0.0f);
    ident(&w);
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &w);
}

typedef struct { float x, y, z; DWORD c; float u, v; } VtxPCT;
static const D3DVERTEXELEMENT9 el_pct[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 },
                                            { 0, 16, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
typedef struct { float x, y, z, nx, ny, nz, u, v; DWORD c; } VtxPNTC;
static const D3DVERTEXELEMENT9 el_pntc[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL, 0 },
                                             { 0, 24, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, { 0, 32, D3DDECLTYPE_D3DCOLOR, 0, D3DDECLUSAGE_COLOR, 0 }, D3DDECL_END() };
static const D3DVERTEXELEMENT9 el_pos3[] = { { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, D3DDECL_END() };

static IDirect3DVertexShader9 *mkvs(const uint32_t *t) { IDirect3DVertexShader9 *s = NULL; if (FAILED(IDirect3DDevice9_CreateVertexShader(dev, (const DWORD *)t, &s))) s = NULL; return s; }
static IDirect3DPixelShader9 *mkps(const uint32_t *t) { IDirect3DPixelShader9 *s = NULL; if (FAILED(IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)t, &s))) s = NULL; return s; }
static IDirect3DVertexDeclaration9 *mkdecl(const D3DVERTEXELEMENT9 *e) { IDirect3DVertexDeclaration9 *d = NULL; if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(dev, e, &d))) d = NULL; return d; }
static void unbind_shaders(void) {
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, NULL);
    IDirect3DDevice9_SetVertexDeclaration(dev, NULL);
    IDirect3DDevice9_SetTexture(dev, 0, NULL);
    IDirect3DDevice9_SetTexture(dev, 1, NULL);
}
static int count_lit(void) { int n = 0; for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) if (px(x, y)) n++; return n; }

// the shared textured quad of scenes (a) and (d): local quad -1..1 turned into pixels 48..111 x 16..79 by world = scale(.5) * translate(.25, 0, 0)
// and view = translate(0, .25, 0): every number is a power of two, so the result is exact
static const unsigned k_tex4[4] = { 0xFFFFFFFFu, 0xFF0000FFu, 0xFF00FF00u, 0xFFFFFF00u };   // (0,0) white, (1,0) blue, (0,1) green, (1,1) yellow
static D3DMATRIX g_world, g_view;
static void make_wv(void) {
    D3DMATRIX s, t;
    mat_scale(&s, 0.5f, 0.5f, 1.0f);
    mat_translate(&t, 0.25f, 0, 0);
    mat_mul(&g_world, &s, &t);
    mat_translate(&g_view, 0, 0.25f, 0);
}
static void draw_pct_quad(void) {
    VtxPCT q[4] = { { -1, 1, 0.5f, 0xFFFF8080, 0, 0 }, { 1, 1, 0.5f, 0xFFFF8080, 1, 0 }, { -1, -1, 0.5f, 0xFFFF8080, 0, 1 }, { 1, -1, 0.5f, 0xFFFF8080, 1, 1 } };
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VtxPCT));
}
static void set_wvp_constants(void) {
    D3DMATRIX wv, wvp, ident_m;
    ident(&ident_m);
    mat_mul(&wv, &g_world, &g_view);
    mat_mul(&wvp, &wv, &ident_m);
    float c[16];
    mat_transpose(c, &wvp);     // shader matrices are column major: c0..c3 hold the columns of the row-vector matrix
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, c, 4);
}
// pixels of the four texels, away from the texel borders; the corners are exact because of the top-left rule
static void v_quad_corners(const char *name, unsigned t00, unsigned t10, unsigned t01, unsigned t11, int tol) {
    char nm[96];
    snprintf(nm, sizeof nm, "%s texel (0,0)", name); check_px(nm, px(60, 30), t00, tol);
    snprintf(nm, sizeof nm, "%s texel (1,0)", name); check_px(nm, px(100, 30), t10, tol);
    snprintf(nm, sizeof nm, "%s texel (0,1)", name); check_px(nm, px(60, 70), t01, tol);
    snprintf(nm, sizeof nm, "%s texel (1,1)", name); check_px(nm, px(100, 70), t11, tol);
    snprintf(nm, sizeof nm, "%s extent", name);
    check(nm, px(48, 16) != 0 && px(47, 16) == 0 && px(48, 15) == 0 && px(111, 79) != 0 && px(112, 79) == 0 && px(111, 80) == 0 && px(48, 80) == 0 && px(40, 30) == 0 && px(116, 30) == 0);
}

// (a) vs_2_0: world-view-projection from constants; ps_2_0: texture x diffuse
static void t_shader_wvp(void) {
    IDirect3DVertexShader9 *vs = mkvs(SH_vs20_wvp);
    IDirect3DPixelShader9 *ps = mkps(SH_ps20_texmod);
    IDirect3DVertexDeclaration9 *vd = mkdecl(el_pct);
    check("vs_2_0 / ps_2_0 created", vs && ps && vd);
    if (!(vs && ps && vd)) return;
    reset_states();
    make_wv();
    IDirect3DTexture9 *t = make_tex(2, 2, D3DFMT_A8R8G8B8, k_tex4, 4);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetVertexDeclaration(dev, vd);
    set_wvp_constants();
    clear(0xFF000000);
    draw_pct_quad();
    IDirect3DSurface9 *bb = backbuffer();
    if (snapshot_rt(bb)) {
        // texel x diffuse (1, 0.5, 0.5)
        v_quad_corners("vs_2_0 wvp + ps_2_0", 0xFF8080, 0x000080, 0x008000, 0xFF8000, 2);
        check("vs_2_0 wvp covers exactly 64 x 64 pixels", count_lit() == 64 * 64);
        snap_end();
    }
    IDirect3DSurface9_Release(bb);
    // a different matrix between two draws: the quad moves by one whole pixel column worth of NDC (1/64) and shrinks nothing
    D3DMATRIX tr, m;
    mat_translate(&tr, -0.5f, -0.5f, 0);
    mat_mul(&m, &g_world, &g_view);
    mat_mul(&m, &m, &tr);
    float c[16];
    mat_transpose(c, &m);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, c, 4);
    clear(0xFF000000);
    draw_pct_quad();
    bb = backbuffer();
    if (snapshot_rt(bb)) {
        // 0.5 in NDC = 32 pixels right/down?: x grows by -0.5 * 64 = -32 pixels, y by +32 pixels (screen y is flipped): quad at 16..79 x 48..111
        check_px("vs_2_0 constants changed between draws", px(28, 60), 0xFF8080, 2);
        check("vs_2_0 moved quad extent", px(16, 48) != 0 && px(15, 48) == 0 && px(79, 111) != 0 && px(80, 111) == 0 && px(79, 112) == 0 && px(60, 40) == 0);
        snap_end();
    }
    IDirect3DSurface9_Release(bb);
    unbind_shaders();
    IDirect3DTexture9_Release(t);
    IDirect3DVertexDeclaration9_Release(vd);
    IDirect3DVertexShader9_Release(vs);
    IDirect3DPixelShader9_Release(ps);
}

// (b) vs_3_0 with semantics and a loop over constants; ps_3_0 discarding through vPos and texkill
static void t_shader_sm3(void) {
    IDirect3DVertexShader9 *vs = mkvs(SH_vs30_loop);
    IDirect3DPixelShader9 *ps = mkps(SH_ps30_discard);
    IDirect3DVertexDeclaration9 *vd = mkdecl(el_pntc);
    check("vs_3_0 / ps_3_0 created", vs && ps && vd);
    if (!(vs && ps && vd)) return;
    reset_states();
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetVertexDeclaration(dev, vd);
    static const float consts[12] = { 0.25f, 0.5f, 0.25f, 0.5f,   0.25f, 0.25f, 0.25f, 0.25f,   0.0f, 0.25f, 0.25f, 0.25f };   // c10..c12, sum = (.5, 1, .75, 1)
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 10, consts, 3);
    // pixels 36..99 x 20..83 (NDC -0.4375..0.5625 and 0.6875..-0.3125), v from 0 at the top to 1 at the bottom
    const float x0 = -0.4375f, x1 = 0.5625f, y0 = 0.6875f, y1 = -0.3125f;
    VtxPNTC q[4] = { { x0, y0, 0.5f, 0, 0, 0.5f, 0, 0, 0xFFFFFFFF }, { x1, y0, 0.5f, 0, 0, 0.5f, 1, 0, 0xFFFFFFFF },
                     { x0, y1, 0.5f, 0, 0, 0.5f, 0, 1, 0xFFFFFFFF }, { x1, y1, 0.5f, 0, 0, 0.5f, 1, 1, 0xFFFFFFFF } };
    for (int pass = 0; pass < 2; pass++) {
        // pass 1: table fog switched on; pixel shader 3.0 does not get fixed-function fog applied
        if (pass) { rs(D3DRS_FOGENABLE, TRUE); rs(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR); rs(D3DRS_FOGSTART, fbits(0.0f)); rs(D3DRS_FOGEND, fbits(1.0f)); rs(D3DRS_FOGCOLOR, 0xFF0000FF); }
        clear(0xFF000000);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(VtxPNTC));
        IDirect3DSurface9 *bb = backbuffer();
        if (snapshot_rt(bb)) {
            const char *sfx = pass ? " (fog enabled)" : "";
            char nm[96];
            // colour = vertex colour (1,1,1,1) x loop sum (.5, 1, .75), blue replaced by the interpolated texcoord1.z = 0.5
            snprintf(nm, sizeof nm, "vs_3_0 loop + semantics%s", sfx); check_px(nm, px(45, 60), 0x80FF80, 1);
            snprintf(nm, sizeof nm, "ps_3_0 vPos discards column 40%s", sfx); check(nm, px(40, 60) == 0 && px(41, 60) != 0 && px(39, 60) != 0);
            snprintf(nm, sizeof nm, "ps_3_0 vPos discards column 48 and 96%s", sfx); check(nm, px(48, 60) == 0 && px(49, 60) != 0 && px(96, 60) == 0 && px(97, 60) != 0);
            snprintf(nm, sizeof nm, "ps_3_0 texkill on texcoord v%s", sfx); check(nm, px(45, 37) == 0 && px(45, 38) != 0 && px(45, 20) == 0);
            snprintf(nm, sizeof nm, "ps_3_0 quad extent%s", sfx); check(nm, px(36, 60) != 0 && px(35, 60) == 0 && px(99, 60) != 0 && px(100, 60) == 0 && px(45, 83) != 0 && px(45, 84) == 0);
            // 64 columns minus the 8 columns with x = 0 (mod 8) = 56, rows 38..83 = 46
            snprintf(nm, sizeof nm, "ps_3_0 surviving pixel count%s", sfx); check(nm, count_lit() == 56 * 46);
            snap_end();
        }
        IDirect3DSurface9_Release(bb);
    }
    rs(D3DRS_FOGENABLE, FALSE);
    unbind_shaders();
    IDirect3DVertexDeclaration9_Release(vd);
    IDirect3DVertexShader9_Release(vs);
    IDirect3DPixelShader9_Release(ps);
}

// (c) ps_1_4: a texture lookup, "phase", then a dependent read that uses the first result as texture coordinates
static void t_shader_ps14(void) {
    IDirect3DPixelShader9 *ps = mkps(SH_ps14_phase);
    check("ps_1_4 created", ps != NULL);
    if (!ps) return;
    reset_states();
    static const unsigned coords[4] = { 0xFF000000u, 0xFFFF0000u, 0xFF00FF00u, 0xFFFFFF00u };   // (r,g) = (0,0) (1,0) (0,1) (1,1)
    static const unsigned palette[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u };   // red green blue yellow
    IDirect3DTexture9 *t0 = make_tex(2, 2, D3DFMT_A8R8G8B8, coords, 4);
    IDirect3DTexture9 *t1 = make_tex(2, 2, D3DFMT_A8R8G8B8, palette, 4);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t0);
    IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)t1);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    clear(0xFF000000);
    // pixels 16..79 square (pretransformed vertices; D3D9 pixel centres are integers, so the edges sit at -0.5)
    quad_tex(15.5f, 15.5f, 79.5f, 79.5f, 0, 0, 1, 1, 0xFF8080FF);
    IDirect3DSurface9 *bb = backbuffer();
    if (snapshot_rt(bb)) {
        // dependent read picks the palette entry named by the first texture, then diffuse (.5, .5, 1) is multiplied in
        check_px("ps_1_4 phase / dependent read (0,0)", px(30, 30), 0x800000, 2);
        check_px("ps_1_4 phase / dependent read (1,0)", px(65, 30), 0x008000, 2);
        check_px("ps_1_4 phase / dependent read (0,1)", px(30, 65), 0x0000FF, 2);
        check_px("ps_1_4 phase / dependent read (1,1)", px(65, 65), 0x808000, 2);
        check("ps_1_4 quad extent", px(16, 16) != 0 && px(15, 16) == 0 && px(79, 79) != 0 && px(80, 79) == 0 && px(16, 15) == 0 && count_lit() == 64 * 64);
        snap_end();
    }
    IDirect3DSurface9_Release(bb);
    unbind_shaders();
    IDirect3DTexture9_Release(t0);
    IDirect3DTexture9_Release(t1);
    IDirect3DPixelShader9_Release(ps);
}

// (d) vertex shader + fixed-function texture stages, and fixed-function vertices + pixel shader
static void t_shader_mixed(void) {
    IDirect3DVertexShader9 *vs = mkvs(SH_vs11_wvp);
    IDirect3DPixelShader9 *ps = mkps(SH_ps20_const);
    IDirect3DVertexDeclaration9 *vd = mkdecl(el_pct);
    check("mixed pipeline shaders created", vs && ps && vd);
    if (!(vs && ps && vd)) return;
    reset_states();
    make_wv();
    IDirect3DTexture9 *t = make_tex(2, 2, D3DFMT_A8R8G8B8, k_tex4, 4);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)t);
    // vs_1_1 + fixed-function pixel pipeline: stage 0 = texture x diffuse, stage 1 = previous + texture factor (16, 16, 16)
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetVertexDeclaration(dev, vd);
    set_wvp_constants();
    rs(D3DRS_TEXTUREFACTOR, 0x00101010);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_ADD);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
    clear(0xFF000000);
    draw_pct_quad();
    IDirect3DSurface9 *bb = backbuffer();
    if (snapshot_rt(bb)) {
        v_quad_corners("vs_1_1 + fixed-function stages", 0xFF9090, 0x101090, 0x109010, 0xFF9010, 2);
        snap_end();
    }
    IDirect3DSurface9_Release(bb);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    IDirect3DDevice9_SetTextureStageState(dev, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    // fixed-function vertex pipeline (world / view / projection transforms) + ps_2_0 (texture x diffuse x (.5, 1, .25))
    IDirect3DDevice9_SetVertexShader(dev, NULL);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &g_world);
    IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &g_view);
    clear(0xFF000000);
    draw_pct_quad();
    bb = backbuffer();
    if (snapshot_rt(bb)) {
        v_quad_corners("fixed-function vertices + ps_2_0", 0x808020, 0x000020, 0x008000, 0x808000, 2);
        snap_end();
    }
    IDirect3DSurface9_Release(bb);
    unbind_shaders();
    IDirect3DTexture9_Release(t);
    IDirect3DVertexDeclaration9_Release(vd);
    IDirect3DVertexShader9_Release(vs);
    IDirect3DPixelShader9_Release(ps);
}

// (e) ps_2_0 + table fog: fog is applied after the shader (pixel shaders below 3.0), the fog factor comes from the depth
static void t_shader_fog(void) {
    IDirect3DPixelShader9 *ps = mkps(SH_ps20_solid);
    check("ps_2_0 solid created", ps != NULL);
    if (!ps) return;
    reset_states();
    IDirect3DDevice9_SetPixelShader(dev, ps);
    rs(D3DRS_FOGENABLE, TRUE); rs(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
    rs(D3DRS_FOGSTART, fbits(0.0f)); rs(D3DRS_FOGEND, fbits(1.0f)); rs(D3DRS_FOGCOLOR, 0xFF0000FF);
    clear(0xFF000000);
    quad_t(9.5f, 9.5f, 39.5f, 39.5f, 0.5f, 0xFFFFFFFF);    // z = 0.5: factor (end - z) / (end - start) = 0.5 -> half red, half blue
    quad_t(49.5f, 9.5f, 79.5f, 39.5f, 0.0f, 0xFFFFFFFF);   // z = 0: no fog, the shader colour
    quad_t(89.5f, 9.5f, 119.5f, 39.5f, 1.0f, 0xFFFFFFFF);  // z = 1: factor 0, pure fog colour
    IDirect3DSurface9 *bb = backbuffer();
    if (snapshot_rt(bb)) {
        check_px("ps_2_0 table fog halfway", px(25, 25), 0x800080, 2);
        check_px("ps_2_0 table fog near", px(65, 25), 0xFF0000, 1);
        check_px("ps_2_0 table fog far", px(105, 25), 0x0000FF, 1);
        snap_end();
    }
    IDirect3DSurface9_Release(bb);
    rs(D3DRS_FOGENABLE, FALSE);
    clear(0xFF000000);
    quad_t(9.5f, 9.5f, 39.5f, 39.5f, 0.5f, 0xFFFFFFFF);
    bb = backbuffer();
    if (snapshot_rt(bb)) { check_px("ps_2_0 without fog", px(25, 25), 0xFF0000, 1); snap_end(); }
    IDirect3DSurface9_Release(bb);
    unbind_shaders();
    IDirect3DPixelShader9_Release(ps);
}

// (f) vertex shader constants (float, integer, boolean) changed between draws of the same shader
static void t_shader_consts(void) {
    IDirect3DVertexShader9 *vs = mkvs(SH_vs20_consts);
    IDirect3DPixelShader9 *ps = mkps(SH_ps11_diffuse);
    IDirect3DVertexDeclaration9 *vd = mkdecl(el_pos3);
    check("constant test shaders created", vs && ps && vd);
    if (!(vs && ps && vd)) return;
    reset_states();
    IDirect3DDevice9_SetVertexShader(dev, vs);
    IDirect3DDevice9_SetPixelShader(dev, ps);
    IDirect3DDevice9_SetVertexDeclaration(dev, vd);
    float quad[4][3] = { { -0.25f, 0.25f, 0.5f }, { 0.25f, 0.25f, 0.5f }, { -0.25f, -0.25f, 0.5f }, { 0.25f, -0.25f, 0.5f } };
    // colour = c4 + count * c5, multiplied by c6 when b0 is set; the quad is moved by c7
    float c4[4] = { 0.0f, 0.25f, 0.0f, 1.0f }, c5[4] = { 0.25f, 0.0f, 0.0f, 0.0f }, c6[4] = { 0.5f, 2.0f, 1.0f, 1.0f };
    float off[4][4] = { { -0.5f, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0.5f, 0, 0, 0 }, { 0, -0.5f, 0, 0 } };
    int cnt[4][4] = { { 0, 0, 1, 0 }, { 2, 0, 1, 0 }, { 2, 0, 1, 0 }, { 1, 0, 1, 0 } };
    BOOL b[4] = { FALSE, FALSE, TRUE, FALSE };
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, c4, 1);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 5, c5, 1);
    IDirect3DDevice9_SetVertexShaderConstantF(dev, 6, c6, 1);
    clear(0xFF000000);
    for (int i = 0; i < 4; i++) {
        if (i == 3) { c4[1] = 0.0f; c4[2] = 0.5f; IDirect3DDevice9_SetVertexShaderConstantF(dev, 4, c4, 1); }   // only a float constant changes here
        IDirect3DDevice9_SetVertexShaderConstantF(dev, 7, off[i], 1);
        IDirect3DDevice9_SetVertexShaderConstantI(dev, 0, cnt[i], 1);
        IDirect3DDevice9_SetVertexShaderConstantB(dev, 0, &b[i], 1);
        IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]);
    }
    int gi[4] = { 0 }; BOOL gb = TRUE; float gf[4] = { 0 };
    IDirect3DDevice9_GetVertexShaderConstantI(dev, 0, gi, 1);
    IDirect3DDevice9_GetVertexShaderConstantB(dev, 0, &gb, 1);
    IDirect3DDevice9_GetVertexShaderConstantF(dev, 4, gf, 1);
    check("vertex shader constants read back", gi[0] == 1 && gi[2] == 1 && gb == FALSE && gf[1] == 0.0f && gf[2] == 0.5f && gf[3] == 1.0f);
    IDirect3DSurface9 *bb = backbuffer();
    if (snapshot_rt(bb)) {
        check_px("constants: zero loop iterations, no scale", px(32, 64), 0x004000, 1);       // c4 = (0, .25, 0, 1)
        check_px("constants: integer loop count 2", px(64, 64), 0x804000, 1);                 // c4 + 2 * c5 = (.5, .25, 0, 1)
        check_px("constants: boolean enables the scale", px(96, 64), 0x408000, 1);            // x c6 = (.25, .5, 0, 1)
        check_px("constants: float change between draws", px(64, 96), 0x400080, 1);           // c4 = (0, 0, .5) + c5 = (.25, 0, .5)
        check("constants: quads placed by c7", px(16, 48) != 0 && px(15, 48) == 0 && px(47, 79) != 0 && px(48, 79) != 0 && px(111, 79) != 0 && px(112, 79) == 0 && px(64, 112) == 0 && px(64, 111) != 0);
        snap_end();
    }
    IDirect3DSurface9_Release(bb);
    unbind_shaders();
    IDirect3DVertexDeclaration9_Release(vd);
    IDirect3DVertexShader9_Release(vs);
    IDirect3DPixelShader9_Release(ps);
}

static void t_shader_scenes(void) {
    t_shader_wvp();
    t_shader_sm3();
    t_shader_ps14();
    t_shader_mixed();
    t_shader_fog();
    t_shader_consts();
}

// the frame the GUI script looks at: a gradient background with a few solid rectangles
static void draw_gui_frame(void) {
    reset_states();
    VtxT bg[4] = { { -0.5f, -0.5f, 0, 1, 0xFF204080 }, { 127.5f, -0.5f, 0, 1, 0xFF204080 }, { -0.5f, 127.5f, 0, 1, 0xFF204080 }, { 127.5f, 127.5f, 0, 1, 0xFF204080 } };
    IDirect3DDevice9_SetFVF(dev, FVF_T);
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, bg, sizeof(VtxT));
    quad_t(-0.5f + 10, -0.5f + 10, 49.5f, 49.5f, 0, 0xFFFF0000);
    quad_t(-0.5f + 70, -0.5f + 70, 119.5f, 119.5f, 0, 0xFF00FF00);
}

int main(int argc, char **argv) {
    int gui = argc > 1 && !strcmp(argv[1], "--gui");
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "SoulD3D9Test";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    wnd = CreateWindowExA(0, "SoulD3D9Test", "d3d9", WS_POPUP, 100, 100, 128, 128, NULL, NULL, wc.hInstance, NULL);
    if (gui) ShowWindow(wnd, SW_SHOW);
    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = 128; pp.BackBufferHeight = 128; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice failed %08x\n", (unsigned)hr); return 1; }
    IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 128, 128, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sysmem, NULL);
    if (gui) {
        draw_gui_frame();
        IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        printf("d3d9 gui ready\n");
        fflush(stdout);
        MSG m;
        for (int i = 0; i < 100000; i++) {
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT || m.message == WM_CLOSE) return 0; TranslateMessage(&m); DispatchMessageA(&m); }
            Sleep(10);
            if (!IsWindow(wnd)) break;
            IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        }
        return 0;
    }
    printf("device created\n");
    t_caps();
    t_clear();
    t_halfpixel();
    t_cube();
    t_texture();
    t_formats();
    t_render_to_texture();
    t_blend();
    t_alphatest();
    t_stencil();
    t_scissor();
    t_stages();
    t_fog();
    t_points();
    t_indexed();
    t_up();
    t_decl();
    t_cube_map();
    t_surfaces();
    t_texxform();
    t_fill_shade();
    t_clipplane();
    t_lights();
    t_instancing();
    t_mips();
    t_fog2();
    t_cull();
    t_volume();
    t_palette();
    t_autogen();
    t_occlusion();
    t_sprite();
    t_swapchains();
    t_refcounts();
    t_shaders();
    t_multiply_transform();
    t_shader_scenes();
    t_reset();
    draw_gui_frame();
    check("present", IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL) == D3D_OK);
    IDirect3DSurface9_Release(sysmem);
    ULONG rc = IDirect3DDevice9_Release(dev);
    printf("device release: %u\n", (unsigned)rc);
    rc = IDirect3D9_Release(d3d);
    printf("d3d release: %u\n", (unsigned)rc);
    printf("failures: %d\n", g_fail);
    return g_fail != 0;
}
