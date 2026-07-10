#!/usr/bin/env python3
"""tools/oracle/parse_fifo_dff.py — ground-truth parser for Dolphin .dff FIFO logs.

Standalone reimplementation of Dolphin's FifoDataFile (Core/FifoPlayer/FifoDataFile.h/.cpp)
and GX opcode decoder (VideoCommon/OpcodeDecoding.h) purely from the public Dolphin source
(struct layouts + opcode table fetched directly from
https://github.com/dolphin-emu/dolphin, Source/Core/{Core/FifoPlayer,VideoCommon}/*,
mirrored alongside this script in scratch/oracle_fifo/*.ref for provenance). This does NOT
build or link against the Dolphin submodule (which is retired for this project, see
CLAUDE.md) — it's an independent, from-scratch byte-level parser of the .dff file format.

Emits, in order, for each recorded frame: XF register loads (flagging the projection range
XFMEM_SETPROJECTION=0x1020..0x103f and viewport XFMEM_SETVIEWPORT=0x101a..0x101f) and GX
primitive/draw commands (opcodes 0x80-0xbf), which is exactly what's needed to answer:
relative to draw batches, when does retail (re)bind a perspective projection each frame, and
does it rely on carry-over from the previous frame's tail?

A correct decode requires tracking enough CP (Command Processor) state to compute each
primitive's vertex size in bytes (VCD + VAT bitfields) so the byte stream stays in sync —
implemented per VertexLoaderBase::GetVertexSize and the VertexLoader_{Position,Normal,Color,
TextCoord} size tables (also mirrored as .ref files). Actual vertex/attribute VALUES are not
decoded (not needed for this task) — only their byte lengths, to skip correctly.

Usage: parse_fifo_dff.py <file.dff> [--summary] [--frame N] [--csv OUT]
"""
import argparse
import struct
import sys
from dataclasses import dataclass, field

# ---------------------------------------------------------------------------
# FifoDataFile format (Core/FifoPlayer/FifoDataFile.cpp, #pragma pack(push, 1))
# ---------------------------------------------------------------------------

FILE_ID = 0x0D01F1F0

# u32 fileId; u32 file_version; u32 min_loader_version;
# u64 bpMemOffset; u32 bpMemSize;
# u64 cpMemOffset; u32 cpMemSize;
# u64 xfMemOffset; u32 xfMemSize;
# u64 xfRegsOffset; u32 xfRegsSize;
# u64 frameListOffset; u32 frameCount; u32 flags;
# u64 texMemOffset; u32 texMemSize;
# u32 mem1_size; u32 mem2_size;
# char gameid[8]; u8 reserved[24];
HEADER_FMT = "<3I" + "QI" * 4 + "QII" + "QI" + "II" + "8s24s"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
assert HEADER_SIZE == 128, HEADER_SIZE

# u64 fifoDataOffset; u32 fifoDataSize; u32 fifoStart; u32 fifoEnd;
# u64 memoryUpdatesOffset; u32 numMemoryUpdates; u8 reserved[32]
FRAME_FMT = "<QIIIQI32s"
FRAME_SIZE = struct.calcsize(FRAME_FMT)
assert FRAME_SIZE == 64, FRAME_SIZE

# u32 fifoPosition; u32 address; u64 dataOffset; u32 dataSize; u8 type; u8 reserved[3]
MEMUPDATE_FMT = "<IIQIB3s"
MEMUPDATE_SIZE = struct.calcsize(MEMUPDATE_FMT)
assert MEMUPDATE_SIZE == 24, MEMUPDATE_SIZE


@dataclass
class Header:
    file_id: int
    file_version: int
    min_loader_version: int
    bp_mem_offset: int
    bp_mem_size: int
    cp_mem_offset: int
    cp_mem_size: int
    xf_mem_offset: int
    xf_mem_size: int
    xf_regs_offset: int
    xf_regs_size: int
    frame_list_offset: int
    frame_count: int
    flags: int
    tex_mem_offset: int
    tex_mem_size: int
    mem1_size: int
    mem2_size: int
    gameid: str


