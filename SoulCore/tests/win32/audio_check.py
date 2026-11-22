#!/usr/bin/env python3
# Checks a recording made with soulrun --audio: lists the test tones heard (Goertzel over 50 ms windows).
import math, struct, sys, wave

TONES = [330, 440, 550, 660, 880, 990]
w = wave.open(sys.argv[1], 'rb')
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
