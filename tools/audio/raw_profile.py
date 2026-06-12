#!/usr/bin/env python3
# Per-second RMS + zcr + smoothness profile of a raw s16 interleaved-stereo dump
# (default 32028 Hz), e.g. scratch/wav/njas_solo.raw from SUNBRIGHT_DUMP_NJAS=1.
# Companion to wav_rms.py. Columns:
#   rms — loudness (int16 scale)
#   zcr — zero crossings/s (pitch-class fingerprint; compare vs reference/oracle)
#   d2e — adjacent-sample delta^2 / energy: real audio <~0.05, byteswapped noise >~0.25
#   dc  — mean sample value (constant nonzero = frozen voice feed)
import sys, array

d = array.array('h'); d.frombytes(open(sys.argv[1], 'rb').read())
mono = d[::2]; rate = int(sys.argv[2]) if len(sys.argv) > 2 else 32028
for s in range(0, len(mono), rate):
    w = mono[s:s + rate]
    if not w: break
    n = len(w)
    energy = sum(x * x for x in w)
    rms = (energy / n) ** 0.5
    zc = sum(1 for i in range(1, n) if (w[i - 1] < 0) != (w[i] < 0))
    d2 = sum((w[i] - w[i - 1]) ** 2 for i in range(1, n))
    d2e = d2 / energy if energy else 0.0
    dc = sum(w) / n
    print(f"{s // rate:4d}s rms={rms:7.1f} zcr={zc * rate / n:6.0f} d2e={d2e:5.2f} dc={dc:7.1f}")
