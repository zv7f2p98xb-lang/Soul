// SoulCore - user32 / advapi32 / shell32 / bcrypt / ntdll helpers.
#include "hle.h"
#include <ctype.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <wctype.h>

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))

static void random_bytes(void *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t r = read(fd, buf, n);
        close(fd);
        if (r == (ssize_t)n) return;
    }
    for (size_t i = 0; i < n; i++) ((uint8_t *)buf)[i] = (uint8_t)random();
}

HLE_STUB_RET(crt_ismbblead, 0)

// ---- user32 ------------------------------------------------------------------------------

int soul_message_box(const char *title, const char *text, unsigned type) {
    SoulProcess *p = g_proc;
    con_flush();
    if (p->opts.message_box) {
        SoulThread *self = soul_cur_thread();
        if (self) gil_unlock(self); // the UI may take a while; let other guest threads run
        int r = p->opts.message_box(p->opts.ctx, title ? title : "", text ? text : "", type);
        if (self) gil_lock(self);
        return r;
    }
    char buf[2048];
    int n = snprintf(buf, sizeof buf, "\n┌─ %s ─\n│ %s\n└─\n", title ? title : "", text ? text : "");
    con_write(1, buf, (size_t)n);
    return 1;
}
HLE(u32_MessageBoxA) { RET(soul_message_box(ARG(2) ? CSTR(2) : "Error", ARG(1) ? CSTR(1) : "", (unsigned)ARG(3))); }
HLE(u32_MessageBoxW) {
    char *t = ARG(2) ? w2u_dup(WSTR(2)) : strdup("Error");
    char *m = ARG(1) ? w2u_dup(WSTR(1)) : strdup("");
    RET(soul_message_box(t, m, (unsigned)ARG(3)));
    free(t); free(m);
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
HLE(adv_SystemFunction036) { random_bytes(PTR(0), (uint32_t)ARG(1)); RET(1); }
HLE(adv_CryptAcquireContext) { wr_ptr(ARG(0), 0x5000); RET(1); }
HLE(adv_CryptGenRandom) { random_bytes(PTR(2), (uint32_t)ARG(1)); RET(1); }
HLE_STUB_RET(adv_CryptReleaseContext, 1)
HLE(adv_OpenProcessToken) { wr_ptr(ARG(2), 0x6000); RET(1); }
HLE(adv_GetTokenInformation) { if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, 0); set_last_error(st, ERROR_INVALID_PARAMETER); RET(0); }
HLE_STUB_RET(adv_EventRegister, 0)
HLE_STUB_RET(adv_EventUnregister, 0)
HLE_STUB_RET(adv_EventWriteTransfer, 0)
HLE_STUB_RET(adv_EventSetInformation, 0)
HLE(bcr_BCryptGenRandom) { random_bytes(PTR(1), (uint32_t)ARG(2)); RET(0); }
HLE(bcr_ProcessPrng) { random_bytes(PTR(0), ARG(1)); RET(1); }

// ---- shell32 --------------------------------------------------------------------------------

