#!/usr/bin/env python3
"""Detect Dolphin granule-mixer LOOPING in a pulled-stream capture (raw s16 stereo 48k).

The dry-queue path jumps the playhead back ~half the queue and replays it, so the
audible artifact is a block of samples reappearing shortly after itself. For each
256-frame block, search the next 12000 frames (250 ms) for an (near-)identical block.
Reports loop events per second of stream. Silence blocks are skipped.
"""
import sys, array

raw = open(sys.argv[1], 'rb').read()
a = array.array('h')
a.frombytes(raw[:len(raw) // 4 * 4])
n = len(a) // 2  # frames
B = 256          # block frames
SCAN = 12000     # frames ahead to search
events = {}
i = 0
while i + B <= n:
    blk = a[2*i:2*(i+B)]
    if max(map(abs, blk), default=0) < 64:   # silence — looping silence is inaudible
        i += B
        continue
    # compare with candidate offsets ahead (block-aligned for speed)
    j = i + B
    end = min(i + SCAN, n - B)
    found = False
    while j + B <= end:
        if a[2*j:2*(j+B)] == blk:
            found = True
            break
        j += B
    if found:
        sec = i // 48000
        events[sec] = events.get(sec, 0) + 1
        i = j + B     # skip past the repeat
    else:
        i += B
total = sum(events.values())
print(f"frames={n} ({n/48000:.1f}s)  loop_events={total}")
for s in sorted(events):
    print(f"  t={s}s loops={events[s]}")
