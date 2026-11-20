// placeholder - replaced by the ARM64 translator
#include "soul_internal.h"
JitCache *jit_create(SoulProcess *p) { (void)p; return NULL; }
void jit_destroy(JitCache *j) { (void)j; }
void jit_flush(JitCache *j) { (void)j; }
int jit_run(JitCache *j, X86State *st) { (void)j; (void)st; return 0; }
void jit_stats(JitCache *j, SoulStats *s) { (void)j; (void)s; }
