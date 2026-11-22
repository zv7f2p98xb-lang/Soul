// SoulCore - C runtime emulation (msvcrt.dll, ucrtbase.dll / api-ms-win-crt-*, vcruntime140.dll).
#include "hle.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#define HOST_MSIZE(p) malloc_size(p)
#else
#include <malloc.h>
#define HOST_MSIZE(p) malloc_usable_size(p)
#endif

// Floating point arguments: XMM0-3 in 64-bit programs; on the stack in 32-bit ones, where a
// double takes two slots (FARG(n) assumes all earlier arguments are doubles too) and a float one.
static inline double farg32(X86State *st, int slot) { uint64_t v = mem_rd((uint32_t)(st->gpr[RSP] + 4 + 4 * (uint32_t)slot), 8); double d; memcpy(&d, &v, 8); return d; }
static inline float fargf32(X86State *st, int slot) { uint32_t v = (uint32_t)hle_arg(st, slot); float f; memcpy(&f, &v, 4); return f; }
#define FARG(n) (g_mode32 ? farg32(st, 2 * (n)) : st->xmm[(n)].fd[0])
#define FARGF(n) (g_mode32 ? fargf32(st, (n)) : st->xmm[(n)].f[0])
// Floating point results: XMM0 in 64-bit programs, x87 ST(0) in 32-bit ones.
void x87_push_value(X86State *st, double v);
#define RETD(v) do { double _v = (v); if (g_mode32) x87_push_value(st, _v); else { memset(&st->xmm[0], 0, 16); st->xmm[0].fd[0] = _v; } } while (0)
#define RETF(v) do { float _v = (v); if (g_mode32) x87_push_value(st, (double)_v); else { memset(&st->xmm[0], 0, 16); st->xmm[0].f[0] = _v; } } while (0)
#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))
// Arguments after a 64-bit integer / double argument: index n64 in 64-bit programs, stack slot s32 in 32-bit ones.
#define ARGS(n64, s32) (g_mode32 ? hle_arg(st, (s32)) : hle_arg(st, (n64)))
#define ARG64(n64, s32) (g_mode32 ? (hle_arg(st, (s32)) | (hle_arg(st, (s32) + 1) << 32)) : hle_arg(st, (n64)))

// ---- global CRT data visible to the guest ------------------------------------------

#define MAX_FILES 256
enum { CF_HOST = 0, CF_STDIN, CF_STDOUT, CF_STDERR };
typedef struct { int used; int kind; FILE *hf; int text; int eof, err; int ungot; } CrtFile;
typedef struct { int32_t v[9]; } GuestTm;

// Everything the guest can address (FILE array, errno, argv cells, ctype table, static result
// buffers) lives in one block of guest memory: inside the 4 GB window for 32-bit programs.
// Pointer cells are 8 bytes; 32-bit programs read and write their low 4 bytes.
typedef struct CrtGuest {
    uint8_t iob[MAX_FILES][48];   // FILE: 48 bytes in 64-bit programs, 32 in 32-bit ones
    int32_t fmode, commode, errno_v, doserrno, argc, mb_cur_max, timezone_v, daylight_v;
    uint64_t argv, wargv, environ_v, wenviron, acmdln, wcmdln, pgmptr, wpgmptr, pctype;
    uint16_t ctype_table[257];
    uint8_t lconv[160];
    uint64_t current_exception[2];
    GuestTm tm;
    char asctime_buf[64];
    char strerror_buf[128];
    char locale_c[4];
    uint16_t wlocale_c[4];
    char empty[2], dot[2];
    uint16_t wempty[2], wdot[2];
    uint64_t lc_names[6], locale_obj[4];
} CrtGuest;
static CrtGuest *g_cg;
#define g_iob (g_cg->iob)
#define g_fmode (g_cg->fmode)
#define g_commode (g_cg->commode)
#define g_errno (g_cg->errno_v)
#define g_doserrno (g_cg->doserrno)
#define g_argc (g_cg->argc)
#define g_mb_cur_max (g_cg->mb_cur_max)
#define g_ctype_table (g_cg->ctype_table)
#define g_lconv (g_cg->lconv)
#define g_current_exception (g_cg->current_exception)
#define g_tm (g_cg->tm)
#define g_asctime_buf (g_cg->asctime_buf)
#define g_timezone (g_cg->timezone_v)
#define g_daylight (g_cg->daylight_v)
#define IOB_STRIDE (g_mode32 ? 32u : 48u)
static int g_args_built;

static CrtFile g_files[MAX_FILES];
static uint32_t g_rand_state = 1;
static uint64_t *g_atexit;
static int g_atexit_n, g_atexit_cap;
static char g_strtok_save_dummy;
static char *g_strtok_save;
static uint16_t *g_wcstok_save;
static double g_clock_start;
SOUL_PROCESS_VAR(g_cg)
SOUL_PROCESS_VAR(g_args_built)
SOUL_PROCESS_VAR(g_files)
SOUL_PROCESS_VAR(g_rand_state)
SOUL_PROCESS_VAR(g_atexit)
SOUL_PROCESS_VAR(g_atexit_n)
SOUL_PROCESS_VAR(g_atexit_cap)
SOUL_PROCESS_VAR(g_strtok_save)
SOUL_PROCESS_VAR(g_wcstok_save)
SOUL_PROCESS_VAR(g_clock_start)

void crt_reset(void) {
    for (int i = 3; i < MAX_FILES; i++) if (g_files[i].used && g_files[i].hf) fclose(g_files[i].hf);
    memset(g_files, 0, sizeof g_files);
    g_cg = g_calloc(1, sizeof(CrtGuest));
    g_args_built = 0;
    g_mb_cur_max = 1;
    // FILE._file: offset 28 in the 64-bit layout, 16 in the 32-bit one
    for (int i = 0; i < MAX_FILES; i++) { int32_t idx = i; memcpy((uint8_t *)g_iob + (size_t)i * IOB_STRIDE + (g_mode32 ? 16 : 28), &idx, 4); }
    g_files[0] = (CrtFile){ 1, CF_STDIN, NULL, 1, 0, 0, -1 };
    g_files[1] = (CrtFile){ 1, CF_STDOUT, NULL, 1, 0, 0, -1 };
    g_files[2] = (CrtFile){ 1, CF_STDERR, NULL, 1, 0, 0, -1 };
    g_fmode = 0; g_commode = 0; g_errno = 0; g_doserrno = 0;
    g_rand_state = 1;
    free(g_atexit); g_atexit = NULL; g_atexit_n = g_atexit_cap = 0;
    g_clock_start = soul_now();
    // ctype table (C locale)
    for (int c = 0; c < 256; c++) {
        uint16_t f = 0;
        if (c < 128) {
            if (isupper(c)) f |= 0x1 | 0x100;
            if (islower(c)) f |= 0x2 | 0x100;
            if (isdigit(c)) f |= 0x4;
            if (isspace(c)) f |= 0x8;
            if (ispunct(c)) f |= 0x10;
            if (iscntrl(c)) f |= 0x20;
            if (c == ' ' || c == '\t') f |= 0x40;
            if (isxdigit(c)) f |= 0x80;
        }
        g_ctype_table[c + 1] = f;
    }
    g_cg->pctype = H2G(&g_ctype_table[1]);
    // lconv: 10 char* + 8 chars + 6 wchar_t*
    g_cg->dot[0] = '.'; g_cg->wdot[0] = '.';
    memcpy(g_cg->locale_c, "C", 2);
    g_cg->wlocale_c[0] = 'C';
    uint64_t lc = H2G(g_lconv);
    for (int i = 0; i < 10; i++) wr_ptr(lc + (uint64_t)i * PSZ, H2G(i ? g_cg->empty : g_cg->dot));
    memset(g_lconv + 10 * PSZ, 127, 8);
    uint64_t wl = lc + 10 * (uint64_t)PSZ + 8;
    for (int i = 0; i < 6; i++) wr_ptr(wl + (uint64_t)i * PSZ, H2G(i ? g_cg->wempty : g_cg->wdot));
}

static uint64_t gstr(const char *s) { return H2G(g_strdup(s)); }
static uint64_t gwstr(const char *s) { return H2G(u2w_gdup(s)); }

static void build_args(void) {
    SoulProcess *p = g_proc;
    // split command line using Windows rules
    if (g_args_built) return;
    g_args_built = 1;
    const char *s = p->cmdline;
    uint64_t argv = H2G(g_calloc(256, (size_t)PSZ)), wargv = H2G(g_calloc(256, (size_t)PSZ));
    int argc = 0;
    while (*s && argc < 255) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        char buf[2048];
        size_t n = 0;
        int inq = 0;
        while (*s && (inq || (*s != ' ' && *s != '\t'))) {
            if (*s == '\\') {
                int bs = 0;
                while (*s == '\\') { bs++; s++; }
                if (*s == '"') {
                    for (int k = 0; k < bs / 2 && n < sizeof buf - 1; k++) buf[n++] = '\\';
                    if (bs % 2) { if (n < sizeof buf - 1) buf[n++] = '"'; s++; }
                } else {
                    for (int k = 0; k < bs && n < sizeof buf - 1; k++) buf[n++] = '\\';
                }
                continue;
            }
            if (*s == '"') { inq = !inq; s++; continue; }
            if (n < sizeof buf - 1) buf[n++] = *s;
            s++;
        }
        buf[n] = 0;
        wr_ptr(argv + (uint64_t)argc * PSZ, gstr(buf));
        wr_ptr(wargv + (uint64_t)argc * PSZ, gwstr(buf));
        argc++;
    }
    g_argc = argc;
    g_cg->argv = argv;
    g_cg->wargv = wargv;
    HleState *h = hs();
    uint64_t env = H2G(g_calloc((size_t)h->env_count + 1, (size_t)PSZ));
    uint64_t wenv = H2G(g_calloc((size_t)h->env_count + 1, (size_t)PSZ));
    for (int i = 0; i < h->env_count; i++) { wr_ptr(env + (uint64_t)i * PSZ, H2G(h->env[i])); wr_ptr(wenv + (uint64_t)i * PSZ, gwstr(h->env[i])); }
    g_cg->environ_v = env;
    g_cg->wenviron = wenv;
    g_cg->acmdln = gstr(p->cmdline);
    g_cg->wcmdln = gwstr(p->cmdline);
    g_cg->pgmptr = gstr(p->exe_win);
    g_cg->wpgmptr = gwstr(p->exe_win);
}

static void free_args(void) {
    if (!g_cg) return;
    if (g_args_built) {
        for (int i = 0; i < g_argc; i++) { g_free(G2H(rd_ptr(g_cg->argv + (uint64_t)i * PSZ))); g_free(G2H(rd_ptr(g_cg->wargv + (uint64_t)i * PSZ))); }
        for (uint64_t a = g_cg->wenviron; a && rd_ptr(a); a += (uint64_t)PSZ) g_free(G2H(rd_ptr(a)));
        g_free(G2H(g_cg->argv)); g_free(G2H(g_cg->wargv)); g_free(G2H(g_cg->environ_v)); g_free(G2H(g_cg->wenviron));
        g_free(G2H(g_cg->acmdln)); g_free(G2H(g_cg->wcmdln)); g_free(G2H(g_cg->pgmptr)); g_free(G2H(g_cg->wpgmptr));
    }
    g_args_built = 0;
    g_free(g_cg);
    g_cg = NULL;
}

// ---- FILE helpers ------------------------------------------------------------------------

static CrtFile *cf_get(uint64_t fp) {
    uint64_t base = H2G(g_iob);
    if (fp < base || fp >= base + (uint64_t)MAX_FILES * IOB_STRIDE) return NULL;
    uint64_t i = (fp - base) / IOB_STRIDE;
    return g_files[i].used ? &g_files[i] : NULL;
}
static uint64_t cf_ptr(int i) { return H2G(g_iob) + (uint64_t)i * IOB_STRIDE; }

static void cf_write(CrtFile *f, const char *d, size_t n) {
    if (!f || !n) return;
    if (f->kind == CF_STDOUT) { con_write(1, d, n); return; }
    if (f->kind == CF_STDERR) { con_write(2, d, n); return; }
    if (f->kind != CF_HOST || !f->hf) { f->err = 1; return; }
    if (!f->text) { if (fwrite(d, 1, n, f->hf) != n) f->err = 1; return; }
    size_t s = 0;
    for (size_t i = 0; i < n; i++) {
        if (d[i] == '\n') {
            if (i > s) fwrite(d + s, 1, i - s, f->hf);
            fputs("\r\n", f->hf);
            s = i + 1;
        }
    }
    if (n > s) fwrite(d + s, 1, n - s, f->hf);
}

static int cf_getc(CrtFile *f) {
    if (!f) return -1;
    if (f->ungot >= 0) { int c = f->ungot; f->ungot = -1; return c; }
    if (f->eof) return -1;
    int c;
    if (f->kind == CF_STDIN) {
        c = con_getc();
    } else if (f->kind == CF_HOST && f->hf) {
        c = fgetc(f->hf);
        if (f->text && c == '\r') {
            int c2 = fgetc(f->hf);
            if (c2 == '\n') c = '\n';
            else if (c2 != EOF) ungetc(c2, f->hf);
        }
        if (f->text && c == 0x1A) c = EOF;
    } else {
        c = -1;
    }
    if (c < 0) { f->eof = 1; return -1; }
    return c;
}
static void cf_ungetc(CrtFile *f, int c) { if (f && c >= 0) { f->ungot = c; f->eof = 0; } }

static size_t cf_read(CrtFile *f, char *buf, size_t n) {
    if (!f) return 0;
    if (f->kind == CF_HOST && f->hf && !f->text && f->ungot < 0) {
        size_t r = fread(buf, 1, n, f->hf);
        if (r < n) f->eof = 1;
        return r;
    }
    size_t i = 0;
    for (; i < n; i++) {
        int c = cf_getc(f);
        if (c < 0) break;
        buf[i] = (char)c;
    }
    return i;
}

static int in_file_getc(InSrc *in) { return cf_getc((CrtFile *)in->ctx); }
static void in_file_ungetc(InSrc *in, int c) { cf_ungetc((CrtFile *)in->ctx, c); }
static int in_str_getc(InSrc *in) {
    unsigned char c = (unsigned char)in->str[in->pos];
    if (!c) return -1;
    in->pos++;
    return c;
}
static void in_str_ungetc(InSrc *in, int c) { (void)c; if (in->pos) in->pos--; }

static int open_mode_flags(const char *mode, int *text) {
    int fl = 0;
    int plus = strchr(mode, '+') != NULL;
    *text = strchr(mode, 'b') == NULL;
    if (strchr(mode, 't')) *text = 1;
    switch (mode[0]) {
    case 'r': fl = plus ? O_RDWR : O_RDONLY; break;
    case 'w': fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': fl = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default: return -1;
    }
    return fl;
}

static uint64_t crt_open_file(const char *path, const char *mode) {
    int text;
    if (open_mode_flags(mode, &text) < 0) { g_errno = EINVAL; return 0; }
    char host[2048];
    path_w2h(path, host, sizeof host);
    char hmode[8];
    size_t k = 0;
    for (const char *m = mode; *m && k < 6; m++) if (strchr("rwa+", *m)) hmode[k++] = *m;
    hmode[k++] = 'b';
    hmode[k] = 0;
    FILE *hf = fopen(host, hmode);
    if (!hf) { g_errno = errno; return 0; }
    for (int i = 3; i < MAX_FILES; i++) {
        if (!g_files[i].used) {
            g_files[i] = (CrtFile){ 1, CF_HOST, hf, text, 0, 0, -1 };
            return cf_ptr(i);
        }
    }
    fclose(hf);
    g_errno = EMFILE;
    return 0;
}

// ---- formatted output ----------------------------------------------------------------------

static char *fmt_from(X86State *st, int idx, int wide) {
    (void)st;
    uint64_t p = hle_arg(st, idx);
    if (!p) return strdup("");
    if (wide) return w2u_dup((const uint16_t *)G2H(p));
    return strdup((const char *)G2H(p));
}

static int64_t write_formatted(CrtFile *f, const char *fmt, ArgSrc *a, int wide) {
    size_t n;
    char *s = fmt_vformat(fmt, a, wide, &n);
    cf_write(f, s, n);
    int64_t units = wide ? (int64_t)u2w(s, (long)n, NULL, 0) : (int64_t)n;
    free(s);
    return units;
}

// mode: 0 = unlimited (sprintf), 1 = C99 snprintf, 2 = MS _snprintf (-1 on truncation, no NUL), 3 = _s (fail on truncation)
static int64_t print_to_buffer(uint64_t buf, uint64_t count, const char *fmt, ArgSrc *a, int wide_fmt, int wide_out, int mode) {
    size_t n;
    char *s = fmt_vformat(fmt, a, wide_fmt, &n);
    size_t units = wide_out ? u2w(s, (long)n, NULL, 0) : n;
    int64_t ret = (int64_t)units;
    if (mode == 0) count = units + 1;
    if (buf && count) {
        size_t usz = wide_out ? 2 : 1;
        if (units < count) {
            if (wide_out) u2w(s, (long)n, (uint16_t *)G2H(buf), units + 1);
            else memcpy(G2H(buf), s, n + 1);
        } else if (mode == 1) {
            if (wide_out) u2w(s, (long)n, (uint16_t *)G2H(buf), count);
            else { memcpy(G2H(buf), s, count - 1); ((char *)G2H(buf))[count - 1] = 0; }
        } else if (mode == 2) {
            if (wide_out) {
                uint16_t *tmp = malloc((units + 1) * 2);
                u2w(s, (long)n, tmp, units + 1);
                memcpy(G2H(buf), tmp, count * 2);
                free(tmp);
            } else memcpy(G2H(buf), s, count);
            ret = (units == count) ? (int64_t)units : -1;
        } else {
            memset(G2H(buf), 0, usz);
            ret = -1;
        }
    }
    free(s);
    return ret;
}

