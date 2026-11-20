// SoulCore - shared helpers for the high level emulation (HLE) of Win32 / CRT APIs.
#pragma once
#include "soul_internal.h"
#include "interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

typedef void *(*HleDataFn)(void);
typedef struct HleEntry {
    const char *name;
    HleFn fn;
    HleDataFn data; // when set, the import resolves to this data address
} HleEntry;

#define HLE(fname) static void fname(X86State *st)
#define HLE_STUB_RET(fname, v) static void fname(X86State *st) { RET(v); }

extern const HleEntry hle_kernel32_table[];
extern const HleEntry hle_crt_table[];
extern const HleEntry hle_misc_table[];

// ---- Win32 error codes --------------------------------------------------
#define ERROR_SUCCESS 0
#define ERROR_INVALID_FUNCTION 1
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_NOT_ENOUGH_MEMORY 8
#define ERROR_NO_MORE_FILES 18
#define ERROR_HANDLE_EOF 38
#define ERROR_NOT_SUPPORTED 50
#define ERROR_FILE_EXISTS 80
#define ERROR_INVALID_PARAMETER 87
#define ERROR_BROKEN_PIPE 109
#define ERROR_CALL_NOT_IMPLEMENTED 120
#define ERROR_INSUFFICIENT_BUFFER 122
#define ERROR_MOD_NOT_FOUND 126
#define ERROR_PROC_NOT_FOUND 127
#define ERROR_DIR_NOT_EMPTY 145
#define ERROR_ALREADY_EXISTS 183
#define ERROR_ENVVAR_NOT_FOUND 203
#define ERROR_NO_UNICODE_TRANSLATION 1113

// ---- handles --------------------------------------------------------------
typedef enum {
    H_FREE = 0,
    H_CONIN,
    H_CONOUT,
    H_CONERR,
    H_FILE,
    H_DIR,
    H_FIND,
    H_EVENT,
    H_MUTEX,
    H_SEMAPHORE,
    H_THREAD,
    H_PROCESS,
    H_HEAP,
    H_NULLDEV,
    H_MAPPING,
} HType;

typedef struct HObj {
    HType type;
    int fd;
    int append;
    DIR *dir;
    char pattern[260];
    char path[1024];
    int signaled;
    int manual;
    long count;
    void *ptr;
    size_t len;
    uint32_t owner;      // mutex owner thread id
    uint32_t recursion;  // mutex recursion count
} HObj;

#define MAX_HANDLES 1024
#define HANDLE_BASE 0x100

uint64_t h_alloc(HType t, HObj **out);
HObj *h_get(uint64_t h);
void h_free(uint64_t h);
uint64_t h_std(int which); // 0 in, 1 out, 2 err

// ---- state ---------------------------------------------------------------
typedef struct HleState {
    HObj handles[MAX_HANDLES];
    uint64_t std_handles[3];
    uint64_t tls_next;
    uint64_t fls_next;
    uint64_t fls_values[128];
    // environment (UTF-8 "KEY=VALUE")
    char **env;
    int env_count;
    // process heap marker
    uint64_t process_heap;
    // console
    uint16_t con_attr;
    char con_title[256];
    // output line buffers
    char outbuf[2][4096];
    size_t outlen[2];
    uint64_t api_calls;
} HleState;

HleState *hs(void);
X86State *hle_state_of(void);

void set_last_error(X86State *st, uint32_t e);
uint32_t get_last_error(X86State *st);
uint32_t errno_to_win(int e);

// ---- strings -------------------------------------------------------------
size_t wstrlen(const uint16_t *w);
// Convert UTF-16 (len -1 = NUL terminated) to UTF-8; returns bytes written (excluding NUL)
size_t w2u(const uint16_t *w, long wlen, char *out, size_t outcap);
// Convert UTF-8 (len -1 = NUL terminated) to UTF-16; returns code units written (excluding NUL)
size_t u2w(const char *s, long slen, uint16_t *out, size_t outcap);
char *w2u_dup(const uint16_t *w);      // malloc'ed
uint16_t *u2w_dup(const char *s);      // malloc'ed

// ---- console / stdio -------------------------------------------------------
void con_write(int stream, const char *data, size_t len); // stream 1 = stdout, 2 = stderr
void con_flush(void);
// Blocking read from the UI provided stdin. Returns bytes, 0 on EOF.
size_t con_read(char *buf, size_t max, int line_mode);
int con_getc(void);        // -1 on EOF
int con_has_input(void);

// ---- files ---------------------------------------------------------------
int path_w2h(const char *win, char *host, size_t hostlen); // uses g_proc
void path_normalize_win(const char *in, char *out, size_t outlen); // absolute Windows path
uint64_t filetime_from_unix(double t);
void fill_find_data(void *dst, int wide, const char *name, const char *host_path);
int wild_match(const char *pat, const char *s);

// ---- memory regions ---------------------------------------------------------
void *vm_alloc(uint64_t addr, size_t size, int *existed);
int vm_free(uint64_t addr);
int vm_query(uint64_t addr, uint64_t *base, uint64_t *alloc_base, uint64_t *size, int *type);

// ---- CRT internals shared with kernel32 -------------------------------------
void crt_run_atexit(X86State *st);
void crt_reset(void);
void crt_flush_all(void);

// ---- printf / scanf engine ------------------------------------------------------
typedef struct ArgSrc {
    X86State *st;
    int next;        // next argument index (register/stack mode)
    uint64_t va;     // va_list pointer (0 = register mode)
} ArgSrc;
static inline uint64_t arg_next(ArgSrc *a) {
    if (a->va) { uint64_t v = mem_rd(a->va, 8); a->va += 8; return v; }
    return hle_arg(a->st, a->next++);
}
typedef struct OutSink {
    char *buf;      // destination buffer (may be NULL to only count)
    size_t cap;     // capacity in units
    size_t len;     // units produced (may exceed cap)
    int wide;       // write UTF-16 into buf
    int stream;     // >0: write to console/file stream via callback
    void (*flush)(struct OutSink *, const char *, size_t);
    void *ctx;
} OutSink;
// fmt is UTF-8 (wide formats are converted first). wide_fmt: %s means wide string.
size_t fmt_format(OutSink *out, const char *fmt, ArgSrc *args, int wide_fmt);
char *fmt_vformat(const char *fmt, ArgSrc *a, int wide_fmt, size_t *outlen); // malloc'ed UTF-8
typedef struct InSrc {
    int (*getc)(struct InSrc *);
    void (*ungetc)(struct InSrc *, int);
    const char *str;  // for string sources
    size_t pos;
    void *ctx;
    int consumed;
    int secure;       // *_s variants pass buffer sizes for %s %c %[
} InSrc;
int fmt_scan(InSrc *in, const char *fmt, ArgSrc *args, int wide_fmt);
