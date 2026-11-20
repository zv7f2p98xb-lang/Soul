// Hesap Makinesi (GUI) - düğmeli hesap makinesi (Soul GUI örneği: BUTTON/STATIC denetimleri, menü, klavye)
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define IDC_DISPLAY 100
#define IDC_HISTORY 101
#define IDM_COPY 200
#define IDM_ABOUT 201
#define IDM_EXIT 202

static HWND g_display, g_history;
static HFONT g_big, g_btn;
static wchar_t g_text[64] = L"0";
static double g_acc;
static wchar_t g_op;
static int g_fresh = 1;

static const struct { const wchar_t *label; int id; } k_keys[] = {
    { L"C", 'C' }, { L"±", 'N' }, { L"%", '%' }, { L"÷", '/' },
    { L"7", '7' }, { L"8", '8' }, { L"9", '9' }, { L"×", '*' },
    { L"4", '4' }, { L"5", '5' }, { L"6", '6' }, { L"−", '-' },
    { L"1", '1' }, { L"2", '2' }, { L"3", '3' }, { L"+", '+' },
    { L"⌫", 'B' }, { L"0", '0' }, { L",", '.' }, { L"=", '=' },
};

static double value(void) { wchar_t t[64]; wcscpy(t, g_text); for (wchar_t *p = t; *p; p++) if (*p == ',') *p = '.'; return wcstod(t, NULL); }
static void show(double v) {
    if (v != v || v > 1e15 || v < -1e15) { wcscpy(g_text, L"Hata"); return; }
    swprintf(g_text, 64, L"%.10g", v);
    for (wchar_t *p = g_text; *p; p++) if (*p == '.') *p = ',';
}
static double apply(double a, double b, wchar_t op) {
    switch (op) { case '+': return a + b; case '-': return a - b; case '*': return a * b; case '/': return b == 0 ? 0.0 / 0.0 : a / b; }
    return b;
}
static void press(int k) {
    wchar_t hist[128] = L"";
    if (k >= '0' && k <= '9') {
        if (g_fresh || !wcscmp(g_text, L"0")) { g_text[0] = 0; g_fresh = 0; }
        size_t n = wcslen(g_text);
        if (n < 15) { g_text[n] = (wchar_t)k; g_text[n + 1] = 0; }
    } else if (k == '.') {
        if (g_fresh) { wcscpy(g_text, L"0"); g_fresh = 0; }
        if (!wcschr(g_text, ',')) wcscat(g_text, L",");
    } else if (k == 'C') { wcscpy(g_text, L"0"); g_acc = 0; g_op = 0; g_fresh = 1;
    } else if (k == 'B') { size_t n = wcslen(g_text); if (!g_fresh && n > 1) g_text[n - 1] = 0; else wcscpy(g_text, L"0");
    } else if (k == 'N') { show(-value());
    } else if (k == '%') { show(value() / 100.0);
    } else if (k == '=') {
        if (g_op) {
            double b = value(), r = apply(g_acc, b, g_op);
            wchar_t a[64]; wcscpy(a, g_text);
            swprintf(hist, 128, L"%.10g %lc %ls =", g_acc, g_op == '*' ? L'×' : g_op == '/' ? L'÷' : g_op, a);
            show(r);
            g_op = 0;
        }
        g_fresh = 1;
    } else { // + - * /
        if (g_op && !g_fresh) show(apply(g_acc, value(), g_op));
        g_acc = value();
        g_op = (wchar_t)k;
        g_fresh = 1;
        swprintf(hist, 128, L"%ls %lc", g_text, k == '*' ? L'×' : k == '/' ? L'÷' : (wchar_t)k);
    }
    SetWindowTextW(g_display, g_text);
    if (hist[0] || k == 'C') SetWindowTextW(g_history, hist);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCTW *)lp)->hInstance;
        g_big = CreateFontW(-40, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        g_btn = CreateFontW(-22, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        HFONT small = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        g_history = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT, 12, 10, 296, 20, h, (HMENU)IDC_HISTORY, hi, NULL);
        g_display = CreateWindowW(L"STATIC", L"0", WS_CHILD | WS_VISIBLE | SS_RIGHT, 12, 32, 296, 52, h, (HMENU)IDC_DISPLAY, hi, NULL);
        SendMessageW(g_history, WM_SETFONT, (WPARAM)small, 0);
        SendMessageW(g_display, WM_SETFONT, (WPARAM)g_big, 0);
        for (int i = 0; i < 20; i++) {
            int r = i / 4, c = i % 4;
            HWND b = CreateWindowW(L"BUTTON", k_keys[i].label, WS_CHILD | WS_VISIBLE | (k_keys[i].id == '=' ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                                   12 + c * 75, 94 + r * 62, 71, 58, h, (HMENU)(INT_PTR)(1000 + i), hi, NULL);
            SendMessageW(b, WM_SETFONT, (WPARAM)g_btn, 0);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkColor(dc, RGB(243, 243, 243));
        SetTextColor(dc, (HWND)lp == g_history ? RGB(110, 110, 110) : RGB(20, 20, 20));
        static HBRUSH bg;
        if (!bg) bg = CreateSolidBrush(RGB(243, 243, 243));
        return (LRESULT)bg;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= 1000 && id < 1020) { press(k_keys[id - 1000].id); SetFocus(h); }
        else if (id == IDM_COPY) {
            if (OpenClipboard(h)) {
                EmptyClipboard();
                size_t n = (wcslen(g_text) + 1) * sizeof(wchar_t);
                HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n);
                memcpy(GlobalLock(mem), g_text, n);
                GlobalUnlock(mem);
                SetClipboardData(CF_UNICODETEXT, mem);
                CloseClipboard();
            }
        } else if (id == IDM_ABOUT) MessageBoxW(h, L"Soul ile çalışan düğmeli hesap makinesi.\nKlavyeden de kullanılabilir.", L"Hesap Makinesi", MB_OK | MB_ICONINFORMATION);
        else if (id == IDM_EXIT) DestroyWindow(h);
        return 0;
    }
    case WM_CHAR: {
        wchar_t c = (wchar_t)wp;
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '*' || c == '/' || c == '%') press(c);
        else if (c == ',' || c == '.') press('.');
        else if (c == '=' || c == '\r') press('=');
        else if (c == 8) press('B');
        else if (c == 27 || c == 'c' || c == 'C') press('C');
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
    wc.hbrBackground = CreateSolidBrush(RGB(243, 243, 243));
    wc.lpszClassName = L"SoulHesap";
    RegisterClassW(&wc);
    HMENU bar = CreateMenu(), edit = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_COPY, L"&Kopyala");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, IDM_EXIT, L"Çı&kış");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&Hakkında");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Düzen");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Yardım");
    HWND h = CreateWindowW(L"SoulHesap", L"Hesap Makinesi", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 320, 410, NULL, bar, hi, NULL);
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
