// Yılan - klasik yılan oyunu (Soul GUI örneği: zamanlayıcı, klavye ve dokunmatik girdi)
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#define CELL 24
#define COLS 24
#define ROWS 18
#define TOP 48

static int g_x[COLS * ROWS], g_y[COLS * ROWS], g_len, g_dx, g_dy, g_ndx, g_ndy, g_fx, g_fy, g_score, g_best, g_over, g_paused;

static void place_food(void) {
    for (;;) {
        g_fx = rand() % COLS; g_fy = rand() % ROWS;
        int ok = 1;
        for (int i = 0; i < g_len; i++) if (g_x[i] == g_fx && g_y[i] == g_fy) ok = 0;
        if (ok) return;
    }
}
static void reset(void) {
    g_len = 4; g_dx = 1; g_dy = 0; g_ndx = 1; g_ndy = 0; g_score = 0; g_over = 0; g_paused = 0;
    for (int i = 0; i < g_len; i++) { g_x[i] = 8 - i; g_y[i] = ROWS / 2; }
    place_food();
}
static void turn(int dx, int dy) {
    if (g_over) return;
    if (dx == -g_dx && dy == -g_dy) return;
    g_ndx = dx; g_ndy = dy;
}
static void step(HWND h) {
    if (g_over || g_paused) return;
    g_dx = g_ndx; g_dy = g_ndy;
    int nx = g_x[0] + g_dx, ny = g_y[0] + g_dy;
    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) { g_over = 1; }
    for (int i = 0; i < g_len - 1 && !g_over; i++) if (g_x[i] == nx && g_y[i] == ny) g_over = 1;
    if (g_over) { if (g_score > g_best) g_best = g_score; InvalidateRect(h, NULL, FALSE); return; }
    int grow = nx == g_fx && ny == g_fy;
    if (grow && g_len < COLS * ROWS) g_len++;
    for (int i = g_len - 1; i > 0; i--) { g_x[i] = g_x[i - 1]; g_y[i] = g_y[i - 1]; }
    g_x[0] = nx; g_y[0] = ny;
    if (grow) { g_score += 10; place_food(); }
    InvalidateRect(h, NULL, FALSE);
}

static void fill(HDC dc, int x0, int y0, int x1, int y1, COLORREF c) {
    RECT r = { x0, y0, x1, y1 };
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void draw(HDC dc, int w, int h) {
    fill(dc, 0, 0, w, h, RGB(30, 36, 48));
    int ox = (w - COLS * CELL) / 2;
    if (ox < 0) ox = 0;
    fill(dc, ox, TOP, ox + COLS * CELL, TOP + ROWS * CELL, RGB(46, 125, 50));
    for (int y = 0; y < ROWS; y++) for (int x = 0; x < COLS; x++)
        if ((x + y) & 1) fill(dc, ox + x * CELL, TOP + y * CELL, ox + (x + 1) * CELL, TOP + (y + 1) * CELL, RGB(56, 142, 60));
    HBRUSH apple = CreateSolidBrush(RGB(229, 57, 53));
    HGDIOBJ ob = SelectObject(dc, apple), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, ox + g_fx * CELL + 3, TOP + g_fy * CELL + 3, ox + (g_fx + 1) * CELL - 2, TOP + (g_fy + 1) * CELL - 2);
    SelectObject(dc, ob);
    DeleteObject(apple);
    for (int i = g_len - 1; i >= 0; i--) {
        COLORREF c = i == 0 ? RGB(255, 213, 79) : (i % 2 ? RGB(253, 216, 53) : RGB(251, 192, 45));
        HBRUSH b = CreateSolidBrush(c);
        ob = SelectObject(dc, b);
        RoundRect(dc, ox + g_x[i] * CELL + 1, TOP + g_y[i] * CELL + 1, ox + (g_x[i] + 1) * CELL - 1, TOP + (g_y[i] + 1) * CELL - 1, 8, 8);
        SelectObject(dc, ob);
        DeleteObject(b);
    }
    SelectObject(dc, op);
    // eyes
    fill(dc, ox + g_x[0] * CELL + 6, TOP + g_y[0] * CELL + 6, ox + g_x[0] * CELL + 10, TOP + g_y[0] * CELL + 10, RGB(0, 0, 0));
    fill(dc, ox + g_x[0] * CELL + 14, TOP + g_y[0] * CELL + 6, ox + g_x[0] * CELL + 18, TOP + g_y[0] * CELL + 10, RGB(0, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    HFONT f = CreateFontW(-22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, RGB(255, 255, 255));
    wchar_t s[96];
    swprintf(s, 96, L"Puan: %d     En iyi: %d", g_score, g_best);
    RECT tr = { ox, 10, ox + COLS * CELL, TOP };
    DrawTextW(dc, s, -1, &tr, DT_LEFT | DT_SINGLELINE);
    SetTextColor(dc, RGB(170, 180, 200));
    DrawTextW(dc, g_paused ? L"Duraklatıldı" : L"Ok tuşları / dokunma", -1, &tr, DT_RIGHT | DT_SINGLELINE);
    if (g_over) {
        fill(dc, ox + 60, TOP + ROWS * CELL / 2 - 60, ox + COLS * CELL - 60, TOP + ROWS * CELL / 2 + 60, RGB(20, 20, 20));
        RECT r = { ox, TOP + ROWS * CELL / 2 - 50, ox + COLS * CELL, TOP + ROWS * CELL / 2 };
        SetTextColor(dc, RGB(255, 120, 120));
        DrawTextW(dc, L"Oyun bitti!", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        RECT r2 = { ox, TOP + ROWS * CELL / 2, ox + COLS * CELL, TOP + ROWS * CELL / 2 + 50 };
        SetTextColor(dc, RGB(230, 230, 230));
        DrawTextW(dc, L"Yeniden başlamak için dokunun veya Boşluk'a basın", -1, &r2, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
    }
    SelectObject(dc, of);
    DeleteObject(f);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: srand(GetTickCount()); reset(); SetTimer(h, 1, 120, NULL); return 0;
    case WM_TIMER: step(h); return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT: case 'A': turn(-1, 0); break;
        case VK_RIGHT: case 'D': turn(1, 0); break;
        case VK_UP: case 'W': turn(0, -1); break;
        case VK_DOWN: case 'S': turn(0, 1); break;
        case VK_SPACE: if (g_over) reset(); else g_paused = !g_paused; InvalidateRect(h, NULL, FALSE); break;
        case VK_ESCAPE: DestroyWindow(h); break;
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (g_over) { reset(); InvalidateRect(h, NULL, FALSE); return 0; }
        RECT rc;
        GetClientRect(h, &rc);
        int ox = (rc.right - COLS * CELL) / 2;
        int hx = ox + g_x[0] * CELL + CELL / 2, hy = TOP + g_y[0] * CELL + CELL / 2;
        int dx = (short)LOWORD(lp) - hx, dy = (short)HIWORD(lp) - hy;
        if (g_dx != 0) turn(0, dy < 0 ? -1 : 1);   // moving horizontally: tap above/below to turn
        else turn(dx < 0 ? -1 : 1, 0);              // moving vertically: tap left/right
        return 0;
    }
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
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
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
    wc.lpszClassName = L"SoulYilan";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(L"SoulYilan", L"Yılan", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                           COLS * CELL + 40, TOP + ROWS * CELL + 20, NULL, NULL, hi, NULL);
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
