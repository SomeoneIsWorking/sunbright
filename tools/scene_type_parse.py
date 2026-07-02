#!/usr/bin/env python3
"""scene_type_parse.py — dump (path/name, type_str) for every JDrama scene object
in a stage's scene.bin.

The scene loader (JDRNameRef.cpp / JDRNameRefPtrList.hpp) parses each object as:

    [u32 size][u16 type_len][type_str][u16 key][u16 name_len][name][subclass data]

For TNameRefPtrListT-derived types (TPerformList, TIdxGroupObj, TViewObjPtrListT,
… — the container classes) the subclass data starts with [s32 count][{child}×count],
each child being another object block. The `size` prefix is the total object bytes
including itself, so we can always skip cleanly to the next sibling.

For leaf types the subclass data is class-specific; we don't decode it — the size
prefix lets us skip. We heuristically treat an object as a container when the first
4 bytes after (type, name) parse as a plausible count AND the children exactly
consume the remaining block size.

Extraction pipeline: ROM (RVZ or ISO) → sunbright-jingle --extract `/data/scene/…`
→ Yaz0 → RARC → scene.bin.

Usage:
    tools/scene_type_parse.py <arc.szs>
    tools/scene_type_parse.py <rom.rvz> --stage option    # auto-extract option.szs

Fresh scene.bin extraction (once, in scratch/):
    ./build/sunbright-jingle "$SUNBRIGHT_ROM" --extract /data/scene/option.szs scratch/arc
    tools/scene_type_parse.py scratch/arc/data/scene/option.szs
"""
from __future__ import annotations
import io
import os
import struct
import subprocess
import sys
from pathlib import Path

# Reuse Yaz0 + RARC from the jingle tool.
sys.path.insert(0, str(Path(__file__).parent / "jingle"))
from jingle import yaz0, rarc_files  # type: ignore


def be32(b: bytes, o: int) -> int:
    return struct.unpack_from(">I", b, o)[0]


def be16(b: bytes, o: int) -> int:
    return struct.unpack_from(">H", b, o)[0]


def bes32(b: bytes, o: int) -> int:
    return struct.unpack_from(">i", b, o)[0]


class SceneParseError(RuntimeError):
    pass


def _read_str16(buf: bytes, off: int) -> tuple[str, int]:
    """[u16 len][len bytes] (no null term). Returns (str, bytes-consumed)."""
    if off + 2 > len(buf):
        raise SceneParseError(f"str16: EOF at {off}")
    n = be16(buf, off)
    if off + 2 + n > len(buf):
        raise SceneParseError(f"str16: len {n} overflows at {off} (buf {len(buf)})")
    try:
        s = buf[off + 2:off + 2 + n].decode("shift-jis", errors="replace")
    except UnicodeDecodeError:
        s = buf[off + 2:off + 2 + n].decode("latin-1", errors="replace")
    return s, 2 + n


def _walk_object(buf: bytes, block_end: int, cursor: int, path: str, out: list) -> int:
    """Parse ONE object starting at `cursor` (which points at the u32 size prefix).
    Returns the cursor position AFTER this object (== cursor + size).
    Appends (path/name, type_str, is_container) rows to `out`.
    """
    if cursor + 4 > block_end:
        raise SceneParseError(f"obj size: EOF at {cursor}")
    size = be32(buf, cursor)
    if size < 4 or cursor + size > block_end:
        raise SceneParseError(
            f"obj size {size} at {cursor} overflows parent end {block_end}")
    obj_end = cursor + size

    # After the u32 size, getType() does readU16() + readString() — the u16 is
    # discarded (decomp: `u32 len = param_2.readU16();` is never used, only
    # readString()'s own u16-length prefix determines the type string). Verified
    # empirically: every object we see starts [u32 size][u16 pad=0x41B8][u16
    # type_len][type_str][u16 key][u16 name_len][name][subclass data].
    p = cursor + 4
    if p + 2 > obj_end:
        raise SceneParseError(f"obj at {cursor}: no pad-u16 room")
    p += 2   # u16 discarded by getType()
    type_str, n = _read_str16(buf, p); p += n
    if p + 2 > obj_end:
        raise SceneParseError(f"obj at {cursor}: no keyCode room")
    p += 2   # u16 keyCode
    name_str, n = _read_str16(buf, p); p += n

    is_container = False

    # Heuristic: try to read u32 count next. If it parses to a plausible non-negative
    # count AND each successive child fits exactly, treat as container.
    saved_p = p
    if p + 4 <= obj_end:
        count = bes32(buf, p)
        if 0 <= count < 20000:
            trial_p = p + 4
            children_ok = True
            trial_rows: list = []
            for _ in range(count):
                if trial_p + 4 > obj_end:
                    children_ok = False; break
                csize = be32(buf, trial_p)
                if csize < 4 or trial_p + csize > obj_end:
                    children_ok = False; break
                # Optimistically walk the child (recursively) so we surface nested
                # names — if the walk fails, back off to leaf treatment.
                try:
                    child_path = f"{path}/{name_str}" if name_str else path
                    trial_p = _walk_object(buf, obj_end, trial_p, child_path, trial_rows)
                except SceneParseError:
                    children_ok = False; break
            # For a valid container, children must consume the block exactly.
            if children_ok and trial_p == obj_end:
                is_container = True
                out.append((f"{path}/{name_str}" if name_str else name_str, type_str, True))
                out.extend(trial_rows)
                return obj_end
        # Fall through to leaf if trial failed.
        p = saved_p

    # Leaf: emit + skip to end.
    out.append((f"{path}/{name_str}" if name_str else name_str, type_str, False))
    return obj_end


