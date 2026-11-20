// SoulCore - public C API used by the iPad app (Swift) and the soulrun CLI.
//
// SoulCore runs 64-bit Windows (PE32+, x86-64) console programs on ARM64 by
// translating x86-64 machine code to ARM64 at runtime (JIT) and providing a
// small Win32 / C runtime compatibility layer.
#ifndef SOULCORE_H
#define SOULCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SoulProcess SoulProcess;

enum {
    SOUL_STREAM_STDOUT = 1,
    SOUL_STREAM_STDERR = 2,
    SOUL_STREAM_LOG = 3,
};

// MessageBox return values (Win32 IDOK, IDCANCEL, ...)
typedef void (*SoulOutputFn)(void *ctx, int stream, const char *data, size_t len);
typedef int (*SoulMessageBoxFn)(void *ctx, const char *title, const char *text, unsigned type);

typedef struct SoulLaunchOptions {
    const char *exe_path;     // host path of the .exe
    const char *drive_c;      // host directory mapped to C:
    const char *exe_win_path; // optional Windows path, e.g. "C:\\Program Files\\App\\app.exe"
    const char *args;         // optional command line arguments
    int jit_enabled;          // 1 = translate x86-64 to ARM64 when JIT memory is available
    int verbose;              // 1 = log unimplemented APIs and loader details
    void *ctx;
    SoulOutputFn output;
    SoulMessageBoxFn message_box;
} SoulLaunchOptions;

typedef struct SoulStats {
    uint64_t jit_blocks;
    uint64_t jit_insns;     // guest instructions executed by translated code
    uint64_t interp_insns;  // guest instructions executed by the interpreter
    uint64_t jit_code_bytes;
    uint64_t api_calls;
    int jit_active;
    double seconds;
} SoulStats;

int soul_process_create(const SoulLaunchOptions *opts, SoulProcess **out, char *err, size_t errlen);
int soul_process_run(SoulProcess *p); // blocks; returns the guest exit code
void soul_process_terminate(SoulProcess *p);
void soul_process_send_input(SoulProcess *p, const char *data, size_t len);
void soul_process_close_input(SoulProcess *p);
void soul_process_stats(SoulProcess *p, SoulStats *out);
const char *soul_process_error(SoulProcess *p); // empty string if none
void soul_process_destroy(SoulProcess *p);

// ---- JIT memory -------------------------------------------------------
typedef enum {
    SOUL_JIT_AUTO = 0,   // iOS 26+: TXM protocol, older: classic debugger mapping
    SOUL_JIT_LEGACY = 1, // CS_DEBUGGED + RW/RX mirror (iOS 17/18, macOS, Linux)
    SOUL_JIT_TXM = 2,    // iOS 26 TXM: ask StikDebug (universal.js) via brk #0xf00d
    SOUL_JIT_OFF = 3,
} SoulJitMode;

int soul_jit_is_debugged(void);      // 1 if a debugger has set CS_DEBUGGED
int soul_jit_init(SoulJitMode mode, size_t size, char *msg, size_t msglen);
int soul_jit_available(void);        // 1 if executable memory is ready
const char *soul_jit_method(void);   // human readable description
int soul_host_is_arm64(void);

// ---- PE inspection -------------------------------------------------------
typedef struct SoulPEInfo {
    int valid;
    int is_64bit;
    int is_dll;
    int is_gui;          // IMAGE_SUBSYSTEM_WINDOWS_GUI
    uint16_t machine;
    uint64_t image_base;
    uint32_t size_of_image;
    char imports[2048];  // comma separated DLL list
} SoulPEInfo;

int soul_pe_inspect(const char *path, SoulPEInfo *info);
// Extracts the main icon as .ico bytes (or PNG data). Free with soul_free.
int soul_pe_extract_icon(const char *path, unsigned char **out, size_t *outlen);
void soul_free(void *ptr);
const char *soul_version(void);

#ifdef __cplusplus
}
#endif
#endif
