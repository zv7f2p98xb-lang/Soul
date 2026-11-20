// SoulCore - executable memory for the JIT.
//
// iOS does not allow normal apps to create executable memory. Sideloaded apps
// can get it while a debugger has marked the process CS_DEBUGGED (StikDebug,
// SideJITServer, Xcode). Two methods are supported:
//
//  * Legacy (iOS 14-18, non-TXM devices): allocate RW memory, create a second
//    mapping of the same pages with vm_remap and mprotect it to RX. Code is
//    written through the RW alias and executed through the RX alias.
//
//  * TXM (iOS/iPadOS 26 on A15+/M2+ devices, e.g. iPad Air M3): page
//    permissions are enforced by the Trusted Execution Monitor, so the
//    debugger must prepare the region. The app executes `brk #0xf00d` with
//    x16 = 1 (JIT26PrepareRegion); StikDebug's universal.js script allocates an
//    RX region, prepares it and returns its address in x0. A RW alias is then
//    created with vm_remap. Finally x16 = 0 (JIT26Detach) lets the debugger go.
#include "soul_internal.h"
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

static uint8_t *g_rw, *g_rx;
static size_t g_size;
static char g_method[256] = "JIT kapalı";

void *jitmem_rw(void) { return g_rw; }
void *jitmem_rx(void) { return g_rx; }
size_t jitmem_size(void) { return g_size; }
int jitmem_ready(void) { return g_rx != NULL && soul_host_is_arm64(); }
int soul_jit_available(void) { return jitmem_ready(); }
const char *soul_jit_method(void) { return g_method; }

void jitmem_flush_icache(void *rx, size_t len) {
#if defined(__APPLE__)
    sys_icache_invalidate(rx, len);
#else
    __builtin___clear_cache((char *)rx, (char *)rx + len);
#endif
}

#if defined(__APPLE__)
#define CS_OPS_STATUS 0
#define CS_DEBUGGED 0x10000000
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#endif

int soul_jit_is_debugged(void) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    uint32_t flags = 0;
    if (csops(getpid(), CS_OPS_STATUS, &flags, sizeof flags) != 0) return 0;
    return (flags & CS_DEBUGGED) != 0;
#else
    return 1; // desktop hosts can always map executable memory
#endif
}

#if defined(__APPLE__) && TARGET_OS_IPHONE && (defined(__arm64__) || defined(__aarch64__))
extern void *soul_jit26_prepare_region(void *addr, size_t len); // jit26_arm64.S
extern void soul_jit26_detach(void);

static int ios_major(void) {
    char buf[64] = { 0 };
    size_t n = sizeof buf - 1;
    if (sysctlbyname("kern.osproductversion", buf, &n, NULL, 0) != 0) return 0;
    return atoi(buf);
}

static void *make_alias(void *src, size_t size, vm_prot_t prot) {
    vm_address_t dst = 0;
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &dst, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                (vm_address_t)src, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) return NULL;
    if (mprotect((void *)dst, size, prot) != 0) {
        vm_deallocate(mach_task_self(), dst, size);
        return NULL;
    }
    return (void *)dst;
}

static int init_legacy(size_t size, char *msg, size_t msglen) {
    void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rw == MAP_FAILED) { snprintf(msg, msglen, "Bellek ayrılamadı"); return 0; }
    void *rx = make_alias(rw, size, PROT_READ | PROT_EXEC);
    if (!rx) {
        munmap(rw, size);
        snprintf(msg, msglen, "RX ayna eşlemesi oluşturulamadı (JIT izni yok)");
        return 0;
    }
    g_rw = rw; g_rx = rx; g_size = size;
    snprintf(g_method, sizeof g_method, "Klasik JIT (CS_DEBUGGED, RW/RX ayna) - %zu MB", size >> 20);
    snprintf(msg, msglen, "%s", g_method);
    return 1;
}

// If no debugger script handles the brk, the kernel delivers SIGTRAP to us:
// catch it so a missing StikDebug script does not crash the whole app.
static sigjmp_buf g_trap_jb;
static void trap_handler(int sig) { (void)sig; siglongjmp(g_trap_jb, 1); }

static void *guarded_prepare(size_t size, int *trapped) {
    struct sigaction sa, old;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = trap_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTRAP, &sa, &old);
    void *volatile rx = NULL;
    *trapped = 0;
    if (sigsetjmp(g_trap_jb, 1) == 0) rx = soul_jit26_prepare_region(NULL, size);
    else *trapped = 1;
    sigaction(SIGTRAP, &old, NULL);
    return rx;
}

static int init_txm(size_t size, char *msg, size_t msglen) {
    int trapped = 0;
    void *rx = guarded_prepare(size, &trapped);
    if (trapped) {
        snprintf(msg, msglen, "Hata ayıklayıcı JIT isteğini yanıtlamadı (StikDebug betiği çalışmıyor). StikDebug'da Soul için universal.js atayın.");
        return 0;
    }
    if (!rx || (uintptr_t)rx == 0xE0000069) {
        snprintf(msg, msglen, "StikDebug JIT bölgesi hazırlamadı. StikDebug'da 'Always Run Scripts' açık olmalı ve Soul için universal.js seçilmeli.");
        return 0;
    }
    void *rw = make_alias(rx, size, PROT_READ | PROT_WRITE);
    if (!rw) {
        snprintf(msg, msglen, "TXM bölgesi için RW ayna oluşturulamadı");
        return 0;
    }
    g_rw = rw; g_rx = rx; g_size = size;
    soul_jit26_detach();
    snprintf(g_method, sizeof g_method, "iOS 26 TXM JIT (StikDebug universal.js) - %zu MB", size >> 20);
    snprintf(msg, msglen, "%s", g_method);
    return 1;
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
#if defined(__APPLE__) && TARGET_OS_IPHONE && (defined(__arm64__) || defined(__aarch64__))
    if (!soul_jit_is_debugged()) {
        snprintf(g_method, sizeof g_method, "JIT etkin değil (StikDebug ile başlatın)");
        snprintf(msg, msglen, "%s", g_method);
        return 0;
    }
    int txm = (mode == SOUL_JIT_TXM) || (mode == SOUL_JIT_AUTO && ios_major() >= 26);
    int ok = txm ? init_txm(size, msg, msglen) : init_legacy(size, msg, msglen);
    if (!ok) snprintf(g_method, sizeof g_method, "JIT başlatılamadı: %s", msg);
    return ok;
#elif defined(__APPLE__)
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { snprintf(msg, msglen, "MAP_JIT başarısız: %s", strerror(errno)); return 0; }
    g_rw = g_rx = p; g_size = size;
    snprintf(g_method, sizeof g_method, "macOS MAP_JIT - %zu MB", size >> 20);
    snprintf(msg, msglen, "%s", g_method);
    return 1;
#else
    (void)mode;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { snprintf(msg, msglen, "RWX bellek ayrılamadı: %s", strerror(errno)); return 0; }
    g_rw = g_rx = p; g_size = size;
    snprintf(g_method, sizeof g_method, "Linux RWX - %zu MB", size >> 20);
    snprintf(msg, msglen, "%s", g_method);
    return 1;
#endif
}

void jitmem_begin_write(void) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(0);
#endif
}
void jitmem_end_write(void) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    pthread_jit_write_protect_np(1);
#endif
}
