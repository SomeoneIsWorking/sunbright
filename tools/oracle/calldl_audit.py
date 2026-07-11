#!/usr/bin/env python3
"""calldl_audit.py — audit GX_CMD_CALL_DL opcodes in a .dff: load-bearing or inert?

The open risk for FIFO replay: aurora logs-and-ignores nested CALL_DL. If the
2936/frame CALL_DL bodies contain real state, a replay that skips them is wrong.
This script walks frame 0 in sync (reusing the parser's decode_frame) and reports
the CALL_DL (addr,size) distribution so we can judge.
"""
import struct, sys, collections, re
sys.path.insert(0, 'tools/oracle')
from parse_fifo_dff import parse_header, FRAME_FMT, decode_frame, CPState

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'scratch/oracle/fifo/title_press_start_vi_stable.dff'
    buf = open(path, 'rb').read()
    hdr = parse_header(buf)
    print("=== %s ===" % path)
    print("frames: %d  mem1_size: %d  tex_mem_size: %d" % (hdr.frame_count, hdr.mem1_size, hdr.tex_mem_size))
    print("(DL bodies live in MEM1 main RAM; the .dff captures NO MEM1 blob — only")
    print(" register/TMEM snapshots. So DL bodies are almost certainly ABSENT.)")
    print()

    for fi in range(hdr.frame_count):
        frame_off = hdr.frame_list_offset + fi * 64
        fifo_data_off, fifo_data_size = struct.unpack_from("<QI", buf, frame_off)[:2]
        data = buf[fifo_data_off:fifo_data_off + fifo_data_size]
        cp = CPState()
        _, _, _, unknown, warnings = decode_frame(fi, data, cp)
        calls = []
        for w in warnings:
            m = re.search(r'CALL_DL addr=0x([0-9a-f]+) size=(\d+)', w)
            if m:
                calls.append((int(m.group(1), 16), int(m.group(2))))
        if not calls:
            print("frame %d: NO CALL_DL opcodes" % fi)
            continue
        sizes = [s for _, s in calls]
        uniq = collections.Counter(calls)
        print("--- frame %d: %d cmd bytes, %d CALL_DL ---" % (fi, fifo_data_size, len(calls)))
        print("  unique (addr,size): %d   (low => shared setup macros called repeatedly)" % len(uniq))
        print("  DL size: min=%d max=%d mean=%.0f total_bytes_referenced=%d" %
              (min(sizes), max(sizes), sum(sizes)/len(sizes), sum(sizes)))
        # NOP-slide detection: if most DLs are size <= 1 (just a NOP), they're inert.
        tiny = sum(1 for s in sizes if s <= 4)
        print("  DLs with size<=4 (likely NOP/inert): %d / %d (%.0f%%)" %
              (tiny, len(sizes), 100.0*tiny/len(sizes)))
        print("  size histogram (top 8): %s" % collections.Counter(sizes).most_common(8))
        print("  most-called DLs:")
        for (a, s), c in uniq.most_common(6):
            print("    addr=0x%08x size=%-6d called %-5d times" % (a, s, c))
        print()


if __name__ == '__main__':
    main()
