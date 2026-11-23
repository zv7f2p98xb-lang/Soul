#!/usr/bin/env python3
"""Compares the output of a test program rendered by a GPU backend with the recorded CPU expectation.

    compare.py [--tol N] EXPECTED ACTUAL      exit 0 = equal within tolerance, 1 = differences
    compare.py --selftest

Every line must match exactly except for colours (6 hex digits such as the "got=204080 want=204182" of
d3d9.c's pixel checks): they may differ by N (default 2) in each of the R, G, B channels. A pixel check
that the program itself judged FAIL ("name: FAIL got=RRGGBB want=RRGGBB") is accepted where the recorded
line says "name: ok" when got and want are within the tolerance; the "failures: K" line then has to equal
the number of such accepted lines. "ok"-style lines (and everything else without colours) must match
exactly, so a real failure ("name: FAIL") is never hidden.
"""
import re
import sys

COLOUR = re.compile(r'(?<![0-9A-Za-z])[0-9a-fA-F]{6}(?![0-9A-Za-z])')
SOFT_FAIL = re.compile(r'^(?P<name>.*): FAIL got=(?P<got>[0-9a-fA-F]{6}) want=(?P<want>[0-9a-fA-F]{6})$')
FAILURES = re.compile(r'^failures: (\d+)$')


def near(a, b, tol):
    a, b = int(a, 16), int(b, 16)
    return all(abs(((a >> s) & 255) - ((b >> s) & 255)) <= tol for s in (0, 8, 16))


def compare(expected, actual, tol):
    """returns (list of problems, number of lines accepted only thanks to the tolerance)"""
    exp = expected.rstrip('\n').split('\n') if expected.strip('\n') else []
    act = actual.rstrip('\n').split('\n') if actual.strip('\n') else []
    problems, tolerated = [], 0
    reported_failures = None   # (line number, expected text, actual count)
    for i in range(max(len(exp), len(act))):
        e = exp[i] if i < len(exp) else None
        a = act[i] if i < len(act) else None
        if e is None or a is None:
            problems.append('line %d: %s' % (i + 1, 'unexpected extra output: %r' % a if e is None else 'missing, expected %r' % e))
            continue
        mf = FAILURES.match(a)
        if mf and FAILURES.match(e):
            reported_failures = (i + 1, e, int(mf.group(1)))
            continue
        if e == a:
            continue
        m = SOFT_FAIL.match(a)
        if m and e == m.group('name') + ': ok':
            if near(m.group('got'), m.group('want'), tol):
                tolerated += 1
                continue
            problems.append('line %d: colour differs by more than %d: %r' % (i + 1, tol, a))
            continue
        ce, ca = COLOUR.findall(e), COLOUR.findall(a)
        if ce and len(ce) == len(ca) and COLOUR.sub('#', e) == COLOUR.sub('#', a):
            if all(near(x, y, tol) for x, y in zip(ce, ca)):
                tolerated += 1
                continue
            problems.append('line %d: colour differs by more than %d: expected %r, got %r' % (i + 1, tol, e, a))
            continue
        problems.append('line %d: expected %r, got %r' % (i + 1, e, a))
    if reported_failures:
        n, e, count = reported_failures
        want = int(FAILURES.match(e).group(1)) + tolerated
        if count != want:
            problems.append('line %d: %r but %d pixel line(s) were within tolerance (expected failures: %d)' % (n, 'failures: %d' % count, tolerated, want))
    return problems, tolerated


def selftest():
    exp = 'device created\nclear color: ok\ntex linear mid: ok\nblend: ok\nfailures: 0\n'
    cases = [
        ('identical', exp, exp, 2, True),
        ('off by 2 in each channel', exp, exp.replace('tex linear mid: ok', 'tex linear mid: FAIL got=204082 want=1f4080').replace('failures: 0', 'failures: 1'), 2, True),
        ('off by 3', exp, exp.replace('tex linear mid: ok', 'tex linear mid: FAIL got=204083 want=204080').replace('failures: 0', 'failures: 1'), 2, False),
        ('same with tolerance 3', exp, exp.replace('tex linear mid: ok', 'tex linear mid: FAIL got=204083 want=204080').replace('failures: 0', 'failures: 1'), 3, True),
        ('boolean failure is not tolerated', exp, exp.replace('blend: ok', 'blend: FAIL').replace('failures: 0', 'failures: 1'), 2, False),
        ('failure count must account for tolerated lines', exp, exp.replace('tex linear mid: ok', 'tex linear mid: FAIL got=204081 want=204080'), 2, False),
        ('hidden failure count', exp, exp.replace('tex linear mid: ok', 'tex linear mid: FAIL got=204081 want=204080').replace('failures: 0', 'failures: 3'), 2, False),
        ('missing line', exp, exp.replace('blend: ok\n', ''), 2, False),
        ('extra line', exp, exp + 'extra\n', 2, False),
        ('other text must match', exp, exp.replace('device created', 'device lost'), 2, False),
        ('different name with a soft fail', exp, exp.replace('tex linear mid: ok', 'tex linear other: FAIL got=204081 want=204080').replace('failures: 0', 'failures: 1'), 2, False),
        ('colours inside both lines', 'px: 204080 rest\n', 'px: 214282 rest\n', 2, True),
        ('colours inside both lines, too far', 'px: 204080 rest\n', 'px: 244080 rest\n', 2, False),
        ('colours only, text differs', 'px: 204080 rest\n', 'qx: 204080 rest\n', 2, False),
        ('wrong digits in non-colour text', 'count=1234567\n', 'count=1234568\n', 2, False),
    ]
    bad = 0
    for name, e, a, tol, want_ok in cases:
        problems, _ = compare(e, a, tol)
        ok = not problems
        print('%s %s' % ('ok  ' if ok == want_ok else 'FAIL', name))
        bad += ok != want_ok
    print('selftest: %s' % ('FAILED' if bad else 'passed'))
    return 1 if bad else 0


def main(argv):
    tol = 2
    args = argv[1:]
    if args[:1] == ['--selftest']:
        return selftest()
    if args[:1] == ['--tol'] and len(args) >= 2:
        tol = int(args[1])
        args = args[2:]
    if len(args) != 2:
        sys.stderr.write(__doc__)
        return 2
    with open(args[0], encoding='utf-8', errors='replace') as f:
        expected = f.read()
    with open(args[1], encoding='utf-8', errors='replace') as f:
        actual = f.read()
    problems, tolerated = compare(expected, actual, tol)
    for p in problems[:40]:
        print('DIFF ' + p)
    if len(problems) > 40:
        print('... %d more' % (len(problems) - 40))
    print('compare: %s (%d line(s) accepted within +-%d)' % ('DIFFERENT' if problems else 'equal', tolerated, tol))
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
