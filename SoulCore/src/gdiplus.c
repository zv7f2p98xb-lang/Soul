// SoulCore - GDI+ (gdiplus.dll). Skeleton holding the old stand-ins until the real implementation lands.
#include "hle.h"

HLE(gp_GdiplusStartup) { if (ARG(0)) wr_ptr(ARG(0), 0); RET(6); } // NotImplemented
HLE(gp_GdiplusShutdown) { RET(0); }

const HleEntry hle_gdiplus_table[] = {
    { "GdiplusStartup", gp_GdiplusStartup, NULL }, { "GdiplusShutdown", gp_GdiplusShutdown, NULL },
    { NULL, NULL, NULL }
};
