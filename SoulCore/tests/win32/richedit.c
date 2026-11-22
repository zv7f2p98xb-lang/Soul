// RichEdit text loading as installers do it: EM_STREAMIN with RTF (code page 1254 escapes and
// \u Unicode escapes), EM_SETTEXTEX, EM_EXSETSEL and EM_GETTEXTRANGE.
#include <windows.h>
#include <richedit.h>
#include <stdio.h>
#include <string.h>

static const char *g_rtf =
    "{\\rtf1\\ansi\\ansicpg1254\\deff0{\\fonttbl{\\f0\\fnil Tahoma;}}{\\colortbl;\\red0\\green0\\blue0;}"
    "{\\*\\generator Riched20;}\\viewkind4\\uc1\\pard\\f0\\fs20 L\\'ddSANS S\\'d6ZLE\\'deMES\\'dd\\par"
    "\\b Kabul\\b0  ediyor musunuz? \\u8220?Evet\\u8221?\\tab Tamam\\par}";
static size_t g_pos;
static DWORD CALLBACK feed(DWORD_PTR cookie, LPBYTE buf, LONG cb, LONG *pcb) {
    (void)cookie;
    size_t left = strlen(g_rtf) - g_pos, n = left < 37 ? left : 37; // small chunks: several callbacks
    if ((LONG)n > cb) n = (size_t)cb;
    memcpy(buf, g_rtf + g_pos, n);
    g_pos += n;
    *pcb = (LONG)n;
    return 0;
}
static void show(HWND re, const char *what) {
    wchar_t w[512]; char u[1024];
    GetWindowTextW(re, w, 512);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    for (char *p = u; *p; p++) if (*p == '\r') *p = '|'; else if (*p == '\n') *p = '/'; else if (*p == '\t') *p = '>';
    printf("%s: %s\n", what, u);
}
int main(void) {
    LoadLibraryA("riched20.dll");
    HWND parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 400, 300, NULL, NULL, NULL, NULL);
    HWND re = CreateWindowExW(0, L"RichEdit20W", L"", WS_CHILD | ES_MULTILINE | ES_READONLY, 0, 0, 400, 300, parent, NULL, NULL, NULL);
    printf("created=%d\n", re != NULL);
    EDITSTREAM es = { 0, 0, feed };
    LRESULT n = SendMessageW(re, EM_STREAMIN, SF_RTF, (LPARAM)&es);
    printf("streamin=%d error=%lu\n", (int)n, es.dwError);
    show(re, "rtf");
    SETTEXTEX stx = { ST_DEFAULT, 1200 };
    SendMessageW(re, EM_SETTEXTEX, (WPARAM)&stx, (LPARAM)L"Düz metin\r\nikinci satır");
    show(re, "settextex");
    CHARRANGE cr = { 4, 9 };
    SendMessageW(re, EM_EXSETSEL, 0, (LPARAM)&cr);
    wchar_t part[16] = { 0 };
    TEXTRANGEW tr = { { 4, 9 }, part };
    LRESULT got = SendMessageW(re, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    char u[64]; WideCharToMultiByte(CP_UTF8, 0, part, -1, u, sizeof u, NULL, NULL);
    CHARRANGE back = { 0, 0 };
    SendMessageW(re, EM_EXGETSEL, 0, (LPARAM)&back);
    printf("range=%d '%s' sel=%ld-%ld length=%d\n", (int)got, u, back.cpMin, back.cpMax, (int)SendMessageW(re, WM_GETTEXTLENGTH, 0, 0));
    return 0;
}
