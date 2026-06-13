// native_exi.cpp — PC-native service for the EXI bus parts SMS uses BESIDES the memory card.
// ============================================================================================
// Opt-in:  SUNBRIGHT_NATIVE_EXI=1   (default OFF → every override below transparently runs the
//          original recompiled body via recomp_raw(), so behaviour is unchanged unless asked).
//
// WHY (root cause class). Dolphin's External Interface (externals/dolphin/.../HW/EXI/) emulates
// the EXI channels at 0xCC006800+; transfers complete via an EXI transfer-complete interrupt and
// the SDK busy-polls EXISync()/the channel CSR for them. Under our hybrid timing the host-clock /
// audio governor parks emulated time at a target, so any "deliver later" CoreTiming completion
// can become unreachable (the same lost-completion class that wedged __CARDSync — see
// runtime/overrides/native_card.cpp). The memory card is ALREADY owned natively there; this file
// owns the REMAINING non-card EXI consumers so no EXI poll/interrupt black box can wedge boot or
// require debugging:
//
//   * SRAM / settings block  (channel 0, device 1, the RTC+SRAM chip)
//   * __OSReadROM            (IPL font image — channel 0, device 1, same chip)
//
// (The RTC *time* itself, __OSGetSystemTime @ 0x803494f8 = OSGetTime()+counterBias, is the Gekko
//  time base (mftb), NOT an EXI transfer — Sunbright already serves mftb from Dolphin's live TB.
//  So there is no separate "RTC read" EXI black box to own; counterBias lives in the SRAM block,
//  which we populate below. SMS reads no host wall-clock RTC.)
//
// ── GC EXI usage RE (reference/sms/src/dolphin/exi/EXIBios.c, /os/OSRtc.c, /os/OSFont.c) ─────
// EXI primitives (EXIBios.c): EXILock/EXIUnlock, EXISelect(chan,dev,freq), EXIImm/EXIImmEx (PIO),
//   EXIDma (block), EXISync (spin until the transfer's TC fires / channel idle), EXIDeselect.
//   The transfer-complete path is interrupt-driven; EXISync is the spin point that depends on it.
// SRAM (OSRtc.c): ReadSram (INLINED into __OSInitSram) does EXISelect(0,1,3) then EXIImm(cmd=
//   0x20000100) + EXISync + EXIDma(buffer,0x40) + EXISync — i.e. one 64-byte read of the SRAM
//   block off channel 0 / device 1 at boot. WriteSram (0x803474f0) writes it back (cmd 0xA00001xx
//   + EXIImmEx). The 64-byte block is the OSSram header:
//     +0x00 u16 checkSum     +0x02 u16 checkSumInv   +0x04 u32 ead0    +0x08 u32 ead1
//     +0x0C u32 counterBias  +0x10 s8 displayOffsetH +0x11 u8 ntd      +0x12 u8 language
//     +0x13 u8 flags         (bytes 0x14..0x3F = OSSramEx: flashID/wirelessID/dvdErrorCode…)
//   UnlockSram recomputes checkSum/checkSumInv as a 16-bit big-endian additive sum over the u16s
//   in [counterBias (0x0C) .. sizeof(OSSram)=0x14) (4 words). flags: bit2=kStereo, bit3=kOobeDone,
//   bit6=kBootToMenu, bit7=kProgressiveScan. (Matches Dolphin Sram.cpp FixSRAMChecksums / SramFlags
//   and its English template sram_dump: flags = 0x20|kOobeDone|kStereo = 0x2C, lang=0=English.)
//   Consumers: OSGetSoundMode (flags&4), OSGetProgressiveMode (flags&0x80), OSGetLanguage (+0x12),
//   OSGetWirelessID (OSSramEx) — all read the in-RAM Scb.sram; once it holds a valid block they
//   need no EXI.
// __OSReadROM (0x80347b54): __OSReadROM(void* buf=r3, long len=r4, long off=r5) — EXISelect(0,1,3)
//   + EXIImm(cmd=off<<6) + EXISync + EXIDma(buf,len) + EXISync. OSFont.c ReadROM pulls the IPL
//   font (offset 0x1AFF00 SJIS / 0x1FCF00 ANSI) and retries while it returns 0.
//
// ── Statics / addresses (GMSE01; reference/sms_gmse01_funcs.txt + recomp disasm) ─────────────
//   Scb (SramControl) base = 0x80402640  (from __OSLockSram: r3=0x80400000, r31=r3+9792=0x2640;
//                                          locked@+0x48, enabled@+0x44, sync@+0x4C verified)
//   __OSInitSram   0x80347608   __OSReadROM   0x80347b54
//   WriteSram      0x803474f0   __OSSyncSram  0x80347b44
//
// ── What we do (faithful, no EXI) ────────────────────────────────────────────────────────────
//   __OSInitSram : populate Scb.sram[64] with a valid English/stereo/OOBE-done block (correct
//                  checksum), set offset=0x40, locked=enabled=0, sync=1. No EXISelect/Imm/Dma.
//                  Every later OSGet* reads valid data straight from RAM.
//   WriteSram    : no EXI. Persist the 64-byte block to a host file (SUNBRIGHT_SRAM, default
//                  scratch/sram.raw) so settings survive a run; return success.
//   __OSSyncSram : return 1 (always synced — there is no outstanding async EXI write).
//   __OSReadROM  : serve the IPL font from a host file if present (SUNBRIGHT_IPL_FONT), else
//                  zero-fill (blank glyphs, never wedges) and return success so OSFont's retry
//                  loop exits. No EXIDma, no EXISync spin.
//
// ── REQUIRED SEAM ────────────────────────────────────────────────────────────────────────────
//   NONE. Self-registers via SUNBRIGHT_OVERRIDE; consulted by recomp_lookup()/SunbrightBridge on
//   JIT-entry, bl, and bctr/blr alike. No edit to any existing file is required.
//
// ── Verify recipe ────────────────────────────────────────────────────────────────────────────
//   1. Default OFF (no flag): headless boot to gameplay must be byte-for-byte as before
//      (overrides fall through to recomp_raw). SUNBRIGHT_HEADLESS=1 ./build/sunbright.
//   2. SUNBRIGHT_NATIVE_EXI=1 SUNBRIGHT_HEADLESS=1 ./build/sunbright  → reaches gameplay; stderr
//      shows "[native_exi] SRAM init native …"; no EXI-sync spin/hang; audio/video unchanged
//      (sound mode = stereo, language English). Probe: /r?a=80402640&n=16 shows the block; the
//      first two bytes (checkSum/checkSumInv) are a consistent additive pair over words 0x0C..0x14.
//   3. A/B parity: SUNBRIGHT_DISABLE_RECOMP run is unaffected (overrides never dispatch under the
//      oracle), so it remains a clean baseline.
//
// Build note: NEW file → run `cmake -B build` (CMake file(GLOB) is configure-time) before building.
// ============================================================================================

