#!/usr/bin/env python3
"""Decode a dumped /scene/map/scene.bin per TNameRef::genObject wire format
(same format as tools/oracle/decode_performlists.py; see that file's header
comment for the exact byte layout). Generalizes the container-detection to
ANY TNameRef-derived list type (TViewObjPtrListT/TNameRefPtrListT), not just
GroupObj/PerformList, since scene.bin nests arbitrary declared type strings
(e.g. "Draw Buffer Group" -> DrawBufObj / MirrorMapDrawBuf children).

Container test: after [type][name], try reading `s32 count`; if 0<=count<20000
and `count` nested [u32 len]-prefixed records exactly consume the remaining
body, it's a container (TViewObjPtrListT<T,U>::load / TNameRefPtrListT::load
shape). Otherwise it's a leaf (type-specific body we don't decode further,
just skip via the outer record's length prefix).

Dump the scene.bin blob first (in-game, decompressed, no Yaz0/RARC extraction
needed): `SB_SCENE_DUMP=<path> SB_STAGE=<n> SB_HEADLESS=1 ./run.sh`.

Usage:
    tools/oracle/decode_scene_bin.py <scene.bin> [--find "Draw Buffer Group"]
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
        if self.pos + 2 > self.end:
            raise ValueError(f"u16: EOF at {self.pos} (end {self.end})")
        v = struct.unpack_from(">H", self.data, self.pos)[0]
        self.pos += 2
        return v

    def u32(self):
        if self.pos + 4 > self.end:
            raise ValueError(f"u32: EOF at {self.pos} (end {self.end})")
        v = struct.unpack_from(">I", self.data, self.pos)[0]
        self.pos += 4
        return v

    def s32(self):
        if self.pos + 4 > self.end:
            raise ValueError(f"s32: EOF at {self.pos} (end {self.end})")
        v = struct.unpack_from(">i", self.data, self.pos)[0]
        self.pos += 4
        return v

    def bstr(self, n):
        if self.pos + n > self.end:
            raise ValueError(f"bstr({n}): EOF at {self.pos} (end {self.end})")
        s = self.data[self.pos:self.pos + n]
        self.pos += n
        return s.decode("shift_jis", errors="replace")

    def pstr(self):
        n = self.u16()
        return self.bstr(n)


class Node:
    __slots__ = ("type_name", "obj_name", "is_container", "children", "leaf_len", "trailer_len")

    def __init__(self, type_name, obj_name):
        self.type_name = type_name
        self.obj_name = obj_name
        self.is_container = False
        self.children = []
        self.leaf_len = 0
        self.trailer_len = 0


def read_record(r: Reader) -> Node:
    """Consume one `u32 len` + payload record at r.pos. Recurses into
    container-shaped bodies. Always advances r.pos to end of record."""
    total_len = r.u32()
    if total_len < 4:
        raise ValueError(f"record at {r.pos - 4}: bogus len {total_len}")
    payload_start = r.pos
    payload_end = payload_start + (total_len - 4)
    if payload_end > r.end:
        raise ValueError(
            f"record at {r.pos - 4}: len {total_len} overflows parent end {r.end}")
    body = Reader(r.data, payload_start, payload_end)

    _type_key = body.u16()
    type_name = body.pstr()
    _obj_key = body.u16()
    obj_name = body.pstr()

    node = Node(type_name, obj_name)

    # TSmJ3DScn ("MarScene") overrides loadSuper() to read keyCode+name (above)
    # THEN an inline TLightMap block BEFORE the [count][children] list -- see
    # JDRSmJ3DScn.cpp's loadSuper + TViewObjPtrListT::load call order, and
    # TLightMap::load (JDRLighting.cpp:167): s32 count, then count*
    # [u32 unk0][readString into a 0x50 buf (u16 len + that many raw bytes,
    # truncated on copy but the STREAM still advances by the full len)].
    # Consume that block here so the generic container test below lands on
    # the real [count][children] list, not on TLightMap's leading s32.
    if type_name == "MarScene":
        lm_count = body.s32()
        for _ in range(lm_count):
            body.u32()          # unk0
            body.pstr()         # readString(buf, 0x50) -- full string still consumed

    # Container test: s32 count, then `count` nested [u32 len]-records.
    # TViewObjPtrListT::load(GroupObj/NameRefGrp) consumes the body exactly.
    # TSmJ3DScn::loadSuper (type "MarScene") calls TViewObjPtrListT::loadSuper
    # for the same [count][children] shape and then ALSO reads a trailing
    # TLightMap blob after the child list -- so accept a nonzero remainder
    # too (recorded as trailer_len), just require the count + every child to
    # parse cleanly (a wrong container guess would fail a child's own u32-len
    # bounds check almost immediately).
    saved_pos = body.pos
    is_container = False
    trailer_len = 0
    if body.remaining() >= 4:
        try:
            count = body.s32()
        except ValueError:
            count = -1
        if 0 <= count < 20000:
            trial_children = []
            ok = True
            for _ in range(count):
                if body.remaining() < 4:
                    ok = False
                    break
                try:
                    child = read_record(body)
                except ValueError:
                    ok = False
                    break
                trial_children.append(child)
            if ok:
                remainder = body.remaining()
                # Reject the degenerate false-positive: count==0 trivially
                # "parses" for ANY leaf type with >=4 bytes left (zero
                # children to check), so require either an exact match
                # (remainder==0, the common GroupObj/NameRefGrp shape) or a
                # nonzero count (so we actually validated real child records)
                # before trusting a nonzero trailer. TSmJ3DScn ("MarScene")
                # is the one known type whose loadSuper reads [count][children]
                # then an extra TLightMap blob -- that's the only case a
                # nonzero trailer is expected.
                if remainder == 0 or count > 0:
                    is_container = True
                    node.is_container = True
                    node.children = trial_children
                    trailer_len = remainder
        if not is_container:
            body.pos = saved_pos

    if not is_container:
        node.leaf_len = body.remaining()
    node.trailer_len = trailer_len

    r.pos = payload_end
    return node


def walk(node: Node, path: str, out: list):
    full = f"{path}/{node.obj_name}" if node.obj_name else path
    out.append((full, node.type_name, node.is_container, node.leaf_len))
    for c in node.children:
        walk(c, full, out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("scene_bin")
    ap.add_argument("--find", default=None,
                     help="only print this node and its direct children")
    args = ap.parse_args()

    with open(args.scene_bin, "rb") as f:
        data = f.read()
    if len(data) < 8:
        sys.exit(f"REFUSING: {args.scene_bin} is only {len(data)} bytes -- "
                  "empty/truncated dump")

    r = Reader(data)
    root = read_record(r)
    if r.pos != len(data):
        print(f"# NOTE: {len(data) - r.pos} trailing bytes after root record",
              file=sys.stderr)

    rows = []
    walk(root, "", rows)
    print(f"# {args.scene_bin}: {len(data)} bytes, {len(rows)} nodes")

    if args.find:
        target_idx = None
        for i, (path, type_name, is_c, leaf_len) in enumerate(rows):
            if path.split("/")[-1] == args.find:
                target_idx = i
                break
        if target_idx is None:
            sys.exit(f"'{args.find}' not found among {len(rows)} nodes")
        path, type_name, is_c, leaf_len = rows[target_idx]
        print(f"MATCH [{target_idx}] path={path!r} type={type_name!r} "
              f"container={is_c} leaf_len={leaf_len}")
        # Print children: rows whose path is exactly f"{path}/{name}" one level
        # deeper, i.e. those immediately following until we hit a path that is
        # NOT prefixed by path+"/".
        prefix = path + "/"
        n = 0
        for j in range(target_idx + 1, len(rows)):
            p2 = rows[j][0]
            if not p2.startswith(prefix):
                break
            rest = p2[len(prefix):]
            if "/" in rest:
                continue  # deeper than direct child
            print(f"  [{n:3}] name={rest!r:40s} type={rows[j][1]!r:28s} "
                  f"container={rows[j][2]}")
            n += 1
        print(f"  ({n} direct children)")
        return 0

    for path, type_name, is_c, leaf_len in rows:
        tag = "CONTAINER" if is_c else f"leaf({leaf_len}B)"
        print(f"  {tag:14s} type={type_name!r:28s} path={path!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
