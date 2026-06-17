// Native per-field EFB-copy textures for true 60fps screen-space effects (SUNBRIGHT_EFB_NATIVE).
//
// RE write-up: docs/re_notes/efb_native_60fps.md (read it — addresses, offsets, alt-address
// justification, and the EXACT interp_redraw.cpp integration edits live there).
//
// THE PROBLEM (confirmed, docs/re_notes/efb_dynamic_texture_chain.md):
//   Every JDrama::TEfbCtrlTex copies the EFB into a texture at a FIXED guest address held in
//   TEfbCtrlTex+0x2C (mImagePtr) via GXCopyTex in its copy phase (param_1 & 0x8). The screen-space
//   consumers (plaza water refraction, mirror, dash blur, shimmer, underwater filter, graffiti
//   coverage) bind THAT SAME address through a GXTexObj and sample it. Dolphin keys EFB-copy
//   textures by their guest address. On the 60fps in-between field the copy re-runs against the
//   INTERPOLATED-camera EFB and OVERWRITES the real field's texture at the same address -> the
//   consumer flickers every other field.
//
//   The in-between IS a valid interpolated-camera render (interp60 blends the draw matrices), so a
//   per-field copy is a CORRECT per-field frame. The ONLY bug is the shared destination address.
//
// THE NATIVE FIX (this file) — the proven XFB alt-address pattern generalized:
//   Give the in-between field its OWN per-field EFB-copy texture. For the duration of the
//   in-between draw, redirect BOTH halves of the chain to an ALT address:
//     (1) the COPY destination  — intercept GXCopyTex(dest=mImagePtr) and swap dest -> ALT, AND
//     (2) the CONSUMER's read    — intercept GXLoadTexObjPreLoaded and, for any GXTexObj whose
//                                  encoded image base == a tracked orig mImagePtr, swap its
//                                  image3 to the ALT-encoded base for that one load.
//   Real field uses orig; in-between uses alt; Dolphin's texture cache keeps them as two distinct
//   textures -> no overwrite, no ghost, true 60fps. This is NOT strategy b1 (skip/freeze the copy)
//   — the copy and the consumer BOTH run on the in-between, against the in-between's own EFB.
//
// WHY INTERCEPT THE GX FUNNELS instead of rewriting TEfbCtrlTex+0x2C / resolving each consumer
// GXTexObj statically:
//   - The copy dest fed to Dolphin comes from GXCopyTex's `dest` arg (GXFrameBuf.c: phyAddr =
//     dest & 0x3FFFFFFF), NOT from a persistent field we'd have to mutate+restore. Swapping the
//     register-bound GXCopyTex arg is fully transient (no guest state changed, no restore needed).
//   - EVERY consumer binds its texture through GXLoadTexObj -> GXLoadTexObjPreLoaded (the single
//     register writer). Matching the GXTexObj's encoded image base against the tracked orig set
//     redirects ALL consumers (water/dash/shimmer/underwater filter share ONE JUTTexture for the
//     2a screen tex; mirror uses its cam GXTexObj; graffiti uses the ResTIMG GXTexObj) with no
//     per-instance global resolution and no risk of missing a consumer we didn't enumerate.
//   - It avoids registering a third override on 0x802f8bac (TEfbCtrlTex::perform) — already owned
//     by interp_redraw.cpp (ov_efbctrltex_perform) AND efbtex_widescreen.cpp (ov_efbtex_perform_ws).
//
// ALT-ADDRESS JUSTIFICATION (docs §3): ALT = orig ^ 0x00400000 (the proven XFB offset).
//   With EFBToTextureEnable=true (Dolphin default; GFX_HACK_SKIP_EFB_COPY_TO_RAM) and copy-to-VRAM
//   supported (Vulkan/OGL), non-XFB EFB copies are copy_to_ram=false -> they write NO guest RAM,
//   they are pure VRAM texture-cache entries keyed by address (TextureCacheBase.cpp:2195). So ALT
//   only forms a distinct cache key; it never overwrites live RAM. 0x400000>>5 = 0x20000 fits the
//   21-bit GC image-base field and the 24-bit Dolphin field, and XOR keeps the address inside MEM1.
//   ⚠ If EFB-copy-to-RAM is ever enabled, ALT must instead be a reserved scratch address (the
//   copy would memcpy `covered_range` bytes there) — see docs §3.

