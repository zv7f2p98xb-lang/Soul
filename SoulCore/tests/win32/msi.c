// Windows Installer: installs / queries / uninstalls tests/msi/sample.msi, ca.msi and lzx/lzx.msi through msi.dll, then runs the
// msiexec stub. The packages sit next to this program (the test runner copies them).
#include <windows.h>
#include <msi.h>
#include <msiquery.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static wchar_t g_dir[MAX_PATH];
static int g_actionstart, g_progress, g_initialize, g_error;

static void P(const wchar_t *fmt, ...) {
    wchar_t w[2048];
    char u[6144];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(w, 2048, fmt, ap);
    va_end(ap);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, NULL, NULL);
    fputs(u, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static INT CALLBACK ui_handler(LPVOID ctx, UINT type, LPCWSTR msg) {
    UINT m = type & 0xFF000000;
    if (m == INSTALLMESSAGE_ACTIONSTART) g_actionstart++;
    else if (m == INSTALLMESSAGE_PROGRESS) g_progress++;
    else if (m == INSTALLMESSAGE_INITIALIZE) g_initialize++;
    else if (m == INSTALLMESSAGE_ERROR) { g_error++; P(L"  ui error: %ls", msg); }
    return 0;
}

static int exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static long fsize(const wchar_t *p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return -1;
    return (long)d.nFileSizeLow;
}
static int readfile(const wchar_t *p, char *buf, int cap) {
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { buf[0] = 0; return -1; }
    DWORD n = 0;
    ReadFile(h, buf, cap - 1, &n, NULL);
    CloseHandle(h);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return (int)n;
}
static void show_file(const wchar_t *label, const wchar_t *path) {
    char b[512];
    if (readfile(path, b, sizeof b) < 0) { P(L"%ls: (yok)", label); return; }
    wchar_t w[512];
    MultiByteToWideChar(CP_UTF8, 0, b, -1, w, 512);
    for (wchar_t *c = w; *c; c++) if (*c == L'\n' || *c == L'\r') *c = L'|';
    P(L"%ls: %ls", label, w);
}
static void show_reg(HKEY root, const wchar_t *key, const wchar_t *name, const wchar_t *label) {
    HKEY k;
    wchar_t s[512];
    DWORD type = 0, sz = sizeof s;
    if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != 0) { P(L"%ls: (anahtar yok)", label); return; }
    LONG r = RegQueryValueExW(k, name, NULL, &type, (BYTE *)s, &sz);
    RegCloseKey(k);
    if (r != 0) P(L"%ls: (yok)", label);
    else if (type == REG_DWORD) { DWORD dv; memcpy(&dv, s, 4); P(L"%ls: dword %lu", label, dv); }
    else P(L"%ls: %ls", label, s);
}

static void wait_ms(int ms) { Sleep(ms); }

