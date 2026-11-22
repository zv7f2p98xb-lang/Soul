// SoulCore - process lifecycle, dispatcher loop, guest calls and fault handling.
#include "hle.h"
#include <errno.h>
#include <sched.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

SoulProcess *g_proc;

void crt_process_end(void);

double soul_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

const char *soul_version(void) { return "SoulCore 0.4.0"; }

int soul_host_is_arm64(void) {
#if defined(__aarch64__) || defined(__arm64__)
    return 1;
#else
    return 0;
#endif
}

void soul_out(int stream, const char *data, size_t len) {
    SoulProcess *p = g_proc;
    if (p && p->opts.output) { p->opts.output(p->opts.ctx, stream, data, len); return; }
    FILE *f = stream == SOUL_STREAM_STDOUT ? stdout : stderr;
    fwrite(data, 1, len, f);
    fflush(f);
}

void soul_log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
    con_flush();
    soul_out(SOUL_STREAM_LOG, buf, (size_t)n);
}

// ---- threads and the global interpreter lock ------------------------------------------
// One lock for the guest threads of every process of the session: the windows, GDI objects and
// the registry are shared, and taking the lock activates the private state of the thread's process.

static __thread SoulThread *t_cur;
SoulThread *soul_cur_thread(void) { return t_cur; }

static pthread_mutex_t g_gil;
static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER; // guards g_ev_gen; waiters sleep on g_ev_cv without the GIL
static pthread_cond_t g_ev_cv = PTHREAD_COND_INITIALIZER;    // broadcast whenever a guest sync object changes
static volatile uint64_t g_ev_gen;
static volatile uint64_t g_gil_epoch;                       // incremented on every acquisition
static volatile int g_gil_waiters;
static SoulThread *volatile g_gil_owner;
static pthread_once_t g_sys_once = PTHREAD_ONCE_INIT;
static SoulProcess *g_root;                                 // the process the UI started
static SoulProcess *g_procs;                                // every process of the session
static uint32_t g_next_tid = 4243, g_next_pid = 4240;
static pthread_t g_ticker;
static volatile int g_ticker_running;
// Translated code: the program the UI starts uses the whole code buffer; once it starts other
// programs every process gets a slice of it (or runs in the interpreter when none is left).
#define JIT_SLICES 4
static int g_jit_on;
static uint32_t g_jit_slice_pid[JIT_SLICES];

static void sys_init(void) {
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
#if defined(__APPLE__)
    pthread_mutexattr_setpolicy_np(&ma, PTHREAD_MUTEX_POLICY_FAIRSHARE_NP);
#endif
    pthread_mutex_init(&g_gil, &ma);
    pthread_mutexattr_destroy(&ma);
    signal(SIGPIPE, SIG_IGN); // a program writing into a pipe nobody reads gets ERROR_BROKEN_PIPE, not a dead app
}

SoulProcess *soul_session(void) { return g_root; }
SoulProcess *soul_process_list(void) { return g_procs; }
uint32_t soul_new_tid(void) { g_next_tid += 4; return g_next_tid; }

void gil_lock(SoulThread *t) {
    __atomic_add_fetch(&g_gil_waiters, 1, __ATOMIC_SEQ_CST);
    pthread_mutex_lock(&g_gil);
    __atomic_sub_fetch(&g_gil_waiters, 1, __ATOMIC_SEQ_CST);
    g_gil_owner = t;
    g_gil_epoch++;
    t_cur = t;
    if (t && t->proc && t->proc != g_proc) proc_activate(t->proc);
}

void gil_unlock(SoulThread *t) {
    (void)t;
    g_gil_owner = NULL;
    pthread_mutex_unlock(&g_gil);
}

void gil_notify(void) {
    pthread_mutex_lock(&g_ev_mu);
    g_ev_gen++;
    pthread_cond_broadcast(&g_ev_cv);
    pthread_mutex_unlock(&g_ev_mu);
}

// Releases the GIL until gil_notify() is called or the timeout expires, then
// re-acquires it through gil_lock() so that every contender is counted.
int gil_wait(int64_t timeout_ms) {
    SoulThread *t = t_cur;
    SoulProcess *p = t && t->proc ? t->proc : g_proc;
    con_flush();
    int timed_out = 0;
    struct timespec ts;
    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&g_ev_mu);
    uint64_t gen = g_ev_gen;
    gil_unlock(t);
    while (g_ev_gen == gen && !p->terminate) {
        if (timeout_ms < 0) pthread_cond_wait(&g_ev_cv, &g_ev_mu);
        else if (pthread_cond_timedwait(&g_ev_cv, &g_ev_mu, &ts) == ETIMEDOUT) { timed_out = 1; break; }
    }
    pthread_mutex_unlock(&g_ev_mu);
    gil_lock(t);
    return timed_out;
}

// Hands the GIL to a waiting thread (if any) and takes it back afterwards.
void soul_yield(void) {
    SoulThread *t = t_cur;
    if (g_gil_waiters == 0) return;
    uint64_t epoch = g_gil_epoch;
    gil_unlock(t);
    for (int i = 0; i < 2000 && g_gil_epoch == epoch && g_gil_waiters > 0; i++) sched_yield();
    gil_lock(t);
}

