// oracle_direct.h — bridge from the GX-seam layer into Dolphin's OWN renderer,
// bypassing the FIFO byte reconstruction path entirely.
//
// The old Tier-2 architecture encoded every GX SDK call into GC FIFO bytes
// (sb::gxfifo), then drained them through OpcodeDecoder::RunFifo — meaning we
// had to reconstruct Dolphin's exact byte layout for BP/XF/CP registers plus
// the DRAW opcode + vertex payload. Every missed byte / miscoded field killed
// rasterisation silently until a diagnostic surfaced it (SETINVERTEXSPEC,
// SCISSOROFFSET, etc. — a running list of "silently missing" state).
//
// The new architecture calls Dolphin's own LoadBPReg / LoadXFReg directly —
// the same functions OpcodeDecoder would call after decoding a FIFO byte
// stream. No byte encoding, no decode round-trip, no coverage gaps.
//
// Ownership: this bridge is ONLY meaningful when sb::engine::mode() ==
// GX_ORACLE AND Dolphin's video backend is initialised. Both are checked
// here so callers can invoke unconditionally — the functions no-op if the
// preconditions aren't met (with a log line the first time, so a silent
// mis-init still surfaces).

#pragma once

#include <cstdint>

namespace sb::oracle {

// Idempotent: brings up Dolphin's video backend if it isn't already.
// Called from the harness before the scenario runs, so every subsequent
// GX SDK setter can write to Dolphin's bpmem/xfmem immediately.
bool ensure_up();

// True if Dolphin is ready to receive direct writes.
bool ready();

// Direct BP register write. Reg is the 8-bit BP address; value is 24-bit.
// Idempotent no-op when Dolphin isn't up (harness runs in Tier-1 mode).
void bp_write(uint8_t reg, uint32_t value_24bit);

// Direct XF register/memory write. addr is the base XF address; nwords is
// the number of 32-bit words in `data`. LoadXFReg treats `data` as a raw
// byte array, so words must be in big-endian (Dolphin swaps back).
void xf_write(uint16_t base_addr, uint32_t nwords, const uint32_t* be_words);

// Convenience: single 32-bit write to `addr`.
void xf_write_1(uint16_t addr, uint32_t value_be);

// Convenience: N-word XF write from a host-endian f32 array (words are
// bit-cast to u32 and byte-swapped to BE by LoadXFReg).
void xf_write_f32(uint16_t addr, uint32_t nwords, const float* host_words);

} // namespace sb::oracle
