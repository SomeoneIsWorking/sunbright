// perflist_dump.cpp — LIVE TPerformList walker for the marukage (Mario drop-shadow)
// 60fps in-between flicker investigation. RUNTIME GROUND TRUTH, not decomp reasoning.
//
// WHY THIS EXISTS
// ---------------
// The marukage on/off blink (visible on real fields 2n, absent on in-between 2n+1) cannot
// be settled from the decomp alone: the perform-list MEMBERSHIP and ORDER live in the
// data-driven binary /data/PerformLists.bin, and the shadow's visibility depends on RUNTIME
// GX state. Every pure-decomp theory (projection-swim, masked-&0x1, unk48 gate-straddle) has
// FAILED when tested — the user confirms the bug is unchanged. So we walk the LIVE lists.
//
// This file is SELF-CONTAINED, OPT-IN, and INERT by default:
//   - It registers NO override and hooks NOTHING. It only exposes a function the probe
//     server calls on demand (see "REQUIRED SEAM" below).
//   - It reads guest memory through sb_r32/sb_rf32 (inline, always linkable — intrinsics.h).
//   - Symbol resolution (vtable addr -> class name) is OPTIONAL, behind a function pointer
//     the probe server may install. With no resolver installed it prints raw hex — so this
//     TU links standalone with NO undefined references.
//
// WHAT IT ANSWERS
// ---------------
//   * WHICH perform list (+0x1C..+0x48 off gpMarDirector) contains TSilhouette
//     (gpSilhouetteManager) and with what per-link filter mask.
//   * WHICH list/object draws the ground/map (and water) that CONSUMES texmtx 0x1e.
//   * The live TSilhouette state at dump time: unk48 (alpha chase), unk12.a (raster alpha),
//     unk40/unk44 (the two textures), so the main session can correlate with a GX trace.
//
// Build: it is picked up by the runtime CMake GLOB (a fresh `cmake -B build` reconfigure is
// required for any NEW file — file(GLOB) is configure-time). Syntax-checkable standalone with
// g++ -fsyntax-only (see the bottom-of-file note; the include search root is runtime/).

#include "../intrinsics.h"   // sb_r32 / sb_rf32 / MEM_R32 (inline host load+bswap)

#include <cstdio>
#include <cstdint>
#include <cstring>