static void stop_all_threads(SoulProcess *p) {
    p->terminate = 1;
    if (p->main_state) p->main_state->stop_req = 1;
    for (SoulThread *t = p->threads; t; t = t->next) if (t->st) t->st->stop_req = 1;
    gil_notify();
    pthread_mutex_lock(&p->in_mu);
    pthread_cond_broadcast(&p->in_cv);
    pthread_mutex_unlock(&p->in_mu);
}

static SoulThread *thread_of(X86State *st) {
    SoulProcess *p = g_proc;
    if (st && st->thread) return st->thread;
    return t_cur ? t_cur : &p->main_thread;
}

void soul_fatal(X86State *st, const char *fmt, ...) {
    SoulProcess *p = g_proc;
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (!p->error[0]) snprintf(p->error, sizeof p->error, "%s", msg);
    con_flush();
    char buf[1200];
    int n = snprintf(buf, sizeof buf, "\n[Soul] Program durduruldu: %s\n", msg);
    soul_out(SOUL_STREAM_LOG, buf, (size_t)n);
    p->exit_code = -1;
    SoulThread *t = thread_of(st);
    if (!t->is_main || p->threads) stop_all_threads(p);
    siglongjmp(*t->jb, 2);
}

void soul_exit_process(X86State *st, int code) {
    SoulProcess *p = g_proc;
    p->exit_code = code;
    p->requested_code = code;
    p->exit_requested = 1;
    con_flush();
    SoulThread *t = thread_of(st);
    if (p->threads) stop_all_threads(p);
    siglongjmp(*t->jb, 1);
}

void soul_exit_thread(X86State *st, uint32_t code) {
    SoulThread *t = thread_of(st);
    if (t->is_main) soul_exit_process(st, (int)code);
    t->exit_code = code;
    siglongjmp(*t->jb, 4);
}

void soul_flush_code_cache(void) {
    if (g_proc && g_proc->jit) jit_flush(g_proc->jit);
}

static void jit_attach(SoulProcess *p) {
    if (!g_jit_on) return;
    size_t slice = jitmem_size() / JIT_SLICES;
    if (g_root && g_root->jit && g_root->jit_slice < 0) { // the first child: the root moves to slice 0
        jit_set_range(g_root->jit, 0, slice);
        g_root->jit_slice = 0;
        g_jit_slice_pid[0] = g_root->pid;
    }
    for (int i = 0; i < JIT_SLICES; i++)
        if (!g_jit_slice_pid[i]) {
            p->jit = jit_create_range(p, (size_t)i * slice, slice);
            if (p->jit) { p->jit_slice = i; g_jit_slice_pid[i] = p->pid; }
            return;
        }
}
static void jit_detach(SoulProcess *p) {
    if (!p->jit) return;
    jit_destroy(p->jit);
    p->jit = NULL;
    if (p->jit_slice >= 0) g_jit_slice_pid[p->jit_slice] = 0;
    p->jit_slice = -1;
}

// Called when stop_req is set: either another thread wants the GIL or the process ends.
static void handle_stop(X86State *st) {
    SoulProcess *p = g_proc;
    st->stop_req = 0;
    if (!p->terminate) { soul_yield(); return; }
    SoulThread *t = thread_of(st);
    st->stop_req = 1; // keep stopping nested loops of this thread
    if (p->exit_requested) {
        p->exit_code = p->requested_code;
        siglongjmp(*t->jb, 1);
    }
    if (!p->error[0]) snprintf(p->error, sizeof p->error, "Kullanıcı tarafından durduruldu");
    p->exit_code = -1;
    siglongjmp(*t->jb, 2);
}

// ---- dispatcher ---------------------------------------------------------------------------------

void soul_run_until_trap(X86State *st) {
    SoulProcess *p = g_proc;
    const uint64_t tb = p->thunk_base;
    const uint64_t tsize = (uint64_t)SOUL_MAX_THUNKS * SOUL_THUNK_STRIDE;
    for (;;) {
        uint64_t rip = st->rip;
        if (rip - tb < tsize) {
            if (rip == p->trap_addr) return;
            hle_invoke(st, (uint32_t)((rip - tb) / SOUL_THUNK_STRIDE));
            continue;
        }
        if (__builtin_expect(st->stop_req, 0)) { handle_stop(st); continue; }
        if (p->jit && jit_run(p->jit, st)) continue;
        x86_interp_step(st);
    }
}

// Time slicing: while more than one guest thread exists, ask the GIL owner to yield
// every few milliseconds (translated code checks stop_req at every block exit).
static void *ticker_main(void *arg) {
    (void)arg;
    while (g_ticker_running) {
        usleep(4000);
        if (g_gil_waiters == 0) continue;
        SoulThread *owner = g_gil_owner;
        if (owner && owner->st) owner->st->stop_req = 1;
    }
    return NULL;
}
static void ticker_start(void) {
    if (g_ticker_running) return;
    g_ticker_running = 1;
    pthread_create(&g_ticker, NULL, ticker_main, NULL);
}