def parse_scene_bin(scene_bin: bytes) -> list[tuple[str, str, bool]]:
    """Parse a scene.bin blob. Returns list of (path/name, type_str, is_container)."""
    rows: list = []
    end = _walk_object(scene_bin, len(scene_bin), 0, "", rows)
    # There may be some tail padding after the root object.
    return rows


def load_scene_bin_from_arc(arc_path: Path) -> bytes:
    """Read arc_path (may be Yaz0'd or already-decompressed RARC), return scene.bin bytes."""
    raw = arc_path.read_bytes()
    if raw[:4] == b"Yaz0":
        raw = yaz0(raw)
    if raw[:4] != b"RARC":
        raise SystemExit(f"{arc_path}: not RARC (magic {raw[:4]!r})")
    files = rarc_files(raw)
    # Case-insensitive key match — option.arc uses "scene.bin" but the RARC path
    # depends on the arc.
    for k, v in files.items():
        if k.lower().endswith("scene.bin"):
            return v
    raise SystemExit(f"{arc_path}: no scene.bin inside RARC. Files: {sorted(files)}")


def extract_from_rom(rom: Path, arc_relpath: str, outdir: Path) -> Path:
    """Run sunbright-jingle --extract to pull one archive out of the ROM."""
    jingle = Path(__file__).resolve().parent.parent / "build" / "sunbright-jingle"
    if not jingle.exists():
        raise SystemExit(f"missing {jingle} — run `cmake --build build --target sunbright-jingle`")
    outdir.mkdir(parents=True, exist_ok=True)
    subprocess.check_call([str(jingle), str(rom), "--extract", arc_relpath, str(outdir)])
    return outdir / arc_relpath.lstrip("/")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__); return 2
    target = Path(sys.argv[1])
    stage = None
    args = sys.argv[2:]
    if "--stage" in args:
        i = args.index("--stage"); stage = args[i + 1]

    if stage:
        outdir = Path("scratch/arc")
        arc = extract_from_rom(target, f"/data/scene/{stage}.szs", outdir)
    else:
        arc = target

    # Accept either a Yaz0/RARC arc (extract scene.bin from inside) or a raw
    # scene.bin blob directly (e.g. tables.bin, extracted directly).
    raw = arc.read_bytes()
    if raw[:4] in (b"Yaz0", b"RARC"):
        scene_bin = load_scene_bin_from_arc(arc)
        print(f"# scene.bin from {arc} ({len(scene_bin)} bytes)")
    else:
        scene_bin = raw
        print(f"# raw scene.bin at {arc} ({len(scene_bin)} bytes)")
    rows = parse_scene_bin(scene_bin)
    n_container = sum(1 for _, _, c in rows if c)
    print(f"# {len(rows)} objects ({n_container} containers)")
    print(f"# {'container':<9}  {'type':<38}  path/name")
    print(f"# {'-'*9}  {'-'*38}  {'-'*40}")
    for path, typ, is_container in rows:
        c = "[C]" if is_container else "   "
        print(f"  {c:<9}  {typ:<38}  {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
