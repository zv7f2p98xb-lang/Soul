// msiexec.exe replacement for Soul: installs / removes .msi packages through msi.dll (Soul's installer engine).
// Full UI: Turkish wizard (welcome, progress, finish). /qb /passive: progress only. /qn: no window.
// Hidden mode "/ca dll entry handle" runs a custom action DLL of another bitness for the engine.
#include <windows.h>
#include <commctrl.h>
#include <msi.h>
#include <msiquery.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define WM_MSI_MSG (WM_APP + 1)
#define WM_MSI_DONE (WM_APP + 2)
enum { PG_WELCOME, PG_PROGRESS, PG_DONE };
enum { OP_INSTALL, OP_REMOVE, OP_REPAIR, OP_ADMIN };

static int g_op = OP_INSTALL, g_level = 5;   // 0 none, 1 progress only, 5 full
static wchar_t g_pkg[MAX_PATH * 2], g_props[2048], g_product[128], g_dirprop[64], g_dirdef[MAX_PATH], g_reinstall[16] = L"omus";
static int g_is_code;
static volatile LONG g_cancel;
static UINT g_result;
static int g_page;
static HWND g_wnd, g_prog, g_status, g_title, g_text, g_edit, g_btn1, g_btn2;
static wchar_t g_lasterr[512];
static HINSTANCE g_inst;

static void add_prop(const wchar_t *name, const wchar_t *val) {
    wchar_t t[1024];
    if (wcschr(val, L' ') || !*val) wsprintfW(t, L" %s=\"%s\"", name, val);
    else wsprintfW(t, L" %s=%s", name, val);
    wcsncat(g_props, t, 2047 - wcslen(g_props));
}

static INT CALLBACK ui_handler(LPVOID ctx, UINT type, LPCWSTR msg) {
    UINT m = type & 0xFF000000;
    if (g_wnd && (m == INSTALLMESSAGE_ACTIONSTART || m == INSTALLMESSAGE_PROGRESS || m == INSTALLMESSAGE_ERROR)) {
        wchar_t *c = _wcsdup(msg);
        PostMessageW(g_wnd, WM_MSI_MSG, m, (LPARAM)c);
    }
    return g_cancel ? IDCANCEL : 0;
}

static UINT run_operation(void) {
    wchar_t cmd[2300];
    MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
    if (g_level) MsiSetExternalUIW(ui_handler, INSTALLLOGMODE_ACTIONSTART | INSTALLLOGMODE_PROGRESS | INSTALLLOGMODE_ERROR, NULL);
    if (g_is_code) {
        if (g_op == OP_REMOVE) return MsiConfigureProductExW(g_pkg, 0, INSTALLSTATE_ABSENT, g_props);
        if (g_op == OP_REPAIR) { wsprintfW(cmd, L"REINSTALL=ALL REINSTALLMODE=%s%s", g_reinstall, g_props); return MsiConfigureProductExW(g_pkg, 0, INSTALLSTATE_DEFAULT, cmd); }
        return MsiConfigureProductExW(g_pkg, 0, INSTALLSTATE_DEFAULT, g_props);
    }
    if (g_op == OP_REMOVE) wsprintfW(cmd, L"REMOVE=ALL%s", g_props);
    else if (g_op == OP_REPAIR) wsprintfW(cmd, L"REINSTALL=ALL REINSTALLMODE=%s%s", g_reinstall, g_props);
    else if (g_op == OP_ADMIN) wsprintfW(cmd, L"ACTION=ADMIN%s", g_props);
    else wsprintfW(cmd, L"%s", g_props);
    return MsiInstallProductW(g_pkg, cmd);
}

static DWORD WINAPI worker(LPVOID p) {
    UINT r = run_operation();
    PostMessageW(g_wnd, WM_MSI_DONE, r, 0);
    return 0;
}