uint64_t soul_guest_call(X86State *st, uint64_t fn, int nargs, const uint64_t *args) {
    SoulProcess *p = g_proc;
    uint64_t saved_rip = st->rip, saved_rsp = st->gpr[RSP];
    SoulThread *t = st->thread;
    if (++t->call_depth > 200) soul_fatal(st, "İç içe geri çağrı derinliği aşıldı");
    if (g_mode32) {
        // 32-bit: arguments on the stack right to left, then the return address. ESP is
        // restored afterwards, so this works for stdcall and cdecl callbacks alike.
        uint32_t sp32 = (uint32_t)((st->gpr[RSP] - 64) & ~0xFull);
        sp32 -= (uint32_t)(4 * nargs);
        sp32 &= ~0xFu;
        for (int i = 0; i < nargs; i++) mem_wr(sp32 + 4u * (uint32_t)i, 4, args[i]);
        sp32 -= 4;
        mem_wr(sp32, 4, p->trap_addr);
        st->gpr[RSP] = sp32;
        st->rip = fn;
        soul_run_until_trap(st);
        // EAX, sign-extended like a 32-bit LRESULT / int so that callers see -1 as -1
        uint64_t r = (uint64_t)(int64_t)(int32_t)st->gpr[RAX];
        st->gpr[RSP] = saved_rsp;
        st->rip = saved_rip;
        t->call_depth--;
        return r;
    }
    uint64_t sp = (st->gpr[RSP] - 128) & ~0xFull;
    int stack_args = nargs > 4 ? nargs - 4 : 0;
    uint64_t area = (uint64_t)(32 + 8 * stack_args + 15) & ~0xFull;
    sp -= area;
    for (int i = 0; i < stack_args; i++) mem_wr(sp + 32 + 8 * (uint64_t)i, 8, args[4 + i]);
    sp -= 8;
    mem_wr(sp, 8, p->trap_addr);
    static const int regs[4] = { RCX, RDX, R8, R9 };
    for (int i = 0; i < nargs && i < 4; i++) {
        st->gpr[regs[i]] = args[i];
        st->xmm[i].q[0] = args[i];
    }
    st->gpr[RSP] = sp;
    st->rip = fn;
    soul_run_until_trap(st);
    uint64_t r = st->gpr[RAX];
    st->gpr[RSP] = saved_rsp;
    st->rip = saved_rip;
    t->call_depth--;
    return r;
}

// ---- faults ------------------------------------------------------------------------------------

static struct sigaction g_old_segv, g_old_bus, g_old_ill, g_old_fpe;
static volatile int g_in_fault;

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    SoulProcess *p = g_proc;
    SoulThread *t = t_cur;
    if (!p || !t || t->proc != p || !t->jb || g_in_fault || g_gil_owner != t) {
        // not a guest fault (e.g. another app thread): restore the previous handler and retry
        struct sigaction *old = sig == SIGSEGV ? &g_old_segv : sig == SIGBUS ? &g_old_bus : sig == SIGILL ? &g_old_ill : &g_old_fpe;
        sigaction(sig, old, NULL);
        return;
    }
    g_in_fault = 1;
    X86State *st = t->st;
    const char *what = sig == SIGSEGV ? "Bellek erişim ihlali" : sig == SIGBUS ? "Hizalama/veri yolu hatası"
                     : sig == SIGILL ? "Geçersiz komut" : "Aritmetik hata";
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    if (g_mode32 && fa - g_gbase < 0x100000000ull) fa -= g_gbase; // report the guest address
    if (!p->error[0])
        snprintf(p->error, sizeof p->error, "%s: adres 0x%llx (x86 RIP ~ 0x%llx%s)", what,
                 (unsigned long long)fa,
                 (unsigned long long)(t->block_rip ? t->block_rip : st->rip), t->is_main ? "" : ", yan iş parçacığı");
    p->exit_code = (int)0xC0000005;
    g_in_fault = 0;
    if (!t->is_main || p->threads) stop_all_threads(p);
    siglongjmp(*t->jb, 3);
}

static void install_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fault_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sigaction(SIGBUS, &sa, &g_old_bus);
    sigaction(SIGILL, &sa, &g_old_ill);
    sigaction(SIGFPE, &sa, &g_old_fpe);
}
static void restore_handlers(void) {
    sigaction(SIGSEGV, &g_old_segv, NULL);
    sigaction(SIGBUS, &g_old_bus, NULL);
    sigaction(SIGILL, &g_old_ill, NULL);
    sigaction(SIGFPE, &g_old_fpe, NULL);
}

// ---- process setup -------------------------------------------------------------------------------

// UNICODE_STRING: { USHORT Length, MaximumLength; PWSTR Buffer } (buffer at +4 in 32-bit, +8 in 64-bit)
static void unicode_string(uint8_t *at, const uint16_t *s) {
    uint16_t len = (uint16_t)(wstrlen(s) * 2), max = (uint16_t)(len + 2);
    uint64_t ptr = H2G(s);
    memcpy(at, &len, 2); memcpy(at + 2, &max, 2); memcpy(at + PSZ, &ptr, (size_t)PSZ);
}

static uint16_t *g_pp_strings[3];
static uint8_t *g_peb_block; // PEB, loader data and process parameters (shared by all threads)
SOUL_PROCESS_VAR(g_pp_strings)
SOUL_PROCESS_VAR(g_peb_block)

static uint16_t *g_wdup(const char *s) {
    uint16_t *h = u2w_dup(s);
    size_t n = (wstrlen(h) + 1) * 2;
    uint16_t *g = g_malloc(n);
    memcpy(g, h, n);
    free(h);
    return g;
}

