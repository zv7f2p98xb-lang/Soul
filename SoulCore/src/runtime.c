// SoulCore - process lifecycle, dispatcher loop, guest calls and fault handling.
#include "hle.h"
#include <errno.h>
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

const char *soul_version(void) { return "SoulCore 0.3.0"; }

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

void soul_fatal(X86State *st, const char *fmt, ...) {
    SoulProcess *p = g_proc;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error, sizeof p->error, fmt, ap);
    va_end(ap);
    con_flush();
    char buf[1200];
    int n = snprintf(buf, sizeof buf, "\n[Soul] Program durduruldu: %s\n", p->error);
    soul_out(SOUL_STREAM_LOG, buf, (size_t)n);
    p->exit_code = -1;
    SoulThread *t = st && st->thread ? st->thread : &p->main_thread;
    siglongjmp(*t->jb, 2);
}

void soul_exit_process(X86State *st, int code) {
    SoulProcess *p = g_proc;
    p->exit_code = code;
    con_flush();
    SoulThread *t = st && st->thread ? st->thread : &p->main_thread;
    siglongjmp(*t->jb, 1);
}

void soul_flush_code_cache(void) {
    if (g_proc && g_proc->jit) jit_flush(g_proc->jit);
}

// ---- dispatcher ---------------------------------------------------------------------------------

void soul_run_until_trap(X86State *st) {
    SoulProcess *p = g_proc;
    const uint64_t tb = p->thunk_base;
    const uint64_t tsize = (uint64_t)SOUL_MAX_THUNKS * SOUL_THUNK_STRIDE;
    JitCache *jit = p->jit;
    for (;;) {
        uint64_t rip = st->rip;
        if (rip - tb < tsize) {
            if (rip == p->trap_addr) return;
            hle_invoke(st, (uint32_t)((rip - tb) / SOUL_THUNK_STRIDE));
            continue;
        }
        if (__builtin_expect(p->terminate, 0)) {
            snprintf(p->error, sizeof p->error, "Kullanıcı tarafından durduruldu");
            p->exit_code = -1;
            siglongjmp(*st->thread->jb, 2);
        }
        if (jit && jit_run(jit, st)) continue;
        x86_interp_step(st);
    }
}