static void sample_tests(void) {
    wchar_t pkg[MAX_PATH];
    swprintf(pkg, MAX_PATH, L"%ls\\sample.msi", g_dir);
    const wchar_t *code = L"{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}";
    const wchar_t *root = L"C:\\Program Files (x86)\\Soul \u00d6rnek";
    P(L"== sample.msi ==");
    P(L"state before: %d", (int)MsiQueryProductStateW(code));
    MsiSetInternalUI(INSTALLUILEVEL_NONE, NULL);
    MsiSetExternalUIW(ui_handler, INSTALLLOGMODE_ACTIONSTART | INSTALLLOGMODE_PROGRESS | INSTALLLOGMODE_INITIALIZE | INSTALLLOGMODE_ERROR, NULL);
    UINT r = MsiInstallProductW(pkg, L"");
    P(L"install: %u", r);
    P(L"ui: initialize=%ls actionstart=%ls progress=%ls errors=%d", g_initialize ? L"yes" : L"no", g_actionstart > 5 ? L"yes" : L"no", g_progress > 3 ? L"yes" : L"no", g_error);
    wchar_t p[MAX_PATH];
    swprintf(p, MAX_PATH, L"%ls\\readme.txt", root);
    show_file(L"readme.txt", p);
    swprintf(p, MAX_PATH, L"%ls\\blob.bin", root);
    P(L"blob.bin size: %ld", fsize(p));
    swprintf(p, MAX_PATH, L"%ls\\data\\ek.txt", root);
    show_file(L"data\\ek.txt", p);
    show_reg(HKEY_LOCAL_MACHINE, L"Software\\SoulOrnek", L"Yol", L"reg Yol");
    show_reg(HKEY_LOCAL_MACHINE, L"Software\\SoulOrnek", L"Sayi", L"reg Sayi");
    show_reg(HKEY_CURRENT_USER, L"Software\\SoulOrnek", L"Kisayol", L"reg Kisayol");
    P(L"shortcut: %d", exists(L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Soul \u00d6rnek\\Oku.lnk"));
    P(L"state after: %d", (int)MsiQueryProductStateW(code));
    wchar_t buf[256];
    DWORD n = 256;
    r = MsiGetProductInfoW(code, L"InstalledProductName", buf, &n);
    P(L"name: %u '%ls' len=%lu", r, buf, n);
    n = 256;
    r = MsiGetProductInfoW(code, L"VersionString", buf, &n);
    P(L"version: %u '%ls'", r, buf);
    n = 256;
    r = MsiGetProductInfoW(code, L"Publisher", buf, &n);
    P(L"publisher: %u '%ls'", r, buf);
    n = 3;
    wchar_t small[3];
    r = MsiGetProductInfoW(code, L"VersionString", small, &n);
    P(L"small buffer: %u need=%lu", r, n);
    r = MsiGetProductInfoW(code, L"NoSuchProperty", buf, &n);
    P(L"unknown property: %u", r);
    int found = 0;
    for (DWORD i = 0; i < 50; i++) {
        wchar_t pc[40];
        if (MsiEnumProductsW(i, pc) != ERROR_SUCCESS) break;
        if (!wcscmp(pc, code)) found = 1;
    }
    P(L"enum found: %d", found);
    P(L"uninstall key: %d", exists(L"C:\\Windows\\Installer\\{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}.msi"));
    show_reg(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}", L"DisplayName", L"uninstall DisplayName");
    show_reg(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}", L"UninstallString", L"uninstall UninstallString");
    show_reg(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}", L"WindowsInstaller", L"uninstall WindowsInstaller");
    n = 256;
    wchar_t cp[256];
    UINT st = MsiGetComponentPathW(code, L"{11111111-2222-3333-4444-555555555502}", cp, &n);
    P(L"component path: %u %ls", st, cp);
    P(L"feature state: %d", (int)MsiQueryFeatureStateW(code, L"Main"));

    // database access
    MSIHANDLE db = 0, view = 0, rec = 0;
    r = MsiOpenDatabaseW(pkg, (LPCWSTR)MSIDBOPEN_READONLY, &db);
    P(L"open database: %u", r);
    r = MsiDatabaseOpenViewW(db, L"SELECT `File`, `FileName`, `FileSize` FROM `File` ORDER BY `Sequence`", &view);
    P(L"open view: %u", r);
    r = MsiViewExecute(view, 0);
    P(L"execute: %u", r);
    while (MsiViewFetch(view, &rec) == ERROR_SUCCESS) {
        wchar_t a[64], b[64];
        DWORD na = 64, nb = 64;
        MsiRecordGetStringW(rec, 1, a, &na);
        MsiRecordGetStringW(rec, 2, b, &nb);
        P(L"  file %ls %ls size=%d fields=%u", a, b, MsiRecordGetInteger(rec, 3), MsiRecordGetFieldCount(rec));
        MsiCloseHandle(rec);
    }
    MSIHANDLE cols = 0;
    MsiViewGetColumnInfo(view, MSICOLINFO_NAMES, &cols);
    wchar_t cn[64];
    DWORD ncn = 64;
    MsiRecordGetStringW(cols, 2, cn, &ncn);
    P(L"column 2: %ls", cn);
    MsiCloseHandle(cols);
    MsiViewGetColumnInfo(view, MSICOLINFO_TYPES, &cols);
    ncn = 64;
    MsiRecordGetStringW(cols, 3, cn, &ncn);
    P(L"column 3 type: %ls", cn);
    MsiCloseHandle(cols);
    MsiViewClose(view);
    MsiCloseHandle(view);
    r = MsiDatabaseOpenViewW(db, L"SELECT `Value` FROM `Property` WHERE `Property`='ProductName'", &view);
    MsiViewExecute(view, 0);
    if (MsiViewFetch(view, &rec) == ERROR_SUCCESS) {
        wchar_t v[128];
        DWORD nv = 128;
        MsiRecordGetStringW(rec, 1, v, &nv);
        P(L"ProductName: %ls", v);
        MsiCloseHandle(rec);
    }
    MsiCloseHandle(view);
    r = MsiDatabaseOpenViewW(db, L"SELECT * FROM `NoSuchTable`", &view);
    P(L"bad query: %u", r);
    MSIHANDLE sum = 0;
    r = MsiGetSummaryInformationW(db, NULL, 0, &sum);
    UINT type;
    INT iv;
    FILETIME ft;
    wchar_t sv[128];
    DWORD nsv = 128;
    MsiSummaryInfoGetPropertyW(sum, 7, &type, &iv, &ft, sv, &nsv);
    P(L"summary %u template: %ls (type %u)", r, sv, type);
    nsv = 128;
    MsiSummaryInfoGetPropertyW(sum, 4, &type, &iv, &ft, sv, &nsv);
    P(L"summary author: %ls", sv);
    MsiCloseHandle(sum);
    MsiCloseHandle(db);

    // package session
    MSIHANDLE h = 0;
    r = MsiOpenPackageW(pkg, &h);
    P(L"open package: %u", r);
    nsv = 128;
    MsiGetPropertyW(h, L"ProductName", sv, &nsv);
    P(L"session ProductName: %ls", sv);
    nsv = 128;
    MsiGetPropertyW(h, L"ProgramFilesFolder", sv, &nsv);
    P(L"ProgramFilesFolder: %ls", sv);
    nsv = 128;
    MsiGetPropertyW(h, L"VersionNT", sv, &nsv);
    P(L"VersionNT: %ls", sv);
    MsiSetPropertyW(h, L"MYPROP", L"abc");
    P(L"cond MYPROP=\"abc\": %d", (int)MsiEvaluateConditionW(h, L"MYPROP = \"abc\" AND VersionNT >= 601"));
    P(L"cond false: %d", (int)MsiEvaluateConditionW(h, L"MYPROP <> \"abc\" OR NOT VersionNT"));
    P(L"cond syntax: %d", (int)MsiEvaluateConditionW(h, L"((("));
    MSIHANDLE fr = MsiCreateRecord(2);
    MsiRecordSetStringW(fr, 0, L"[1] -> [MYPROP] -> [2]");
    MsiRecordSetStringW(fr, 1, L"bir");
    MsiRecordSetInteger(fr, 2, 2);
    nsv = 128;
    MsiFormatRecordW(h, fr, sv, &nsv);
    P(L"format: %ls", sv);
    MsiCloseHandle(fr);
    MsiCloseHandle(h);

    // uninstall
    r = MsiConfigureProductW(code, 0, INSTALLSTATE_ABSENT);
    P(L"uninstall: %u", r);
    swprintf(p, MAX_PATH, L"%ls\\readme.txt", root);
    P(L"readme exists: %d", exists(p));
    P(L"data dir exists: %d", exists(L"C:\\Program Files (x86)\\Soul \u00d6rnek\\data"));
    P(L"install dir exists: %d", exists(root));
    P(L"menu dir exists: %d", exists(L"C:\\ProgramData\\Microsoft\\Windows\\Start Menu\\Programs\\Soul \u00d6rnek"));
    show_reg(HKEY_LOCAL_MACHINE, L"Software\\SoulOrnek", L"Yol", L"reg Yol");
    show_reg(HKEY_LOCAL_MACHINE, L"Software\\SoulOrnek", L"Sayi", L"reg Sayi");
    show_reg(HKEY_CURRENT_USER, L"Software\\SoulOrnek", L"Kisayol", L"reg Kisayol");
    {
        HKEY k;
        P(L"SoulOrnek key gone: %d %d", RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\SoulOrnek", 0, KEY_READ, &k) != 0,
          RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SoulOrnek", 0, KEY_READ, &k) != 0);
    }
    P(L"uninstall key: %d", exists(L"C:\\Windows\\Installer\\{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}.msi"));
    show_reg(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}", L"DisplayName", L"uninstall DisplayName");
    P(L"state: %d", (int)MsiQueryProductStateW(code));
    r = MsiConfigureProductW(code, 0, INSTALLSTATE_ABSENT);
    P(L"uninstall again: %u", r);
    wait_ms(1);
}


static void get_code(const wchar_t *pkg, wchar_t *code) {
    MSIHANDLE h = 0;
    DWORD n = 64;
    code[0] = 0;
    if (MsiOpenPackageW(pkg, &h) == ERROR_SUCCESS) { MsiGetPropertyW(h, L"ProductCode", code, &n); MsiCloseHandle(h); }
}

static void ca_tests(void) {
    wchar_t pkg[MAX_PATH], code[64], p[MAX_PATH];
    swprintf(pkg, MAX_PATH, L"%ls\\ca.msi", g_dir);
    get_code(pkg, code);
    const wchar_t *root = L"C:\\Program Files (x86)\\SoulCA";
    P(L"== ca.msi ==");
    g_error = 0;
    UINT r = MsiInstallProductW(pkg, L"VersionNT=300");
    P(L"install with failing launch condition: %u errors=%d state=%d", r, g_error, (int)MsiQueryProductStateW(code));
    g_error = 0;
    r = MsiInstallProductW(pkg, L"");
    P(L"install: %u errors=%d", r, g_error);
    wchar_t *names[] = { L"hello.txt", L"tool.exe", L"yes.txt", L"nope.txt" };
    for (int i = 0; i < 4; i++) { swprintf(p, MAX_PATH, L"%ls\\%ls", root, names[i]); P(L"%ls exists: %d", names[i], exists(p)); }
    swprintf(p, MAX_PATH, L"%ls\\ca_imm.txt", root);
    show_file(L"ca_imm.txt", p);
    swprintf(p, MAX_PATH, L"%ls\\ca_def.txt", root);
    show_file(L"ca_def.txt", p);
    swprintf(p, MAX_PATH, L"%ls\\ca_exe.txt", root);
    show_file(L"ca_exe.txt", p);
    P(L"state: %d", (int)MsiQueryProductStateW(code));
    r = MsiConfigureProductW(code, 0, INSTALLSTATE_ABSENT);
    P(L"uninstall: %u", r);
    for (int i = 0; i < 4; i++) { swprintf(p, MAX_PATH, L"%ls\\%ls", root, names[i]); P(L"%ls exists: %d", names[i], exists(p)); }
    swprintf(p, MAX_PATH, L"%ls\\ca_imm.txt", root);
    P(L"ca_imm.txt exists: %d", exists(p));
    P(L"install dir exists: %d", exists(root));
    P(L"state: %d", (int)MsiQueryProductStateW(code));
}

static DWORD run_msiexec(const wchar_t *args) {
    wchar_t cmd[1024];
    swprintf(cmd, 1024, L"C:\\Windows\\System32\\msiexec.exe %ls", args);
    STARTUPINFOW si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 0xFFFF0000 | GetLastError();
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 99999;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

static unsigned long crc32_file(const wchar_t *path, long *size) {
    static unsigned long tab[256];
    if (!tab[1]) for (unsigned long i = 0; i < 256; i++) { unsigned long c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1; tab[i] = c; }
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    *size = -1;
    if (h == INVALID_HANDLE_VALUE) return 0;
    unsigned long crc = 0xFFFFFFFFUL;
    static unsigned char buf[16384];
    DWORD n;
    *size = 0;
    while (ReadFile(h, buf, sizeof buf, &n, NULL) && n) {
        for (DWORD i = 0; i < n; i++) crc = tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
        *size += (long)n;
    }
    CloseHandle(h);
    return crc ^ 0xFFFFFFFFUL;
}

// lzx.msi: the embedded cabinet is LZX-compressed (window 2^21, E8 translation, verbatim/aligned/uncompressed blocks).
static void lzx_tests(void) {
    wchar_t pkg[MAX_PATH], p[MAX_PATH];
    swprintf(pkg, MAX_PATH, L"%ls\\lzx.msi", g_dir);
    const wchar_t *code = L"{3F0B7C1E-5D2A-4E8B-9A41-6C7D8E9F0A1B}";
    const wchar_t *root = L"C:\\Program Files (x86)\\Soul LZX";
    P(L"== lzx.msi ==");
    g_error = 0;
    UINT r = MsiInstallProductW(pkg, L"");
    P(L"install: %u errors=%d state=%d", r, g_error, (int)MsiQueryProductStateW(code));
    const wchar_t *files[] = { L"code.exe", L"text.txt", L"sub\\table.dat", L"tiny.txt", L"empty.bin" };
    for (int i = 0; i < 5; i++) {
        long sz;
        swprintf(p, MAX_PATH, L"%ls\\%ls", root, files[i]);
        unsigned long c = crc32_file(p, &sz);
        P(L"%ls: size=%ld crc=%08lx", files[i], sz, c);
    }
    swprintf(p, MAX_PATH, L"%ls\\tiny.txt", root);
    show_file(L"tiny.txt", p);
    r = MsiInstallProductW(pkg, L"REMOVE=ALL");
    P(L"uninstall: %u", r);
    for (int i = 0; i < 5; i++) {
        swprintf(p, MAX_PATH, L"%ls\\%ls", root, files[i]);
        P(L"%ls exists: %d", files[i], exists(p));
    }
    P(L"install dir exists: %d", exists(root));
    P(L"state: %d", (int)MsiQueryProductStateW(code));
}

static void msiexec_tests(void) {
    wchar_t args[MAX_PATH * 2];
    P(L"== msiexec ==");
    swprintf(args, MAX_PATH * 2, L"/i %ls\\sample.msi /qn", g_dir);
    DWORD r = run_msiexec(args);
    P(L"msiexec /i /qn: %lu readme exists: %d state: %d", r, exists(L"C:\\Program Files (x86)\\Soul \u00d6rnek\\readme.txt"), (int)MsiQueryProductStateW(L"{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}"));
    r = run_msiexec(L"/x {5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8} /qn");
    P(L"msiexec /x /qn: %lu readme exists: %d state: %d", r, exists(L"C:\\Program Files (x86)\\Soul \u00d6rnek\\readme.txt"), (int)MsiQueryProductStateW(L"{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}"));
    swprintf(args, MAX_PATH * 2, L"/i %ls\\sample.msi /qn INSTALLDIR=\"C:\\Soul Test\\Dir 1\"", g_dir);
    r = run_msiexec(args);
    P(L"msiexec /i INSTALLDIR: %lu readme exists: %d", r, exists(L"C:\\Soul Test\\Dir 1\\readme.txt"));
    swprintf(args, MAX_PATH * 2, L"/x %ls\\sample.msi /qn", g_dir);
    r = run_msiexec(args);
    P(L"msiexec /x package: %lu readme exists: %d dir exists: %d", r, exists(L"C:\\Soul Test\\Dir 1\\readme.txt"), exists(L"C:\\Soul Test\\Dir 1"));
    r = run_msiexec(L"/x {5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8} /qn");
    P(L"msiexec /x again: %lu", r);
}

// "msiexec" by name (a real file in system32) and ShellExecute of a .msi
static void shell_tests(void) {
    const wchar_t *code = L"{5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8}";
    P(L"== CreateProcess / ShellExecute ==");
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    char cmd[512];
    strcpy(cmd, "msiexec /i C:\\sample.msi /qn");
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    DWORD code_ = 99999;
    if (ok) { WaitForSingleObject(pi.hProcess, 60000); GetExitCodeProcess(pi.hProcess, &code_); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    P(L"CreateProcess msiexec /i: %d exit %lu state %d", ok, code_, (int)MsiQueryProductStateW(code));
    strcpy(cmd, "msiexec /x {5777B2F1-FBEE-4B87-9C3D-79BC72E3C3E8} /qn");
    ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    code_ = 99999;
    if (ok) { WaitForSingleObject(pi.hProcess, 60000); GetExitCodeProcess(pi.hProcess, &code_); CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    P(L"CreateProcess msiexec /x: %d exit %lu state %d", ok, code_, (int)MsiQueryProductStateW(code));
    SHELLEXECUTEINFOW sx = { sizeof sx };
    sx.fMask = SEE_MASK_NOCLOSEPROCESS;
    sx.lpFile = L"C:\\sample.msi";
    sx.lpParameters = L"/qn";
    sx.nShow = SW_SHOWNORMAL;
    ok = ShellExecuteExW(&sx);
    code_ = 99999;
    if (ok && sx.hProcess) { WaitForSingleObject(sx.hProcess, 60000); GetExitCodeProcess(sx.hProcess, &code_); CloseHandle(sx.hProcess); }
    P(L"ShellExecuteEx .msi: %d exit %lu state %d", ok, code_, (int)MsiQueryProductStateW(code));
    MsiConfigureProductW(code, 0, INSTALLSTATE_ABSENT);
    P(L"removed: state %d", (int)MsiQueryProductStateW(code));
}

int main(int argc, char **argv) {
    GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    wchar_t *sl = wcsrchr(g_dir, L'\\');
    if (sl) *sl = 0;
    P(L"msi test");
    sample_tests();
    ca_tests();
    lzx_tests();
    msiexec_tests();
    shell_tests();
    return 0;
}
