// Common controls test: toolbar, tab control, list view (report view with columns, checkboxes,
// sorting), tree view, up-down control with a buddy edit box and a SysLink. Every notification
// the program receives is printed so the GUI script can check the behaviour.
#define _WIN32_IE 0x0600
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

static HWND g_lv, g_tv, g_tab, g_edit, g_ud, g_tb;
static HTREEITEM g_root;

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}
static int CALLBACK by_size_desc(LPARAM a, LPARAM b, LPARAM sort) { (void)sort; return (int)(b - a); }

static void lv_dump(void) {
    char buf[64], line[256] = "";
    int n = (int)SendMessageA(g_lv, LVM_GETITEMCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        LVITEMA it = { 0 };
        it.iSubItem = 0; it.pszText = buf; it.cchTextMax = sizeof buf;
        SendMessageA(g_lv, LVM_GETITEMTEXTA, (WPARAM)i, (LPARAM)&it);
        strcat(line, buf);
        if (i + 1 < n) strcat(line, ",");
    }
    say("lv order %s", line);
}

static void create_children(HWND hwnd) {
    HINSTANCE hi = GetModuleHandleA(NULL);
    g_tb = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_LIST, 0, 0, 0, 0, hwnd, (HMENU)100, hi, NULL);
    SendMessageW(g_tb, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    TBADDBITMAP ab = { HINST_COMMCTRL, IDB_STD_SMALL_COLOR };
    SendMessageW(g_tb, TB_ADDBITMAP, 0, (LPARAM)&ab);
    TBBUTTON b[4] = {
        { STD_FILENEW, 501, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, (INT_PTR)L"Yeni" },
        { STD_FILEOPEN, 502, TBSTATE_ENABLED, BTNS_BUTTON, { 0 }, 0, (INT_PTR)L"Aç" },
        { 0, 0, 0, BTNS_SEP, { 0 }, 0, 0 },
        { STD_FILESAVE, 503, TBSTATE_ENABLED, BTNS_CHECK, { 0 }, 0, (INT_PTR)L"Kaydet" },
    };
    SendMessageW(g_tb, TB_ADDBUTTONSW, 4, (LPARAM)b);
    SendMessageW(g_tb, TB_AUTOSIZE, 0, 0);
    RECT tr; GetWindowRect(g_tb, &tr);
    say("toolbar buttons=%d height=%d", (int)SendMessageW(g_tb, TB_BUTTONCOUNT, 0, 0), (int)(tr.bottom - tr.top) > 0);

    g_tab = CreateWindowExW(0, WC_TABCONTROLW, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 10, 40, 320, 260, hwnd, (HMENU)200, hi, NULL);
    TCITEMW ti = { 0 };
    ti.mask = TCIF_TEXT;
    ti.pszText = L"Liste"; SendMessageW(g_tab, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = L"Ağaç"; SendMessageW(g_tab, TCM_INSERTITEMW, 1, (LPARAM)&ti);
    RECT body = { 10, 40, 330, 300 };
    SendMessageW(g_tab, TCM_ADJUSTRECT, FALSE, (LPARAM)&body);

    g_lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS,
                           body.left, body.top, body.right - body.left, body.bottom - body.top, hwnd, (HMENU)300, hi, NULL);
    SendMessageW(g_lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_GRIDLINES);
    LVCOLUMNW c = { 0 };
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.cx = 150; c.pszText = L"Ad"; c.iSubItem = 0; SendMessageW(g_lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
    c.cx = 100; c.pszText = L"Boyut"; c.iSubItem = 1; SendMessageW(g_lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
    const wchar_t *names[3] = { L"Belge.txt", L"Resim.png", L"Müzik.mp3" }, *sizes[3] = { L"12 KB", L"340 KB", L"4 MB" };
    const LPARAM bytes[3] = { 12, 340, 4096 };
    for (int i = 0; i < 3; i++) {
        LVITEMW it = { 0 };
        it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = i; it.pszText = (LPWSTR)names[i]; it.lParam = bytes[i];
        int at = (int)SendMessageW(g_lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.mask = LVIF_TEXT; it.iItem = at; it.iSubItem = 1; it.pszText = (LPWSTR)sizes[i];
        SendMessageW(g_lv, LVM_SETITEMW, 0, (LPARAM)&it);
    }
    say("lv items=%d columns ok=%d", (int)SendMessageW(g_lv, LVM_GETITEMCOUNT, 0, 0), (int)SendMessageW(g_lv, LVM_GETCOLUMNWIDTH, 1, 0) == 100);

    g_tv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, NULL, WS_CHILD | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                           body.left, body.top, body.right - body.left, body.bottom - body.top, hwnd, (HMENU)400, hi, NULL);
    TVINSERTSTRUCTW ins = { 0 };
    ins.hParent = TVI_ROOT; ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT; ins.item.pszText = L"Bilgisayar";
    g_root = (HTREEITEM)SendMessageW(g_tv, TVM_INSERTITEMW, 0, (LPARAM)&ins);
    ins.hParent = g_root;
    ins.item.pszText = L"C: sürücüsü"; HTREEITEM c1 = (HTREEITEM)SendMessageW(g_tv, TVM_INSERTITEMW, 0, (LPARAM)&ins);
    ins.item.pszText = L"Belgeler"; SendMessageW(g_tv, TVM_INSERTITEMW, 0, (LPARAM)&ins);
    ins.hParent = c1; ins.item.pszText = L"Program Files"; SendMessageW(g_tv, TVM_INSERTITEMW, 0, (LPARAM)&ins);
    say("tv count=%d", (int)SendMessageW(g_tv, TVM_GETCOUNT, 0, 0));

    g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"5", WS_CHILD | WS_VISIBLE | ES_NUMBER, 350, 60, 80, 24, hwnd, (HMENU)500, hi, NULL);
    g_ud = CreateWindowExW(0, UPDOWN_CLASSW, NULL, WS_CHILD | WS_VISIBLE | UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_ARROWKEYS, 0, 0, 0, 0, hwnd, (HMENU)501, hi, NULL);
    SendMessageW(g_ud, UDM_SETBUDDY, (WPARAM)g_edit, 0);
    SendMessageW(g_ud, UDM_SETRANGE32, 0, 10);
    SendMessageW(g_ud, UDM_SETPOS32, 0, 5);

    CreateWindowExW(0, WC_LINK, L"Site: <a href=\"https://example.com/soul\">example.com</a>", WS_CHILD | WS_VISIBLE, 350, 110, 220, 22, hwnd, (HMENU)600, hi, NULL);
}

static LRESULT CALLBACK wnd(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: create_children(hwnd); return 0;
    case WM_COMMAND:
        if (LOWORD(wp) >= 501 && LOWORD(wp) <= 503 && (HWND)lp == g_tb)
            say("toolbar %d checked=%d", LOWORD(wp), (int)SendMessageW(g_tb, TB_ISBUTTONCHECKED, LOWORD(wp), 0));
        return 0;
    case WM_VSCROLL:
        if ((HWND)lp == g_ud) { wchar_t t[16]; GetWindowTextW(g_edit, t, 16); say("updown pos=%d edit=%ls", (int)SendMessageW(g_ud, UDM_GETPOS32, 0, 0), t); }
        return 0;
    case WM_NOTIFY: {
        NMHDR *h = (NMHDR *)lp;
        if (h->hwndFrom == g_tab && h->code == TCN_SELCHANGE) {
            int cur = (int)SendMessageW(g_tab, TCM_GETCURSEL, 0, 0);
            say("tab %d", cur);
            ShowWindow(g_lv, cur == 0 ? SW_SHOW : SW_HIDE);
            ShowWindow(g_tv, cur == 1 ? SW_SHOW : SW_HIDE);
        } else if (h->hwndFrom == g_lv && h->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *n = (NMLISTVIEW *)lp;
            if ((n->uNewState ^ n->uOldState) & LVIS_SELECTED && (n->uNewState & LVIS_SELECTED)) say("lv select %d", n->iItem);
            if ((n->uNewState ^ n->uOldState) & LVIS_STATEIMAGEMASK && (n->uOldState & LVIS_STATEIMAGEMASK))
                say("lv check %d %d", n->iItem, (int)ListView_GetCheckState(g_lv, n->iItem));
        } else if (h->hwndFrom == g_lv && h->code == NM_DBLCLK) {
            say("lv dblclk %d", ((NMITEMACTIVATE *)lp)->iItem);
        } else if (h->hwndFrom == g_lv && h->code == LVN_COLUMNCLICK) {
            say("lv column %d", ((NMLISTVIEW *)lp)->iSubItem);
            SendMessageW(g_lv, LVM_SORTITEMS, 0, (LPARAM)by_size_desc);
            lv_dump();
        } else if (h->hwndFrom == g_tv && h->code == TVN_SELCHANGEDW) {
            NMTREEVIEWW *n = (NMTREEVIEWW *)lp;
            wchar_t t[64]; TVITEMW it = { 0 };
            it.mask = TVIF_TEXT; it.hItem = n->itemNew.hItem; it.pszText = t; it.cchTextMax = 64;
            SendMessageW(g_tv, TVM_GETITEMW, 0, (LPARAM)&it);
            char u8[128];
            WideCharToMultiByte(CP_UTF8, 0, t, -1, u8, sizeof u8, NULL, NULL);
            say("tv select %s action=%d", u8, (int)n->action);
        } else if (h->hwndFrom == g_tv && h->code == TVN_ITEMEXPANDEDW) {
            NMTREEVIEWW *n = (NMTREEVIEWW *)lp;
            say("tv expanded=%d visible=%d", (n->itemNew.state & TVIS_EXPANDED) != 0,
                (int)(SendMessageW(g_tv, TVM_GETNEXTITEM, TVGN_LASTVISIBLE, 0) != (LRESULT)g_root));
        } else if (h->code == NM_CLICK && GetDlgCtrlID(h->hwndFrom) == 600) {
            say("link %ls", ((NMLINK *)lp)->item.szUrl);
        }
        return 0;
    }
    case WM_DESTROY:
        say("destroy lv=%d selected=%d", (int)SendMessageW(g_lv, LVM_GETITEMCOUNT, 0, 0), (int)SendMessageW(g_lv, LVM_GETSELECTEDCOUNT, 0, 0));
        PostQuitMessage(3);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE prev, PWSTR cmd, int show) {
    (void)prev; (void)cmd;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_WIN95_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = wnd; wc.hInstance = hi; wc.lpszClassName = L"SoulComctl";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"SoulComctl", L"Ortak denetimler", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 600, 340, NULL, NULL, hi, NULL);
    ShowWindow(w, show);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return (int)m.wParam;
}
