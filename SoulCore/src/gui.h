// SoulCore - Win32 GUI emulation (user32 windows/messages + gdi32 drawing).
//
// Every top-level window owns a 32-bit BGRA framebuffer the size of its client
// area (Soul windows have no non-client area: the iPad UI draws the title bar).
// Child windows (controls) draw into their top-level ancestor's buffer at their
// offset. The iPad UI copies these buffers and feeds touch/keyboard events back
// through a thread-safe input queue that the guest's message loop drains.
//
// Locking: guest code runs under the GIL. GuiState.mu additionally guards what
// the UI thread reads (window list, geometry, text, framebuffers, menus) and
// the input queue. Never call guest code while holding mu.
#pragma once
#include "hle.h"

// ---- colours ----------------------------------------------------------------------------------
static inline uint32_t rgb_px(uint32_t colorref) { // COLORREF 0x00BBGGRR -> pixel 0xFFRRGGBB (BGRA in memory)
    return 0xFF000000u | ((colorref & 0xFF) << 16) | (colorref & 0xFF00) | ((colorref >> 16) & 0xFF);
}
static inline uint32_t px_rgb(uint32_t px) { return ((px >> 16) & 0xFF) | (px & 0xFF00) | ((px & 0xFF) << 16); }
uint32_t gui_sys_color(int index); // COLORREF

// ---- surfaces ---------------------------------------------------------------------------------
typedef struct Surface {
    uint8_t *bits;
    int w, h;
    int stride;        // bytes per row (positive)
    int bpp;           // 1, 4, 8, 16, 24 or 32
    int top_down;
    uint32_t palette[256]; // pixels 0xFFRRGGBB for <= 8 bpp
    int bitfields565;  // 16 bpp: 1 = 565, 0 = 555
} Surface;

uint32_t surf_get(const Surface *s, int x, int y);          // pixel 0xFFRRGGBB
void surf_put(Surface *s, int x, int y, uint32_t px);

// ---- GDI objects ------------------------------------------------------------------------------
enum { GO_FREE = 0, GO_BRUSH, GO_PEN, GO_FONT, GO_BITMAP, GO_DC, GO_REGION, GO_PALETTE };
enum { DC_WINDOW = 1, DC_MEMORY, DC_SCREEN };

typedef struct GdiObj {
    int type;
    int stock;
    // brush / pen
    int style;
    int width;
    uint32_t color;   // COLORREF
    // font
    int height, weight, italic, underline;
    char face[64];
    // bitmap
    Surface surf;
    int owns_bits;
    int selected;     // selected into a DC
    // region (rectangle only)
    int rx0, ry0, rx1, ry1;
    // dc
    struct Dc *dc;
} GdiObj;

typedef struct Dc {
    int kind;
    uint64_t hwnd;
    uint64_t bitmap;              // selected bitmap (memory DCs)
    uint64_t pen, brush, font, palette;
    uint32_t text_color, bk_color;
    int bk_mode;                  // 1 TRANSPARENT, 2 OPAQUE
    int text_align;
    int rop2;
    int cur_x, cur_y;
    int vp_x, vp_y;               // viewport origin
    int clip_set, cx0, cy0, cx1, cy1; // extra clip rectangle in logical coordinates
    int saved_count;
    struct Dc *saved[16];
} Dc;

#define GDI_MAX 8192
#define GDI_HANDLE_BASE 0x40000

uint64_t gdi_alloc(int type, GdiObj **out);
GdiObj *gdi_get(uint64_t h, int type);   // type 0 = any
void gdi_free(uint64_t h);
uint64_t gdi_create_dc(int kind, uint64_t hwnd);
Dc *dc_get(uint64_t h);
void gdi_release_dc(uint64_t h);
uint64_t gdi_default_font(void);
void gdi_reset_objects(void);
uint64_t gdi_make_brush(uint32_t colorref);
uint64_t gdi_make_font(int height, int weight, const char *face);
uint64_t gdi_bitmap_from_dib(const uint8_t *bmi, const uint8_t *bits); // 32 bpp copy of a packed DIB