static void read_product_info(void) {
    wchar_t v[256];
    DWORD n = 256;
    g_product[0] = 0;
    if (g_is_code) {
        if (MsiGetProductInfoW(g_pkg, L"InstalledProductName", v, &n) == ERROR_SUCCESS) wcsncpy(g_product, v, 127);
        return;
    }
    MSIHANDLE h;
    if (MsiOpenPackageW(g_pkg, &h) != ERROR_SUCCESS) return;
    n = 256;
    if (MsiGetPropertyW(h, L"ProductName", v, &n) == ERROR_SUCCESS) wcsncpy(g_product, v, 127);
    static const wchar_t *cands[] = { L"INSTALLDIR", L"APPLICATIONFOLDER", L"INSTALLFOLDER", L"INSTALLLOCATION", NULL };
    for (int i = 0; cands[i] && g_op == OP_INSTALL; i++) {
        wchar_t d[MAX_PATH];
        n = MAX_PATH;
        if (MsiGetTargetPathW(h, cands[i], d, &n) == ERROR_SUCCESS && d[0]) {
            wcscpy(g_dirprop, cands[i]);
            wcscpy(g_dirdef, d);
            break;
        }
    }
    MsiCloseHandle(h);
}

static void set_page(int pg) {
    g_page = pg;
    wchar_t t[512];
    int welcome = pg == PG_WELCOME, prog = pg == PG_PROGRESS;
    ShowWindow(g_edit, welcome && g_dirprop[0] ? SW_SHOW : SW_HIDE);
    ShowWindow(g_prog, prog ? SW_SHOW : SW_HIDE);
    ShowWindow(g_status, prog ? SW_SHOW : SW_HIDE);
    EnableWindow(g_btn1, pg != PG_PROGRESS);
    const wchar_t *verb = g_op == OP_REMOVE ? L"kaldırılsÄ±n" : L"";
    (void)verb;
    if (welcome) {
        if (g_op == OP_REMOVE) { wsprintfW(t, L"%s kaldırılsın mı?", g_product[0] ? g_product : L"Bu ürün"); SetWindowTextW(g_title, t); SetWindowTextW(g_text, L"Kaldırmak için Kaldır düğmesine basın."); SetWindowTextW(g_btn1, L"Kaldır"); }
        else if (g_op == OP_REPAIR) { wsprintfW(t, L"%s onarılsın mı?", g_product[0] ? g_product : L"Bu ürün"); SetWindowTextW(g_title, t); SetWindowTextW(g_text, L"Onarmak için Kur düğmesine basın."); SetWindowTextW(g_btn1, L"Kur"); }
        else { wsprintfW(t, L"%s kurulumuna hoş geldiniz", g_product[0] ? g_product : L"Program"); SetWindowTextW(g_title, t); SetWindowTextW(g_text, g_dirprop[0] ? L"Kurulum klasörü:" : L"Kurmak için Kur düğmesine basın."); SetWindowTextW(g_btn1, L"Kur"); }
        SetWindowTextW(g_btn2, L"İptal");
    } else if (prog) {
        SetWindowTextW(g_title, g_op == OP_REMOVE ? L"Kaldırılıyor..." : L"Kuruluyor...");
        SetWindowTextW(g_text, L"Lütfen bekleyin.");
        SetWindowTextW(g_status, L"");
        SetWindowTextW(g_btn2, L"İptal");
    } else {
        const wchar_t *what = g_op == OP_REMOVE ? L"Kaldırma" : L"Kurulum";
        if (g_result == 0) { wsprintfW(t, L"%s tamamlandı", what); SetWindowTextW(g_title, t); SetWindowTextW(g_text, g_op == OP_REMOVE ? L"Ürün kaldırıldı." : L"Kurulum tamamlandı."); }
        else if (g_result == 1602) { wsprintfW(t, L"%s iptal edildi", what); SetWindowTextW(g_title, t); SetWindowTextW(g_text, L"İşlem kullanıcı tarafından iptal edildi."); }
        else { wsprintfW(t, L"%s başarısız oldu (hata kodu %u).", what, g_result); SetWindowTextW(g_title, t); SetWindowTextW(g_text, g_lasterr[0] ? g_lasterr : L"Ayrıntı için kayıt dosyasına bakın."); }
        SetWindowTextW(g_btn1, L"Son");
        EnableWindow(g_btn1, TRUE);
        ShowWindow(g_btn2, SW_HIDE);
    }
}