static uint64_t make_teb(SoulProcess *p, SoulThread *t) {
    t->teb_size = 0x2000;
    t->teb_block = g_pages(t->teb_size);
    uint64_t tebv = H2G(t->teb_block);
    uint64_t tls = H2G(g_calloc(64, (size_t)PSZ));
    if (g_mode32) {
        mem_wr(tebv + 0x00, 4, 0xFFFFFFFFu);      // SEH chain: end of list
        mem_wr(tebv + 0x04, 4, t->stack_base);
        mem_wr(tebv + 0x08, 4, t->stack_limit);
        mem_wr(tebv + 0x18, 4, tebv);             // Self
        mem_wr(tebv + 0x20, 4, p->pid);           // ClientId.UniqueProcess
        mem_wr(tebv + 0x24, 4, t->tid);
        mem_wr(tebv + 0x2C, 4, tls);              // ThreadLocalStoragePointer
        mem_wr(tebv + 0x30, 4, H2G(g_peb_block));
        t->st->fs_base = tebv;
    } else {
        mem_wr(tebv + 0x08, 8, t->stack_base);
        mem_wr(tebv + 0x10, 8, t->stack_limit);
        mem_wr(tebv + 0x30, 8, tebv);
        mem_wr(tebv + 0x40, 8, p->pid);
        mem_wr(tebv + 0x48, 8, t->tid);
        mem_wr(tebv + 0x58, 8, tls);
        mem_wr(tebv + 0x60, 8, H2G(g_peb_block));
        t->st->gs_base = tebv;
    }
    t->teb = tebv;
    return tebv;
}

static void free_teb(SoulThread *t) {
    if (!t->teb_block) return;
    uint64_t tls = rd_ptr(t->teb + TEB_TLSPTR);
    if (tls) {
        for (int i = 0; i < 64; i++) g_free(G2H(rd_ptr(tls + (uint64_t)i * PSZ)));
        g_free(G2H(tls));
    }
    g_pages_free(t->teb_block, t->teb_size);
    t->teb_block = NULL;
}

static void setup_peb(SoulProcess *p) {
    g_peb_block = g_pages(0x2000);
    uint8_t *peb = g_peb_block, *ldr = g_peb_block + 0x1000, *pp = g_peb_block + 0x1200;
    uint64_t pebv = H2G(peb), ldrv = H2G(ldr);
    for (int i = 0; i < 3; i++) g_free(g_pp_strings[i]);
    g_pp_strings[0] = g_wdup(p->cwd_win);
    g_pp_strings[1] = g_wdup(p->exe_win);
    g_pp_strings[2] = g_wdup(p->cmdline);
    if (g_mode32) {
        mem_wr(pebv + 0x08, 4, p->main_module ? p->main_module->base : 0); // ImageBaseAddress
        mem_wr(pebv + 0x0C, 4, ldrv);
        mem_wr(pebv + 0x10, 4, H2G(pp));
        mem_wr(pebv + 0x18, 4, hs()->process_heap);
        mem_wr(pebv + 0x64, 4, 1);         // NumberOfProcessors
        mem_wr(pebv + 0xA4, 4, 10);        // OSMajorVersion
        mem_wr(pebv + 0xA8, 4, 0);
        mem_wr(pebv + 0xAC, 2, 19045);     // OSBuildNumber
        mem_wr(pebv + 0xB0, 4, 2);         // OSPlatformId
        mem_wr(ldrv + 0x4, 1, 1);
        for (int i = 0; i < 3; i++) { uint64_t l = ldrv + 0x0C + 8 * (uint64_t)i; mem_wr(l, 4, l); mem_wr(l + 4, 4, l); }
        unicode_string(pp + 0x24, g_pp_strings[0]); // CurrentDirectory.DosPath
        unicode_string(pp + 0x38, g_pp_strings[1]); // ImagePathName
        unicode_string(pp + 0x40, g_pp_strings[2]); // CommandLine
        return;
    }
    mem_wr(pebv + 0x10, 8, p->main_module ? p->main_module->base : 0);
    mem_wr(pebv + 0x18, 8, ldrv);
    mem_wr(pebv + 0x20, 8, H2G(pp));
    mem_wr(pebv + 0x30, 8, hs()->process_heap);
    mem_wr(pebv + 0x118, 4, 10);
    mem_wr(pebv + 0x11C, 4, 0);
    mem_wr(pebv + 0x120, 2, 19045);
    mem_wr(pebv + 0x124, 4, 2);
    mem_wr(ldrv + 0x4, 1, 1);
    for (int i = 0; i < 3; i++) { uint64_t l = ldrv + 0x10 + 16 * (uint64_t)i; mem_wr(l, 8, l); mem_wr(l + 8, 8, l); }
    unicode_string(pp + 0x38, g_pp_strings[0]);
    unicode_string(pp + 0x60, g_pp_strings[1]);
    unicode_string(pp + 0x70, g_pp_strings[2]);
}

static X86State *new_state(SoulThread *t) {
    X86State *st = NULL;
    if (posix_memalign((void **)&st, 64, sizeof(X86State)) != 0) return NULL;
    memset(st, 0, sizeof *st);
    st->eflags = 0x202;
    st->mxcsr = 0x1F80;
    st->gbase = g_gbase;
    x87_reset(st);
    st->thread = t;
    t->st = st;
    return st;
}