#include "../overrides.h"
#include "../intrinsics.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" void func_8035ee5c(CPUState&);   // GXCopyTex(void* dest, GXBool clear)
extern "C" void func_8035ffb8(CPUState&);   // GXLoadTexObjPreLoaded(GXTexObj*, GXTexRegion*, GXTexMapID)
extern void ngx_note_efb_copy(bool is_disp, u32 dest, u32 clear);   // ngx_j3d_shape.cpp: epoch tracking

namespace {

constexpr u32 GX_COPY_TEX            = 0x8035ee5cu;
constexpr u32 GX_LOAD_TEXOBJ_PRELOAD = 0x8035ffb8u;

// GXTexObj internal layout (reference/sms src/dolphin/gx/GXTexture.c __GXTexObjInt):
//   +0x00 mode0  +0x04 mode1  +0x08 image0  +0x0C image3  ...
// image3 low 21 bits = (image_ptr >> 5) & 0x1FFFFF (GXInitTexObj SET_REG_FIELD(0x24A,...,21,0)).
// Dolphin reads a 24-bit image_base and forms addr = image_base << 5 (BPMemory.h TexImage3,
// TextureInfo.cpp:30). The top byte (bits 31..24) holds the BP-register id (GXLoadTexObjPreLoaded
// SET_REG_FIELD(0x408,...,8,24,...)) — we MUST preserve it.
constexpr u32 GXTEXOBJ_IMAGE3_OFF = 0x0Cu;
constexpr u32 IMG_BASE_MASK       = 0x001FFFFFu;   // 21-bit GC image-base field (low bits of image3)

constexpr u32 ALT_XOR = 0x00400000u;               // proven XFB alt-address offset (4 MB)

// Encode a guest byte address into the image-base field value (>> 5, masked to 21 bits).
inline u32 encode_base(u32 addr)  { return (addr >> 5) & IMG_BASE_MASK; }

// Default ON (the goal); toggle OFF with SUNBRIGHT_EFB_NATIVE=0 for A/B.
bool efb_native_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("SUNBRIGHT_EFB_NATIVE");
        v = (e && e[0] == '0' && e[1] == '\0') ? 0 : 1;   // default ON; only "0" disables
    }
    return v == 1;
}

// Tracked set of orig copy-dest addresses (one per live TEfbCtrlTex copy this frame). Small and
// bounded (screen tex, optional mirror, N graffiti layers). Encoded image-base form is cached so
// the GXLoadTexObjPreLoaded matcher is a cheap compare (no per-load >>5).
struct Tracked {
    u32  orig;        // mImagePtr (byte address) for this copy
    u32  orig_base;   // encode_base(orig)
    bool alt_live;    // this in-between actually re-ran the copy to ALT (so consumers may read ALT)
};
constexpr int kMaxTracked = 32;
Tracked g_tracked[kMaxTracked];
int     g_tracked_n = 0;

// True only while the in-between field is being drawn (set by begin/end below).
bool g_inbetween = false;

// ── live counters (read via sb_efb_native_status / a probe) ──
unsigned long g_copies_redirected = 0;   // GXCopyTex dests swapped to ALT on the in-between
unsigned long g_loads_redirected  = 0;   // consumer GXTexObj image3 swapped to ALT on the in-between
unsigned long g_tracked_seen      = 0;   // distinct real-field copy dests recorded

// Record an EFB-copy destination address into the tracked set (idempotent, dedup by address).
inline void track_dest(u32 dest) {
    if (dest < 0x80000000u) return;                 // null / non-RAM (graffito-check stays null)
    for (int i = 0; i < g_tracked_n; i++) if (g_tracked[i].orig == dest) return;
    if (g_tracked_n >= kMaxTracked) return;
    g_tracked[g_tracked_n].orig      = dest;
    g_tracked[g_tracked_n].orig_base = encode_base(dest);
    g_tracked[g_tracked_n].alt_live  = false;
    g_tracked_n++;
    g_tracked_seen++;
}

}  // namespace

// ── C-ABI integration helpers (called from interp_redraw.cpp; see docs §4) ──────────────────────

// Optional precise hook (NOT required for integration): record a specific TEfbCtrlTex's copy
// destination (mImagePtr @ +0x2C). The integration instead lets ov_efb_native_copytex SELF-TRACK
// every real-field GXCopyTex dest, which needs no call into the contested 0x802f8bac override.
extern "C" void sb_efb_native_track_copy(u32 tefbctrltex_ptr) {
    if (!efb_native_enabled() || !tefbctrltex_ptr) return;
    track_dest(MEM_R32(tefbctrltex_ptr + 0x2Cu));   // mImagePtr
}

