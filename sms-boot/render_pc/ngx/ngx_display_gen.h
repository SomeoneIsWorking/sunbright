// Clear-aware EFB display-generation model (pure, unit-tested in render_test `display_gen`).
//
// GPU truth: the EFB accumulates every draw and is RESET only by a clearing copy (GXCopyTex/CopyDisp
// with clear=1). A frame's displayed image is therefore everything drawn since the LAST clear — the
// "final generation". A generation counter increments AFTER each clearing copy, so the final
// generation equals the number of clearing copies in the frame, and that is what GX's last CopyDisp
// captures to the XFB.
//
// The present must show only batches whose generation == the display generation. The OLD heuristic
// ("display epoch = highest tex-closed epoch; show epoch >= that") was wrong when a small offscreen
// GXCopyTex happened AFTER the main scene's grab: it raised the display epoch and demoted the main
// scene out of the displayed set (the Sirena-beach black-screen bug — the 171-shape scene at gen 2
// was dropped because a later 8-shape pass closed epoch 3). Generation tracking is reset-aware and
// does not suffer this: the later non-clearing copy keeps the same generation.
#pragma once

// Final display generation of a frame = count of clearing copies (the incremental g_efb_gen the
// capture path arrives at). copy_clears[i] = did copy i clear the EFB?
inline int ngx_display_gen_for(const bool* copy_clears, int ncopies) {
    int gen = 0;
    for (int i = 0; i < ncopies; ++i) if (copy_clears[i]) gen++;
    return gen;
}

// Is a batch drawn under generation batch_gen part of the displayed image? display_gen < 0 means
// "no generation info this frame" (e.g. a scene drawn straight to the EFB with no copies) → show all.
inline bool ngx_batch_displayed(int batch_gen, int display_gen) {
    return display_gen < 0 || batch_gen == display_gen;
}
