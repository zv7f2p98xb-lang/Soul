// SoulCore - public C API used by the iPad app (Swift) and the soulrun CLI.
//
// SoulCore runs 64-bit Windows (PE32+, x86-64) console and GUI programs on ARM64 by
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
    int gui_enabled;          // 1 = a UI shows windows and answers popup menus (soul_gui_*)
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

// ---- GUI (Win32 windows drawn by SoulCore, shown by the app) ---------------
// Every visible top-level window has a BGRA framebuffer (premultiplied, alpha
// 255) the size of its client area. Windows have no non-client area: the app
// draws the title bar, close button and menu bar itself. Coordinates passed to
// soul_gui_event are client coordinates of the given top-level window.
typedef enum SoulGuiEventType {
    SOUL_GUI_MOUSE_DOWN = 1,   // data: 0 left, 1 right, 2 middle
    SOUL_GUI_MOUSE_UP = 2,
    SOUL_GUI_MOUSE_MOVE = 3,
    SOUL_GUI_MOUSE_WHEEL = 4,  // data: wheel delta (120 per notch), | 0x10000 for horizontal
    SOUL_GUI_KEY = 5,          // data: Windows virtual-key code, x: 1 down / 0 up, y: SOUL_MOD_*, ch: character (UTF-16)
    SOUL_GUI_TEXT = 6,         // text: UTF-8 typed text
    SOUL_GUI_CLOSE = 7,        // title bar close button
    SOUL_GUI_COMMAND = 8,      // data: menu item id
    SOUL_GUI_RESIZE = 9,       // x, y: new client size
    SOUL_GUI_POPUP_RESULT = 10,// data: chosen popup item id (0 = cancelled)
    SOUL_GUI_MENU_OPEN = 11,   // the app is about to show the menu bar menus
    SOUL_GUI_ACTIVATE = 12,    // the user tapped the window
} SoulGuiEventType;
typedef enum SoulModifier { SOUL_MOD_SHIFT = 1, SOUL_MOD_CTRL = 2, SOUL_MOD_ALT = 4 } SoulModifier;
typedef enum SoulWindowFlag {
    SOUL_WIN_CAPTION = 1, SOUL_WIN_RESIZABLE = 2, SOUL_WIN_DIALOG = 4, SOUL_WIN_ACTIVE = 8, SOUL_WIN_DISABLED = 16,
    SOUL_WIN_WANTS_TEXT = 32, SOUL_WIN_HAS_MENU = 64, SOUL_WIN_MAXIMIZED = 128, SOUL_WIN_CLOSABLE = 256,
} SoulWindowFlag;
typedef enum SoulMenuFlag { SOUL_MENU_POPUP = 1, SOUL_MENU_SEPARATOR = 2, SOUL_MENU_CHECKED = 4, SOUL_MENU_DISABLED = 8, SOUL_MENU_RADIO = 16 } SoulMenuFlag;

typedef struct SoulWindowInfo {
    uint64_t hwnd;
    uint64_t owner;
    int x, y, width, height;   // client area on the virtual desktop
    unsigned flags;            // SOUL_WIN_*
    uint64_t version;          // changes whenever the pixels change
    int caret_visible, caret_x, caret_y, caret_w, caret_h;
    char title[256];
} SoulWindowInfo;

typedef struct SoulMenuEntry {
    int depth;                 // 0 = menu bar item, 1 = item of its drop-down, ...
    uint32_t id;
    unsigned flags;            // SOUL_MENU_*
    char text[128];            // '&' marks the access key
} SoulMenuEntry;

typedef struct SoulPopupInfo {
    int active;
    int kind;                  // 1 = context menu (TrackPopupMenu), 2 = combo box list
    uint64_t hwnd;             // top-level window
    int x, y;                  // anchor in client coordinates of hwnd
} SoulPopupInfo;

// All functions are safe to call from the UI thread while the program runs.
int soul_gui_windows(SoulProcess *p, SoulWindowInfo *out, int max);   // visible top-level windows, back to front
int soul_gui_copy(SoulProcess *p, uint64_t hwnd, void *dst, size_t dst_stride, int width, int height, uint64_t *version); // 1 if copied
void soul_gui_event(SoulProcess *p, uint64_t hwnd, int type, int x, int y, int data, uint32_t ch, const char *text);
int soul_gui_menu(SoulProcess *p, uint64_t hwnd, SoulMenuEntry *out, int max);
int soul_gui_popup(SoulProcess *p, SoulPopupInfo *info, SoulMenuEntry *out, int max);
uint64_t soul_gui_generation(SoulProcess *p);   // changes when windows, titles or menus change
void soul_gui_set_screen(SoulProcess *p, int width, int height); // virtual desktop size (default 1280x800)
int soul_gui_has_windows(SoulProcess *p);
int soul_gui_idle(SoulProcess *p);               // 1 when no input, posted message or repaint is pending (tests)
int soul_gui_popup_active(SoulProcess *p);

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
const char *soul_jit_log(void);      // step by step log of the JIT setup (for diagnostics)
uint32_t soul_jit_cs_flags(void);    // raw csops(CS_OPS_STATUS) flags (iOS)
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
