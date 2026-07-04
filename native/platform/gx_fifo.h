// gx_fifo.h — STUB. The GX-seam FIFO byte recorder used to feed the retired
// Dolphin videovulkan oracle sink. All calls are gated on enabled() which is
// permanently false in the post-purge single-path world, so an optimizing
// compiler elides every call. Kept as a stub only so gx_impl.cpp / gx_imm_impl.cpp
// still compile without a mass-edit; the callsites are pruned as those files
// are folded into sms-boot/render_pc during consolidation.
//
// Do NOT restore state to this file. If sms-boot needs a FIFO recorder again,
// it will be built against Aurora's fifo, not resurrected from this stub.

#pragma once

#include <cstddef>
#include <cstdint>

namespace sb::gxfifo {

inline constexpr bool enabled() { return false; }
inline void init() {}
inline void reset_frame() {}
inline void set_enabled(bool) {}

inline void write_u8 (uint8_t)  {}
inline void write_u16_be(uint16_t) {}
inline void write_u32_be(uint32_t) {}
inline void write_f32_be(float)    {}
inline void write_bytes(const void*, std::size_t) {}

inline void bp_write(uint8_t, uint32_t) {}
inline void cp_write(uint8_t, uint32_t) {}
inline void xf_write(uint16_t, uint32_t, const uint32_t*) {}

// Buffer accessors — used by present-code paths that assume there's a drained
// FIFO buffer. Always empty in the stub.
inline const uint8_t* data() { return nullptr; }
inline std::size_t    size() { return 0; }

} // namespace sb::gxfifo
