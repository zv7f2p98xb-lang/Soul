#!/usr/bin/env python3
# Checks a recording made with soulrun --audio: lists the test tones heard (Goertzel over 50 ms windows).
# Usage: audio_check.py FILE.wav [--tones 440,660]   (with --tones: only those tones are looked for and an
# extra line tells whether all of them were heard)
import math, struct, sys, wave

TONES = [330, 440, 550, 660, 880, 990]
want = None
args = sys.argv[1:]
if '--tones' in args:
    i = args.index('--tones')
    want = [int(x) for x in args[i + 1].split(',')]
    TONES = want
    del args[i:i + 2]
w = wave.open(args[0], 'rb')
rate, n = w.getframerate(), w.getnframes()
raw = w.readframes(n)
left = struct.unpack('<%dh' % (len(raw) // 2), raw)[0::2]
win = rate // 20
heard = set()
loud = 0
for start in range(0, len(left) - win, win):
    seg = left[start:start + win]
    energy = sum(x * x for x in seg) / win
    if energy < 1e4:
        continue
    loud += 1
    for f in TONES:
        k = 2 * math.cos(2 * math.pi * f / rate)
        s1 = s2 = 0.0
        for x in seg:
            s1, s2 = x + k * s1 - s2, s1
        power = (s1 * s1 + s2 * s2 - k * s1 * s2) / (win * win / 4)
        if power > energy * 0.2:
            heard.add(f)
print('heard:', ' '.join(str(f) for f in sorted(heard)))
print('sound windows>=20:', loud >= 20)
if want is not None:
    print('expected tones present:', all(f in heard for f in want))
