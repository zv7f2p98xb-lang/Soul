// Saat - analog ve dijital saat (Soul GUI örneği: zamanlayıcı, çift tamponlu GDI çizimi)
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <math.h>
#include <stdio.h>

static const wchar_t *k_days[] = { L"Pazar", L"Pazartesi", L"Salı", L"Çarşamba", L"Perşembe", L"Cuma", L"Cumartesi" };
static const wchar_t *k_months[] = { L"Ocak", L"Şubat", L"Mart", L"Nisan", L"Mayıs", L"Haziran", L"Temmuz", L"Ağustos", L"Eylül", L"Ekim", L"Kasım", L"Aralık" };

static void hand(HDC dc, int cx, int cy, double angle, double len, int width, COLORREF c) {
    HPEN p = CreatePen(PS_SOLID, width, c);
    HGDIOBJ o = SelectObject(dc, p);
    MoveToEx(dc, cx - (int)(sin(angle) * len * 0.15), cy + (int)(cos(angle) * len * 0.15), NULL);
    LineTo(dc, cx + (int)(sin(angle) * len), cy - (int)(cos(angle) * len));
    SelectObject(dc, o);
    DeleteObject(p);
}

static void draw(HDC dc, int w, int h) {
    RECT rc = { 0, 0, w, h };
    HBRUSH bg = CreateSolidBrush(RGB(24, 28, 38));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SYSTEMTIME t;
    GetLocalTime(&t);
    int cx = w / 2, cy = h / 2 - 30;
    int r = (w < h - 80 ? w : h - 80) / 2 - 20;
    if (r < 40) r = 40;
    HBRUSH face = CreateSolidBrush(RGB(245, 245, 240));
    HPEN rim = CreatePen(PS_SOLID, 6, RGB(212, 175, 55));
    HGDIOBJ ob = SelectObject(dc, face), op = SelectObject(dc, rim);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(face); DeleteObject(rim);
    for (int i = 0; i < 60; i++) {
        double a = i * 3.14159265 / 30;
        int len = i % 5 == 0 ? r / 8 : r / 20;
        HPEN p = CreatePen(PS_SOLID, i % 5 == 0 ? 3 : 1, RGB(40, 40, 40));
        HGDIOBJ o = SelectObject(dc, p);
        MoveToEx(dc, cx + (int)(sin(a) * (r - 8)), cy - (int)(cos(a) * (r - 8)), NULL);
        LineTo(dc, cx + (int)(sin(a) * (r - 8 - len)), cy - (int)(cos(a) * (r - 8 - len)));
        SelectObject(dc, o);
        DeleteObject(p);
    }
    HFONT f = CreateFontW(-(r / 7), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(40, 40, 40));
    for (int i = 1; i <= 12; i++) {
        double a = i * 3.14159265 / 6;
        wchar_t s[4];
        swprintf(s, 4, L"%d", i);
        RECT nr = { cx + (int)(sin(a) * r * 0.72) - 30, cy - (int)(cos(a) * r * 0.72) - 20, cx + (int)(sin(a) * r * 0.72) + 30, cy - (int)(cos(a) * r * 0.72) + 20 };
        DrawTextW(dc, s, -1, &nr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, of);
    DeleteObject(f);
    double sec = t.wSecond + t.wMilliseconds / 1000.0;
    hand(dc, cx, cy, (t.wHour % 12 + t.wMinute / 60.0) * 3.14159265 / 6, r * 0.5, 8, RGB(30, 30, 30));
    hand(dc, cx, cy, (t.wMinute + sec / 60.0) * 3.14159265 / 30, r * 0.75, 5, RGB(30, 30, 30));
    hand(dc, cx, cy, sec * 3.14159265 / 30, r * 0.85, 2, RGB(220, 40, 40));
    HBRUSH hub = CreateSolidBrush(RGB(220, 40, 40));
    ob = SelectObject(dc, hub);
    op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - 7, cy - 7, cx + 8, cy + 8);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(hub);
    wchar_t digital[64], date[96];
    swprintf(digital, 64, L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    swprintf(date, 96, L"%d %ls %d, %ls", t.wDay, k_months[t.wMonth - 1], t.wYear, k_days[t.wDayOfWeek]);
    HFONT big = CreateFontW(-34, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Consolas");
    of = SelectObject(dc, big);
    SetTextColor(dc, RGB(240, 240, 240));
    RECT dr = { 0, cy + r + 8, w, cy + r + 50 };
    DrawTextW(dc, digital, -1, &dr, DT_CENTER | DT_SINGLELINE);
    SelectObject(dc, of);
    DeleteObject(big);
    SetTextColor(dc, RGB(170, 180, 200));
    RECT tr = { 0, cy + r + 50, w, cy + r + 76 };
    DrawTextW(dc, date, -1, &tr, DT_CENTER | DT_SINGLELINE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: SetTimer(h, 1, 100, NULL); return 0;
    case WM_TIMER: InvalidateRect(h, NULL, FALSE); return 0;
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
    case WM_DESTROY: KillTimer(h, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.lpszClassName = L"SoulSaat";
    RegisterClassW(&wc);
    HWND h = CreateWindowW(L"SoulSaat", L"Saat", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 520, 600, NULL, NULL, hi, NULL);
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
