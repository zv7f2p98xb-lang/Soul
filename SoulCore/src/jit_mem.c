// SoulCore - executable memory for the JIT.
//
// iOS does not allow normal apps to create executable memory. Sideloaded apps
// can get it while a debugger is attached (StikDebug). Two methods exist:
//
//  * TXM (iOS/iPadOS 26+ on TXM devices, e.g. M-series iPads): the app executes
//    `brk #0xf00d` with x16 = 1 (JIT26PrepareRegion, x0 = NULL, x1 = size).
//    StikDebug's universal.js script allocates an RX region, prepares it and
//    returns its address in x0. The app writes through an RW alias created with
//    vm_remap + vm_protect. The debugger is NOT detached afterwards: execute
//    permission may depend on it staying attached (StikDebug keeps running in
//    the background with Picture in Picture).
//
//  * Legacy (iOS 14-18 and non-TXM devices): once CS_DEBUGGED is set, RW memory
//    gets a second RX mapping via vm_remap.
//
// The debugger attaches *after* the app has started, so the app polls
// CS_DEBUGGED (see JITManager.swift). Every candidate region is verified by
// writing and executing a tiny probe function before the JIT may use it;
// all faults during setup are caught so a broken setup never crashes the app.
#include "soul_internal.h"
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

static uint8_t *g_rw, *g_rx;
static void (*g_wp)(int); // pthread_jit_write_protect_np (simulator)
static size_t g_size;
static char g_method[256] = "JIT kapalı";
static char g_log[8192];

void *jitmem_rw(void) { return g_rw; }
void *jitmem_rx(void) { return g_rx; }
size_t jitmem_size(void) { return g_size; }
int jitmem_ready(void) { return g_rx != NULL && soul_host_is_arm64(); }
int soul_jit_available(void) { return jitmem_ready(); }
const char *soul_jit_method(void) { return g_method; }
const char *soul_jit_log(void) { return g_log; }

static void jitlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void jitlog(const char *fmt, ...) {
    size_t used = strlen(g_log);
    if (used + 200 >= sizeof g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_log + used, sizeof g_log - used - 1, fmt, ap);
    va_end(ap);
    used = strlen(g_log);
    g_log[used] = '\n';
    g_log[used + 1] = 0;
}

void jitmem_flush_icache(void *rx, size_t len) {
#if defined(__APPLE__)
    sys_icache_invalidate(rx, len);
#else
    __builtin___clear_cache((char *)rx, (char *)rx + len);
#endif
}

void jitmem_begin_write(void) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(0);
#else
    if (g_wp) g_wp(0);
#endif
}
void jitmem_end_write(void) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(1);
#else
    if (g_wp) g_wp(1);
#endif
}

#if defined(__APPLE__)
#define CS_OPS_STATUS 0
#define CS_DEBUGGED 0x10000000
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#endif

uint32_t soul_jit_cs_flags(void) {
#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    uint32_t flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags) != 0) return 0;
    return flags;
#else
    return 0;
#endif
}

int soul_jit_is_debugged(void) {
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
    return 1; // the simulator is a macOS process: MAP_JIT works without a debugger
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return (soul_jit_cs_flags() & CS_DEBUGGED) != 0;
#else
    return 1; // desktop hosts can always map executable memory
#endif
}

// ---- guarded execution: catch SIGTRAP/SIGSEGV/SIGBUS/SIGILL during setup --------------------

static sigjmp_buf g_guard_jb;
static volatile int g_guard_sig;
static const int k_guard_sigs[4] = { SIGTRAP, SIGSEGV, SIGBUS, SIGILL };
typedef struct { struct sigaction old[4]; } Guard;
static void guard_handler(int sig) { g_guard_sig = sig; siglongjmp(g_guard_jb, 1); }
static void guard_on(Guard *g) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = guard_handler;
    sa.sa_flags = SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    g_guard_sig = 0;
    for (int i = 0; i < 4; i++) sigaction(k_guard_sigs[i], &sa, &g->old[i]);
}
static void guard_off(Guard *g) {
    for (int i = 0; i < 4; i++) sigaction(k_guard_sigs[i], &g->old[i], NULL);
}