static void *thread_main(void *arg) {
    SoulThread *t = arg;
    SoulProcess *p = t->proc;
    gil_lock(t);
    while (t->suspended && !p->terminate) gil_wait(-1);
    sigjmp_buf jb;
    t->jb = &jb;
    int r = sigsetjmp(jb, 1);
    if (r == 0 && !p->terminate) {
        pe_init_thread_tls(p, t->st, t->teb, 1);
        uint64_t args[1] = { t->start_arg };
        t->exit_code = (uint32_t)soul_guest_call(t->st, t->start_fn, 1, args);
    } else if (r == 3) {
        char buf[1200];
        int n = snprintf(buf, sizeof buf, "\n[Soul] İş parçacığı çöktü: %s\n", p->error);
        soul_out(SOUL_STREAM_LOG, buf, (size_t)n);
    }
    con_flush();
    t->jb = NULL;
    p->retired_jit += t->st->icount_jit;
    p->retired_interp += t->st->icount_interp;
    t->st->icount_jit = t->st->icount_interp = 0;
    t->finished = 1;
    gil_notify();
    gil_unlock(t);
    return NULL;
}

SoulThread *soul_thread_create(X86State *st, uint64_t fn, uint64_t arg, size_t stack, int suspended) {
    SoulProcess *p = g_proc;
    (void)st;
    SoulThread *t = calloc(1, sizeof *t);
    t->proc = p;
    if (!new_state(t)) { free(t); return NULL; }
    t->tid = soul_new_tid();
    t->stack_size = stack < (1u << 20) ? (4u << 20) : (stack > (64u << 20) ? (64u << 20) : stack);
    t->stack_mem = g_pages(t->stack_size);
    if (!t->stack_mem) { free(t->st); free(t); return NULL; }
    t->stack_limit = H2G(t->stack_mem);
    t->stack_base = t->stack_limit + t->stack_size;
    t->st->gpr[RSP] = t->stack_base - 64;
    make_teb(p, t);
    t->start_fn = fn;
    t->start_arg = arg;
    t->suspended = suspended;
    t->next = p->threads;
    p->threads = t;
    ticker_start();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&t->host, &attr, thread_main, t) != 0) {
        pthread_attr_destroy(&attr);
        t->finished = 1;
        return NULL;
    }
    pthread_attr_destroy(&attr);
    return t;
}

void soul_thread_resume(SoulThread *t) {
    t->suspended = 0;
    gil_notify();
}

static void win_path_from_host(SoulProcess *p, char *out, size_t outlen) {
    size_t dl = strlen(p->drive_c);
    if (strncmp(p->exe_host, p->drive_c, dl) == 0 && p->exe_host[dl] == '/') {
        size_t o = (size_t)snprintf(out, outlen, "C:");
        for (const char *s = p->exe_host + dl; *s && o + 1 < outlen; s++) out[o++] = *s == '/' ? '\\' : *s;
        out[o] = 0;
    } else {
        const char *b = strrchr(p->exe_host, '/');
        snprintf(out, outlen, "C:\\%s", b ? b + 1 : p->exe_host);
    }
}


// ---- processes -------------------------------------------------------------------------------------

static SoulProcess *proc_alloc(const SoulLaunchOptions *o) {
    SoulProcess *p = calloc(1, sizeof *p);
    p->opts = *o;
    p->verbose = o->verbose;
    pthread_mutex_init(&p->in_mu, NULL);
    pthread_cond_init(&p->in_cv, NULL);
    g_next_pid += 4;
    p->pid = g_next_pid;
    p->main_thread.tid = soul_new_tid();
    p->main_thread.is_main = 1;
    p->main_thread.proc = p;
    p->std_fd[0] = p->std_fd[1] = p->std_fd[2] = -1;
    p->pvars = proc_vars_new();
    return p;
}

// Loads the program into p, which must be the active process.
static int proc_load(SoulProcess *p, char **env, char *err, size_t errlen) {
    // 32-bit (x86) programs run in a 4 GB guest window; 64-bit ones use host addresses directly.
    g_mode32 = 0;
    if (pe_machine(p->exe_host) == 0x14C) {
        if (!gwin_init()) { snprintf(err, errlen, "32-bit program için bellek penceresi ayrılamadı"); return -1; }
        g_mode32 = 1;
    }
    gheap_reset();
    if (g_mode32) {
        uint64_t a = gwin_map(0x7FE00000ull, (size_t)SOUL_MAX_THUNKS * SOUL_THUNK_STRIDE);
        p->thunk_mem = a ? G2H(a) : NULL;
    } else {
        p->thunk_mem = g_pages((size_t)SOUL_MAX_THUNKS * SOUL_THUNK_STRIDE);
    }
    if (!p->thunk_mem) { snprintf(err, errlen, "Bellek ayrılamadı"); return -1; }
    p->thunk_base = H2G(p->thunk_mem);
    p->thunk_count = 1; // id 0 is the guest-call return trap
    p->trap_addr = p->thunk_base;
    p->thunk_mem[0] = 0xCC; p->thunk_mem[1] = 0x53; p->thunk_mem[2] = 0x43; p->thunk_mem[7] = 0xC3;

    hle_init(p, env);
    crt_reset();

    X86State *st = new_state(&p->main_thread);
    if (!st) { snprintf(err, errlen, "Bellek ayrılamadı"); return -1; }
    p->main_state = st;

    PEModule *m = pe_load(p, p->exe_host, p->exe_win, err, errlen);
    if (!m) return -1;
    if (m->is_dll) { snprintf(err, errlen, "Bu dosya bir DLL; doğrudan çalıştırılamaz"); return -1; }
    p->main_module = m;

    size_t stack = m->stack_reserve > (8u << 20) ? (size_t)m->stack_reserve : (8u << 20);
    if (stack > (256u << 20)) stack = 256u << 20;
    if (g_mode32 && stack > (64u << 20)) stack = 64u << 20;
    void *sk = g_pages(stack);
    if (!sk) { snprintf(err, errlen, "Yığın ayrılamadı"); return -1; }
    p->main_thread.stack_limit = H2G(sk);
    p->main_thread.stack_base = H2G(sk) + stack;
    st->gpr[RSP] = p->main_thread.stack_base - 64;
    setup_peb(p);
    make_teb(p, &p->main_thread);
    p->use_jit = p->jit != NULL;
    return 0;
}