static void start_work(void) {
    wchar_t d[MAX_PATH];
    if (g_page == PG_WELCOME && g_dirprop[0] && g_op == OP_INSTALL) {
        GetWindowTextW(g_edit, d, MAX_PATH);
        if (wcscmp(d, g_dirdef) != 0 && d[0]) add_prop(g_dirprop, d);
    }
    set_page(PG_PROGRESS);
    CreateThread(NULL, 0, worker, NULL, 0, NULL);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            if (g_page == PG_WELCOME) start_work();
            else if (g_page == PG_DONE) DestroyWindow(h);
        } else if (LOWORD(w) == IDCANCEL) {
            if (g_page == PG_PROGRESS) { g_cancel = 1; SetWindowTextW(g_status, L"İptal ediliyor..."); }
            else { if (g_page == PG_WELCOME) g_result = 1602; DestroyWindow(h); }
        }
        return 0;
    case WM_CLOSE:
        if (g_page == PG_PROGRESS) { g_cancel = 1; return 0; }
        if (g_page == PG_WELCOME) g_result = 1602;
        DestroyWindow(h);
        return 0;
    case WM_MSI_MSG: {
        wchar_t *t = (wchar_t *)l;
        if (w == INSTALLMESSAGE_ACTIONSTART) {
            wchar_t *dot = wcsstr(t, L". ");
            const wchar_t *txt = dot && dot[2] ? dot + 2 : t;
            SetWindowTextW(g_status, txt);
        } else if (w == INSTALLMESSAGE_PROGRESS) {
            int f1 = -1, f2 = 0;
            const wchar_t *p2 = wcsstr(t, L" 2: ");
            if (t[0] == L'1' && t[1] == L':') f1 = (int)wcstol(t + 2, NULL, 10);
            if (p2) f2 = (int)wcstol(p2 + 4, NULL, 10);
            if (f1 >= 0) {
                if (f1 == 0 && f2 > 0) { SendMessageW(g_prog, PBM_SETRANGE32, 0, f2); SendMessageW(g_prog, PBM_SETPOS, 0, 0); }
                else if (f1 == 2) SendMessageW(g_prog, PBM_DELTAPOS, f2, 0);
            }
        } else if (w == INSTALLMESSAGE_ERROR) {
            wcsncpy(g_lasterr, t, 511);
        }
        free(t);
        return 0;
    }
    case WM_MSI_DONE:
        g_result = (UINT)w;
        if (g_level == 5) set_page(PG_DONE); else DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int run_ui(void) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SoulMsiexec";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(L"SoulMsiexec", L"Windows Installer", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 100, 100, 500, 320, NULL, NULL, g_inst, NULL);
    g_title = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 20, 440, 28, g_wnd, NULL, g_inst, NULL);
    g_text = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE, 20, 60, 440, 24, g_wnd, NULL, g_inst, NULL);
    g_edit = CreateWindowW(L"EDIT", g_dirdef, WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 20, 90, 440, 24, g_wnd, (HMENU)10, g_inst, NULL);
    g_prog = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD, 20, 120, 440, 22, g_wnd, (HMENU)11, g_inst, NULL);
    g_status = CreateWindowW(L"STATIC", L"", WS_CHILD, 20, 150, 440, 24, g_wnd, NULL, g_inst, NULL);
    g_btn1 = CreateWindowW(L"BUTTON", L"Kur", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 290, 235, 90, 28, g_wnd, (HMENU)IDOK, g_inst, NULL);
    g_btn2 = CreateWindowW(L"BUTTON", L"İptal", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 390, 235, 90, 28, g_wnd, (HMENU)IDCANCEL, g_inst, NULL);
    g_result = 0;
    set_page(PG_WELCOME);
    if (g_level != 5) start_work();
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)g_result;
}

