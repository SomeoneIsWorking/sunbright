// ─────────────────────────────────────────────────────────────────────────────
// native_vi2.cpp — PC-native VI scan-out / present-address ownership.
//
// OPT-IN: SUNBRIGHT_NATIVE_VI=1. Default OFF = fully inert (interp_redraw.cpp's
// existing setNextXFB present path is unchanged — the working 60fps baseline is
// protected). This file is INERT unless the flag is set: every entry point returns
// immediately. Clean NEW-FILE rewrite of scratch/native_drafts/native_vi_present.cpp
// with NO existing-file edits — the coordination point in interp_redraw.cpp is a
// call site documented as the REQUIRED SEAM below.
//
// ── WHAT THIS OWNS ───────────────────────────────────────────────────────────
// PC-native control of the VI's scan-out ADDRESS SELECTION so the 60fps interpolator
// (interp_redraw.cpp) puts TWO DISTINCT XFB addresses on screen per 30 Hz game tick
// — a clean R,B,R,B present cadence — independent of Dolphin's VI register quirks or
// the guest VI library's single-buffer / skip-if-unchanged aliasing.
//
// ── WHAT I RE'd ──────────────────────────────────────────────────────────────
//  docs/re_notes/vi_xfb_present.md — the RE of this path + the five RRBB hazards.
//  GC VI lib  reference/sms/src/dolphin/vi/vi.c
//    - VISetNextFrameBuffer (811) → setFbbRegs (568) → calcFbbs (547): programs
//      regs[14/15] (top/tfbb) + regs[18/19] (bottom/bfbb); address stored >>5 with a
//      POFF page-offset flag in reg[14] bit12. VIFlush (793) latches → shdwRegs;
//      __VIRetraceHandler (162) → VISetRegs (144) writes shdwRegs → __VIRegs at a
//      field boundary.
//    - TVideo::waitForRetrace only calls VISetNextFrameBuffer when cur!=next (H3):
//      in SMS single-buffer mode the address never changes → after frame 1 the call
//      is a NO-OP and the VI keeps scanning one buffer.
//  Dolphin VI  externals/dolphin/Source/Core/Core/HW/VideoInterface.cpp/.h
//    - m_xfb_info_top / m_xfb_info_bottom (.h:426/427): live scan-out FBB registers,
//      programmed via VI MMIO VI_FB_LEFT_TOP_HI=0x1C/_LO=0x1E (top) and
//      VI_FB_LEFT_BOTTOM_HI=0x24/_LO=0x26 (bottom).
//    - GetXFBAddressTop/Bottom (.cpp:450/458): return FBB (<<5 if POFF).
//    - OutputField (.cpp:804): Odd field scans top; Even field scans bottom. Called
//      twice/game-frame (EndField, .cpp:880) → ViSwap (Present.cpp:178) → present ring.
//
// THE KEY LEVERAGE: Dolphin scans a SEPARATE register per field — top for odd, bottom
// for even. By programming top:=orig and bottom:=alt directly into the VI MMIO, the
// odd field scans `orig` and the even field scans `alt` deterministically — independent
// of the guest's cur==next skip (H3) and single-buffer aliasing (H1/H2). Two distinct
// frames per game tick reach the screen with no retrace-count juggling fighting the latch.
//
// ════════════════════════════════════════════════════════════════════════════
// ==== REQUIRED SEAM (apply manually — one call site in interp_redraw.cpp) ====
//   File: runtime/overrides/interp_redraw.cpp
//
//   Right AFTER the existing setNextXFB(alt) call (the block that computes `alt` and
//   does `call_ppc(cpu, VIDEO_SET_NEXT_XFB)` near line ~202, where `orig_fb` and `alt`
//   are in scope), add:
//
//           extern "C" void native_vi_set_field_pair(uint32_t orig, uint32_t alt);
//           native_vi_set_field_pair(orig_fb, alt);   // no-op unless SUNBRIGHT_NATIVE_VI=1
//
//   Optionally, in the single-real-frame / skipped-insertion paths (menus already at
//   60fps, or when the in-between is skipped), add:
//
//           extern "C" void native_vi_set_single(uint32_t orig);
//           native_vi_set_single(orig_fb);
//
//   so the bottom register isn't left holding a stale alt. Both are no-ops when the
//   flag is off, so the seam is inert in the default build. This file LINKS without
//   the seam applied (the seam is only the call site that invokes these functions).
// ════════════════════════════════════════════════════════════════════════════
//
// ── HOW TO VERIFY (headless) ─────────────────────────────────────────────────
//  1. Reconfigure (new file): cmake -B build ... && cmake --build build -j.
//  2. Apply the seam.
//  3. SUNBRIGHT_HEADLESS=1 SUNBRIGHT_INTERP60=1 SUNBRIGHT_NATIVE_VI=1 SUNBRIGHT_PROBE=1 ./build/sunbright
//  4. curl http://127.0.0.1:17654/interp60 → "PRESENTED XFB addrs" must ALTERNATE
//     between orig and orig^0x400000 with NO "(dup)" markers (flag OFF may show a
//     repeated addr / dup = the RRBB cadence this owns away).
//  5. curl http://127.0.0.1:17654/nativevi for this layer's own accounting (pairs
//     programmed, fields, live top/bottom FBB; top!=bottom = two distinct fields).
//  6. Framedump count (SUNBRIGHT_DUMP=1) ≈ 2× the engine redraw count.
// ─────────────────────────────────────────────────────────────────────────────

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifdef HAVE_DOLPHIN_CORE

