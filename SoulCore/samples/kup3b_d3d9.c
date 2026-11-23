// Küp 3B (Direct3D 9) - Direct3D 9 ile dönen dokulu ve ışıklı küp (Soul Direct3D 9 katmanı örneği)
// Köşe ve piksel gölgelendiricisi (vs_2_0 / ps_2_0) kullanır; F tuşu sabit işlevli (gölgelendiricisiz) boru hattına geçirir.
// Sürükleyerek döndürün. Gölgelendirici baytkodu kup3b_d3d9.asm'den d3dasm.py ile üretilir (kup3b_d3d9_shaders.h).
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "kup3b_d3d9_shaders.h"

#define CW 640
#define CH 480

typedef struct { float x, y, z, nx, ny, nz, u, v; } Vtx;
static Vtx g_vtx[24];
static unsigned short g_idx[36];

static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static IDirect3DTexture9 *g_tex;
static IDirect3DVertexShader9 *g_vs;
static IDirect3DPixelShader9 *g_ps;
static IDirect3DVertexDeclaration9 *g_decl;
static HWND g_wnd;
static float g_yaw = 30, g_pitch = 20, g_spin;
static int g_drag, g_lx, g_ly, g_fixed, g_shaders = 1;

// ---- 4x4 matrices, row vectors (v * M), as in Direct3D ----
typedef struct { float m[16]; } Mat;
static Mat mat_id(void) { Mat r = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } }; return r; }
static Mat mat_mul(Mat a, Mat b) {
    Mat r;
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float s = 0; for (int k = 0; k < 4; k++) s += a.m[i * 4 + k] * b.m[k * 4 + j]; r.m[i * 4 + j] = s; }
    return r;
}
static Mat mat_roty(float a) { Mat r = mat_id(); r.m[0] = cosf(a); r.m[2] = -sinf(a); r.m[8] = sinf(a); r.m[10] = cosf(a); return r; }
static Mat mat_rotx(float a) { Mat r = mat_id(); r.m[5] = cosf(a); r.m[6] = sinf(a); r.m[9] = -sinf(a); r.m[10] = cosf(a); return r; }
static Mat mat_translate(float x, float y, float z) { Mat r = mat_id(); r.m[12] = x; r.m[13] = y; r.m[14] = z; return r; }
static Mat mat_perspective(float fovy, float aspect, float zn, float zf) { // left handed, like D3DXMatrixPerspectiveFovLH
    float ys = 1.0f / tanf(fovy * 0.5f), xs = ys / aspect;
    Mat r = { { xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0 } };
    return r;
}
static void mat_transposed(const Mat *a, float *out) { for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) out[j * 4 + i] = a->m[i * 4 + j]; }

// ---- geometry: 6 faces of 4 vertices, clockwise seen from outside (Direct3D's front faces) ----
static void make_cube(void) {
    static const float N[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    static const float C[4][2] = { { -1, 1 }, { 1, 1 }, { 1, -1 }, { -1, -1 } };   // top-left, top-right, bottom-right, bottom-left
    static const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    for (int f = 0; f < 6; f++) {
        const float *n = N[f];
        float u[3] = { 0, 1, 0 };
        if (n[1] != 0) { u[1] = 0; u[2] = 1; }
        float r[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };   // right = n x up
        for (int k = 0; k < 4; k++) {
            Vtx *v = &g_vtx[f * 4 + k];
            v->x = n[0] + C[k][0] * r[0] + C[k][1] * u[0];
            v->y = n[1] + C[k][0] * r[1] + C[k][1] * u[1];
            v->z = n[2] + C[k][0] * r[2] + C[k][1] * u[2];
            v->nx = n[0]; v->ny = n[1]; v->nz = n[2];
            v->u = UV[k][0]; v->v = UV[k][1];
        }
        unsigned short b = (unsigned short)(f * 4), *ix = &g_idx[f * 6];
        ix[0] = b; ix[1] = (unsigned short)(b + 1); ix[2] = (unsigned short)(b + 2);
        ix[3] = b; ix[4] = (unsigned short)(b + 2); ix[5] = (unsigned short)(b + 3);
    }
}

static void make_texture(void) {
    if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_tex, NULL))) { g_tex = NULL; return; }
    D3DLOCKED_RECT lr;
    IDirect3DTexture9_LockRect(g_tex, 0, &lr, NULL, 0);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        int check = ((x / 8) + (y / 8)) & 1;
        int border = x < 3 || y < 3 || x > 60 || y > 60;
        unsigned r = check ? 240 : 40, g = check ? 200 : 120, b = check ? 60 : 220;
        if (border) r = g = b = 255;
        ((unsigned *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xFF000000u | r << 16 | g << 8 | b;
    }
    IDirect3DTexture9_UnlockRect(g_tex, 0);
}