namespace {

// ── Guest global addresses ───────────────────────────────────────────────────────────────
// These are from the GMSJ (JP) symbol map (reference/sms_gmsj01_symbols.txt); the running
// build is GMSE01 (US), so the addresses MAY differ by a small delta. gpMarDirector is the
// only one we strictly need and it is also tracked live by interp_redraw.cpp (g_mardir) — the
// probe accepts an explicit mardir override (see perflist_dump(u32)) so a wrong constant can
// be corrected at call time without a rebuild. gpSilhouetteManager is printed for cross-check
// only (we FIND the silhouette object by walking the lists, we do not trust this constant).
constexpr u32 GP_MARDIRECTOR_JP        = 0x8040A2A8u;  // TMarDirector*  (.sbss, JP)
constexpr u32 GP_SILHOUETTE_MGR_JP     = 0x8040A208u;  // TSilhouette*   (.sbss, JP)
constexpr u32 GP_MARIO_POS_JP          = 0x8040A39Cu;  // Vec*           (.sbss, JP)

// ── TMarDirector perform-list member offsets (reference/sms include/System/MarDirector.hpp;
//    matches interp_redraw.cpp's g_mardir + 0x1C .. +0x40 indexing). ────────────────────────
struct ListSlot { u32 off; const char* name; };
constexpr ListSlot kListSlots[] = {
    { 0x1C, "mPerformListGX        (mirror pass)" },
    { 0x20, "mPerformListSilhouette(SILHOUETTE)" },
    { 0x24, "mPerformListGXPost    (post/2D/HUD)" },
    { 0x28, "mPerformListMovement  (CALC)" },
    { 0x2C, "mPerformListCalcAnim  (CALC)" },
    { 0x30, "unk30                 (2D group CALC)" },
    { 0x34, "unk34                 (MAIN 3D ENTRY/preEntry)" },
    { 0x38, "unk38                 (graffiti EFB)" },
    { 0x3C, "unk3C                 (graffiti EFB)" },
    { 0x40, "unk40                 (drawbuf/light pre-pass)" },
    { 0x44, "mShinePfLstMov        (CALC)" },
    { 0x48, "mShinePfLstAnm        (CALC)" },
};

// ── TPerformList in-memory layout (derived from the decomp, verified against the headers) ───
// TPerformList : public TViewObj, public TSingleLinkList<TPerformLink,0>
//   TViewObj : TNameRef { vtable@0x0; mName@0x4; mKeyCode@0x8 (u16); unkC@0xC (TFlagT<u16>) }
//     -> TViewObj sub-object spans 0x0..0xF (size 0x10).
//   TSingleNodeLinkList base then begins at +0x10: unk0(int)@0x10, unk4(node mNext)@0x14,
//     unk8(node*)@0x18.   begin() = &unk4.mNext  ==>  first link node ptr = *(list + 0x14).
//
// TPerformLink { TSingleLinkListNode unk0@0x0 (mNext); TViewObj* unk4@0x4; u32 unk8@0x8 }.
//   Walk: node = *(list+0x14); per node: obj=*(node+0x4), filter=*(node+0x8),
//         next=*(node+0x0); stop at next==0.
constexpr u32 LIST_FIRST_NODE_OFF = 0x14;  // &unk4.mNext, i.e. begin()
constexpr u32 LINK_NEXT_OFF       = 0x00;  // TSingleLinkListNode.mNext
constexpr u32 LINK_OBJ_OFF        = 0x04;  // TPerformLink.unk4 (TViewObj*)
constexpr u32 LINK_FILTER_OFF     = 0x08;  // TPerformLink.unk8 (filter mask)

// TViewObj object layout (for each member object): vtable@0x0, unkC (disable mask)@0xC.
constexpr u32 OBJ_VTABLE_OFF      = 0x00;
constexpr u32 OBJ_NAME_OFF        = 0x04;  // TNameRef.mName (const char*) — JP-name C string
constexpr u32 OBJ_UNKC_OFF        = 0x0C;  // TViewObj.unkC (TFlagT<u16> disable mask), low 16b

// ── TSilhouette member offsets (reference/sms include/MarioUtil/DrawUtil.hpp) ───────────────
constexpr u32 SIL_UNK12_OFF       = 0x12;  // GXColor unk12 ; .a (raster alpha) = +0x12+3 = 0x15
constexpr u32 SIL_UNK12_A_OFF     = 0x15;  // unk12.a byte (= unk48, refreshed only in &0x1)
constexpr u32 SIL_UNK40_OFF       = 0x40;  // JUTTexture* unk40 (ground/pollution tex -> TEXMAP0)
constexpr u32 SIL_UNK44_OFF       = 0x44;  // JUTTexture* unk44 (H_marukage_xlu_i8 -> TEXMAP1)
constexpr u32 SIL_UNK48_OFF       = 0x48;  // f32 unk48 (occluded-alpha chase; the gate value)

inline bool ram_ok(u32 ea) { return ea >= 0x80000000u && ea < 0x81800000u; }

// ── Optional symbol resolver (vtable addr -> class/function name). The probe server installs
//    its own sym() (reference/sms_gmse01_funcs.txt loader). NULL => print raw hex. This keeps
//    the TU free of undefined references so it links standalone. ─────────────────────────────
using SymResolver = const char* (*)(u32 addr);
SymResolver g_sym = nullptr;

const char* resolve(u32 addr, char* scratch, size_t n) {
    if (g_sym) {
        const char* s = g_sym(addr);
        if (s && *s) return s;
    }
    snprintf(scratch, n, "%08x", addr);
    return scratch;
}

// Read a guest C string (TNameRef.mName) into a host buffer. The decomp pushes the JP object
// names ("水マネージャ" etc.) so this is best-effort; non-ASCII is shown as '.'.
void read_gstr(u32 ea, char* out, size_t n) {
    size_t i = 0;
    if (!ram_ok(ea)) { snprintf(out, n, "(badptr %08x)", ea); return; }
    for (; i + 1 < n; i++) {
        u8 c = sb_r8(ea + (u32)i);
        if (c == 0) break;
        out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    out[i] = 0;
}

}  // namespace

// ============================================================================================
// perflist_dump — walk every TMarDirector perform list, print each link's object + filter.
//   mardir_override: pass the live g_mardir from interp_redraw.cpp (preferred). 0 => use the
//                    JP constant GP_MARDIRECTOR_JP (may be wrong on GMSE — see note above).
//   Returns the number of bytes written to `out`.
// ============================================================================================
extern "C" int perflist_dump(char* out, int cap, u32 mardir_override) {
    int n = 0;
    auto app = [&](auto&&... a) {
        if (n < cap) n += snprintf(out + n, (size_t)(cap - n), a...);
    };
    char sb1[256], sb2[256], nm[96];

    const u32 mardir = mardir_override ? mardir_override
                                       : (ram_ok(GP_MARDIRECTOR_JP) ? sb_r32(GP_MARDIRECTOR_JP) : 0);

    app("==== perflist_dump ====\n");
    app("gpMarDirector = %08x  (source: %s)\n", mardir,
        mardir_override ? "live g_mardir (interp_redraw)" : "JP .sbss constant — VERIFY on GMSE");

    const u32 sil_mgr = ram_ok(GP_SILHOUETTE_MGR_JP) ? sb_r32(GP_SILHOUETTE_MGR_JP) : 0;
    app("gpSilhouetteManager(JP const) = %08x\n", sil_mgr);
    if (ram_ok(sil_mgr)) {
        const float unk48 = sb_rf32(sil_mgr + SIL_UNK48_OFF);
        const u8 unk12a   = sb_r8(sil_mgr + SIL_UNK12_A_OFF);
        const u32 tex40   = sb_r32(sil_mgr + SIL_UNK40_OFF);
        const u32 tex44   = sb_r32(sil_mgr + SIL_UNK44_OFF);
        const u32 vt      = sb_r32(sil_mgr + OBJ_VTABLE_OFF);
        app("  TSilhouette: vtable=%s  unk48(alpha-chase/gate)=%.5f  unk12.a(raster)=%u\n",
            resolve(vt, sb1, sizeof sb1), unk48, unk12a);
        app("  TSilhouette: unk40(ground tex)=%08x  unk44(marukage tex)=%08x\n", tex40, tex44);
    } else {
        app("  (gpSilhouetteManager const invalid on this build — find it via the walk below)\n");
    }

    if (!ram_ok(mardir)) {
        app("\n!! mardir invalid; pass live g_mardir via the /perflists seam.\n");
        return n;
    }

    app("\n-- perform lists (object | vtable->class | filter | unkC-disable) --\n");
    for (const auto& slot : kListSlots) {
        const u32 list = sb_r32(mardir + slot.off);
        app("\n[+0x%02x] %s  list=%08x\n", slot.off, slot.name, list);
        if (!ram_ok(list)) { app("    (null/invalid list)\n"); continue; }

        u32 node = sb_r32(list + LIST_FIRST_NODE_OFF);
        int guard = 0, idx = 0;
        while (ram_ok(node) && guard++ < 4096) {
            const u32 obj    = sb_r32(node + LINK_OBJ_OFF);
            const u32 filter = sb_r32(node + LINK_FILTER_OFF);
            const u32 next   = sb_r32(node + LINK_NEXT_OFF);

            if (ram_ok(obj)) {
                const u32 vt = sb_r32(obj + OBJ_VTABLE_OFF);
                const u32 namep = sb_r32(obj + OBJ_NAME_OFF);
                const u16 unkc  = sb_r16(obj + OBJ_UNKC_OFF);
                read_gstr(namep, nm, sizeof nm);
                const bool is_sil = (obj == sil_mgr);
                app("    [%2d] obj=%08x  %-44s  filter=%08x  unkC=%04x  name=\"%s\"%s\n",
                    idx, obj, resolve(vt, sb2, sizeof sb2), filter, unkc, nm,
                    is_sil ? "   <== gpSilhouetteManager (TSilhouette)" : "");
            } else {
                app("    [%2d] obj=%08x (invalid)\n", idx, obj);
            }
            idx++;
            node = next;
        }
        if (idx == 0) app("    (empty)\n");
    }

    app("\n-- NEXT STEP --\n");
    app("Identify the list holding TSilhouette (the <== row), then note the link DIRECTLY\n");
    app("AFTER it in the SAME list with a 0x8/0x10/0x200/0x204 draw filter: that object is the\n");
    app("ground/map receiver that consumes texmtx 0x1e. Cross-check its vtable->class, then GX-\n");
    app("trace whether its draw RUNS on the in-between (see docs/re_notes/marukage_runtime_findings.md).\n");
    return n;
}

// Install the probe server's symbol resolver (addr -> name). Optional; call once at startup.
extern "C" void perflist_set_sym_resolver(const char* (*fn)(u32)) { g_sym = fn; }

// ============================================================================================
// ==== REQUIRED SEAM ====
// Add to runtime/probe_server.cpp handle_repl(), as ONE endpoint (do NOT edit anything else):
//
//   extern "C" int  perflist_dump(char* out, int cap, unsigned mardir);
//   extern "C" void perflist_set_sym_resolver(const char*(*)(unsigned));
//   extern unsigned g_mardir;   // defined in interp_redraw.cpp (live TMarDirector*)
//
//   if (strncmp(path, "/perflists", 10) == 0) {
//       static bool once = false;
//       if (!once) { once = true; perflist_set_sym_resolver([](unsigned a){
//           static thread_local std::string s; s = sym(a); return s.c_str(); }); }
//       static thread_local char pl[32768];
//       int w = perflist_dump(pl, sizeof pl, g_mardir);   // live g_mardir; 0 falls back to const
//       return std::string(pl, (size_t)w);
//   }
//
// Then:  curl 'http://127.0.0.1:17654/perflists'
// (sym() and the symtab loader already exist in probe_server.cpp; g_mardir is a file-scope
//  global in interp_redraw.cpp — declare it extern as shown. No other file changes.)
// ============================================================================================
//
// Standalone syntax check (from repo root):
//   g++ -std=c++17 -fsyntax-only -I runtime -I externals/dolphin/Source/Core \
//       runtime/overrides/perflist_dump.cpp
// (intrinsics.h is the only project include; it pulls Dolphin headers for the slow-path decls.)
