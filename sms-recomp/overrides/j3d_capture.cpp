// j3d_capture.cpp — see the game's geometry at the J3D level, not as a GX command stream.
//
// DIRECTION (2026-07-23, user): render PC-native by overriding the game's RENDER CALLS rather than
// reimplementing GX. Tapping J3D gives meshes, materials and textures SEMANTICALLY — a shape with a
// vertex array, a display list and a material — instead of a fixed-function pixel pipeline we would
// have to rebuild (TEV combiners and all) just to arrive back where we started.
//
// The retired renderer took this same seam (git 9283f44^:native/render/sms_boot_j3d_capture.cpp,
// "tap J3DShape::draw and capture, per shape: geometry ... material"). The difference here: that ran
// in the DECOMP runtime where J3D objects are native C++. In the recomp they are guest memory, so
// every field is read at an RE'd offset — layouts below come from
// decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DShape.hpp, which is the structure ground truth.
//
// This first step CAPTURES AND REPORTS ONLY — it always runs the real draw, so the picture is
// unchanged and aurora stays the oracle. Decoding the geometry and rendering it natively comes next.
//
//   SBR_J3D_CAPTURE=1   record shapes; report at the probe's /j3d

#include "overrides.h"

#include "../runtime/probe_server.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>

extern "C" void func_802e0390(CPUState&);   // J3DShape::draw() const

namespace {

// J3DShape (J3DShape.hpp): the fields this seam needs.
constexpr u32 SHAPE_ELEMENT_COUNT = 0x06;   // u16
constexpr u32 SHAPE_GD_COMMANDS   = 0x28;   // the VCD/VAT setup display list
constexpr u32 SHAPE_DRAWS         = 0x38;   // J3DShapeDraw** , mElementCount entries
constexpr u32 SHAPE_VERTEX_DATA   = 0x44;   // J3DVertexData*
constexpr u32 SHAPE_DRAW_MATRICES = 0x50;   // Mtx**
constexpr u32 SHAPE_CURRENT_VIEW  = 0x58;   // u32*

// J3DShapeDraw: vtable at +0x00 (virtual dtor), then size, then the display list.
constexpr u32 SHAPEDRAW_DL_SIZE = 0x04;
constexpr u32 SHAPEDRAW_DL_PTR  = 0x08;

bool capture_on() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_J3D_CAPTURE");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

struct Stats {
    unsigned long shapes = 0;      // J3DShape::draw calls this frame-window
    unsigned long elements = 0;    // total mtx-group elements walked
    unsigned long dl_bytes = 0;    // total geometry display-list bytes seen
    unsigned long no_verts = 0;    // shapes with no vertex data
    unsigned long no_mtx = 0;      // shapes whose draw matrices are not ready
    unsigned long distinct = 0;    // distinct shape objects seen
};
Stats g_st;
std::unordered_set<u32> g_seen;

bool ok(u32 p) { return sb_ram_fast(p) != nullptr; }

const bool g_probe = [] {
    sb_probe_register("/j3d", "J3D shape capture: what the game draws, semantically",
                      [](const ProbeArgs&) {
                          char buf[512];
                          std::snprintf(buf, sizeof buf,
                                        "capture=%d\n"
                                        "shape draws      %lu\n"
                                        "distinct shapes  %lu\n"
                                        "elements         %lu\n"
                                        "geometry DL bytes %lu\n"
                                        "skipped: no vertex data %lu, matrices not ready %lu\n",
                                        (int)capture_on(), g_st.shapes, g_st.distinct,
                                        g_st.elements, g_st.dl_bytes, g_st.no_verts, g_st.no_mtx);
                          return std::string(buf);
                      });
    return true;
}();

void ov_shape_draw(CPUState& cpu) {
    const u32 shape = cpu.gpr[3];
    if (capture_on() && ok(shape)) {
        ++g_st.shapes;
        if (g_seen.insert(shape).second) ++g_st.distinct;

        const u32 verts = sb_r32(shape + SHAPE_VERTEX_DATA);
        const u32 mtx   = sb_r32(shape + SHAPE_DRAW_MATRICES);
        if (!ok(verts)) ++g_st.no_verts;
        // The owning model may not have been update()d yet (the decomp guards the same case), so a
        // null matrix array is expected early, not a bug.
        if (!ok(mtx)) ++g_st.no_mtx;

        const u32 n = sb_r16(shape + SHAPE_ELEMENT_COUNT);
        const u32 draws = sb_r32(shape + SHAPE_DRAWS);
        if (ok(draws) && n > 0 && n < 4096) {
            for (u32 i = 0; i < n; ++i) {
                const u32 d = sb_r32(draws + i * 4);
                if (!ok(d)) continue;
                ++g_st.elements;
                const u32 dl   = sb_r32(d + SHAPEDRAW_DL_PTR);
                const u32 size = sb_r32(d + SHAPEDRAW_DL_SIZE);
                if (ok(dl) && size > 0 && size < (16u << 20)) g_st.dl_bytes += size;
            }
        }
    }
    // Always run the real draw: this step only observes. The picture must be unchanged so aurora
    // stays a valid oracle while the native path is built.
    func_802e0390(cpu);
}

} // namespace

SB_OVERRIDE(0x802e0390u, ov_shape_draw, "J3DShape::draw",
            "native render: capture the game's geometry at the J3D level (observe only for now)")