#include "../overrides.h"
#include "../intrinsics.h"

#ifdef HAVE_DOLPHIN_CORE

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <sys/stat.h>

namespace {

// ── addresses ────────────────────────────────────────────────────────────────
constexpr u32 SCB_BASE      = 0x80402640u;   // SramControl Scb; sram[64] is the first member
constexpr u32 SCB_SRAM      = SCB_BASE + 0x00;
constexpr u32 SCB_OFFSET    = SCB_BASE + 0x40;
constexpr u32 SCB_ENABLED   = SCB_BASE + 0x44;
constexpr u32 SCB_LOCKED    = SCB_BASE + 0x48;
constexpr u32 SCB_SYNC      = SCB_BASE + 0x4C;

constexpr u32 ADDR_INIT_SRAM = 0x80347608u;  // __OSInitSram
constexpr u32 ADDR_WRITE_SRAM= 0x803474f0u;  // WriteSram
constexpr u32 ADDR_SYNC_SRAM = 0x80347b44u;  // __OSSyncSram
constexpr u32 ADDR_READ_ROM  = 0x80347b54u;  // __OSReadROM

// SramFlags (Dolphin Sram.h / GC IPL): stereo=bit2, oobeDone=bit3, bootToMenu=bit6, progScan=bit7.
constexpr u8  SRAM_FLAGS     = 0x20u | 0x08u | 0x04u;   // 0x2C — English template: OOBE done, stereo
constexpr u8  SRAM_LANGUAGE  = 0u;                       // 0 = English (GMSE01 = NTSC-U)

// ── opt-in gate ──────────────────────────────────────────────────────────────
bool native_exi_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("SUNBRIGHT_NATIVE_EXI");
        v = (e && e[0] == '1') ? 1 : 0;
    }
    return v != 0;
}

