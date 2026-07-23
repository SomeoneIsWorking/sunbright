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
#include "../runtime/scene.h"
#include "../runtime/j3d_decode.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <vector>

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
constexpr u32 SHAPE_MATRICES = 0x34;   // J3DShapeMtx**
constexpr u32 SHAPEMTX_SLOT  = 0x04;   // base J3DShapeMtx: u16 unk4 (vptr at +0x00)

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
    unsigned long layout_ok = 0, layout_fail = 0;
    unsigned long decoded = 0, decode_fail = 0;
    unsigned long tris = 0;
};
Stats g_st;
std::unordered_set<u32> g_seen;
J3DVertexLayout g_layout;
std::vector<J3DVert> g_tri;

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
                                        "skipped: no vertex data %lu, matrices not ready %lu\n"
                                        "scene: %d drawables last tick, %d matched the previous "
                                        "tick (interpolating)\n"
                                        "layout ok %lu / fail %lu\n"
                                        "elements decoded %lu / failed %lu -> %lu triangles\n",
                                        (int)capture_on(), g_st.shapes, g_st.distinct,
                                        g_st.elements, g_st.dl_bytes, g_st.no_verts, g_st.no_mtx,
                                        sbr_scene_last_count(), sbr_scene_matched_count(),
                                        g_st.layout_ok, g_st.layout_fail,
                                        g_st.decoded, g_st.decode_fail, g_st.tris);
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

        // One layout per shape (the descriptor and formats are per-shape, not per-element).
        if (j3d_build_layout(shape, g_layout)) ++g_st.layout_ok; else ++g_st.layout_fail;

        const u32 n = sb_r16(shape + SHAPE_ELEMENT_COUNT);
        const u32 draws = sb_r32(shape + SHAPE_DRAWS);
        // The live draw-matrix array for this view. drawMtxArray[i] is a 3x4 model x view matrix
        // (J3D concats the view in viewCalc), which is exactly what a drawable carries.
        const u32 viewNoPtr = sb_r32(shape + SHAPE_CURRENT_VIEW);
        const u32 viewNo = ok(viewNoPtr) ? sb_r32(viewNoPtr) : 0;
        const u32 drawMtxArray = (ok(mtx) && viewNo <= 16) ? sb_r32(mtx + 4 * viewNo) : 0;

        if (ok(draws) && n > 0 && n < 4096) {
            for (u32 i = 0; i < n; ++i) {
                const u32 d = sb_r32(draws + i * 4);
                if (!ok(d)) continue;
                ++g_st.elements;
                const u32 dl   = sb_r32(d + SHAPEDRAW_DL_PTR);
                const u32 size = sb_r32(d + SHAPEDRAW_DL_SIZE);
                if (ok(dl) && size > 0 && size < (16u << 20)) g_st.dl_bytes += size;

                // Decode the geometry. The vertex sizing is SELF-CHECKING: a wrong size lands
                // mid-payload and hits a non-primitive opcode, which j3d_decode_element reports
                // loudly rather than rendering garbage.
                if (g_layout.valid) {
                    g_tri.clear();
                    if (j3d_decode_element(shape, i, g_layout, g_tri)) {
                        ++g_st.decoded;
                        g_st.tris += g_tri.size() / 3;
                    } else {
                        ++g_st.decode_fail;
                    }
                }

                // Record the drawable for the interpolated scene. The element's matrix comes from
                // its J3DShapeMtx; for the base class every vertex uses one slot (unk4), which is
                // the common case and enough to carry the transform. The multi-matrix (skinned)
                // case resolves per-vertex and is handled when the geometry decode lands.
                if (drawMtxArray != 0) {
                    const u32 mtxObj = ok(sb_r32(shape + SHAPE_MATRICES))
                                           ? sb_r32(sb_r32(shape + SHAPE_MATRICES) + i * 4) : 0;
                    const u32 slot = ok(mtxObj) ? sb_r16(mtxObj + SHAPEMTX_SLOT) : 0;
                    // drawMtxArray is the Mtx* base; entries are inline f32[3][4] = 48 bytes.
                    const u32 mtxAddr = drawMtxArray + slot * 48;
                    if (ok(mtxAddr) && ok(mtxAddr + 47)) {
                        SbrDrawable dr{};
                        dr.key = ((uint64_t)shape << 16) | (uint64_t)i;
                        dr.geom = 0;   // decode lands next
                        for (int k = 0; k < 12; ++k) {
                            const u32 bits = sb_r32(mtxAddr + k * 4);
                            __builtin_memcpy(&dr.mtx[k], &bits, 4);
                        }
                        sbr_scene_add(dr);
                    }
                }
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