// ---- windows ----------------------------------------------------------------------------------
typedef struct WinClass {
    char name[128];
    uint64_t wndproc;
    uint32_t style;
    uint64_t brush;
    uint64_t hinst, icon, cursor, menu_name;
    int cls_extra, wnd_extra;
    int unicode;
    int used;
    int builtin;                  // CTL_* for system classes
    uint16_t atom;
} WinClass;

typedef struct MenuItem {
    uint32_t id;
    uint32_t flags;               // MF_* flags
    char text[128];
    uint64_t submenu;
    uint64_t data;
} MenuItem;

typedef struct Menu {
    int used;
    MenuItem *items;
    int count, cap;
} Menu;

typedef struct Win {
    int used;
    uint64_t hwnd;
    uint64_t seq;                 // creation order: lower = above its siblings
    uint64_t zstamp;              // top-level activation order: higher = in front
    WinClass *cls;
    uint64_t wndproc;
    int unicode;
    uint32_t style, exstyle;
    uint64_t parent, owner;
    uint64_t id;                  // control id (child windows)
    uint64_t hinst;
    uint64_t menu;                // HMENU (top-level)
    int x, y, w, h;               // relative to the parent client area (desktop for top-level)
    char *text;                   // UTF-8 window text (never NULL while used)
    int visible, enabled, destroying, message_only, maximized, minimized;
    int invalid, erase;
    int ix0, iy0, ix1, iy1;       // update rectangle (client coordinates)
    uint32_t *pixels;             // top-level framebuffer (client area)
    int fb_w, fb_h;
    volatile uint64_t version;
    uint64_t userdata;
    uint8_t *extra;
    int extra_size;
    uint32_t tid;
    uint64_t font;                // WM_SETFONT
    // built-in controls
    int ctl;                      // CTL_* below
    int checked, pressed, hot, tracking;
    int sel_start, sel_end;       // edit selection (code points)
    int scroll_x, scroll_y;       // edit scroll offsets (pixels / lines)
    int limit, modified, readonly;
    uint32_t password_char;
    int cur_sel, top_index;       // list / combo
    char **items;                 // listbox / combobox strings
    uint64_t *item_data;
    int item_count, item_cap;
    int progress_min, progress_max, progress_pos;
    uint64_t image;               // static / button image (HBITMAP)
    // dialogs
    int is_dialog;
    uint64_t dlgproc;
    int dlg_unicode;
    uint64_t dlg_result;
    int dlg_ended;
    uint64_t def_id;
    int dlg_bx, dlg_by;           // dialog base units
    uint64_t dlg_focus;           // control that had the focus
    // scroll bars (SB_HORZ = 0, SB_VERT = 1)
    int sb_min[2], sb_max[2], sb_page[2], sb_pos[2];
    // properties
    struct WinProp { char name[64]; uint64_t value; } props[8];
    int prop_count;
} Win;

enum { CTL_NONE = 0, CTL_BUTTON, CTL_STATIC, CTL_EDIT, CTL_LISTBOX, CTL_COMBOBOX, CTL_PROGRESS, CTL_DIALOG,
       CTL_STATUS, CTL_TRACKBAR, CTL_TOOLTIP, CTL_GENERIC };

#define WIN_MAX 1024
#define WIN_HANDLE_BASE 0x10000
#define MENU_MAX 256
#define MENU_HANDLE_BASE 0x20000
#define DESKTOP_HWND 0x10010000ull