namespace {

// VI MMIO base + the four XFB-info register halfwords (Dolphin VideoInterface.h).
constexpr uint32_t VI_MMIO           = 0xCC002000u;
constexpr uint32_t VI_FB_LEFT_TOP_HI = 0x1Cu;   // top FBB high half + POFF/XOFF
constexpr uint32_t VI_FB_LEFT_TOP_LO = 0x1Eu;   // top FBB low half
constexpr uint32_t VI_FB_LEFT_BOT_HI = 0x24u;   // bottom FBB high half
constexpr uint32_t VI_FB_LEFT_BOT_LO = 0x26u;   // bottom FBB low half

bool native_vi_enabled() {
    static int v = -1;
    if (v < 0) v = getenv("SUNBRIGHT_NATIVE_VI") ? 1 : 0;
    return v == 1;
}

struct NativeViDbg {
    unsigned long pairs = 0;          // field-pairs programmed
    unsigned long single = 0;         // single-address programs (real-only frames)
    uint32_t last_top = 0, last_bot = 0;
    uint32_t last_orig = 0, last_alt = 0;
    unsigned long fields = 0;
} g_nvi;

// Encode a 32-aligned guest XFB virtual address into the VI FBB register pair the
// way the GC VI library does (vi.c calcFbbs/setFbbRegs): physical = addr & 0x3FFFFFFF,
// stored >>5 with POFF set (reg HI bit12). Dolphin's GetXFBAddressTop returns FBB<<5
// when POFF, recovering the physical address.
//   HI: bit12 = POFF(shifted), bits[11:0] = (fbb >> 16)   (XOFF=0)
//   LO: fbb & 0xFFFF
void program_fbb(uint32_t hi_off, uint32_t lo_off, uint32_t vaddr) {
    const uint32_t phys = vaddr & 0x3FFFFFFFu;
    const uint32_t fbb  = phys >> 5;                          // POFF=1 form
    const uint16_t lo   = (uint16_t)(fbb & 0xFFFFu);
    const uint16_t hi   = (uint16_t)((1u << 12) | ((fbb >> 16) & 0x0FFFu));
    sb_w16(VI_MMIO + lo_off, lo);
    sb_w16(VI_MMIO + hi_off, hi);
}

// Decode the live top/bottom FBB back to a virtual MEM1 address (probe/sanity).
uint32_t read_fbb(uint32_t hi_off, uint32_t lo_off) {
    const uint32_t hi = sb_r16(VI_MMIO + hi_off);
    const uint32_t lo = sb_r16(VI_MMIO + lo_off);
    uint32_t fbb = ((hi & 0x0FFFu) << 16) | lo;
    const bool poff = (hi & (1u << 12)) != 0;
    uint32_t phys = poff ? (fbb << 5) : fbb;
    return phys | 0x80000000u;
}

}  // namespace