// Frees what the process owns (memory, handles, windows); p must be the active process and its
// other threads must have ended.
static void proc_release(SoulProcess *p) {
    if (p->released) return;
    p->released = 1;
    gui_process_exit(p->pid);
    if (p->hle_state) hle_shutdown(p);
    crt_process_end();
    pe_unload_all(p);
    if (p->main_thread.stack_limit) g_pages_free(G2H(p->main_thread.stack_limit), (size_t)(p->main_thread.stack_base - p->main_thread.stack_limit));
    p->main_thread.stack_limit = p->main_thread.stack_base = 0;
    free_teb(&p->main_thread);
    int stuck = 0;
    for (SoulThread *t = p->threads; t; t = t->next) if (!t->finished) stuck = 1;
    while (!stuck && p->threads) {
        SoulThread *t = p->threads;
        p->threads = t->next;
        pthread_join(t->host, NULL);
        free_teb(t);
        if (t->stack_mem) g_pages_free(t->stack_mem, t->stack_size);
        free(t->st);
        free(t);
    }
    for (int i = 0; i < 3; i++) { g_free(g_pp_strings[i]); g_pp_strings[i] = NULL; }
    if (g_peb_block) { g_pages_free(g_peb_block, 0x2000); g_peb_block = NULL; }
    if (p->thunk_mem) g_pages_free(p->thunk_mem, (size_t)SOUL_MAX_THUNKS * SOUL_THUNK_STRIDE);
    p->thunk_mem = NULL;
    for (uint32_t i = 1; i < p->thunk_count; i++) { free((void *)p->thunks[i].dll); free((void *)p->thunks[i].name); p->thunks[i].dll = p->thunks[i].name = NULL; }
    p->thunk_count = 0;
    free(p->main_state);
    p->main_state = NULL;
    p->main_thread.st = NULL;
    gwin_release();
    gheap_reset();
    g_mode32 = 0;
}

// Runs the program on its main thread (the GIL is held, p active) until it ends, then stops
// its other threads. Returns with the GIL held and p active.
static void run_main_thread(SoulProcess *p) {
    X86State *st = p->main_state;
    sigjmp_buf jb;
    p->main_thread.jb = &jb;
    p->start_time = soul_now();
    int r = sigsetjmp(jb, 1);
    if (r == 0) {
        while (p->main_thread.suspended && !p->terminate) gil_wait(-1); // CREATE_SUSPENDED
        if (p->terminate) {
            if (p->exit_requested) p->exit_code = p->requested_code;
        } else {
            pe_run_initializers(p, st);
            uint64_t ret = soul_guest_call(st, p->main_module->entry, 0, NULL);
            crt_run_atexit(st);
            p->exit_code = (int)(uint32_t)ret;
        }
    }
    con_flush();
    crt_flush_all();
    if (r == 3) {
        char buf[1200];
        int n = p == g_root ? snprintf(buf, sizeof buf, "\n[Soul] Program çöktü: %s\n", p->error)
                            : snprintf(buf, sizeof buf, "\n[Soul] Program çöktü (%s): %s\n", p->exe_win, p->error);
        soul_out(SOUL_STREAM_LOG, buf, (size_t)n);
    }
    p->main_thread.jb = NULL;
    // the process is over: stop every other guest thread and wait for them
    if (p->threads) {
        int code = p->exit_code;
        if (!p->terminate) { // normal end of the program: other threads just stop
            p->exit_requested = 1;
            p->requested_code = code;
        }
        stop_all_threads(p);
        gil_unlock(&p->main_thread);
        for (int i = 0; i < 300; i++) {
            int alive = 0;
            for (SoulThread *t = p->threads; t; t = t->next) if (!t->finished) alive = 1;
            if (!alive) break;
            usleep(10000);
        }
        gil_lock(&p->main_thread);
        p->exit_code = code;
    }
    p->main_thread.exit_code = (uint32_t)p->exit_code;
    p->main_thread.finished = 1;
    p->exited = 1;
    gui_process_exit(p->pid); // its windows disappear right away
    gil_notify();
}