// Writes `mov w0, #42; ret` through rw and executes it through rx.
static int probe(uint8_t *rw, uint8_t *rx) {
    static const uint32_t code[2] = { 0x52800540u, 0xD65F03C0u };
    Guard g;
    volatile int stage = 0, result = -1;
    guard_on(&g);
    if (sigsetjmp(g_guard_jb, 1) == 0) {
        stage = 1;
        jitmem_begin_write();
        memcpy(rw, code, sizeof code);
        jitmem_end_write();
        stage = 2;
        jitmem_flush_icache(rx, sizeof code);
        result = ((int (*)(void))(void *)rx)();
    } else {
        jitmem_end_write();
    }
    guard_off(&g);
    if (result == 42) { jitlog("deneme kodu çalıştı (RW %p, RX %p)", (void *)rw, (void *)rx); return 1; }
    if (g_guard_sig) jitlog("deneme kodu başarısız: %s sırasında sinyal %d", stage == 1 ? "yazma" : "çalıştırma", g_guard_sig);
    else jitlog("deneme kodu yanlış sonuç döndürdü (%d)", result);
    return 0;
}

static void set_region(uint8_t *rw, uint8_t *rx, size_t size, const char *what) {
    g_rw = rw;
    g_rx = rx;
    g_size = size;
    snprintf(g_method, sizeof g_method, "%s - %zu MB", what, size >> 20);
    jitlog("JIT hazır: %s", g_method);
}

#if defined(__APPLE__) && TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR && (defined(__arm64__) || defined(__aarch64__))
extern void *soul_jit26_prepare_region(void *addr, size_t len); // jit26_arm64.S
extern void soul_jit26_detach(void);

static int ios_major(void) {
    char buf[64] = { 0 };
    size_t n = sizeof buf - 1;
    if (sysctlbyname("kern.osproductversion", buf, &n, NULL, 0) != 0) return 0;
    return atoi(buf);
}

static uint8_t *make_alias(void *src, size_t size, vm_prot_t prot) {
    vm_address_t dst = 0;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = vm_remap(mach_task_self(), &dst, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                (vm_address_t)src, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) { jitlog("vm_remap başarısız (%d)", kr); return NULL; }
    kr = vm_protect(mach_task_self(), dst, size, FALSE, prot);
    if (kr != KERN_SUCCESS) {
        jitlog("vm_protect(%d) başarısız (%d), cur=%d max=%d", prot, kr, cur, max);
        vm_deallocate(mach_task_self(), dst, size);
        return NULL;
    }
    return (uint8_t *)dst;
}

static int valid_region(uintptr_t v) {
    if (v == 0 || v == UINTPTR_MAX || (v & 0x3FFF) != 0) return 0;
    if ((v & 0xFFFFFFFFu) == 0x690000e0u || v == 0xE0000069u) return 0; // leftover brk encodings / script errors
    if (v >> 47) return 0;
    return 1;
}

static int try_legacy(size_t size) {
    jitlog("klasik yöntem deneniyor (RW + vm_remap RX)");
    void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rw == MAP_FAILED) { jitlog("mmap başarısız"); return 0; }
    uint8_t *rx = make_alias(rw, size, VM_PROT_READ | VM_PROT_EXECUTE);
    if (!rx) { munmap(rw, size); return 0; }
    if (!probe(rw, rx)) {
        vm_deallocate(mach_task_self(), (vm_address_t)rx, size);
        munmap(rw, size);
        return 0;
    }
    set_region(rw, rx, size, "Klasik JIT (CS_DEBUGGED, RW/RX ayna)");
    return 1;
}

static int try_txm(size_t size) {
    jitlog("TXM yöntemi deneniyor (brk #0xf00d, x16=1, %zu MB)", size >> 20);
    for (int attempt = 1; attempt <= 3; attempt++) {
        Guard g;
        void *volatile rx = NULL;
        volatile int trapped = 0;
        guard_on(&g);
        if (sigsetjmp(g_guard_jb, 1) == 0) rx = soul_jit26_prepare_region(NULL, size);
        else trapped = 1;
        guard_off(&g);
        if (trapped) {
            jitlog("deneme %d: SIGTRAP - hata ayıklayıcı betiği (universal.js) isteği yanıtlamadı", attempt);
            return 0;
        }
        jitlog("deneme %d: hata ayıklayıcı x0=%p döndürdü", attempt, (void *)rx);
        if (valid_region((uintptr_t)rx)) {
            uint8_t *rw = make_alias(rx, size, VM_PROT_READ | VM_PROT_WRITE);
            if (!rw) return 0;
            if (probe(rw, (uint8_t *)rx)) {
                set_region(rw, (uint8_t *)rx, size, "TXM JIT (StikDebug universal.js)");
                return 1;
            }
            vm_deallocate(mach_task_self(), (vm_address_t)rw, size);
            return 0;
        }
        usleep(50000);
    }
    return 0;
}
#endif

