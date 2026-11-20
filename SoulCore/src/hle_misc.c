// SoulCore - user32 / advapi32 / shell32 / bcrypt / ntdll helpers.
#include "hle.h"
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <wctype.h>

#define CSTR(n) ((const char *)(uintptr_t)hle_arg(st, (n)))
#define WSTR(n) ((const uint16_t *)(uintptr_t)hle_arg(st, (n)))

static void random_bytes(void *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, n);
        close(fd);
        if (r == (ssize_t)n) return;
    }
    for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = (uint8_t)random();
}

// ---- user32 ------------------------------------------------------------------------------

static int message_box(const char *title, const char *text, unsigned type) {
    SoulProcess *p = g_proc;
    con_flush();
    if (p->opts.message_box) return p->opts.message_box(p->opts.ctx, title ? title : "", text ? text : "", type);
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "\n┌─ %s ─\n│ %s\n└─\n", title ? title : "", text ? text : "");
    con_write(1, buf, (size_t)n);
    return 1;
}
HLE(u32_MessageBoxA) { RET(message_box(ARG(2) ? CSTR(2) : "Error", ARG(1) ? CSTR(1) : "", (unsigned)ARG(3))); }
HLE(u32_MessageBoxW) {
    char *t = ARG(2) ? w2u_dup(WSTR(2)) : strdup("Error");
    char *m = ARG(1) ? w2u_dup(WSTR(1)) : strdup("");
    RET(message_box(t, m, (unsigned)ARG(3)));
    free(t); free(m);
}
HLE(u32_GetSystemMetrics) {
    switch ((int)ARG(0)) {
    case 0: RET(2360); break;  // SM_CXSCREEN (iPad Air 11" in points*2)
    case 1: RET(1640); break;  // SM_CYSCREEN
    case 80: RET(1); break;    // SM_CMONITORS
    default: RET(0); break;
    }
}
HLE(u32_wsprintfA) {
    char *f = strdup(CSTR(1));
    ArgSrc a = { st, 2, 0 };
    size_t n;
    char *s = fmt_vformat(f, &a, 0, &n);
    if (n > 1023) n = 1023;
    memcpy(PTR(0), s, n);
    ((char *)PTR(0))[n] = 0;
    free(s); free(f);
    RET(n);
}
HLE(u32_wsprintfW) {
    char *f = w2u_dup(WSTR(1));
    ArgSrc a = { st, 2, 0 };
    size_t n;
    char *s = fmt_vformat(f, &a, 1, &n);
    size_t w = u2w(s, (long)n, PTR(0), 1024);
    free(s); free(f);
    RET(w);
}
HLE(u32_wvsprintfA) {
    char *f = strdup(CSTR(1));
    ArgSrc a = { st, 0, ARG(2) };
    size_t n;
    char *s = fmt_vformat(f, &a, 0, &n);
    if (n > 1023) n = 1023;
    memcpy(PTR(0), s, n);
    ((char *)PTR(0))[n] = 0;
    free(s); free(f);
    RET(n);
}
HLE(u32_CharUpperA) { if (ARG(0) < 0x10000) { RET(toupper((int)ARG(0))); return; } for (char *p = PTR(0); *p; p++) *p = (char)toupper((unsigned char)*p); RET(ARG(0)); }
HLE(u32_CharLowerA) { if (ARG(0) < 0x10000) { RET(tolower((int)ARG(0))); return; } for (char *p = PTR(0); *p; p++) *p = (char)tolower((unsigned char)*p); RET(ARG(0)); }
HLE(u32_CharUpperW) { if (ARG(0) < 0x10000) { RET((uint16_t)towupper((wint_t)ARG(0))); return; } for (uint16_t *p = PTR(0); *p; p++) *p = (uint16_t)towupper(*p); RET(ARG(0)); }
HLE(u32_CharLowerW) { if (ARG(0) < 0x10000) { RET((uint16_t)towlower((wint_t)ARG(0))); return; } for (uint16_t *p = PTR(0); *p; p++) *p = (uint16_t)towlower(*p); RET(ARG(0)); }
HLE(u32_CharNextA) { const char *p = CSTR(0); RET(*p ? p + 1 : p); }
HLE(u32_CharNextW) { const uint16_t *p = WSTR(0); RET(*p ? p + 1 : p); }
HLE(u32_CharPrevA) { const char *s = CSTR(0), *p = CSTR(1); RET(p > s ? p - 1 : s); }
HLE(u32_IsCharAlphaA) { RET(isalpha((unsigned char)ARG(0)) != 0); }
HLE(u32_IsCharAlphaW) { RET(iswalpha((wint_t)(uint16_t)ARG(0)) != 0); }
HLE(u32_IsCharAlphaNumericW) { RET(iswalnum((wint_t)(uint16_t)ARG(0)) != 0); }
HLE(u32_LoadStringA) { if (ARG(3)) ((char *)PTR(2))[0] = 0; RET(0); }
HLE(u32_LoadStringW) { if (ARG(3)) ((uint16_t *)PTR(2))[0] = 0; RET(0); }
HLE_STUB_RET(u32_GetDesktopWindow, 0x10010)
HLE_STUB_RET(u32_GetForegroundWindow, 0)
HLE_STUB_RET(u32_GetActiveWindow, 0)
HLE_STUB_RET(u32_GetKeyState, 0)
HLE_STUB_RET(u32_GetAsyncKeyState, 0)
HLE_STUB_RET(u32_MessageBeep, 1)
HLE_STUB_RET(u32_GetProcessWindowStation, 0x20)
HLE(u32_GetUserObjectInformationW) {
    // claim an interactive window station so CRTs show message boxes
    if (ARG(1) == 1 && ARG(2) && ARG(3) >= 12) { memset(PTR(2), 0, 12); mem_wr(ARG(2) + 8, 4, 1); }
    if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, 12);
    RET(1);
}
HLE_STUB_RET(u32_SetProcessDPIAware, 1)
HLE_STUB_RET(u32_SetProcessDpiAwarenessContext, 1)
HLE(u32_GetDpiForSystem) { RET(96); }