static void link_process(SoulProcess *p) {
    SoulProcess **pp = &g_procs;
    while (*pp) pp = &(*pp)->next_proc;
    *pp = p;
}

int soul_process_create(const SoulLaunchOptions *o, SoulProcess **out, char *err, size_t errlen) {
    *out = NULL;
    if (g_root) { snprintf(err, errlen, "Başka bir program zaten çalışıyor"); return -1; }
    if (!o || !o->exe_path || !o->drive_c) { snprintf(err, errlen, "Eksik başlatma seçenekleri"); return -1; }
    pthread_once(&g_sys_once, sys_init);
    mkdir(o->drive_c, 0755);
    char rp[PATH_MAX];
    if (!realpath(o->exe_path, rp)) { snprintf(err, errlen, "Program dosyası bulunamadı: %s", o->exe_path); return -1; }
    SoulProcess *p = proc_alloc(o);
    snprintf(p->exe_host, sizeof p->exe_host, "%s", rp);
    if (realpath(o->drive_c, rp)) snprintf(p->drive_c, sizeof p->drive_c, "%s", rp);
    else snprintf(p->drive_c, sizeof p->drive_c, "%s", o->drive_c);
    if (o->exe_win_path && *o->exe_win_path) snprintf(p->exe_win, sizeof p->exe_win, "%s", o->exe_win_path);
    else win_path_from_host(p, p->exe_win, sizeof p->exe_win);
    snprintf(p->cwd_win, sizeof p->cwd_win, "%s", p->exe_win);
    char *bs = strrchr(p->cwd_win, '\\');
    if (bs && bs - p->cwd_win > 2) *bs = 0; else if (bs) bs[1] = 0;
    snprintf(p->cmdline, sizeof p->cmdline, "\"%s\"%s%s", p->exe_win, o->args && *o->args ? " " : "", o->args ? o->args : "");

    g_proc = NULL;
    proc_activate(p);
    g_root = p;
    g_procs = p;
    g_jit_on = o->jit_enabled && jitmem_ready();
    memset(g_jit_slice_pid, 0, sizeof g_jit_slice_pid);
    p->jit_slice = -1;
    if (g_jit_on) p->jit = jit_create(p);
    hle_init_system(p);
    if (proc_load(p, NULL, err, errlen) != 0) {
        proc_release(p);
        jit_detach(p);
        hle_shutdown_system();
        proc_vars_reset();
        g_proc = NULL; g_root = NULL; g_procs = NULL;
        free(p->pvars);
        free(p);
        return -1;
    }
    *out = p;
    return 0;
}

// A process started by a guest program (CreateProcess / ShellExecute). Called with the GIL held
// by a thread of the parent; returns with the parent active again.
static void *proc_main(void *arg) {
    SoulProcess *p = arg;
    gil_lock(&p->main_thread);
    run_main_thread(p);
    if (p->verbose) soul_log("[süreç] %s (#%u) sona erdi: %d\n", p->exe_win, p->pid, p->exit_code);
    if (g_root) { g_root->retired_jit += p->retired_jit; g_root->retired_interp += p->retired_interp; }
    proc_release(p);
    jit_detach(p);
    gil_notify();
    gil_unlock(&p->main_thread);
    return NULL;
}

SoulProcess *soul_process_spawn(const SoulSpawn *s, char *err, size_t errlen) {
    SoulProcess *parent = g_proc;
    SoulLaunchOptions o = parent->opts;
    o.exe_path = o.exe_win_path = o.args = o.drive_c = NULL;
    SoulProcess *p = proc_alloc(&o);
    p->parent_pid = parent->pid;
    snprintf(p->drive_c, sizeof p->drive_c, "%s", parent->drive_c);
    snprintf(p->exe_host, sizeof p->exe_host, "%s", s->exe_host);
    snprintf(p->exe_win, sizeof p->exe_win, "%s", s->exe_win);
    snprintf(p->cmdline, sizeof p->cmdline, "%s", s->cmdline);
    snprintf(p->cwd_win, sizeof p->cwd_win, "%s", s->cwd_win && *s->cwd_win ? s->cwd_win : parent->cwd_win);
    for (int i = 0; i < 3; i++) p->std_fd[i] = s->std_fd[i];
    p->in_closed = 1; // console input belongs to the program the UI started
    p->jit_slice = -1;
    jit_attach(p);
    proc_activate(p);
    if (proc_load(p, s->env, err, errlen) != 0) {
        proc_release(p);
        jit_detach(p);
        proc_activate(parent);
        for (int i = 0; i < 3; i++) if (p->std_fd[i] >= 0) close(p->std_fd[i]);
        free(p->pvars);
        free(p);
        return NULL;
    }
    p->main_thread.suspended = s->suspended;
    link_process(p);
    proc_activate(parent);
    if (parent->verbose) soul_log("[süreç] %s başlatıldı (#%u)\n", p->exe_win, p->pid);
    ticker_start();
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&p->main_thread.host, &attr, proc_main, p) != 0) {
        pthread_attr_destroy(&attr);
        p->exited = 1;
        p->main_thread.finished = 1;
        p->exit_code = -1;
        return p;
    }
    p->host_started = 1;
    pthread_attr_destroy(&attr);
    return p;
}

void soul_process_kill(SoulProcess *p, int code) {
    if (!p || p->exited) return;
    p->exit_requested = 1;
    p->requested_code = code;
    p->main_thread.suspended = 0;
    stop_all_threads(p);
}

