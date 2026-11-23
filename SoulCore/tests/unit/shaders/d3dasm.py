#!/usr/bin/env python3
"""d3dasm.py - a small Direct3D 9 shader assembler (vs_1_1 .. vs_3_0, ps_1_1 .. ps_3_0).

Emits bytecode laid out like Microsoft's assembler: version token, instruction tokens (length in
bits 24-27 for shader model 2+ only), destination/source parameter tokens, relative address tokens,
end token. Used to generate the test shader token arrays (d3dsh_shaders.h).

Usage: d3dasm.py out.h in1.asm [in2.asm ...]     (a file holds one or more shaders, each started by a
                                                   "### name" line)
       d3dasm.py -d file.asm                     (hex dump of every shader, for debugging)
"""
import re, struct, sys, os

OPS = {  # name: (opcode, has_dst, n_src)
    'nop': (0, 0, 0), 'mov': (1, 1, 1), 'add': (2, 1, 2), 'sub': (3, 1, 2), 'mad': (4, 1, 3), 'mul': (5, 1, 2),
    'rcp': (6, 1, 1), 'rsq': (7, 1, 1), 'dp3': (8, 1, 2), 'dp4': (9, 1, 2), 'min': (10, 1, 2), 'max': (11, 1, 2),
    'slt': (12, 1, 2), 'sge': (13, 1, 2), 'exp': (14, 1, 1), 'log': (15, 1, 1), 'lit': (16, 1, 1), 'dst': (17, 1, 2),
    'lrp': (18, 1, 3), 'frc': (19, 1, 1), 'm4x4': (20, 1, 2), 'm4x3': (21, 1, 2), 'm3x4': (22, 1, 2), 'm3x3': (23, 1, 2),
    'm3x2': (24, 1, 2), 'call': (25, 0, 1), 'callnz': (26, 0, 2), 'loop': (27, 0, 2), 'ret': (28, 0, 0), 'endloop': (29, 0, 0),
    'label': (30, 0, 1), 'pow': (32, 1, 2), 'crs': (33, 1, 2), 'sgn': (34, 1, 3), 'abs': (35, 1, 1), 'nrm': (36, 1, 1),
    'sincos': (37, 1, -1), 'rep': (38, 0, 1), 'endrep': (39, 0, 0), 'if': (40, 0, 1), 'else': (42, 0, 0), 'endif': (43, 0, 0),
    'break': (44, 0, 0), 'mova': (46, 1, 1),
    'texcoord': (64, 1, -1), 'texcrd': (64, 1, -1), 'texkill': (65, 1, 0), 'tex': (66, 1, 0), 'texld': (66, 1, -1),
    'texldp': (66, 1, 2), 'texldb': (66, 1, 2), 'texbem': (67, 1, 1), 'texbeml': (68, 1, 1), 'texreg2ar': (69, 1, 1),
    'texreg2gb': (70, 1, 1), 'texm3x2pad': (71, 1, 1), 'texm3x2tex': (72, 1, 1), 'texm3x3pad': (73, 1, 1),
    'texm3x3tex': (74, 1, 1), 'texm3x3spec': (76, 1, 2), 'texm3x3vspec': (77, 1, 1), 'expp': (78, 1, 1), 'logp': (79, 1, 1),
    'cnd': (80, 1, 3), 'texreg2rgb': (82, 1, 1), 'texdp3tex': (83, 1, 1), 'texm3x2depth': (84, 1, 1), 'texdp3': (85, 1, 1),
    'texm3x3': (86, 1, 1), 'texdepth': (87, 1, 0), 'cmp': (88, 1, 3), 'bem': (89, 1, 2), 'dp2add': (90, 1, 3),
    'dsx': (91, 1, 1), 'dsy': (92, 1, 1), 'texldd': (93, 1, 4), 'setp': (94, 1, 2), 'texldl': (95, 1, 2), 'breakp': (96, 0, 1),
}
CMP = {'gt': 1, 'eq': 2, 'ge': 3, 'lt': 4, 'ne': 5, 'le': 6}
USAGE = {'position': 0, 'blendweight': 1, 'blendindices': 2, 'normal': 3, 'psize': 4, 'texcoord': 5, 'tangent': 6,
         'binormal': 7, 'tessfactor': 8, 'positiont': 9, 'color': 10, 'fog': 11, 'depth': 12, 'sample': 13}
SRCMOD = {'neg': 1, 'bias': 2, 'bx2': 4, 'comp': 6, 'x2': 7, 'dz': 9, 'dw': 10, 'abs': 11}
SHIFT = {'x2': 1, 'x4': 2, 'x8': 3, 'd2': 15, 'd4': 14, 'd8': 13}


class AsmError(Exception):
    pass


