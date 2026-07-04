// gx_fifo.h — the FIFO byte buffer sms-boot's GX seam writes into.
//
// Under GX_ORACLE (SB_RENDER=oracle), each GX seam call in gx_impl.cpp / gx_imm_impl.cpp
// / gx_fb_impl.cpp appends the matching GC FIFO command bytes into a per-frame buffer
// via sb_fifo_write_*. At present time, the oracle sink drains the buffer through
// Dolphin's OpcodeDecoder::Run — which invokes the SAME callback path Dolphin's
// CommandProcessor uses when running a real game. Every BP write, XF write, CP write,
// and DRAW opcode goes through Dolphin's own BPWritten/XFWritten/VertexManager, so the
// output matches build/sunbright by construction.
//
// Under NATIVE_PC, sb_fifo_enabled() returns false and every write is a fast no-op —
// the SDL3 renderer consumes state via the existing seam captures, not the FIFO.
//
// Format (see reference/sms/include/dolphin/gx/GXCommandList.h):
//   0x00 NOP
//   0x08 CP_REG    +u8 reg           +u32 value
//   0x10 XF_REG    +u16 count-1      +u16 reg_addr  +u32 value[count]
//   0x61 BP_REG    +u8 reg_hi        +u24 value  (packed as u32 BE: reg<<24 | value&0xFFFFFF)
//   0x80/0x90/0x98/0xA0/0xA8/0xB0/0xB8 DRAW  +u16 count  +vertex_data[count * stride]
//     Low 3 bits of the DRAW opcode select the vertex-attribute table index (VAT).

#pragma once
#include <cstddef>
#include <cstdint>

// Weak-linked bridge to Dolphin's own LoadBPReg/LoadXFReg entry points (see
// native/render/oracle_direct.{h,cpp}). When Dolphin's video backend is up,
// every BP / XF write below ALSO fires directly against Dolphin's live
// bpmem/xfmem — no FIFO decode round-trip, no missed registers due to encode
// bugs. Weak so the standalone (native-only) build still links.
namespace sb::oracle {
    bool ready() __attribute__((weak));
    void bp_write(uint8_t, uint32_t) __attribute__((weak));
    void xf_write(uint16_t, uint32_t, const uint32_t*) __attribute__((weak));
}

namespace sb::gxfifo {

// Called once at engine init from sb::engine::init_from_env(): enables recording
// only when the render mode is GX_ORACLE. Idempotent; no cost if called twice.
void init();

// True when writes should actually record. Inlined out to a plain global bool
// in the .cpp so the per-call cost is a single load + branch.
bool enabled();

// Explicit setter for A/B testing outside the engine mode plumbing.
void set_enabled(bool on);

// Append raw bytes into the frame buffer. All variants are safe when disabled
// (single global load + early return). Big-endian encoding matches GC FIFO wire
// format — Dolphin's OpcodeDecoder assumes BE.
void write_u8(uint8_t v);
void write_u16_be(uint16_t v);
void write_u32_be(uint32_t v);
void write_f32_be(float v);
void write_bytes(const void* p, size_t n);   // raw copy (for vertex payloads)

// Drop everything recorded so far. Called at frame boundary by the oracle sink
// AFTER draining so the next frame starts empty.
void reset_frame();

// Snapshot the buffer for the drain step. Pointer is valid until reset_frame().
const uint8_t* data();
size_t size();

// Helpers for the most common opcodes so seam call sites stay terse. When
// Dolphin is up, ALSO route the write into Dolphin's live bpmem/xfmem via the
// direct bridge (sb::oracle::bp_write / xf_write). The FIFO byte buffer is
// still populated for compatibility with the OpcodeDecoder drain path — but
// state actually LANDS in Dolphin through the direct route, which sidesteps
// every "we forgot to emit register X" silent-fail (SETINVERTEXSPEC,
// SCISSOROFFSET, TREF, etc. were all discoverable this way).
inline void bp_write(uint8_t reg, uint32_t value24) {
    write_u8(0x61);
    write_u32_be((uint32_t)reg << 24 | (value24 & 0xFFFFFF));
    if (&sb::oracle::ready && sb::oracle::ready() && &sb::oracle::bp_write)
        sb::oracle::bp_write(reg, value24);
}
inline void cp_write(uint8_t reg, uint32_t value) {
    write_u8(0x08);
    write_u8(reg);
    write_u32_be(value);
    // CP registers control VertexLoader/CP state on Dolphin. There's no
    // exposed LoadCPReg equivalent in Dolphin's namespace at the same level
    // as LoadBPReg/LoadXFReg — CP state is handled by the VertexManager +
    // CommandProcessor internally. For now CP writes stay FIFO-only; the
    // OpcodeDecoder drain applies them. Add a direct path here when
    // Dolphin exposes one.
}
// XF single-register write (count=1).
inline void xf_write_u32(uint16_t reg, uint32_t value) {
    write_u8(0x10);
    write_u16_be(0);
    write_u16_be(reg);
    write_u32_be(value);
    if (&sb::oracle::ready && sb::oracle::ready() && &sb::oracle::xf_write) {
        // Direct bridge expects big-endian on the wire (LoadXFReg unswaps).
        uint32_t be = __builtin_bswap32(value);
        sb::oracle::xf_write(reg, 1, &be);
    }
}
inline void xf_write_f32(uint16_t reg, float value) {
    write_u8(0x10);
    write_u16_be(0);
    write_u16_be(reg);
    write_f32_be(value);
    if (&sb::oracle::ready && sb::oracle::ready() && &sb::oracle::xf_write) {
        uint32_t u; __builtin_memcpy(&u, &value, 4);
        uint32_t be = __builtin_bswap32(u);
        sb::oracle::xf_write(reg, 1, &be);
    }
}

} // namespace sb::gxfifo