Win *win_get(uint64_t hwnd);
Win *win_top(Win *w);                        // top-level ancestor
void win_client_origin(Win *w, int *ox, int *oy); // offset of w's client area inside its top-level framebuffer
int win_visible_rect(Win *w, int *x0, int *y0, int *x1, int *y1); // drawable area in framebuffer coordinates
void win_invalidate_rect(Win *w, const int32_t *rc, int erase); // rc in client coordinates (NULL = all)
void win_touch(Win *w);                      // framebuffer changed
void win_set_text(Win *w, const char *utf8);
uint64_t win_send(X86State *st, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
uint64_t win_send_ex(X86State *st, uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp, int unicode);
void win_post(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);
uint64_t win_def_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode);
void win_set_focus(X86State *st, uint64_t hwnd);
void win_notify_parent(X86State *st, Win *w, uint32_t code); // WM_COMMAND notification
uint64_t win_ctl_color(X86State *st, Win *w, uint64_t hdc, uint32_t msg); // brush via WM_CTLCOLOR*
Menu *menu_get(uint64_t h);
uint64_t win_create(X86State *st, uint32_t exstyle, uint64_t cls, uint64_t name, uint32_t style, int32_t x, int32_t y,
                    int32_t cw, int32_t ch, uint64_t parent, uint64_t menu, uint64_t hinst, uint64_t param, int wide);
void win_destroy(X86State *st, Win *w);
void win_show(X86State *st, Win *w, int cmd);
void win_move(X86State *st, Win *w, int x, int y, int cw, int ch, int flags);
int gui_get_message(X86State *st, uint8_t *msg);          // 0 on WM_QUIT
void gui_translate_dispatch(X86State *st, uint8_t *msg);
void gui_repost_quit(int code);
void combo_select_from_popup(X86State *st, Win *w, int index);
void gdi_rect_dc(uint64_t hdc, int l, int t, int r, int b);

// built-in controls (controls.c)
int ctl_class_for(const char *name);
uint64_t ctl_proc(X86State *st, Win *w, int ctl, uint32_t msg, uint64_t wp, uint64_t lp, int unicode);
uint64_t dlg_proc(X86State *st, Win *w, uint32_t msg, uint64_t wp, uint64_t lp, int unicode);
extern const HleEntry hle_controls_table[];
int dlg_is_message(X86State *st, Win *dlg, uint8_t *msgp);
uint64_t dlg_create(X86State *st, uint64_t hinst, const uint8_t *tmpl, uint64_t owner, uint64_t proc, uint64_t param, int unicode);
void gui_caret_set(uint64_t hwnd, int x, int y, int w, int h, int visible);

// ---- drawing helpers shared with built-in controls ---------------------------------------------
void gdi_fill_rect_px(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref);
void gdi_frame_rect_px(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref);
void gdi_draw_text_utf8(uint64_t hdc, const char *s, int x0, int y0, int x1, int y1, uint32_t format);
void gdi_text_out_utf8(uint64_t hdc, int x, int y, const char *s, int len);
int gdi_text_width(uint64_t font, const char *s, int len, int *height);
void gdi_font_metrics(uint64_t font, int *ascent, int *descent, int *avg_width);
void gdi_fill_ellipse_px(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t fill, uint32_t border);
void gdi_line_px(uint64_t hdc, int x0, int y0, int x1, int y1, uint32_t colorref, int width);
int gdi_draw_text_dc(uint64_t hdc, const char *s, int len, int32_t *rect, uint32_t format);

// ---- message queue -----------------------------------------------------------------------------
typedef struct QMsg {
    uint64_t hwnd;
    uint32_t msg;
    uint64_t wp, lp;
    uint32_t time;
    int x, y;
    uint32_t tid;
} QMsg;

typedef struct GuiTimer {
    int used;
    uint64_t hwnd, id, proc;
    uint32_t interval;
    double due;
    uint32_t tid;
} GuiTimer;

typedef struct GuiInput {
    uint64_t hwnd;
    int type;       // SOUL_GUI_* event
    int x, y, data;
    uint32_t ch;
    char *text;
} GuiInput;