def regtok(rtype, num):
    return 0x80000000 | (num & 0x7FF) | ((rtype & 7) << 28) | (((rtype >> 3) & 3) << 11)


class Asm:
    def __init__(self):
        self.t = []
        self.major = 0
        self.pixel = False
        self.minor = 0

    # ---- register parsing ---------------------------------------------------------------
    def reg(self, name):
        """name like r0, c[a0.x+3] pieces already stripped; returns (type, num)"""
        m = re.match(r'^(oPos|oFog|oPts|oDepth|vPos|vFace|aL|a0|p0|oD|oT|oC|o|r|v|c|b|i|s|t|l|x)(\d*)$', name)
        if not m:
            raise AsmError('bad register ' + name)
        k, n = m.group(1), m.group(2)
        n = int(n) if n else 0
        simple = {'oPos': (4, 0), 'oFog': (4, 1), 'oPts': (4, 2), 'oDepth': (9, 0), 'vPos': (17, 0), 'vFace': (17, 1),
                  'aL': (15, 0), 'p0': (19, 0)}
        if k in simple:
            return simple[k]
        if k == 'a0':
            return (3, 0)
        if k == 'r' or k == 'x':
            return (0, n)
        if k == 'v':
            return (1, n)
        if k == 'c':
            return (2, n)
        if k == 't':
            return (3, n)
        if k == 'oD':
            return (5, n)
        if k == 'oT':
            return (6, n)
        if k == 'o':
            return (6, n)
        if k == 'i':
            return (7, n)
        if k == 'oC':
            return (8, n)
        if k == 's':
            return (10, n)
        if k == 'b':
            return (14, n)
        if k == 'l':
            return (18, n)
        raise AsmError('bad register ' + name)

    @staticmethod
    def comps(sw):
        m = {'x': 0, 'y': 1, 'z': 2, 'w': 3, 'r': 0, 'g': 1, 'b': 2, 'a': 3}
        try:
            return [m[c] for c in sw]
        except KeyError:
            raise AsmError('bad swizzle ' + sw)

    def parse_regref(self, text):
        """register with optional relative part: returns (type, num, rel) rel = None | ('a0', comp) | ('aL', 0)"""
        m = re.match(r'^(\w+?)(\d*)\[(.*)\]$', text)
        if not m:
            t, n = self.reg(text)
            return t, n, None
        base = m.group(1) + m.group(2)
        t, n = self.reg(base) if m.group(2) else self.reg(m.group(1) + '0')
        if not m.group(2):
            n = 0
        inner = m.group(3).replace(' ', '')
        rel = None
        off = 0
        for term in re.findall(r'[+-]?[^+-]+', inner):
            sign = -1 if term.startswith('-') else 1
            term = term.lstrip('+-')
            if term.startswith('a0'):
                comp = 'xyzw'.index(term[3]) if len(term) > 3 else 0
                rel = ('a0', comp)
            elif term == 'aL':
                rel = ('aL', 0)
            else:
                off += sign * int(term)
        return t, n + off, rel

    def src(self, text, allow_mod=True):
        text = text.strip()
        mod = 0
        neg = False
        if text.startswith('!'):
            mod = 13
            text = text[1:]
        elif text.startswith('1-'):
            mod = 6
            text = text[2:]
        elif text.startswith('-'):
            neg = True
            text = text[1:]
        # split off swizzle (outside brackets) and underscore modifiers
        depth = 0
        cut = None
        for i, ch in enumerate(text):
            if ch == '[':
                depth += 1
            elif ch == ']':
                depth -= 1
            elif ch == '.' and depth == 0:
                cut = i
                break
        body, swz = (text[:cut], text[cut + 1:]) if cut is not None else (text, '')
        mods = []
        # modifiers may sit before or after the swizzle
        m2 = re.match(r'^([a-z0-9_]+?)((?:_(?:abs|bx2|bias|x2|dz|dw))+)$', swz)
        if m2:
            swz = m2.group(1)
            mods += m2.group(2).split('_')[1:]
        m1 = re.match(r'^(.*?)((?:_(?:abs|bx2|bias|x2|dz|dw))+)$', body)
        if m1:
            body = m1.group(1)
            mods += m1.group(2).split('_')[1:]
        t, n, rel = self.parse_regref(body)
        sw = 0xE4
        if swz:
            c = self.comps(swz)
            c = (c + [c[-1]] * 4)[:4]
            sw = c[0] | c[1] << 2 | c[2] << 4 | c[3] << 6
        for md in mods:
            base = SRCMOD[md]
            if md == 'abs':
                mod = 12 if neg else 11
                neg = False
            elif md == 'bx2':
                mod = 5 if neg else 4
                neg = False
            elif md == 'bias':
                mod = 3 if neg else 2
                neg = False
            elif md == 'x2':
                mod = 8 if neg else 7
                neg = False
            else:
                mod = base
        if neg:
            if mod not in (0,):
                raise AsmError('cannot combine - with ' + text)
            mod = 1
        tok = regtok(t, n) | (sw << 16) | (mod << 24)
        out = []
        if rel:
            tok |= 0x2000
            out.append(tok)
            if self.major >= 2:
                if rel[0] == 'a0':
                    out.append(regtok(3, 0) | (rel[1] << 16))
                else:
                    out.append(regtok(15, 0) | (0xE4 << 16))
            return out
        return [tok]

    def dst(self, text, mods):
        text = text.strip()
        if '.' in text:
            r, m = text.split('.', 1)
            c = self.comps(m)
            mask = 0
            for x in c:
                mask |= 1 << x
        else:
            r, mask = text, 0xF
        t, n = self.reg(r)
        tok = regtok(t, n) | (mask << 16)
        shift = 0
        for md in mods:
            if md == 'sat':
                tok |= 1 << 20
            elif md == 'pp':
                tok |= 2 << 20
            elif md == 'centroid':
                tok |= 4 << 20
            elif md in SHIFT:
                shift = SHIFT[md]
            else:
                raise AsmError('bad modifier ' + md)
        tok |= shift << 24
        return tok

    # ---- statements ----------------------------------------------------------------------
    def emit(self, op, ctl, operands, pred=None, coissue=False):
        n = len(operands) + (1 if pred is not None else 0)
        tok = op | (ctl << 16)
        if self.major >= 2:
            tok |= n << 24
        if pred is not None:
            tok |= 1 << 28
        if coissue:
            tok |= 1 << 30
        self.t.append(tok)
        self.t.extend(operands)
        if pred is not None:
            self.t.append(pred)

    def line(self, ln):
        coissue = False
        if ln.startswith('+'):
            coissue = True
            ln = ln[1:].strip()
        pred = None
        m = re.match(r'^\((!?)(p0)(?:\.(\w+))?\)\s*(.*)$', ln)
        if m:
            sw = 0xE4
            if m.group(3):
                c = self.comps(m.group(3))
                c = (c + [c[-1]] * 4)[:4]
                sw = c[0] | c[1] << 2 | c[2] << 4 | c[3] << 6
            pred = regtok(19, 0) | (sw << 16) | ((13 if m.group(1) else 0) << 24)
            ln = m.group(4)
        parts = ln.split(None, 1)
        mnem = parts[0].lower()
        args = [a.strip() for a in re.split(r',(?![^\[]*\])', parts[1])] if len(parts) > 1 else []
        if mnem == 'phase':
            self.t.append(0xFFFD)
            return
        segs = mnem.split('_')
        base = segs[0]
        mods = segs[1:]
        if base == 'def':
            r = self.dst(args[0], [])
            vals = [struct.unpack('<I', struct.pack('<f', float(a)))[0] for a in args[1:5]]
            return self.emit(81, 0, [r] + vals)
        if base == 'defi':
            vals = [int(a) & 0xFFFFFFFF for a in args[1:5]]
            return self.emit(48, 0, [self.dst(args[0], [])] + vals)
        if base == 'defb':
            return self.emit(47, 0, [self.dst(args[0], []), 1 if args[1].lower() in ('true', '1') else 0])
        if base == 'dcl':
            return self.dcl(mods, args)
        ctl = 0
        name = base
        if base == 'if' and mods and mods[0] in CMP:
            name = 'ifc'
        if base == 'if' and mods and mods[0] == 'pred':
            mods = mods[1:]
        if base == 'break' and mods and mods[0] in CMP:
            name = 'breakc'
        if base == 'break' and mods and mods[0] == 'pred':
            name = 'breakp'
        if base == 'callnz' and mods and mods[0] == 'pred':
            mods = mods[1:]
        if base == 'setp':
            name = 'setp'
        if name == 'ifc':
            op, hd, ns = 41, 0, 2
        elif name == 'breakc':
            op, hd, ns = 45, 0, 2
        elif name not in OPS:
            raise AsmError('unknown instruction ' + mnem)
        else:
            op, hd, ns = OPS[name]
        if name in ('ifc', 'breakc', 'setp'):
            ctl = CMP[mods[0]]
            mods = mods[1:]
        if name == 'texldp':
            ctl = 1
        if name == 'texldb':
            ctl = 2
        ops = []
        i = 0
        if hd:
            ops.append(self.dst(args[0], mods))
            i = 1
        rest = args[i:]
        for a in rest:
            if op in (25, 26, 30) and a.startswith('l') and re.match(r'^l\d+$', a):
                ops.append(regtok(18, int(a[1:])))
            else:
                ops.extend(self.src(a))
        if ns >= 0 and len(rest) != ns and name not in ('tex', 'texkill', 'texdepth'):
            raise AsmError('%s expects %d source operands, got %d' % (mnem, ns, len(rest)))
        self.emit(op, ctl, ops, pred, coissue)

    def dcl(self, mods, args):
        tok1 = 0x80000000
        centroid = 0
        if mods and mods[-1] == 'centroid':
            centroid = 4 << 20
            mods = mods[:-1]
        if mods:
            u = mods[0]
            if u in ('2d', 'cube', 'volume'):
                tok1 |= {'2d': 2, 'cube': 3, 'volume': 4}[u] << 27
            else:
                m = re.match(r'^([a-z]+)(\d*)$', u)
                if not m or m.group(1) not in USAGE:
                    raise AsmError('bad dcl usage ' + u)
                tok1 |= USAGE[m.group(1)] | (int(m.group(2) or 0) << 16)
        r = self.dst(args[0], [])
        if mods and mods[0] in ('2d', 'cube', 'volume'):
            r |= 0xF << 16
        r |= centroid
        self.emit(31, 0, [tok1, r])


