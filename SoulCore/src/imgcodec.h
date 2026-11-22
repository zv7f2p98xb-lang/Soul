// SoulCore - image codecs (pure C): BMP, PNG, GIF, JPEG, ICO/CUR decoders to non-premultiplied
// ARGB (0xAARRGGBB) and BMP / PNG encoders. Used by the GDI+ implementation (gdiplus.c).
#ifndef SOUL_IMGCODEC_H
#define SOUL_IMGCODEC_H
#include <stddef.h>
#include <stdint.h>

enum { IMG_UNKNOWN = 0, IMG_BMP, IMG_PNG, IMG_GIF, IMG_JPEG, IMG_ICO, IMG_TIFF };

typedef struct ImgFrame {
    uint32_t *px;     // w * h pixels, non-premultiplied ARGB
    int delay_cs;     // GIF frame delay in 1/100 s
} ImgFrame;

typedef struct Img {
    int fmt;          // IMG_*
    int w, h;
    int nframes;
    ImgFrame *fr;
    int src_bpp;      // bits per pixel of the source (1, 4, 8, 16, 24, 32, 48, 64)
    int has_alpha;    // some pixel is not opaque (or the format carries alpha)
    int indexed;      // source was palettised (<= 8 bpp): pal / pal_n are valid
    int pal_n;
    uint32_t pal[256];// 0xAARRGGBB
    int gray;         // greyscale source (JPEG / PNG)
    float dpix, dpiy; // 0 = unknown
    int loop;         // GIF loop count (0 = forever)
} Img;

int img_detect(const uint8_t *d, size_t n);           // IMG_* by signature
Img *img_decode(const uint8_t *d, size_t n);          // NULL on unknown / corrupt data
void img_free(Img *im);

// Encoders return malloc'ed buffers.
uint8_t *img_encode_png(const uint32_t *px, int w, int h, int has_alpha, float dpix, float dpiy, size_t *outlen);
uint8_t *img_encode_bmp(const uint32_t *px, int w, int h, int has_alpha, float dpix, float dpiy, size_t *outlen);
#endif
