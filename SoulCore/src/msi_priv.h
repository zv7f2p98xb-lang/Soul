// SoulCore - private interface between the Windows Installer engine (msi_engine.c) and the msi.dll API (msi_install.c).
#pragma once
#include "hle.h"
#include "msi.h"

typedef struct MsiSess MsiSess;

// Installer messages (INSTALLMESSAGE)
#define IM_FATALEXIT 0x00000000
#define IM_ERROR 0x01000000
#define IM_WARNING 0x02000000
#define IM_USER 0x03000000
#define IM_INFO 0x04000000
#define IM_FILESINUSE 0x05000000
#define IM_RESOLVESOURCE 0x06000000
#define IM_OUTOFDISKSPACE 0x07000000
#define IM_ACTIONSTART 0x08000000
#define IM_ACTIONDATA 0x09000000
#define IM_PROGRESS 0x0A000000
#define IM_COMMONDATA 0x0B000000
#define IM_INITIALIZE 0x0C000000
#define IM_TERMINATE 0x0D000000
#define IM_SHOWDIALOG 0x0E000000

// Fields of a record handed to the formatter: n fields (1-based in templates, field 0 = template), UTF-8, NULL = null.
typedef struct { int n; const char *const *f; } MsiFmtRec;

// ---- engine (msi_engine.c) -------------------------------------------------------------------
MsiSess *msis_open(const char *pkg_host, const char *pkg_win, char *err, size_t errlen); // loads the package, sets the standard properties
void msis_free(MsiSess *s);
MsiDb *msis_db(MsiSess *s);
void msis_set_handle(MsiSess *s, uint32_t h);
uint32_t msis_handle(MsiSess *s);
void msis_set_state(MsiSess *s, X86State *st);
char *msis_prop(MsiSess *s, const char *name);               // malloc'ed, NULL when unset
void msis_setprop(MsiSess *s, const char *name, const char *value); // NULL / "" removes
char *msis_format(MsiSess *s, const char *tmpl, const MsiFmtRec *rec, size_t *outlen); // malloc'ed
int msis_eval(MsiSess *s, const char *cond);                  // 0 false, 1 true, -1 syntax error
char *msis_dir_path(MsiSess *s, const char *dir);             // target path with trailing backslash, malloc'ed, NULL if unknown
char *msis_src_path(MsiSess *s, const char *dir);
int msis_set_target(MsiSess *s, const char *dir, const char *path);
int msis_feature_state(MsiSess *s, const char *f, int *inst, int *act);   // 0 ok, else error code
int msis_set_feature(MsiSess *s, const char *f, int state);
int msis_component_state(MsiSess *s, const char *c, int *inst, int *act);
int msis_set_component(MsiSess *s, const char *c, int state);
void msis_set_level(MsiSess *s, int level);
int msis_do_action(MsiSess *s, const char *action);           // Win32 error / install result code
int msis_run(MsiSess *s, int action, const char *cmdline);    // action 0 install/configure, 1 remove, 2 reinstall; returns the MSI result code
int msis_in_deferred(MsiSess *s);
const char *msis_ca_data(MsiSess *s);
int msis_ui_message(MsiSess *s, uint32_t type, const char *text); // forwards to the external handler; returns its answer (0 = none)
int msis_x64(MsiSess *s);

// product registry
int msi_product_registered(const char *code);
int msi_product_info(const char *code, const char *attr, char *out, size_t outlen); // 0 ok, 1 unknown attribute, 2 not installed
int msi_enum_products(int index, char *code);              // 0 ok, 259 no more
int msi_component_path(const char *product, const char *comp_id, char *out, size_t outlen); // INSTALLSTATE
int msi_feature_installed(const char *product, const char *feature);   // INSTALLSTATE
int msi_pe_version(const char *host_path, char *ver, size_t vn, char *lang, size_t ln);
int msi_remove_product(const char *code, X86State *st);     // used by upgrades; MSI result code
void msi_package_local(const char *code, char *win, size_t n);

// external UI (per process)
void msi_ui_set(uint64_t fn, uint32_t filter, uint64_t ctx, int wide, uint64_t *prev_fn);
int msi_ui_level_set(int level);

// ---- API side (msi_install.c) ------------------------------------------------------------------
uint32_t msih_new_session(MsiSess *s);   // handle for a session created by the engine (custom actions need it)
void msih_release(uint32_t h);
