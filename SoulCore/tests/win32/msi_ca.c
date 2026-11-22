// Custom action DLL for tests/msi/ca.msi (32-bit): reads / sets properties and writes files under the install folder.
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <stdio.h>
#include <string.h>

static void get(MSIHANDLE h, const wchar_t *name, wchar_t *out, DWORD cap) {
    DWORD n = cap;
    out[0] = 0;
    MsiGetPropertyW(h, name, out, &n);
}
static void put_file(const wchar_t *dir, const wchar_t *file, const wchar_t *text) {
    wchar_t path[MAX_PATH];
    CreateDirectoryW(dir, NULL);
    wsprintfW(path, L"%s%s", dir, file);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    char u[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, u, sizeof u, NULL, NULL);
    DWORD w;
    WriteFile(f, u, n > 0 ? n - 1 : 0, &w, NULL);
    CloseHandle(f);
}

__declspec(dllexport) UINT __stdcall CaImmediate(MSIHANDLE h) {
    wchar_t in[256], out[300], dir[MAX_PATH], text[600];
    get(h, L"CA_IN", in, 256);
    wsprintfW(out, L"dll:%s", in);
    MsiSetPropertyW(h, L"CA_OUT", out);
    DWORD n = MAX_PATH;
    MsiGetTargetPathW(h, L"INSTALLDIR", dir, &n);
    wsprintfW(text, L"in=%s out=%s sched=%d", in, out, MsiGetMode(h, MSIRUNMODE_SCHEDULED));
    put_file(dir, L"ca_imm.txt", text);
    return ERROR_SUCCESS;
}

__declspec(dllexport) UINT __stdcall CaDeferred(MSIHANDLE h) {
    wchar_t data[700], text[800];
    get(h, L"CustomActionData", data, 700);
    wchar_t *bar = wcschr(data, L'|');
    if (!bar) return ERROR_INSTALL_FAILURE;
    *bar = 0;
    wsprintfW(text, L"data=%s sched=%d", bar + 1, MsiGetMode(h, MSIRUNMODE_SCHEDULED));
    put_file(data, L"ca_def.txt", text);
    return ERROR_SUCCESS;
}

// returns an error: the install must fail with 1603
__declspec(dllexport) UINT __stdcall CaFail(MSIHANDLE h) { return ERROR_INSTALL_FAILURE; }
