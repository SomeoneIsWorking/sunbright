#!/usr/bin/env python3
"""Inject a .gci save into a GC memory card .raw image (replace same-game entry).

Usage: gci_import.py <card.raw> <save.gci>

Replaces the existing directory entry with the same gamecode (keeping its block
chain when the block count matches — the simple, BAT-preserving case) and writes
the GCI's data blocks over the chain. Updates both directory copies and their
checksums. A .bak copy of the card is written first.
"""
import sys, shutil, struct

BLOCK = 0x2000
DIR_OFFS = (0x2000, 0x4000)

def csum(data: bytes):
    s = inv = 0
    for i in range(0, len(data), 2):
        v = struct.unpack_from(">H", data, i)[0]
        s = (s + v) & 0xFFFF
        inv = (inv + (~v & 0xFFFF)) & 0xFFFF
    if s == 0xFFFF: s = 0
    if inv == 0xFFFF: inv = 0
    return s, inv

def fix_dir_checksum(card: bytearray, off: int):
    s, inv = csum(card[off:off + 0x1FFC])
    struct.pack_into(">HH", card, off + 0x1FFC, s, inv)

def main():
    raw_path, gci_path = sys.argv[1], sys.argv[2]
    card = bytearray(open(raw_path, "rb").read())
    gci = open(gci_path, "rb").read()
    entry, data = gci[:0x40], gci[0x40:]
    assert len(data) % BLOCK == 0, "gci data not block-aligned"
    gci_blocks = len(data) // BLOCK
    gamecode = entry[:4]

    shutil.copyfile(raw_path, raw_path + ".bak")

    # find the existing entry in the first directory copy
    found = None
    for i in range(127):
        off = DIR_OFFS[0] + i * 0x40
        if card[off:off + 4] == gamecode:
            found = (i, off)
            break
    assert found, "no existing entry for %s — free-block allocation not implemented" % gamecode
    i, off = found
    first_block, block_count = struct.unpack_from(">HH", card, off + 0x36)
    assert block_count == gci_blocks, \
        f"block count mismatch (card {block_count}, gci {gci_blocks}) — need BAT rewrite"

    # splice the GCI dir entry, preserving the existing chain start
    new_entry = bytearray(entry)
    struct.pack_into(">HH", new_entry, 0x36, first_block, block_count)
    for doff in DIR_OFFS:
        eoff = doff + i * 0x40
        assert card[eoff:eoff + 4] in (gamecode, b"\xff\xff\xff\xff"), "dir copies disagree"
        card[eoff:eoff + 0x40] = new_entry
        fix_dir_checksum(card, doff)

    # walk the BAT chain writing data blocks (chain should be linear here, but follow it)
    bat = 0x6000
    blk = first_block
    for n in range(block_count):
        card[blk * BLOCK: blk * BLOCK + BLOCK] = data[n * BLOCK:(n + 1) * BLOCK]
        nxt = struct.unpack_from(">H", card, bat + 0xA + (blk - 5) * 2)[0]
        if n < block_count - 1:
            assert 5 <= nxt < 0xFFFF, f"broken BAT chain at block {blk}"
            blk = nxt

    open(raw_path, "wb").write(card)
    print(f"imported {gci_path} -> {raw_path}: entry #{i}, blocks {first_block}..(+{block_count})")

if __name__ == "__main__":
    main()