def parse_header(buf: bytes) -> Header:
    vals = struct.unpack_from(HEADER_FMT, buf, 0)
    (file_id, file_version, min_loader_version,
     bp_mem_offset, bp_mem_size,
     cp_mem_offset, cp_mem_size,
     xf_mem_offset, xf_mem_size,
     xf_regs_offset, xf_regs_size,
     frame_list_offset, frame_count, flags,
     tex_mem_offset, tex_mem_size,
     mem1_size, mem2_size,
     gameid_raw, _reserved) = vals
    if file_id != FILE_ID:
        raise SystemExit(
            f"REFUSING: bad .dff magic 0x{file_id:08x} (expected 0x{FILE_ID:08x}) — "
            f"not a Dolphin FIFO log, or corrupt file")
    gameid = gameid_raw.split(b"\x00", 1)[0].decode("ascii", errors="replace")
    return Header(file_id, file_version, min_loader_version, bp_mem_offset, bp_mem_size,
                  cp_mem_offset, cp_mem_size, xf_mem_offset, xf_mem_size, xf_regs_offset,
                  xf_regs_size, frame_list_offset, frame_count, flags, tex_mem_offset,
                  tex_mem_size, mem1_size, mem2_size, gameid)


@dataclass
class FrameInfo:
    fifo_data_offset: int
    fifo_data_size: int
    fifo_start: int
    fifo_end: int
    memory_updates_offset: int
    num_memory_updates: int


def parse_frame_info(buf: bytes, off: int) -> FrameInfo:
    (fifo_data_offset, fifo_data_size, fifo_start, fifo_end,
     memory_updates_offset, num_memory_updates, _reserved) = struct.unpack_from(
        FRAME_FMT, buf, off)
    return FrameInfo(fifo_data_offset, fifo_data_size, fifo_start, fifo_end,
                      memory_updates_offset, num_memory_updates)


@dataclass
class MemoryUpdate:
    fifo_position: int
    address: int
    data_offset: int
    data_size: int
    type: int


def parse_memory_updates(buf: bytes, off: int, count: int):
    updates = []
    for i in range(count):
        (fifo_position, address, data_offset, data_size, type_, _reserved) = (
            struct.unpack_from(MEMUPDATE_FMT, buf, off + i * MEMUPDATE_SIZE))
        updates.append(MemoryUpdate(fifo_position, address, data_offset, data_size, type_))
    return updates


# ---------------------------------------------------------------------------
# GX opcode decoding (VideoCommon/OpcodeDecoding.h) + enough CP/VAT state to
# compute vertex sizes (VideoCommon/{CPMemory,VertexLoaderBase,
# VertexLoader_{Position,Normal,Color,TextCoord}}.h)
# ---------------------------------------------------------------------------

GX_NOP = 0x00
GX_LOAD_CP_REG = 0x08
GX_LOAD_XF_REG = 0x10
GX_LOAD_INDX_A = 0x20
GX_LOAD_INDX_B = 0x28
GX_LOAD_INDX_C = 0x30
GX_LOAD_INDX_D = 0x38
GX_CMD_CALL_DL = 0x40
GX_CMD_UNKNOWN_METRICS = 0x44
GX_CMD_INVL_VC = 0x48
GX_LOAD_BP_REG = 0x61
GX_PRIMITIVE_START = 0x80
GX_PRIMITIVE_END = 0xBF

CP_COMMAND_MASK = 0xF0
MATINDEX_A = 0x30
MATINDEX_B = 0x40
XF_POSMTX_END = 0x100  # XF pos/normal matrix memory: addr 0x000-0x0FF, matrix N at addr N*12
                        # (3 rows x 4 floats/matrix; verified empirically against this .dff's
                        # own posmtx write addresses: 0, 120, 132, 144, ... = 0*12, 10*12, 11*12,
                        # 12*12, ... i.e. exact multiples of 12 words, never 4 or 8 alone) —
                        # matches Dolphin's XFMemory.h posMatrices[] / VertexShaderManager
                        # convention (position matrix stride = 12 words == 3x4 GX matrix).
VCD_LO = 0x50
VCD_HI = 0x60
CP_VAT_REG_A = 0x70
CP_VAT_REG_B = 0x80
CP_VAT_REG_C = 0x90
ARRAY_BASE = 0xA0
CP_VAT_MASK = 0x07

# XFMemory.h register map (subset relevant to this task)
XFMEM_SETVIEWPORT = 0x101A       # 6 words: 0x101a-0x101f
XFMEM_SETPROJECTION = 0x1020     # 7 words incl. type: 0x1020-0x1026 (task's stated range 0x1020-0x103f covers this + padding)
XFMEM_SETNUMTEXGENS = 0x103F