HLE(crt_printf) { char *f = fmt_from(st, 0, 0); ArgSrc a = { st, 1, 0 }; RET(write_formatted(&g_files[1], f, &a, 0)); free(f); }
HLE(crt_wprintf) { char *f = fmt_from(st, 0, 1); ArgSrc a = { st, 1, 0 }; RET(write_formatted(&g_files[1], f, &a, 1)); free(f); }
HLE(crt_vprintf) { char *f = fmt_from(st, 0, 0); ArgSrc a = { st, 0, ARG(1) }; RET(write_formatted(&g_files[1], f, &a, 0)); free(f); }
HLE(crt_vwprintf) { char *f = fmt_from(st, 0, 1); ArgSrc a = { st, 0, ARG(1) }; RET(write_formatted(&g_files[1], f, &a, 1)); free(f); }
HLE(crt_fprintf) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 2, 0 }; RET(write_formatted(cf_get(ARG(0)), f, &a, 0)); free(f); }
HLE(crt_fwprintf) { char *f = fmt_from(st, 1, 1); ArgSrc a = { st, 2, 0 }; RET(write_formatted(cf_get(ARG(0)), f, &a, 1)); free(f); }
HLE(crt_vfprintf) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 0, ARG(2) }; RET(write_formatted(cf_get(ARG(0)), f, &a, 0)); free(f); }
HLE(crt_vfwprintf) { char *f = fmt_from(st, 1, 1); ArgSrc a = { st, 0, ARG(2) }; RET(write_formatted(cf_get(ARG(0)), f, &a, 1)); free(f); }
HLE(crt_sprintf) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 2, 0 }; RET(print_to_buffer(ARG(0), 0, f, &a, 0, 0, 0)); free(f); }
HLE(crt_vsprintf) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 0, ARG(2) }; RET(print_to_buffer(ARG(0), 0, f, &a, 0, 0, 0)); free(f); }
HLE(crt_snprintf) { char *f = fmt_from(st, 2, 0); ArgSrc a = { st, 3, 0 }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 0, 0, 1)); free(f); }
HLE(crt_vsnprintf) { char *f = fmt_from(st, 2, 0); ArgSrc a = { st, 0, ARG(3) }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 0, 0, 1)); free(f); }
HLE(crt__snprintf) { char *f = fmt_from(st, 2, 0); ArgSrc a = { st, 3, 0 }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 0, 0, 2)); free(f); }
HLE(crt__vsnprintf) { char *f = fmt_from(st, 2, 0); ArgSrc a = { st, 0, ARG(3) }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 0, 0, 2)); free(f); }
HLE(crt_sprintf_s) { char *f = fmt_from(st, 2, 0); ArgSrc a = { st, 3, 0 }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 0, 0, 3)); free(f); }
HLE(crt_vsprintf_s) { char *f = fmt_from(st, 2, 0); ArgSrc a = { st, 0, ARG(3) }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 0, 0, 3)); free(f); }
HLE(crt__snprintf_s) { char *f = fmt_from(st, 3, 0); ArgSrc a = { st, 4, 0 }; uint64_t c = ARG(2) < ARG(1) ? ARG(2) + 1 : ARG(1); RET(print_to_buffer(ARG(0), c, f, &a, 0, 0, 1)); free(f); }
HLE(crt__vsnprintf_s) { char *f = fmt_from(st, 3, 0); ArgSrc a = { st, 0, ARG(4) }; uint64_t c = ARG(2) < ARG(1) ? ARG(2) + 1 : ARG(1); RET(print_to_buffer(ARG(0), c, f, &a, 0, 0, 1)); free(f); }
HLE(crt_swprintf_legacy) { char *f = fmt_from(st, 1, 1); ArgSrc a = { st, 2, 0 }; RET(print_to_buffer(ARG(0), 0, f, &a, 1, 1, 0)); free(f); }
HLE(crt_vswprintf_legacy) { char *f = fmt_from(st, 1, 1); ArgSrc a = { st, 0, ARG(2) }; RET(print_to_buffer(ARG(0), 0, f, &a, 1, 1, 0)); free(f); }
HLE(crt__snwprintf) { char *f = fmt_from(st, 2, 1); ArgSrc a = { st, 3, 0 }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 1, 1, 2)); free(f); }
HLE(crt__vsnwprintf) { char *f = fmt_from(st, 2, 1); ArgSrc a = { st, 0, ARG(3) }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 1, 1, 2)); free(f); }
HLE(crt_swprintf_s) { char *f = fmt_from(st, 2, 1); ArgSrc a = { st, 3, 0 }; RET(print_to_buffer(ARG(0), ARG(1), f, &a, 1, 1, 3)); free(f); }
HLE(crt__scprintf) { char *f = fmt_from(st, 0, 0); ArgSrc a = { st, 1, 0 }; RET(print_to_buffer(0, 0, f, &a, 0, 0, 1)); free(f); }
HLE(crt__vscprintf) { char *f = fmt_from(st, 0, 0); ArgSrc a = { st, 0, ARG(1) }; RET(print_to_buffer(0, 0, f, &a, 0, 0, 1)); free(f); }
HLE(crt__vscwprintf) { char *f = fmt_from(st, 0, 1); ArgSrc a = { st, 0, ARG(1) }; RET(print_to_buffer(0, 0, f, &a, 1, 1, 1)); free(f); }

// UCRT entry points: options (bit0 legacy null termination, bit1 standard snprintf behaviour).
// The options argument is 64-bit, so in 32-bit programs the later arguments start one slot later.
#define UI(n) ((n) + (g_mode32 ? 1 : 0))
#define UA(n) hle_arg(st, UI(n))
HLE(ucrt_vfprintf) { char *f = fmt_from(st, UI(2), 0); ArgSrc a = { st, 0, UA(4) }; RET(write_formatted(cf_get(UA(1)), f, &a, 0)); free(f); }
HLE(ucrt_vfwprintf) { char *f = fmt_from(st, UI(2), 1); ArgSrc a = { st, 0, UA(4) }; RET(write_formatted(cf_get(UA(1)), f, &a, 1)); free(f); }
static int64_t ucrt_vsprintf_impl(X86State *st, int wide) {
    uint64_t opt = ARG(0), buf = UA(1), count = UA(2);
    char *f = fmt_from(st, UI(3), wide);
    ArgSrc a = { st, 0, UA(5) };
    int mode = (opt & 2) ? 1 : 2;
    int64_t r;
    if (!buf || !count) r = print_to_buffer(0, 0, f, &a, wide, wide, 1);
    else {
        r = print_to_buffer(buf, count, f, &a, wide, wide, mode);
        if (r < 0 && !(opt & 1)) { // UCRT NUL-terminates truncated output unless legacy flag
            if (wide) ((uint16_t *)G2H(buf))[count - 1] = 0;
            else ((char *)G2H(buf))[count - 1] = 0;
        }
    }
    free(f);
    return r;
}
HLE(ucrt_vsprintf) { RET(ucrt_vsprintf_impl(st, 0)); }
HLE(ucrt_vswprintf) { RET(ucrt_vsprintf_impl(st, 1)); }
HLE(ucrt_vsprintf_s) { char *f = fmt_from(st, UI(3), 0); ArgSrc a = { st, 0, UA(5) }; RET(print_to_buffer(UA(1), UA(2), f, &a, 0, 0, 3)); free(f); }
HLE(ucrt_vswprintf_s) { char *f = fmt_from(st, UI(3), 1); ArgSrc a = { st, 0, UA(5) }; RET(print_to_buffer(UA(1), UA(2), f, &a, 1, 1, 3)); free(f); }
HLE(ucrt_vsnprintf_s) {
    char *f = fmt_from(st, UI(4), 0);
    ArgSrc a = { st, 0, UA(6) };
    uint64_t c = UA(3) < UA(2) ? UA(3) + 1 : UA(2);
    RET(print_to_buffer(UA(1), c, f, &a, 0, 0, 1));
    free(f);
}
HLE(ucrt_vsnwprintf_s) {
    char *f = fmt_from(st, UI(4), 1);
    ArgSrc a = { st, 0, UA(6) };
    uint64_t c = UA(3) < UA(2) ? UA(3) + 1 : UA(2);
    RET(print_to_buffer(UA(1), c, f, &a, 1, 1, 1));
    free(f);
}

// ---- formatted input ---------------------------------------------------------------

static int scan_file(CrtFile *f, const char *fmt, ArgSrc *a, int wide, int secure) {
    InSrc in = { in_file_getc, in_file_ungetc, NULL, 0, f, 0, secure };
    return fmt_scan(&in, fmt, a, wide);
}
static int scan_str(const char *s, const char *fmt, ArgSrc *a, int wide, int secure) {
    InSrc in = { in_str_getc, in_str_ungetc, s, 0, NULL, 0, secure };
    return fmt_scan(&in, fmt, a, wide);
}
HLE(crt_scanf) { char *f = fmt_from(st, 0, 0); ArgSrc a = { st, 1, 0 }; RET((int64_t)scan_file(&g_files[0], f, &a, 0, 0)); free(f); }
HLE(crt_scanf_s) { char *f = fmt_from(st, 0, 0); ArgSrc a = { st, 1, 0 }; RET((int64_t)scan_file(&g_files[0], f, &a, 0, 1)); free(f); }
HLE(crt_fscanf) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 2, 0 }; RET((int64_t)scan_file(cf_get(ARG(0)), f, &a, 0, 0)); free(f); }
HLE(crt_fscanf_s) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 2, 0 }; RET((int64_t)scan_file(cf_get(ARG(0)), f, &a, 0, 1)); free(f); }
HLE(crt_sscanf) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 2, 0 }; RET((int64_t)scan_str(CSTR(0), f, &a, 0, 0)); free(f); }
HLE(crt_sscanf_s) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 2, 0 }; RET((int64_t)scan_str(CSTR(0), f, &a, 0, 1)); free(f); }
HLE(crt_vscanf) { char *f = fmt_from(st, 0, 0); ArgSrc a = { st, 0, ARG(1) }; RET((int64_t)scan_file(&g_files[0], f, &a, 0, 0)); free(f); }
HLE(crt_vsscanf) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 0, ARG(2) }; RET((int64_t)scan_str(CSTR(0), f, &a, 0, 0)); free(f); }
HLE(crt_vfscanf) { char *f = fmt_from(st, 1, 0); ArgSrc a = { st, 0, ARG(2) }; RET((int64_t)scan_file(cf_get(ARG(0)), f, &a, 0, 0)); free(f); }
HLE(crt_swscanf) {
    char *f = fmt_from(st, 1, 1);
    char *s = w2u_dup(WSTR(0));
    ArgSrc a = { st, 2, 0 };
    RET((int64_t)scan_str(s, f, &a, 1, 0));
    free(f); free(s);
}
HLE(ucrt_vsscanf) { // (options, buffer, count, format, locale, va)
    char *f = fmt_from(st, UI(3), 0);
    size_t cnt = UA(2);
    const char *src = CSTR(UI(1));
    char *copy = NULL;
    if (cnt != (size_t)-1) { copy = strndup(src, cnt); src = copy; }
    ArgSrc a = { st, 0, UA(5) };
    RET((int64_t)scan_str(src, f, &a, 0, (ARG(0) & 1) != 0));
    free(f); free(copy);
}
HLE(ucrt_vswscanf) {
    char *f = fmt_from(st, UI(3), 1);
    char *s = w2u_dup(WSTR(UI(1)));
    ArgSrc a = { st, 0, UA(5) };
    RET((int64_t)scan_str(s, f, &a, 1, (ARG(0) & 1) != 0));
    free(f); free(s);
}
HLE(ucrt_vfscanf) { // (options, FILE*, format, locale, va)
    char *f = fmt_from(st, UI(2), 0);
    ArgSrc a = { st, 0, UA(4) };
    RET((int64_t)scan_file(cf_get(UA(1)), f, &a, 0, (ARG(0) & 1) != 0));
    free(f);
}

// ---- stdio ---------------------------------------------------------------------------------

HLE(crt___iob_func) { RET(cf_ptr(0)); }
HLE(crt___acrt_iob_func) { RET(cf_ptr((int)(ARG(0) & 0xFF))); }
static void *data_iob(void) { return g_iob; }

