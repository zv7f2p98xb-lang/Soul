// SoulCore - HLE core: import thunks, handles, paths, strings, console streams.
#include "hle.h"
#include "gui.h"
#include "stdcall_args.h"
#include <ctype.h>
#include <errno.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

// Per-process state (handles, environment, console buffers); see proc.c.
static HleState *g_hsp;
SOUL_PROCESS_VAR(g_hsp)
#define g_hs (*g_hsp)
HleState *hs(void) { return g_hsp; }
X86State *hle_state_of(void) { return g_proc ? g_proc->main_state : NULL; }

// ---- registry --------------------------------------------------------------

static const char *const k_system_dlls[] = {
    "kernel32", "kernelbase", "ntdll", "msvcrt", "ucrtbase", "ucrtbased", "vcruntime140", "vcruntime140d",
    "vcruntime140_1", "msvcr80", "msvcr90", "msvcr100", "msvcr110", "msvcr120", "crtdll",
    "user32", "gdi32", "advapi32", "shell32", "ole32", "oleaut32", "shlwapi", "ws2_32", "wsock32", "winmm",
    "comctl32", "comdlg32", "version", "imm32", "uxtheme", "dwmapi", "setupapi", "bcrypt", "bcryptprimitives",
    "crypt32", "psapi", "dbghelp", "winhttp", "wininet", "iphlpapi", "secur32", "userenv", "rpcrt4",
    "d3d9", "d3d11", "d3d12", "dxgi", "opengl32", "glu32", "xinput1_3", "xinput1_4", "xinput9_1_0", "dsound",
    "dinput8", "hid", "powrprof", "mpr", "netapi32", "wtsapi32", "normaliz", "sechost", "cfgmgr32",
    "shfolder", "apphelp", "propsys", "cryptbase", "oleacc", "clbcatq", "ntmarta", "riched20", "riched32", "msftedit",
    "msimg32", "winspool", "urlmon", "wintrust", "imagehlp", "shcore", "oledlg", "olepro32", "msacm32", "msvfw32",
    "avifil32", "credui", "dnsapi", "ncrypt", "sspicli", "profapi", "kernel.appcore", "msasn1", "mswsock", "wldap32",
    "dwrite", "d2d1", "windowscodecs", "gdiplus", "ddraw", "dinput", "cabinet", "msi", "mscoree", "uiautomationcore",
    "xaudio2_7", "xaudio2_8", "xaudio2_9", "xaudio2_9redist", "x3daudio1_7", "x3daudio1_9",
    "rstrtmgr", "msctf", "sxs", "wer", "faultrep", "dbgcore", "netapi32", "urlmon", "winhttp", "wininet", NULL
};

static void dll_key(const char *dll, char *out, size_t n) {
    size_t i = 0;
    for (; dll[i] && i + 1 < n; i++) out[i] = (char)tolower((unsigned char)dll[i]);
    out[i] = 0;
    char *dot = strrchr(out, '.');
    if (dot && (strcmp(dot, ".dll") == 0 || strcmp(dot, ".drv") == 0)) *dot = 0;
}

int hle_is_system_dll(const char *dll) {
    char k[128];
    dll_key(dll, k, sizeof k);
    if (strncmp(k, "api-ms-win-", 11) == 0 || strncmp(k, "ext-ms-", 7) == 0) return 1;
    for (int i = 0; k_system_dlls[i]; i++)
        if (strcmp(k, k_system_dlls[i]) == 0) return 1;
    return 0;
}

static const HleEntry *find_entry(const char *name) {
    // processes and COM first: they replace older stand-ins in the other tables
    const HleEntry *tables[] = { hle_process_table, hle_com_table, hle_audio_table, hle_msi_table, hle_gdiplus_table, hle_xaudio2_table, hle_kernel32_table, hle_crt_table, hle_user32_table, hle_controls_table,
                                 hle_comctl_table, hle_gdi32_table, hle_opengl_table, hle_registry_table, hle_misc_table, hle_shell_table,
                                 hle_oleaut_table, hle_shlwapi_table, hle_winsock_table, hle_inet_table };
    for (size_t t = 0; t < sizeof tables / sizeof *tables; t++)
        for (const HleEntry *e = tables[t]; e->name; e++)
            if (strcmp(e->name, name) == 0) return e;
    return NULL;
}

