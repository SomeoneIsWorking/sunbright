#pragma once

// ───────────────────────────────────────────────────────────────────────────
// Pure, Dolphin/GX-free extraction of the per-vertex skin-matrix BOUNDS CHECK used by
// sb_boot_capture_j3d (native/render/sms_boot_j3d_capture.cpp) to clamp a GX matrix-slot
// index into the shape's draw-matrix table.
//
// REGRESSION this guards (found + fixed 2026-07-01, commit 32a03fa — the file-select
// mangled-Mario bug): the bound MUST come from the CAPTURED SHAPE's own draw-matrix-table
// descriptor (J3DShape::mDrawMtxData, bound once per-shape at model-init via
// setDrawMtxDataPointer), never from j3dSys.getModel()'s table size. j3dSys.getModel() is
// whichever J3DModel's calc/entry/viewCalc ran MOST RECENTLY (every J3DModel entry point
// calls j3dSys.setModel(this)) — NOT necessarily the model owning the shape currently being
// captured, since draw() happens later at buffer-flush time. At the file-select picker,
// TMarioCap's cap sub-models run their own calc/entry INSIDE TMario::perform, AFTER the
// body's entryModels in the same call, so by the time ChrOpa/ChrXlu actually draw() (much
// later, GXPost pass), j3dSys.getModel() was the cap (draw-matrix table size 1) — using that
// as the bound silently clamped every body vertex needing skin index >=1 to slot 0, and
// every limb collapsed onto whichever joint matrix lived there.
// ───────────────────────────────────────────────────────────────────────────

namespace sb {

// The bound to clamp a per-vertex skin-matrix index against, given the CAPTURED SHAPE's own
// entry count (preferred, always correct for that shape) and a FALLBACK count (only used when
// the shape has no own table bound yet — matches sb_boot_capture_j3d's original behavior for
// that corner case).
inline int skin_drawmtx_bound(bool shape_has_own_table, int shape_own_entry_num,
                               int fallback_model_entry_num) {
    return shape_has_own_table ? shape_own_entry_num : fallback_model_entry_num;
}

// Clamp a GX skin-matrix slot index `di` (0xffff = "no override, use packet default") against
// `bound` (see skin_drawmtx_bound above), returning the resolved draw-matrix table index to use.
// Falls back to 0 both when di is the sentinel and when di is out of bounds — mirrors
// sb_boot_capture_j3d's mtx_for() exactly.
inline int resolve_skin_index(unsigned di, int bound) {
    const unsigned kNoOverride = 0xffffu;
    if (di != kNoOverride && (int)di < bound) return (int)di;
    return 0;
}

}  // namespace sb