int soul_jit_init(SoulJitMode mode, size_t size, char *msg, size_t msglen) {
    if (g_rx) { snprintf(msg, msglen, "%s", g_method); return 1; }
    if (mode == SOUL_JIT_OFF) { snprintf(g_method, sizeof g_method, "JIT kapalı (yalnızca yorumlayıcı)"); snprintf(msg, msglen, "%s", g_method); return 0; }
    if (!soul_host_is_arm64()) {
        snprintf(g_method, sizeof g_method, "JIT yalnızca ARM64 üzerinde (bu ana makinede yorumlayıcı kullanılıyor)");
        snprintf(msg, msglen, "%s", g_method);
        return 0;
    }
    if (size == 0) size = 64u << 20;
    size = (size + 0x3FFF) & ~(size_t)0x3FFF;
#if defined(__APPLE__) && TARGET_OS_SIMULATOR
    (void)mode;
    g_wp = (void (*)(int))dlsym(RTLD_DEFAULT, "pthread_jit_write_protect_np");
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED || !g_wp) {
        if (p != MAP_FAILED) munmap(p, size);
        snprintf(msg, msglen, "Simülatörde MAP_JIT kullanılamadı");
        return 0;
    }
    if (!probe(p, p)) { munmap(p, size); snprintf(msg, msglen, "Simülatör JIT denemesi başarısız"); return 0; }
    set_region(p, p, size, "Simülatör MAP_JIT");
    snprintf(msg, msglen, "%s", g_method);
    return 1;
#elif defined(__APPLE__) && TARGET_OS_IPHONE && (defined(__arm64__) || defined(__aarch64__))
    uint32_t flags = soul_jit_cs_flags();
    int major = ios_major();
    jitlog("iOS %d, csops bayrakları 0x%08x, CS_DEBUGGED=%d, mod=%d", major, flags, (flags & CS_DEBUGGED) != 0, (int)mode);
    if (!(flags & CS_DEBUGGED)) {
        snprintf(g_method, sizeof g_method, "JIT etkin değil (StikDebug ile etkinleştirin)");
        snprintf(msg, msglen, "%s", g_method);
        return 0;
    }
    int ok = 0;
    if (mode == SOUL_JIT_TXM) ok = try_txm(size);
    else if (mode == SOUL_JIT_LEGACY) ok = try_legacy(size);
    else if (major >= 26) ok = try_txm(size) || try_legacy(size);
    else ok = try_legacy(size);
    if (!ok) {
        snprintf(g_method, sizeof g_method, "JIT başlatılamadı (ayrıntılar JIT günlüğünde)");
        snprintf(msg, msglen, "%s", major >= 26
                 ? "JIT belleği hazırlanamadı. StikDebug'da Soul için universal.js betiğini atayın, 'Always Run Scripts' ve 'Picture in Picture' seçeneklerini açın, sonra tekrar deneyin."
                 : "JIT belleği hazırlanamadı. Soul'u StikDebug'ın 'Enable JIT' seçeneğiyle başlatın.");
        return 0;
    }
    snprintf(msg, msglen, "%s", g_method);
    return 1;
#elif defined(__APPLE__)
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { snprintf(msg, msglen, "MAP_JIT başarısız: %s", strerror(errno)); return 0; }
    set_region(p, p, size, "macOS MAP_JIT");
    snprintf(msg, msglen, "%s", g_method);
    return 1;
#else
    (void)mode;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { snprintf(msg, msglen, "RWX bellek ayrılamadı: %s", strerror(errno)); return 0; }
    if (!probe(p, p)) { munmap(p, size); snprintf(msg, msglen, "RWX deneme kodu çalışmadı"); return 0; }
    set_region(p, p, size, "Linux RWX");
    snprintf(msg, msglen, "%s", g_method);
    return 1;
#endif
}
