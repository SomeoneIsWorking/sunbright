#!/usr/bin/env python3
"""read_vanilla_camera.py — read the LIVE camera (gpCamera) from a running VANILLA Dolphin-GX
sunbright instance via the probe server, as ground truth for the sms-boot native camera.

Launch vanilla first (it renders the real guest GX; the probe reads guest RAM):
    SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_NGX_SHAPE=1 SUNBRIGHT_NGX_PRESENT=0 SUNBRIGHT_WIDESCREEN=0 \
      SUNBRIGHT_PROBE=1 ./run.sh scratch/disc/sms.iso &
    # wait for the plaza scene (dolpic*.szs in the log), then:
    tools/render/read_vanilla_camera.py

How it finds gpCamera (no US data-symbol map needed): gpCamera = *(r13 - 0x7118) — RE'd from
TSky::perform (0x8019ffe4: `lwz r3,-0x7118(r13); addi r31,r3,0x1ec`). r13 (SDA base) = the current
OSThread's saved r13 = OSThread+0x34; current OSThread = *(0x800000E4). Camera fields (Camera.hpp):
eye=unk124 (+0x124), target=unk148 (+0x148), view mtx=unk1EC (+0x1EC), from
C_MTXLookAt(unk1EC, unk124, mUp, unk148).
"""
import struct, sys, urllib.request

PORT = sys.argv[1] if len(sys.argv) > 1 else "17654"
BASE = f"http://127.0.0.1:{PORT}"

def rd_words(addr, n):
    """Read n guest words at addr. Tolerates the probe's multi-line 'ADDR: w0 w1 ...' format."""
    txt = urllib.request.urlopen(f"{BASE}/r?a={addr:x}&n={n}", timeout=5).read().decode()
    words = []
    for tok in txt.replace(":", " ").split():
        try:
            v = int(tok, 16)
        except ValueError:
            continue
        # skip the address tokens (they equal addr, addr+4, ... and are >0x80000000 too) — the
        # probe prints "<addr>: <w0> <w1>..."; drop the leading addr of each line by tracking.
        words.append(v)
    # every line is "addr w...": addresses are the running addr, addr+0x10, etc. Filter them out by
    # removing any value that exactly equals one of the expected line addresses.
    line_addrs = {addr + 0x10 * i for i in range((n + 3) // 4 + 1)}
    return [w for w in words if w not in line_addrs][:n]

def f32(words):
    return [struct.unpack(">f", struct.pack(">I", w))[0] for w in words]

def main():
    cur_thread = rd_words(0x800000E4, 1)[0]
    r13 = rd_words(cur_thread + 0x34, 1)[0]
    gpcam_addr = r13 - 0x7118
    gpcam = rd_words(gpcam_addr, 1)[0]
    print(f"current OSThread = 0x{cur_thread:08x}")
    print(f"r13 (SDA base)   = 0x{r13:08x}")
    print(f"gpCamera ptr @ 0x{gpcam_addr:08x} = 0x{gpcam:08x}")
    if not (0x80000000 <= gpcam < 0x81800000):
        print("  !! gpCamera doesn't look like a valid object — is the plaza loaded?")
        return 1
    vtable = rd_words(gpcam, 1)[0]
    eye = f32(rd_words(gpcam + 0x124, 3))
    target = f32(rd_words(gpcam + 0x148, 3))
    print(f"vtable           = 0x{vtable:08x}")
    print(f"VANILLA eye    (unk124) = ({eye[0]:.1f}, {eye[1]:.1f}, {eye[2]:.1f})")
    print(f"VANILLA target (unk148) = ({target[0]:.1f}, {target[1]:.1f}, {target[2]:.1f})")
    look = [target[i] - eye[i] for i in range(3)]
    import math
    horiz = math.hypot(look[0], look[2])
    pitch = math.degrees(math.atan2(-look[1], horiz))
    print(f"  look=({look[0]:.0f},{look[1]:.0f},{look[2]:.0f})  pitch={pitch:.1f}deg (down+)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