SoulProcess *soul_process_by_pid(uint32_t pid) {
    for (SoulProcess *p = g_procs; p; p = p->next_proc) if (p->pid == pid) return p;
    return NULL;
}

int soul_process_run(SoulProcess *p) {
    p->guest_thread = pthread_self();
    gil_lock(&p->main_thread);
    install_handlers();
    run_main_thread(p);
    gil_unlock(&p->main_thread);
    // the session lasts while programs it started are still running
    for (;;) {
        int alive = 0;
        for (SoulProcess *c = g_procs; c; c = c->next_proc) if (c != p && !(c->exited && c->released)) alive = 1;
        if (!alive) break;
        if (p->terminate && !p->exit_requested)
            for (SoulProcess *c = g_procs; c; c = c->next_proc) if (c != p && !c->exited) soul_process_kill(c, -1);
        pthread_mutex_lock(&g_ev_mu);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_ev_cv, &g_ev_mu, &ts);
        pthread_mutex_unlock(&g_ev_mu);
    }
    g_ticker_running = 0;
    restore_handlers();
    return p->exit_code;
}

void soul_process_terminate(SoulProcess *p) {
    if (!p) return;
    if (p == g_root) for (SoulProcess *c = g_procs; c; c = c->next_proc) if (c != p) { c->main_thread.suspended = 0; stop_all_threads(c); }
    stop_all_threads(p);
}

void soul_process_send_input(SoulProcess *p, const char *data, size_t len) {
    if (!p) return;
    pthread_mutex_lock(&p->in_mu);
    if (p->in_len + len > p->in_cap) {
        p->in_cap = (p->in_len + len) * 2 + 256;
        p->in_buf = realloc(p->in_buf, p->in_cap);
    }
    memcpy(p->in_buf + p->in_len, data, len);
    p->in_len += len;
    pthread_cond_broadcast(&p->in_cv);
    pthread_mutex_unlock(&p->in_mu);
}

void soul_process_close_input(SoulProcess *p) {
    if (!p) return;
    pthread_mutex_lock(&p->in_mu);
    p->in_closed = 1;
    pthread_cond_broadcast(&p->in_cv);
    pthread_mutex_unlock(&p->in_mu);
}

void soul_process_stats(SoulProcess *p, SoulStats *s) {
    memset(s, 0, sizeof *s);
    if (!p) return;
    if (p->jit) jit_stats(p->jit, s);
    s->jit_insns = p->retired_jit;
    s->interp_insns = p->retired_interp;
    for (SoulProcess *q = g_procs; q; q = q->next_proc) {
        if (q->main_state && !q->released) { s->jit_insns += q->main_state->icount_jit; s->interp_insns += q->main_state->icount_interp; }
        for (SoulThread *t = q->threads; t; t = t->next)
            if (t->st && !t->finished) { s->jit_insns += t->st->icount_jit; s->interp_insns += t->st->icount_interp; }
    }
    s->api_calls = p->retired_api;
    for (SoulProcess *q = g_procs; q; q = q->next_proc) if (q->hle_state) s->api_calls += ((HleState *)q->hle_state)->api_calls;
    s->jit_active = p->jit != NULL;
    s->seconds = p->start_time > 0 ? soul_now() - p->start_time : 0;
}

const char *soul_process_error(SoulProcess *p) { return p ? p->error : ""; }

void soul_process_destroy(SoulProcess *p) {
    if (!p) return;
    g_ticker_running = 0;
    if (g_ticker) { pthread_join(g_ticker, NULL); g_ticker = 0; }
    int stuck = 0;
    for (SoulProcess *c = g_procs; c; c = c->next_proc) {
        if (c == p || !c->host_started) continue;
        if (c->exited && c->released) pthread_join(c->main_thread.host, NULL);
        else { stuck = 1; pthread_detach(c->main_thread.host); }
    }
    for (SoulThread *t = p->threads; t; t = t->next) if (!t->finished) stuck = 1;
    if (stuck) {
        // A guest thread never returned (e.g. blocked in a UI callback). Its memory is
        // intentionally leaked instead of freeing it under its feet.
        soul_log("[Soul] Uyarı: bazı iş parçacıkları sonlanmadı; belleği serbest bırakılmadı.\n");
        g_proc = NULL; g_root = NULL; g_procs = NULL;
        return;
    }
    proc_activate(p);
    proc_release(p);
    jit_detach(p);
    hle_shutdown_system();
    memset(g_jit_slice_pid, 0, sizeof g_jit_slice_pid);
    while (g_procs) {
        SoulProcess *c = g_procs;
        g_procs = c->next_proc;
        if (c == p) continue;
        for (int i = 0; i < 3; i++) if (c->std_fd[i] >= 0) close(c->std_fd[i]);
        free(c->in_buf);
        pthread_mutex_destroy(&c->in_mu);
        pthread_cond_destroy(&c->in_cv);
        free(c->pvars);
        free(c);
    }
    free(p->in_buf);
    pthread_mutex_destroy(&p->in_mu);
    pthread_cond_destroy(&p->in_cv);
    proc_vars_reset();
    g_proc = NULL;
    g_root = NULL;
    free(p->pvars);
    free(p);
}