HLE(crt_puts) { const char *s = CSTR(0); cf_write(&g_files[1], s, strlen(s)); cf_write(&g_files[1], "\n", 1); RET(0); }
HLE(crt__putws) { char *s = w2u_dup(WSTR(0)); cf_write(&g_files[1], s, strlen(s)); cf_write(&g_files[1], "\n", 1); free(s); RET(0); }
HLE(crt_fputs) { const char *s = CSTR(0); CrtFile *f = cf_get(ARG(1)); if (!f) { RET(-1); return; } cf_write(f, s, strlen(s)); RET(0); }
HLE(crt_fputws) { char *s = w2u_dup(WSTR(0)); CrtFile *f = cf_get(ARG(1)); if (f) cf_write(f, s, strlen(s)); free(s); RET(f ? 0 : -1); }
HLE(crt_fputc) { char c = (char)ARG(0); CrtFile *f = cf_get(ARG(1)); if (!f) { RET(-1); return; } cf_write(f, &c, 1); RET((uint8_t)c); }
HLE(crt_fputwc) { uint16_t w[2] = { (uint16_t)ARG(0), 0 }; char u[8]; size_t n = w2u(w, 1, u, sizeof u); CrtFile *f = cf_get(ARG(1)); if (f) cf_write(f, u, n); RET((uint16_t)ARG(0)); }
HLE(crt_putchar) { char c = (char)ARG(0); cf_write(&g_files[1], &c, 1); RET((uint8_t)c); }
HLE(crt_putwchar) { uint16_t w[2] = { (uint16_t)ARG(0), 0 }; char u[8]; size_t n = w2u(w, 1, u, sizeof u); cf_write(&g_files[1], u, n); RET((uint16_t)ARG(0)); }
HLE(crt_fwrite) {
    CrtFile *f = cf_get(ARG(3));
    uint64_t sz = ARG(1), cnt = ARG(2);
    if (!f || !sz || !cnt) { RET(0); return; }
    cf_write(f, PTR(0), sz * cnt);
    RET(f->err ? 0 : cnt);
}
HLE(crt_fread) {
    CrtFile *f = cf_get(ARG(3));
    uint64_t sz = ARG(1), cnt = ARG(2);
    if (!f || !sz || !cnt) { RET(0); return; }
    size_t r = cf_read(f, PTR(0), sz * cnt);
    RET(r / sz);
}
HLE(crt_fgetc) { RET((uint64_t)(int64_t)cf_getc(cf_get(ARG(0)))); }
HLE(crt_getchar) { RET((uint64_t)(int64_t)cf_getc(&g_files[0])); }
HLE(crt_fgetwc) { int c = cf_getc(cf_get(ARG(0))); RET(c < 0 ? 0xFFFF : (uint64_t)c); }
HLE(crt__getch) { int c = con_getc(); if (c == '\n') c = '\r'; RET((uint64_t)(int64_t)c); }
HLE(crt__kbhit) { RET(con_has_input()); }
HLE(crt_ungetc) { int c = (int)(int32_t)ARG(0); cf_ungetc(cf_get(ARG(1)), c); RET((uint64_t)(int64_t)c); }
HLE(crt_fgets) {
    char *buf = PTR(0);
    int n = (int)(int32_t)ARG(1);
    CrtFile *f = cf_get(ARG(2));
    if (!f || n <= 0) { RET(0); return; }
    int i = 0;
    while (i < n - 1) {
        int c = cf_getc(f);
        if (c < 0) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = 0;
    RET(i ? ARG(0) : 0);
}
HLE(crt_fgetws) {
    uint16_t *buf = PTR(0);
    int n = (int)(int32_t)ARG(1);
    CrtFile *f = cf_get(ARG(2));
    if (!f || n <= 0) { RET(0); return; }
    char tmp[4096];
    int i = 0;
    while (i < n - 1 && i < (int)sizeof tmp - 1) {
        int c = cf_getc(f);
        if (c < 0) break;
        tmp[i++] = (char)c;
        if (c == '\n') break;
    }
    tmp[i] = 0;
    u2w(tmp, -1, buf, (size_t)n);
    RET(i ? ARG(0) : 0);
}
HLE(crt_gets) {
    char *buf = PTR(0);
    int i = 0;
    for (;;) {
        int c = cf_getc(&g_files[0]);
        if (c < 0) { if (!i) { RET(0); return; } break; }
        if (c == '\n') break;
        buf[i++] = (char)c;
    }
    buf[i] = 0;
    RET(ARG(0));
}
HLE(crt_fopen) { RET(crt_open_file(CSTR(0), CSTR(1))); }
HLE(crt__wfopen) { char *p = w2u_dup(WSTR(0)), *m = w2u_dup(WSTR(1)); RET(crt_open_file(p, m)); free(p); free(m); }
HLE(crt_fopen_s) { uint64_t f = crt_open_file(CSTR(1), CSTR(2)); wr_ptr(ARG(0), f); RET(f ? 0 : (uint64_t)g_errno); }
HLE(crt__wfopen_s) { char *p = w2u_dup(WSTR(1)), *m = w2u_dup(WSTR(2)); uint64_t f = crt_open_file(p, m); wr_ptr(ARG(0), f); RET(f ? 0 : (uint64_t)g_errno); free(p); free(m); }
HLE(crt__fsopen) { RET(crt_open_file(CSTR(0), CSTR(1))); }
HLE(crt_freopen) {
    CrtFile *f = cf_get(ARG(2));
    if (!f) { RET(0); return; }
    int text;
    if (open_mode_flags(CSTR(1), &text) < 0) { RET(0); return; }
    char host[2048];
    path_w2h(CSTR(0), host, sizeof host);
    char hm[4] = { CSTR(1)[0], 'b', strchr(CSTR(1), '+') ? '+' : 0, 0 };
    FILE *hf = fopen(host, hm);
    if (!hf) { g_errno = errno; RET(0); return; }
    if (f->hf) fclose(f->hf);
    *f = (CrtFile){ 1, CF_HOST, hf, text, 0, 0, -1 };
    RET(ARG(2));
}
HLE(crt_fclose) {
    CrtFile *f = cf_get(ARG(0));
    if (!f) { RET(-1); return; }
    if (f->kind == CF_HOST && f->hf) fclose(f->hf);
    if (f->kind == CF_HOST) memset(f, 0, sizeof *f);
    else con_flush();
    RET(0);
}
HLE(crt_fflush) {
    CrtFile *f = cf_get(ARG(0));
    if (!ARG(0) || !f || f->kind != CF_HOST) con_flush();
    else if (f->hf) fflush(f->hf);
    RET(0);
}
void crt_flush_all(void) {
    con_flush();
    for (int i = 3; i < MAX_FILES; i++) if (g_files[i].used && g_files[i].hf) fflush(g_files[i].hf);
}
HLE(crt__flushall) { crt_flush_all(); RET(0); }
HLE(crt_fseek) {
    CrtFile *f = cf_get(ARG(0));
    if (!f || f->kind != CF_HOST) { RET(-1); return; }
    f->ungot = -1; f->eof = 0;
    RET(fseeko(f->hf, (off_t)(int64_t)(f->kind == CF_HOST ? (int32_t)ARG(1) : 0), (int)ARG(2)) ? -1 : 0);
}
HLE(crt__fseeki64) {
    CrtFile *f = cf_get(ARG(0));
    if (!f || f->kind != CF_HOST) { RET(-1); return; }
    f->ungot = -1; f->eof = 0;
    RET(fseeko(f->hf, (off_t)(int64_t)ARG64(1, 1), (int)ARGS(2, 3)) ? -1 : 0);
}
HLE(crt_ftell) { CrtFile *f = cf_get(ARG(0)); RET(f && f->kind == CF_HOST ? (uint64_t)(int64_t)(int32_t)ftello(f->hf) : (uint64_t)-1); }
HLE(crt__ftelli64) { CrtFile *f = cf_get(ARG(0)); RET(f && f->kind == CF_HOST ? (uint64_t)(int64_t)ftello(f->hf) : (uint64_t)-1); }
HLE(crt_rewind) { CrtFile *f = cf_get(ARG(0)); if (f && f->hf) { rewind(f->hf); f->eof = f->err = 0; f->ungot = -1; } RET(0); }
HLE(crt_fgetpos) { CrtFile *f = cf_get(ARG(0)); int64_t p = f && f->hf ? (int64_t)ftello(f->hf) : 0; mem_wr(ARG(1), 8, (uint64_t)p); RET(f ? 0 : -1); }
HLE(crt_fsetpos) { CrtFile *f = cf_get(ARG(0)); if (f && f->hf) { fseeko(f->hf, (off_t)mem_rd(ARG(1), 8), SEEK_SET); f->eof = 0; f->ungot = -1; } RET(f ? 0 : -1); }
HLE(crt_feof) { CrtFile *f = cf_get(ARG(0)); RET(f ? f->eof : 0); }
HLE(crt_ferror) { CrtFile *f = cf_get(ARG(0)); RET(f ? f->err : 0); }
HLE(crt_clearerr) { CrtFile *f = cf_get(ARG(0)); if (f) { f->eof = f->err = 0; if (f->hf) clearerr(f->hf); } RET(0); }
HLE_STUB_RET(crt_setvbuf, 0)
HLE_STUB_RET(crt_setbuf, 0)
HLE(crt__fileno) {
    CrtFile *f = cf_get(ARG(0));
    if (!f) { RET(-1); return; }
    if (f->kind == CF_STDIN) RET(0);
    else if (f->kind == CF_STDOUT) RET(1);
    else if (f->kind == CF_STDERR) RET(2);
    else RET(fileno(f->hf));
}
HLE(crt__isatty) { RET((int)ARG(0) <= 2 ? 0x40 : 0); }
HLE(crt__setmode) { RET(0x4000); }
HLE(crt_tmpfile) {
    FILE *hf = tmpfile();
    for (int i = 3; hf && i < MAX_FILES; i++)
        if (!g_files[i].used) { g_files[i] = (CrtFile){ 1, CF_HOST, hf, 0, 0, 0, -1 }; RET(cf_ptr(i)); return; }
    RET(0);
}
HLE(crt_remove) { char h[2048]; path_w2h(CSTR(0), h, sizeof h); int r = unlink(h); if (r) g_errno = errno; RET(r ? -1 : 0); }
HLE(crt__wremove) { char *p = w2u_dup(WSTR(0)); char h[2048]; path_w2h(p, h, sizeof h); int r = unlink(h); free(p); RET(r ? -1 : 0); }
HLE(crt_rename) { char a[2048], b[2048]; path_w2h(CSTR(0), a, sizeof a); path_w2h(CSTR(1), b, sizeof b); int r = rename(a, b); if (r) g_errno = errno; RET(r ? -1 : 0); }
HLE(crt__access) { char h[2048]; path_w2h(CSTR(0), h, sizeof h); int m = (int)ARG(1); int r = access(h, (m & 2) ? W_OK : F_OK); if (r) g_errno = errno; RET(r ? -1 : 0); }
HLE(crt__waccess) { char *p = w2u_dup(WSTR(0)); char h[2048]; path_w2h(p, h, sizeof h); free(p); int r = access(h, ((int)ARG(1) & 2) ? W_OK : F_OK); RET(r ? -1 : 0); }
HLE(crt__mkdir) { char h[2048]; path_w2h(CSTR(0), h, sizeof h); int r = mkdir(h, 0755); if (r) g_errno = errno; RET(r ? -1 : 0); }
HLE(crt__wmkdir) { char *p = w2u_dup(WSTR(0)); char h[2048]; path_w2h(p, h, sizeof h); free(p); RET(mkdir(h, 0755) ? -1 : 0); }
HLE(crt__rmdir) { char h[2048]; path_w2h(CSTR(0), h, sizeof h); RET(rmdir(h) ? -1 : 0); }
HLE(crt__chdir) {
    char norm[1200];
    path_normalize_win(CSTR(0), norm, sizeof norm);
    char h[2048];
    path_w2h(norm, h, sizeof h);
    struct stat sb;
    if (stat(h, &sb) || !S_ISDIR(sb.st_mode)) { g_errno = ENOENT; RET(-1); return; }
    snprintf(g_proc->cwd_win, sizeof g_proc->cwd_win, "%s", norm);
    RET(0);
}
HLE(crt__getcwd) {
    uint64_t buf = ARG(0);
    size_t n = (size_t)(int32_t)ARG(1);
    size_t need = strlen(g_proc->cwd_win) + 1;
    if (!buf) { buf = H2G(g_malloc(need > n ? need : n)); n = need > n ? need : n; }
    if (need > n) { g_errno = ERANGE; RET(0); return; }
    memcpy(G2H(buf), g_proc->cwd_win, need);
    RET(buf);
}
HLE(crt__wgetcwd) {
    uint64_t buf = ARG(0);
    size_t n = (size_t)(int32_t)ARG(1);
    size_t need = u2w(g_proc->cwd_win, -1, NULL, 0) + 1;
    if (!buf) { n = need > n ? need : n; buf = H2G(g_malloc(n * 2)); }
    if (need > n) { RET(0); return; }
    u2w(g_proc->cwd_win, -1, (uint16_t *)G2H(buf), n);
    RET(buf);
}
HLE(crt__fullpath) {
    char norm[1200];
    path_normalize_win(CSTR(1), norm, sizeof norm);
    uint64_t buf = ARG(0);
    size_t n = ARG(2);
    if (!buf) { buf = H2G(g_strdup(norm)); RET(buf); return; }
    if (strlen(norm) + 1 > n) { RET(0); return; }
    memcpy(G2H(buf), norm, strlen(norm) + 1);
    RET(buf);
}

// big: 1 = struct _stat64, 0 = _stat64i32, 2 = 32-bit struct _stat32 (32-bit times and size)
static void fill_stat(uint64_t dst, const struct stat *sb, int big) {
    uint8_t *d = (uint8_t *)G2H(dst);
    if (big == 2) {
        memset(d, 0, 36);
        uint16_t mode = (uint16_t)((S_ISDIR(sb->st_mode) ? 0x4000 : 0x8000) | ((sb->st_mode & S_IRUSR) ? 0x124 : 0) | ((sb->st_mode & S_IWUSR) ? 0x92 : 0));
        uint16_t nlink = 1;
        uint32_t dev = 2, sz = (uint32_t)sb->st_size, at = (uint32_t)sb->st_atime, mt = (uint32_t)sb->st_mtime, ct = (uint32_t)sb->st_ctime;
        memcpy(d, &dev, 4); memcpy(d + 6, &mode, 2); memcpy(d + 8, &nlink, 2); memcpy(d + 16, &dev, 4);
        memcpy(d + 20, &sz, 4); memcpy(d + 24, &at, 4); memcpy(d + 28, &mt, 4); memcpy(d + 32, &ct, 4);
        return;
    }
    memset(d, 0, big ? 56 : 48);
    uint16_t mode = (uint16_t)((S_ISDIR(sb->st_mode) ? 0x4000 : 0x8000) | ((sb->st_mode & S_IRUSR) ? 0x124 : 0) | ((sb->st_mode & S_IWUSR) ? 0x92 : 0));
    uint16_t nlink = 1;
    memcpy(d + 6, &mode, 2);
    memcpy(d + 8, &nlink, 2);
    uint32_t dev = 2;
    memcpy(d, &dev, 4);
    memcpy(d + 16, &dev, 4);
    int64_t at = sb->st_atime, mt = sb->st_mtime, ct = sb->st_ctime;
    if (big) {
        int64_t sz = sb->st_size;
        memcpy(d + 24, &sz, 8); memcpy(d + 32, &at, 8); memcpy(d + 40, &mt, 8); memcpy(d + 48, &ct, 8);
    } else {
        int32_t sz = (int32_t)sb->st_size;
        memcpy(d + 20, &sz, 4); memcpy(d + 24, &at, 8); memcpy(d + 32, &mt, 8); memcpy(d + 40, &ct, 8);
    }
}
static void stat_impl(X86State *st, const char *path, int big) {
    char h[2048];
    path_w2h(path, h, sizeof h);
    struct stat sb;
    if (stat(h, &sb)) { g_errno = ENOENT; RET(-1); return; }
    fill_stat(ARG(1), &sb, big);
    RET(0);
}
HLE(crt__stat64) { stat_impl(st, CSTR(0), 1); }
HLE(crt__stat64i32) { stat_impl(st, CSTR(0), 0); }
HLE(crt__wstat64) { char *p = w2u_dup(WSTR(0)); stat_impl(st, p, 1); free(p); }
HLE(crt__wstat64i32) { char *p = w2u_dup(WSTR(0)); stat_impl(st, p, 0); free(p); }
static void fstat_impl(X86State *st, int big) {
    int fd = (int)ARG(0);
    struct stat sb;
    if (fd <= 2) { memset(&sb, 0, sizeof sb); sb.st_mode = S_IFCHR; }
    else if (fstat(fd, &sb)) { RET(-1); return; }
    fill_stat(ARG(1), &sb, big);
    RET(0);
}
HLE(crt__fstat64) { fstat_impl(st, 1); }
HLE(crt__fstat64i32) { fstat_impl(st, 0); }
// msvcrt's plain _stat / _fstat use 32-bit times and sizes in 32-bit programs
HLE(crt__stat_plain) { stat_impl(st, CSTR(0), g_mode32 ? 2 : 0); }
HLE(crt__wstat_plain) { char *p = w2u_dup(WSTR(0)); stat_impl(st, p, g_mode32 ? 2 : 0); free(p); }
HLE(crt__fstat_plain) { fstat_impl(st, g_mode32 ? 2 : 0); }
HLE(crt__stat32) { stat_impl(st, CSTR(0), 2); }
HLE(crt__fstat32) { fstat_impl(st, 2); }

// low level io
HLE(crt__open) {
    int wf = (int)ARG(1);
    int fl = (wf & 3) == 0 ? O_RDONLY : (wf & 3) == 1 ? O_WRONLY : O_RDWR;
    if (wf & 0x100) fl |= O_CREAT;
    if (wf & 0x200) fl |= O_TRUNC;
    if (wf & 0x400) fl |= O_EXCL;
    if (wf & 0x8) fl |= O_APPEND;
    char h[2048];
    path_w2h(CSTR(0), h, sizeof h);
    int fd = open(h, fl, 0644);
    if (fd < 0) g_errno = errno;
    RET((uint64_t)(int64_t)fd);
}
HLE(crt__close) { int fd = (int)ARG(0); RET(fd > 2 ? (uint64_t)(int64_t)close(fd) : 0); }
HLE(crt__read) {
    int fd = (int)ARG(0);
    if (fd == 0) { RET(con_read(PTR(1), (size_t)(uint32_t)ARG(2), 1)); return; }
    RET((uint64_t)(int64_t)read(fd, PTR(1), (size_t)(uint32_t)ARG(2)));
}
HLE(crt__write) {
    int fd = (int)ARG(0);
    size_t n = (size_t)(uint32_t)ARG(2);
    if (fd == 1 || fd == 2) { con_write(fd, PTR(1), n); RET(n); return; }
    RET((uint64_t)(int64_t)write(fd, PTR(1), n));
}
HLE(crt__lseek) { RET((uint64_t)(int64_t)(int32_t)lseek((int)ARG(0), (off_t)(int32_t)ARG(1), (int)ARG(2))); }
HLE(crt__lseeki64) { RET((uint64_t)(int64_t)lseek((int)ARG(0), (off_t)(int64_t)ARG64(1, 1), (int)ARGS(2, 3))); }
HLE(crt__filelength) { struct stat sb; RET(fstat((int)ARG(0), &sb) ? (uint64_t)-1 : (uint64_t)sb.st_size); }
HLE(crt__get_osfhandle) { int fd = (int)ARG(0); RET(fd <= 2 ? h_std(fd) : (uint64_t)fd); }
HLE(crt_perror) {
    char buf[512];
    const char *s = CSTR(0);
    int n = snprintf(buf, sizeof buf, "%s%s%s\n", s && *s ? s : "", s && *s ? ": " : "", strerror(g_errno));
    cf_write(&g_files[2], buf, (size_t)n);
    RET(0);
}

// ---- memory ---------------------------------------------------------------------------------

HLE(crt_malloc) { RET(g_malloc(ARG(0) ? ARG(0) : 1)); }
HLE(crt_calloc) { RET(g_calloc(ARG(0) ? ARG(0) : 1, ARG(1) ? ARG(1) : 1)); }
HLE(crt_realloc) { RET(g_realloc(PTR(0), ARG(1))); }
HLE(crt_free) { g_free(PTR(0)); RET(0); }
HLE(crt__msize) { RET(ARG(0) ? g_msize(PTR(0)) : (size_t)-1); }
HLE(crt__recalloc) {
    size_t old = ARG(0) ? g_msize(PTR(0)) : 0;
    size_t n = ARG(1) * ARG(2);
    void *p = g_realloc(PTR(0), n);
    if (p && n > old) memset((char *)p + old, 0, n - old);
    RET(p);
}
// Aligned blocks: 64-bit programs use posix_memalign; 32-bit ones over-allocate in the guest heap
// and keep the real block address (and a marker) in the 8 bytes before the aligned pointer.
#define ALIGN_MAGIC 0x4E474C41u
static void *aligned_alloc_g(size_t n, size_t al) {
    if (!g_mode32) { void *p = NULL; if (posix_memalign(&p, al, n ? n : 1)) p = NULL; return p; }
    uint8_t *raw = g_malloc(n + al + 8);
    if (!raw) return NULL;
    uint64_t a = (H2G(raw) + 8 + al - 1) & ~(uint64_t)(al - 1);
    mem_wr(a - 8, 4, ALIGN_MAGIC);
    mem_wr(a - 4, 4, H2G(raw));
    return G2H(a);
}
static void aligned_free_g(void *p) {
    if (!p) return;
    if (!g_mode32) { free(p); return; }
    uint64_t a = H2G(p);
    if (mem_rd(a - 8, 4) == ALIGN_MAGIC) { mem_wr(a - 8, 4, 0); g_free(G2H(mem_rd(a - 4, 4))); }
    else g_free(p);
}
static size_t aligned_size_g(void *p) {
    if (!g_mode32) return HOST_MSIZE(p);
    uint64_t a = H2G(p);
    if (mem_rd(a - 8, 4) != ALIGN_MAGIC) return g_msize(p);
    uint64_t raw = mem_rd(a - 4, 4);
    return g_msize(G2H(raw)) - (size_t)(a - raw);
}
HLE(crt__aligned_malloc) {
    size_t al = ARG(1) < 16 ? 16 : ARG(1);
    RET(aligned_alloc_g(ARG(0) ? ARG(0) : 1, al));
}
HLE(crt__aligned_free) { aligned_free_g(PTR(0)); RET(0); }
HLE(crt__aligned_realloc) {
    size_t al = ARG(2) < 16 ? 16 : ARG(2);
    void *p = aligned_alloc_g(ARG(1) ? ARG(1) : 1, al);
    if (p && ARG(0)) {
        size_t old = aligned_size_g(PTR(0));
        memcpy(p, PTR(0), old < ARG(1) ? old : ARG(1));
        aligned_free_g(PTR(0));
    }
    RET(p);
}
HLE_STUB_RET(crt__expand, 0)
HLE(crt__callnewh) { RET(0); }

// ---- strings ----------------------------------------------------------------------------------

HLE(crt_strlen) { RET(strlen(CSTR(0))); }
HLE(crt_strnlen) { RET(strnlen(CSTR(0), ARG(1))); }
HLE(crt_strcpy) { strcpy(PTR(0), CSTR(1)); RET(ARG(0)); }
HLE(crt_strncpy) { strncpy(PTR(0), CSTR(1), ARG(2)); RET(ARG(0)); }
HLE(crt_strcat) { strcat(PTR(0), CSTR(1)); RET(ARG(0)); }
HLE(crt_strncat) { strncat(PTR(0), CSTR(1), ARG(2)); RET(ARG(0)); }
HLE(crt_strcmp) { RET((int64_t)strcmp(CSTR(0), CSTR(1))); }
HLE(crt_strncmp) { RET((int64_t)strncmp(CSTR(0), CSTR(1), ARG(2))); }
HLE(crt__stricmp) { RET((int64_t)strcasecmp(CSTR(0), CSTR(1))); }
HLE(crt__strnicmp) { RET((int64_t)strncasecmp(CSTR(0), CSTR(1), ARG(2))); }
HLE(crt_strchr) { RET(strchr(CSTR(0), (int)ARG(1))); }
HLE(crt_strrchr) { RET(strrchr(CSTR(0), (int)ARG(1))); }
HLE(crt_strstr) { RET(strstr(CSTR(0), CSTR(1))); }
HLE(crt_strpbrk) { RET(strpbrk(CSTR(0), CSTR(1))); }
HLE(crt_strspn) { RET(strspn(CSTR(0), CSTR(1))); }
HLE(crt_strcspn) { RET(strcspn(CSTR(0), CSTR(1))); }
HLE(crt__strdup) { RET(ARG(0) ? g_strdup(CSTR(0)) : NULL); }
HLE(crt_strtok) { RET(strtok_r(PTR(0), CSTR(1), &g_strtok_save)); (void)g_strtok_save_dummy; }
HLE(crt_strtok_s) { char *save = (char *)G2H(rd_ptr(ARG(2))); char *r = strtok_r(PTR(0), CSTR(1), &save); wr_ptr(ARG(2), H2G(save)); RET(r); }
HLE(crt__strlwr) { for (char *p = PTR(0); *p; p++) *p = (char)tolower((unsigned char)*p); RET(ARG(0)); }
HLE(crt__strupr) { for (char *p = PTR(0); *p; p++) *p = (char)toupper((unsigned char)*p); RET(ARG(0)); }
HLE(crt__strrev) { char *s = PTR(0); size_t n = strlen(s); for (size_t i = 0; i < n / 2; i++) { char t = s[i]; s[i] = s[n - 1 - i]; s[n - 1 - i] = t; } RET(ARG(0)); }
HLE(crt_strerror) { snprintf(g_cg->strerror_buf, sizeof g_cg->strerror_buf, "%s", strerror((int)ARG(0))); RET(g_cg->strerror_buf); }
HLE(crt_strerror_s) { snprintf(PTR(0), ARG(1), "%s", strerror((int)ARG(2))); RET(0); }
HLE(crt_strcoll) { RET((int64_t)strcmp(CSTR(0), CSTR(1))); }
HLE(crt_strxfrm) { size_t n = strlen(CSTR(1)); if (ARG(2) > n) strcpy(PTR(0), CSTR(1)); RET(n); }
HLE(crt_strcpy_s) {
    size_t n = strlen(CSTR(2));
    if (!ARG(0) || n + 1 > ARG(1)) { if (ARG(0) && ARG(1)) *(char *)PTR(0) = 0; RET(ERANGE); return; }
    memcpy(PTR(0), CSTR(2), n + 1);
    RET(0);
}
HLE(crt_strncpy_s) {
    size_t n = strnlen(CSTR(2), ARG(3));
    if (!ARG(0) || n + 1 > ARG(1)) { if (ARG(0) && ARG(1)) *(char *)PTR(0) = 0; RET(ERANGE); return; }
    memcpy(PTR(0), CSTR(2), n);
    ((char *)PTR(0))[n] = 0;
    RET(0);
}
HLE(crt_strcat_s) {
    size_t a = strlen(CSTR(0)), b = strlen(CSTR(2));
    if (a + b + 1 > ARG(1)) { RET(ERANGE); return; }
    memcpy((char *)PTR(0) + a, CSTR(2), b + 1);
    RET(0);
}
HLE(crt_strncat_s) {
    size_t a = strlen(CSTR(0)), b = strnlen(CSTR(2), ARG(3));
    if (a + b + 1 > ARG(1)) { RET(ERANGE); return; }
    memcpy((char *)PTR(0) + a, CSTR(2), b);
    ((char *)PTR(0))[a + b] = 0;
    RET(0);
}
HLE(crt_memcpy) { memmove(PTR(0), PTR(1), ARG(2)); RET(ARG(0)); }
HLE(crt_memset) { memset(PTR(0), (int)ARG(1), ARG(2)); RET(ARG(0)); }
HLE(crt_memcmp) { RET((int64_t)memcmp(PTR(0), PTR(1), ARG(2))); }
HLE(crt_memchr) { RET(memchr(PTR(0), (int)ARG(1), ARG(2))); }
HLE(crt_memcpy_s) { if (ARG(3) > ARG(1)) { RET(ERANGE); return; } memmove(PTR(0), PTR(2), ARG(3)); RET(0); }

// wide strings (16-bit wchar_t)
static int wcmp(const uint16_t *a, const uint16_t *b, size_t n, int icase) {
    for (size_t i = 0; i < n; i++) {
        uint32_t x = a[i], y = b[i];
        if (icase) { x = (uint32_t)towlower((wint_t)x); y = (uint32_t)towlower((wint_t)y); }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
HLE(crt_wcslen) { RET(wstrlen(WSTR(0))); }
HLE(crt_wcsnlen) { const uint16_t *w = WSTR(0); size_t n = 0; while (n < ARG(1) && w[n]) n++; RET(n); }
HLE(crt_wcscpy) { uint16_t *d = PTR(0); const uint16_t *s = WSTR(1); size_t n = wstrlen(s); memmove(d, s, (n + 1) * 2); RET(ARG(0)); }
HLE(crt_wcsncpy) { uint16_t *d = PTR(0); const uint16_t *s = WSTR(1); size_t n = ARG(2), i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; RET(ARG(0)); }
HLE(crt_wcscat) { uint16_t *d = PTR(0); const uint16_t *s = WSTR(1); size_t a = wstrlen(d), n = wstrlen(s); memmove(d + a, s, (n + 1) * 2); RET(ARG(0)); }
HLE(crt_wcsncat) { uint16_t *d = PTR(0); const uint16_t *s = WSTR(1); size_t a = wstrlen(d), i = 0; for (; i < ARG(2) && s[i]; i++) d[a + i] = s[i]; d[a + i] = 0; RET(ARG(0)); }
HLE(crt_wcscmp) { RET((int64_t)wcmp(WSTR(0), WSTR(1), (size_t)-1, 0)); }
HLE(crt_wcsncmp) { RET((int64_t)wcmp(WSTR(0), WSTR(1), ARG(2), 0)); }
HLE(crt__wcsicmp) { RET((int64_t)wcmp(WSTR(0), WSTR(1), (size_t)-1, 1)); }
HLE(crt__wcsnicmp) { RET((int64_t)wcmp(WSTR(0), WSTR(1), ARG(2), 1)); }
HLE(crt_wcschr) { const uint16_t *s = WSTR(0); uint16_t c = (uint16_t)ARG(1); for (;; s++) { if (*s == c) { RET(s); return; } if (!*s) break; } RET(0); }
HLE(crt_wcsrchr) { const uint16_t *s = WSTR(0), *r = NULL; uint16_t c = (uint16_t)ARG(1); for (;; s++) { if (*s == c) r = s; if (!*s) break; } RET(r); }
HLE(crt_wcsstr) {
    const uint16_t *h = WSTR(0), *n = WSTR(1);
    size_t nl = wstrlen(n);
    if (!nl) { RET(h); return; }
    for (; *h; h++) if (wcmp(h, n, nl, 0) == 0) { RET(h); return; }
    RET(0);
}
HLE(crt__wcsdup) { const uint16_t *s = WSTR(0); size_t n = wstrlen(s); uint16_t *d = g_malloc((n + 1) * 2); memcpy(d, s, (n + 1) * 2); RET(d); }
HLE(crt__wcslwr) { for (uint16_t *p = PTR(0); *p; p++) *p = (uint16_t)towlower(*p); RET(ARG(0)); }
HLE(crt__wcsupr) { for (uint16_t *p = PTR(0); *p; p++) *p = (uint16_t)towupper(*p); RET(ARG(0)); }
HLE(crt_wcscpy_s) { const uint16_t *s = WSTR(2); size_t n = wstrlen(s); if (n + 1 > ARG(1)) { RET(ERANGE); return; } memmove(PTR(0), s, (n + 1) * 2); RET(0); }
HLE(crt_wcscat_s) { uint16_t *d = PTR(0); const uint16_t *s = WSTR(2); size_t a = wstrlen(d), n = wstrlen(s); if (a + n + 1 > ARG(1)) { RET(ERANGE); return; } memmove(d + a, s, (n + 1) * 2); RET(0); }
HLE(crt_wcsncpy_s) { const uint16_t *s = WSTR(2); size_t n = 0; while (n < ARG(3) && s[n]) n++; if (n + 1 > ARG(1)) { RET(ERANGE); return; } memmove(PTR(0), s, n * 2); ((uint16_t *)PTR(0))[n] = 0; RET(0); }
HLE(crt_wcsspn) { const uint16_t *s = WSTR(0), *set = WSTR(1); size_t i = 0; for (; s[i]; i++) { const uint16_t *q = set; while (*q && *q != s[i]) q++; if (!*q) break; } RET(i); }
HLE(crt_wcscspn) { const uint16_t *s = WSTR(0), *set = WSTR(1); size_t i = 0; for (; s[i]; i++) { const uint16_t *q = set; while (*q && *q != s[i]) q++; if (*q) break; } RET(i); }
HLE(crt_wcspbrk) { const uint16_t *s = WSTR(0), *set = WSTR(1); for (; *s; s++) for (const uint16_t *q = set; *q; q++) if (*q == *s) { RET(s); return; } RET(0); }
HLE(crt_wcstok) {
    uint16_t *s = PTR(0);
    const uint16_t *delim = WSTR(1);
    uint16_t **save = ARG(2) ? (uint16_t **)PTR(2) : &g_wcstok_save;
    if (!s) s = *save;
    if (!s) { RET(0); return; }
    #define ISDELIM(c) ({ int _r = 0; for (const uint16_t *_q = delim; *_q; _q++) if (*_q == (c)) _r = 1; _r; })
    while (*s && ISDELIM(*s)) s++;
    if (!*s) { *save = NULL; RET(0); return; }
    uint16_t *start = s;
    while (*s && !ISDELIM(*s)) s++;
    if (*s) { *s = 0; *save = s + 1; } else *save = NULL;
    RET(start);
}
HLE(crt_wmemcpy) { memmove(PTR(0), PTR(1), ARG(2) * 2); RET(ARG(0)); }
HLE(crt_wmemset) { uint16_t *d = PTR(0); for (size_t i = 0; i < ARG(2); i++) d[i] = (uint16_t)ARG(1); RET(ARG(0)); }

static char *wide_ascii(const uint16_t *w, size_t *n) {
    size_t l = 0;
    while (w[l] && w[l] < 128) l++;
    char *s = malloc(l + 1);
    for (size_t i = 0; i < l; i++) s[i] = (char)w[i];
    s[l] = 0;
    *n = l;
    return s;
}

// ---- conversion -------------------------------------------------------------------------------

static int64_t clamp32(long long v) { if (v > INT32_MAX) { g_errno = ERANGE; return INT32_MAX; } if (v < INT32_MIN) { g_errno = ERANGE; return INT32_MIN; } return v; }
HLE(crt_atoi) { RET((int64_t)(int32_t)strtol(CSTR(0), NULL, 10)); }
HLE(crt_atoll) { RET((int64_t)strtoll(CSTR(0), NULL, 10)); }
HLE(crt_atof) { RETD(atof(CSTR(0))); }
HLE(crt_strtod) { char *e; double v = strtod(CSTR(0), &e); if (ARG(1)) wr_ptr(ARG(1), H2G(e)); RETD(v); }
HLE(crt_strtof) { char *e; float v = strtof(CSTR(0), &e); if (ARG(1)) wr_ptr(ARG(1), H2G(e)); RETF(v); }
HLE(crt_strtol) { char *e; long long v = strtoll(CSTR(0), &e, (int)ARG(2)); if (ARG(1)) wr_ptr(ARG(1), H2G(e)); RET(clamp32(v)); }
HLE(crt_strtoul) {
    char *e;
    const char *s = CSTR(0);
    unsigned long long v = strtoull(s, &e, (int)ARG(2));
    if (ARG(1)) wr_ptr(ARG(1), H2G(e));
    const char *p = s; while (isspace((unsigned char)*p)) p++;
    if (*p != '-' && v > UINT32_MAX) { g_errno = ERANGE; v = UINT32_MAX; }
    RET((uint32_t)v);
}
HLE(crt_strtoll) { char *e; long long v = strtoll(CSTR(0), &e, (int)ARG(2)); if (ARG(1)) wr_ptr(ARG(1), H2G(e)); RET(v); }
HLE(crt_strtoull) { char *e; unsigned long long v = strtoull(CSTR(0), &e, (int)ARG(2)); if (ARG(1)) wr_ptr(ARG(1), H2G(e)); RET(v); }
HLE(crt__wtoi) { size_t n; char *s = wide_ascii(WSTR(0), &n); RET((int64_t)(int32_t)strtol(s, NULL, 10)); free(s); }
HLE(crt__wtoi64) { size_t n; char *s = wide_ascii(WSTR(0), &n); RET((int64_t)strtoll(s, NULL, 10)); free(s); }
HLE(crt__wtof) { size_t n; char *s = wide_ascii(WSTR(0), &n); RETD(atof(s)); free(s); }
HLE(crt_wcstol) {
    size_t n; const uint16_t *w = WSTR(0); char *s = wide_ascii(w, &n); char *e;
    long long v = strtoll(s, &e, (int)ARG(2));
    if (ARG(1)) wr_ptr(ARG(1), H2G(w + (e - s)));
    RET(clamp32(v)); free(s);
}
HLE(crt_wcstoul) {
    size_t n; const uint16_t *w = WSTR(0); char *s = wide_ascii(w, &n); char *e;
    unsigned long long v = strtoull(s, &e, (int)ARG(2));
    if (ARG(1)) wr_ptr(ARG(1), H2G(w + (e - s)));
    RET((uint32_t)v); free(s);
}
HLE(crt_wcstoll) {
    size_t n; const uint16_t *w = WSTR(0); char *s = wide_ascii(w, &n); char *e;
    long long v = strtoll(s, &e, (int)ARG(2));
    if (ARG(1)) wr_ptr(ARG(1), H2G(w + (e - s)));
    RET(v); free(s);
}
HLE(crt_wcstod) {
    size_t n; const uint16_t *w = WSTR(0); char *s = wide_ascii(w, &n); char *e;
    double v = strtod(s, &e);
    if (ARG(1)) wr_ptr(ARG(1), H2G(w + (e - s)));
    RETD(v); free(s);
}
static void itoa_impl(uint64_t v, int neg, char *buf, int radix) {
    char tmp[72];
    int n = 0;
    if (radix < 2 || radix > 36) radix = 10;
    do { int d = (int)(v % (uint64_t)radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (uint64_t)radix; } while (v);
    int o = 0;
    if (neg) buf[o++] = '-';
    while (n) buf[o++] = tmp[--n];
    buf[o] = 0;
}
HLE(crt__itoa) { int32_t v = (int32_t)ARG(0); int r = (int)ARG(2); itoa_impl(v < 0 && r == 10 ? (uint64_t)(-(int64_t)v) : (uint32_t)v, v < 0 && r == 10, PTR(1), r); RET(ARG(1)); }
HLE(crt__ultoa) { itoa_impl((uint32_t)ARG(0), 0, PTR(1), (int)ARG(2)); RET(ARG(1)); }
HLE(crt__i64toa) { int64_t v = (int64_t)ARG64(0, 0); int r = (int)ARGS(2, 3); itoa_impl(v < 0 && r == 10 ? (uint64_t)(-v) : (uint64_t)v, v < 0 && r == 10, G2H(ARGS(1, 2)), r); RET(ARGS(1, 2)); }
HLE(crt__ui64toa) { itoa_impl(ARG64(0, 0), 0, G2H(ARGS(1, 2)), (int)ARGS(2, 3)); RET(ARGS(1, 2)); }
HLE(crt__itoa_s) { int32_t v = (int32_t)ARG(0); int r = (int)ARG(3); char tmp[72]; itoa_impl(v < 0 && r == 10 ? (uint64_t)(-(int64_t)v) : (uint32_t)v, v < 0 && r == 10, tmp, r); if (strlen(tmp) + 1 > ARG(2)) { RET(ERANGE); return; } strcpy(PTR(1), tmp); RET(0); }
HLE(crt__i64toa_s) { int64_t v = (int64_t)ARG64(0, 0); int r = (int)ARGS(3, 4); char tmp[72]; itoa_impl(v < 0 && r == 10 ? (uint64_t)(-v) : (uint64_t)v, v < 0 && r == 10, tmp, r); if (strlen(tmp) + 1 > ARGS(2, 3)) { RET(ERANGE); return; } strcpy(G2H(ARGS(1, 2)), tmp); RET(0); }
HLE(crt__ui64toa_s) { char tmp[72]; itoa_impl(ARG64(0, 0), 0, tmp, (int)ARGS(3, 4)); if (strlen(tmp) + 1 > ARGS(2, 3)) { RET(ERANGE); return; } strcpy(G2H(ARGS(1, 2)), tmp); RET(0); }
HLE(crt__itow) { int32_t v = (int32_t)ARG(0); int r = (int)ARG(2); char tmp[72]; itoa_impl(v < 0 && r == 10 ? (uint64_t)(-(int64_t)v) : (uint32_t)v, v < 0 && r == 10, tmp, r); u2w(tmp, -1, PTR(1), 72); RET(ARG(1)); }

// multibyte <-> wide (UTF-8)
HLE(crt_mbstowcs) {
    const char *s = CSTR(1);
    size_t need = u2w(s, -1, NULL, 0);
    if (!ARG(0)) { RET(need); return; }
    uint16_t *tmp = malloc((need + 1) * 2);
    u2w(s, -1, tmp, need + 1);
    size_t n = need < ARG(2) ? need : ARG(2);
    memcpy(PTR(0), tmp, n * 2);
    if (n < ARG(2)) ((uint16_t *)PTR(0))[n] = 0;
    free(tmp);
    RET(n);
}
HLE(crt_wcstombs) {
    const uint16_t *w = WSTR(1);
    size_t need = w2u(w, -1, NULL, 0);
    if (!ARG(0)) { RET(need); return; }
    char *tmp = malloc(need + 1);
    w2u(w, -1, tmp, need + 1);
    size_t n = need < ARG(2) ? need : ARG(2);
    memcpy(PTR(0), tmp, n);
    if (n < ARG(2)) ((char *)PTR(0))[n] = 0;
    free(tmp);
    RET(n);
}
HLE(crt_mbtowc) {
    const char *s = CSTR(1);
    if (!s) { RET(0); return; }
    if (!*s) { if (ARG(0)) mem_wr(ARG(0), 2, 0); RET(0); return; }
    unsigned char c = (unsigned char)*s;
    int len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if ((size_t)len > ARG(2)) { RET(-1); return; }
    uint16_t w[4];
    u2w(s, len, w, 4);
    if (ARG(0)) mem_wr(ARG(0), 2, w[0]);
    RET(len);
}
HLE(crt_wctomb) {
    if (!ARG(0)) { RET(0); return; }
    uint16_t w[2] = { (uint16_t)ARG(1), 0 };
    char u[8];
    size_t n = w2u(w, 1, u, sizeof u);
    memcpy(PTR(0), u, n);
    RET(n);
}
HLE(crt_mblen) { const char *s = CSTR(0); if (!s || !*s) { RET(0); return; } unsigned char c = (unsigned char)*s; RET(c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2); }
HLE(crt_btowc) { int c = (int)ARG(0); RET(c < 0 || c > 127 ? 0xFFFF : (uint64_t)c); }

// ---- ctype ------------------------------------------------------------------------------------

#define CT(name, expr) HLE(crt_##name) { int c = (int)(int32_t)ARG(0); RET(c >= 0 && c < 256 ? (expr) : 0); }
CT(isalpha, isalpha(c) ? 0x103 : 0)
CT(isdigit, isdigit(c) ? 4 : 0)
CT(isalnum, isalnum(c) ? 0x107 : 0)
CT(isspace, isspace(c) ? 8 : 0)
CT(isupper, isupper(c) ? 1 : 0)
CT(islower, islower(c) ? 2 : 0)
CT(isxdigit, isxdigit(c) ? 0x80 : 0)
CT(ispunct, ispunct(c) ? 0x10 : 0)
CT(isprint, isprint(c) ? 0x157 : 0)
CT(isgraph, isgraph(c) ? 0x117 : 0)
CT(iscntrl, iscntrl(c) ? 0x20 : 0)
CT(isblank, (c == ' ' || c == '\t') ? 0x40 : 0)
HLE(crt_toupper) { int c = (int)(int32_t)ARG(0); RET((int64_t)(c >= 0 && c < 256 ? toupper(c) : c)); }
HLE(crt_tolower) { int c = (int)(int32_t)ARG(0); RET((int64_t)(c >= 0 && c < 256 ? tolower(c) : c)); }
HLE(crt_isascii) { RET(ARG(0) < 128); }
HLE(crt_iswalpha) { RET(iswalpha((wint_t)(uint16_t)ARG(0)) ? 0x103 : 0); }
HLE(crt_iswdigit) { RET(iswdigit((wint_t)(uint16_t)ARG(0)) ? 4 : 0); }
HLE(crt_iswspace) { RET(iswspace((wint_t)(uint16_t)ARG(0)) ? 8 : 0); }
HLE(crt_iswalnum) { RET(iswalnum((wint_t)(uint16_t)ARG(0)) ? 0x107 : 0); }
HLE(crt_iswupper) { RET(iswupper((wint_t)(uint16_t)ARG(0)) ? 1 : 0); }
HLE(crt_iswlower) { RET(iswlower((wint_t)(uint16_t)ARG(0)) ? 2 : 0); }
HLE(crt_iswpunct) { RET(iswpunct((wint_t)(uint16_t)ARG(0)) ? 0x10 : 0); }
HLE(crt_iswprint) { RET(iswprint((wint_t)(uint16_t)ARG(0)) ? 0x157 : 0); }
HLE(crt_iswxdigit) { RET(iswxdigit((wint_t)(uint16_t)ARG(0)) ? 0x80 : 0); }
HLE(crt_iswctype) {
    wint_t c = (wint_t)(uint16_t)ARG(0);
    unsigned m = (unsigned)ARG(1);
    unsigned r = 0;
    if (c < 256) r = g_ctype_table[c + 1];
    else { if (iswupper(c)) r |= 1; if (iswlower(c)) r |= 2; if (iswdigit(c)) r |= 4; if (iswspace(c)) r |= 8; if (iswpunct(c)) r |= 0x10; if (iswalpha(c)) r |= 0x100; }
    RET(r & m);
}
HLE(crt_towupper) { RET((uint16_t)towupper((wint_t)(uint16_t)ARG(0))); }
HLE(crt_towlower) { RET((uint16_t)towlower((wint_t)(uint16_t)ARG(0))); }
HLE(crt__isctype) { int c = (int)(int32_t)ARG(0); RET(c >= -1 && c < 256 ? (g_ctype_table[c + 1] & (unsigned)ARG(1)) : 0); }
HLE(crt___pctype_func) { RET(g_cg->pctype); }
static void *data_pctype(void) { return &g_cg->pctype; }

// ---- math -------------------------------------------------------------------------------------

#define M1(name) HLE(crt_##name) { RETD(name(FARG(0))); }
#define M2(name) HLE(crt_##name) { RETD(name(FARG(0), FARG(1))); }
#define M1F(name) HLE(crt_##name) { RETF(name(FARGF(0))); }
#define M2F(name) HLE(crt_##name) { RETF(name(FARGF(0), FARGF(1))); }
M1(sin) M1(cos) M1(tan) M1(asin) M1(acos) M1(atan) M1(sinh) M1(cosh) M1(tanh) M1(asinh) M1(acosh) M1(atanh)
M1(exp) M1(exp2) M1(expm1) M1(log) M1(log10) M1(log2) M1(log1p) M1(sqrt) M1(cbrt) M1(fabs) M1(floor) M1(ceil)
M1(round) M1(trunc) M1(rint) M1(nearbyint) M1(erf) M1(erfc) M1(tgamma) M1(lgamma) M1(logb)
M2(atan2) M2(pow) M2(fmod) M2(hypot) M2(fmin) M2(fmax) M2(copysign) M2(remainder) M2(fdim) M2(nextafter)
M1F(sinf) M1F(cosf) M1F(tanf) M1F(asinf) M1F(acosf) M1F(atanf) M1F(expf) M1F(logf) M1F(log10f) M1F(log2f) M1F(sqrtf)
M1F(floorf) M1F(ceilf) M1F(roundf) M1F(truncf) M1F(fabsf) M1F(sinhf) M1F(coshf) M1F(tanhf) M1F(exp2f) M1F(cbrtf)
M2F(atan2f) M2F(powf) M2F(fmodf) M2F(hypotf) M2F(fminf) M2F(fmaxf) M2F(copysignf)
HLE(crt_ldexp) { RETD(ldexp(FARG(0), (int)ARGS(1, 2))); }
HLE(crt_ldexpf) { RETF(ldexpf(FARGF(0), (int)ARG(1))); }
HLE(crt_frexp) { int e; double m = frexp(FARG(0), &e); mem_wr(ARGS(1, 2), 4, (uint32_t)e); RETD(m); }
HLE(crt_modf) { double ip; double f = modf(FARG(0), &ip); memcpy(G2H(ARGS(1, 2)), &ip, 8); RETD(f); }
HLE(crt_modff) { float ip; float f = modff(FARGF(0), &ip); memcpy(PTR(1), &ip, 4); RETF(f); }
HLE(crt_fma) { RETD(fma(FARG(0), FARG(1), FARG(2))); }
HLE(crt_lround) { RET((int64_t)(int32_t)lround(FARG(0))); }
HLE(crt_llround) { RET((int64_t)llround(FARG(0))); }
HLE(crt_lrint) { RET((int64_t)(int32_t)lrint(FARG(0))); }
HLE(crt_llrint) { RET((int64_t)llrint(FARG(0))); }
HLE(crt_ilogb) { RET((int64_t)ilogb(FARG(0))); }
HLE(crt_scalbn) { RETD(scalbn(FARG(0), (int)ARGS(1, 2))); }
HLE(crt__finite) { RET(isfinite(FARG(0)) ? 1 : 0); }
HLE(crt__isnan) { RET(isnan(FARG(0)) ? 1 : 0); }
HLE(crt__fpclass) {
    double v = FARG(0);
    int c = fpclassify(v), neg = signbit(v) != 0;
    uint64_t r;
    switch (c) {
    case FP_NAN: r = 2; break;
    case FP_INFINITE: r = neg ? 4 : 0x200; break;
    case FP_ZERO: r = neg ? 0x20 : 0x40; break;
    case FP_SUBNORMAL: r = neg ? 0x10 : 0x80; break;
    default: r = neg ? 8 : 0x100; break;
    }
    RET(r);
}
HLE(crt__dclass) { int c = fpclassify(FARG(0)); RET(c == FP_NAN ? 2 : c == FP_INFINITE ? 1 : c == FP_ZERO ? 0 : c == FP_SUBNORMAL ? (uint64_t)-2 : (uint64_t)-1); }
HLE(crt__dsign) { RET(signbit(FARG(0)) ? 0x8000 : 0); }
HLE(crt_abs) { int32_t v = (int32_t)ARG(0); RET((uint32_t)(v < 0 ? -v : v)); }
HLE(crt_llabs) { int64_t v = (int64_t)ARG64(0, 0); RET((uint64_t)(v < 0 ? -v : v)); }
HLE(crt_div) { int32_t a = (int32_t)ARG(0), b = (int32_t)ARG(1); RET(((uint64_t)(uint32_t)(a % b) << 32) | (uint32_t)(a / b)); }
HLE(crt_ldiv) { int32_t a = (int32_t)ARG(0), b = (int32_t)ARG(1); RET(((uint64_t)(uint32_t)(a % b) << 32) | (uint32_t)(a / b)); }
HLE(crt_lldiv) { int64_t a = (int64_t)ARG64(1, 1), b = (int64_t)ARG64(2, 3); mem_wr(ARG(0), 8, (uint64_t)(a / b)); mem_wr(ARG(0) + 8, 8, (uint64_t)(a % b)); RET(ARG(0)); }
HLE(crt__controlfp) { RET(0x0008001F); }
HLE(crt__controlfp_s) { if (ARG(0)) mem_wr(ARG(0), 4, 0x0008001F); RET(0); }
HLE_STUB_RET(crt__clearfp, 0)
HLE_STUB_RET(crt__statusfp, 0)
HLE_STUB_RET(crt__fpreset, 0)
HLE_STUB_RET(crt___setusermatherr, 0)
HLE_STUB_RET(crt__set_SSE2_enable, 1)

// ---- stdlib -----------------------------------------------------------------------------------

static void push_atexit(uint64_t fn) {
    if (g_atexit_n == g_atexit_cap) { g_atexit_cap = g_atexit_cap ? g_atexit_cap * 2 : 32; g_atexit = realloc(g_atexit, sizeof(uint64_t) * (size_t)g_atexit_cap); }
    g_atexit[g_atexit_n++] = fn;
}
void crt_run_atexit(X86State *st) {
    while (g_atexit_n > 0) {
        uint64_t fn = g_atexit[--g_atexit_n];
        if (fn) soul_guest_call(st, fn, 0, NULL);
    }
    crt_flush_all();
}
HLE(crt_atexit) { push_atexit(ARG(0)); RET(0); }
HLE(crt__onexit) { push_atexit(ARG(0)); RET(ARG(0)); }
HLE(crt___dllonexit) { push_atexit(ARG(0)); RET(ARG(0)); }
HLE(crt_exit) { int code = (int)ARG(0); crt_run_atexit(st); soul_exit_process(st, code); }
HLE(crt__exit) { crt_flush_all(); soul_exit_process(st, (int)ARG(0)); }
HLE(crt__cexit) { crt_run_atexit(st); RET(0); }
HLE_STUB_RET(crt__c_exit, 0)
HLE(crt_abort) { con_write(2, "\nabnormal program termination\n", 31); crt_flush_all(); soul_exit_process(st, 3); }
HLE(crt__amsg_exit) { soul_fatal(st, "C çalışma zamanı hatası R60%02d", (int)ARG(0)); }
HLE(crt_terminate) { soul_fatal(st, "terminate() çağrıldı (yakalanmamış C++ istisnası?)"); }
HLE(crt__purecall) { soul_fatal(st, "Saf sanal işlev çağrısı (_purecall)"); }
HLE(crt__invalid_parameter_noinfo) { if (g_proc->verbose) soul_log("[crt] geçersiz parametre\n"); RET(0); }
HLE(crt__invalid_parameter_noinfo_noreturn) { soul_fatal(st, "CRT geçersiz parametre (noreturn)"); }
HLE(crt__invoke_watson) { soul_fatal(st, "CRT _invoke_watson çağrıldı"); }
HLE(crt__CxxThrowException) { soul_fatal(st, "Program bir C++ istisnası fırlattı; C++ istisnaları henüz desteklenmiyor"); }
HLE_STUB_RET(crt___C_specific_handler, 1)
HLE_STUB_RET(crt___CxxFrameHandler, 1)
HLE(crt___current_exception) { RET(&g_current_exception[0]); }
HLE(crt___current_exception_context) { RET(&g_current_exception[1]); }
HLE_STUB_RET(crt___std_type_info_destroy_list, 0)
HLE(crt___std_exception_copy) {
    uint64_t from = ARG(0), to = ARG(1);
    uint64_t what = rd_ptr(from);
    if (mem_rd(from + PSZ, 1) && what) { wr_ptr(to, H2G(g_strdup((const char *)G2H(what)))); mem_wr(to + PSZ, 1, 1); }
    else { wr_ptr(to, what); mem_wr(to + PSZ, 1, 0); }
    RET(0);
}
HLE(crt___std_exception_destroy) { uint64_t p = ARG(0); if (mem_rd(p + PSZ, 1)) g_free(G2H(rd_ptr(p))); wr_ptr(p, 0); mem_wr(p + PSZ, 1, 0); RET(0); }
HLE(crt___std_type_info_compare) { RET((int64_t)strcmp((const char *)G2H(ARG(0) + 2 * PSZ + 1), (const char *)G2H(ARG(1) + 2 * PSZ + 1))); }
HLE(crt___std_type_info_name) { RET(ARG(0) + 2 * PSZ); }
HLE(crt___std_type_info_hash) { const unsigned char *s = (const unsigned char *)G2H(ARG(0) + 2 * PSZ + 1); uint64_t h = 14695981039346656037ull; while (*s) { h ^= *s++; h *= 1099511628211ull; } RET(h); }

static void sort_swap(uint8_t *a, uint8_t *b, size_t w) { while (w--) { uint8_t t = *a; *a++ = *b; *b++ = t; } }
static int sort_cmp(X86State *st, uint64_t fn, uint64_t ctx, int has_ctx, uint8_t *a, uint8_t *b) {
    uint64_t args[3];
    int n;
    if (has_ctx) { args[0] = ctx; args[1] = H2G(a); args[2] = H2G(b); n = 3; }
    else { args[0] = H2G(a); args[1] = H2G(b); n = 2; }
    return (int)(int32_t)soul_guest_call(st, fn, n, args);
}
static void guest_qsort(X86State *st, uint8_t *base, size_t n, size_t w, uint64_t fn, uint64_t ctx, int has_ctx) {
    while (n > 1) {
        if (n < 12) { // insertion sort
            for (size_t i = 1; i < n; i++)
                for (size_t j = i; j > 0 && sort_cmp(st, fn, ctx, has_ctx, base + (j - 1) * w, base + j * w) > 0; j--)
                    sort_swap(base + (j - 1) * w, base + j * w, w);
            return;
        }
        size_t mid = n / 2;
        uint8_t *lo = base, *m = base + mid * w, *hi = base + (n - 1) * w;
        if (sort_cmp(st, fn, ctx, has_ctx, m, lo) < 0) sort_swap(m, lo, w);
        if (sort_cmp(st, fn, ctx, has_ctx, hi, m) < 0) { sort_swap(hi, m, w); if (sort_cmp(st, fn, ctx, has_ctx, m, lo) < 0) sort_swap(m, lo, w); }
        sort_swap(m, hi - w, w); // pivot at n-2
        uint8_t *pivot = hi - w;
        size_t i = 0, j = n - 2;
        for (;;) {
            while (sort_cmp(st, fn, ctx, has_ctx, base + (++i) * w, pivot) < 0) {}
            while (j > 0 && sort_cmp(st, fn, ctx, has_ctx, base + (--j) * w, pivot) > 0) {}
            if (i >= j) break;
            sort_swap(base + i * w, base + j * w, w);
        }
        sort_swap(base + i * w, pivot, w);
        // recurse into smaller part
        if (i < n - i - 1) { guest_qsort(st, base, i, w, fn, ctx, has_ctx); base += (i + 1) * w; n -= i + 1; }
        else { guest_qsort(st, base + (i + 1) * w, n - i - 1, w, fn, ctx, has_ctx); n = i; }
    }
}
HLE(crt_qsort) { guest_qsort(st, PTR(0), ARG(1), ARG(2), ARG(3), 0, 0); RET(0); }
HLE(crt_qsort_s) { guest_qsort(st, PTR(0), ARG(1), ARG(2), ARG(3), ARG(4), 1); RET(0); }
HLE(crt_bsearch) {
    uint64_t key = ARG(0);
    uint8_t *base = PTR(1);
    size_t n = ARG(2), w = ARG(3);
    uint64_t fn = ARG(4);
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint64_t args[2] = { key, H2G(base + mid * w) };
        int c = (int)(int32_t)soul_guest_call(st, fn, 2, args);
        if (c == 0) { RET(base + mid * w); return; }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    RET(0);
}
HLE(crt_rand) { g_rand_state = g_rand_state * 214013u + 2531011u; RET((g_rand_state >> 16) & 0x7FFF); }
HLE(crt_srand) { g_rand_state = (uint32_t)ARG(0); RET(0); }
HLE(crt_rand_s) { uint32_t r = (uint32_t)random() ^ ((uint32_t)random() << 16); mem_wr(ARG(0), 4, r); RET(0); }

static const char *env_lookup(const char *name) {
    HleState *h = hs();
    size_t n = strlen(name);
    for (int i = 0; i < h->env_count; i++)
        if (strncasecmp(h->env[i], name, n) == 0 && h->env[i][n] == '=') return h->env[i] + n + 1;
    return NULL;
}
static void env_set(const char *name, const char *value) {
    HleState *h = hs();
    size_t n = strlen(name);
    for (int i = 0; i < h->env_count; i++) {
        if (strncasecmp(h->env[i], name, n) == 0 && h->env[i][n] == '=') {
            g_free(h->env[i]);
            if (value && *value) {
                h->env[i] = g_malloc(n + strlen(value) + 2);
                sprintf(h->env[i], "%s=%s", name, value);
            } else {
                memmove(&h->env[i], &h->env[i + 1], sizeof(char *) * (size_t)(h->env_count - i));
                h->env_count--;
            }
            return;
        }
    }
    if (!value || !*value) return;
    h->env = realloc(h->env, sizeof(char *) * (size_t)(h->env_count + 2));
    h->env[h->env_count] = g_malloc(n + strlen(value) + 2);
    sprintf(h->env[h->env_count], "%s=%s", name, value);
    h->env[++h->env_count] = NULL;
}
const char *env_get_value(const char *name) { return env_lookup(name); }
void env_set_value(const char *name, const char *value) { env_set(name, value); }
HLE(crt_getenv) { RET(env_lookup(CSTR(0))); }
HLE(crt__wgetenv) {
    char *n = w2u_dup(WSTR(0));
    const char *v = env_lookup(n);
    free(n);
    RET(v ? u2w_gdup(v) : NULL); // leaked intentionally (rare)
}
HLE(crt_getenv_s) {
    const char *v = env_lookup(CSTR(3));
    size_t need = v ? strlen(v) + 1 : 0;
    if (ARG(0)) wr_ptr(ARG(0), need);
    if (!v) { if (ARG(1) && ARG(2)) *(char *)PTR(1) = 0; RET(0); return; }
    if (ARG(2) < need) { RET(ERANGE); return; }
    memcpy(PTR(1), v, need);
    RET(0);
}
HLE(crt__dupenv_s) {
    const char *v = env_lookup(CSTR(2));
    wr_ptr(ARG(0), v ? H2G(g_strdup(v)) : 0);
    if (ARG(1)) wr_ptr(ARG(1), v ? strlen(v) + 1 : 0);
    RET(0);
}
HLE(crt__putenv) {
    char *kv = strdup(CSTR(0));
    char *eq = strchr(kv, '=');
    if (eq) { *eq = 0; env_set(kv, eq + 1); }
    free(kv);
    RET(eq ? 0 : (uint64_t)-1);
}
HLE(crt__putenv_s) { env_set(CSTR(0), CSTR(1)); RET(0); }
HLE(crt_system) {
    if (!ARG(0)) { RET(0); return; }
    const char *cmd = CSTR(0);
    if (strcasecmp(cmd, "pause") == 0) {
        con_write(1, "Devam etmek için Enter'a basın . . . ", 37);
        char b[64];
        con_read(b, sizeof b, 1);
        RET(0);
        return;
    }
    if (strcasecmp(cmd, "cls") == 0) { con_write(1, "\x1b[2J\x1b[H", 7); RET(0); return; }
    soul_log("[crt] system(\"%s\") desteklenmiyor\n", cmd);
    g_errno = ENOENT;
    RET(-1);
}
HLE(crt__errno) { RET(&g_errno); }
HLE(crt___doserrno) { RET(&g_doserrno); }
HLE(crt__get_errno) { mem_wr(ARG(0), 4, (uint32_t)g_errno); RET(0); }
HLE(crt__set_errno) { g_errno = (int)ARG(0); RET(0); }
HLE(crt_signal) { RET(0); }
HLE(crt_raise) { if ((int)ARG(0) == 22) soul_exit_process(st, 3); RET(0); }
HLE(crt_setlocale) { RET(g_cg->locale_c); }
HLE(crt__wsetlocale) { RET(g_cg->wlocale_c); }
HLE(crt_localeconv) { RET(g_lconv); }
HLE(crt____lc_codepage_func) { RET(65001); }
HLE(crt____mb_cur_max_func) { RET((uint64_t)g_mb_cur_max); }
HLE(crt___lc_locale_name_func) { RET(g_cg->lc_names); }
HLE_STUB_RET(crt__configthreadlocale, 2)
HLE(crt__create_locale) { RET(g_cg->locale_obj); }
HLE_STUB_RET(crt__free_locale, 0)
HLE(crt__get_current_locale) { RET(g_cg->locale_obj); }
HLE(crt__getpid) { RET(4242); }
uint64_t k32_create_thread(X86State *st, uint64_t stack, uint64_t fn, uint64_t arg, uint32_t flags, uint64_t tid_out);
// _beginthreadex(security, stack, start, arg, initflag, thrdaddr)
HLE(crt__beginthreadex) {
    uint64_t h = k32_create_thread(st, ARG(1), ARG(2), ARG(3), (uint32_t)hle_arg(st, 4), hle_arg(st, 5));
    if (!h) g_errno = EAGAIN;
    RET(h);
}
// _beginthread(start, stack, arg): the start routine returns void
HLE(crt__beginthread) {
    uint64_t h = k32_create_thread(st, ARG(1), ARG(0), ARG(2), 0, 0);
    RET(h ? h : (uint64_t)-1);
}
HLE(crt__endthreadex) { soul_exit_thread(st, (uint32_t)ARG(0)); }
HLE(crt__endthread) { soul_exit_thread(st, 0); }
HLE(crt__Thrd_yield) { soul_yield(); RET(0); }
HLE(crt__sleep) {
    uint32_t ms = (uint32_t)ARG(0);
    double deadline = soul_now() + ms / 1000.0;
    if (!ms) soul_yield();
    while (ms && !g_proc->terminate && soul_now() < deadline) gil_wait((int64_t)((deadline - soul_now()) * 1000.0) + 1);
    RET(0);
}

// setjmp / longjmp. 32-bit _JUMP_BUFFER: Ebp, Ebx, Edi, Esi, Esp, Eip, Registration, TryLevel, ...
static void setjmp32(X86State *st) {
    uint64_t b = ARG(0);
    mem_wr(b + 0, 4, st->gpr[RBP]);
    mem_wr(b + 4, 4, st->gpr[RBX]);
    mem_wr(b + 8, 4, st->gpr[RDI]);
    mem_wr(b + 12, 4, st->gpr[RSI]);
    mem_wr(b + 16, 4, (uint32_t)(st->gpr[RSP] + 4)); // ESP after returning to the caller
    mem_wr(b + 20, 4, mem_rd(st->gpr[RSP], 4));      // return address
    mem_wr(b + 24, 4, mem_rd(teb_of(st), 4));        // SEH registration
    mem_wr(b + 28, 4, 0xFFFFFFFFu);
    mem_wr(b + 32, 4, 0x56433230u);                  // "VC20" cookie
    RET(0);
}
static void longjmp32(X86State *st) {
    uint64_t b = ARG(0);
    int v = (int)ARG(1);
    st->gpr[RBP] = mem_rd(b + 0, 4);
    st->gpr[RBX] = mem_rd(b + 4, 4);
    st->gpr[RDI] = mem_rd(b + 8, 4);
    st->gpr[RSI] = mem_rd(b + 12, 4);
    st->gpr[RSP] = mem_rd(b + 16, 4);
    st->rip = mem_rd(b + 20, 4);
    mem_wr(teb_of(st), 4, mem_rd(b + 24, 4));
    st->gpr[RAX] = (uint32_t)(v ? v : 1);
    st->thread->skip_ret = 1;
}
// Win64 _JUMP_BUFFER layout
HLE(crt__setjmp) {
    if (g_mode32) { setjmp32(st); return; }
    uint64_t b = ARG(0);
    uint64_t ret = mem_rd(st->gpr[RSP], 8);
    mem_wr(b + 0, 8, ARG(1));
    mem_wr(b + 8, 8, st->gpr[RBX]);
    mem_wr(b + 16, 8, st->gpr[RSP] + 8);
    mem_wr(b + 24, 8, st->gpr[RBP]);
    mem_wr(b + 32, 8, st->gpr[RSI]);
    mem_wr(b + 40, 8, st->gpr[RDI]);
    mem_wr(b + 48, 8, st->gpr[R12]);
    mem_wr(b + 56, 8, st->gpr[R13]);
    mem_wr(b + 64, 8, st->gpr[R14]);
    mem_wr(b + 72, 8, st->gpr[R15]);
    mem_wr(b + 80, 8, ret);
    mem_wr(b + 88, 4, st->mxcsr);
    for (int i = 0; i < 10; i++) memcpy(G2H(b + 96 + 16 * (uint64_t)i), &st->xmm[6 + i], 16);
    RET(0);
}
HLE(crt_longjmp) {
    if (g_mode32) { longjmp32(st); return; }
    uint64_t b = ARG(0);
    int v = (int)ARG(1);
    st->gpr[RBX] = mem_rd(b + 8, 8);
    st->gpr[RSP] = mem_rd(b + 16, 8);
    st->gpr[RBP] = mem_rd(b + 24, 8);
    st->gpr[RSI] = mem_rd(b + 32, 8);
    st->gpr[RDI] = mem_rd(b + 40, 8);
    st->gpr[R12] = mem_rd(b + 48, 8);
    st->gpr[R13] = mem_rd(b + 56, 8);
    st->gpr[R14] = mem_rd(b + 64, 8);
    st->gpr[R15] = mem_rd(b + 72, 8);
    st->rip = mem_rd(b + 80, 8);
    for (int i = 0; i < 10; i++) memcpy(&st->xmm[6 + i], G2H(b + 96 + 16 * (uint64_t)i), 16);
    st->gpr[RAX] = (uint32_t)(v ? v : 1);
    st->thread->skip_ret = 1;
}

// ---- time ---------------------------------------------------------------------------------

static void tm_to_guest(const struct tm *t) {
    g_tm.v[0] = t->tm_sec; g_tm.v[1] = t->tm_min; g_tm.v[2] = t->tm_hour; g_tm.v[3] = t->tm_mday;
    g_tm.v[4] = t->tm_mon; g_tm.v[5] = t->tm_year; g_tm.v[6] = t->tm_wday; g_tm.v[7] = t->tm_yday; g_tm.v[8] = t->tm_isdst > 0;
}
static void guest_to_tm(uint64_t p, struct tm *t) {
    memset(t, 0, sizeof *t);
    int32_t v[9];
    memcpy(v, G2H(p), sizeof v);
    t->tm_sec = v[0]; t->tm_min = v[1]; t->tm_hour = v[2]; t->tm_mday = v[3]; t->tm_mon = v[4];
    t->tm_year = v[5]; t->tm_wday = v[6]; t->tm_yday = v[7]; t->tm_isdst = v[8];
}
HLE(crt_time64) { time_t t = time(NULL); if (ARG(0)) mem_wr(ARG(0), 8, (uint64_t)t); RET(t); }
HLE(crt_time32) { time_t t = time(NULL); if (ARG(0)) mem_wr(ARG(0), 4, (uint64_t)t); RET((uint32_t)t); }
HLE(crt_clock) { RET((int64_t)((soul_now() - g_clock_start) * 1000.0)); }
HLE(crt_difftime) { RETD((double)((int64_t)ARG64(0, 0) - (int64_t)ARG64(1, 2))); }
HLE(crt_difftime32) { RETD((double)((int32_t)ARG(0) - (int32_t)ARG(1))); }
HLE(crt_localtime64) { time_t t = (time_t)(int64_t)mem_rd(ARG(0), 8); struct tm r; localtime_r(&t, &r); tm_to_guest(&r); RET(&g_tm); }
HLE(crt_gmtime64) { time_t t = (time_t)(int64_t)mem_rd(ARG(0), 8); struct tm r; gmtime_r(&t, &r); tm_to_guest(&r); RET(&g_tm); }
HLE(crt_localtime32) { time_t t = (time_t)(int32_t)mem_rd(ARG(0), 4); struct tm r; localtime_r(&t, &r); tm_to_guest(&r); RET(&g_tm); }
HLE(crt_localtime64_s) { time_t t = (time_t)(int64_t)mem_rd(ARG(1), 8); struct tm r; localtime_r(&t, &r); tm_to_guest(&r); memcpy(PTR(0), &g_tm, 36); RET(0); }
HLE(crt_gmtime64_s) { time_t t = (time_t)(int64_t)mem_rd(ARG(1), 8); struct tm r; gmtime_r(&t, &r); tm_to_guest(&r); memcpy(PTR(0), &g_tm, 36); RET(0); }
HLE(crt_mktime64) { struct tm t; guest_to_tm(ARG(0), &t); time_t r = mktime(&t); tm_to_guest(&t); memcpy(PTR(0), &g_tm, 36); RET((int64_t)r); }
HLE(crt_localtime_plain);
HLE(crt_gmtime_plain);
HLE(crt_mktime_plain);
HLE(crt_ctime_plain);
HLE(crt_difftime_plain);
HLE(crt_mkgmtime64) { struct tm t; guest_to_tm(ARG(0), &t); RET((int64_t)timegm(&t)); }
HLE(crt_strftime) { struct tm t; guest_to_tm(ARG(3), &t); RET(strftime(PTR(0), ARG(1), CSTR(2), &t)); }
HLE(crt_wcsftime) {
    struct tm t; guest_to_tm(ARG(3), &t);
    char *f = w2u_dup(WSTR(2));
    char buf[1024];
    size_t n = strftime(buf, sizeof buf, f, &t);
    free(f);
    size_t w = u2w(buf, (long)n, PTR(0), ARG(1));
    RET(w < ARG(1) ? w : 0);
}
HLE(crt_asctime) { struct tm t; guest_to_tm(ARG(0), &t); strftime(g_asctime_buf, sizeof g_asctime_buf, "%a %b %d %H:%M:%S %Y\n", &t); RET(g_asctime_buf); }
HLE(crt_ctime32) { time_t t = (time_t)(int32_t)mem_rd(ARG(0), 4); struct tm r; localtime_r(&t, &r); strftime(g_asctime_buf, sizeof g_asctime_buf, "%a %b %d %H:%M:%S %Y\n", &r); RET(g_asctime_buf); }
HLE(crt_gmtime32) { time_t t = (time_t)(int32_t)mem_rd(ARG(0), 4); struct tm r; gmtime_r(&t, &r); tm_to_guest(&r); RET(&g_tm); }
HLE(crt_mktime32) { struct tm t; guest_to_tm(ARG(0), &t); time_t r = mktime(&t); tm_to_guest(&t); memcpy(PTR(0), &g_tm, 36); RET((uint32_t)(int32_t)r); }
// msvcrt.dll: in 32-bit programs time(), localtime() & co. use a 32-bit time_t
HLE(crt_time_plain) { if (g_mode32) { time_t t = time(NULL); if (ARG(0)) mem_wr(ARG(0), 4, (uint64_t)t); RET((uint32_t)t); } else { time_t t = time(NULL); if (ARG(0)) mem_wr(ARG(0), 8, (uint64_t)t); RET(t); } }
HLE(crt_ctime64) { time_t t = (time_t)(int64_t)mem_rd(ARG(0), 8); struct tm r; localtime_r(&t, &r); strftime(g_asctime_buf, sizeof g_asctime_buf, "%a %b %d %H:%M:%S %Y\n", &r); RET(g_asctime_buf); }
HLE(crt_ftime64) {
    struct timeval tv; gettimeofday(&tv, NULL);
    uint8_t *d = PTR(0);
    int64_t s = tv.tv_sec; uint16_t ms = (uint16_t)(tv.tv_usec / 1000); int16_t tz = 0, dst = 0;
    memcpy(d, &s, 8); memcpy(d + 8, &ms, 2); memcpy(d + 10, &tz, 2); memcpy(d + 12, &dst, 2);
    RET(0);
}
HLE(crt_localtime_plain) { if (g_mode32) crt_localtime32(st); else crt_localtime64(st); }
HLE(crt_gmtime_plain) { if (g_mode32) crt_gmtime32(st); else crt_gmtime64(st); }
HLE(crt_mktime_plain) { if (g_mode32) crt_mktime32(st); else crt_mktime64(st); }
HLE(crt_ctime_plain) { if (g_mode32) crt_ctime32(st); else crt_ctime64(st); }
HLE(crt_difftime_plain) { if (g_mode32) crt_difftime32(st); else crt_difftime(st); }
HLE(crt__strdate) { time_t t = time(NULL); struct tm r; localtime_r(&t, &r); strftime(PTR(0), 9, "%m/%d/%y", &r); RET(ARG(0)); }
HLE(crt__strtime) { time_t t = time(NULL); struct tm r; localtime_r(&t, &r); strftime(PTR(0), 9, "%H:%M:%S", &r); RET(ARG(0)); }
HLE(crt__tzset) { RET(0); }
static void *data_timezone(void) { return &g_timezone; }
static void *data_daylight(void) { return &g_daylight; }
HLE(crt___timezone) { RET(&g_timezone); }
HLE(crt___daylight) { RET(&g_daylight); }

// ---- startup ------------------------------------------------------------------------------

HLE(crt___getmainargs) {
    build_args();
    mem_wr(ARG(0), 4, (uint32_t)g_argc);
    wr_ptr(ARG(1), g_cg->argv);
    wr_ptr(ARG(2), g_cg->environ_v);
    RET(0);
}
HLE(crt___wgetmainargs) {
    build_args();
    mem_wr(ARG(0), 4, (uint32_t)g_argc);
    wr_ptr(ARG(1), g_cg->wargv);
    wr_ptr(ARG(2), g_cg->wenviron);
    RET(0);
}
HLE(crt___p___argc) { build_args(); RET(&g_argc); }
HLE(crt___p___argv) { build_args(); RET(&g_cg->argv); }
HLE(crt___p___wargv) { build_args(); RET(&g_cg->wargv); }
HLE(crt___p__environ) { build_args(); RET(&g_cg->environ_v); }
HLE(crt___p__wenviron) { build_args(); RET(&g_cg->wenviron); }
HLE(crt___p__acmdln) { build_args(); RET(&g_cg->acmdln); }
HLE(crt___p__wcmdln) { build_args(); RET(&g_cg->wcmdln); }
HLE(crt___p__pgmptr) { build_args(); RET(&g_cg->pgmptr); }
HLE(crt___p__wpgmptr) { build_args(); RET(&g_cg->wpgmptr); }
HLE(crt__get_pgmptr) { build_args(); wr_ptr(ARG(0), g_cg->pgmptr); RET(0); }
HLE(crt__get_wpgmptr) { build_args(); wr_ptr(ARG(0), g_cg->wpgmptr); RET(0); }
HLE(crt___p__fmode) { RET(&g_fmode); }
HLE(crt___p__commode) { RET(&g_commode); }
HLE(crt__set_fmode) { g_fmode = (int)ARG(0); RET(0); }
HLE(crt__get_fmode) { mem_wr(ARG(0), 4, (uint32_t)g_fmode); RET(0); }
HLE(crt__get_initial_narrow_environment) { build_args(); RET(g_cg->environ_v); }
HLE(crt__get_initial_wide_environment) { build_args(); RET(g_cg->wenviron); }
HLE(crt__configure_narrow_argv) { build_args(); RET(0); }
HLE(crt__initialize_narrow_environment) { build_args(); RET(0); }
HLE_STUB_RET(crt__set_app_type, 0)
HLE_STUB_RET(crt__set_new_mode, 0)
HLE_STUB_RET(crt__seh_filter_exe, 0)
HLE_STUB_RET(crt__register_thread_local_exe_atexit_callback, 0)
HLE_STUB_RET(crt__lock, 0)
HLE_STUB_RET(crt__set_invalid_parameter_handler, 0)
HLE_STUB_RET(crt__set_abort_behavior, 0)
HLE_STUB_RET(crt__set_purecall_handler, 0)
HLE_STUB_RET(crt__crt_debugger_hook, 0)
HLE(crt__initterm) {
    for (uint64_t p = ARG(0); p < ARG(1); p += (uint64_t)PSZ) {
        uint64_t fn = rd_ptr(p);
        if (fn) soul_guest_call(st, fn, 0, NULL);
    }
    RET(0);
}
HLE(crt__initterm_e) {
    for (uint64_t p = ARG(0); p < ARG(1); p += (uint64_t)PSZ) {
        uint64_t fn = rd_ptr(p);
        if (fn) {
            uint64_t r = soul_guest_call(st, fn, 0, NULL) & 0xFFFFFFFF;
            if (r) { RET(r); return; }
        }
    }
    RET(0);
}
// UCRT onexit tables: struct { void **first, **last, **end; } (pointer-sized fields)
HLE(crt__initialize_onexit_table) { uint64_t t = ARG(0); wr_ptr(t, 0); wr_ptr(t + PSZ, 0); wr_ptr(t + 2 * PSZ, 0); RET(0); }
HLE(crt__register_onexit_function) {
    uint64_t t = ARG(0);
    const uint64_t ps = (uint64_t)PSZ;
    uint64_t first = rd_ptr(t), last = rd_ptr(t + ps), end = rd_ptr(t + 2 * ps);
    if (last == end) {
        size_t n = first ? (end - first) / ps : 0;
        size_t nn = n ? n * 2 : 32;
        uint64_t nf = H2G(g_realloc(G2H(first), nn * ps));
        last = nf + (last - first);
        first = nf;
        end = nf + nn * ps;
    }
    wr_ptr(last, ARG(1));
    last += ps;
    wr_ptr(t, first); wr_ptr(t + ps, last); wr_ptr(t + 2 * ps, end);
    RET(0);
}
HLE(crt__execute_onexit_table) {
    uint64_t t = ARG(0);
    const uint64_t ps = (uint64_t)PSZ;
    uint64_t first = rd_ptr(t), last = rd_ptr(t + ps);
    wr_ptr(t, 0); wr_ptr(t + ps, 0); wr_ptr(t + 2 * ps, 0);
    while (last > first) {
        last -= ps;
        uint64_t fn = rd_ptr(last);
        if (fn) soul_guest_call(st, fn, 0, NULL);
    }
    g_free(G2H(first));
    RET(0);
}
HLE(crt__crt_atexit) { push_atexit(ARG(0)); RET(0); }
HLE(crt__get_stream_buffer_pointers) { RET(0); }

static void *data_fmode(void) { return &g_fmode; }
static void *data_commode(void) { return &g_commode; }
static void *data_argc(void) { build_args(); return &g_argc; }
static void *data_argv(void) { build_args(); return &g_cg->argv; }
static void *data_wargv(void) { build_args(); return &g_cg->wargv; }
static void *data_environ(void) { build_args(); return &g_cg->environ_v; }
static void *data_wenviron(void) { build_args(); return &g_cg->wenviron; }
static void *data_acmdln(void) { build_args(); return &g_cg->acmdln; }
static void *data_wcmdln(void) { build_args(); return &g_cg->wcmdln; }
static void *data_mb_cur_max(void) { return &g_mb_cur_max; }
static void *data_errno(void) { return &g_errno; }

void crt_process_end(void) { free_args(); }

const HleEntry hle_crt_table[] = {
    // stdio output
    { "printf", crt_printf, NULL }, { "wprintf", crt_wprintf, NULL }, { "vprintf", crt_vprintf, NULL }, { "vwprintf", crt_vwprintf, NULL },
    { "fprintf", crt_fprintf, NULL }, { "fwprintf", crt_fwprintf, NULL }, { "vfprintf", crt_vfprintf, NULL }, { "vfwprintf", crt_vfwprintf, NULL },
    { "sprintf", crt_sprintf, NULL }, { "vsprintf", crt_vsprintf, NULL }, { "snprintf", crt_snprintf, NULL }, { "vsnprintf", crt_vsnprintf, NULL },
    { "_snprintf", crt__snprintf, NULL }, { "_vsnprintf", crt__vsnprintf, NULL }, { "sprintf_s", crt_sprintf_s, NULL }, { "vsprintf_s", crt_vsprintf_s, NULL },
    { "_snprintf_s", crt__snprintf_s, NULL }, { "_vsnprintf_s", crt__vsnprintf_s, NULL }, { "vsnprintf_s", crt__vsnprintf_s, NULL },
    { "swprintf", crt_swprintf_legacy, NULL }, { "vswprintf", crt_vswprintf_legacy, NULL }, { "_swprintf", crt_swprintf_legacy, NULL },
    { "_snwprintf", crt__snwprintf, NULL }, { "_vsnwprintf", crt__vsnwprintf, NULL }, { "swprintf_s", crt_swprintf_s, NULL },
    { "_scprintf", crt__scprintf, NULL }, { "_vscprintf", crt__vscprintf, NULL }, { "_vscwprintf", crt__vscwprintf, NULL },
    { "__stdio_common_vfprintf", ucrt_vfprintf, NULL }, { "__stdio_common_vfwprintf", ucrt_vfwprintf, NULL },
    { "__stdio_common_vsprintf", ucrt_vsprintf, NULL }, { "__stdio_common_vswprintf", ucrt_vswprintf, NULL },
    { "__stdio_common_vsprintf_s", ucrt_vsprintf_s, NULL }, { "__stdio_common_vswprintf_s", ucrt_vswprintf_s, NULL },
    { "__stdio_common_vsnprintf_s", ucrt_vsnprintf_s, NULL }, { "__stdio_common_vsnwprintf_s", ucrt_vsnwprintf_s, NULL },
    { "__stdio_common_vfprintf_s", ucrt_vfprintf, NULL }, { "__stdio_common_vfprintf_p", ucrt_vfprintf, NULL },
    // stdio input
    { "scanf", crt_scanf, NULL }, { "scanf_s", crt_scanf_s, NULL }, { "fscanf", crt_fscanf, NULL }, { "fscanf_s", crt_fscanf_s, NULL },
    { "sscanf", crt_sscanf, NULL }, { "sscanf_s", crt_sscanf_s, NULL }, { "vscanf", crt_vscanf, NULL }, { "vsscanf", crt_vsscanf, NULL },
    { "vfscanf", crt_vfscanf, NULL }, { "swscanf", crt_swscanf, NULL },
    { "__stdio_common_vsscanf", ucrt_vsscanf, NULL }, { "__stdio_common_vswscanf", ucrt_vswscanf, NULL }, { "__stdio_common_vfscanf", ucrt_vfscanf, NULL },
    // stdio
    { "__iob_func", crt___iob_func, NULL }, { "__acrt_iob_func", crt___acrt_iob_func, NULL }, { "_iob", NULL, data_iob },
    { "puts", crt_puts, NULL }, { "_putws", crt__putws, NULL }, { "fputs", crt_fputs, NULL }, { "fputws", crt_fputws, NULL },
    { "fputc", crt_fputc, NULL }, { "putc", crt_fputc, NULL }, { "fputwc", crt_fputwc, NULL }, { "putwc", crt_fputwc, NULL },
    { "putchar", crt_putchar, NULL }, { "_putch", crt_putchar, NULL }, { "putwchar", crt_putwchar, NULL }, { "_putwch", crt_putwchar, NULL },
    { "fwrite", crt_fwrite, NULL }, { "fread", crt_fread, NULL }, { "fgetc", crt_fgetc, NULL }, { "getc", crt_fgetc, NULL },
    { "_fgetc_nolock", crt_fgetc, NULL }, { "_fputc_nolock", crt_fputc, NULL }, { "_fwrite_nolock", crt_fwrite, NULL }, { "_fread_nolock", crt_fread, NULL },
    { "getchar", crt_getchar, NULL }, { "fgetwc", crt_fgetwc, NULL }, { "getwc", crt_fgetwc, NULL },
    { "_getch", crt__getch, NULL }, { "_getche", crt__getch, NULL }, { "_getwch", crt__getch, NULL }, { "_kbhit", crt__kbhit, NULL },
    { "ungetc", crt_ungetc, NULL }, { "ungetwc", crt_ungetc, NULL }, { "fgets", crt_fgets, NULL }, { "fgetws", crt_fgetws, NULL }, { "gets", crt_gets, NULL },
    { "fopen", crt_fopen, NULL }, { "_wfopen", crt__wfopen, NULL }, { "fopen_s", crt_fopen_s, NULL }, { "_wfopen_s", crt__wfopen_s, NULL },
    { "_fsopen", crt__fsopen, NULL }, { "freopen", crt_freopen, NULL }, { "fclose", crt_fclose, NULL }, { "fflush", crt_fflush, NULL },
    { "_flushall", crt__flushall, NULL }, { "fseek", crt_fseek, NULL }, { "_fseeki64", crt__fseeki64, NULL }, { "ftell", crt_ftell, NULL },
    { "_ftelli64", crt__ftelli64, NULL }, { "rewind", crt_rewind, NULL }, { "fgetpos", crt_fgetpos, NULL }, { "fsetpos", crt_fsetpos, NULL },
    { "feof", crt_feof, NULL }, { "ferror", crt_ferror, NULL }, { "clearerr", crt_clearerr, NULL }, { "setvbuf", crt_setvbuf, NULL },
    { "setbuf", crt_setbuf, NULL }, { "_fileno", crt__fileno, NULL }, { "fileno", crt__fileno, NULL }, { "_isatty", crt__isatty, NULL },
    { "_setmode", crt__setmode, NULL }, { "tmpfile", crt_tmpfile, NULL }, { "remove", crt_remove, NULL }, { "_unlink", crt_remove, NULL },
    { "_wremove", crt__wremove, NULL }, { "_wunlink", crt__wremove, NULL }, { "rename", crt_rename, NULL }, { "_access", crt__access, NULL },
    { "_waccess", crt__waccess, NULL }, { "_mkdir", crt__mkdir, NULL }, { "_wmkdir", crt__wmkdir, NULL }, { "_rmdir", crt__rmdir, NULL },
    { "_chdir", crt__chdir, NULL }, { "_getcwd", crt__getcwd, NULL }, { "_wgetcwd", crt__wgetcwd, NULL }, { "_fullpath", crt__fullpath, NULL },
    { "_stat64", crt__stat64, NULL }, { "_stat64i32", crt__stat64i32, NULL }, { "_stat", crt__stat_plain, NULL }, { "_stat32", crt__stat32, NULL }, { "_stati64", crt__stat64, NULL },
    { "_wstat64", crt__wstat64, NULL }, { "_wstat64i32", crt__wstat64i32, NULL }, { "_wstat", crt__wstat_plain, NULL },
    { "_fstat64", crt__fstat64, NULL }, { "_fstat64i32", crt__fstat64i32, NULL }, { "_fstat", crt__fstat_plain, NULL }, { "_fstat32", crt__fstat32, NULL },
    { "_open", crt__open, NULL }, { "_close", crt__close, NULL }, { "_read", crt__read, NULL }, { "_write", crt__write, NULL },
    { "_lseek", crt__lseek, NULL }, { "_lseeki64", crt__lseeki64, NULL }, { "_filelength", crt__filelength, NULL },
    { "_get_osfhandle", crt__get_osfhandle, NULL }, { "perror", crt_perror, NULL },
    // memory
    { "malloc", crt_malloc, NULL }, { "calloc", crt_calloc, NULL }, { "realloc", crt_realloc, NULL }, { "free", crt_free, NULL },
    { "_msize", crt__msize, NULL }, { "_recalloc", crt__recalloc, NULL }, { "_aligned_malloc", crt__aligned_malloc, NULL },
    { "_aligned_free", crt__aligned_free, NULL }, { "_aligned_realloc", crt__aligned_realloc, NULL }, { "_expand", crt__expand, NULL },
    { "_callnewh", crt__callnewh, NULL }, { "_free_base", crt_free, NULL }, { "_malloc_base", crt_malloc, NULL }, { "_calloc_base", crt_calloc, NULL }, { "_realloc_base", crt_realloc, NULL },
    // strings
    { "strlen", crt_strlen, NULL }, { "strnlen", crt_strnlen, NULL }, { "strcpy", crt_strcpy, NULL }, { "strncpy", crt_strncpy, NULL },
    { "strcat", crt_strcat, NULL }, { "strncat", crt_strncat, NULL }, { "strcmp", crt_strcmp, NULL }, { "strncmp", crt_strncmp, NULL },
    { "_stricmp", crt__stricmp, NULL }, { "_strcmpi", crt__stricmp, NULL }, { "stricmp", crt__stricmp, NULL }, { "_stricoll", crt__stricmp, NULL },
    { "_strnicmp", crt__strnicmp, NULL }, { "strnicmp", crt__strnicmp, NULL }, { "strchr", crt_strchr, NULL }, { "strrchr", crt_strrchr, NULL },
    { "strstr", crt_strstr, NULL }, { "strpbrk", crt_strpbrk, NULL }, { "strspn", crt_strspn, NULL }, { "strcspn", crt_strcspn, NULL },
    { "_strdup", crt__strdup, NULL }, { "strdup", crt__strdup, NULL }, { "strtok", crt_strtok, NULL }, { "strtok_s", crt_strtok_s, NULL },
    { "_strlwr", crt__strlwr, NULL }, { "_strupr", crt__strupr, NULL }, { "_strrev", crt__strrev, NULL }, { "strerror", crt_strerror, NULL },
    { "strerror_s", crt_strerror_s, NULL }, { "strcoll", crt_strcoll, NULL }, { "strxfrm", crt_strxfrm, NULL },
    { "strcpy_s", crt_strcpy_s, NULL }, { "strncpy_s", crt_strncpy_s, NULL }, { "strcat_s", crt_strcat_s, NULL }, { "strncat_s", crt_strncat_s, NULL },
    { "memcpy", crt_memcpy, NULL }, { "memmove", crt_memcpy, NULL }, { "memset", crt_memset, NULL }, { "memcmp", crt_memcmp, NULL },
    { "memchr", crt_memchr, NULL }, { "memcpy_s", crt_memcpy_s, NULL }, { "memmove_s", crt_memcpy_s, NULL },
    { "wcslen", crt_wcslen, NULL }, { "wcsnlen", crt_wcsnlen, NULL }, { "wcscpy", crt_wcscpy, NULL }, { "wcsncpy", crt_wcsncpy, NULL },
    { "wcscat", crt_wcscat, NULL }, { "wcsncat", crt_wcsncat, NULL }, { "wcscmp", crt_wcscmp, NULL }, { "wcsncmp", crt_wcsncmp, NULL },
    { "_wcsicmp", crt__wcsicmp, NULL }, { "_wcsnicmp", crt__wcsnicmp, NULL }, { "wcschr", crt_wcschr, NULL }, { "wcsrchr", crt_wcsrchr, NULL },
    { "wcsstr", crt_wcsstr, NULL }, { "_wcsdup", crt__wcsdup, NULL }, { "_wcslwr", crt__wcslwr, NULL }, { "_wcsupr", crt__wcsupr, NULL },
    { "wcscpy_s", crt_wcscpy_s, NULL }, { "wcscat_s", crt_wcscat_s, NULL }, { "wcsncpy_s", crt_wcsncpy_s, NULL }, { "wcsspn", crt_wcsspn, NULL },
    { "wcscspn", crt_wcscspn, NULL }, { "wcspbrk", crt_wcspbrk, NULL }, { "wcstok", crt_wcstok, NULL }, { "wcstok_s", crt_wcstok, NULL },
    { "wmemcpy", crt_wmemcpy, NULL }, { "wmemmove", crt_wmemcpy, NULL }, { "wmemset", crt_wmemset, NULL }, { "wcscoll", crt_wcscmp, NULL },
    // conversion
    { "atoi", crt_atoi, NULL }, { "atol", crt_atoi, NULL }, { "atoll", crt_atoll, NULL }, { "_atoi64", crt_atoll, NULL }, { "atof", crt_atof, NULL },
    { "strtod", crt_strtod, NULL }, { "strtof", crt_strtof, NULL }, { "strtold", crt_strtod, NULL }, { "strtol", crt_strtol, NULL },
    { "strtoul", crt_strtoul, NULL }, { "strtoll", crt_strtoll, NULL }, { "strtoull", crt_strtoull, NULL }, { "_strtoi64", crt_strtoll, NULL },
    { "_strtoui64", crt_strtoull, NULL }, { "_wtoi", crt__wtoi, NULL }, { "_wtol", crt__wtoi, NULL }, { "_wtoi64", crt__wtoi64, NULL },
    { "_wtof", crt__wtof, NULL }, { "wcstol", crt_wcstol, NULL }, { "wcstoul", crt_wcstoul, NULL }, { "wcstoll", crt_wcstoll, NULL },
    { "_wcstoi64", crt_wcstoll, NULL }, { "wcstod", crt_wcstod, NULL }, { "_itoa", crt__itoa, NULL }, { "itoa", crt__itoa, NULL },
    { "_ltoa", crt__itoa, NULL }, { "_ultoa", crt__ultoa, NULL }, { "_i64toa", crt__i64toa, NULL }, { "_ui64toa", crt__ui64toa, NULL },
    { "_itoa_s", crt__itoa_s, NULL }, { "_ltoa_s", crt__itoa_s, NULL }, { "_i64toa_s", crt__i64toa_s, NULL }, { "_ui64toa_s", crt__ui64toa_s, NULL }, { "_itow", crt__itow, NULL },
    { "mbstowcs", crt_mbstowcs, NULL }, { "wcstombs", crt_wcstombs, NULL }, { "mbtowc", crt_mbtowc, NULL }, { "wctomb", crt_wctomb, NULL },
    { "mblen", crt_mblen, NULL }, { "btowc", crt_btowc, NULL },
    // ctype
    { "isalpha", crt_isalpha, NULL }, { "isdigit", crt_isdigit, NULL }, { "isalnum", crt_isalnum, NULL }, { "isspace", crt_isspace, NULL },
    { "isupper", crt_isupper, NULL }, { "islower", crt_islower, NULL }, { "isxdigit", crt_isxdigit, NULL }, { "ispunct", crt_ispunct, NULL },
    { "isprint", crt_isprint, NULL }, { "isgraph", crt_isgraph, NULL }, { "iscntrl", crt_iscntrl, NULL }, { "isblank", crt_isblank, NULL },
    { "toupper", crt_toupper, NULL }, { "tolower", crt_tolower, NULL }, { "_toupper", crt_toupper, NULL }, { "_tolower", crt_tolower, NULL },
    { "__isascii", crt_isascii, NULL }, { "isascii", crt_isascii, NULL }, { "iswalpha", crt_iswalpha, NULL }, { "iswdigit", crt_iswdigit, NULL },
    { "iswspace", crt_iswspace, NULL }, { "iswalnum", crt_iswalnum, NULL }, { "iswupper", crt_iswupper, NULL }, { "iswlower", crt_iswlower, NULL },
    { "iswpunct", crt_iswpunct, NULL }, { "iswprint", crt_iswprint, NULL }, { "iswxdigit", crt_iswxdigit, NULL }, { "iswctype", crt_iswctype, NULL },
    { "towupper", crt_towupper, NULL }, { "towlower", crt_towlower, NULL }, { "_isctype", crt__isctype, NULL },
    { "__pctype_func", crt___pctype_func, NULL }, { "_pctype", NULL, data_pctype },
    // math
    { "sin", crt_sin, NULL }, { "cos", crt_cos, NULL }, { "tan", crt_tan, NULL }, { "asin", crt_asin, NULL }, { "acos", crt_acos, NULL },
    { "atan", crt_atan, NULL }, { "sinh", crt_sinh, NULL }, { "cosh", crt_cosh, NULL }, { "tanh", crt_tanh, NULL }, { "asinh", crt_asinh, NULL },
    { "acosh", crt_acosh, NULL }, { "atanh", crt_atanh, NULL }, { "exp", crt_exp, NULL }, { "exp2", crt_exp2, NULL }, { "expm1", crt_expm1, NULL },
    { "log", crt_log, NULL }, { "log10", crt_log10, NULL }, { "log2", crt_log2, NULL }, { "log1p", crt_log1p, NULL }, { "sqrt", crt_sqrt, NULL },
    { "cbrt", crt_cbrt, NULL }, { "fabs", crt_fabs, NULL }, { "floor", crt_floor, NULL }, { "ceil", crt_ceil, NULL }, { "round", crt_round, NULL },
    { "trunc", crt_trunc, NULL }, { "rint", crt_rint, NULL }, { "nearbyint", crt_nearbyint, NULL }, { "erf", crt_erf, NULL }, { "erfc", crt_erfc, NULL },
    { "tgamma", crt_tgamma, NULL }, { "lgamma", crt_lgamma, NULL }, { "logb", crt_logb, NULL }, { "_logb", crt_logb, NULL },
    { "atan2", crt_atan2, NULL }, { "pow", crt_pow, NULL }, { "fmod", crt_fmod, NULL }, { "hypot", crt_hypot, NULL }, { "_hypot", crt_hypot, NULL },
    { "fmin", crt_fmin, NULL }, { "fmax", crt_fmax, NULL }, { "copysign", crt_copysign, NULL }, { "_copysign", crt_copysign, NULL },
    { "remainder", crt_remainder, NULL }, { "fdim", crt_fdim, NULL }, { "nextafter", crt_nextafter, NULL }, { "_nextafter", crt_nextafter, NULL },
    { "sinf", crt_sinf, NULL }, { "cosf", crt_cosf, NULL }, { "tanf", crt_tanf, NULL }, { "asinf", crt_asinf, NULL }, { "acosf", crt_acosf, NULL },
    { "atanf", crt_atanf, NULL }, { "expf", crt_expf, NULL }, { "logf", crt_logf, NULL }, { "log10f", crt_log10f, NULL }, { "log2f", crt_log2f, NULL },
    { "sqrtf", crt_sqrtf, NULL }, { "floorf", crt_floorf, NULL }, { "ceilf", crt_ceilf, NULL }, { "roundf", crt_roundf, NULL }, { "truncf", crt_truncf, NULL },
    { "fabsf", crt_fabsf, NULL }, { "sinhf", crt_sinhf, NULL }, { "coshf", crt_coshf, NULL }, { "tanhf", crt_tanhf, NULL }, { "exp2f", crt_exp2f, NULL },
    { "cbrtf", crt_cbrtf, NULL }, { "atan2f", crt_atan2f, NULL }, { "powf", crt_powf, NULL }, { "fmodf", crt_fmodf, NULL }, { "hypotf", crt_hypotf, NULL },
    { "fminf", crt_fminf, NULL }, { "fmaxf", crt_fmaxf, NULL }, { "copysignf", crt_copysignf, NULL }, { "ldexp", crt_ldexp, NULL }, { "ldexpf", crt_ldexpf, NULL },
    { "frexp", crt_frexp, NULL }, { "modf", crt_modf, NULL }, { "modff", crt_modff, NULL }, { "fma", crt_fma, NULL }, { "lround", crt_lround, NULL },
    { "llround", crt_llround, NULL }, { "lrint", crt_lrint, NULL }, { "llrint", crt_llrint, NULL }, { "ilogb", crt_ilogb, NULL }, { "scalbn", crt_scalbn, NULL },
    { "_finite", crt__finite, NULL }, { "_isnan", crt__isnan, NULL }, { "_fpclass", crt__fpclass, NULL }, { "_dclass", crt__dclass, NULL }, { "_dsign", crt__dsign, NULL },
    { "abs", crt_abs, NULL }, { "labs", crt_abs, NULL }, { "llabs", crt_llabs, NULL }, { "_abs64", crt_llabs, NULL }, { "div", crt_div, NULL },
    { "ldiv", crt_ldiv, NULL }, { "lldiv", crt_lldiv, NULL }, { "_controlfp", crt__controlfp, NULL }, { "_control87", crt__controlfp, NULL },
    { "_controlfp_s", crt__controlfp_s, NULL }, { "_clearfp", crt__clearfp, NULL }, { "_statusfp", crt__statusfp, NULL }, { "_fpreset", crt__fpreset, NULL },
    { "__setusermatherr", crt___setusermatherr, NULL }, { "_setusermatherr", crt___setusermatherr, NULL }, { "_set_SSE2_enable", crt__set_SSE2_enable, NULL },
    // stdlib / process
    { "atexit", crt_atexit, NULL }, { "_onexit", crt__onexit, NULL }, { "__dllonexit", crt___dllonexit, NULL }, { "exit", crt_exit, NULL },
    { "_exit", crt__exit, NULL }, { "_Exit", crt__exit, NULL }, { "quick_exit", crt__exit, NULL }, { "_cexit", crt__cexit, NULL }, { "_c_exit", crt__c_exit, NULL },
    { "abort", crt_abort, NULL }, { "_amsg_exit", crt__amsg_exit, NULL }, { "terminate", crt_terminate, NULL }, { "__std_terminate", crt_terminate, NULL },
    { "_purecall", crt__purecall, NULL }, { "_invalid_parameter_noinfo", crt__invalid_parameter_noinfo, NULL },
    { "_invalid_parameter_noinfo_noreturn", crt__invalid_parameter_noinfo_noreturn, NULL }, { "_invoke_watson", crt__invoke_watson, NULL },
    { "_CxxThrowException", crt__CxxThrowException, NULL }, { "__C_specific_handler", crt___C_specific_handler, NULL },
    { "__CxxFrameHandler3", crt___CxxFrameHandler, NULL }, { "__CxxFrameHandler4", crt___CxxFrameHandler, NULL }, { "__CxxFrameHandler", crt___CxxFrameHandler, NULL },
    { "__current_exception", crt___current_exception, NULL }, { "__current_exception_context", crt___current_exception_context, NULL },
    { "__std_type_info_destroy_list", crt___std_type_info_destroy_list, NULL }, { "__std_exception_copy", crt___std_exception_copy, NULL },
    { "__std_exception_destroy", crt___std_exception_destroy, NULL }, { "__std_type_info_compare", crt___std_type_info_compare, NULL },
    { "__std_type_info_name", crt___std_type_info_name, NULL }, { "__std_type_info_hash", crt___std_type_info_hash, NULL },
    { "qsort", crt_qsort, NULL }, { "qsort_s", crt_qsort_s, NULL }, { "bsearch", crt_bsearch, NULL }, { "rand", crt_rand, NULL }, { "srand", crt_srand, NULL },
    { "rand_s", crt_rand_s, NULL }, { "getenv", crt_getenv, NULL }, { "_wgetenv", crt__wgetenv, NULL }, { "getenv_s", crt_getenv_s, NULL },
    { "_dupenv_s", crt__dupenv_s, NULL }, { "_putenv", crt__putenv, NULL }, { "_putenv_s", crt__putenv_s, NULL }, { "system", crt_system, NULL },
    { "_errno", crt__errno, NULL }, { "__doserrno", crt___doserrno, NULL }, { "_get_errno", crt__get_errno, NULL }, { "_set_errno", crt__set_errno, NULL },
    { "errno", NULL, data_errno }, { "signal", crt_signal, NULL }, { "raise", crt_raise, NULL }, { "setlocale", crt_setlocale, NULL },
    { "_wsetlocale", crt__wsetlocale, NULL }, { "localeconv", crt_localeconv, NULL }, { "___lc_codepage_func", crt____lc_codepage_func, NULL },
    { "___mb_cur_max_func", crt____mb_cur_max_func, NULL }, { "__mb_cur_max", NULL, data_mb_cur_max }, { "___lc_locale_name_func", crt___lc_locale_name_func, NULL },
    { "_configthreadlocale", crt__configthreadlocale, NULL }, { "_create_locale", crt__create_locale, NULL }, { "_free_locale", crt__free_locale, NULL },
    { "_get_current_locale", crt__get_current_locale, NULL }, { "_getpid", crt__getpid, NULL }, { "_beginthreadex", crt__beginthreadex, NULL },
    { "_beginthread", crt__beginthread, NULL }, { "_sleep", crt__sleep, NULL },
    { "_endthreadex", crt__endthreadex, NULL }, { "_endthread", crt__endthread, NULL }, { "_Thrd_yield", crt__Thrd_yield, NULL },
    { "_setjmp", crt__setjmp, NULL }, { "_setjmp3", crt__setjmp, NULL }, { "setjmp", crt__setjmp, NULL }, { "_setjmpex", crt__setjmp, NULL }, { "longjmp", crt_longjmp, NULL }, { "_longjmp", crt_longjmp, NULL },
    // time
    { "time", crt_time_plain, NULL }, { "_time64", crt_time64, NULL }, { "_time32", crt_time32, NULL }, { "clock", crt_clock, NULL }, { "difftime", crt_difftime_plain, NULL }, { "_difftime32", crt_difftime32, NULL },
    { "_difftime64", crt_difftime, NULL }, { "localtime", crt_localtime_plain, NULL }, { "_localtime64", crt_localtime64, NULL }, { "_localtime32", crt_localtime32, NULL },
    { "gmtime", crt_gmtime_plain, NULL }, { "_gmtime32", crt_gmtime32, NULL }, { "_gmtime64", crt_gmtime64, NULL }, { "localtime_s", crt_localtime64_s, NULL }, { "_localtime64_s", crt_localtime64_s, NULL },
    { "gmtime_s", crt_gmtime64_s, NULL }, { "_gmtime64_s", crt_gmtime64_s, NULL }, { "mktime", crt_mktime_plain, NULL }, { "_mktime32", crt_mktime32, NULL }, { "_mktime64", crt_mktime64, NULL },
    { "_mkgmtime64", crt_mkgmtime64, NULL }, { "_mkgmtime", crt_mkgmtime64, NULL }, { "strftime", crt_strftime, NULL }, { "wcsftime", crt_wcsftime, NULL },
    { "asctime", crt_asctime, NULL }, { "ctime", crt_ctime_plain, NULL }, { "_ctime32", crt_ctime32, NULL }, { "_ctime64", crt_ctime64, NULL }, { "_ftime", crt_ftime64, NULL }, { "_ftime64", crt_ftime64, NULL },
    { "_strdate", crt__strdate, NULL }, { "_strtime", crt__strtime, NULL }, { "_tzset", crt__tzset, NULL }, { "_timezone", NULL, data_timezone },
    { "_daylight", NULL, data_daylight }, { "__timezone", crt___timezone, NULL }, { "__daylight", crt___daylight, NULL },
    // startup
    { "__getmainargs", crt___getmainargs, NULL }, { "__wgetmainargs", crt___wgetmainargs, NULL }, { "__p___argc", crt___p___argc, NULL },
    { "__p___argv", crt___p___argv, NULL }, { "__p___wargv", crt___p___wargv, NULL }, { "__p__environ", crt___p__environ, NULL },
    { "__p__wenviron", crt___p__wenviron, NULL }, { "__p__acmdln", crt___p__acmdln, NULL }, { "__p__wcmdln", crt___p__wcmdln, NULL },
    { "__p__pgmptr", crt___p__pgmptr, NULL }, { "__p__wpgmptr", crt___p__wpgmptr, NULL }, { "_get_pgmptr", crt__get_pgmptr, NULL }, { "_get_wpgmptr", crt__get_wpgmptr, NULL },
    { "__p__fmode", crt___p__fmode, NULL }, { "__p__commode", crt___p__commode, NULL }, { "_set_fmode", crt__set_fmode, NULL }, { "_get_fmode", crt__get_fmode, NULL },
    { "_get_initial_narrow_environment", crt__get_initial_narrow_environment, NULL }, { "_get_initial_wide_environment", crt__get_initial_wide_environment, NULL },
    { "_configure_narrow_argv", crt__configure_narrow_argv, NULL }, { "_configure_wide_argv", crt__configure_narrow_argv, NULL },
    { "_initialize_narrow_environment", crt__initialize_narrow_environment, NULL }, { "_initialize_wide_environment", crt__initialize_narrow_environment, NULL },
    { "__set_app_type", crt__set_app_type, NULL }, { "_set_app_type", crt__set_app_type, NULL }, { "_set_new_mode", crt__set_new_mode, NULL },
    { "_seh_filter_exe", crt__seh_filter_exe, NULL }, { "_seh_filter_dll", crt__seh_filter_exe, NULL },
    { "_register_thread_local_exe_atexit_callback", crt__register_thread_local_exe_atexit_callback, NULL },
    { "_lock", crt__lock, NULL }, { "_unlock", crt__lock, NULL }, { "_lock_file", crt__lock, NULL }, { "_unlock_file", crt__lock, NULL },
    { "_set_invalid_parameter_handler", crt__set_invalid_parameter_handler, NULL }, { "_set_thread_local_invalid_parameter_handler", crt__set_invalid_parameter_handler, NULL },
    { "_set_abort_behavior", crt__set_abort_behavior, NULL }, { "_set_purecall_handler", crt__set_purecall_handler, NULL }, { "_crt_debugger_hook", crt__crt_debugger_hook, NULL },
    { "_initterm", crt__initterm, NULL }, { "_initterm_e", crt__initterm_e, NULL }, { "_initialize_onexit_table", crt__initialize_onexit_table, NULL },
    { "_register_onexit_function", crt__register_onexit_function, NULL }, { "_execute_onexit_table", crt__execute_onexit_table, NULL },
    { "_crt_atexit", crt__crt_atexit, NULL }, { "_crt_at_quick_exit", crt__crt_atexit, NULL }, { "_get_stream_buffer_pointers", crt__get_stream_buffer_pointers, NULL },
    { "_fmode", NULL, data_fmode }, { "_commode", NULL, data_commode }, { "__argc", NULL, data_argc }, { "__argv", NULL, data_argv },
    { "__wargv", NULL, data_wargv }, { "_environ", NULL, data_environ }, { "__initenv", NULL, data_environ }, { "_wenviron", NULL, data_wenviron },
    { "__winitenv", NULL, data_wenviron }, { "_acmdln", NULL, data_acmdln }, { "_wcmdln", NULL, data_wcmdln },
    { NULL, NULL, NULL }
};