// Stack bytes a 32-bit system function removes on return (0 = cdecl or unknown).
// stdcall functions the generated table (MinGW import libraries) does not know
static const StdcallArgs k_extra_stdcall[] = {
    { "EventSetInformation", 20 }, { "InterlockedIncrement", 4 }, { "InterlockedDecrement", 4 }, { "InterlockedExchange", 8 },
    { "InterlockedExchangeAdd", 8 }, { "InterlockedCompareExchange", 12 }, { "InterlockedCompareExchange64", 20 }, { "GetDpiForMonitor", 16 }, { "GetDpiForShellUIComponent", 4 }, { "GetProcessDpiAwareness", 8 },
    { "GetScaleFactorForMonitor", 8 }, { "GetTempPath2W", 8 }, { "OpenThemeDataForDpi", 12 }, { "ProcessPrng", 8 },
    { "SetProcessDpiAwareness", 4 }, { "WaitOnAddress", 16 }, { "WakeByAddressAll", 4 }, { "WakeByAddressSingle", 4 }, { "XAudio2Create", 12 }, { "XAudio2CreateWithVersionInfo", 16 }, { "CreateFX", 16 },
    { "CreateAudioReverb", 4 }, { "CreateAudioVolumeMeter", 4 },
    { NULL, 0 }
};
static uint16_t stdcall_bytes(const char *name) {
    if (!name || name[0] == '#') return 0;
    for (const StdcallArgs *e = k_extra_stdcall; e->name; e++) if (!strcmp(e->name, name)) return e->bytes;
    int lo = 0, hi = K_STDCALL_ARGS_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(k_stdcall_args[mid].name, name);
        if (!c) return k_stdcall_args[mid].bytes;
        if (c < 0) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

uint32_t hle_thunk_for(SoulProcess *p, HleFn fn, const char *dll, const char *name) {
    for (uint32_t i = 1; i < p->thunk_count; i++) {
        HleThunk *t = &p->thunks[i];
        if (t->fn == fn && t->name && name && strcmp(t->name, name) == 0) return i;
    }
    if (p->thunk_count >= SOUL_MAX_THUNKS) return 0;
    uint32_t id = p->thunk_count++;
    HleThunk *t = &p->thunks[id];
    t->fn = fn;
    t->dll = dll ? strdup(dll) : NULL;
    t->name = name ? strdup(name) : NULL;
    t->argbytes = 0;
    if (g_mode32 && (!dll || hle_is_system_dll(dll))) {
        // C runtime DLLs are cdecl even where a same-named stdcall function exists elsewhere
        char k[128];
        dll_key(dll ? dll : "", k, sizeof k);
        int crt = strncmp(k, "msvc", 4) == 0 || strncmp(k, "ucrt", 4) == 0 || strncmp(k, "vcruntime", 9) == 0 ||
                  strcmp(k, "crtdll") == 0 || strncmp(k, "api-ms-win-crt-", 15) == 0;
        if (!crt) t->argbytes = stdcall_bytes(name);
    }
    uint8_t *c = p->thunk_mem + (size_t)id * SOUL_THUNK_STRIDE;
    c[0] = 0xCC; c[1] = 0x53; c[2] = 0x43;
    memcpy(c + 3, &id, 4);
    c[7] = 0xC3;
    return id;
}

static void hle_unimplemented(X86State *st) {
    SoulProcess *p = g_proc;
    HleThunk *t = &p->thunks[st->thread->cur_thunk];
    if (!t->warned) {
        t->warned = 1;
        soul_log("[api] Uygulanmamış işlev çağrıldı: %s!%s (0 döndürülüyor)\n", t->dll ? t->dll : "?", t->name ? t->name : "?");
    }
    RET(0);
}

uint64_t hle_get_proc(SoulProcess *p, const char *dll, const char *name) {
    const HleEntry *e = find_entry(name);
    if (!e) return 0;
    if (e->data) return H2G(e->data());
    uint32_t id = hle_thunk_for(p, e->fn, dll, name);
    return id ? p->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE : 0;
}

uint64_t hle_resolve_import(SoulProcess *p, const char *dll, const char *name, int ordinal) {
    if (!hle_is_system_dll(dll)) {
        PEModule *m = pe_find_loaded(p, dll);
        if (m) {
            uint64_t a = pe_find_export(m, name, ordinal);
            if (a) return a;
        }
    }
    if (!name) name = hle_ordinal_name(dll, ordinal); // system DLL functions imported by number
    if (name) {
        uint64_t a = hle_get_proc(p, dll, name);
        if (a) return a;
    }
    char label[300];
    if (name) snprintf(label, sizeof label, "%s", name);
    else snprintf(label, sizeof label, "#%d", ordinal);
    if (p->verbose) soul_log("[yükleyici] Taklit edilmeyen içe aktarım: %s!%s\n", dll, label);
    uint32_t id = hle_thunk_for(p, hle_unimplemented, dll, label);
    return id ? p->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE : 0;
}

void hle_invoke(X86State *st, uint32_t id) {
    SoulProcess *p = g_proc;
    if (id == 0 || id >= p->thunk_count) soul_fatal(st, "Geçersiz API köprüsü #%u", id);
    HleThunk *t = &p->thunks[id];
    SoulThread *th = st->thread;
    uint32_t prev = th->cur_thunk;
    th->skip_ret = 0;
    th->cur_thunk = id;
    g_hs.api_calls++;
    if (__builtin_expect(g_proc->verbose > 1, 0))
        soul_log("[api] %s!%s %llx %llx %llx\n", t->dll ? t->dll : "", t->name ? t->name : "?",
                 (unsigned long long)hle_arg(st, 0), (unsigned long long)hle_arg(st, 1), (unsigned long long)hle_arg(st, 2));
    t->fn(st);
    th->cur_thunk = prev;
    if (!th->skip_ret) {
        st->rip = pop64(st);
        if (g_mode32) st->gpr[RSP] = (uint32_t)(st->gpr[RSP] + t->argbytes); // stdcall: callee removes arguments
    }
    th->skip_ret = 0;
}

// ---- guest structure layouts -----------------------------------------------------

// '|' inside a layout aligns the next field to the pointer size without being a field itself:
// it marks the end of an embedded structure such as NMHDR (24 bytes in 64-bit programs).
static int gs_field_size(char c) { return c == 'P' ? PSZ : c - '0'; }
static const char *gs_field(const char *spec, int field) {
    for (int i = 0; *spec; spec++) { if (*spec == '|') continue; if (i++ == field) return spec; }
    return spec;
}
size_t gs_off(const char *spec, int field) {
    size_t o = 0;
    for (int i = 0; spec[0]; spec++) {
        if (spec[0] == '|') { o = (o + (size_t)PSZ - 1) & ~((size_t)PSZ - 1); continue; }
        int sz = gs_field_size(spec[0]);
        o = (o + (size_t)sz - 1) & ~((size_t)sz - 1);
        if (i++ == field) return o;
        o += (size_t)sz;
    }
    return o;
}
size_t gs_size(const char *spec) {
    size_t o = 0, al = 1;
    for (; spec[0]; spec++) {
        if (spec[0] == '|') { o = (o + (size_t)PSZ - 1) & ~((size_t)PSZ - 1); if ((size_t)PSZ > al) al = (size_t)PSZ; continue; }
        size_t sz = (size_t)gs_field_size(spec[0]);
        o = (o + sz - 1) & ~(sz - 1);
        o += sz;
        if (sz > al) al = sz;
    }
    return (o + al - 1) & ~(al - 1);
}
uint64_t gs_get(uint64_t base, const char *spec, int field) { return mem_rd(base + gs_off(spec, field), gs_field_size(*gs_field(spec, field))); }
void gs_set(uint64_t base, const char *spec, int field, uint64_t v) { mem_wr(base + gs_off(spec, field), gs_field_size(*gs_field(spec, field)), v); }

// ---- handles -----------------------------------------------------------------

uint64_t h_alloc(HType t, HObj **out) {
    for (int i = 0; i < MAX_HANDLES; i++) {
        if (g_hs.handles[i].type == H_FREE) {
            memset(&g_hs.handles[i], 0, sizeof(HObj));
            g_hs.handles[i].type = t;
            g_hs.handles[i].fd = -1;
            if (out) *out = &g_hs.handles[i];
            return HANDLE_BASE + (uint64_t)i * 4;
        }
    }
    return 0;
}

HObj *h_get(uint64_t h) {
    if ((int64_t)h >= -12 && (int64_t)h <= -10) h = h_std((int)(-10 - (int64_t)h));
    if (h < HANDLE_BASE) return NULL;
    uint64_t i = (h - HANDLE_BASE) / 4;
    if (i >= MAX_HANDLES || (h - HANDLE_BASE) % 4) return NULL;
    HObj *o = &g_hs.handles[i];
    return o->type == H_FREE ? NULL : o;
}

void h_free(uint64_t h) {
    HObj *o = h_get(h);
    if (!o) return;
    if (o->type == H_CONIN || o->type == H_CONOUT || o->type == H_CONERR) return; // console handles stay
    if (o->fd >= 0) close(o->fd);
    if (o->dir) closedir(o->dir);
    if (o->type == H_MAPPING && o->ptr) free(o->ptr);
    memset(o, 0, sizeof *o);
}

uint64_t h_std(int which) { return g_hs.std_handles[which]; }

void set_last_error(X86State *st, uint32_t e) {
    if (st && teb_of(st)) mem_wr(teb_of(st) + TEB_LASTERR, 4, e);
}
uint32_t get_last_error(X86State *st) {
    return (st && teb_of(st)) ? (uint32_t)mem_rd(teb_of(st) + TEB_LASTERR, 4) : 0;
}

uint32_t errno_to_win(int e) {
    switch (e) {
    case 0: return 0;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EISDIR: case EROFS: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case ENOSPC: return 112;
    case EPIPE: return ERROR_BROKEN_PIPE;
    default: return ERROR_INVALID_PARAMETER;
    }
}

// ---- strings ----------------------------------------------------------------

// Unicode upper/lower case for the scripts Windows programs meet most (Latin incl. Turkish,
// Greek, Cyrillic), independent of the host's C locale.
uint32_t uc_upper(uint32_t c) {
    if (c < 0x80) return c >= 'a' && c <= 'z' ? c - 32 : c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0xB5) return 0x39C;
    if (c == 0x131) return 'I';
    if (c == 0x17F) return 'S';
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : c;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x3B1 && c <= 0x3C9) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}
uint32_t uc_lower(uint32_t c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 32;
    if (c == 0x178) return 0xFF;
    if (c == 0x130) return 'i';
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c : c + 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c + 1 : c;
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c >= 0x400 && c <= 0x40F) return c + 80;
    return c;
}

