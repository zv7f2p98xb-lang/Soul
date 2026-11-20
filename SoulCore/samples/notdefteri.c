// Not Defteri - basit metin düzenleyici (Soul GUI örneği: çok satırlı EDIT, menüler, kısayollar, dosya kaydetme)
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>

#define IDC_EDIT 100
#define IDM_NEW 1
#define IDM_OPEN 2
#define IDM_SAVE 3
#define IDM_EXIT 4
#define IDM_CUT 10
#define IDM_COPY 11
#define IDM_PASTE 12
#define IDM_ALL 13
#define IDM_TIME 14
#define IDM_BIGGER 20
#define IDM_SMALLER 21
#define IDM_ABOUT 30

static HWND g_edit, g_status;
static HFONT g_font;
static int g_size = 18;
static wchar_t g_path[MAX_PATH];

static void set_font(void) {
    HFONT f = CreateFontW(-g_size, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, FIXED_PITCH, L"Consolas");
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)f, TRUE);
    if (g_font) DeleteObject(g_font);
    g_font = f;
}
static void update_status(void) {
    int len = GetWindowTextLengthW(g_edit);
    int lines = (int)SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0);
    wchar_t s[128];
    swprintf(s, 128, L"  %d karakter, %d satır  |  %ls", len, lines, g_path);
    SetWindowTextW(g_status, s);
}
static void save(HWND h) {
    int n = GetWindowTextLengthW(g_edit);
    wchar_t *buf = malloc(sizeof(wchar_t) * (size_t)(n + 1));
    GetWindowTextW(g_edit, buf, n + 1);
    int bytes = WideCharToMultiByte(CP_UTF8, 0, buf, n, NULL, 0, NULL, NULL);
    char *u8 = malloc((size_t)bytes + 1);
    WideCharToMultiByte(CP_UTF8, 0, buf, n, u8, bytes, NULL, NULL);
    HANDLE f = CreateFileW(g_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { MessageBoxW(h, L"Dosya kaydedilemedi.", L"Not Defteri", MB_ICONERROR); }
    else { DWORD w; WriteFile(f, u8, (DWORD)bytes, &w, NULL); CloseHandle(f); SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0); SetWindowTextW(h, L"not.txt - Not Defteri"); }
    free(u8); free(buf);
}
static void load(HWND h) {
    HANDLE f = CreateFileW(g_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { MessageBoxW(h, L"Henüz kaydedilmiş bir not yok.", L"Not Defteri", MB_ICONINFORMATION); return; }
    DWORD size = GetFileSize(f, NULL), r;
    char *u8 = malloc(size + 1);
    ReadFile(f, u8, size, &r, NULL);
    CloseHandle(f);
    int n = MultiByteToWideChar(CP_UTF8, 0, u8, (int)r, NULL, 0);
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)(n + 1));
    MultiByteToWideChar(CP_UTF8, 0, u8, (int)r, w, n);
    w[n] = 0;
    SetWindowTextW(g_edit, w);
    free(w); free(u8);
    update_status();
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HINSTANCE hi = ((CREATESTRUCTW *)lp)->hInstance;
        g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                 0, 0, 100, 100, h, (HMENU)IDC_EDIT, hi, NULL);
        g_status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE, 0, 0, 100, 24, h, NULL, hi, NULL);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), 0);
        set_font();
        ExpandEnvironmentStringsW(L"%USERPROFILE%\\Documents\\not.txt", g_path, MAX_PATH);
        SetWindowTextW(g_edit, L"Soul Not Defteri'ne hoş geldiniz!\r\n\r\nBu metin, iPad'de çalışan gerçek bir Windows programının\r\nçok satırlı metin kutusunda. Yazmak için dokunun.\r\n");
        update_status();
        SetFocus(g_edit);
        return 0;
    }
    case WM_SIZE:
        MoveWindow(g_edit, 0, 0, LOWORD(lp), HIWORD(lp) - 24, TRUE);
        MoveWindow(g_status, 0, HIWORD(lp) - 24, LOWORD(lp), 24, TRUE);
        return 0;
    case WM_SETFOCUS: SetFocus(g_edit); return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_EDIT:
            if (HIWORD(wp) == EN_CHANGE) { update_status(); SetWindowTextW(h, L"*not.txt - Not Defteri"); }
            break;
        case IDM_NEW: SetWindowTextW(g_edit, L""); update_status(); break;
        case IDM_OPEN: load(h); break;
        case IDM_SAVE: save(h); break;
        case IDM_EXIT: SendMessageW(h, WM_CLOSE, 0, 0); break;
        case IDM_CUT: SendMessageW(g_edit, WM_CUT, 0, 0); break;
        case IDM_COPY: SendMessageW(g_edit, WM_COPY, 0, 0); break;
        case IDM_PASTE: SendMessageW(g_edit, WM_PASTE, 0, 0); break;
        case IDM_ALL: SendMessageW(g_edit, EM_SETSEL, 0, -1); break;
        case IDM_TIME: {
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t s[64];
            swprintf(s, 64, L"%02d:%02d %02d.%02d.%04d", t.wHour, t.wMinute, t.wDay, t.wMonth, t.wYear);
            SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)s);
            break;
        }
        case IDM_BIGGER: if (g_size < 48) { g_size += 4; set_font(); } break;
        case IDM_SMALLER: if (g_size > 10) { g_size -= 4; set_font(); } break;
        case IDM_ABOUT: MessageBoxW(h, L"Soul ile iPad'de çalışan Windows not defteri.\nNotlar Belgeler\\not.txt dosyasına kaydedilir.", L"Not Defteri hakkında", MB_OK); break;
        }
        return 0;
    case WM_CLOSE:
        if (SendMessageW(g_edit, EM_GETMODIFY, 0, 0)) {
            int r = MessageBoxW(h, L"Değişiklikler kaydedilsin mi?", L"Not Defteri", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL) return 0;
            if (r == IDYES) save(h);
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SoulNotDefteri";
    RegisterClassW(&wc);
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), edit = CreatePopupMenu(), view = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_NEW, L"&Yeni\tCtrl+N");
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"&Aç\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE, L"&Kaydet\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"Çı&kış");
    AppendMenuW(edit, MF_STRING, IDM_CUT, L"&Kes\tCtrl+X");
    AppendMenuW(edit, MF_STRING, IDM_COPY, L"K&opyala\tCtrl+C");
    AppendMenuW(edit, MF_STRING, IDM_PASTE, L"&Yapıştır\tCtrl+V");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, IDM_ALL, L"&Tümünü seç\tCtrl+A");
    AppendMenuW(edit, MF_STRING, IDM_TIME, L"&Saat/Tarih\tF5");
    AppendMenuW(view, MF_STRING, IDM_BIGGER, L"Yazıyı &büyüt\tCtrl++");
    AppendMenuW(view, MF_STRING, IDM_SMALLER, L"Yazıyı &küçült\tCtrl+-");
    AppendMenuW(help, MF_STRING, IDM_ABOUT, L"&Hakkında");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&Dosya");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"Dü&zen");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&Görünüm");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Yardım");
    ACCEL acc[] = { { FVIRTKEY | FCONTROL, 'N', IDM_NEW }, { FVIRTKEY | FCONTROL, 'O', IDM_OPEN }, { FVIRTKEY | FCONTROL, 'S', IDM_SAVE },
                    { FVIRTKEY, VK_F5, IDM_TIME }, { FVIRTKEY | FCONTROL, VK_OEM_PLUS, IDM_BIGGER }, { FVIRTKEY | FCONTROL, VK_OEM_MINUS, IDM_SMALLER } };
    HACCEL hacc = CreateAcceleratorTableW(acc, sizeof acc / sizeof acc[0]);
    HWND h = CreateWindowW(L"SoulNotDefteri", L"not.txt - Not Defteri", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 560, NULL, bar, hi, NULL);
    ShowWindow(h, show);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(h, hacc, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