uint64_t soul_guest_call(X86State *st, uint64_t fn, int nargs, const uint64_t *args) {
    SoulProcess *p = g_proc;
    uint64_t saved_rip = st->rip, saved_rsp = st->gpr[RSP];
    SoulThread *t = st->thread;
    if (++t->call_depth > 200) soul_fatal(st, "İç içe geri çağrı derinliği aşıldı");
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
    if (!p || !p->main_thread.jb || g_in_fault) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_in_fault = 1;
    X86State *st = p->main_state;
    const char *what = sig == SIGSEGV ? "Bellek erişim ihlali" : sig == SIGBUS ? "Hizalama/veri yolu hatası"
                     : sig == SIGILL ? "Geçersiz komut" : "Aritmetik hata";
    snprintf(p->error, sizeof p->error, "%s: adres 0x%llx (x86 RIP ~ 0x%llx)", what,
             (unsigned long long)(uintptr_t)si->si_addr,
             (unsigned long long)(st->thread && st->thread->block_rip ? st->thread->block_rip : st->rip));
    p->exit_code = (int)0xC0000005;
    g_in_fault = 0;
    siglongjmp(*p->main_thread.jb, 3);
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

static void unicode_string(uint8_t *at, const uint16_t *s) {
    uint16_t len = (uint16_t)(wstrlen(s) * 2), max = (uint16_t)(len + 2);
    uint64_t ptr = (uint64_t)(uintptr_t)s;
    memcpy(at, &len, 2); memcpy(at + 2, &max, 2); memcpy(at + 8, &ptr, 8);
}

static uint8_t *g_teb_block;
static size_t g_teb_size;
static uint16_t *g_pp_strings[3];

static void setup_teb(SoulProcess *p, X86State *st) {
    g_teb_size = 0x4000;
    g_teb_block = mmap(NULL, g_teb_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *teb = g_teb_block, *peb = g_teb_block + 0x2000, *ldr = g_teb_block + 0x3000, *pp = g_teb_block + 0x3200;
    uint64_t tebv = (uint64_t)(uintptr_t)teb;
    uint64_t *tls = calloc(64, sizeof(uint64_t));
    mem_wr(tebv + 0x08, 8, p->main_thread.stack_base);
    mem_wr(tebv + 0x10, 8, p->main_thread.stack_limit);
    mem_wr(tebv + 0x30, 8, tebv);
    mem_wr(tebv + 0x40, 8, 4242);
    mem_wr(tebv + 0x48, 8, 4243);
    mem_wr(tebv + 0x58, 8, (uint64_t)(uintptr_t)tls);
    mem_wr(tebv + 0x60, 8, (uint64_t)(uintptr_t)peb);
    // PEB
    uint64_t pebv = (uint64_t)(uintptr_t)peb;
    mem_wr(pebv + 0x10, 8, p->main_module ? p->main_module->base : 0);
    mem_wr(pebv + 0x18, 8, (uint64_t)(uintptr_t)ldr);
    mem_wr(pebv + 0x20, 8, (uint64_t)(uintptr_t)pp);
    mem_wr(pebv + 0x30, 8, hs()->process_heap);
    mem_wr(pebv + 0x118, 4, 10);
    mem_wr(pebv + 0x11C, 4, 0);
    mem_wr(pebv + 0x120, 2, 19045);
    mem_wr(pebv + 0x124, 4, 2);
    // empty loader lists that point to themselves
    uint64_t ldrv = (uint64_t)(uintptr_t)ldr;
    mem_wr(ldrv + 0x4, 1, 1);
    for (int i = 0; i < 3; i++) { uint64_t l = ldrv + 0x10 + 16 * (uint64_t)i; mem_wr(l, 8, l); mem_wr(l + 8, 8, l); }
    // process parameters
    for (int i = 0; i < 3; i++) free(g_pp_strings[i]);
    g_pp_strings[0] = u2w_dup(p->cwd_win);
    g_pp_strings[1] = u2w_dup(p->exe_win);
    g_pp_strings[2] = u2w_dup(p->cmdline);
    unicode_string(pp + 0x38, g_pp_strings[0]);
    unicode_string(pp + 0x60, g_pp_strings[1]);
    unicode_string(pp + 0x70, g_pp_strings[2]);
    st->gs_base = tebv;
    p->main_thread.teb = tebv;
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

int soul_process_create(const SoulLaunchOptions *o, SoulProcess **out, char *err, size_t errlen) {
    *out = NULL;
    if (g_proc) { snprintf(err, errlen, "Başka bir program zaten çalışıyor"); return -1; }
    if (!o || !o->exe_path || !o->drive_c) { snprintf(err, errlen, "Eksik başlatma seçenekleri"); return -1; }
    SoulProcess *p = calloc(1, sizeof *p);
    p->opts = *o;
    p->verbose = o->verbose;
    pthread_mutex_init(&p->in_mu, NULL);
    pthread_cond_init(&p->in_cv, NULL);
    mkdir(o->drive_c, 0755);
    char rp[PATH_MAX];
    if (realpath(o->drive_c, rp)) snprintf(p->drive_c, sizeof p->drive_c, "%s", rp);
    else snprintf(p->drive_c, sizeof p->drive_c, "%s", o->drive_c);
    if (!realpath(o->exe_path, rp)) {
        snprintf(err, errlen, "Program dosyası bulunamadı: %s", o->exe_path);
        free(p);
        return -1;
    }
    snprintf(p->exe_host, sizeof p->exe_host, "%s", rp);
    if (o->exe_win_path && *o->exe_win_path) snprintf(p->exe_win, sizeof p->exe_win, "%s", o->exe_win_path);
    else win_path_from_host(p, p->exe_win, sizeof p->exe_win);
    snprintf(p->cwd_win, sizeof p->cwd_win, "%s", p->exe_win);
    char *bs = strrchr(p->cwd_win, '\\');
    if (bs && bs - p->cwd_win > 2) *bs = 0; else if (bs) bs[1] = 0;
    snprintf(p->cmdline, sizeof p->cmdline, "\"%s\"%s%s", p->exe_win, o->args && *o->args ? " " : "", o->args ? o->args : "");

    p->thunk_mem = mmap(NULL, (size_t)SOUL_MAX_THUNKS * SOUL_THUNK_STRIDE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p->thunk_mem == MAP_FAILED) { snprintf(err, errlen, "Bellek ayrılamadı"); free(p); return -1; }
    p->thunk_base = (uint64_t)(uintptr_t)p->thunk_mem;
    p->thunk_count = 1; // id 0 is the guest-call return trap
    p->trap_addr = p->thunk_base;
    p->thunk_mem[0] = 0xCC; p->thunk_mem[1] = 0x53; p->thunk_mem[2] = 0x43; p->thunk_mem[7] = 0xC3;

    g_proc = p;
    hle_init(p);
    crt_reset();

    X86State *st = NULL;
    if (posix_memalign((void **)&st, 64, sizeof(X86State)) != 0) { snprintf(err, errlen, "Bellek ayrılamadı"); g_proc = NULL; free(p); return -1; }
    memset(st, 0, sizeof *st);
    st->eflags = 0x202;
    st->mxcsr = 0x1F80;
    x87_reset(st);
    st->thread = &p->main_thread;
    p->main_state = st;
    p->main_thread.st = st;
    p->main_thread.proc = p;

    PEModule *m = pe_load(p, p->exe_host, p->exe_win, err, errlen);
    if (!m) {
        hle_shutdown(p);
        pe_unload_all(p);
        munmap(p->thunk_mem, (size_t)SOUL_MAX_THUNKS * SOUL_THUNK_STRIDE);
        free(st);
        g_proc = NULL;
        free(p);
        return -1;
    }
    if (m->is_dll) {
        snprintf(err, errlen, "Bu dosya bir DLL; doğrudan çalıştırılamaz");
        soul_process_destroy(p);
        return -1;
    }
    p->main_module = m;

    size_t stack = m->stack_reserve > (8u << 20) ? (size_t)m->stack_reserve : (8u << 20);
    if (stack > (256u << 20)) stack = 256u << 20;
    void *sk = mmap(NULL, stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (sk == MAP_FAILED) { snprintf(err, errlen, "Yığın ayrılamadı"); soul_process_destroy(p); return -1; }
    p->main_thread.stack_limit = (uint64_t)(uintptr_t)sk;
    p->main_thread.stack_base = (uint64_t)(uintptr_t)sk + stack;
    st->gpr[RSP] = p->main_thread.stack_base - 64;
    setup_teb(p, st);

    if (m->subsystem == 2)
        soul_log("[Soul] Not: Bu bir grafik arayüzlü (GUI) Windows programı. Soul şu anda pencere çizimini desteklemiyor; konsol çıktısı ve mesaj kutuları gösterilecek.\n");
    if (o->jit_enabled && jitmem_ready()) p->jit = jit_create(p);
    p->use_jit = p->jit != NULL;
    *out = p;
    return 0;
}

int soul_process_run(SoulProcess *p) {
    g_proc = p;
    X86State *st = p->main_state;
    sigjmp_buf jb;
    p->main_thread.jb = &jb;
    p->start_time = soul_now();
    install_handlers();
    int r = sigsetjmp(jb, 1);
    if (r == 0) {
        pe_run_initializers(p, st);
        uint64_t ret = soul_guest_call(st, p->main_module->entry, 0, NULL);
        crt_run_atexit(st);
        p->exit_code = (int)(uint32_t)ret;
    }
    restore_handlers();
    con_flush();
    crt_flush_all();
    p->main_thread.jb = NULL;
    p->exited = 1;
    return p->exit_code;
}

void soul_process_terminate(SoulProcess *p) {
    if (!p) return;
    p->terminate = 1;
    if (p->main_state) p->main_state->stop_req = 1;
    pthread_mutex_lock(&p->in_mu);
    pthread_cond_broadcast(&p->in_cv);
    pthread_mutex_unlock(&p->in_mu);
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
    if (p->main_state) {
        s->jit_insns = p->main_state->icount_jit;
        s->interp_insns = p->main_state->icount_interp;
    }
    s->api_calls = hs()->api_calls;
    s->jit_active = p->jit != NULL;
    s->seconds = p->start_time > 0 ? soul_now() - p->start_time : 0;
}

const char *soul_process_error(SoulProcess *p) { return p ? p->error : ""; }

void soul_process_destroy(SoulProcess *p) {
    if (!p) return;
    if (p->jit) jit_destroy(p->jit);
    hle_shutdown(p);
    crt_process_end();
    pe_unload_all(p);
    if (p->main_thread.stack_limit) munmap((void *)(uintptr_t)p->main_thread.stack_limit, (size_t)(p->main_thread.stack_base - p->main_thread.stack_limit));
    if (g_teb_block) {
        free((void *)(uintptr_t)mem_rd((uint64_t)(uintptr_t)g_teb_block + 0x58, 8));
        munmap(g_teb_block, g_teb_size);
        g_teb_block = NULL;
    }
    if (p->thunk_mem) munmap(p->thunk_mem, (size_t)SOUL_MAX_THUNKS * SOUL_THUNK_STRIDE);
    for (uint32_t i = 1; i < p->thunk_count; i++) { free((void *)p->thunks[i].dll); free((void *)p->thunks[i].name); }
    free(p->main_state);
    free(p->in_buf);
    pthread_mutex_destroy(&p->in_mu);
    pthread_cond_destroy(&p->in_cv);
    if (g_proc == p) g_proc = NULL;
    free(p);
}
