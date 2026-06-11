#!/usr/bin/env python3
"""Per-window RMS profile of a WAV (the ear-free audio check).

Usage: wav_rms.py file.wav [window_ms=100]
Prints one line per window: t_start_s rms (int16 scale), plus a summary:
total duration, first/last non-silent window, and mid-signal dropout windows
(rms < 50 between the first and last loud window — skips/jitter show up here).
"""
import sys, wave, array, math

path = sys.argv[1]
win_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 100
w = wave.open(path, 'rb')
rate, ch, sw = w.getframerate(), w.getnchannels(), w.getsampwidth()
assert sw == 2, "expects 16-bit PCM"
nwin = rate * win_ms // 1000
rms = []
while True:
    d = w.readframes(nwin)
    if not d:
        break
    a = array.array('h')
    a.frombytes(d[:len(d) // 2 * 2])
    rms.append(int(math.sqrt(sum(x * x for x in a) / len(a))) if len(a) else 0)
dur = w.getnframes() / rate
loud = [i for i, r in enumerate(rms) if r > 200]
print(f"# {path}: {dur:.2f}s rate={rate} ch={ch} windows={len(rms)} win={win_ms}ms")
if not loud:
    print("# ALL SILENT")
    sys.exit(0)
first, last = loud[0], loud[-1]
drop = [i for i in range(first, last + 1) if rms[i] < 50]
print(f"# signal {first*win_ms/1000:.2f}s..{(last+1)*win_ms/1000:.2f}s  "
      f"({(last-first+1)*win_ms/1000:.2f}s)  dropout_windows={len(drop)}")
if drop:
    print("# dropouts at:", " ".join(f"{i*win_ms/1000:.1f}" for i in drop[:40]))
for i, r in enumerate(rms):
    print(f"{i*win_ms/1000:6.2f} {r}")
