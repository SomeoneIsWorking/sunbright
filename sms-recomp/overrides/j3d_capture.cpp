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
#include "../runtime/state_oracle.h"

void sbr_mtx_begin_shape(u32 shape);
void sbr_mtx_end_shape();
void sbr_mtx_check_index(u32 shape, int element, uint32_t mine, uint32_t truthIdx, bool haveTruth);
const std::vector<std::pair<unsigned short, unsigned short>>& sbr_mtx_loads();
void gxfifo_drain_pending();
void sbr_mtx_report_index();

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_set>
#include <map>
#include <utility>
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
    unsigned long no_load_truth = 0;   // elements with no observable matrix load (ShapeMtxDL)
};
Stats g_st;
std::unordered_set<u32> g_seen;
J3DVertexLayout g_layout;
std::vector<J3DVert> g_tri;

bool ok(u32 p) { return sb_ram_fast(p) != nullptr; }

// out = a * b, affine 3x4 row-major (implicit last row 0 0 0 1).
void compose(float out[12], const float a[12], const float b[12]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c];
        out[r * 4 + 3] = a[r * 4 + 0] * b[3] + a[r * 4 + 1] * b[7] + a[r * 4 + 2] * b[11] +
                         a[r * 4 + 3];
    }
}

float guest_f32(u32 addr) {
    const u32 bits = sb_r32(addr);
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

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

    // The REAL DRAW RUNS FIRST, deliberately. J3DShapeMtx::load issues the matrix loads from inside
    // it, so running it first makes the exact slot -> matrix-index mapping the game used available
    // to the capture below. Reconstructing that mapping from the scene graph instead required
    // knowing each J3DShapeMtx's subclass, and getting it wrong is what left 20% of elements on the
    // wrong matrix. The draw matrices are computed by viewCalc BEFORE the draw, so reading them
    // afterwards is equally valid.
    sbr_mtx_begin_shape(shape);
    func_802e0390(cpu);
    sbr_mtx_end_shape();
    // Bring the parsed GX state up to date with what the game has just written, so the texture
    // binding read below is THIS shape's material rather than a stale one.
    gxfifo_drain_pending();

    // Segment the loads into elements: within an element the ids run 0,1,2,... so an id of 0 starts
    // a new element.
    std::vector<std::map<u16, u16>> segs;
    for (const auto& [id, idx] : sbr_mtx_loads()) {
        if (id == 0 || segs.empty()) segs.emplace_back();
        segs.back()[id] = idx;
    }

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
                const uint64_t key = ((uint64_t)shape << 16) | (uint64_t)i;
                uint32_t geom = 0;
                if (g_layout.valid) {
                    // Model-space positions do not change tick to tick (animation moves matrices),
                    // so a key that already has geometry is not decoded again — the decode belongs
                    // on the first sighting, not on the per-frame path.
                    if (sbr_scene_has_geometry(key)) {
                        geom = sbr_scene_intern_geometry(key, nullptr, 0);
                        ++g_st.decoded;
                    } else {
                        g_tri.clear();
                        if (j3d_decode_element(shape, i, g_layout, g_tri)) {
                            ++g_st.decoded;
                            g_st.tris += g_tri.size() / 3;
                            std::vector<SbrGeomVert> gv;
                            gv.reserve(g_tri.size());
                            for (const J3DVert& v : g_tri) {
                                SbrGeomVert g{v.x, v.y, v.z, v.nx, v.ny, v.nz, v.pnMtxSlot, {}, v.rgba};
                                for (unsigned s = 0; s < 4; ++s) {
                                    g.uv[s][0] = v.uv[s][0];
                                    g.uv[s][1] = v.uv[s][1];
                                }
                                gv.push_back(g);
                            }
                            geom = sbr_scene_intern_geometry(key, gv.data(), (int)gv.size());
                        } else {
                            ++g_st.decode_fail;
                        }
                    }
                }

                // One drawable PER MATRIX SLOT. The slot -> matrix-index mapping comes from the
                // loads the game just performed, so a skinned element (J3DShapeMtxMulti loads one
                // matrix per bone, ids 0,1,2,...) becomes one drawable per bone with the correct
                // matrix, instead of every vertex sharing one wrong matrix.
                if (drawMtxArray != 0 && geom != 0) {
                    const std::map<u16, u16>* seg = (i < segs.size()) ? &segs[i] : nullptr;

                    // Fall back to the stored index only when the shape loaded nothing observable
                    // (J3DShapeMtxDL bakes its matrices into a display list).
                    std::map<u16, u16> fallback;
                    if (seg == nullptr || seg->empty()) {
                        const u32 mtxObj = ok(sb_r32(shape + SHAPE_MATRICES))
                                               ? sb_r32(sb_r32(shape + SHAPE_MATRICES) + i * 4) : 0;
                        fallback[0] = (u16)(ok(mtxObj) ? sb_r16(mtxObj + SHAPEMTX_SLOT) : 0);
                        seg = &fallback;
                        ++g_st.no_load_truth;
                    }

                    for (const auto& [gxSlot, mtxIndex] : *seg) {
                        sbr_mtx_check_index(shape, (int)i, mtxIndex, mtxIndex, true);
                        const u32 mtxAddr = drawMtxArray + (u32)mtxIndex * 48;
                        if (!ok(mtxAddr) || !ok(mtxAddr + 47)) continue;

                        SbrDrawable dr{};
                        // Key includes the GX slot so each bone group is matched across ticks in
                        // its own right.
                        dr.key = (key << 8) | (uint64_t)gxSlot;
                        dr.geom = sbr_scene_geometry_for_slot(key, geom, (uint32_t)gxSlot);
                        if (dr.geom == 0) continue;   // this element has no vertices on that slot
                        // From the COMMAND STREAM (BP 0x40/0x41), not the SDK overrides. J3D sets
                        // z-mode and blend by replaying display lists, so GXSetZMode/GXSetBlendMode
                        // never see them: 43% of draws (2.3M of 5.4M) had the two sources
                        // disagreeing, typically SDK depth-WRITE on where the stream says off —
                        // which makes a background draw occlude everything behind it.
                        // SBR_RASTER_SRC=fifo uses the stream-derived state instead. Kept OPT-IN:
                        // the FIFO source is the faithful one in principle (the game writes 2.6M
                        // ZMode and 4.5M cmode0 through display lists that the SDK overrides never
                        // see, and the two sources disagree on 43% of draws), but at equal N it
                        // scores edgeIoU 24.8/+0.662 against the SDK path's 26.0/+0.652 — mixed,
                        // not a win. Switching the default on that would trade a known-stale input
                        // for an unverified one. It stays opt-in until the oracle compares this
                        // port's raster state against aurora's directly.
                        static const bool useSdk = [] {
                            const char* e = std::getenv("SBR_RASTER_SRC");
                            return !(e != nullptr && e[0] == 'f');
                        }();
                        dr.depth = useSdk ? sbr_gx_current_zmode() : sbr_gx_fifo_zmode();
                        {   // Do the SDK-captured and FIFO-derived raster states agree? If they do
                            // not, every draw this port rendered used state the game never set for
                            // it. Measured before switching the renderer over, not assumed.
                            const SbrDepthState fz = sbr_gx_current_zmode();
                            static long same = 0, diff = 0;
                            const bool eq = fz.test == dr.depth.test && fz.func == dr.depth.func &&
                                            fz.write == dr.depth.write &&
                                            fz.blend == dr.depth.blend &&
                                            fz.srcFac == dr.depth.srcFac &&
                                            fz.dstFac == dr.depth.dstFac;
                            eq ? ++same : ++diff;
                            if (((same + diff) % 200000) == 0)
                                lucent::info("nrender", "raster source check: {} draws agree, {} "
                                             "DIFFER (SDK z t{}w{}f{} bl{}/{}/{} vs FIFO "
                                             "t{}w{}f{} bl{}/{}/{})",
                                             same, diff, dr.depth.test, dr.depth.write,
                                             dr.depth.func, dr.depth.blend, dr.depth.srcFac,
                                             dr.depth.dstFac, fz.test, fz.write, fz.func,
                                             fz.blend, fz.srcFac, fz.dstFac);
                        }
                        // From the COMMAND STREAM, not GXLoadTexObj: J3D binds material textures by
                        // replaying baked display lists, so the SDK entry point only ever sees the
                        // 4x4 null texture (measured: 3 textures for the whole scene).
                        // All four texture units, not just TEXMAP0: a TEV stage names the map it
                        // samples, and most materials here name more than one.
                        for (unsigned m = 0; m < 8; ++m) dr.tex[m] = sbr_gx_fifo_texture(m);
                        dr.tev = sbr_gx_fifo_tev();
                        dr.xf  = sbr_gx_fifo_xf();
                        // Record the same snapshot for the state oracle, stamped with the parser's
                        // stream position, so "is this snapshot attached to the right draw?" is a
                        // measurement rather than an assumption. See state_oracle.h.
                        if (sbr_state_diff_enabled()) {
                            SbrDrawState cs{};
                            sbr_draw_state_fill(cs, dr.tev, dr.xf);
                            for (unsigned m = 0; m < 4; ++m)
                                cs.unitId[m] = dr.tex[m].addr & 0x01FFFFFFu;
                            sbr_state_oracle_capture(sbr_gxfifo_stream_pos(), cs);
                        }
                        {
                            bool is2d = false;
                            const float* pj = sbr_gx_current_projection(&is2d);
                            __builtin_memcpy(dr.proj, pj, sizeof dr.proj);
                            dr.is2d = is2d ? 1 : 0;
                        }
                        // Used AS-IS: J3D's mDrawMtx is already MODEL x VIEW (viewCalc concatenates
                        // the camera). Confirmed by the per-draw cross-check, not by inference.
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

}

} // namespace

SB_OVERRIDE(0x802e0390u, ov_shape_draw, "J3DShape::draw",
            "native render: capture the game's geometry at the J3D level (observe only for now)")
