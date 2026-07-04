// gx_fifo.cpp — the FIFO byte buffer sms-boot's GX seam writes into.
// See gx_fifo.h.

#include "gx_fifo.h"
#include "../../runtime/engine.h"

#include <cstring>
#include <vector>

// Weak-linked bridge to Dolphin's own LoadBPReg/LoadXFReg (native/render/
// oracle_direct.cpp). When Dolphin's video backend is up, EVERY sb::gxfifo
// register write also fires directly against Dolphin's live bpmem/xfmem — no
// FIFO decode round-trip. Weak so the standalone (native-only) build links
// without oracle_direct.o.
namespace sb::oracle {
    bool ready() __attribute__((weak));
    void bp_write(uint8_t, uint32_t) __attribute__((weak));
    void xf_write(uint16_t, uint32_t, const uint32_t*) __attribute__((weak));
}

namespace sb::gxfifo {

namespace {
bool s_enabled = false;
// Frame FIFO buffer. Grows as writes come in; capped at ~64 MB to notice runaway
// captures instead of eating host RAM. reset_frame() clears (keeps capacity).
std::vector<uint8_t> s_buf;
constexpr size_t kMaxBufBytes = 64ull * 1024 * 1024;
} // namespace

void init() {
    s_enabled = (sb::engine::mode() == sb::engine::RenderMode::GX_ORACLE);
    if (s_enabled) s_buf.reserve(4 * 1024 * 1024);   // 4 MB initial
}

bool enabled() { return s_enabled; }
void set_enabled(bool on) { s_enabled = on; }

static inline void append(const void* p, size_t n) {
    if (!s_enabled) return;
    if (s_buf.size() + n > kMaxBufBytes) return;   // hard cap; log elsewhere
    const auto old_size = s_buf.size();
    s_buf.resize(old_size + n);
    std::memcpy(s_buf.data() + old_size, p, n);
}

void write_u8(uint8_t v)       { append(&v, 1); }

void write_u16_be(uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    append(b, 2);
}

void write_u32_be(uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    append(b, 4);
}

void write_f32_be(float v) {
    uint32_t u;
    std::memcpy(&u, &v, 4);
    write_u32_be(u);
}

void write_bytes(const void* p, size_t n) { append(p, n); }

void reset_frame() { s_buf.clear(); }

const uint8_t* data() { return s_buf.data(); }
size_t size() { return s_buf.size(); }

} // namespace sb::gxfifo

// C-linkage bridges the SDK write-gather pipe redirect calls into (GXVert.h).
// One entry per width; all funnel into the same FIFO buffer. These are the
// SOLE integration point for J3D's raw BP/XF/CP writes via GXCmd/GXParam.
extern "C" {
void sb_gx_wgfifo_u8 (unsigned char  v) { sb::gxfifo::write_u8(v); }
void sb_gx_wgfifo_u16(unsigned short v) { sb::gxfifo::write_u16_be(v); }
void sb_gx_wgfifo_u32(unsigned int   v) { sb::gxfifo::write_u32_be(v); }
void sb_gx_wgfifo_f32(float          v) { sb::gxfifo::write_f32_be(v); }
}
