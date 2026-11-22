#!/usr/bin/env python3
"""Generates the test images of tests/win32/gdiplus.c and the reference values (decoded with PIL / known
pixel data). Writes the image files, images.h (image bytes + reference samples included by the test) and
expected_images.txt (the lines the decoder part of the test must print).  Run: python3 make_images.py"""
import os, struct, zlib, io
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
def out(name): return os.path.join(HERE, name)

images = []   # (name, filename, mode, samples[(x,y,argb)], extra)

def argb(r, g, b, a=255): return (a << 24) | (r << 16) | (g << 8) | b
def norm(p):  # transparent pixels print as 0 (the colour of a fully transparent pixel is not specified)
    return 0 if (p >> 24) == 0 else p

# ---- own PNG writer (16-bit grey, Adam7) -------------------------------------------------------------------
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
def write_png(path, w, h, ctype, depth, rowfn, interlace=0):
    bpp_bits = {0: 1, 2: 3, 4: 2, 6: 4}[ctype] * depth
    def rows(xs, ys):
        raw = b""
        for y in ys:
            row = b"".join(rowfn(x, y) for x in xs)
            raw += b"\x00" + row   # filter type 0 (None); a second filter is mixed in below
        return raw
    def filtered(xs, ys):
        # use Sub filter on odd rows, Up on every third to exercise the unfilter code
        raw = b""
        prev = None
        bpp = max(1, bpp_bits // 8)
        for n, y in enumerate(ys):
            row = b"".join(rowfn(x, y) for x in xs)
            ft = (n % 3)  # 0 none, 1 sub, 2 up
            if ft == 1:
                f = bytes((row[i] - (row[i - bpp] if i >= bpp else 0)) & 255 for i in range(len(row)))
            elif ft == 2:
                f = bytes((row[i] - (prev[i] if prev else 0)) & 255 for i in range(len(row)))
            else: f = row
            raw += bytes([ft]) + f
            prev = row
        return raw
    if not interlace:
        data = filtered(range(w), range(h))
    else:
        data = b""
        for ax, ay, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)):
            xs = range(ax, w, dx); ys = range(ay, h, dy)
            if len(xs) and len(ys): data += filtered(xs, ys)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)) + \
          chunk(b"pHYs", struct.pack(">IIB", 3780, 3780, 1)) + chunk(b"IDAT", zlib.compress(data)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)

# ---- PNG RGBA (PIL) -----------------------------------------------------------------------------------------
W, H = 8, 6
im = Image.new("RGBA", (W, H))
for y in range(H):
    for x in range(W):
        im.putpixel((x, y), (x * 30, 255 - y * 40, (x * y * 9) & 255, 255 - x * 25 - y * 5))
im.save(out("png_rgba.png"))
ref = Image.open(out("png_rgba.png")).convert("RGBA")
pts = [(0, 0), (7, 0), (3, 2), (7, 5), (5, 3)]
images.append(("png_rgba", "png_rgba.png", "exact", [(x, y, argb(*ref.getpixel((x, y))[:3], ref.getpixel((x, y))[3])) for x, y in pts], (W, H, 0x26200A)))

# ---- PNG palette + tRNS (PIL) ---------------------------------------------------------------------------------
im = Image.new("P", (10, 4))
pal = [0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 200, 200, 50]
im.putpalette(pal + [0] * (768 - len(pal)))
for y in range(4):
    for x in range(10):
        im.putpixel((x, y), (x + y) % 5)
im.save(out("png_pal.png"), transparency=bytes([255, 128, 0, 255, 77]))
ref = Image.open(out("png_pal.png")).convert("RGBA")
pts = [(0, 0), (1, 0), (2, 0), (3, 0), (4, 0), (9, 3)]
images.append(("png_pal", "png_pal.png", "exact", [(x, y, norm(argb(*ref.getpixel((x, y))[:3], ref.getpixel((x, y))[3]))) for x, y in pts], (10, 4, None)))

# ---- PNG 16-bit grey -----------------------------------------------------------------------------------------------
def g16(x, y):
    g8 = (x * 17 + y * 6) & 255
    return struct.pack(">H", g8 * 257)
write_png(out("png_gray16.png"), 9, 7, 0, 16, g16)
pts = [(0, 0), (8, 0), (4, 3), (8, 6)]
images.append(("png_gray16", "png_gray16.png", "exact", [(x, y, argb(*(((x * 17 + y * 6) & 255),) * 3)) for x, y in pts], (9, 7, None)))

