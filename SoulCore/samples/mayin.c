// Mayın Tarlası - mayın bulma oyunu (Soul GUI örneği: fare/dokunma, sağ tık ile bayrak, zamanlayıcı)
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define W 12
#define H 12
#define MINES 20
#define CELL 34
#define TOP 56
#define PAD 12

static int g_mine[H][W], g_open[H][W], g_flag[H][W], g_count[H][W];
static int g_state, g_opened, g_flags, g_seconds, g_started; // state 0 playing, 1 won, 2 lost
static int g_flag_mode;

static void reset(void) {
    memset(g_mine, 0, sizeof g_mine); memset(g_open, 0, sizeof g_open); memset(g_flag, 0, sizeof g_flag);
    g_state = 0; g_opened = 0; g_flags = 0; g_seconds = 0; g_started = 0;
}
static void place(int sx, int sy) { // mines are placed after the first tap so it is never a mine
    int n = 0;
    while (n < MINES) {
        int x = rand() % W, y = rand() % H;
        if (g_mine[y][x] || (abs(x - sx) <= 1 && abs(y - sy) <= 1)) continue;
        g_mine[y][x] = 1; n++;
    }
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int c = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && ny >= 0 && nx < W && ny < H && g_mine[ny][nx]) c++;
        }
        g_count[y][x] = c;
    }
}
static void reveal(int x, int y) {
    if (x < 0 || y < 0 || x >= W || y >= H || g_open[y][x] || g_flag[y][x]) return;
    g_open[y][x] = 1; g_opened++;
    if (g_mine[y][x]) { g_state = 2; return; }
    if (g_count[y][x] == 0) for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dx || dy) reveal(x + dx, y + dy);
    if (g_opened == W * H - MINES && g_state == 0) g_state = 1;
}

static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c) { RECT r = { x0, y0, x1, y1 }; HBRUSH b = CreateSolidBrush(c); FillRect(dc, &r, b); DeleteObject(b); }

static void draw(HDC dc, int cw, int ch) {
    static const COLORREF nums[9] = { 0, RGB(25, 118, 210), RGB(56, 142, 60), RGB(211, 47, 47), RGB(123, 31, 162), RGB(255, 143, 0), RGB(0, 151, 167), RGB(66, 66, 66), RGB(158, 158, 158) };
    fill(dc, 0, 0, cw, ch, RGB(250, 250, 250));
    SetBkMode(dc, TRANSPARENT);
    HFONT f = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    wchar_t s[64];
    swprintf(s, 64, L"Mayın: %d", MINES - g_flags);
    RECT r = { PAD, 12, PAD + 160, 44 };
    SetTextColor(dc, RGB(60, 60, 60));
    DrawTextW(dc, s, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    swprintf(s, 64, L"Süre: %d", g_seconds);
    RECT r2 = { cw - PAD - 160, 12, cw - PAD, 44 };
    DrawTextW(dc, s, -1, &r2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT face = { cw / 2 - 60, 10, cw / 2 + 60, 46 };
    fill(dc, face.left, face.top, face.right, face.bottom, g_flag_mode ? RGB(255, 205, 210) : RGB(225, 232, 240));
    DrawTextW(dc, g_state == 1 ? L"Kazandın!" : g_state == 2 ? L"Yeniden" : g_flag_mode ? L"Bayrak ▲" : L"Kaz", -1, &face, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        int x0 = PAD + x * CELL, y0 = TOP + y * CELL;
        RECT c = { x0 + 1, y0 + 1, x0 + CELL - 1, y0 + CELL - 1 };
        int show_mine = g_mine[y][x] && (g_open[y][x] || g_state == 2);
        if (g_open[y][x] || show_mine) {
            fill(dc, c.left, c.top, c.right, c.bottom, show_mine ? (g_open[y][x] ? RGB(239, 83, 80) : RGB(224, 224, 224)) : RGB(236, 239, 241));
            if (show_mine) {
                HBRUSH b = CreateSolidBrush(RGB(33, 33, 33));
                HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
                Ellipse(dc, x0 + 9, y0 + 9, x0 + CELL - 9, y0 + CELL - 9);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(b);
            } else if (g_count[y][x]) {
                swprintf(s, 64, L"%d", g_count[y][x]);
                SetTextColor(dc, nums[g_count[y][x]]);
                DrawTextW(dc, s, -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        } else {
            fill(dc, c.left, c.top, c.right, c.bottom, RGB(144, 164, 174));
            fill(dc, c.left, c.top, c.right, c.top + 3, RGB(176, 190, 197));
            if (g_flag[y][x]) {
                POINT tri[3] = { { x0 + 12, y0 + 8 }, { x0 + 25, y0 + 13 }, { x0 + 12, y0 + 18 } };
                HBRUSH b = CreateSolidBrush(RGB(229, 57, 53));
                HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
                Polygon(dc, tri, 3);
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(b);
                fill(dc, x0 + 11, y0 + 8, x0 + 13, y0 + 26, RGB(40, 40, 40));
            }
        }
    }
    SelectObject(dc, of);
    DeleteObject(f);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: srand(GetTickCount()); reset(); SetTimer(h, 1, 1000, NULL); return 0;
    case WM_TIMER: if (g_started && g_state == 0) { g_seconds++; RECT r = { 0, 0, 2000, TOP }; InvalidateRect(h, &r, FALSE); } return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        int px = (short)LOWORD(lp), py = (short)HIWORD(lp);
        RECT rc;
        GetClientRect(h, &rc);
        if (py < TOP) {
            if (px > rc.right / 2 - 60 && px < rc.right / 2 + 60) { if (g_state) reset(); else g_flag_mode = !g_flag_mode; InvalidateRect(h, NULL, FALSE); }
            return 0;
        }
        int x = (px - PAD) / CELL, y = (py - TOP) / CELL;
        if (x < 0 || y < 0 || x >= W || y >= H || g_state) return 0;
        if (m == WM_RBUTTONDOWN || g_flag_mode) {
            if (!g_open[y][x]) { g_flag[y][x] = !g_flag[y][x]; g_flags += g_flag[y][x] ? 1 : -1; }
        } else if (!g_flag[y][x]) {
            if (!g_started) { place(x, y); g_started = 1; }
            reveal(x, y);
            if (g_state == 2) MessageBeep(MB_ICONHAND);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_KEYDOWN: if (wp == VK_F2 || wp == VK_SPACE) { reset(); InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        draw(mem, rc.right, rc.bottom);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"SoulMayin";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(L"SoulMayin", L"Mayın Tarlası", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                           W * CELL + 2 * PAD, TOP + H * CELL + PAD, NULL, NULL, hi, NULL);
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