PRIMITIVE_NAMES = {
    0x0: "GX_DRAW_QUADS", 0x1: "GX_DRAW_QUADS_2", 0x2: "GX_DRAW_TRIANGLES",
    0x3: "GX_DRAW_TRIANGLE_STRIP", 0x4: "GX_DRAW_TRIANGLE_FAN", 0x5: "GX_DRAW_LINES",
    0x6: "GX_DRAW_LINE_STRIP", 0x7: "GX_DRAW_POINTS",
}

# ComponentFormat element byte sizes (GetElementSize)
ELEM_SIZE = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 4}

# VertexLoader_Position size table: [type][format] -> (size_XY, size_XYZ)
POS_SIZE = {
    1: {0: (2, 3), 1: (2, 3), 2: (4, 6), 3: (4, 6), 4: (8, 12), 5: (8, 12), 6: (8, 12), 7: (8, 12)},
    2: {f: (1, 1) for f in range(8)},
    3: {f: (2, 2) for f in range(8)},
}

# VertexLoader_Normal size table: [type][index3][elements(N=0/NTB=1)] -> size
NORMAL_SIZE_DIRECT = {
    (False, 0): {0: 3, 1: 3, 2: 6, 3: 6, 4: 12, 5: 12, 6: 12, 7: 12},
    (False, 1): {0: 9, 1: 9, 2: 18, 3: 18, 4: 36, 5: 36, 6: 36, 7: 36},
    (True, 0): {0: 3, 1: 3, 2: 6, 3: 6, 4: 12, 5: 12, 6: 12, 7: 12},
    (True, 1): {0: 9, 1: 9, 2: 18, 3: 18, 4: 36, 5: 36, 6: 36, 7: 36},
}
NORMAL_SIZE_IDX8 = {
    (False, 0): 1, (False, 1): 1, (True, 0): 1, (True, 1): 3,
}
NORMAL_SIZE_IDX16 = {
    (False, 0): 2, (False, 1): 2, (True, 0): 2, (True, 1): 6,
}

# VertexLoader_Color size table: [type][ColorFormat 0..5] -> size
COLOR_SIZE = {
    1: [2, 3, 4, 2, 3, 4],
    2: [1, 1, 1, 1, 1, 1],
    3: [2, 2, 2, 2, 2, 2],
}

# VertexLoader_TextCoord size table: [type][format] -> (size_S, size_ST)
TEX_SIZE = {
    1: {0: (1, 2), 1: (1, 2), 2: (2, 4), 3: (2, 4), 4: (4, 8), 5: (4, 8), 6: (4, 8), 7: (4, 8)},
    2: {f: (1, 1) for f in range(8)},
    3: {f: (2, 2) for f in range(8)},
}


def bits(value: int, lo: int, width: int) -> int:
    return (value >> lo) & ((1 << width) - 1)


class VAT:
    """One of the 8 CP_VAT_REG_{A,B,C} slots (3 32-bit words: g0, g1, g2)."""

    __slots__ = ("g0", "g1", "g2")

    def __init__(self):
        self.g0 = 0
        self.g1 = 0
        self.g2 = 0

    # --- group 0 fields ---
    def pos_elements(self):
        return bits(self.g0, 0, 1)

    def pos_format(self):
        return bits(self.g0, 1, 3)

    def normal_elements(self):
        return bits(self.g0, 9, 1)

    def normal_format(self):
        return bits(self.g0, 10, 3)

    def normal_index3(self):
        return bool(bits(self.g0, 31, 1))

    def color_elements(self, idx):
        return bits(self.g0, 13, 1) if idx == 0 else bits(self.g0, 17, 1)

    def color_format(self, idx):
        return bits(self.g0, 14, 3) if idx == 0 else bits(self.g0, 18, 3)

    def tex_elements(self, idx):
        if idx == 0:
            return bits(self.g0, 21, 1)
        if idx in (1, 2, 3, 4):
            # g1: Tex1(0,1) Tex2(9,1) Tex3(18,1) Tex4(27,1)  [elements bit is 1 lsb of each 9-bit group]
            shift = {1: 0, 2: 9, 3: 18, 4: 27}[idx]
            return bits(self.g1, shift, 1)
        # g2: Tex5(5,1) Tex6(14,1) Tex7(23,1)
        shift = {5: 5, 6: 14, 7: 23}[idx]
        return bits(self.g2, shift, 1)

    def tex_format(self, idx):
        if idx == 0:
            return bits(self.g0, 22, 3)
        if idx in (1, 2, 3, 4):
            shift = {1: 1, 2: 10, 3: 19, 4: 28}[idx]
            return bits(self.g1, shift, 3)
        shift = {5: 6, 6: 15, 7: 24}[idx]
        return bits(self.g2, shift, 3)


