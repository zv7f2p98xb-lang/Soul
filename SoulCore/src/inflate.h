// SoulCore - standalone DEFLATE decoder (RFC 1951) and zlib wrapper (RFC 1950). Only needs libc.
#ifndef SOUL_INFLATE_H
#define SOUL_INFLATE_H
#include <stddef.h>
#include <stdint.h>

// Raw DEFLATE. dict/dict_len: optional preset window (the last 32 KB are used) the stream may
// refer back into - MSZIP cabinet blocks use the previous block's output this way.
// Returns the number of bytes written to out, or -1 on corrupt data or when out_cap is too small.
long soul_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, const uint8_t *dict, size_t dict_len);
// zlib stream (e.g. PNG IDAT): checks/skips the 2-byte header (FDICT -> -1), ignores the Adler-32 trailer.
long soul_zlib_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap);

#endif
