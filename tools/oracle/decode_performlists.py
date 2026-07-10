#!/usr/bin/env python3
"""Decode /data/PerformLists.bin (the GC disc's perform-list data file) per the exact
TNameRef::genObject + TPerformList::load wire format used by
reference/sms/src/JSystem/JDrama/JDRNameRef.cpp, JDRViewObjPtrList.hpp, System/PerformList.cpp.

This is ground truth for "what order does the disc specify" questions -- e.g. whether
mPerformListGX's mirror-camera/mirror-scene entries precede its world-camera/DrawBuf-draw
entries (they do: idx1 "鏡カメラ" vs idx23 "camera 1"/idx33+ "DrawBuf Sky Opa" etc). Note
"Draw Buffer Group" itself is NOT an entry in any PerformLists.bin list -- it's a
scene.bin-loaded TViewObjPtrListT container, manually push_back'd into TMarDirector::unk40
at MarDirectorSetupObjects.cpp:427 (code, not data); see
debug_journal/2026-07-10_performlists_disc_decode.md.

Usage: extract /data/PerformLists.bin from an ISO first (uncompressed, found via the FST --
see tools/oracle/ siblings for FST-walk helpers), then run this script against the extracted
file. Refuses empty/short/malformed input loudly (FAIL FAST, no silent empty dump).

Format (all big-endian on disc):
  record := u32 totalLen                  # = 4 + len(payload)
            payload[totalLen-4]           # exactly this many bytes
  payload (as consumed by TNameRef::genObject + obj->load()):
            u16 typeKeyCode  u16 typeLen  char typeName[typeLen]      -- getType()
            u16 objKeyCode   u16 nameLen  char objName[nameLen]       -- TNameRef::load() (via loadSuper)
            <type-specific body, consuming the rest of payload>

  GroupObj (TViewObjPtrListT<TViewObj,TViewObj>) body: s32 count, then `count` nested records
            (each shaped exactly like the outer record: u32 len + payload).

  PerformList (TPerformList) body: repeat until payload exhausted:
            u16 entryNameLen  char entryName[entryNameLen]   (readString, no name-length cap here
                                                                since 80-byte dest buf just truncates
                                                                on overflow -- not relevant, GC names
                                                                are short)
            u32 filter   (raw BE dword, ANDed with the perform() call flag at dispatch)
"""
import argparse
import struct
import sys


class Reader:
    def __init__(self, data, pos=0, end=None):
        self.data = data
        self.pos = pos
        self.end = end if end is not None else len(data)

    def remaining(self):
        return self.end - self.pos

    def u16(self):
        v = struct.unpack_from(">H", self.data, self.pos)[0]
        self.pos += 2
        return v

    def u32(self):
        v = struct.unpack_from(">I", self.data, self.pos)[0]
        self.pos += 4
        return v

    def s32(self):
        v = struct.unpack_from(">i", self.data, self.pos)[0]
        self.pos += 4
        return v

    def bstr(self, n):
        s = self.data[self.pos:self.pos + n]
        self.pos += n
        return s.decode("shift_jis", errors="replace")

    def pstr(self):
        # readString(): u16 BE length prefix + raw bytes
        n = self.u16()
        return self.bstr(n)


def read_record(r: Reader):
    """Consume one `u32 len` + payload record starting at r.pos; return
    (type_name, obj_name, body_reader) where body_reader is scoped to the
    record's payload (post type+name), consumable by the type-specific loader."""
    total_len = r.u32()
    payload_start = r.pos
    payload_end = payload_start + (total_len - 4)
    body = Reader(r.data, payload_start, payload_end)
    _type_key = body.u16()
    type_name = body.pstr()
    _obj_key = body.u16()
    obj_name = body.pstr()
    r.pos = payload_end  # advance outer reader past this whole record
    return type_name, obj_name, body


def read_performlist_entries(body: Reader):
    entries = []
    while body.remaining() > 0:
        name = body.pstr()
        filt_raw = body.u32()
        entries.append((name, filt_raw))
    return entries


def read_group(body: Reader):
    count = body.s32()
    children = []
    for _ in range(count):
        type_name, obj_name, child_body = read_record(body)
        children.append((type_name, obj_name, child_body))
    return children


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", default="scratch/oracle/PerformLists.bin",
                     help="extracted /data/PerformLists.bin (default: scratch/oracle/PerformLists.bin)")
    args = ap.parse_args()

    with open(args.path, "rb") as f:
        data = f.read()
    if len(data) < 8:
        sys.exit(f"FATAL: {args.path} is {len(data)} bytes -- too short to be a real "
                  f"PerformLists.bin (expect a u32 length header >= file size); refusing "
                  f"to decode degenerate/empty input")

    r = Reader(data)
    declared_len = struct.unpack_from(">I", data, 0)[0]
    if declared_len != len(data):
        sys.exit(f"FATAL: top-level record declares length {declared_len} but file is "
                  f"{len(data)} bytes -- malformed/truncated input, refusing to decode")

    type_name, obj_name, body = read_record(r)
    print(f"# top-level record: type={type_name!r} name={obj_name!r} bytes={len(data)}")
    assert r.pos == len(data), f"trailing bytes after top record: {r.pos} vs {len(data)}"

    if type_name != "GroupObj":
        print(f"UNEXPECTED top-level type {type_name!r}", file=sys.stderr)
        sys.exit(1)

    children = read_group(body)
    print(f"# {len(children)} child perform-lists")
    assert body.remaining() == 0, f"trailing bytes in GroupObj body: {body.remaining()}"

    all_lists = {}
    for type_name, obj_name, child_body in children:
        print(f"\n== list type={type_name!r} name={obj_name!r} ==")
        if type_name != "PerformList":
            print(f"  (not a PerformList body -- skipping entry decode)")
            continue
        entries = read_performlist_entries(child_body)
        all_lists[obj_name] = entries
        for i, (name, filt_raw) in enumerate(entries):
            print(f"  [{i:3}] name={name!r:40s} filter_raw=0x{filt_raw:08x}")
        print(f"  ({len(entries)} entries)")

    return all_lists


if __name__ == "__main__":
    main()
