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
    uint64_t seh_disp[16];  // 32-bit SEH: guest addresses of exception dispatches in progress
    int seh_n;
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
    uint16_t argbytes; // 32-bit programs: bytes of stack arguments a stdcall function removes
} HleThunk;

#define SOUL_THUNK_STRIDE 16
#define SOUL_MAX_THUNKS 8192

struct SoulProcess {
    SoulLaunchOptions opts;
    uint32_t pid, parent_pid;
    struct SoulProcess *next_proc; // session list
    uint8_t *pvars;           // private variables while another process is active (proc.c)
    int std_fd[3];            // inherited standard handles (host descriptors, -1 = console)
    int released, host_started, jit_slice;
    uint64_t retired_api;     // API calls of the process's HLE state once it was released
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

extern SoulProcess *g_proc; // the active process: the one whose thread holds the GIL

// proc.c: per-process copies of global variables (see the file comment)
void proc_var_register(void *addr, size_t size);
#define SOUL_PROCESS_VAR(v) __attribute__((constructor)) static void soul_pv_##v(void) { proc_var_register(&(v), sizeof(v)); }
uint8_t *proc_vars_new(void);
void proc_activate(SoulProcess *p);
void proc_vars_reset(void);

// Processes started by guest programs (CreateProcess, ShellExecute)
typedef struct SoulSpawn {
    const char *exe_host, *exe_win, *cmdline, *cwd_win;
    char **env;          // NULL-terminated "NAME=value" list (UTF-8), NULL = the parent's
    int suspended;
    int std_fd[3];       // host descriptors the child owns, -1 = console
} SoulSpawn;
SoulProcess *soul_process_spawn(const SoulSpawn *s, char *err, size_t errlen);
void soul_process_kill(SoulProcess *p, int code);
SoulProcess *soul_process_by_pid(uint32_t pid);
SoulProcess *soul_session(void);      // the process the UI started
SoulProcess *soul_process_list(void); // every process of the session
uint32_t soul_new_tid(void);
void gui_process_exit(uint32_t pid); // user32.c

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
void hle_init_system(SoulProcess *root);  // shared state: windows, GDI, registry
void hle_shutdown_system(void);
void hle_init(SoulProcess *p, char **env); // per process (env: inherited environment or NULL)
void hle_shutdown(SoulProcess *p);
uint64_t hle_resolve_import(SoulProcess *p, const char *dll, const char *name, int ordinal);
uint64_t hle_get_proc(SoulProcess *p, const char *dll, const char *name); // 0 if not implemented
int hle_is_system_dll(const char *dll);
void hle_invoke(X86State *st, uint32_t id);
uint32_t hle_thunk_for(SoulProcess *p, HleFn fn, const char *dll, const char *name);

// Argument access inside HLE functions. 64-bit programs use the Win64 calling convention
// (RCX, RDX, R8, R9, then the stack); 32-bit programs pass every argument on the stack
// (stdcall / cdecl), so argument n is the 32-bit value at [ESP + 4 + 4n].
static inline uint64_t hle_arg(X86State *st, int n) {
    if (g_mode32) return mem_rd((uint32_t)(st->gpr[RSP] + 4 + 4 * (uint32_t)n), 4);
    switch (n) {
    case 0: return st->gpr[RCX];
    case 1: return st->gpr[RDX];
    case 2: return st->gpr[R8];
    case 3: return st->gpr[R9];
    default: return mem_rd(st->gpr[RSP] + 8 + 8 * (uint64_t)n, 8);
    }
}
// Return values. Host pointers into the guest window become guest addresses; in 32-bit mode
// 64-bit results are split into EDX:EAX (EDX is caller-saved, so setting it is harmless).
static inline void hle_ret(X86State *st, uint64_t v) {
    if (g_mode32) {
        if (v - g_gbase < 0x100000000ull) v -= g_gbase;
        st->gpr[RAX] = (uint32_t)v;
        st->gpr[RDX] = (uint32_t)(v >> 32);
        return;
    }
    st->gpr[RAX] = v;
}
#define ARG(n) hle_arg(st, (n))
#define PTR(n) G2H(hle_arg(st, (n)))
#define RET(v) hle_ret(st, (uint64_t)(v))

// Pointer-sized guest memory access (4 bytes for 32-bit programs, 8 for 64-bit ones).
#define PSZ (g_mode32 ? 4 : 8)
static inline uint64_t rd_ptr(uint64_t a) { return mem_rd(a, PSZ); }
static inline void wr_ptr(uint64_t a, uint64_t v) { mem_wr(a, PSZ, v); }
// Thread environment block fields (TEB64 through GS, TEB32 through FS).
static inline uint64_t teb_of(const X86State *st) { return g_mode32 ? st->fs_base : st->gs_base; }
#define TEB_LASTERR (g_mode32 ? 0x34 : 0x68)
#define TEB_TLSPTR (g_mode32 ? 0x2C : 0x58)
#define TEB_TLSSLOTS (g_mode32 ? 0xE10 : 0x1480)

// gwin.c: 4 GB guest window for 32-bit programs and the heap for memory handed to the guest
int gwin_init(void);
void gwin_release(void);
uint64_t gwin_map(uint64_t want, size_t size);
void gwin_unmap(uint64_t a, size_t size);
int gwin_contains(uint64_t a);
void *g_malloc(size_t n);
void *g_calloc(size_t a, size_t b);
void *g_realloc(void *p, size_t n);
void g_free(void *p);
size_t g_msize(void *p);
char *g_strdup(const char *s);
void gheap_reset(void);
// Allocates guest-visible memory of the given size: in the window for 32-bit programs,
// anywhere (mmap) for 64-bit ones. Returns a host pointer.
void *g_pages(size_t size);
void g_pages_free(void *p, size_t size);

// hle_kernel32.c: raises a Windows exception in a 32-bit program through its SEH chain
// (fs:[0]); eip/esp describe where execution would continue. Returns 0 when it cannot be
// dispatched (64-bit program), 1 when st now runs the first exception handler.
int soul_seh_raise(X86State *st, uint32_t code, uint32_t flags, uint64_t eip, uint64_t esp, int nparams, const uint32_t *params);

// pe.c
int pe_machine(const char *host_path); // 0x14C (i386), 0x8664 (x64) or 0
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
// Resource data of a loaded module (base 0 = main program). id_or_name < 0x10000 is an
// integer id, otherwise a NUL-terminated name (UTF-16 when wide, UTF-8 otherwise).
const void *pe_resource(uint64_t module_base, uint64_t type, uint64_t id_or_name, int wide, uint32_t *size);

// jit_mem.c / jit_a64.c
int jitmem_ready(void);
void *jitmem_rw(void);
void *jitmem_rx(void);
size_t jitmem_size(void);
void jitmem_flush_icache(void *rx, size_t len);
void jitmem_begin_write(void);
void jitmem_end_write(void);

JitCache *jit_create(SoulProcess *p);
JitCache *jit_create_range(SoulProcess *p, size_t off, size_t size);
void jit_set_range(JitCache *j, size_t off, size_t size);
void jit_destroy(JitCache *j);
void jit_flush(JitCache *j);
// Executes JIT code at st->rip if possible. Returns 1 if anything ran, 0 if caller must interpret.
int jit_run(JitCache *j, X86State *st);
void jit_stats(JitCache *j, SoulStats *s);

// paths (hle_core.c)
int win_to_host_path(SoulProcess *p, const char *win, char *out, size_t outlen);