// ---- advapi32 -------------------------------------------------------------------------------

static void user_name(X86State *st, int wide) {
    uint32_t cap = (uint32_t)mem_rd(ARG(1), 4);
    const char *n = "soul";
    size_t need = 5;
    if (cap < need) { mem_wr(ARG(1), 4, need); set_last_error(st, ERROR_INSUFFICIENT_BUFFER); RET(0); return; }
    if (wide) u2w(n, -1, PTR(0), cap); else memcpy(PTR(0), n, need);
    mem_wr(ARG(1), 4, need);
    RET(1);
}
HLE(adv_GetUserNameA) { user_name(st, 0); }
HLE(adv_GetUserNameW) { user_name(st, 1); }
HLE(adv_RegOpenKeyEx) { if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 8, 0); RET(ERROR_FILE_NOT_FOUND); }
HLE(adv_RegOpenKey) { if (ARG(2)) mem_wr(ARG(2), 8, 0); RET(ERROR_FILE_NOT_FOUND); }
HLE(adv_RegCreateKeyEx) { if (hle_arg(st, 7)) mem_wr(hle_arg(st, 7), 8, 0x80001234ull); if (hle_arg(st, 8)) mem_wr(hle_arg(st, 8), 4, 1); RET(0); }
HLE_STUB_RET(adv_RegQueryValueEx, ERROR_FILE_NOT_FOUND)
HLE_STUB_RET(adv_RegGetValue, ERROR_FILE_NOT_FOUND)
HLE_STUB_RET(adv_RegSetValueEx, 0)
HLE_STUB_RET(adv_RegCloseKey, 0)
HLE_STUB_RET(adv_RegDeleteKey, 0)
HLE_STUB_RET(adv_RegDeleteValue, 0)
HLE_STUB_RET(adv_RegEnumKeyEx, 259)
HLE_STUB_RET(adv_RegEnumValue, 259)
HLE_STUB_RET(adv_RegQueryInfoKey, ERROR_FILE_NOT_FOUND)
HLE(adv_SystemFunction036) { random_bytes(PTR(0), (uint32_t)ARG(1)); RET(1); }
HLE(adv_CryptAcquireContext) { mem_wr(ARG(0), 8, 0x5000); RET(1); }
HLE(adv_CryptGenRandom) { random_bytes(PTR(2), (uint32_t)ARG(1)); RET(1); }
HLE_STUB_RET(adv_CryptReleaseContext, 1)
HLE(adv_OpenProcessToken) { mem_wr(ARG(2), 8, 0x6000); RET(1); }
HLE(adv_GetTokenInformation) { if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, 0); set_last_error(st, ERROR_INVALID_PARAMETER); RET(0); }
HLE_STUB_RET(adv_EventRegister, 0)
HLE_STUB_RET(adv_EventUnregister, 0)
HLE_STUB_RET(adv_EventWriteTransfer, 0)
HLE_STUB_RET(adv_EventSetInformation, 0)
HLE(bcr_BCryptGenRandom) { random_bytes(PTR(1), (uint32_t)ARG(2)); RET(0); }
HLE(bcr_ProcessPrng) { random_bytes(PTR(0), ARG(1)); RET(1); }

