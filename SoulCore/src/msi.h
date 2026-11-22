// SoulCore - host-side reader for Windows Installer databases (.msi) and Microsoft Cabinet files (.cab).
// Pure C, only libc + inflate.h; no emulator dependencies. Implemented in msi_db.c and cab.c.
#ifndef SOUL_MSI_H
#define SOUL_MSI_H
#include <stddef.h>
#include <stdint.h>

#define MSI_NULL_INT ((int32_t)0x80000000)

typedef struct MsiDb MsiDb;

MsiDb *msidb_open(const char *host_path, char *err, size_t errlen);      // NULL + message on failure
void msidb_close(MsiDb *db);
int msidb_codepage(MsiDb *db);
int msidb_table_count(MsiDb *db);
const char *msidb_table_name(MsiDb *db, int i);
int msidb_table_exists(MsiDb *db, const char *table);
int msidb_rows(MsiDb *db, const char *table);                             // 0 if the table is missing
int msidb_column_count(MsiDb *db, const char *table);
const char *msidb_column_name(MsiDb *db, const char *table, int col);
int msidb_column_type(MsiDb *db, const char *table, int col);             // 's' string, 'i' integer, 'v' stream/binary
int msidb_column_is_key(MsiDb *db, const char *table, int col);
int msidb_column_index(MsiDb *db, const char *table, const char *column); // -1 if missing
// Raw column type word from _Columns (0x0100 valid, 0x0200 localizable, 0x0800 string, 0x1000 nullable,
// 0x2000 key, 0x4000 temporary, low byte = size); -1 when missing.
int msidb_column_raw_type(MsiDb *db, const char *table, int col);
const char *msidb_str(MsiDb *db, const char *table, int row, const char *column); // UTF-8; NULL when null/missing (integers: NULL)
int32_t msidb_int(MsiDb *db, const char *table, int row, const char *column);     // MSI_NULL_INT when null/missing
int msidb_find(MsiDb *db, const char *table, const char *column, const char *value); // first matching row, -1 if none
int msidb_stream_count(MsiDb *db);
const char *msidb_stream_name(MsiDb *db, int i);                         // decoded names, e.g. "Binary.MyDll", "cab1.cab"
uint8_t *msidb_stream(MsiDb *db, const char *name, size_t *len);         // malloc'ed contents, NULL if missing
// Contents of the stream a 'v' column cell refers to ("<Table>.<key1>[.<key2>...]"); malloc'ed, NULL if missing.
uint8_t *msidb_cell_stream(MsiDb *db, const char *table, int row, const char *column, size_t *len);
const char *msidb_summary_str(MsiDb *db, int pid);   // 2 title, 3 subject, 4 author, 5 keywords, 6 comments, 7 template, 9 revision, 18 app name
int32_t msidb_summary_int(MsiDb *db, int pid);       // 1 codepage, 14 schema, 15 source flags, 19 security; MSI_NULL_INT if missing

// Small SQL subset: SELECT `col`, col2 | * FROM `Table` [WHERE cond] [ORDER BY col[, col] [ASC|DESC]]
// cond: col (=|<>|<|>|<=|>=) 'text' | 123 | ?, col IS [NOT] NULL, combined with AND / OR / NOT / parentheses.
typedef struct MsiView MsiView;
MsiView *msiview_open(MsiDb *db, const char *sql, char *err, size_t errlen);
int msiview_execute(MsiView *v, const char *const *params, int nparams);  // params as text; 0 = ok
int msiview_fetch(MsiView *v);                // next matching row index of the table, -1 at the end
const char *msiview_table(MsiView *v);
int msiview_column_count(MsiView *v);
const char *msiview_column_name(MsiView *v, int i);  // selected columns in order (all columns for *)
void msiview_close(MsiView *v);

// Cabinets (MS-CAB): uncompressed and MSZIP folders. LZX / Quantum folders fail with a "not supported yet" error.
typedef int (*CabFileFn)(void *ctx, const char *name, const uint8_t *data, size_t len, uint32_t attribs); // return non-zero to stop
int cab_extract(const uint8_t *cab, size_t len, CabFileFn fn, void *ctx, char *err, size_t errlen); // 0 ok, -1 error

#endif
