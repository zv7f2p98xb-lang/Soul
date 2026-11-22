// SoulCore - several Windows processes inside one host process.
//
// Every guest process has its own address space (for 32-bit programs a separate 4 GB window),
// modules, handle table, environment and C runtime state, while windows, GDI objects and the
// registry are shared like on Windows. Guest code of all processes runs under one global lock,
// so only one process is "active" at a time: its private variables live in their usual global
// locations, and the copies of the others wait in their SoulProcess. Handing the lock to a
// thread of another process swaps these variables (proc_activate).
#include "soul_internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct ProcVar { void *addr; size_t size, off; } ProcVar;
static ProcVar g_vars[64];
static int g_nvars;
static size_t g_total;
static uint8_t *g_template; // the variables' values before any process started

void proc_var_register(void *addr, size_t size) {
    if (g_nvars >= (int)(sizeof g_vars / sizeof g_vars[0])) abort();
    g_vars[g_nvars].addr = addr;
    g_vars[g_nvars].size = size;
    g_vars[g_nvars].off = g_total;
    g_nvars++;
    g_total += (size + 15) & ~(size_t)15;
}

static void snapshot_template(void) {
    if (g_template) return;
    g_template = malloc(g_total ? g_total : 1);
    for (int i = 0; i < g_nvars; i++) memcpy(g_template + g_vars[i].off, g_vars[i].addr, g_vars[i].size);
}

// A fresh set of private variables for a new process (the program-start values).
uint8_t *proc_vars_new(void) {
    snapshot_template();
    uint8_t *v = malloc(g_total ? g_total : 1);
    memcpy(v, g_template, g_total);
    return v;
}

// Makes p the active process. The caller holds the global lock.
void proc_activate(SoulProcess *p) {
    SoulProcess *cur = g_proc;
    if (cur == p || !p) return;
    snapshot_template();
    if (cur && cur->pvars)
        for (int i = 0; i < g_nvars; i++) memcpy(cur->pvars + g_vars[i].off, g_vars[i].addr, g_vars[i].size);
    if (p->pvars)
        for (int i = 0; i < g_nvars; i++) memcpy(g_vars[i].addr, p->pvars + g_vars[i].off, g_vars[i].size);
    g_proc = p; // each process has its own translation cache (p->jit)
}

// Restores the program-start values of the private variables (the session is over).
void proc_vars_reset(void) {
    snapshot_template();
    for (int i = 0; i < g_nvars; i++) memcpy(g_vars[i].addr, g_template + g_vars[i].off, g_vars[i].size);
}
