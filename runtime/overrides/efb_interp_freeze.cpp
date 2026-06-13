// EFB dynamic-texture freeze for 60fps interpolation — CANDIDATE, DEFAULT OFF.
//
// RE write-up: docs/re_notes/efb_dynamic_texture_chain.md.
//
// THE BUG (confirmed): every JDrama::TEfbCtrlTex copies the EFB into a texture at a FIXED
// guest address held in TEfbCtrlTex+0x2C (mImagePtr) via GXCopyTex in its copy phase
// (param_1 & 0x8). Instances: the plaza "通常シーン描画ステージ" screen texture (RGB565,
// half-res), the "鏡描画ステージ" mirror texture, and the per-pollution-layer "graffito"
// coverage textures (R8). The copy phase lives in perform lists the 60fps in-between RE-ISSUES
// (mPerformListGX +0x1C, graffiti unk38/unk3C +0x38/+0x3C). On the in-between the copy re-runs
// against the INTERPOLATED EFB and overwrites the real field's texture at the same fixed
// address — Dolphin keys EFB-copy textures by address — so the screen-space consumers
// (water refraction, mirror, dash blur, shimmer) flicker every other field. This is the same
// class as the fixed 30fps single-XFB-address bug.
//
// THE FIX (strategy b1, docs/re_notes/interp_screenspace_strategy.md): on the in-between, hold
// the EFB-feedback layer at its native 30fps — do NOT re-resolve the copy (drop the & 0x8 bit)
// so the fixed-address texture survives as the real field's content; and hold the consumer
// surface (water/mirror) at tick N so the frozen source and the frozen surface agree (no ghost).
//
// WHY THIS FILE DOES NOT REGISTER AN OVERRIDE ON 0x802f8bac:
//   runtime/overrides/interp_redraw.cpp ALREADY owns TEfbCtrlTex::perform (0x802f8bac) via
//   ov_efbctrltex_perform, which drops the & 0x8 copy when g_i60.skip_efbcopy is set while the
//   in-between is being drawn. register_override() on the same address would REPLACE that seam.
//   So, to avoid clobbering existing working code and to keep this deliverable non-destructive,
//   this file does NOT register a second override on that address. It instead exposes:
//     (1) a single env gate `SUNBRIGHT_EFB_INTERP_FREEZE` (default OFF), and
//     (2) `sb_efb_interp_freeze_wanted()` — a query the in-between path could consult to make
//         the existing skip_efbcopy default-on (the proper one-line flip lives in interp60.h /
//         interp_redraw.cpp; see docs §8). Until that flip, this is inert.
//
// To ACTUALLY enable the fix (proper, in-tree): set interp60.h `skip_efbcopy` default 0 -> 1
//   (drops the & 0x8 copy on the in-between for all TEfbCtrlTex), then add the water/mirror
//   no-blend tag in interp_capture.cpp. See docs/re_notes/efb_dynamic_texture_chain.md §6, §8.
//   This file is the build-verified reference of the mechanism, not the activation switch.

#include "../overrides.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

// Default OFF. Returns true only if SUNBRIGHT_EFB_INTERP_FREEZE is set to a non-"0" value.
extern "C" bool sb_efb_interp_freeze_wanted() {
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("SUNBRIGHT_EFB_INTERP_FREEZE");
        v = (e && e[0] && !(e[0] == '0' && e[1] == '\0')) ? 1 : 0;
    }
    return v == 1;
}

// Offset of TEfbCtrlTex::mImagePtr, documented here so any future seam reads it from one place
// (header reference/sms include/JSystem/JDrama/JDREfbCtrl.hpp; disasm-confirmed at 0x802f8cf8
// `lwz r0,0x2C(r29)`).
extern "C" unsigned sb_efbctrltex_mimageptr_off() { return 0x2Cu; }

// Small probe string for /help-style reporting (no behavior change). Kept tiny.
extern "C" int sb_efb_interp_freeze_status(char* out, int cap) {
    if (!out || cap <= 0) return 0;
    return snprintf(out, cap,
        "efb_interp_freeze: wanted=%d (SUNBRIGHT_EFB_INTERP_FREEZE; default OFF) "
        "mImagePtr_off=0x%X — see docs/re_notes/efb_dynamic_texture_chain.md\n",
        (int)sb_efb_interp_freeze_wanted(), sb_efbctrltex_mimageptr_off());
}