class CPState:
    """Enough of CPMemory's global CP state to compute vertex sizes: the current
    VCD (vertex component descriptor, VCD_LO/VCD_HI) and the 8 VAT slots."""

    def __init__(self):
        self.vcd_lo = 0
        self.vcd_hi = 0
        self.vats = [VAT() for _ in range(8)]
        self.mat_idx_a = 0  # CP reg 0x30: PosNormalMtxIdx (bits 0-5) + Tex0-4MtxIdx
        self.mat_idx_b = 0  # CP reg 0x40: Tex5-7MtxIdx (not needed for pos matrix)

    def load(self, command: int, value: int):
        sub = command & CP_COMMAND_MASK
        if sub == VCD_LO:
            self.vcd_lo = value
        elif sub == VCD_HI:
            self.vcd_hi = value
        elif sub == CP_VAT_REG_A:
            self.vats[command & CP_VAT_MASK].g0 = value
        elif sub == CP_VAT_REG_B:
            self.vats[command & CP_VAT_MASK].g1 = value
        elif sub == CP_VAT_REG_C:
            self.vats[command & CP_VAT_MASK].g2 = value
        elif sub == MATINDEX_A:
            self.mat_idx_a = value
        elif sub == MATINDEX_B:
            self.mat_idx_b = value
        # ARRAY_BASE: not needed for vertex-size computation or matrix resolution.

    # --- matrix-index resolution (task: per-draw posmtx dump) ---
    def pnmtxidx_enabled(self) -> bool:
        """VCD_LO bit 0: PNMTXIDX present per-vertex (CPMemory.h TVtxDesc::Low0::PosMatIdx)."""
        return bool(bits(self.vcd_lo, 0, 1))

    def pnmtx_default(self) -> int:
        """CP MatrixIndexA bits 0-5: PosNormalMtxIdx, the matrix used when PNMTXIDX is NOT
        streamed per-vertex (CPMemory.h TMatrixIndexA::PosNormalMtxIdx)."""
        return bits(self.mat_idx_a, 0, 6)

    # --- VCD low (9 bits: PosMatIdx + 8x TexMatIdx) + Position/Normal/Color0/Color1 (2 bits each) ---
    def vcd_low9_popcount(self):
        return bin(self.vcd_lo & 0x1FF).count("1")

    def position_type(self):
        return bits(self.vcd_lo, 9, 2)

    def normal_type(self):
        return bits(self.vcd_lo, 11, 2)

    def color_type(self, idx):
        return bits(self.vcd_lo, 13 + idx * 2, 2)

    def texcoord_type(self, idx):
        return bits(self.vcd_hi, idx * 2, 2)

    def get_vertex_size(self, vat_idx: int) -> int:
        vat = self.vats[vat_idx]
        size = self.vcd_low9_popcount()  # PosMatIdx + up to 8 TexMatIdx bytes

        pos_type = self.position_type()
        if pos_type != 0:  # NotPresent
            size += POS_SIZE[pos_type][vat.pos_format()][vat.pos_elements()]

        norm_type = self.normal_type()
        if norm_type != 0:
            fmt = vat.normal_format()
            elements = vat.normal_elements()
            index3 = vat.normal_index3()
            if norm_type == 1:  # Direct
                size += NORMAL_SIZE_DIRECT[(index3, elements)][fmt]
            elif norm_type == 2:  # Index8
                size += NORMAL_SIZE_IDX8[(index3, elements)]
            elif norm_type == 3:  # Index16
                size += NORMAL_SIZE_IDX16[(index3, elements)]

        for i in (0, 1):
            c_type = self.color_type(i)
            if c_type != 0:
                size += COLOR_SIZE[c_type][vat.color_format(i)]

        for i in range(8):
            # TexCoord[i] enable comes from VCD_HI 2 bits per component (0..7), matching
            # TVtxDesc::High::TexCoord bitfield array (BitFieldArray<0,2,8,...>).
            tc_type = bits(self.vcd_hi, i * 2, 2)
            if tc_type != 0:
                fmt = vat.tex_format(i)
                elements = vat.tex_elements(i)
                size += TEX_SIZE[tc_type][fmt][elements]

        return size