// ---- shell32 --------------------------------------------------------------------------------

static const char *csidl_path(int csidl) {
    switch (csidl & 0xFF) {
    case 0x00: case 0x10: return "C:\\users\\soul\\Desktop";
    case 0x05: return "C:\\users\\soul\\Documents";
    case 0x1A: return "C:\\users\\soul\\AppData\\Roaming";
    case 0x1C: return "C:\\users\\soul\\AppData\\Local";
    case 0x23: return "C:\\ProgramData";
    case 0x24: return "C:\\windows";
    case 0x25: case 0x29: return "C:\\windows\\system32";
    case 0x26: case 0x2A: return "C:\\Program Files";
    case 0x28: return "C:\\users\\soul";
    default: return "C:\\users\\soul";
    }
}
HLE(sh_SHGetFolderPathA) { snprintf(PTR(4), 260, "%s", csidl_path((int)ARG(1))); RET(0); }
HLE(sh_SHGetFolderPathW) { u2w(csidl_path((int)ARG(1)), -1, PTR(4), 260); RET(0); }
HLE(sh_SHGetSpecialFolderPathA) { snprintf(PTR(1), 260, "%s", csidl_path((int)ARG(2))); RET(1); }
HLE(sh_SHGetSpecialFolderPathW) { u2w(csidl_path((int)ARG(2)), -1, PTR(1), 260); RET(1); }
HLE(sh_SHGetKnownFolderPath) {
    // FOLDERID_RoamingAppData {3EB685DB-...}, LocalAppData {F1B32785-...}, Documents {FDD39AD0-...}
    uint32_t d1 = (uint32_t)mem_rd(ARG(0), 4);
    const char *p = "C:\\users\\soul";
    if (d1 == 0x3EB685DB) p = "C:\\users\\soul\\AppData\\Roaming";
    else if (d1 == 0xF1B32785) p = "C:\\users\\soul\\AppData\\Local";
    else if (d1 == 0xFDD39AD0) p = "C:\\users\\soul\\Documents";
    else if (d1 == 0xB4BFCC3A) p = "C:\\users\\soul\\Desktop";
    else if (d1 == 0x905E63B6) p = "C:\\Program Files";
    else if (d1 == 0x62AB5D82) p = "C:\\ProgramData";
    mem_wr(ARG(3), 8, (uint64_t)(uintptr_t)u2w_dup(p));
    RET(0);
}
HLE(ole_CoTaskMemFree) { free(PTR(0)); RET(0); }
HLE(ole_CoTaskMemAlloc) { RET(malloc(ARG(0) ? ARG(0) : 1)); }
HLE_STUB_RET(ole_CoInitialize, 0)
HLE_STUB_RET(ole_CoUninitialize, 0)
HLE_STUB_RET(ole_CoCreateInstance, 0x80040154)
HLE(sh_CommandLineToArgvW) {
    const uint16_t *cmd = WSTR(0);
    char *s = w2u_dup(cmd);
    char *argv[128];
    int argc = 0;
    char *p = s;
    while (*p && argc < 127) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *out = p, *start = p;
        int inq = 0;
        while (*p && (inq || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') { inq = !inq; p++; continue; }
            *out++ = *p++;
        }
        if (*p) p++;
        *out = 0;
        argv[argc++] = start;
    }
    size_t total = sizeof(uint64_t) * (size_t)(argc + 1);
    for (int i = 0; i < argc; i++) total += (u2w(argv[i], -1, NULL, 0) + 1) * 2;
    uint8_t *blk = malloc(total);
    uint64_t *ptrs = (uint64_t *)blk;
    uint16_t *strs = (uint16_t *)(blk + sizeof(uint64_t) * (size_t)(argc + 1));
    for (int i = 0; i < argc; i++) {
        ptrs[i] = (uint64_t)(uintptr_t)strs;
        size_t n = u2w(argv[i], -1, strs, total);
        strs += n + 1;
    }
    ptrs[argc] = 0;
    mem_wr(ARG(1), 4, (uint32_t)argc);
    free(s);
    RET(blk);
}
HLE(sh_ShellExecute) { soul_log("[api] ShellExecute desteklenmiyor\n"); RET(2); }

