// Küp 3B - OpenGL ile dönen dokulu ve ışıklı küp (Soul grafik katmanı örneği)
// Dokunarak/sürükleyerek döndürün, Boşluk: tel kafes, F: sis.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <GL/gl.h>
#include <GL/glu.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

static HDC g_dc;
static HGLRC g_rc;
static GLuint g_tex, g_floor_list;
static float g_yaw = 30, g_pitch = 20, g_spin;
static int g_drag, g_lx, g_ly, g_wire, g_fog = 1, g_fixed, g_w = 800, g_h = 600;
static GLUquadric *g_quad;

static void make_texture(void) {
    static unsigned char px[64][64][3];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        int check = ((x / 8) + (y / 8)) & 1;
        int border = x < 3 || y < 3 || x > 60 || y > 60;
        unsigned char r = check ? 240 : 40, g = check ? 200 : 120, b = check ? 60 : 220;
        if (border) r = g = b = 255;
        px[y][x][0] = r; px[y][x][1] = g; px[y][x][2] = b;
    }
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    gluBuild2DMipmaps(GL_TEXTURE_2D, GL_RGB, 64, 64, GL_RGB, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}

static void make_floor(void) {
    g_floor_list = glGenLists(1);
    glNewList(g_floor_list, GL_COMPILE);
    glDisable(GL_LIGHTING);
    glDisable(GL_TEXTURE_2D);
    glBegin(GL_LINES);
    for (int i = -10; i <= 10; i++) {
        glColor3f(0.25f, 0.55f, 0.65f);
        glVertex3f((float)i, -1.5f, -10); glVertex3f((float)i, -1.5f, 10);
        glVertex3f(-10, -1.5f, (float)i); glVertex3f(10, -1.5f, (float)i);
    }
    glEnd();
    glEndList();
}

static void cube(void) {
    static const float v[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
    static const int f[6][4] = { {4,5,6,7},{1,0,3,2},{5,1,2,6},{0,4,7,3},{7,6,2,3},{0,1,5,4} };
    static const float n[6][3] = { {0,0,1},{0,0,-1},{1,0,0},{-1,0,0},{0,1,0},{0,-1,0} };
    static const float uv[4][2] = { {0,0},{1,0},{1,1},{0,1} };
    glBegin(GL_QUADS);
    for (int i = 0; i < 6; i++) {
        glNormal3fv(n[i]);
        for (int k = 0; k < 4; k++) { glTexCoord2fv(uv[k]); glVertex3fv(v[f[i][k]]); }
    }
    glEnd();
}

static void render(void) {
    glViewport(0, 0, g_w, g_h);
    glClearColor(0.08f, 0.10f, 0.16f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(55.0, (double)g_w / (g_h ? g_h : 1), 0.5, 60.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAt(0, 1.2, 7, 0, 0, 0, 0, 1, 0);
    glRotatef(g_pitch, 1, 0, 0);
    glRotatef(g_yaw, 0, 1, 0);
    float light_pos[4] = { 3, 5, 4, 1 };
    glLightfv(GL_LIGHT0, GL_POSITION, light_pos);

    if (g_fog) { glEnable(GL_FOG); float fc[4] = { 0.08f, 0.10f, 0.16f, 1 }; glFogfv(GL_FOG_COLOR, fc); glFogi(GL_FOG_MODE, GL_LINEAR); glFogf(GL_FOG_START, 6); glFogf(GL_FOG_END, 18); }
    else glDisable(GL_FOG);
    glCallList(g_floor_list);

    glPolygonMode(GL_FRONT_AND_BACK, g_wire ? GL_LINE : GL_FILL);
    // textured, lit cube
    glEnable(GL_LIGHTING);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glColor3f(1, 1, 1);
    glPushMatrix();
    glTranslatef(-1.4f, 0, 0);
    glRotatef(g_spin, 0.3f, 1, 0.2f);
    cube();
    glPopMatrix();
    // shiny sphere
    glDisable(GL_TEXTURE_2D);
    float red[4] = { 0.9f, 0.25f, 0.3f, 1 }, white[4] = { 1, 1, 1, 1 };
    glMaterialfv(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE, red);
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, white);
    glMaterialf(GL_FRONT_AND_BACK, GL_SHININESS, 40);
    glDisable(GL_COLOR_MATERIAL);
    glPushMatrix();
    glTranslatef(1.6f, 0, 0);
    gluSphere(g_quad, 1.0, 32, 20);
    glPopMatrix();
    glEnable(GL_COLOR_MATERIAL);
    float black[4] = { 0, 0, 0, 1 };
    glMaterialfv(GL_FRONT_AND_BACK, GL_SPECULAR, black);
    glDisable(GL_LIGHTING);
    // transparent pane through vertex arrays
    static const float pane[] = { -2.6f, -1.2f, 1.6f, 2.6f, -1.2f, 1.6f, 2.6f, 0.4f, 1.6f, -2.6f, 0.4f, 1.6f };
    static const unsigned char pane_col[] = { 80, 200, 255, 90, 80, 200, 255, 90, 255, 255, 255, 40, 255, 255, 255, 40 };
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, pane);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, pane_col);
    glDrawArrays(GL_QUADS, 0, 4);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    SwapBuffers(g_dc);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE: g_w = LOWORD(lp); g_h = HIWORD(lp); return 0;
    case WM_LBUTTONDOWN: g_drag = 1; g_lx = (short)LOWORD(lp); g_ly = (short)HIWORD(lp); SetCapture(h); return 0;
    case WM_MOUSEMOVE:
        if (g_drag) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            g_yaw += (x - g_lx) * 0.5f; g_pitch += (y - g_ly) * 0.5f;
            g_lx = x; g_ly = y;
        }
        return 0;
    case WM_LBUTTONUP: g_drag = 0; ReleaseCapture(); return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE) g_wire = !g_wire;
        else if (wp == 'F') g_fog = !g_fog;
        else if (wp == VK_LEFT) g_yaw -= 5;
        else if (wp == VK_RIGHT) g_yaw += 5;
        else if (wp == VK_ESCAPE) DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show) {
    g_fixed = cmd && strstr(cmd, "--sabit") != NULL;
    WNDCLASSW wc = { 0 };
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"SoulKup";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(L"SoulKup", L"Küp 3B (OpenGL)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, NULL, NULL, hi, NULL);
    g_dc = GetDC(h);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
    g_rc = wglCreateContext(g_dc);
    wglMakeCurrent(g_dc, g_rc);
    printf("GL: %s | %s | %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_LIGHT0);
    glEnable(GL_CULL_FACE);
    glEnable(GL_COLOR_MATERIAL);
    glShadeModel(GL_SMOOTH);
    g_quad = gluNewQuadric();
    make_texture();
    make_floor();
    printf("GL hata: %u\n", glGetError());
    ShowWindow(h, show);
    MSG msg;
    DWORD start = GetTickCount(), frames = 0, last = start;
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DWORD now = GetTickCount();
        g_spin = g_fixed ? 35 : (now - start) * 0.06f;
        render();
        frames++;
        if (now - last >= 1000) {
            wchar_t t[64];
            swprintf(t, 64, L"Küp 3B (OpenGL) — %lu FPS", (unsigned long)(frames * 1000 / (now - last)));
            SetWindowTextW(h, t);
            frames = 0; last = now;
        }
    }
done:
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(g_rc);
    printf("çıkış\n");
    return 0;
}
