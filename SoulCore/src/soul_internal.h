// SoulCore - internal runtime declarations shared between modules.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <setjmp.h>
#include <pthread.h>
#include "cpu.h"
#include "../include/SoulCore.h"

typedef struct SoulProcess SoulProcess;
typedef struct PEModule PEModule;
typedef struct JitCache JitCache;

struct SoulThread {
    X86State *st;
    SoulProcess *proc;
    uint64_t teb;
    uint64_t stack_base;  // highest address
    uint64_t stack_limit; // lowest address
    sigjmp_buf *jb;       // where fatal errors / exit unwind to
    int call_depth;
    int skip_ret;         // set by HLE functions that redirect control flow
    uint32_t cur_thunk;   // id of the HLE thunk being executed
    uint64_t block_rip;   // start of the JIT block being executed (diagnostics)
    // multithreading
    uint32_t tid;
    int is_main;
    int suspended;
    volatile int finished;
    uint32_t exit_code;
    uint64_t start_fn, start_arg;
    pthread_t host;
    uint8_t *teb_block;
    size_t teb_size;
    void *stack_mem;
    size_t stack_size;
    uint64_t fls[128];
    struct SoulThread *next;
};

#define SOUL_MAX_TLS_MODULES 32
typedef struct TlsTemplate { uint64_t start; size_t raw, zero; int index; uint64_t callbacks; uint64_t module_base; } TlsTemplate;

typedef void (*HleFn)(X86State *st);

typedef struct HleThunk {
    HleFn fn;
    const char *dll;
    const char *name;
    int warned;
} HleThunk;

#define SOUL_THUNK_STRIDE 16
#define SOUL_MAX_THUNKS 8192

struct SoulProcess {
    SoulLaunchOptions opts;
    char exe_host[1024];
    char exe_win[520];
    char drive_c[1024];
    char cwd_win[520];
    char cmdline[2048];
    char error[1024];
    int exit_code;
    int exited;
    int verbose;
    int use_jit;

    PEModule *modules;
    PEModule *main_module;

    uint8_t *thunk_mem;       // executable-looking guest memory for import thunks
    uint64_t thunk_base;
    uint32_t thunk_count;
    HleThunk thunks[SOUL_MAX_THUNKS];
    uint64_t trap_addr;       // return address used by guest_call

    SoulThread main_thread;
    X86State *main_state;

    JitCache *jit;
    double start_time;

    // stdin pipe fed by the UI
    pthread_mutex_t in_mu;
    pthread_cond_t in_cv;
    char *in_buf;
    size_t in_len, in_cap;
    int in_closed;

    volatile int terminate;
    void *hle_state;          // owned by hle_core.c
    pthread_t guest_thread;   // host thread running the guest

    // threads: guest code runs under one global interpreter lock (GIL)
    pthread_mutex_t gil;
    pthread_cond_t gil_cv;     // unused (kept for ABI of older builds)
    pthread_mutex_t ev_mu;     // guards ev_gen; waiters sleep on ev_cv without holding the GIL
    pthread_cond_t ev_cv;      // broadcast whenever a guest sync object changes
    volatile uint64_t ev_gen;
    volatile uint64_t gil_epoch; // incremented on every GIL acquisition
    volatile int gil_waiters;
    SoulThread *gil_owner;
    SoulThread *threads;       // additional guest threads
    uint32_t next_tid;
    volatile int exit_requested;
    int requested_code;
    uint64_t retired_jit, retired_interp; // instruction counts of finished threads
    TlsTemplate tls[SOUL_MAX_TLS_MODULES];
    int tls_count;
    pthread_t ticker;
    int ticker_running;
};

extern SoulProcess *g_proc;

