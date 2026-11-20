// GUI smoke test for Soul: windows, controls, menus, dialogs, GDI, timers.
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#define IDD_ABOUT 100
#define IDC_NAME 1001
#define IDC_AGREE 1002

static HWND g_edit, g_list, g_check, g_combo, g_progress, g_button, g_status, g_track;
static int g_ticks;
static char g_name[64] = "";

static INT_PTR CALLBACK about_proc(HWND d, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemTextA(d, IDC_NAME, "Soul");
        printf("dialog init param=%d\n", (int)lp);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            GetDlgItemTextA(d, IDC_NAME, g_name, sizeof g_name);
            printf("dialog end id=%d name=%s agree=%d\n", LOWORD(wp), g_name, IsDlgButtonChecked(d, IDC_AGREE));
            EndDialog(d, LOWORD(wp));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    GetClientRect(h, &rc);
    HBRUSH b = CreateSolidBrush(RGB(30, 120, 220));
    RECT r = { 300, 20, 460, 80 };
    FillRect(dc, &r, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(200, 40, 40));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, 480, 20, 580, 120);
    MoveToEx(dc, 300, 100, NULL);
    LineTo(dc, 460, 140);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    HFONT f = CreateFontA(-28, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 140, 60));
    TextOutA(dc, 300, 150, "Merhaba Soul! \xC4\x9F\xC3\xBC\xC5\x9F", 21);
    SelectObject(dc, of);
    DeleteObject(f);
    // gradient through a DIB section and BitBlt
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 128;
    bi.bmiHeader.biHeight = -64;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) ((DWORD *)bits)[y * 128 + x] = RGB(y * 4, x * 2, 255 - x * 2);
    HDC mem = CreateCompatibleDC(dc);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(dc, 300, 200, 128, 64, mem, 0, 0, SRCCOPY);
    StretchBlt(dc, 440, 200, 64, 32, mem, 0, 0, 128, 64, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(bmp);
    char buf[64];
    snprintf(buf, sizeof buf, "Sayac: %d", g_ticks);
    SetTextColor(dc, RGB(0, 0, 0));
    RECT tr = { 300, 280, 580, 300 };
    DrawTextA(dc, buf, -1, &tr, DT_LEFT | DT_SINGLELINE);
    EndPaint(h, &ps);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        HFONT gui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HINSTANCE hi = ((CREATESTRUCTA *)lp)->hInstance;
        CreateWindowA("STATIC", "Ad:", WS_CHILD | WS_VISIBLE, 10, 14, 40, 20, h, (HMENU)100, hi, NULL);
        g_edit = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 50, 10, 150, 24, h, (HMENU)101, hi, NULL);
        g_button = CreateWindowA("BUTTON", "Ekle", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 210, 10, 70, 26, h, (HMENU)102, hi, NULL);
        g_check = CreateWindowA("BUTTON", "Büyük harf", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 10, 44, 150, 22, h, (HMENU)103, hi, NULL);
        g_list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", NULL, WS_CHILD | WS_VISIBLE | LBS_NOTIFY, 10, 74, 270, 120, h, (HMENU)104, hi, NULL);
        g_combo = CreateWindowA("COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 10, 204, 270, 200, h, (HMENU)105, hi, NULL);
        g_progress = CreateWindowA("msctls_progress32", NULL, WS_CHILD | WS_VISIBLE, 10, 244, 270, 18, h, (HMENU)106, hi, NULL);
        g_track = CreateWindowA(TRACKBAR_CLASSA, NULL, WS_CHILD | WS_VISIBLE, 10, 266, 270, 26, h, (HMENU)107, hi, NULL);
        SendMessageA(g_track, TBM_SETRANGE, TRUE, MAKELPARAM(0, 50));
        g_status = CreateWindowA(STATUSCLASSNAMEA, "Hazır", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)108, hi, NULL);
        int parts[2] = { 200, -1 };
        SendMessageA(g_status, SB_SETPARTS, 2, (LPARAM)parts);
        SendMessageA(g_status, SB_SETTEXTA, 1, (LPARAM)"Soul");
        for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageA(c, WM_SETFONT, (WPARAM)gui, TRUE);
        SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)"Kırmızı");
        SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)"Yeşil");
        SendMessageA(g_combo, CB_ADDSTRING, 0, (LPARAM)"Mavi");
        SendMessageA(g_combo, CB_SETCURSEL, 1, 0);
        SendMessageA(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 10));
        SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)"ilk öğe");
        SetTimer(h, 1, 50, NULL);
        char res[64];
        LoadStringA(GetModuleHandleA(NULL), 1, res, sizeof res);
        printf("create ok, resource=\"%s\"\n", res);
        return 0;
    }
    case WM_TIMER:
        if (g_ticks < 10) {
            g_ticks++;
            SendMessageA(g_progress, PBM_SETPOS, g_ticks, 0);
            RECT r = { 300, 280, 580, 300 };
            InvalidateRect(h, &r, TRUE);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case 102: {
            char t[128];
            GetWindowTextA(g_edit, t, sizeof t);
            if (SendMessageA(g_check, BM_GETCHECK, 0, 0) == BST_CHECKED) CharUpperA(t);
            int idx = (int)SendMessageA(g_list, LB_ADDSTRING, 0, (LPARAM)t);
            SendMessageA(g_list, LB_SETCURSEL, idx, 0);
            SetWindowTextA(g_edit, "");
            printf("added \"%s\" count=%d\n", t, (int)SendMessageA(g_list, LB_GETCOUNT, 0, 0));
            break;
        }
        case 103: printf("check=%d\n", (int)SendMessageA(g_check, BM_GETCHECK, 0, 0)); break;
        case 104:
            if (HIWORD(wp) == LBN_SELCHANGE) {
                char t[128];
                int i = (int)SendMessageA(g_list, LB_GETCURSEL, 0, 0);
                SendMessageA(g_list, LB_GETTEXT, i, (LPARAM)t);
                printf("list select %d \"%s\"\n", i, t);
            }
            break;
        case 105:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                char t[64];
                GetWindowTextA(g_combo, t, sizeof t);
                printf("combo %d \"%s\"\n", (int)SendMessageA(g_combo, CB_GETCURSEL, 0, 0), t);
            }
            break;
        case 101:
            if (HIWORD(wp) == EN_CHANGE) { char t[128]; GetWindowTextA(g_edit, t, sizeof t); if (strlen(t) == 7) printf("edit \"%s\"\n", t); }
            break;
        case 201: printf("menu new\n"); break;
        case 202: DestroyWindow(h); break;
        case 301: {
            INT_PTR r = DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_ABOUT), h, about_proc, 42);
            printf("dialog returned %d\n", (int)r);
            break;
        }
        case 302: {
            HMENU m = GetMenu(h);
            UINT st = GetMenuState(m, 302, MF_BYCOMMAND);
            CheckMenuItem(m, 302, MF_BYCOMMAND | ((st & MF_CHECKED) ? MF_UNCHECKED : MF_CHECKED));
            printf("option %s\n", (st & MF_CHECKED) ? "off" : "on");
            break;
        }
        }
        return 0;
    case WM_LBUTTONDOWN:
        printf("click %d,%d\n", (short)LOWORD(lp), (short)HIWORD(lp));
        return 0;
    case WM_SIZE: SendMessageA(g_status, WM_SIZE, 0, 0); return 0;
    case WM_HSCROLL:
        if ((HWND)lp == g_track && LOWORD(wp) == TB_ENDTRACK) {
            char t[64];
            int pos = (int)SendMessageA(g_track, TBM_GETPOS, 0, 0);
            snprintf(t, sizeof t, "Konum %d", pos);
            SendMessageA(g_status, SB_SETTEXTA, 0, (LPARAM)t);
            printf("track %d\n", pos);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == 'N' && (GetKeyState(VK_CONTROL) & 0x8000)) printf("ctrl+n\n");
        return 0;
    case WM_PAINT: paint(h); return 0;
    case WM_CLOSE: printf("close\n"); DestroyWindow(h); return 0;
    case WM_DESTROY: {
        char t[64] = "";
        SendMessageA(g_status, SB_GETTEXTA, 0, (LPARAM)t);
        printf("destroy ticks=%d status=\"%s\"\n", g_ticks, t);
        PostQuitMessage(7);
        return 0;
    }
    }
    return DefWindowProcA(h, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd, int show) {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "SoulTest";
    wc.lpszMenuName = MAKEINTRESOURCEA(200);
    if (!RegisterClassA(&wc)) return 1;
    HWND h = CreateWindowA("SoulTest", "Soul GUI Testi", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 600, 320, NULL, NULL, hi, NULL);
    if (!h) return 2;
    ShowWindow(h, show);
    UpdateWindow(h);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    printf("exit %d\n", (int)msg.wParam);
    return (int)msg.wParam;
}
