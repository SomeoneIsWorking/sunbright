#pragma once
// gxblend_summary.h — pure, Dolphin-free formatter for the per-draw BLEND/TEV value oracle.
//
// The Dolphin-GX command-stream oracle (gx_parse.cpp / gx_capture.cpp) records, per draw, the live
// GX pixel state (blend factors, TEV stage count, EFB pass, projection type). For the file-select
// overbright we need to read those as VALUES and diff them against the native renderer's per-batch
// blend (sms_boot_present batchdbg). A frame has ~1170 draws, so the raw list is unreadable; this
// unit run-length-collapses consecutive draws with identical pixel state into one line each, naming
// the GX blend factors. Kept here as a pure function so it is unit-tested Dolphin-free (render_test)
// and is the SAME code gx_capture.cpp ships (the test validates the shipping path, not a fork).

#include <cstdint>
#include <string>
#include <vector>
#include <cstdio>

namespace gxblend {

// One draw's GX pixel state (the subset the value oracle compares). Mirrors GxFrameInfo::DrawRec.
struct Draw {
    uint8_t efb_pass = 0;     // EFB-copy index this draw falls in (0 = pre-first-copy; POST = highest)
    uint8_t src = 1, dst = 0; // GX blend src/dst factor (0..7)
    uint8_t blend_enable = 0;
    uint8_t subtract = 0;
    uint8_t logic_enable = 0;
    uint8_t color_update = 1, alpha_update = 1;
    uint8_t numtevstages = 1; // BPMEM_GENMODE.numtevstages + 1
    uint8_t proj_type = 0;    // 0 perspective / 1 ortho
    uint32_t verts = 0;       // num_vertices of the primitive
};

// GX blend factor names (GXBlendFactor): index 0..7.
inline const char* factor_name(uint8_t f) {
    static const char* k[8] = {"ZERO","ONE","SRCCLR","INVSRCCLR","SRCALPHA","INVSRCALPHA","DSTALPHA","INVDSTALPHA"};
    return k[f & 7];
}

// Two draws collapse into one summary line iff their entire pixel state matches (verts accumulate).
inline bool same_state(const Draw& a, const Draw& b) {
    return a.efb_pass == b.efb_pass && a.src == b.src && a.dst == b.dst
        && a.blend_enable == b.blend_enable && a.subtract == b.subtract
        && a.logic_enable == b.logic_enable && a.numtevstages == b.numtevstages
        && a.color_update == b.color_update && a.alpha_update == b.alpha_update
        && a.proj_type == b.proj_type;
}

// The op label, in the same precedence GX uses (subtract overrides blend overrides logicop).
inline const char* op_name(const Draw& d) {
    return d.subtract ? "SUB" : (d.blend_enable ? "BLEND" : (d.logic_enable ? "LOGIC" : "NONE"));
}

// Format one collapsed group (a run of `count` identical-state draws summing `verts` vertices).
inline std::string format_group(const Draw& d, size_t count, uint32_t verts) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "pass%u %-5s %-11s/%-11s tev=%u %s%s proj=%s x%zu (%u verts)",
                  (unsigned)d.efb_pass, op_name(d), factor_name(d.src), factor_name(d.dst),
                  (unsigned)d.numtevstages, d.color_update ? "" : "[noC]", d.alpha_update ? "" : "[noA]",
                  d.proj_type ? "ortho" : "persp", count, verts);
    return std::string(buf);
}

// Run-length-collapse the draw list into one summary line per maximal run of identical pixel state.
inline std::vector<std::string> summarize(const std::vector<Draw>& draws) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < draws.size()) {
        const Draw& d = draws[i];
        size_t j = i + 1; uint32_t vsum = d.verts;
        while (j < draws.size() && same_state(draws[j], d)) { vsum += draws[j].verts; ++j; }
        out.push_back(format_group(d, j - i, vsum));
        i = j;
    }
    return out;
}

}  // namespace gxblend
