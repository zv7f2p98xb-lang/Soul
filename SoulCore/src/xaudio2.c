// SoulCore - XAudio2 / X3DAudio. Skeleton: functions are added to the table below.
#include "hle.h"

const HleEntry hle_xaudio2_table[] = { { NULL, NULL, NULL } };

// Called by CoCreateInstance(Ex) before the generic class handling; REGDB_E_CLASSNOTREG = not an XAudio2 class.
uint32_t xaudio2_com_create(X86State *st, uint64_t clsid, uint64_t riid, uint64_t ppv) {
    (void)st; (void)clsid; (void)riid; (void)ppv;
    return 0x80040154u;
}
