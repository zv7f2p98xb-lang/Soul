#!/usr/bin/env python3
"""A small but complete LZX encoder that produces Microsoft Cabinet (.cab) files with LZX folders.

It exists only to generate test data for Soul's LZX decoder (src/cab.c): no LZX compressor is available on
Linux. Features: hash-chain matching with lazy evaluation, verbatim / aligned-offset / uncompressed blocks,
repeated-offset matches (R0-R2), Intel E8 call translation, 32 KB frames (one CFDATA each), delta-coded trees
carried between blocks, window sizes 2^15..2^21. Every cabinet is checked with cabextract (libmspack).

  lzxenc.py fixtures            regenerate sources (src/), cabinets (cabs/) and manifest.txt next to this script
  lzxenc.py cab OUT.cab [--window N] [--e8 SIZE] [--plan v:40000,a:30000,u:5001] NAME=PATH ...
"""
import os, struct, subprocess, sys, zlib, heapq, bisect

FRAME = 32768
SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}


def pos_tables(nslots):
    eb = [0 if s < 4 else min(17, (s - 2) // 2) for s in range(nslots)]
    base = [0] * (nslots + 1)
    for s in range(1, nslots + 1):
        base[s] = base[s - 1] + (1 << eb[s - 1])
    return eb, base


class Rng:
    def __init__(self, seed):
        self.x = (seed * 0x9E3779B97F4A7C15 + 0x1234567) & (2**64 - 1) or 1

    def next(self):
        x = self.x
        x ^= x >> 12
        x ^= (x << 25) & (2**64 - 1)
        x ^= x >> 27
        self.x = x
        return ((x * 0x2545F4914F6CDD1D) & (2**64 - 1)) >> 32

    def below(self, n):
        return self.next() % n


# ---- bit writer: 16-bit little-endian words, bits taken MSB first ----------------------------------------------------
class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.nb = 0

    def write(self, value, n):
        while n > 16:
            n -= 16
            self.write((value >> n) & 0xFFFF, 16)
            value &= (1 << n) - 1
        if n == 0:
            return
        self.acc = (self.acc << n) | value
        self.nb += n
        while self.nb >= 16:
            self.nb -= 16
            w = (self.acc >> self.nb) & 0xFFFF
            self.out += struct.pack('<H', w)
            self.acc &= (1 << self.nb) - 1

    def align16(self):
        if self.nb:
            self.write(0, 16 - self.nb)


# ---- Huffman ---------------------------------------------------------------------------------------------------------
def huff_lengths(freqs, maxlen):
    n = len(freqs)
    used = [i for i in range(n) if freqs[i] > 0]
    lens = [0] * n
    if not used:
        return lens
    if len(used) == 1:
        lens[used[0]] = 1
        lens[1 if used[0] == 0 else 0] = 1
        return lens
    f = {i: freqs[i] for i in used}
    while True:
        heap = [(v, i, ('L', i)) for i, v in f.items()]
        heapq.heapify(heap)
        cnt = n
        while len(heap) > 1:
            a = heapq.heappop(heap)
            b = heapq.heappop(heap)
            cnt += 1
            heapq.heappush(heap, (a[0] + b[0], cnt, ('N', a[2], b[2])))
        depth = {}
        stack = [(heap[0][2], 0)]
        while stack:
            node, d = stack.pop()
            if node[0] == 'L':
                depth[node[1]] = d
            else:
                stack.append((node[1], d + 1))
                stack.append((node[2], d + 1))
        if max(depth.values()) <= maxlen:
            break
        f = {i: (v + 1) // 2 for i, v in f.items()}
    for i, d in depth.items():
        lens[i] = d
    return lens


def canon_codes(lens):
    maxl = max(lens) if lens else 0
    bl = [0] * (maxl + 2)
    for l in lens:
        if l:
            bl[l] += 1
    nxt = [0] * (maxl + 2)
    code = 0
    for b in range(1, maxl + 1):
        code = (code + bl[b - 1]) << 1
        nxt[b] = code
    codes = [0] * len(lens)
    for i, l in enumerate(lens):
        if l:
            codes[i] = nxt[l]
            nxt[l] += 1
    return codes


class Tree:
    def __init__(self, lens):
        self.lens = lens
        self.codes = canon_codes(lens)

    def put(self, bw, sym):
        assert self.lens[sym] > 0
        bw.write(self.codes[sym], self.lens[sym])


def encode_tree(bw, old, new, first, last):
    """Writes the pretree and the delta-coded lengths new[first:last] relative to old (LZX tree format)."""
    items = []  # (pretree symbol, extra value, extra bits, follow-up symbol or None)
    i = first
    while i < last:
        if new[i] == 0:
            j = i
            while j < last and new[j] == 0:
                j += 1
            run = j - i
            if run >= 20:
                n = min(run, 51)
                items.append((18, n - 20, 5, None))
                i += n
                continue
            if run >= 4:
                items.append((17, run - 4, 4, None))
                i += run
                continue
        j = i
        while j < last and new[j] == new[i]:
            j += 1
        run = j - i
        z = (old[i] - new[i]) % 17
        if run >= 4:
            n = min(run, 5)
            items.append((19, n - 4, 1, z))
            i += n
            continue
        items.append((z, 0, 0, None))
        i += 1
    freq = [0] * 20
    for s, _, _, f in items:
        freq[s] += 1
        if f is not None:
            freq[f] += 1
    pl = huff_lengths(freq, 15)
    pt = Tree(pl)
    for l in pl:
        bw.write(l, 4)
    for s, ev, eb, f in items:
        pt.put(bw, s)
        if eb:
            bw.write(ev, eb)
        if f is not None:
            pt.put(bw, f)


# ---- Intel E8 translation (encoder direction) -------------------------------------------------------------------------
def e8_translate(data, filesize):
    out = bytearray(data)
    for f in range(min(32768, (len(data) + FRAME - 1) // FRAME)):
        base = f * FRAME
        fl = min(FRAME, len(data) - base)
        if fl <= 10:
            continue
        i = 0
        end = fl - 10
        while i < end:
            if out[base + i] != 0xE8:
                i += 1
                continue
            curpos = base + i
            rel = struct.unpack_from('<i', out, base + i + 1)[0]
            if -curpos <= rel < filesize:
                if rel < filesize - curpos:
                    a = rel + curpos
                else:
                    a = rel - filesize
                struct.pack_into('<i', out, base + i + 1, a)
            i += 5
    return bytes(out)


# ---- the encoder -----------------------------------------------------------------------------------------------------
class Stats(dict):
    def add(self, k, n=1):
        self[k] = self.get(k, 0) + n


class LZXEncoder:
    def __init__(self, data, wbits, plan, e8size=0, depth=24):
        assert wbits in SLOTS
        self.wbits = wbits
        self.window = 1 << wbits
        self.maxoff = self.window - 3
        self.nslots = SLOTS[wbits]
        self.EB, self.BASE = pos_tables(self.nslots)
        self.n = len(data)
        self.e8size = e8size
        self.T = e8_translate(data, e8size) if e8size else bytes(data)
        self.plan = plan
        self.depth = depth
        self.hash = {}
        self.hashed = 0
        self.R = [1, 1, 1]
        self.stats = Stats()

    # hash chain
    def insert_upto(self, upto):
        T = self.T
        while self.hashed < upto:
            p = self.hashed
            if p + 2 < self.n:
                self.hash.setdefault((T[p] << 16) | (T[p + 1] << 8) | T[p + 2], []).append(p)
            self.hashed += 1

    def best_match(self, i, maxlen):
        T = self.T
        if maxlen < 2:
            return (0, 0)
        rb = (0, 0)
        for r in self.R:
            if r <= i and r <= self.maxoff:
                l = 0
                while l < maxlen and T[i + l] == T[i + l - r]:
                    l += 1
                if l >= 2 and l > rb[0]:
                    rb = (l, r)
        cb = (0, 0)
        if maxlen >= 3 and i + 2 < self.n:
            chain = self.hash.get((T[i] << 16) | (T[i + 1] << 8) | T[i + 2])
            if chain:
                d = 0
                for p in reversed(chain):
                    if p >= i:
                        continue
                    off = i - p
                    if off > self.maxoff:
                        break
                    l = 3
                    while l < maxlen and T[i + l] == T[p + l]:
                        l += 1
                    if l > cb[0]:
                        cb = (l, off)
                        if l == maxlen:
                            break
                    d += 1
                    if d >= self.depth:
                        break
        if rb[0] and rb[0] + 2 >= cb[0]:
            return rb
        if cb[0] >= 3:
            return cb
        return rb

    def make_match(self, l, off):
        R = self.R
        if off == R[0]:
            slot, extra = 0, 0
            self.stats.add('R0')
        elif off == R[1]:
            slot, extra = 1, 0
            R[0], R[1] = R[1], R[0]
            self.stats.add('R1')
        elif off == R[2]:
            slot, extra = 2, 0
            R[0], R[2] = R[2], R[0]
            self.stats.add('R2')
        else:
            fo = off + 2
            slot = bisect.bisect_right(self.BASE, fo) - 1
            assert 3 <= slot < self.nslots, (off, slot)
            extra = fo - self.BASE[slot]
            assert extra < (1 << self.EB[slot])
            R[2], R[1], R[0] = R[1], R[0], off
            self.stats.add('new')
            self.stats.add('maxeb%d' % self.EB[slot])
        return ('M', l, slot, extra)

    def tokenise(self, bs, be):
        toks = []
        i = bs
        T = self.T
        while i < be:
            fe = min(be, (i // FRAME + 1) * FRAME)
            ml = min(257, fe - i)
            self.insert_upto(i)
            m = self.best_match(i, ml)
            if m[0] >= 2 and i + 1 < fe:
                self.insert_upto(i + 1)
                m2 = self.best_match(i + 1, min(257, fe - i - 1))
                if m2[0] > m[0] + 1:
                    m = (0, 0)
            if m[0] >= 2:
                toks.append(self.make_match(m[0], m[1]))
                i += m[0]
            else:
                toks.append(('L', T[i]))
                self.stats.add('lit')
                i += 1
        self.insert_upto(be)
        return toks

    def encode(self):
        """Returns the list of compressed frames (one per 32768 bytes of input, the last one shorter)."""
        T, n = self.T, self.n
        bw = BitWriter()
        cuts = []
        if self.e8size:
            bw.write(1, 1)
            bw.write(self.e8size, 32)
        else:
            bw.write(0, 1)
        mainold = [0] * (256 + 8 * self.nslots)
        lenold = [0] * 249
        pos = 0
        pad_pending = False
        bi = 0

        def frame_end_check(p):
            if p % FRAME == 0 or p == n:
                bw.align16()
                cuts.append(len(bw.out))

        if n == 0:
            return []
        while pos < n:
            btype, bsize = self.plan[bi % len(self.plan)]
            bi += 1
            bsize = min(bsize, n - pos, (1 << 24) - 1)
            if pad_pending:
                assert bw.nb == 0
                bw.out.append(0)
                pad_pending = False
            tcode = {'v': 1, 'a': 2, 'u': 3}[btype]
            bw.write(tcode, 3)
            bw.write(bsize, 24)
            self.stats.add('block_' + btype)
            if btype == 'u':
                pad = 16 - bw.nb if bw.nb else 16
                bw.write(0, pad)
                assert bw.nb == 0
                bw.out += struct.pack('<III', *self.R)
                p = pos
                while p < pos + bsize:
                    e = min(pos + bsize, (p // FRAME + 1) * FRAME)
                    bw.out += T[p:e]
                    p = e
                    frame_end_check(p)
                self.insert_upto(pos + bsize)
                self.stats.add('ublock_bytes', bsize)
                pad_pending = bool(bsize & 1)
                pos += bsize
                continue
            toks = self.tokenise(pos, pos + bsize)
            mainf = [0] * (256 + 8 * self.nslots)
            lenf = [0] * 249
            alf = [0] * 8
            for t in toks:
                if t[0] == 'L':
                    mainf[t[1]] += 1
                else:
                    _, l, slot, extra = t
                    mainf[256 + slot * 8 + min(l - 2, 7)] += 1
                    if l - 2 >= 7:
                        lenf[l - 9] += 1
                    if btype == 'a' and slot >= 3 and self.EB[slot] >= 3:
                        alf[extra & 7] += 1
            mainl = huff_lengths(mainf, 16)
            lenl = huff_lengths(lenf, 16)
            if btype == 'a':
                if not any(alf):
                    alf = [1] * 8      # libmspack rejects an all-zero aligned tree
                all_ = huff_lengths(alf, 7)
                for l in all_:
                    bw.write(l, 3)
                at = Tree(all_)
            encode_tree(bw, mainold, mainl, 0, 256)
            encode_tree(bw, mainold, mainl, 256, 256 + 8 * self.nslots)
            encode_tree(bw, lenold, lenl, 0, 249)
            mainold, lenold = mainl, lenl
            mt, lt = Tree(mainl), Tree(lenl)
            p = pos
            for t in toks:
                if t[0] == 'L':
                    mt.put(bw, t[1])
                    p += 1
                else:
                    _, l, slot, extra = t
                    hdr = min(l - 2, 7)
                    mt.put(bw, 256 + slot * 8 + hdr)
                    if hdr == 7:
                        lt.put(bw, l - 9)
                    if slot >= 3:
                        eb = self.EB[slot]
                        if btype == 'a' and eb >= 3:
                            if eb > 3:
                                bw.write(extra >> 3, eb - 3)
                            at.put(bw, extra & 7)
                        elif eb:
                            bw.write(extra, eb)
                    p += l
                frame_end_check(p)
            pos += bsize
        assert pos == n
        # split the stream at the recorded cut points
        frames = []
        prev = 0
        for c in cuts:
            frames.append(bytes(bw.out[prev:c]))
            prev = c
        assert prev == len(bw.out)
        assert len(frames) == (n + FRAME - 1) // FRAME, (len(frames), n)
        return frames


def parse_plan(s):
    return [(x.split(':')[0], int(x.split(':')[1])) for x in s.split(',')]


# ---- cabinet writer --------------------------------------------------------------------------------------------------
def cab_checksum(data, seed=0):
    c = seed
    n4 = len(data) // 4
    for (w,) in struct.iter_unpack('<I', data[:n4 * 4]):
        c ^= w
    rem = data[n4 * 4:]
    ul = 0
    if len(rem) == 3:
        ul = rem[0] << 16 | rem[1] << 8 | rem[2]
    elif len(rem) == 2:
        ul = rem[0] << 8 | rem[1]
    elif len(rem) == 1:
        ul = rem[0]
    return c ^ ul


def make_cab(folders, stats=None):
    """folders: list of dicts {files: [(name, bytes)], window: 15..21, e8: size or 0, plan: [(type,size)]}"""
    fdata = []
    for fd in folders:
        data = b''.join(d for _, d in fd['files'])
        enc = LZXEncoder(data, fd['window'], fd['plan'], fd.get('e8', 0), fd.get('depth', 24))
        frames = enc.encode()
        if stats is not None:
            for k, v in enc.stats.items():
                stats.add(k, v)
        cus = [FRAME] * (len(data) // FRAME)
        if len(data) % FRAME:
            cus.append(len(data) % FRAME)
        fdata.append((frames, cus))
    nf = len(folders)
    files_blob = bytearray()
    nfiles = 0
    for fi, fd in enumerate(folders):
        off = 0
        for name, d in fd['files']:
            files_blob += struct.pack('<IIHHHH', len(d), off, fi, 0x5A21, 0x6000, 0x20) + name.encode('ascii') + b'\0'
            off += len(d)
            nfiles += 1
    coff_files = 36 + 8 * nf
    coff_data = coff_files + len(files_blob)
    folder_tab = bytearray()
    data_blob = bytearray()
    for fi, fd in enumerate(folders):
        frames, cus = fdata[fi]
        folder_tab += struct.pack('<IHH', coff_data + len(data_blob), len(frames), 3 | (fd['window'] << 8))
        for fr, cu in zip(frames, cus):
            assert len(fr) < 65536
            seed = cab_checksum(struct.pack('<HH', len(fr), cu))
            data_blob += struct.pack('<IHH', cab_checksum(fr, seed), len(fr), cu) + fr
    total = coff_data + len(data_blob)
    hdr = b'MSCF' + struct.pack('<IIIIIBBHHHHH', 0, total, 0, coff_files, 0, 3, 1, nf, nfiles, 0, 0x4C58, 0)
    assert len(hdr) == 36
    return bytes(hdr) + bytes(folder_tab) + bytes(files_blob) + bytes(data_blob)


def verify_with_cabextract(cab_path, folders, tmp):
    import shutil
    d = os.path.join(tmp, 'x')
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    r = subprocess.run(['cabextract', '-t', cab_path], capture_output=True, text=True)
    if r.returncode != 0 or 'All done, no errors' not in r.stdout:
        raise SystemExit('cabextract -t failed for %s:\n%s%s' % (cab_path, r.stdout, r.stderr))
    r = subprocess.run(['cabextract', '-q', '-d', d, cab_path], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('cabextract failed for %s:\n%s%s' % (cab_path, r.stdout, r.stderr))
    for fd in folders:
        for name, data in fd['files']:
            got = open(os.path.join(d, name), 'rb').read()
            if got != data:
                raise SystemExit('cabextract output differs for %s in %s' % (name, cab_path))
    shutil.rmtree(d, ignore_errors=True)


# ---- deterministic test inputs ---------------------------------------------------------------------------------------
def vocab(seed, count):
    r = Rng(seed)
    letters = b'etaoinshrdlucmfwypvbgkqjxz'
    words = []
    for _ in range(count):
        l = 2 + r.below(8)
        words.append(bytes(letters[min(r.below(26), r.below(26))] for _ in range(l)))
    return words


def text_like(n, seed, words=400):
    r = Rng(seed)
    v = vocab(seed ^ 0x55, words)
    out = bytearray()
    while len(out) < n:
        out += v[min(r.below(len(v)), r.below(len(v)))]
        k = r.below(20)
        out += b'.\r\n' if k == 0 else (b', ' if k == 1 else b' ')
    return bytes(out[:n])


def x86_like(n, seed):
    r = Rng(seed)
    out = bytearray()
    proto = bytes([0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x53, 0x56, 0x57, 0x8B, 0x7D, 0x08])
    while len(out) < n:
        k = r.below(100)
        if k < 24:
            rel = r.below(0x6000) - 0x2000
            out += b'\xE8' + struct.pack('<i', rel)
        elif k < 30:
            out += b'\x83\xEC' + bytes([r.below(8) * 4])
        elif k < 46:
            out += bytes([0x8B, 0x45 + 8 * r.below(4), r.below(32)])
        elif k < 54:
            out += b'\x68' + struct.pack('<I', 0x400000 + r.below(0x8000))
        elif k < 60:
            out += b'\xC3\xCC\xCC'
        elif k < 72:
            out += bytes(r.below(256) for _ in range(1 + r.below(6)))
        elif k < 78:
            out += b'\xE8' + struct.pack('<I', r.next())     # huge operand: left untranslated
        elif k < 84:
            out += b'\x00' * r.below(12)
        elif k < 90:
            out += proto
        else:
            out += b'\x90' * (1 + r.below(4)) + b'\xFF\x15' + struct.pack('<I', 0x402000 + 4 * r.below(64))
    return bytes(out[:n])


def records(n, seed):
    """Interleaved fixed-stride records of two kinds: produces repeated-offset matches (R0, R1 and R2)."""
    r = Rng(seed)
    out = bytearray()
    ida, idb = 0, 1000
    while len(out) < n:
        out += b'\x01\x00REC-A\x00\x00' + struct.pack('<I', ida) + b'\xAA' * 4 + bytes([r.below(256), r.below(4)]) + b'\x00' * 6
        ida += 1
        if r.below(3):
            out += b'B>' + struct.pack('<IH', idb, r.below(65536)) + b'.pad.pad'
            idb += 3
        if r.below(5) == 0:
            out += b'\x02\x00REC-C\x00\x00' + struct.pack('<IH', r.next(), r.below(9))
    return bytes(out[:n])


def far_repeat(n, seed):
    """Text where later parts repeat earlier parts 270+ KB away (hits 17 extra-bit position slots, window 21)."""
    a = text_like(272000, seed, words=6000)
    mid = text_like(n - 272000 - 14000, seed + 1, words=6000)
    out = a + mid[:5000] + a[3000:17000] + mid[5000:]
    return out[:n]


def edge_e8(filesize):
    """E8 operands at the interesting boundaries; the file must be the first in its folder (position == offset)."""
    d = bytearray(b'\x90' * 70000)
    spots = [0, 100, 5000, 32746, 32751, 32757, 32762, 32767, 32773, 65520, 65525, 65531, 65535, 66000]
    spots += list(range(40000, 40000 + 11 * 8, 8))
    for k, p in enumerate(spots):
        v = [-p, -p - 1, -p + 1, 0, filesize - 1, filesize, filesize + 1, 12345, -1, filesize - p - 1, filesize - p][k % 11]
        d[p] = 0xE8
        d[p + 1:p + 5] = struct.pack('<i', v)
    return bytes(d)


def lzx_fixtures(here):
    src = os.path.join(here, 'src')
    cabs = os.path.join(here, 'cabs')
    os.makedirs(src, exist_ok=True)
    os.makedirs(cabs, exist_ok=True)
    text = text_like(60000, 1)
    code = x86_like(100001, 2)
    table = records(50001, 3)
    tiny = b'tiny file, 31 bytes of text....\n'[:17]
    far = far_repeat(292000, 4)
    late = text_like(70000, 5) + x86_like(50000, 6)
    edge_size = 0x30000
    edge = edge_e8(edge_size)
    rr = Rng(7)
    rnd = bytes(rr.below(256) for _ in range(20000))     # incompressible: forces literal-heavy blocks
    runs = (b'A' * 5000 + b'xyz' * 3000 + bytes(range(256)) * 20 + b'\0' * 70000)[:70000]
    all_src = {'text.txt': text, 'code.exe': code, 'table.dat': table, 'empty.bin': b'', 'tiny.txt': tiny,
               'far.dat': far, 'late.exe': late, 'edge.bin': edge, 'random.bin': rnd, 'runs.dat': runs}
    for k, v in all_src.items():
        open(os.path.join(src, k), 'wb').write(v)
    P = parse_plan
    cabsdef = {
        # window 15: many small blocks of every type, odd-length uncompressed blocks
        'lzx15.cab': [dict(window=15, e8=0xA00000, plan=P('v:20000,a:30000,u:7001,v:41111,u:12000,a:100000,v:5,u:1,a:77777'),
                           files=[('text.txt', text), ('tiny.txt', tiny), ('empty.bin', b''), ('code.exe', code), ('table.dat', table)])],
        # window 16: E8 on, uncompressed block that ends exactly on a frame boundary with odd size
        'lzx16.cab': [dict(window=16, e8=0x123456, plan=P('a:32767,u:32769,v:65536,a:11,u:99999,v:200000'),
                           files=[('code.exe', code), ('empty.bin', b''), ('table.dat', table), ('random.bin', rnd), ('tiny.txt', tiny)])],
        # window 21 + a second folder (window 17, no E8) in the same cabinet, files spread over frames
        'lzx21.cab': [dict(window=21, e8=0x2000000, plan=P('v:150000,a:150000'), depth=64,
                           files=[('far.dat', far), ('empty.bin', b''), ('code.exe', code)]),
                      dict(window=17, e8=0, plan=P('a:60000,v:60000,u:12345'),
                           files=[('text.txt', text), ('runs.dat', runs), ('tiny.txt', tiny)])],
        # E8 first appears after the first two frames
        'lzxlate.cab': [dict(window=17, e8=0x800000, plan=P('v:70000,a:100000'), files=[('late.exe', late)])],
        # E8 operands at the translation range boundaries and frame ends
        'lzxe8.cab': [dict(window=15, e8=edge_size, plan=P('v:70000'), files=[('edge.bin', edge)])],
        # one tiny file (a single partial frame), E8 flag set but frame <= 10 bytes irrelevant
        'lzxtiny.cab': [dict(window=15, e8=0x100000, plan=P('v:100'), files=[('tiny.txt', tiny), ('e8.bin', b'\xE8\x00\x00\x00\x00\x90')])],
    }
    all_src['e8.bin'] = b'\xE8\x00\x00\x00\x00\x90'
    open(os.path.join(src, 'e8.bin'), 'wb').write(all_src['e8.bin'])
    stats = Stats()
    manifest = []
    tmp = os.environ.get('TMPDIR', '/tmp')
    for name, folders in cabsdef.items():
        cab = make_cab(folders, stats)
        path = os.path.join(cabs, name)
        open(path, 'wb').write(cab)
        verify_with_cabextract(path, folders, tmp)
        for fd in folders:
            for fn, d in fd['files']:
                manifest.append('%s %s %d %08x' % (name, fn, len(d), zlib.crc32(d)))
        print('%-14s %8d bytes  verified with cabextract' % (name, len(cab)))
    open(os.path.join(here, 'manifest.txt'), 'w').write('\n'.join(manifest) + '\n')
    print('encoder statistics:', dict(sorted(stats.items())))
    return stats


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    if len(sys.argv) >= 2 and sys.argv[1] == 'fixtures':
        lzx_fixtures(here)
        return
    if len(sys.argv) >= 3 and sys.argv[1] == 'cab':
        out = sys.argv[2]
        window, e8, plan = 21, 0x1000000, 'v:150000,a:150000,u:20001'
        files = []
        a = sys.argv[3:]
        while a:
            if a[0] == '--window':
                window = int(a[1]); a = a[2:]
            elif a[0] == '--e8':
                e8 = int(a[1], 0); a = a[2:]
            elif a[0] == '--plan':
                plan = a[1]; a = a[2:]
            else:
                n, p = a[0].split('=', 1)
                files.append((n, open(p, 'rb').read()))
                a = a[1:]
        fd = [dict(window=window, e8=e8, plan=parse_plan(plan), files=files)]
        open(out, 'wb').write(make_cab(fd))
        verify_with_cabextract(out, fd, os.environ.get('TMPDIR', '/tmp'))
        print('wrote', out)
        return
    print(__doc__)
    sys.exit(2)


if __name__ == '__main__':
    main()