const char *soul_csidl_path(int csidl) {
#define SM "C:\\users\\soul\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu"
#define CSM "C:\\ProgramData\\Microsoft\\Windows\\Start Menu"
    switch (csidl & 0xFF) {
    case 0x00: case 0x10: return "C:\\users\\soul\\Desktop";
    case 0x02: return SM "\\Programs";
    case 0x05: return "C:\\users\\soul\\Documents";
    case 0x06: return "C:\\users\\soul\\Favorites";
    case 0x07: return SM "\\Programs\\Startup";
    case 0x08: return "C:\\users\\soul\\AppData\\Roaming\\Microsoft\\Windows\\Recent";
    case 0x09: return "C:\\users\\soul\\AppData\\Roaming\\Microsoft\\Windows\\SendTo";
    case 0x0B: return SM;
    case 0x0D: return "C:\\users\\soul\\Music";
    case 0x0E: return "C:\\users\\soul\\Videos";
    case 0x14: return "C:\\windows\\Fonts";
    case 0x15: return "C:\\users\\soul\\AppData\\Roaming\\Microsoft\\Windows\\Templates";
    case 0x16: return CSM;
    case 0x17: return CSM "\\Programs";
    case 0x18: return CSM "\\Programs\\Startup";
    case 0x19: return "C:\\users\\Public\\Desktop";
    case 0x1A: return "C:\\users\\soul\\AppData\\Roaming";
    case 0x1C: return "C:\\users\\soul\\AppData\\Local";
    case 0x20: return "C:\\users\\soul\\AppData\\Local\\Microsoft\\Windows\\INetCache";
    case 0x23: return "C:\\ProgramData";
    case 0x24: return "C:\\windows";
    case 0x25: case 0x29: return "C:\\windows\\system32";
    case 0x26: return g_mode32 ? "C:\\Program Files (x86)" : "C:\\Program Files";
    case 0x2A: return "C:\\Program Files (x86)";
    case 0x2B: return g_mode32 ? "C:\\Program Files (x86)\\Common Files" : "C:\\Program Files\\Common Files";
    case 0x2C: return "C:\\Program Files (x86)\\Common Files";
    case 0x2D: return "C:\\ProgramData\\Microsoft\\Windows\\Templates";
    case 0x2E: return "C:\\users\\Public\\Documents";
    case 0x27: return "C:\\users\\soul\\Pictures";
    case 0x28: return "C:\\users\\soul";
    default: return "C:\\users\\soul";
    }
#undef SM
#undef CSM
}
HLE(sh_SHGetFolderPathA) { snprintf(PTR(4), 260, "%s", soul_csidl_path((int)ARG(1))); RET(0); }
HLE(sh_SHGetFolderPathW) { u2w(soul_csidl_path((int)ARG(1)), -1, PTR(4), 260); RET(0); }
HLE(sh_SHGetSpecialFolderPathA) { snprintf(PTR(1), 260, "%s", soul_csidl_path((int)ARG(2))); RET(1); }
HLE(sh_SHGetSpecialFolderPathW) { u2w(soul_csidl_path((int)ARG(2)), -1, PTR(1), 260); RET(1); }
HLE(sh_SHGetKnownFolderPath) {
    // FOLDERID_RoamingAppData {3EB685DB-...}, LocalAppData {F1B32785-...}, Documents {FDD39AD0-...}
    uint32_t d1 = (uint32_t)mem_rd(ARG(0), 4);
    const char *p = "C:\\users\\soul";
    if (d1 == 0x3EB685DB) p = "C:\\users\\soul\\AppData\\Roaming";
    else if (d1 == 0xF1B32785) p = "C:\\users\\soul\\AppData\\Local";
    else if (d1 == 0xFDD39AD0) p = "C:\\users\\soul\\Documents";
    else if (d1 == 0xB4BFCC3A) p = "C:\\users\\soul\\Desktop";
    else if (d1 == 0x905E63B6) p = g_mode32 ? "C:\\Program Files (x86)" : "C:\\Program Files";
    else if (d1 == 0x7C5A40EF) p = "C:\\Program Files (x86)";
    else if (d1 == 0x62AB5D82) p = "C:\\ProgramData";
    wr_ptr(ARG(3), H2G(u2w_gdup(p)));
    RET(0);
}
HLE(ole_CoTaskMemFree) { g_free(PTR(0)); RET(0); }
HLE(ole_CoTaskMemAlloc) { RET(g_malloc(ARG(0) ? ARG(0) : 1)); }
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
    const size_t psz = (size_t)PSZ;
    size_t total = psz * (size_t)(argc + 1);
    for (int i = 0; i < argc; i++) total += (u2w(argv[i], -1, NULL, 0) + 1) * 2;
    uint8_t *blk = g_malloc(total); // freed by the program with LocalFree
    uint16_t *strs = (uint16_t *)(blk + psz * (size_t)(argc + 1));
    for (int i = 0; i < argc; i++) {
        wr_ptr(H2G(blk) + psz * (size_t)i, H2G(strs));
        size_t n = u2w(argv[i], -1, strs, total);
        strs += n + 1;
    }
    wr_ptr(H2G(blk) + psz * (size_t)argc, 0);
    mem_wr(ARG(1), 4, (uint32_t)argc);
    free(s);
    RET(blk);
}
// Programs start as new processes; links open in the iPad's browser; documents are shown by the
// iPad (Quick Look).
static char *opt_str(uint64_t p, int wide) { return !p ? NULL : wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p)); }
static void shell_execute(X86State *st, int wide) {
    char *file = opt_str(ARG(2), wide), *params = opt_str(ARG(3), wide), *dir = opt_str(hle_arg(st, 4), wide);
    SoulProcess *p;
    uint64_t r = shell_execute_file(file, params, dir, &p);
    free(file); free(params); free(dir);
    RET(r);
}
HLE(sh_ShellExecuteA) { shell_execute(st, 0); }
HLE(sh_ShellExecuteW) { shell_execute(st, 1); }
#define L_SEI "44PPPPP4PPPP4PP" // cbSize fMask hwnd verb file params dir nShow hInstApp idlist class hkeyClass hotkey hIcon hProcess
static void shell_execute_ex(X86State *st, int wide) {
    uint64_t info = ARG(0);
    uint32_t mask = (uint32_t)gs_get(info, L_SEI, 1);
    char *file = opt_str(gs_get(info, L_SEI, 4), wide), *params = opt_str(gs_get(info, L_SEI, 5), wide), *dir = opt_str(gs_get(info, L_SEI, 6), wide);
    if (!file && (mask & 4) && gs_get(info, L_SEI, 9)) { // SEE_MASK_IDLIST
        uint64_t id = gs_get(info, L_SEI, 9);
        if (mem_rd(id, 2) >= 7 && !memcmp(G2H(id + 2), "SOUL", 4)) file = strdup((const char *)G2H(id + 6));
    }
    SoulProcess *p = NULL;
    uint64_t r = shell_execute_file(file, params, dir, &p);
    free(file); free(params); free(dir);
    gs_set(info, L_SEI, 8, r);
    if (mask & 0x40) { // SEE_MASK_NOCLOSEPROCESS
        uint64_t h = 0;
        if (p) { HObj *o; h = h_alloc(H_PROCESS, &o); if (o) o->ptr = p; }
        gs_set(info, L_SEI, 14, h);
    }
    if (r <= 32) set_last_error(st, r == 2 ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED);
    RET(r > 32);
}
HLE(sh_ShellExecuteExA) { shell_execute_ex(st, 0); }
HLE(sh_ShellExecuteExW) { shell_execute_ex(st, 1); }

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
    { "_ismbblead", crt_ismbblead, NULL }, { "_ismbbtrail", crt_ismbblead, NULL },
    { "MessageBoxA", u32_MessageBoxA, NULL }, { "MessageBoxW", u32_MessageBoxW, NULL }, { "MessageBoxExA", u32_MessageBoxA, NULL },
    { "MessageBoxExW", u32_MessageBoxW, NULL }, { "wsprintfA", u32_wsprintfA, NULL },
    { "wsprintfW", u32_wsprintfW, NULL }, { "wvsprintfA", u32_wvsprintfA, NULL }, { "CharUpperA", u32_CharUpperA, NULL }, { "CharLowerA", u32_CharLowerA, NULL },
    { "CharUpperW", u32_CharUpperW, NULL }, { "CharLowerW", u32_CharLowerW, NULL }, { "CharNextA", u32_CharNextA, NULL }, { "CharNextW", u32_CharNextW, NULL },
    { "CharPrevA", u32_CharPrevA, NULL }, { "IsCharAlphaA", u32_IsCharAlphaA, NULL }, { "IsCharAlphaW", u32_IsCharAlphaW, NULL },
    { "IsCharAlphaNumericW", u32_IsCharAlphaNumericW, NULL }, 
    
    { "MessageBeep", u32_MessageBeep, NULL },
    { "GetProcessWindowStation", u32_GetProcessWindowStation, NULL }, { "GetUserObjectInformationW", u32_GetUserObjectInformationW, NULL },
    { "SetProcessDPIAware", u32_SetProcessDPIAware, NULL }, { "SetProcessDpiAwarenessContext", u32_SetProcessDpiAwarenessContext, NULL },
    { "GetDpiForSystem", u32_GetDpiForSystem, NULL },
    { "GetUserNameA", adv_GetUserNameA, NULL }, { "GetUserNameW", adv_GetUserNameW, NULL }, { "SystemFunction036", adv_SystemFunction036, NULL }, { "CryptAcquireContextA", adv_CryptAcquireContext, NULL },
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
    { "CommandLineToArgvW", sh_CommandLineToArgvW, NULL }, { "ShellExecuteA", sh_ShellExecuteA, NULL }, { "ShellExecuteW", sh_ShellExecuteW, NULL },
    { "ShellExecuteExA", sh_ShellExecuteExA, NULL }, { "ShellExecuteExW", sh_ShellExecuteExW, NULL },
    { "NtQuerySystemInformation", nt_NtQuerySystemInformation, NULL }, { "NtQueryInformationProcess", nt_NtQueryInformationProcess, NULL },
    { "RtlNtStatusToDosError", nt_RtlNtStatusToDosError, NULL }, { "RtlGetNtVersionNumbers", nt_RtlGetNtVersionNumbers, NULL },
    { "GetFileVersionInfoSizeA", ver_GetFileVersionInfoSize, NULL }, { "GetFileVersionInfoSizeW", ver_GetFileVersionInfoSize, NULL },
    { "GetFileVersionInfoA", ver_GetFileVersionInfo, NULL }, { "GetFileVersionInfoW", ver_GetFileVersionInfo, NULL },
    { "VerQueryValueA", ver_VerQueryValue, NULL }, { "VerQueryValueW", ver_VerQueryValue, NULL },
    { "timeGetTime", mm_timeGetTime, NULL }, { "timeBeginPeriod", mm_timeBeginPeriod, NULL }, { "timeEndPeriod", mm_timeBeginPeriod, NULL },
    { "PlaySoundA", mm_PlaySound, NULL }, { "PlaySoundW", mm_PlaySound, NULL },
    { NULL, NULL, NULL }
};