// ---- ntdll -----------------------------------------------------------------------------------

HLE_STUB_RET(nt_NtQuerySystemInformation, 0xC0000002)
HLE_STUB_RET(nt_NtQueryInformationProcess, 0xC0000002)
HLE(nt_RtlNtStatusToDosError) { RET(ERROR_INVALID_FUNCTION); }
HLE(nt_RtlGetNtVersionNumbers) { if (ARG(0)) mem_wr(ARG(0), 4, 10); if (ARG(1)) mem_wr(ARG(1), 4, 0); if (ARG(2)) mem_wr(ARG(2), 4, 0xF0004A65); RET(0); }

// version.dll
HLE_STUB_RET(ver_GetFileVersionInfoSize, 0)
HLE_STUB_RET(ver_GetFileVersionInfo, 0)
HLE_STUB_RET(ver_VerQueryValue, 0)

// winmm
HLE(mm_timeGetTime) { RET((uint32_t)(soul_now() * 1000.0)); }
HLE_STUB_RET(mm_timeBeginPeriod, 0)
HLE_STUB_RET(mm_PlaySound, 1)

const HleEntry hle_misc_table[] = {
    { "MessageBoxA", u32_MessageBoxA, NULL }, { "MessageBoxW", u32_MessageBoxW, NULL }, { "MessageBoxExA", u32_MessageBoxA, NULL },
    { "MessageBoxExW", u32_MessageBoxW, NULL }, { "GetSystemMetrics", u32_GetSystemMetrics, NULL }, { "wsprintfA", u32_wsprintfA, NULL },
    { "wsprintfW", u32_wsprintfW, NULL }, { "wvsprintfA", u32_wvsprintfA, NULL }, { "CharUpperA", u32_CharUpperA, NULL }, { "CharLowerA", u32_CharLowerA, NULL },
    { "CharUpperW", u32_CharUpperW, NULL }, { "CharLowerW", u32_CharLowerW, NULL }, { "CharNextA", u32_CharNextA, NULL }, { "CharNextW", u32_CharNextW, NULL },
    { "CharPrevA", u32_CharPrevA, NULL }, { "IsCharAlphaA", u32_IsCharAlphaA, NULL }, { "IsCharAlphaW", u32_IsCharAlphaW, NULL },
    { "IsCharAlphaNumericW", u32_IsCharAlphaNumericW, NULL }, { "LoadStringA", u32_LoadStringA, NULL }, { "LoadStringW", u32_LoadStringW, NULL },
    { "GetDesktopWindow", u32_GetDesktopWindow, NULL }, { "GetForegroundWindow", u32_GetForegroundWindow, NULL }, { "GetActiveWindow", u32_GetActiveWindow, NULL },
    { "GetKeyState", u32_GetKeyState, NULL }, { "GetAsyncKeyState", u32_GetAsyncKeyState, NULL }, { "MessageBeep", u32_MessageBeep, NULL },
    { "GetProcessWindowStation", u32_GetProcessWindowStation, NULL }, { "GetUserObjectInformationW", u32_GetUserObjectInformationW, NULL },
    { "SetProcessDPIAware", u32_SetProcessDPIAware, NULL }, { "SetProcessDpiAwarenessContext", u32_SetProcessDpiAwarenessContext, NULL },
    { "GetDpiForSystem", u32_GetDpiForSystem, NULL },
    { "GetUserNameA", adv_GetUserNameA, NULL }, { "GetUserNameW", adv_GetUserNameW, NULL }, { "RegOpenKeyExA", adv_RegOpenKeyEx, NULL },
    { "RegOpenKeyExW", adv_RegOpenKeyEx, NULL }, { "RegOpenKeyA", adv_RegOpenKey, NULL }, { "RegOpenKeyW", adv_RegOpenKey, NULL },
    { "RegCreateKeyExA", adv_RegCreateKeyEx, NULL }, { "RegCreateKeyExW", adv_RegCreateKeyEx, NULL }, { "RegQueryValueExA", adv_RegQueryValueEx, NULL },
    { "RegQueryValueExW", adv_RegQueryValueEx, NULL }, { "RegGetValueA", adv_RegGetValue, NULL }, { "RegGetValueW", adv_RegGetValue, NULL },
    { "RegSetValueExA", adv_RegSetValueEx, NULL }, { "RegSetValueExW", adv_RegSetValueEx, NULL }, { "RegCloseKey", adv_RegCloseKey, NULL },
    { "RegDeleteKeyA", adv_RegDeleteKey, NULL }, { "RegDeleteKeyW", adv_RegDeleteKey, NULL }, { "RegDeleteValueA", adv_RegDeleteValue, NULL },
    { "RegDeleteValueW", adv_RegDeleteValue, NULL }, { "RegEnumKeyExA", adv_RegEnumKeyEx, NULL }, { "RegEnumKeyExW", adv_RegEnumKeyEx, NULL },
    { "RegEnumValueA", adv_RegEnumValue, NULL }, { "RegEnumValueW", adv_RegEnumValue, NULL }, { "RegQueryInfoKeyW", adv_RegQueryInfoKey, NULL },
    { "SystemFunction036", adv_SystemFunction036, NULL }, { "CryptAcquireContextA", adv_CryptAcquireContext, NULL },
    { "CryptAcquireContextW", adv_CryptAcquireContext, NULL }, { "CryptGenRandom", adv_CryptGenRandom, NULL }, { "CryptReleaseContext", adv_CryptReleaseContext, NULL },
    { "OpenProcessToken", adv_OpenProcessToken, NULL }, { "GetTokenInformation", adv_GetTokenInformation, NULL },
    { "EventRegister", adv_EventRegister, NULL }, { "EventUnregister", adv_EventUnregister, NULL }, { "EventWriteTransfer", adv_EventWriteTransfer, NULL },
    { "EventSetInformation", adv_EventSetInformation, NULL },
    { "BCryptGenRandom", bcr_BCryptGenRandom, NULL }, { "ProcessPrng", bcr_ProcessPrng, NULL },
    { "SHGetFolderPathA", sh_SHGetFolderPathA, NULL }, { "SHGetFolderPathW", sh_SHGetFolderPathW, NULL },
    { "SHGetSpecialFolderPathA", sh_SHGetSpecialFolderPathA, NULL }, { "SHGetSpecialFolderPathW", sh_SHGetSpecialFolderPathW, NULL },
    { "SHGetKnownFolderPath", sh_SHGetKnownFolderPath, NULL }, { "CoTaskMemFree", ole_CoTaskMemFree, NULL }, { "CoTaskMemAlloc", ole_CoTaskMemAlloc, NULL },
    { "CoInitialize", ole_CoInitialize, NULL }, { "CoInitializeEx", ole_CoInitialize, NULL }, { "CoUninitialize", ole_CoUninitialize, NULL },
    { "CoCreateInstance", ole_CoCreateInstance, NULL },
    { "CommandLineToArgvW", sh_CommandLineToArgvW, NULL }, { "ShellExecuteA", sh_ShellExecute, NULL }, { "ShellExecuteW", sh_ShellExecute, NULL },
    { "NtQuerySystemInformation", nt_NtQuerySystemInformation, NULL }, { "NtQueryInformationProcess", nt_NtQueryInformationProcess, NULL },
    { "RtlNtStatusToDosError", nt_RtlNtStatusToDosError, NULL }, { "RtlGetNtVersionNumbers", nt_RtlGetNtVersionNumbers, NULL },
    { "GetFileVersionInfoSizeA", ver_GetFileVersionInfoSize, NULL }, { "GetFileVersionInfoSizeW", ver_GetFileVersionInfoSize, NULL },
    { "GetFileVersionInfoA", ver_GetFileVersionInfo, NULL }, { "GetFileVersionInfoW", ver_GetFileVersionInfo, NULL },
    { "VerQueryValueA", ver_VerQueryValue, NULL }, { "VerQueryValueW", ver_VerQueryValue, NULL },
    { "timeGetTime", mm_timeGetTime, NULL }, { "timeBeginPeriod", mm_timeBeginPeriod, NULL }, { "timeEndPeriod", mm_timeBeginPeriod, NULL },
    { "PlaySoundA", mm_PlaySound, NULL }, { "PlaySoundW", mm_PlaySound, NULL },
    { NULL, NULL, NULL }
};