typedef struct GuiAccel {
    int used;
    int count;
    uint8_t fvirt[256];
    uint16_t key[256], cmd[256];
} GuiAccel;

typedef struct GuiState {
    pthread_mutex_t mu;
    int mu_init;
    WinClass classes[256];
    Win wins[WIN_MAX];
    Menu menus[MENU_MAX];
    GuiInput *input;              // from the UI thread (mu)
    int input_len, input_cap;
    QMsg *queue;                  // posted messages (GIL)
    int qlen, qcap;
    GuiTimer timers[256];
    int quit_posted, quit_code;
    uint32_t quit_tid;
    uint64_t focus, capture, active;
    int mouse_x, mouse_y;         // last mouse position (desktop)
    uint8_t keys[256];            // key state for GetKeyState (0x80 down, 0x01 toggled)
    uint32_t vk_char[256];        // character produced by the last key-down of each key
    int screen_w, screen_h;
    uint64_t seq, zstamp;
    volatile uint64_t generation; // bumped when windows/menus/titles change (UI polling)
    int cur_unicode;              // encoding of the message being handled by a built-in proc
    // popup menu / combo drop-down shown by the UI
    int popup_active, popup_kind, popup_done;
    uint64_t popup_hwnd, popup_menu;
    int popup_x, popup_y;
    uint32_t popup_result;
    // caret
    uint64_t caret_hwnd;
    int caret_x, caret_y, caret_w, caret_h, caret_visible;
    // double clicks
    double last_click_time;
    int last_click_x, last_click_y, last_click_button;
    uint64_t last_click_hwnd;
    uint64_t builtin_procs[16];
    uint64_t sys_brush[32];
    char *clip_text;
    int clip_open;
    GuiAccel accel[32];
    uint32_t msg_time;
    int msg_x, msg_y;
    int cascade;
    uint64_t hook_id;
    int pending_dialog, pending_dlg_unicode;
    uint64_t pending_dlgproc;
} GuiState;

GuiState *gui(void);
void gui_reset(void);
void gui_shutdown(void);
void gui_lock(void);
void gui_unlock(void);
uint64_t builtin_proc_addr(int ctl);

extern const HleEntry hle_user32_table[];
extern const HleEntry hle_gdi32_table[];

// ---- Win32 constants ----------------------------------------------------------------------------
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_GETMINMAXINFO 0x0024
#define WM_NEXTDLGCTL 0x0028
#define WM_DRAWITEM 0x002B
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_NOTIFY 0x004E
#define WM_GETICON 0x007F
#define WM_SETICON 0x0080
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_GETDLGCODE 0x0087
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_INITMENU 0x0116
#define WM_INITMENUPOPUP 0x0117
#define WM_MENUSELECT 0x011F
#define WM_ENTERIDLE 0x0121
#define WM_CTLCOLORMSGBOX 0x0132
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORDLG 0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC 0x0138
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WM_MOUSEHWHEEL 0x020E
#define WM_CAPTURECHANGED 0x0215
#define WM_MOUSELEAVE 0x02A3
#define WM_USER 0x0400
#define WM_APP 0x8000

#define WS_OVERLAPPED 0x00000000u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00C00000u
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_VSCROLL 0x00200000u
#define WS_HSCROLL 0x00100000u
#define WS_SYSMENU 0x00080000u
#define WS_THICKFRAME 0x00040000u
#define WS_GROUP 0x00020000u
#define WS_TABSTOP 0x00010000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_EX_CLIENTEDGE 0x00000200u
#define WS_EX_TOOLWINDOW 0x00000080u
#define WS_EX_TOPMOST 0x00000008u

#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_DELETE 0x2E
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02

#define COLOR_WINDOW 5
#define COLOR_WINDOWTEXT 8
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_BTNFACE 15
#define COLOR_BTNSHADOW 16
#define COLOR_GRAYTEXT 17
#define COLOR_BTNTEXT 18

#define IDOK 1
#define IDCANCEL 2