# ---- PNG interlaced RGB (13 x 9: every Adam7 pass is used) -----------------------------------------------------------
def rgb8(x, y): return bytes([(x * 19) & 255, (y * 27) & 255, (x * y * 3) & 255])
write_png(out("png_interlaced.png"), 13, 9, 2, 8, rgb8, interlace=1)
pts = [(0, 0), (12, 0), (5, 4), (12, 8), (7, 3), (1, 7)]
images.append(("png_interlaced", "png_interlaced.png", "exact", [(x, y, argb(*rgb8(x, y))) for x, y in pts], (13, 9, 0x21808)))

# ---- GIF: transparency, 2 frames, delay -------------------------------------------------------------------------------
def gif_frame(n):
    f = Image.new("P", (12, 8), 1)
    f.putpalette([0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 0] + [0] * (768 - 15))
    d = ImageDraw.Draw(f)
    d.rectangle([2, 1, 5, 4], fill=2)                    # green block
    d.rectangle([8, 5, 10, 6], fill=3 if n == 0 else 4)  # blue, then yellow in frame 2
    d.rectangle([0, 0, 1, 1], fill=0)                    # index 0 = transparent
    return f
f0, f1 = gif_frame(0), gif_frame(1)
f0.save(out("gif_anim.gif"), save_all=True, append_images=[f1], duration=100, loop=0, transparency=0, disposal=1, optimize=False)
g = Image.open(out("gif_anim.gif"))
assert g.n_frames == 2
frames = []
for i in range(g.n_frames):
    g.seek(i); frames.append(g.convert("RGBA"))
pts0 = [(0, 0), (3, 2), (9, 5), (11, 7), (6, 6)]
samples = [(x, y, norm(argb(*frames[0].getpixel((x, y))[:3], frames[0].getpixel((x, y))[3]))) for x, y in pts0]
samples1 = [(x, y, norm(argb(*frames[1].getpixel((x, y))[:3], frames[1].getpixel((x, y))[3]))) for x, y in pts0]
assert samples[0][2] == 0 and samples[1][2] == argb(0, 255, 0) and samples[2][2] == argb(0, 0, 255) and samples1[2][2] == argb(255, 255, 0)
images.append(("gif_anim", "gif_anim.gif", "gif", samples, (12, 8, 0x30803, samples1, 10)))