// Begin the in-between window: arm the GXCopyTex / GXLoadTexObjPreLoaded redirects to ALT.
// Call IMMEDIATELY BEFORE the kDrawLists re-issue loop in ov_interp_endRendering. The tracked set
// was just (re)populated by this displayed pair's REAL field via ov_efb_native_copytex self-track.
extern "C" void sb_efb_native_begin_inbetween() {
    if (!efb_native_enabled()) return;
    for (int i = 0; i < g_tracked_n; i++) g_tracked[i].alt_live = false;
    g_inbetween = true;
}

// End the in-between window: disarm the redirects and clear the tracked set so the NEXT real field
// rebuilds it from its own live copies (addresses can change across scenes).
extern "C" void sb_efb_native_end_inbetween() {
    g_inbetween = false;
    g_tracked_n = 0;
}

extern "C" int sb_efb_native_status(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    return snprintf(out, cap,
        "efb_native: enabled=%d (SUNBRIGHT_EFB_NATIVE; default ON) tracked=%d "
        "copies_redirected=%lu loads_redirected=%lu track_calls=%lu alt_xor=0x%X\n",
        (int)efb_native_enabled(), g_tracked_n,
        g_copies_redirected, g_loads_redirected, g_tracked_seen, ALT_XOR);
}

// ── GX funnel interceptors (the redirect seams) ─────────────────────────────────────────────────

// GXCopyTex(void* dest, GXBool clear): on the in-between, if `dest` is a tracked orig copy-dest,
// swap it to ALT so the in-between's EFB resolve lands in its own texture-cache slot. Transient:
// only gpr[3] (the arg in a register) is changed for this one call; no guest state mutated.
SUNBRIGHT_OVERRIDE(ov_efb_native_copytex, GX_COPY_TEX) {
    if (efb_native_enabled()) {
        const u32 dest = cpu.gpr[3];
        if (!g_inbetween) {
            track_dest(dest);                       // real field: learn the live EFB-copy dests
        } else {
            for (int i = 0; i < g_tracked_n; i++) {
                if (g_tracked[i].orig == dest) {
                    cpu.gpr[3] = dest ^ ALT_XOR;    // in-between: resolve into our own per-field slot
                    g_tracked[i].alt_live = true;   // and let consumers of this base read ALT now
                    g_copies_redirected++;
                    break;
                }
            }
        }
    }
    ngx_note_efb_copy(/*is_disp=*/false, cpu.gpr[3], cpu.gpr[4]);   // → OFFSCREEN texture
    func_8035ee5c(cpu);
}

// GXLoadTexObjPreLoaded(GXTexObj* obj, GXTexRegion*, GXTexMapID): on the in-between, if obj's
// encoded image base matches a tracked orig, rewrite obj->image3 to the ALT base for this load
// (the body writes image3 to the BP register), then restore it so the guest GXTexObj is unchanged
// for the next real field. Preserves bits[31:21] (the BP-register id byte).
SUNBRIGHT_OVERRIDE(ov_efb_native_loadtexobj, GX_LOAD_TEXOBJ_PRELOAD) {
    if (g_inbetween && efb_native_enabled() && g_tracked_n) {
        const u32 obj = cpu.gpr[3];
        if (obj >= 0x80000000u) {
            const u32 i3   = MEM_R32(obj + GXTEXOBJ_IMAGE3_OFF);
            const u32 base = i3 & IMG_BASE_MASK;
            // Only redirect if THIS in-between actually re-copied this texture to ALT (alt_live);
            // otherwise a texture not re-copied this field would read an empty ALT slot. When it
            // wasn't re-copied, the real field's content at orig is still current — read that.
            bool live = false;
            for (int i = 0; i < g_tracked_n; i++)
                if (g_tracked[i].orig_base == base && g_tracked[i].alt_live) { live = true; break; }
            if (live) {
                const u32 alt_base = (base ^ encode_base(ALT_XOR)) & IMG_BASE_MASK;
                const u32 i3_alt   = (i3 & ~IMG_BASE_MASK) | alt_base;   // keep bits[31:21]
                MEM_W32(obj + GXTEXOBJ_IMAGE3_OFF, i3_alt);
                func_8035ffb8(cpu);                                      // body sends ALT to the GPU
                MEM_W32(obj + GXTEXOBJ_IMAGE3_OFF, i3);                  // restore for real fields
                g_loads_redirected++;
                return;
            }
        }
    }
    func_8035ffb8(cpu);
}