size_t wstrlen(const uint16_t *w) {
    size_t n = 0;
    while (w[n]) n++;
    return n;
}

size_t w2u(const uint16_t *w, long wlen, char *out, size_t outcap) {
    size_t o = 0;
    long i = 0;
    for (;;) {
        if (wlen >= 0 ? i >= wlen : !w[i]) break;
        uint32_t c = w[i++];
        if (c >= 0xD800 && c < 0xDC00 && (wlen < 0 ? w[i] != 0 : i < wlen) && w[i] >= 0xDC00 && w[i] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (w[i++] - 0xDC00);
        }
        char tmp[4];
        int n;
        if (c < 0x80) { tmp[0] = (char)c; n = 1; }
        else if (c < 0x800) { tmp[0] = (char)(0xC0 | (c >> 6)); tmp[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
        else if (c < 0x10000) { tmp[0] = (char)(0xE0 | (c >> 12)); tmp[1] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
        else { tmp[0] = (char)(0xF0 | (c >> 18)); tmp[1] = (char)(0x80 | ((c >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((c >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
        for (int k = 0; k < n; k++) { if (out && o + 1 < outcap) out[o] = tmp[k]; o++; }
    }
    if (out && outcap) out[o < outcap ? o : outcap - 1] = 0;
    return o;
}

size_t u2w(const char *s, long slen, uint16_t *out, size_t outcap) {
    const unsigned char *u = (const unsigned char *)s;
    size_t o = 0;
    long i = 0;
    for (;;) {
        if (slen >= 0 ? i >= slen : !u[i]) break;
        uint32_t c = u[i];
        int n = 1;
        if (c >= 0xF0 && c < 0xF8) { c &= 0x07; n = 4; }
        else if (c >= 0xE0) { c &= 0x0F; n = 3; }
        else if (c >= 0xC0) { c &= 0x1F; n = 2; }
        else if (c >= 0x80) { c = 0xFFFD; n = 1; }
        int ok = 1;
        for (int k = 1; k < n; k++) {
            unsigned char cc = u[i + k];
            if ((slen >= 0 && i + k >= slen) || (cc & 0xC0) != 0x80) { ok = 0; break; }
            c = (c << 6) | (cc & 0x3F);
        }
        if (!ok) { c = 0xFFFD; n = 1; }
        i += n;
        if (c >= 0x10000) {
            c -= 0x10000;
            if (out && o + 1 < outcap) out[o] = (uint16_t)(0xD800 + (c >> 10));
            o++;
            if (out && o + 1 < outcap) out[o] = (uint16_t)(0xDC00 + (c & 0x3FF));
            o++;
        } else {
            if (out && o + 1 < outcap) out[o] = (uint16_t)c;
            o++;
        }
    }
    if (out && outcap) out[o < outcap ? o : outcap - 1] = 0;
    return o;
}

char *w2u_dup(const uint16_t *w) {
    if (!w) return NULL;
    size_t n = w2u(w, -1, NULL, 0);
    char *s = malloc(n + 1);
    w2u(w, -1, s, n + 1);
    return s;
}
uint16_t *u2w_dup(const char *s) {
    if (!s) return NULL;
    size_t n = u2w(s, -1, NULL, 0);
    uint16_t *w = malloc((n + 1) * 2);
    u2w(s, -1, w, n + 1);
    return w;
}
uint16_t *u2w_gdup(const char *s) {
    if (!s) return NULL;
    size_t n = u2w(s, -1, NULL, 0);
    uint16_t *w = g_malloc((n + 1) * 2);
    u2w(s, -1, w, n + 1);
    return w;
}

// ---- console ---------------------------------------------------------------

static void out_flush_stream(int idx) {
    if (g_hsp && g_hs.outlen[idx]) {
        soul_out(idx + 1, g_hs.outbuf[idx], g_hs.outlen[idx]);
        g_hs.outlen[idx] = 0;
    }
}

void con_flush(void) {
    out_flush_stream(0);
    out_flush_stream(1);
}

void con_write(int stream, const char *data, size_t len) {
    if (!g_hsp) return;
    int idx = (stream == 2) ? 1 : 0;
    // standard output redirected by the parent process (e.g. into a pipe)
    HObj *rd = h_get(g_hs.std_handles[idx + 1]);
    if (rd && rd->type == H_FILE && rd->fd >= 0) {
        while (len) { ssize_t w = write(rd->fd, data, len); if (w <= 0) break; data += w; len -= (size_t)w; }
        return;
    }
    if (idx == 1) out_flush_stream(0); // keep ordering between stdout and stderr
    else out_flush_stream(1);
    while (len) {
        size_t room = sizeof(g_hs.outbuf[idx]) - g_hs.outlen[idx];
        size_t n = len < room ? len : room;
        memcpy(g_hs.outbuf[idx] + g_hs.outlen[idx], data, n);
        g_hs.outlen[idx] += n;
        data += n;
        len -= n;
        if (g_hs.outlen[idx] == sizeof(g_hs.outbuf[idx])) out_flush_stream(idx);
    }
    if (g_hs.outlen[idx] && memchr(g_hs.outbuf[idx], '\n', g_hs.outlen[idx])) out_flush_stream(idx);
}

size_t con_read(char *buf, size_t max, int line_mode) {
    SoulProcess *p = g_proc;
    con_flush();
    SoulThread *self = soul_cur_thread();
    int release = self != NULL;
    if (release) {
        // do not block other guest threads while waiting for the keyboard
        pthread_mutex_lock(&p->in_mu);
        int ready = p->in_len > 0 || p->in_closed || p->terminate;
        pthread_mutex_unlock(&p->in_mu);
        if (ready) release = 0;
    }
    if (release) gil_unlock(self);
    pthread_mutex_lock(&p->in_mu);
    while (p->in_len == 0 && !p->in_closed && !p->terminate) pthread_cond_wait(&p->in_cv, &p->in_mu);
    size_t n = 0;
    if (p->in_len) {
        n = p->in_len < max ? p->in_len : max;
        if (line_mode) {
            char *nl = memchr(p->in_buf, '\n', n);
            if (nl) n = (size_t)(nl - p->in_buf) + 1;
        }
        memcpy(buf, p->in_buf, n);
        memmove(p->in_buf, p->in_buf + n, p->in_len - n);
        p->in_len -= n;
    }
    pthread_mutex_unlock(&p->in_mu);
    if (release) gil_lock(self);
    return n;
}

int con_getc(void) {
    char c;
    return con_read(&c, 1, 0) == 1 ? (unsigned char)c : -1;
}

int con_has_input(void) {
    SoulProcess *p = g_proc;
    pthread_mutex_lock(&p->in_mu);
    int r = p->in_len > 0;
    pthread_mutex_unlock(&p->in_mu);
    return r;
}

// ---- paths -------------------------------------------------------------------

void path_normalize_win(const char *in, char *out, size_t outlen) {
    char tmp[1200];
    size_t i = 0;
    for (; in[i] && i + 1 < sizeof tmp; i++) tmp[i] = in[i] == '/' ? '\\' : in[i];
    tmp[i] = 0;
    const char *s = tmp;
    if (strncmp(s, "\\\\?\\", 4) == 0 || strncmp(s, "\\\\.\\", 4) == 0) s += 4;
    char full[1600];
    const char *cwd = g_proc ? g_proc->cwd_win : "C:\\";
    if (isalpha((unsigned char)s[0]) && s[1] == ':') {
        if (s[2] == '\\') snprintf(full, sizeof full, "%c:%s", toupper((unsigned char)s[0]), s + 2);
        else snprintf(full, sizeof full, "%s\\%s", cwd, s + 2);
    } else if (s[0] == '\\') {
        snprintf(full, sizeof full, "%c:%s", cwd[0], s);
    } else {
        snprintf(full, sizeof full, "%s\\%s", cwd, s);
    }
    // split and normalise
    char drive = full[0];
    const char *comps[128];
    int nc = 0;
    char work[1600];
    snprintf(work, sizeof work, "%s", full + 2);
    char *save = NULL;
    for (char *tok = strtok_r(work, "\\", &save); tok; tok = strtok_r(NULL, "\\", &save)) {
        if (strcmp(tok, ".") == 0 || tok[0] == 0) continue;
        if (strcmp(tok, "..") == 0) { if (nc) nc--; continue; }
        if (nc < 128) comps[nc++] = tok;
    }
    size_t o = (size_t)snprintf(out, outlen, "%c:", drive);
    if (nc == 0) { snprintf(out + o, outlen - o, "\\"); return; }
    for (int k = 0; k < nc && o < outlen; k++) o += (size_t)snprintf(out + o, outlen - o, "\\%s", comps[k]);
}

int path_w2h(const char *win, char *host, size_t hostlen) {
    SoulProcess *p = g_proc;
    char norm[1200];
    if (strcasecmp(win, "NUL") == 0) { snprintf(host, hostlen, "/dev/null"); return 0; }
    path_normalize_win(win, norm, sizeof norm);
    char cur[2048];
    snprintf(cur, sizeof cur, "%s", p->drive_c);
    const char *s = norm + 2; // skip "C:"
    int exists = 1;
    while (*s) {
        while (*s == '\\') s++;
        if (!*s) break;
        const char *e = strchr(s, '\\');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        char comp[512];
        if (l >= sizeof comp) l = sizeof comp - 1;
        memcpy(comp, s, l);
        comp[l] = 0;
        // Windows ignores trailing dots/spaces
        while (l > 0 && (comp[l - 1] == ' ' || comp[l - 1] == '.') && strcmp(comp, "..") != 0) comp[--l] = 0;
        char cand[2600];
        snprintf(cand, sizeof cand, "%s/%s", cur, comp);
        struct stat sb;
        if (exists && lstat(cand, &sb) != 0) {
            DIR *d = opendir(cur);
            int found = 0;
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                    if (strcasecmp(de->d_name, comp) == 0) {
                        snprintf(cand, sizeof cand, "%s/%s", cur, de->d_name);
                        found = 1;
                        break;
                    }
                }
                closedir(d);
            }
            if (!found) exists = 0;
        }
        snprintf(cur, sizeof cur, "%s", cand);
        s += l;
        while (*s && *s != '\\') s++;
    }
    snprintf(host, hostlen, "%s", cur);
    return 0;
}

int win_to_host_path(SoulProcess *p, const char *win, char *out, size_t outlen) {
    (void)p;
    return path_w2h(win, out, outlen);
}

uint64_t filetime_from_unix(double t) { return (uint64_t)((t + 11644473600.0) * 10000000.0); }

void fill_find_data(void *dst, int wide, const char *name, const char *host_path) {
    uint8_t *d = dst;
    memset(d, 0, wide ? 592 : 320);
    struct stat sb;
    uint32_t attr = 0x80;
    uint64_t ft = 0, size = 0;
    if (stat(host_path, &sb) == 0) {
        attr = S_ISDIR(sb.st_mode) ? 0x10 : 0x20;
        if (!(sb.st_mode & S_IWUSR)) attr |= 0x1;
        size = S_ISDIR(sb.st_mode) ? 0 : (uint64_t)sb.st_size;
        ft = filetime_from_unix((double)sb.st_mtime);
    }
    if (name[0] == '.' && strcmp(name, ".") && strcmp(name, "..")) attr |= 0x2; // hidden
    memcpy(d, &attr, 4);
    memcpy(d + 4, &ft, 8);
    memcpy(d + 12, &ft, 8);
    memcpy(d + 20, &ft, 8);
    uint32_t hi = (uint32_t)(size >> 32), lo = (uint32_t)size;
    memcpy(d + 28, &hi, 4);
    memcpy(d + 32, &lo, 4);
    if (wide) u2w(name, -1, (uint16_t *)(d + 44), 260);
    else snprintf((char *)(d + 44), 260, "%s", name);
}

int wild_match(const char *pat, const char *s) {
    if (strcmp(pat, "*.*") == 0 || strcmp(pat, "*") == 0) return 1;
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (; *s; s++) if (wild_match(pat, s)) return 1;
            return wild_match(pat, s);
        }
        if (!*s) return 0;
        if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s)) return 0;
        pat++;
        s++;
    }
    return *s == 0;
}

// ---- virtual memory regions -------------------------------------------------

typedef struct VmRegion { uint64_t base, size; struct VmRegion *next; } VmRegion;
static VmRegion *g_regions;
SOUL_PROCESS_VAR(g_regions)

// Regions are kept as guest addresses; vm_alloc returns a host pointer.
void *vm_alloc(uint64_t addr, size_t size, int *existed) {
    size = (size + 0xFFFF) & ~(size_t)0xFFFF;
    if (existed) *existed = 0;
    if (addr) {
        for (VmRegion *r = g_regions; r; r = r->next) {
            if (addr >= r->base && addr < r->base + r->size) {
                if (existed) *existed = 1;
                return G2H(addr); // commit inside reservation
            }
        }
    }
    uint64_t base;
    if (g_mode32) {
        base = gwin_map(addr & ~0xFFFFull, size);
        if (!base || (addr && base != (addr & ~0xFFFFull))) { if (base) gwin_unmap(base, size); return NULL; }
    } else {
        void *want = (void *)(uintptr_t)(addr & ~0xFFFFull);
        void *p = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) return NULL;
        if (addr && p != want) {
            munmap(p, size);
            return NULL;
        }
        base = (uint64_t)(uintptr_t)p;
    }
    VmRegion *r = malloc(sizeof *r);
    r->base = base;
    r->size = size;
    r->next = g_regions;
    g_regions = r;
    return G2H(base);
}

int vm_free(uint64_t addr) {
    for (VmRegion **pr = &g_regions; *pr; pr = &(*pr)->next) {
        VmRegion *r = *pr;
        if (r->base == addr) {
            g_pages_free(G2H(r->base), r->size);
            *pr = r->next;
            free(r);
            return 1;
        }
    }
    return 0;
}

int vm_query(uint64_t addr, uint64_t *base, uint64_t *alloc_base, uint64_t *size, int *type) {
    for (VmRegion *r = g_regions; r; r = r->next) {
        if (addr >= r->base && addr < r->base + r->size) {
            *base = addr & ~0xFFFull;
            *alloc_base = r->base;
            *size = r->base + r->size - *base;
            *type = 0x20000; // MEM_PRIVATE
            return 1;
        }
    }
    for (PEModule *m = g_proc->modules; m; m = m->next) {
        if (addr >= m->base && addr < m->base + m->size) {
            *base = addr & ~0xFFFull;
            *alloc_base = m->base;
            *size = m->base + m->size - *base;
            *type = 0x1000000; // MEM_IMAGE
            return 1;
        }
    }
    SoulThread *t = &g_proc->main_thread;
    if (addr >= t->stack_limit && addr < t->stack_base) {
        *base = addr & ~0xFFFull;
        *alloc_base = t->stack_limit;
        *size = t->stack_base - *base;
        *type = 0x20000;
        return 1;
    }
    return 0;
}

static void vm_free_all(void) {
    while (g_regions) {
        VmRegion *n = g_regions->next;
        g_pages_free(G2H(g_regions->base), g_regions->size);
        free(g_regions);
        g_regions = n;
    }
}

// ---- init ----------------------------------------------------------------------

static void env_add(const char *kv) {
    g_hs.env = realloc(g_hs.env, sizeof(char *) * (size_t)(g_hs.env_count + 2));
    g_hs.env[g_hs.env_count++] = g_strdup(kv);
    g_hs.env[g_hs.env_count] = NULL;
}

void hle_init(SoulProcess *p, char **env) {
    g_hsp = calloc(1, sizeof(HleState));
    p->hle_state = g_hsp;
    HObj *o;
    static const HType con[3] = { H_CONIN, H_CONOUT, H_CONERR };
    for (int i = 0; i < 3; i++) {
        if (p->std_fd[i] >= 0) { // inherited from the parent (STARTF_USESTDHANDLES)
            g_hs.std_handles[i] = h_alloc(H_FILE, &o);
            o->fd = p->std_fd[i];
            p->std_fd[i] = -1;
        } else g_hs.std_handles[i] = h_alloc(con[i], &o);
    }
    g_hs.process_heap = g_mode32 ? 0x00ABCD00ull : 0x00000000ABCD0000ull;
    g_hs.con_attr = 7;
    g_hs.tls_next = 0;
    snprintf(g_hs.con_title, sizeof g_hs.con_title, "%s", p->exe_win);

    if (env) { // the parent's environment (or the one CreateProcess was given)
        for (int i = 0; env[i]; i++) env_add(env[i]);
        return;
    }
    env_add("ALLUSERSPROFILE=C:\\ProgramData");
    env_add("APPDATA=C:\\users\\soul\\AppData\\Roaming");
    env_add("COMPUTERNAME=IPAD");
    env_add("ComSpec=C:\\windows\\system32\\cmd.exe");
    env_add("HOMEDRIVE=C:");
    env_add("HOMEPATH=\\users\\soul");
    env_add("LOCALAPPDATA=C:\\users\\soul\\AppData\\Local");
    env_add("NUMBER_OF_PROCESSORS=1");
    env_add("OS=Windows_NT");
    env_add("PATH=C:\\windows\\system32;C:\\windows");
    env_add("PATHEXT=.COM;.EXE;.BAT;.CMD");
    if (g_mode32) { // a 32-bit program on 64-bit Windows (WOW64)
        env_add("PROCESSOR_ARCHITECTURE=x86");
        env_add("PROCESSOR_ARCHITEW6432=AMD64");
    } else {
        env_add("PROCESSOR_ARCHITECTURE=AMD64");
    }
    env_add("ProgramData=C:\\ProgramData");
    env_add(g_mode32 ? "ProgramFiles=C:\\Program Files (x86)" : "ProgramFiles=C:\\Program Files");
    env_add("ProgramW6432=C:\\Program Files");
    env_add("ProgramFiles(x86)=C:\\Program Files (x86)");
    env_add("PUBLIC=C:\\users\\Public");
    env_add("SystemDrive=C:");
    env_add("SystemRoot=C:\\windows");
    env_add("TEMP=C:\\users\\soul\\Temp");
    env_add("TMP=C:\\users\\soul\\Temp");
    env_add("USERNAME=soul");
    env_add("USERPROFILE=C:\\users\\soul");
    env_add("windir=C:\\windows");
}

void hle_init_system(SoulProcess *p) {
    // ensure the standard directory layout exists inside the container
    const char *dirs[] = { "windows", "windows/system32", "users", "users/soul", "users/soul/Temp",
                           "users/soul/AppData", "users/soul/AppData/Roaming", "users/soul/AppData/Local",
                           "users/soul/Documents", "users/soul/Desktop", "ProgramData", "Program Files", "Program Files (x86)", NULL };
    for (int i = 0; dirs[i]; i++) {
        char d[1200];
        snprintf(d, sizeof d, "%s/%s", p->drive_c, dirs[i]);
        mkdir(d, 0755);
    }
    gui_reset();
    comctl_reset();
    gl_reset();
    reg_init(p);
}


void hle_shutdown(SoulProcess *p) {
    if (!g_hsp) return;
    con_flush();
    for (int i = 0; i < MAX_HANDLES; i++) {
        HObj *o = &g_hs.handles[i];
        if (o->type == H_FREE) continue;
        if (o->fd >= 0) close(o->fd);
        if (o->dir) closedir(o->dir);
        if (o->type == H_MAPPING && o->ptr) g_free(o->ptr);
    }
    k32_shutdown();
    for (int i = 0; i < g_hs.env_count; i++) g_free(g_hs.env[i]);
    free(g_hs.env);
    vm_free_all();
    gdi_process_exit(p->pid);
    audio_process_exit(p->pid);
    if (p->hle_state == g_hsp) p->hle_state = NULL;
    p->retired_api += g_hs.api_calls;
    free(g_hsp);
    g_hsp = NULL;
}

void hle_shutdown_system(void) {
    gui_shutdown();
    gl_reset();
    reg_shutdown();
}
