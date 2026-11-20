// SoulCore - "JIT syscalls" understood by StikDebug's universal.js script on iOS 26 (TXM).
// The debugger traps the brk, reads x16 (command), x0/x1 (arguments), skips the brk and
// writes the result back into x0.
#if defined(__APPLE__) && (defined(__arm64__) || defined(__aarch64__))
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
    .text
    .p2align 2
    .globl _soul_jit26_prepare_region
_soul_jit26_prepare_region:   // void *soul_jit26_prepare_region(void *addr, size_t len)
    mov x16, #1
    brk #0xf00d
    ret

    .p2align 2
    .globl _soul_jit26_detach
_soul_jit26_detach:           // void soul_jit26_detach(void)
    mov x16, #0
    brk #0xf00d
    ret
#endif
#endif

#if defined(__linux__) && defined(__ELF__)
    .section .note.GNU-stack,"",%progbits
#endif