static int ca_mode(int argc, wchar_t **argv) {
    if (argc < 5) return 1;
    typedef UINT (WINAPI *EntryFn)(MSIHANDLE);
    HMODULE m = LoadLibraryW(argv[2]);
    if (!m) return 1603;
    char entry[128];
    WideCharToMultiByte(CP_ACP, 0, argv[3], -1, entry, sizeof entry, NULL, NULL);
    EntryFn f = (EntryFn)GetProcAddress(m, entry);
    if (!f) return 1603;
    return (int)f((MSIHANDLE)wcstoul(argv[4], NULL, 10));
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    g_inst = hi;
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 2 && !_wcsicmp(argv[1], L"/ca")) return ca_mode(argc, argv);
    int have = 0;
    for (int i = 1; i < argc; i++) {
        wchar_t *a = argv[i];
        if (a[0] == L'/' || a[0] == L'-') {
            wchar_t *o = a + 1;
            if (!_wcsicmp(o, L"i") || !_wcsicmp(o, L"package") || !_wcsicmp(o, L"x") || !_wcsicmp(o, L"uninstall") || !_wcsicmp(o, L"a")) {
                g_op = (o[0] == L'x' || o[0] == L'X' || o[0] == L'u' || o[0] == L'U') ? OP_REMOVE : (o[0] == L'a' || o[0] == L'A') ? OP_ADMIN : OP_INSTALL;
                if (i + 1 < argc) { wcsncpy(g_pkg, argv[++i], MAX_PATH * 2 - 1); have = 1; }
            } else if ((o[0] == L'f' || o[0] == L'F') && (o[1] == 0 || wcschr(L"pocemsuvaPOCEMSUVA", o[1]))) {
                g_op = OP_REPAIR;
                if (o[1]) wcsncpy(g_reinstall, o + 1, 15);
                if (i + 1 < argc) { wcsncpy(g_pkg, argv[++i], MAX_PATH * 2 - 1); have = 1; }
            } else if (!_wcsicmp(o, L"passive")) g_level = 1;
            else if (!_wcsicmp(o, L"quiet") || !_wcsicmp(o, L"qn") || !_wcsicmp(o, L"q") || !_wcsicmp(o, L"qn+") || !_wcsicmp(o, L"qn-")) g_level = 0;
            else if (!_wcsnicmp(o, L"qb", 2) || !_wcsnicmp(o, L"qr", 2)) g_level = 1;
            else if (!_wcsnicmp(o, L"qf", 2)) g_level = 5;
            else if (o[0] == L'l' || o[0] == L'L') { if (i + 1 < argc && argv[i + 1][0] != L'/') i++; }
            // /norestart /promptrestart /forcerestart /help /? ... are accepted and ignored
        } else if (wcschr(a, L'=')) {
            wchar_t name[128];
            wchar_t *eq = wcschr(a, L'=');
            size_t nl = (size_t)(eq - a);
            if (nl > 127) nl = 127;
            wcsncpy(name, a, nl);
            name[nl] = 0;
            add_prop(name, eq + 1);
        } else if (!have) { wcsncpy(g_pkg, a, MAX_PATH * 2 - 1); have = 1; }
    }
    if (!have) {
        MessageBoxW(NULL, L"Kullanım: msiexec /i paket.msi | /x paket.msi|{ürün kodu} [/qn|/qb|/passive] [ÖZELLIK=değer]", L"Windows Installer", MB_OK);
        return 1639;
    }
    g_is_code = g_pkg[0] == L'{';
    if (g_level == 0) return (int)run_operation();
    read_product_info();
    return run_ui();
}
