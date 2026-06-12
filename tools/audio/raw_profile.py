#!/usr/bin/env python3
# Per-second RMS + zcr profile of a raw s16 interleaved-stereo dump (default 32028 Hz),
# e.g. scratch/wav/njas_solo.raw from SUNBRIGHT_DUMP_NJAS=1. Companion to wav_rms.py.
import sys, array
# per-second rms + zcr of raw s16 interleaved-stereo @32028
d = array.array('h'); d.frombytes(open(sys.argv[1],'rb').read())
mono = d[::2]; rate = 32028
for s in range(0, len(mono), rate):
    w = mono[s:s+rate]
    if not w: break
    rms = (sum(x*x for x in w)/len(w))**0.5
    zc = sum(1 for i in range(1,len(w)) if (w[i-1]<0)!=(w[i]<0))
    print(f"{s//rate:4d}s rms={rms:7.1f} zcr={zc*rate/len(w):6.0f}")