# ---- JPEG (smooth content so that decoders agree within a small tolerance) -----------------------------------------------
def jpeg_src(w, h):
    im = Image.new("RGB", (w, h))
    for y in range(h):
        for x in range(w):
            im.putpixel((x, y), (x * 255 // (w - 1), y * 255 // (h - 1), 128 + 100 * ((x + y) % 7 == 0) // 4))
    d = ImageDraw.Draw(im); d.ellipse([w // 4, h // 4, 3 * w // 4, 3 * h // 4], fill=(200, 40, 40))
    return im
def add_jpeg(name, im, **kw):
    im.save(out(name + ".jpg"), quality=92, **kw)
    ref = Image.open(out(name + ".jpg")).convert("RGB")
    w, h = ref.size
    pts = [(1, 1), (w - 2, 1), (w // 2, h // 2), (w - 2, h - 2), (3, h - 4), (w // 3, h // 5), (w - 5, h // 2)]
    images.append((name, name + ".jpg", "jpeg", [(x, y, argb(*ref.getpixel((x, y)))) for x, y in pts], (w, h, 0x21808)))
add_jpeg("jpeg_420", jpeg_src(40, 36), subsampling=2)
add_jpeg("jpeg_prog", jpeg_src(37, 29), subsampling=2, progressive=True)
add_jpeg("jpeg_422", jpeg_src(33, 24), subsampling=1)
add_jpeg("jpeg_444p", jpeg_src(24, 21), subsampling=0, progressive=True)
g = jpeg_src(30, 22).convert("L"); g.save(out("jpeg_gray.jpg"), quality=92)
ref = Image.open(out("jpeg_gray.jpg")).convert("RGB")
images.append(("jpeg_gray", "jpeg_gray.jpg", "jpeg", [(x, y, argb(*ref.getpixel((x, y)))) for x, y in [(1, 1), (28, 1), (15, 11), (28, 20)]], (30, 22, None)))

# ---- BMP 24 / 8 (PIL) ------------------------------------------------------------------------------------------------------
im = Image.new("RGB", (11, 5))
for y in range(5):
    for x in range(11): im.putpixel((x, y), (x * 22, y * 50, (x * 7 + y * 11) & 255))
im.save(out("bmp24.bmp"))
ref = Image.open(out("bmp24.bmp")).convert("RGB")
images.append(("bmp24", "bmp24.bmp", "exact", [(x, y, argb(*ref.getpixel((x, y)))) for x, y in [(0, 0), (10, 0), (5, 2), (10, 4), (3, 3)]], (11, 5, 0x21808)))
im = Image.new("P", (9, 6))
pal = []
for i in range(16): pal += [i * 16, 255 - i * 16, (i * 37) & 255]
im.putpalette(pal + [0] * (768 - len(pal)))
for y in range(6):
    for x in range(9): im.putpixel((x, y), (x * 2 + y) % 16)
im.save(out("bmp8.bmp"))
ref = Image.open(out("bmp8.bmp")).convert("RGB")
images.append(("bmp8", "bmp8.bmp", "exact", [(x, y, argb(*ref.getpixel((x, y)))) for x, y in [(0, 0), (8, 0), (4, 3), (8, 5), (1, 1)]], (9, 6, 0x30803)))

# ---- ICO: PNG-in-ICO (PIL) and a hand made DIB icon ----------------------------------------------------------------------------
im = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
d = ImageDraw.Draw(im); d.rectangle([2, 2, 13, 13], fill=(30, 144, 255, 255)); d.rectangle([5, 5, 9, 9], fill=(255, 255, 255, 255))
im.save(out("ico_png.ico"), sizes=[(16, 16)])
data = open(out("ico_png.ico"), "rb").read()
assert data[22:30] == b"\x89PNG\r\n\x1a\n", "PIL did not write PNG-in-ICO"
ref = Image.open(out("ico_png.ico")).convert("RGBA")
images.append(("ico_png", "ico_png.ico", "exact", [(x, y, norm(argb(*ref.getpixel((x, y))[:3], ref.getpixel((x, y))[3]))) for x, y in [(0, 0), (2, 2), (7, 7), (13, 13), (15, 15)]], (16, 16, None)))
# DIB icon 8x8, 32 bpp with alpha + AND mask (bottom-up)
w = h = 8
def dpx(x, y):
    if x < 2 and y < 2: return (0, 0, 0, 0)
    return (x * 30, y * 30, 200, 255 if (x + y) % 3 else 128)
xor = b"".join(b"".join(bytes([dpx(x, y)[2], dpx(x, y)[1], dpx(x, y)[0], dpx(x, y)[3]]) for x in range(w)) for y in range(h - 1, -1, -1))
andm = b"".join(bytes([0]) + b"\0\0\0" for y in range(h))
dib = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(xor) + len(andm), 0, 0, 0, 0) + xor + andm
ico = struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(dib), 22) + dib
open(out("ico_dib.ico"), "wb").write(ico)
images.append(("ico_dib", "ico_dib.ico", "exact", [(x, y, norm(argb(*dpx(x, y)[:3], dpx(x, y)[3]))) for x, y in [(0, 0), (1, 1), (2, 2), (7, 7), (4, 1)]], (8, 8, None)))

# ---- outputs --------------------------------------------------------------------------------------------------------------------
def carray(name, data):
    s = "static const unsigned char %s[] = {" % name
    for i, b in enumerate(data):
        if i % 24 == 0: s += "\n  "
        s += "%d," % b
    return s + "\n};\n"
h = ["/* generated by make_images.py - do not edit */"]
for name, fn, kind, samples, extra in images:
    h.append(carray(name + "_data", open(out(fn), "rb").read()))
    h.append("static const TSample %s_ref[] = {%s};" % (name, ",".join("{%d,%d,0x%08XU}" % s for s in samples)))
    if kind == "gif":
        h.append("static const TSample %s_ref1[] = {%s};" % (name, ",".join("{%d,%d,0x%08XU}" % s for s in extra[3])))
h.append("static const TImage timages[] = {")
for name, fn, kind, samples, extra in images:
    kindc = {"exact": 0, "jpeg": 1, "gif": 2}[kind]
    w, hh, pf = extra[0], extra[1], extra[2]
    h.append('  { "%s", %s_data, sizeof %s_data, %d, %s_ref, %d, %d, %d, 0x%X, %s },' % (
        name, name, name, kindc, name, len(samples), w, hh, pf or 0, ("%s_ref1" % name) if kind == "gif" else "0"))
h.append("};")
open(out("images.h"), "w").write("\n".join(h) + "\n")

lines = []
for name, fn, kind, samples, extra in images:
    w, hh, pf = extra[0], extra[1], extra[2]
    lines.append("%s %dx%d" % (name, w, hh))
    if pf: lines.append("%s format %X" % (name, pf))
    if kind == "jpeg": lines.append("%s samples ok" % name)
    else:
        for x, y, v in samples: lines.append("%s %d,%d %08X" % (name, x, y, v))
    if kind == "gif":
        lines.append("%s frames 2 delay 10" % name)
        for x, y, v in extra[3]: lines.append("%s frame1 %d,%d %08X" % (name, x, y, v))
open(out("expected_images.txt"), "w").write("\n".join(lines) + "\n")
print("ok", len(images), "images")