def assemble(text):
    a = Asm()
    started = False
    for raw in text.split('\n'):
        ln = re.split(r'//|;', raw)[0].strip()
        if not ln:
            continue
        if not started:
            m = re.match(r'^(vs|ps)_(\d)_(\w)$', ln.lower())
            if not m:
                raise AsmError('expected version line, got: ' + ln)
            a.pixel = m.group(1) == 'ps'
            a.major = int(m.group(2))
            mn = m.group(3)
            a.minor = 1 if mn in ('x', 'a', 'b') else int(mn)
            a.t.append((0xFFFF if a.pixel else 0xFFFE) << 16 | a.major << 8 | a.minor)
            started = True
            continue
        try:
            a.line(ln)
        except (AsmError, ValueError, IndexError, KeyError) as e:
            raise AsmError('%s  (in line: %s)' % (e, ln))
    a.t.append(0xFFFF)
    return a.t


def expand(text):
    """%for VAR in a b c ... %end: repeats the block with @VAR@ replaced by each word."""
    out = []
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        m = re.match(r'^%for\s+(\w+)\s+in\s+(.*)$', lines[i])
        if not m:
            out.append(lines[i])
            i += 1
            continue
        j = i + 1
        while not lines[j].startswith('%end'):
            j += 1
        body = '\n'.join(lines[i + 1:j])
        for w in m.group(2).split():
            out.append(body.replace('@%s@' % m.group(1), w))
        i = j + 1
    return '\n'.join(out)