// Run the original recompiled body for `addr` (transparent fall-through when the flag is off).
// Returns true if it dispatched; false if `addr` isn't recompiled (then the override must serve).
bool run_original(u32 addr, CPUState& cpu) {
    if (RecompFunc raw = recomp_raw(addr)) { raw(cpu); return true; }
    return false;
}

// ── host backing files ───────────────────────────────────────────────────────
std::string sram_path() {
    if (const char* e = getenv("SUNBRIGHT_SRAM")) return e;
    return "scratch/sram.raw";
}
std::string font_path() {
    if (const char* e = getenv("SUNBRIGHT_IPL_FONT")) return e;
    return "";   // none by default → zero-fill (blank font)
}

// Compute the GC 16-bit big-endian additive checksum over u16 words in [0x0C, 0x14) of a 64-byte
// SRAM block and write checkSum/checkSumInv at [0x00]/[0x02]. Mirrors UnlockSram / FixSRAMChecksums.
void fix_sram_checksum(uint8_t blk[64]) {
    uint16_t sum = 0, suminv = 0;
    for (int off = 0x0C; off < 0x14; off += 2) {
        uint16_t w = (uint16_t)((blk[off] << 8) | blk[off + 1]);   // big-endian word
        sum    += w;
        suminv += (uint16_t)~w;
    }
    blk[0] = (uint8_t)(sum >> 8);    blk[1] = (uint8_t)(sum & 0xFF);
    blk[2] = (uint8_t)(suminv >> 8); blk[3] = (uint8_t)(suminv & 0xFF);
}

// Build the default English/stereo/OOBE-done SRAM block (matches Dolphin's sram_dump settings).
void build_default_sram(uint8_t blk[64]) {
    memset(blk, 0, 64);
    // counterBias (0x0C..0x0F) = 0, displayOffsetH (0x10) = 0, ntd (0x11) = 0
    blk[0x12] = SRAM_LANGUAGE;   // language
    blk[0x13] = SRAM_FLAGS;      // flags
    fix_sram_checksum(blk);
}

// Load the 64-byte block from the host file if it exists & is large enough; else build default.
void load_sram_block(uint8_t blk[64]) {
    const std::string path = sram_path();
    if (FILE* f = fopen(path.c_str(), "rb")) {
        size_t n = fread(blk, 1, 64, f);
        fclose(f);
        if (n == 64) {
            fix_sram_checksum(blk);   // keep checksum honest regardless of file contents
            return;
        }
    }
    build_default_sram(blk);
}

void save_sram_block(const uint8_t blk[64]) {
    const std::string path = sram_path();
    // best-effort: ensure scratch/ exists when using the default path
    if (path.rfind("scratch/", 0) == 0) ::mkdir("scratch", 0755);
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(blk, 1, 64, f);
        fclose(f);
    }
}

void write_block_to_guest(u32 ea, const uint8_t blk[64]) {
    for (int i = 0; i < 64; ++i) sb_w8(ea + (u32)i, blk[i]);
}
void read_block_from_guest(u32 ea, uint8_t blk[64]) {
    for (int i = 0; i < 64; ++i) blk[i] = sb_r8(ea + (u32)i);
}

// ── overrides ────────────────────────────────────────────────────────────────

