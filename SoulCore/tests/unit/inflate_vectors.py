#!/usr/bin/env python3
# Generates DEFLATE test vectors: <name>.raw (original), <name>.def (raw deflate) or .zlib, plus a manifest.
import os, random, sys, zlib

out = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(out, exist_ok=True)
rnd = random.Random(1234)

def text(n):
    words = ["soul", "core", "install", "registry", "window", "cabinet", "deflate", "huffman", "stream", "the", "of", "and", "x86", "arm64", "jit"]
    s = []
    size = 0
    while size < n:
        w = rnd.choice(words) + rnd.choice([" ", " ", ", ", ".\n"])
        s.append(w); size += len(w)
    return "".join(s).encode()[:n]

datas = {
    "empty": b"",
    "tiny": b"a",
    "random": bytes(rnd.getrandbits(8) for _ in range(300000)),
    "text": text(400000),
    "repeat": (b"abcabcabd" * 50000)[:450000],
    "zeros": bytes(300000),
    "mixed": text(100000) + bytes(rnd.getrandbits(8) for _ in range(50000)) + b"\x00" * 70000 + text(100000),
}

def raw(data, level, zdict=None):
    kw = {"zdict": zdict} if zdict else {}
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, zlib.Z_DEFAULT_STRATEGY, **kw)
    return c.compress(data) + c.flush()

man = open(os.path.join(out, "inflate_manifest.txt"), "w")
def emit(name, kind, comp, orig, dictname="-"):
    open(os.path.join(out, name + ".comp"), "wb").write(comp)
    open(os.path.join(out, name + ".orig"), "wb").write(orig)
    man.write("%s %s %s\n" % (name, kind, dictname))

for dn, d in datas.items():
    for lvl in (0, 1, 9):
        emit("%s_l%d" % (dn, lvl), "raw", raw(d, lvl), d)
    emit("%s_zlib" % dn, "zlib", zlib.compress(d, 6), d)
    # Z_FIXED forces fixed-Huffman blocks
    c = zlib.compressobj(6, zlib.DEFLATED, -15, 9, zlib.Z_FIXED)
    emit("%s_fixed" % dn, "raw", c.compress(d) + c.flush(), d)

# preset dictionary: the stream refers back into the previous block's output
prev = text(32768)
cur = prev[1000:9000] + text(20000) + prev[20000:30000]
open(os.path.join(out, "dict.bin"), "wb").write(prev)
emit("withdict", "dict", raw(cur, 9, prev), cur, "dict.bin")
man.close()