def split_shaders(text, default_name):
    text = expand(text)
    out = []
    cur = None
    for ln in text.split('\n'):
        m = re.match(r'^###\s*(\w+)', ln)
        if m:
            cur = [m.group(1), []]
            out.append(cur)
        else:
            if cur is None:
                cur = [default_name, []]
                out.append(cur)
            cur[1].append(ln)
    res = []
    for n, l in out:
        if any(re.split(r'//|;', x)[0].strip() for x in l):
            res.append((n, '\n'.join(l)))
    return res


def main(argv):
    if len(argv) >= 3 and argv[1] == '-d':
        for n, src in split_shaders(open(argv[2]).read(), 'shader'):
            print(n, ' '.join('%08x' % t for t in assemble(src)))
        return 0
    if len(argv) < 3:
        print(__doc__)
        return 2
    out = ['// Generated by d3dasm.py from tests/unit/shaders/*.asm - do not edit.', '#include <stdint.h>', '']
    names = []
    for f in argv[2:]:
        for n, src in split_shaders(open(f).read(), os.path.splitext(os.path.basename(f))[0]):
            try:
                toks = assemble(src)
            except AsmError as e:
                print('%s: shader %s: %s' % (f, n, e), file=sys.stderr)
                return 1
            if n in names:
                print('duplicate shader name ' + n, file=sys.stderr)
                return 1
            names.append(n)
            out.append('static const uint32_t SH_%s[] = {' % n)
            for i in range(0, len(toks), 8):
                out.append('    ' + ', '.join('0x%08xu' % t for t in toks[i:i + 8]) + ',')
            out.append('};')
    out.append('')
    out.append('typedef struct { const char *name; const uint32_t *t; unsigned n; } D3dshTab;')
    out.append('static const D3dshTab SH_TABLE[] = {')
    for n in names:
        out.append('    { "%s", SH_%s, (unsigned)(sizeof SH_%s / 4) },' % (n, n, n))
    out.append('};')
    out.append('#define SH_TABLE_N %d' % len(names))
    out.append('')
    open(argv[1], 'w').write('\n'.join(out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
