// Çizim - parmakla / fareyle resim çizme (Soul GUI örneği)
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

#define IDM_NEW 1
#define IDM_SAVE 2
#define IDM_EXIT 3
#define IDM_THIN 10
#define IDM_MEDIUM 11
#define IDM_THICK 12
#define IDM_ABOUT 20
#define PAL_H 44

static const COLORREF k_colors[] = { RGB(20, 20, 20), RGB(230, 57, 70), RGB(244, 162, 97), RGB(233, 196, 106), RGB(42, 157, 143),
                                     RGB(38, 70, 83), RGB(69, 123, 157), RGB(155, 93, 229), RGB(255, 255, 255) };
#define NCOLORS (int)(sizeof k_colors / sizeof k_colors[0])
static HDC g_canvas;
static HBITMAP g_bmp;
static int g_cw, g_ch, g_color, g_width = 6, g_drawing, g_lastx, g_lasty;

static void clear_canvas(void) {
    RECT r = { 0, 0, g_cw, g_ch };
    FillRect(g_canvas, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
}

static void ensure_canvas(HWND h, int w, int hh) {
    if (w <= g_cw && hh <= g_ch && g_canvas) return;
    HDC dc = GetDC(h);
    HDC ndc = CreateCompatibleDC(dc);
    int nw = w > g_cw ? w : g_cw, nh = hh > g_ch ? hh : g_ch;
    HBITMAP nb = CreateCompatibleBitmap(dc, nw, nh);
    SelectObject(ndc, nb);
    RECT r = { 0, 0, nw, nh };
    FillRect(ndc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
    if (g_canvas) { BitBlt(ndc, 0, 0, g_cw, g_ch, g_canvas, 0, 0, SRCCOPY); DeleteDC(g_canvas); DeleteObject(g_bmp); }
    g_canvas = ndc; g_bmp = nb; g_cw = nw; g_ch = nh;
    ReleaseDC(h, dc);
}

static void stroke(HWND h, int x0, int y0, int x1, int y1) {
    HPEN pen = CreatePen(PS_SOLID, g_width, k_colors[g_color]);
    HGDIOBJ old = SelectObject(g_canvas, pen);
    MoveToEx(g_canvas, x0, y0 - PAL_H, NULL);
    LineTo(g_canvas, x1, y1 - PAL_H);
    SelectObject(g_canvas, old);
    DeleteObject(pen);
    RECT r = { (x0 < x1 ? x0 : x1) - g_width, (y0 < y1 ? y0 : y1) - g_width, (x0 > x1 ? x0 : x1) + g_width + 1, (y0 > y1 ? y0 : y1) + g_width + 1 };
    InvalidateRect(h, &r, FALSE);
}

static void save_bmp(HWND h) {
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_cw;
    bi.bmiHeader.biHeight = g_ch;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    int stride = ((g_cw * 3) + 3) & ~3;
    DWORD size = (DWORD)(stride * g_ch);
    BYTE *bits = malloc(size);
    GetDIBits(g_canvas, g_bmp, 0, (UINT)g_ch, bits, &bi, DIB_RGB_COLORS);
    wchar_t path[MAX_PATH];
    ExpandEnvironmentStringsW(L"%USERPROFILE%\\Documents\\cizim.bmp", path, MAX_PATH);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        BITMAPFILEHEADER fh = { 0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + size), 0, 0, (DWORD)(sizeof fh + sizeof bi.bmiHeader) };
        DWORD n;
        WriteFile(f, &fh, sizeof fh, &n, NULL);
        WriteFile(f, &bi.bmiHeader, sizeof bi.bmiHeader, &n, NULL);
        WriteFile(f, bits, size, &n, NULL);
        CloseHandle(f);
        wchar_t msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"Resim kaydedildi:\n%ls", path);
        MessageBoxW(h, msg, L"Çizim", MB_ICONINFORMATION);
    }
    free(bits);
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    RECT bar = { 0, 0, rc.right, PAL_H };
    HBRUSH bg = CreateSolidBrush(RGB(236, 239, 244));
    FillRect(dc, &bar, bg);
    DeleteObject(bg);
    for (int i = 0; i < NCOLORS; i++) {
        HBRUSH b = CreateSolidBrush(k_colors[i]);
        HPEN p = CreatePen(PS_SOLID, i == g_color ? 3 : 1, i == g_color ? RGB(0, 120, 215) : RGB(150, 150, 150));
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
        Ellipse(dc, 8 + i * 40, 6, 40 + i * 40, 38);
        SelectObject(dc, ob); SelectObject(dc, op);
        DeleteObject(b); DeleteObject(p);
    }
    int sx = 8 + NCOLORS * 40 + 16;
    HBRUSH dot = CreateSolidBrush(k_colors[g_color]);
    HGDIOBJ ob = SelectObject(dc, dot), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, sx + 16 - g_width / 2, 22 - g_width / 2, sx + 16 + (g_width + 1) / 2, 22 + (g_width + 1) / 2);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(dot);
    RECT clr = { rc.right - 96, 8, rc.right - 8, 36 };
    DrawEdge(dc, &clr, EDGE_RAISED, BF_RECT | BF_MIDDLE);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, L"Temizle", -1, &clr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    BitBlt(dc, 0, PAL_H, rc.right, rc.bottom - PAL_H, g_canvas, 0, 0, SRCCOPY);
    EndPaint(h, &ps);
}