// ── Public API consumed by interp_redraw.cpp (all no-ops unless the flag is on) ──

// odd  field (GetXFBAddressTop)    -> orig (the real frame)
// even field (GetXFBAddressBottom) -> alt  (the in-between)
extern "C" void native_vi_set_field_pair(uint32_t orig, uint32_t alt) {
    if (!native_vi_enabled()) return;
    program_fbb(VI_FB_LEFT_TOP_HI, VI_FB_LEFT_TOP_LO, orig);   // odd field
    program_fbb(VI_FB_LEFT_BOT_HI, VI_FB_LEFT_BOT_LO, alt);    // even field
    g_nvi.last_top = orig; g_nvi.last_bot = alt;
    g_nvi.last_orig = orig; g_nvi.last_alt = alt;
    g_nvi.pairs++;
}

// A frame with no in-between: scan the single real address on BOTH fields so the next
// pair-program isn't left with a stale alt on the bottom register.
extern "C" void native_vi_set_single(uint32_t orig) {
    if (!native_vi_enabled()) return;
    program_fbb(VI_FB_LEFT_TOP_HI, VI_FB_LEFT_TOP_LO, orig);
    program_fbb(VI_FB_LEFT_BOT_HI, VI_FB_LEFT_BOT_LO, orig);
    g_nvi.last_top = g_nvi.last_bot = orig;
    g_nvi.single++;
}

extern "C" void native_vi_note_field() { if (native_vi_enabled()) g_nvi.fields++; }
extern "C" bool native_vi_active() { return native_vi_enabled(); }

// ── /nativevi probe backing (OPTIONAL secondary seam in probe_server.cpp) ─────
extern "C" int native_vi_probe(char* out, int cap, const char* /*query*/) {
    int n = 0;
    auto app = [&](const char* fmt, auto... a){ if (n < cap) n += snprintf(out+n, cap-n, fmt, a...); };
    app("native_vi enabled=%d\n", (int)native_vi_enabled());
    app("pairs=%lu single=%lu fields=%lu\n", g_nvi.pairs, g_nvi.single, g_nvi.fields);
    app("last programmed: top(odd)=%08x bottom(even)=%08x  (orig=%08x alt=%08x)\n",
        g_nvi.last_top, g_nvi.last_bot, g_nvi.last_orig, g_nvi.last_alt);
    app("live VI FBB regs: top=%08x bottom=%08x  %s\n",
        read_fbb(VI_FB_LEFT_TOP_HI, VI_FB_LEFT_TOP_LO),
        read_fbb(VI_FB_LEFT_BOT_HI, VI_FB_LEFT_BOT_LO),
        read_fbb(VI_FB_LEFT_TOP_HI, VI_FB_LEFT_TOP_LO) != read_fbb(VI_FB_LEFT_BOT_HI, VI_FB_LEFT_BOT_LO)
            ? "(top != bottom: two distinct fields -> 60fps cadence)"
            : "(top == bottom: single buffer / not split)");
    return n;
}

#else  // !HAVE_DOLPHIN_CORE — keep the symbols so interp_redraw links offline.

extern "C" void native_vi_set_field_pair(uint32_t, uint32_t) {}
extern "C" void native_vi_set_single(uint32_t) {}
extern "C" void native_vi_note_field() {}
extern "C" bool native_vi_active() { return false; }
extern "C" int  native_vi_probe(char* out, int cap, const char*) {
    return out && cap > 0 ? snprintf(out, cap, "native_vi: no dolphin core\n") : 0;
}

#endif  // HAVE_DOLPHIN_CORE
