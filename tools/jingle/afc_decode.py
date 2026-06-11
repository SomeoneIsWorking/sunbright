#!/usr/bin/env python3
"""Standalone JAudio AFC (4-bit HQ ADPCM) decoder — plays SMS wave-bank audio from ROM data
with zero emulation. Port of the DSP decode (see Dolphin ZeldaAudioRenderer::DecodeAFC):
9-byte blocks; header byte = (delta_exp << 4) | coef_index; 16 4-bit samples;
sample = (delta*nibble + yn1*c0 + yn2*c1) >> 11 with the canonical JAudio coef table.

usage: afc_decode.py <file.aw> <out.wav> [rate=32000] [offset] [length]
"""
import struct, sys, wave

COEF = [
    (0, 0), (2048, 0), (0, 2048), (1024, 1024),
    (4096, -2048), (3584, -1536), (3072, -1024), (4608, -2560),
    (4200, -2248), (4800, -2300), (5120, -3072), (2048, -2048),
    (1024, -1024), (-1024, 1024), (-1024, 0), (-2048, 0),
]

def clamp16(v):
    return -32768 if v < -32768 else (32767 if v > 32767 else v)

def decode(data):
    out = []
    yn1 = yn2 = 0
    for b in range(len(data) // 9):
        blk = data[b * 9:b * 9 + 9]
        delta = 1 << (blk[0] >> 4)
        c0, c1 = COEF[blk[0] & 0xF]
        for i in range(16):
            nib = (blk[1 + i // 2] >> (4 if i % 2 == 0 else 0)) & 0xF
            if nib >= 8:
                nib -= 16
            nib = (nib << 12) >> 1                      # sign-extended 4-bit → s16 <<11
            sample = (delta * nib + yn1 * c0 + yn2 * c1) >> 11
            sample = clamp16(sample)
            yn2, yn1 = yn1, sample
            out.append(sample)
    return out

def main():
    src, dst = sys.argv[1], sys.argv[2]
    rate = int(sys.argv[3]) if len(sys.argv) > 3 else 32000
    off = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0
    ln = int(sys.argv[5], 0) if len(sys.argv) > 5 else None
    data = open(src, "rb").read()
    data = data[off:off + ln] if ln else data[off:]
    samples = decode(data)
    with wave.open(dst, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    print(f"{dst}: {len(samples)} samples ({len(samples)/rate:.2f}s @ {rate} Hz)")

if __name__ == "__main__":
    main()