// void __OSInitSram(void)  — 0x80347608
SUNBRIGHT_OVERRIDE(ov___OSInitSram, ADDR_INIT_SRAM) {
    if (!native_exi_enabled()) { if (run_original(ADDR_INIT_SRAM, cpu)) return; }

    uint8_t blk[64];
    load_sram_block(blk);
    write_block_to_guest(SCB_SRAM, blk);
    sb_w32(SCB_OFFSET,  0x40u);   // nothing pending to write back
    sb_w32(SCB_ENABLED, 0u);
    sb_w32(SCB_LOCKED,  0u);
    sb_w32(SCB_SYNC,    1u);       // ReadSram succeeded

    static bool once = false;
    if (!once) {
        once = true;
        fprintf(stderr,
                "[native_exi] SRAM init native (no EXI): flags=0x%02X lang=%u sync=1\n",
                blk[0x13], blk[0x12]);
    }
    // void function — return through caller's lr (override convention: just return)
}

// static int WriteSram(void* buffer=r3, u32 offset=r4, u32 size=r5)  — 0x803474f0
// Returns BOOL success in r3. We don't touch EXI; we mirror the (already in-RAM) Scb.sram block
// out to the host file so settings persist, and report success.
SUNBRIGHT_OVERRIDE(ov_WriteSram, ADDR_WRITE_SRAM) {
    if (!native_exi_enabled()) { if (run_original(ADDR_WRITE_SRAM, cpu)) return; }

    uint8_t blk[64];
    read_block_from_guest(SCB_SRAM, blk);   // SDK already committed checksum into Scb.sram
    save_sram_block(blk);
    cpu.gpr[3] = 1;   // TRUE
}

// int __OSSyncSram(void)  — 0x80347b44 : returns Scb.sync. With native WriteSram there is never an
// outstanding async transfer, so it is always synced.
SUNBRIGHT_OVERRIDE(ov___OSSyncSram, ADDR_SYNC_SRAM) {
    if (!native_exi_enabled()) { if (run_original(ADDR_SYNC_SRAM, cpu)) return; }
    sb_w32(SCB_SYNC, 1u);
    cpu.gpr[3] = 1;
}

// int __OSReadROM(void* buffer=r3, long length=r4, long offset=r5)  — 0x80347b54
// Serve the IPL font from a host dump if provided, else zero-fill. Returns BOOL success in r3.
SUNBRIGHT_OVERRIDE(ov___OSReadROM, ADDR_READ_ROM) {
    if (!native_exi_enabled()) { if (run_original(ADDR_READ_ROM, cpu)) return; }

    const u32 buffer = cpu.gpr[3];
    const s32 length = (s32)cpu.gpr[4];
    const s32 offset = (s32)cpu.gpr[5];
    if (buffer < 0x80000000u || length <= 0) { cpu.gpr[3] = 0; return; }

    bool served = false;
    const std::string fp = font_path();
    if (!fp.empty()) {
        if (FILE* f = fopen(fp.c_str(), "rb")) {
            if (fseek(f, offset, SEEK_SET) == 0) {
                static thread_local uint8_t tmp[0x4000];
                s32 done = 0;
                served = true;
                while (done < length) {
                    s32 chunk = length - done;
                    if (chunk > (s32)sizeof tmp) chunk = (s32)sizeof tmp;
                    size_t got = fread(tmp, 1, (size_t)chunk, f);
                    for (size_t i = 0; i < got; ++i) sb_w8(buffer + (u32)done + (u32)i, tmp[i]);
                    for (size_t i = got; i < (size_t)chunk; ++i) sb_w8(buffer + (u32)done + (u32)i, 0);
                    done += chunk;
                    if (got < (size_t)chunk) { /* short read → rest zero-filled, stop */ break; }
                }
            }
            fclose(f);
        }
    }
    if (!served) {
        for (s32 i = 0; i < length; ++i) sb_w8(buffer + (u32)i, 0);   // blank font, never wedges
    }

    static bool once = false;
    if (!once) {
        once = true;
        fprintf(stderr, "[native_exi] __OSReadROM native (no EXI): %s, off=0x%X len=0x%X\n",
                served ? fp.c_str() : "zero-fill (no SUNBRIGHT_IPL_FONT)", offset, length);
    }
    cpu.gpr[3] = 1;   // TRUE — OSFont's retry loop exits
}

}  // namespace

#endif  // HAVE_DOLPHIN_CORE