static int init_d3d(HWND wnd) {
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return 0;
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = CW; pp.BackBufferHeight = CH; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd; pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8; pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    if (FAILED(IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &g_dev))) return 0;
    make_cube();
    make_texture();
    static const D3DVERTEXELEMENT9 el[] = {
        { 0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0 }, { 0, 12, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_NORMAL, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
    IDirect3DDevice9_CreateVertexDeclaration(g_dev, el, &g_decl);
    if (FAILED(IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)SH_vs20_cube, &g_vs))) g_vs = NULL;
    if (FAILED(IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)SH_ps20_cube, &g_ps))) g_ps = NULL;
    if (!g_vs || !g_ps || !g_decl) g_shaders = 0;   // fixed-function fallback
    printf("D3D9: gölgelendirici %s\n", g_shaders ? "açık" : "kapalı (sabit işlev)");

    // fixed-function state (used by the fallback and as the base of the shader path)
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_NORMALIZENORMALS, TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_AMBIENT, 0x00404040);
    D3DMATERIAL9 mat;
    memset(&mat, 0, sizeof mat);
    mat.Diffuse.r = mat.Diffuse.g = mat.Diffuse.b = mat.Diffuse.a = 1;
    mat.Ambient = mat.Diffuse;
    IDirect3DDevice9_SetMaterial(g_dev, &mat);
    D3DLIGHT9 lt;
    memset(&lt, 0, sizeof lt);
    lt.Type = D3DLIGHT_DIRECTIONAL;
    lt.Diffuse.r = lt.Diffuse.g = lt.Diffuse.b = lt.Diffuse.a = 1;
    lt.Direction.x = -0.4f; lt.Direction.y = -0.7f; lt.Direction.z = 0.6f;
    IDirect3DDevice9_SetLight(g_dev, 0, &lt);
    IDirect3DDevice9_LightEnable(g_dev, 0, TRUE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetSamplerState(g_dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    IDirect3DDevice9_SetTexture(g_dev, 0, (IDirect3DBaseTexture9 *)g_tex);
    return 1;
}

static void render(void) {
    Mat world = mat_mul(mat_roty((g_yaw + g_spin) * 0.0174533f), mat_rotx(g_pitch * 0.0174533f));
    Mat view = mat_translate(0, 0, 5);
    Mat proj = mat_perspective(0.8f, (float)CW / (float)CH, 1.0f, 20.0f);
    IDirect3DDevice9_Clear(g_dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF141A29, 1.0f, 0);
    IDirect3DDevice9_BeginScene(g_dev);
    if (g_shaders) {
        Mat wvp = mat_mul(mat_mul(world, view), proj);
        float c[16];
        mat_transposed(&wvp, c);
        IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, c, 4);
        mat_transposed(&world, c);
        IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 4, c, 3);       // c4..c6: columns of the world matrix (upper 3x3)
        static const float k[] = { 0.4f / 1.0049876f, 0.7f / 1.0049876f, -0.6f / 1.0049876f, 0,   // c8: towards the light (unit length: |(.4,.7,.6)| = 1.00499)
                                   0, 0, 0, 0,                                                       // c9: zero
                                   1.0f, 0.9f, 0.7f, 0,                                              // c10: diffuse colour (warm)
                                   0.25f, 0.25f, 0.25f, 1 };                                         // c11: ambient
        IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 8, k, 4);
        IDirect3DDevice9_SetVertexShader(g_dev, g_vs);
        IDirect3DDevice9_SetPixelShader(g_dev, g_ps);
        IDirect3DDevice9_SetVertexDeclaration(g_dev, g_decl);
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    } else {
        IDirect3DDevice9_SetVertexShader(g_dev, NULL);
        IDirect3DDevice9_SetPixelShader(g_dev, NULL);
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLD, (const D3DMATRIX *)&world);
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, (const D3DMATRIX *)&view);
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, (const D3DMATRIX *)&proj);
        IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, TRUE);
        IDirect3DDevice9_SetFVF(g_dev, D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
    }
    IDirect3DDevice9_DrawIndexedPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 0, 24, 12, g_idx, D3DFMT_INDEX16, g_vtx, sizeof(Vtx));
    IDirect3DDevice9_EndScene(g_dev);
    IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_LBUTTONDOWN: g_drag = 1; g_lx = (short)LOWORD(lp); g_ly = (short)HIWORD(lp); SetCapture(h); return 0;
    case WM_MOUSEMOVE:
        if (g_drag) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            g_yaw -= (x - g_lx) * 0.5f; g_pitch += (y - g_ly) * 0.5f;
            g_lx = x; g_ly = y;
        }
        return 0;
    case WM_LBUTTONUP: g_drag = 0; ReleaseCapture(); return 0;
    case WM_KEYDOWN:
        if (wp == 'F' && g_vs && g_ps && g_decl) g_shaders = !g_shaders;
        else if (wp == VK_LEFT) g_yaw += 5;
        else if (wp == VK_RIGHT) g_yaw -= 5;
        else if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show) {
    g_fixed = cmd && strstr(cmd, "--sabit") != NULL;
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"SoulKupD3D9";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassW(&wc);
    RECT rc = { 0, 0, CW, CH };
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_wnd = CreateWindowW(L"SoulKupD3D9", L"Küp 3B (Direct3D 9)", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                          rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, hi, NULL);
    if (!init_d3d(g_wnd)) {
        MessageBoxW(g_wnd, L"Direct3D 9 aygıtı oluşturulamadı.", L"Küp 3B (Direct3D 9)", MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(g_wnd, show);
    MSG msg;
    DWORD start = GetTickCount(), frames = 0, total = 0, last = start;
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DWORD now = GetTickCount();
        g_spin = g_fixed ? 35 : (now - start) * 0.06f;
        render();
        frames++; total++;
        if (now - last >= 1000) {
            wchar_t t[96];
            swprintf(t, 96, L"Küp 3B (Direct3D 9) — %lu FPS — kare %lu%ls", (unsigned long)(frames * 1000 / (now - last)), (unsigned long)total,
                     g_shaders ? L"" : L" (sabit işlev)");
            SetWindowTextW(g_wnd, t);
            frames = 0; last = now;
        }
        Sleep(1);
    }
done:
    if (g_tex) IDirect3DTexture9_Release(g_tex);
    if (g_vs) IDirect3DVertexShader9_Release(g_vs);
    if (g_ps) IDirect3DPixelShader9_Release(g_ps);
    if (g_decl) IDirect3DVertexDeclaration9_Release(g_decl);
    IDirect3DDevice9_Release(g_dev);
    IDirect3D9_Release(g_d3d);
    printf("çıkış\n");
    return 0;
}