@dataclass
class XFEvent:
    frame: int
    seq: int
    fifo_pos: int
    address: int
    count: int
    data_words: tuple
    kind: str  # "viewport" | "projection" | "other"


@dataclass
class DrawEvent:
    frame: int
    seq: int
    fifo_pos: int
    primitive: str
    vat: int
    num_vertices: int
    # --- per-draw matrix dump (task step 1 extension) ---
    proj_type: str = None            # "PERSPECTIVE" | "ORTHOGRAPHIC" | None (never set this frame)
    proj_floats: tuple = None        # 7 raw XF words (6 mtx floats + type), as floats/int
    mtx_source: str = None           # "default" | "per-vertex" | "per-vertex(unread)"
    mtx_index: int = None            # resolved pos-matrix number actually used by this draw
    posmtx: tuple = None             # 12 floats (3x4 row-major) or None if never written this frame
    posmtx_complete: bool = False    # False if any of the 12 XF words for mtx_index were never seen


@dataclass
class BPEvent:
    frame: int
    seq: int
    fifo_pos: int
    command: int
    value: int


def u32_to_f32(word: int) -> float:
    return struct.unpack(">f", struct.pack(">I", word & 0xFFFFFFFF))[0]


def classify_xf(address: int) -> str:
    if XFMEM_SETVIEWPORT <= address < XFMEM_SETVIEWPORT + 6:
        return "viewport"
    if 0x1020 <= address <= 0x103F:
        return "projection"
    return "other"


