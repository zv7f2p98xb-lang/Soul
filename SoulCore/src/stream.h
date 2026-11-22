// SoulCore - host implemented IStream objects (memory / file) and helpers for guest IStreams.
#pragma once
#include "hle.h"

// Reads everything from the current position of a guest IStream (any implementation) into a malloc'ed buffer.
uint8_t *stream_read_all(X86State *st, uint64_t pstm, size_t *len);
// Writes bytes to a guest IStream; 0 on success.
int stream_write_all(X86State *st, uint64_t pstm, const void *data, size_t len);
// A new host memory IStream holding a copy of data (refcount 1); returns the guest address of the object.
uint64_t stream_create_mem(const void *data, size_t len);
// HLE exports (listed in the gdiplus table, lookups are by name)
void ole_CreateStreamOnHGlobal(X86State *st);
void ole_GetHGlobalFromStream(X86State *st);
void shl_SHCreateMemStream(X86State *st);
void shl_SHCreateStreamOnFileA(X86State *st);
void shl_SHCreateStreamOnFileW(X86State *st);
void shl_SHCreateStreamOnFileEx(X86State *st);

