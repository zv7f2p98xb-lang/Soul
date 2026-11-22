// 32-bit structured exception handling through the fs:[0] chain, the way Delphi programs
// (Inno Setup!) and MSVC's __try/__except use it: RaiseException, handlers that continue
// execution or search on, RtlUnwind of inner frames, hardware #DE and the unhandled filter.
#include <windows.h>
#include <stdio.h>
#include <setjmp.h>

typedef struct Reg { struct Reg *next; void *handler; jmp_buf *jb; const char *name; } Reg;
static int resumed_fixups;

// own fs:[0] accessors: the MinGW intrinsics' stores can be optimised away
static inline DWORD rd_fs0(void) { DWORD v; __asm__ __volatile__("movl %%fs:0, %0" : "=r"(v) : : "memory"); return v; }
static inline void wr_fs0(DWORD v) { __asm__ __volatile__("movl %0, %%fs:0" : : "r"(v) : "memory"); }
static void push_reg(Reg *r) { r->next = (Reg *)(uintptr_t)rd_fs0(); wr_fs0((DWORD)(uintptr_t)r); }
static void pop_reg(Reg *r) { wr_fs0((DWORD)(uintptr_t)r->next); }

static EXCEPTION_DISPOSITION __cdecl catcher(EXCEPTION_RECORD *rec, void *frame, CONTEXT *ctx, void *dc) {
    Reg *r = frame;
    (void)ctx; (void)dc;
    if (rec->ExceptionFlags & 2) { printf("  %s: unwinding\n", r->name); return ExceptionContinueSearch; }
    if (rec->ExceptionCode == 0xE0000003) { resumed_fixups++; return ExceptionContinueExecution; }
    if (!r->jb) { printf("  %s: passes %08lx on\n", r->name, rec->ExceptionCode); return ExceptionContinueSearch; }
    printf("  %s: catches %08lx params=%lu p0=%lu\n", r->name, rec->ExceptionCode, rec->NumberParameters,
           rec->NumberParameters ? (unsigned long)rec->ExceptionInformation[0] : 0ul);
    RtlUnwind(frame, 0, rec, 0);
    longjmp(*r->jb, 1);
}

static LONG WINAPI last_chance(EXCEPTION_POINTERS *ep) {
    printf("unhandled filter: %08lx\n", ep->ExceptionRecord->ExceptionCode);
    fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(void) {
    jmp_buf jb;
    Reg outer = { 0, (void *)catcher, &jb, "outer" };
    ULONG_PTR p = 42;

    printf("1 raise\n");
    if (!setjmp(jb)) { push_reg(&outer); RaiseException(0xE0000002, 0, 1, &p); printf("  not reached\n"); }
    pop_reg(&outer);

    printf("2 continue execution\n");
    push_reg(&outer);
    RaiseException(0xE0000003, 0, 0, NULL);
    RaiseException(0xE0000003, 0, 0, NULL);
    pop_reg(&outer);
    printf("  resumed %d times\n", resumed_fixups);

    printf("3 nested frames\n");
    if (!setjmp(jb)) {
        push_reg(&outer);
        Reg inner = { 0, (void *)catcher, NULL, "inner" };
        push_reg(&inner);
        RaiseException(0xE0000004, 0, 0, NULL);
        printf("  not reached\n");
    }
    pop_reg(&outer);

    printf("4 divide by zero\n");
    volatile int zero = 0, x = 7;
    if (!setjmp(jb)) { push_reg(&outer); x = x / zero; printf("  not reached %d\n", x); }
    pop_reg(&outer);

    printf("5 chain restored: %s\n", rd_fs0() == 0xFFFFFFFF ? "yes" : "no");
    SetUnhandledExceptionFilter(last_chance);
    RaiseException(0xE0000009, 0, 0, NULL);
    printf("not reached\n");
    return 0;
}