def decode_frame(frame_idx: int, data: bytes, cp: CPState):
    """Runs the GX opcode decoder over one frame's raw FIFO byte stream.
    Returns (xf_events, draw_events, bp_events, unknown_opcodes, warnings)."""
    xf_events, draw_events, bp_events = [], [], []
    warnings = []
    unknown = []
    seq = 0
    pos = 0
    n = len(data)
    # Live XF pos/normal-matrix memory (addr -> raw u32 word) and the most recent
    # GXSetProjection load, threaded through sequentially so each draw snapshots
    # exactly the state live at that point in the stream (persists across frames
    # via the caller-owned `cp`-style pattern would require passing these in too,
    # but posmtx/proj are real GX hardware state that also carries frame-to-frame;
    # the caller only reuses `cp`, so a draw very early in a frame that relies on
    # a matrix loaded in a PRIOR frame will show posmtx_complete=False here — note
    # this rather than silently guessing).
    xf_pos_mem = {}
    last_proj = None  # (type_str, 7-tuple of raw u32 words)
    while pos < n:
        op = data[pos]
        avail = n - pos

        if op == GX_NOP:
            count = 1
            while pos + count < n and data[pos + count] == GX_NOP:
                count += 1
            pos += count
            continue

        if op == GX_LOAD_CP_REG:
            if avail < 6:
                warnings.append(f"@{pos}: truncated GX_LOAD_CP_REG")
                break
            cmd2 = data[pos + 1]
            value = struct.unpack_from(">I", data, pos + 2)[0]
            cp.load(cmd2, value)
            seq += 1
            pos += 6
            continue

        if op == GX_LOAD_XF_REG:
            if avail < 5:
                warnings.append(f"@{pos}: truncated GX_LOAD_XF_REG")
                break
            cmd2 = struct.unpack_from(">I", data, pos + 1)[0]
            base_address = cmd2 & 0xFFFF
            stream_size = (cmd2 >> 16 & 0xF) + 1
            total = 5 + stream_size * 4
            if avail < total:
                warnings.append(f"@{pos}: truncated GX_LOAD_XF_REG data")
                break
            words = struct.unpack_from(f">{stream_size}I", data, pos + 5)
            kind = classify_xf(base_address)
            xf_events.append(XFEvent(frame_idx, seq, pos, base_address, stream_size, words, kind))
            if base_address < XF_POSMTX_END:
                for i, w in enumerate(words):
                    xf_pos_mem[base_address + i] = w
            if kind == "projection" and stream_size == 7:
                ptype = "ORTHOGRAPHIC" if words[6] else "PERSPECTIVE"
                last_proj = (ptype, words)
            seq += 1
            pos += total
            continue

        if op in (GX_LOAD_INDX_A, GX_LOAD_INDX_B, GX_LOAD_INDX_C, GX_LOAD_INDX_D):
            if avail < 5:
                warnings.append(f"@{pos}: truncated GX_LOAD_INDX")
                break
            seq += 1
            pos += 5
            continue

        if op == GX_CMD_CALL_DL:
            if avail < 9:
                warnings.append(f"@{pos}: truncated GX_CMD_CALL_DL")
                break
            addr, size = struct.unpack_from(">II", data, pos + 1)
            warnings.append(
                f"@{pos}: GX_CMD_CALL_DL addr=0x{addr:08x} size={size} — display-list body "
                f"NOT expanded (not resident in this .dff's captured bytes); sequence after "
                f"this point may omit XF/draw events that live inside the DL")
            seq += 1
            pos += 9
            continue

        if op in (GX_CMD_UNKNOWN_METRICS, GX_CMD_INVL_VC):
            seq += 1
            pos += 1
            continue

        if op == GX_LOAD_BP_REG:
            if avail < 5:
                warnings.append(f"@{pos}: truncated GX_LOAD_BP_REG")
                break
            cmd2 = data[pos + 1]
            value = (data[pos + 2] << 16) | (data[pos + 3] << 8) | data[pos + 4]
            bp_events.append(BPEvent(frame_idx, seq, pos, cmd2, value))
            seq += 1
            pos += 5
            continue

        if GX_PRIMITIVE_START <= op <= GX_PRIMITIVE_END:
            if avail < 3:
                warnings.append(f"@{pos}: truncated primitive header")
                break
            primitive = (op & 0x78) >> 3
            vat = op & 0x07
            num_vertices = struct.unpack_from(">H", data, pos + 1)[0]
            vertex_size = cp.get_vertex_size(vat)
            total = 3 + num_vertices * vertex_size
            if avail < total:
                warnings.append(
                    f"@{pos}: primitive claims {num_vertices} verts * {vertex_size}B "
                    f"= {num_vertices * vertex_size}B but only {avail - 3} available — "
                    f"vertex-size computation likely wrong, ABORTING frame {frame_idx} decode")
                break
            # --- resolve which pos matrix this draw uses (task step 1) ---
            per_vertex = cp.pnmtxidx_enabled()
            if per_vertex:
                # PNMTXIDX is the first byte of every vertex (VertexLoaderBase
                # attribute order: PosMtxIdx, Tex0-7MtxIdx, Position, Normal,
                # Color0-1, TexCoord0-7). Read vertex 0's raw index directly —
                # no pragmatic per-vertex breakdown attempted (task allows
                # "else mark it"; here it IS easy, so we read it).
                if num_vertices > 0 and pos + 3 < n:
                    mtx_index = data[pos + 3]
                    mtx_source = "per-vertex"
                else:
                    mtx_index = None
                    mtx_source = "per-vertex(unread)"
            else:
                mtx_index = cp.pnmtx_default()
                mtx_source = "default"

            posmtx = None
            posmtx_complete = False
            if mtx_index is not None:
                base = mtx_index * 12
                raw = [xf_pos_mem.get(base + i) for i in range(12)]
                posmtx_complete = all(w is not None for w in raw)
                posmtx = tuple(u32_to_f32(w) if w is not None else None for w in raw)

            proj_type = last_proj[0] if last_proj else None
            proj_floats = tuple(u32_to_f32(w) for w in last_proj[1][:6]) + (last_proj[1][6],) \
                if last_proj else None

            draw_events.append(DrawEvent(frame_idx, seq, pos, PRIMITIVE_NAMES[primitive], vat,
                                          num_vertices, proj_type=proj_type, proj_floats=proj_floats,
                                          mtx_source=mtx_source, mtx_index=mtx_index, posmtx=posmtx,
                                          posmtx_complete=posmtx_complete))
            seq += 1
            pos += total
            continue

        unknown.append((pos, op))
        seq += 1
        pos += 1

    return xf_events, draw_events, bp_events, unknown, warnings


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dff", help="path to a Dolphin .dff FIFO log")
    ap.add_argument("--frame", type=int, default=None, help="only show this frame index")
    ap.add_argument("--summary", action="store_true", help="print counts only, no full event list")
    ap.add_argument("--matrix-tsv", metavar="OUT",
                     help="write a per-draw TSV (seq, nverts, prim, projection, posmtx) to OUT "
                          "instead of the normal timeline dump")
    args = ap.parse_args()

    with open(args.dff, "rb") as f:
        buf = f.read()
    if len(buf) == 0:
        raise SystemExit("REFUSING: 0-byte .dff file")

    hdr = parse_header(buf)
    print(f"# {args.dff}")
    print(f"# gameid={hdr.gameid!r} file_version={hdr.file_version} frames={hdr.frame_count}")

    frames = [
        parse_frame_info(buf, hdr.frame_list_offset + i * FRAME_SIZE)
        for i in range(hdr.frame_count)
    ]

    cp = CPState()  # CP state persists across frames (real GX hardware state, not per-frame)

    if args.matrix_tsv:
        out_rows = []
        for i, fr in enumerate(frames):
            if args.frame is not None and i != args.frame:
                continue
            data = buf[fr.fifo_data_offset: fr.fifo_data_offset + fr.fifo_data_size]
            _xf_events, draw_events, _bp_events, _unknown, _warnings = decode_frame(i, data, cp)
            out_rows.extend(draw_events)
        cols = ["frame", "seq", "prim", "nverts", "proj_type"] + [f"proj{k}" for k in range(7)] + \
               ["mtx_source", "mtx_index", "posmtx_complete"] + [f"pm{k}" for k in range(12)]
        with open(args.matrix_tsv, "w") as out:
            out.write("\t".join(cols) + "\n")
            for e in out_rows:
                proj = e.proj_floats if e.proj_floats else (None,) * 7
                pm = e.posmtx if e.posmtx else (None,) * 12
                row = [e.frame, e.seq, e.primitive, e.num_vertices, e.proj_type or ""] + \
                      [f"{v:.6g}" if isinstance(v, float) else ("" if v is None else v) for v in proj] + \
                      [e.mtx_source or "", e.mtx_index if e.mtx_index is not None else "",
                       int(e.posmtx_complete)] + \
                      [f"{v:.6g}" if v is not None else "" for v in pm]
                out.write("\t".join(str(x) for x in row) + "\n")
        print(f"wrote {len(out_rows)} draws to {args.matrix_tsv}", file=sys.stderr)
        return

    for i, fr in enumerate(frames):
        if args.frame is not None and i != args.frame:
            continue
        data = buf[fr.fifo_data_offset: fr.fifo_data_offset + fr.fifo_data_size]
        xf_events, draw_events, bp_events, unknown, warnings = decode_frame(i, data, cp)

        print(f"\n== frame {i}  fifoStart=0x{fr.fifo_start:x} fifoEnd=0x{fr.fifo_end:x} "
              f"bytes={fr.fifo_data_size} ==")
        print(f"   xf_loads={len(xf_events)} draws={len(draw_events)} bp_loads={len(bp_events)} "
              f"unknown_opcodes={len(unknown)}")
        for w in warnings:
            print(f"   WARN {w}")

        if args.summary:
            continue

        # Merge XF and draw events into one seq-ordered timeline for this frame.
        timeline = [(e.seq, "XF", e) for e in xf_events] + \
                   [(e.seq, "DRAW", e) for e in draw_events]
        timeline.sort(key=lambda t: t[0])
        for seq, kind, e in timeline:
            if kind == "XF":
                tag = f"[{e.kind.upper()}]" if e.kind != "other" else ""
                print(f"   seq={e.seq:5d} pos=0x{e.fifo_pos:06x} XF addr=0x{e.address:04x} "
                      f"count={e.count} {tag} data={[hex(w) for w in e.data_words]}")
            else:
                print(f"   seq={e.seq:5d} pos=0x{e.fifo_pos:06x} DRAW {e.primitive} vat={e.vat} "
                      f"nverts={e.num_vertices}")


if __name__ == "__main__":
    main()