static void set_width(HWND h, int id) {
    g_width = id == IDM_THIN ? 2 : id == IDM_MEDIUM ? 6 : 14;
    CheckMenuRadioItem(GetMenu(h), IDM_THIN, IDM_THICK, id, MF_BYCOMMAND);
    RECT r = { 0, 0, 2000, PAL_H };
    InvalidateRect(h, &r, FALSE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        RECT rc;
        GetClientRect(h, &rc);
        ensure_canvas(h, rc.right, rc.bottom);
        clear_canvas();
        return 0;
    }
    case WM_SIZE: ensure_canvas(h, LOWORD(lp), HIWORD(lp)); return 0;
    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        RECT rc;
        GetClientRect(h, &rc);
        if (y < PAL_H) {
            if (x >= rc.right - 96) { clear_canvas(); InvalidateRect(h, NULL, FALSE); }
            else if (x >= 8 && x < 8 + NCOLORS * 40) { g_color = (x - 8) / 40; RECT r = { 0, 0, rc.right, PAL_H }; InvalidateRect(h, &r, FALSE); }
            return 0;
        }
        g_drawing = 1; g_lastx = x; g_lasty = y;
        SetCapture(h);
        stroke(h, x, y, x, y);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_drawing) {
            int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
            stroke(h, g_lastx, g_lasty, x, y);
            g_lastx = x; g_lasty = y;
        }
        return 0;
    case WM_LBUTTONUP: g_drawing = 0; ReleaseCapture(); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_NEW: clear_canvas(); InvalidateRect(h, NULL, FALSE); break;
        case IDM_SAVE: save_bmp(h); break;
        case IDM_EXIT: DestroyWindow(h); break;
        case IDM_THIN: case IDM_MEDIUM: case IDM_THICK: set_width(h, LOWORD(wp)); break;
        case IDM_ABOUT: MessageBoxW(h, L"Soul ile iPad'de çalışan bir Windows çizim programı.\nRenk seçip parmağınızla çizin.", L"Çizim hakkında", MB_OK); break;
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_CROSS);
    wc.lpszClassName = L"SoulCizim";
    RegisterClassW(&wc);
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), brush = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_NEW, L"&Yeni");
    AppendMenuW(file, MF_STRING, IDM_SAVE, L"&Kaydet (Belgeler\\cizim.bmp)");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"Çı&kış");
    AppendMenuW(brush, MF_STRING, IDM_THIN, L"İnce");
    AppendMenuW(brush, MF_STRING, IDM_MEDIUM, L"Orta");
    AppendMenuW(brush, MF_STRING, IDM_THICK, L"Kalın");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&Hakkında");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&Dosya");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)brush, L"&Fırça");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Yardım");
    CheckMenuRadioItem(brush, IDM_THIN, IDM_THICK, IDM_MEDIUM, MF_BYCOMMAND);
    HWND h = CreateWindowW(L"SoulCizim", L"Çizim", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 600, NULL, bar, hi, NULL);
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