// runtime.c
SoulThread *soul_cur_thread(void);
void gil_lock(SoulThread *t);
void gil_unlock(SoulThread *t);
void gil_notify(void);                       // wake threads waiting on guest sync objects
int gil_wait(int64_t timeout_ms);            // 0 = woken, 1 = timeout (GIL must be held)
void soul_yield(void);
SoulThread *soul_thread_create(X86State *st, uint64_t fn, uint64_t arg, size_t stack, int suspended);
void soul_thread_resume(SoulThread *t);
void soul_exit_thread(X86State *st, uint32_t code) __attribute__((noreturn));
void pe_init_thread_tls(SoulProcess *p, X86State *st, uint64_t teb, int attach_callbacks);
void soul_fatal(X86State *st, const char *fmt, ...) __attribute__((noreturn, format(printf, 2, 3)));
void soul_exit_process(X86State *st, int code) __attribute__((noreturn));
void soul_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void soul_out(int stream, const char *data, size_t len);
uint64_t soul_guest_call(X86State *st, uint64_t fn, int nargs, const uint64_t *args);
void soul_run_until_trap(X86State *st);
void soul_flush_code_cache(void);
double soul_now(void);

// hle_core.c
void hle_init(SoulProcess *p);
void hle_shutdown(SoulProcess *p);
uint64_t hle_resolve_import(SoulProcess *p, const char *dll, const char *name, int ordinal);
uint64_t hle_get_proc(SoulProcess *p, const char *dll, const char *name); // 0 if not implemented
int hle_is_system_dll(const char *dll);
void hle_invoke(X86State *st, uint32_t id);
uint32_t hle_thunk_for(SoulProcess *p, HleFn fn, const char *dll, const char *name);

// Argument access for the Win64 calling convention inside HLE functions.
static inline uint64_t hle_arg(X86State *st, int n) {
    switch (n) {
    case 0: return st->gpr[RCX];
    case 1: return st->gpr[RDX];
    case 2: return st->gpr[R8];
    case 3: return st->gpr[R9];
    default: return mem_rd(st->gpr[RSP] + 8 + 8 * (uint64_t)n, 8);
    }
}
#define ARG(n) hle_arg(st, (n))
#define PTR(n) ((void *)(uintptr_t)hle_arg(st, (n)))
#define RET(v) (st->gpr[RAX] = (uint64_t)(v))

// pe.c
struct PEModule {
    char name[128];      // lower-case file name, e.g. "foo.dll"
    char host_path[1024];
    char win_path[520];
    uint64_t base;
    uint64_t size;
    uint64_t entry;
    int is_dll;
    int initialized;
    uint32_t export_rva, export_size;
    uint32_t tls_rva;
    uint32_t pdata_rva, pdata_size;
    uint64_t stack_reserve;
    uint16_t subsystem;
    PEModule *next;
};
PEModule *pe_load(SoulProcess *p, const char *host_path, const char *win_path, char *err, size_t errlen);
uint64_t pe_find_export(PEModule *m, const char *name, int ordinal);
PEModule *pe_find_loaded(SoulProcess *p, const char *name);
PEModule *pe_load_dependency(SoulProcess *p, const char *dll, char *err, size_t errlen);
void pe_unload_all(SoulProcess *p);
int pe_run_initializers(SoulProcess *p, X86State *st);

// jit_mem.c / jit_a64.c
int jitmem_ready(void);
void *jitmem_rw(void);
void *jitmem_rx(void);
size_t jitmem_size(void);
void jitmem_flush_icache(void *rx, size_t len);
void jitmem_begin_write(void);
void jitmem_end_write(void);

JitCache *jit_create(SoulProcess *p);
void jit_destroy(JitCache *j);
void jit_flush(JitCache *j);
// Executes JIT code at st->rip if possible. Returns 1 if anything ran, 0 if caller must interpret.
int jit_run(JitCache *j, X86State *st);
void jit_stats(JitCache *j, SoulStats *s);

// paths (hle_core.c)
int win_to_host_path(SoulProcess *p, const char *win, char *out, size_t outlen);
