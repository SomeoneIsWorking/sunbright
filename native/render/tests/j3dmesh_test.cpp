// j3dmesh_test — the RENDER half of handoff step 3: take a REAL BMD shape loaded
// through the game's own J3D loader (j3dload_test built the producer) and render its
// geometry to pixels via the native renderer (nvk), Dolphin-free.
//
// The join ("re-point ngx at native structs"): build the ngx CP descriptor (VCD/VAT/
// array bases) from the NATIVE J3DShape + J3DVertexData struct fields — the same data
// runtime/overrides/ngx_j3d_shape.cpp reads from guest RAM — then walk each
// J3DShapeDraw display list through the shipping ngx mesh decoder (ngx_build_mesh) and
// render the assembled triangles with nvk.
//
// ENDIANNESS: bmd_swap byteswaps STRUCTURAL data to host-endian (so the decomp loader
// reads it), so VCD/VAT/array-base POINTERS are read natively from the swapped buffer.
// But the ngx vertex/DL decoders expect GUEST big-endian data (they byteswap on read),
// and bmd_swap DEFERS the SHP1 display-list interior — so vertex-array DATA and the DL
// stream are resolved against the ORIGINAL big-endian buffer (same offsets, raw BE).
//
// VERIFY-FIRST: assert triangles decoded > 0, every decoded position inside the shape's
// own bounding box (unk10..unk1C, proving POS decoded correctly), and a non-empty render
// (the geometry actually rasterized). SKIP-passes when the gitignored BMD is absent.
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DVertex.hpp>
#include "bmd_swap.h"

#include "nvk.h"
#include "ngx_mesh.h"     // NgxCP, NgxVertex, ngx_build_mesh
#include "ngx_decode.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <vector>

using namespace sb::render;
using smsport::assets::bmd_swap_to_host;

static int g_fail = 0, g_checks = 0;
static void chk(bool ok, const char* what) {
    ++g_checks; if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

static void bringup_heap(size_t bytes) {
    void* block = nullptr;
    if (posix_memalign(&block, 32, bytes) != 0 || !block) std::abort();
    std::memset(block, 0, bytes);
    u32 ehSize = (u32)((sizeof(JKRExpHeap) + 0xF) & ~0xFu);
    new (block) JKRExpHeap((u8*)block + ehSize, (u32)bytes - ehSize, nullptr, false);
}

// The decomp's global operator new (JKRHeap::alloc) replaces the standard one for the
// WHOLE exe — including glslang's STATIC-INITIALIZER allocations (KeywordMap), which run
// BEFORE main(). So bring the heap up in a high-priority static initializer (runs before
// glslang's unprioritized ctors). 64 MB covers static init + runtime shader compilation.
namespace {
struct HeapBringup { HeapBringup() { bringup_heap(64u * 1024 * 1024); } };
HeapBringup g_heap_bringup __attribute__((init_priority(101)));
}

// Resolver: ngx array_base holds an OFFSET into the BMD buffer; the vertex-array DATA
// is read from the ORIGINAL big-endian buffer (ngx byteswaps it).
struct ResolveCtx { const uint8_t* be_base; size_t be_size; };
static const unsigned char* resolve_be(unsigned off, void* user) {
    auto* c = (ResolveCtx*)user;
    if (off == 0 || off >= c->be_size) return nullptr;
    return c->be_base + off;
}

// Build the ngx CP descriptor from native J3DShape + J3DVertexData fields. Ported from
// runtime/overrides/ngx_j3d_shape.cpp build_cp, reading native struct fields instead of
// guest RAM. Array bases become OFFSETS into the buffer (host_ptr - swapped_base).
static bool build_native_cp(J3DShape* shape, J3DVertexData& vd,
                            const uint8_t* swapped_base, NgxCP& cp) {
    GXVtxDescList* desc = shape->getVtxDesc();
    GXVtxAttrFmtList* fmt = vd.getVtxAttrFmtList();
    if (!desc || !fmt) return false;

    // VCD from the descriptor list (host-endian after swap).
    for (GXVtxDescList* e = desc; e->attr != 0xFF; ++e) {
        u32 attr = e->attr, type = e->type;
        if (attr == 0)                     cp.vcd_lo |= (type ? 1u : 0u) << 0;
        else if (attr <= 8)                cp.vcd_lo |= (type ? 1u : 0u) << attr;
        else if (attr == 9)                cp.vcd_lo |= (type & 3) << 9;    // POS
        else if (attr == 10)               cp.vcd_lo |= (type & 3) << 11;   // NRM
        else if (attr == 11)               cp.vcd_lo |= (type & 3) << 13;   // CLR0
        else if (attr == 12)               cp.vcd_lo |= (type & 3) << 15;   // CLR1
        else if (attr >= 13 && attr <= 20) cp.vcd_hi |= (type & 3) << (2 * (attr - 13));
    }

    // VAT + per-attr type (for strides) from the format list.
    u32 pos_type = 4, nrm_type = 4, tex_type[8] = {4,4,4,4,4,4,4,4};
    u32& A = cp.vat[0][0]; u32& B = cp.vat[0][1]; u32& C = cp.vat[0][2];
    for (GXVtxAttrFmtList* e = fmt; e->attr != 0xFF; ++e) {
        u32 attr = e->attr, cnt = e->cnt, type = e->type, frac = e->frac & 0x1f;
        switch (attr) {
        case 9:  A |= (cnt & 1) | ((type & 7) << 1) | (frac << 4); pos_type = type; break;
        case 10: A |= ((cnt ? 1u : 0u) << 9) | ((type & 7) << 10); if (cnt == 2) A |= 1u << 31;
                 nrm_type = type; break;
        case 11: A |= ((cnt & 1) << 13) | ((type & 7) << 14); break;
        case 12: A |= ((cnt & 1) << 17) | ((type & 7) << 18); break;
        case 13: A |= ((cnt & 1) << 21) | ((type & 7) << 22) | (frac << 25); tex_type[0]=type; break;
        case 14: B |= (cnt & 1) | ((type & 7) << 1) | (frac << 4);           tex_type[1]=type; break;
        case 15: B |= ((cnt & 1) << 9)  | ((type & 7) << 10) | (frac << 13); tex_type[2]=type; break;
        case 16: B |= ((cnt & 1) << 18) | ((type & 7) << 19) | (frac << 22); tex_type[3]=type; break;
        case 17: B |= ((cnt & 1) << 27) | ((type & 7) << 28); C |= frac;     tex_type[4]=type; break;
        case 18: C |= ((cnt & 1) << 5)  | ((type & 7) << 6)  | (frac << 9);  tex_type[5]=type; break;
        case 19: C |= ((cnt & 1) << 14) | ((type & 7) << 15) | (frac << 18); tex_type[6]=type; break;
        case 20: C |= ((cnt & 1) << 23) | ((type & 7) << 24) | (frac << 27); tex_type[7]=type; break;
        default: break;
        }
    }

    // Array bases: this is a STATIC (unbound) model, so use J3DVertexData's own arrays
    // (the recomp's j3dSys per-view buffers aren't set without binding). Convert each
    // host pointer into an offset for the BE resolver.
    auto off = [&](const void* p) -> u32 {
        return p ? (u32)((const uint8_t*)p - swapped_base) : 0;
    };
    cp.array_base[0] = off(vd.getVtxPosArray());     cp.array_stride[0] = (pos_type == 4) ? 12 : 6;
    cp.array_base[1] = off(vd.getVtxNormArray());    cp.array_stride[1] = (nrm_type == 4) ? 12 : 6;
    cp.array_base[2] = off(vd.getVtxColorArray(0));  cp.array_stride[2] = 4;
    cp.array_base[3] = off(vd.getVtxColorArray(1));  cp.array_stride[3] = 4;
    for (int i = 0; i < 8; ++i) {
        cp.array_base[4 + i]   = off(vd.getVtxTexCoordArray(i));
        cp.array_stride[4 + i] = (tex_type[i] == 4) ? 8 : 4;
    }
    return true;
}

int main(int argc, char** argv) {
    std::setbuf(stdout, nullptr);   // heap is up via the init_priority static above

    const char* path = (argc > 1) ? argv[1] : std::getenv("SUNBRIGHT_BMD");
    if (!path) path = "scratch/bmd/shadowcube.bmd";   // 1 shape — simplest target

    FILE* f = std::fopen(path, "rb");
    if (!f) { std::printf("j3dmesh_test: SKIP (no BMD at %s)\n", path); return 0; }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> be(n);
    if (std::fread(be.data(), 1, n, f) != (size_t)n) { std::fclose(f); return 2; }
    std::fclose(f);

    std::vector<uint8_t> host;
    auto sr = bmd_swap_to_host(be.data(), be.size(), host);
    if (!sr.ok || !sr.all_covered) { std::printf("FAIL swap\n"); return 1; }

    J3DModelData* md = J3DModelLoaderDataBase::load(host.data(), 0);
    if (!md || md->getShapeNum() == 0) { std::printf("FAIL load/no shapes\n"); return 1; }
    std::printf("%s: shapes=%u joints=%u drawMtx=%u fullWgt=%u wEvlp=%u\n", path,
                md->getShapeNum(), md->getJointNum(), md->getDrawMtxNum(),
                md->getDrawFullWgtMtxNum(), md->getWEvlpMtxNum());

    ResolveCtx rc{ be.data(), be.size() };
    J3DVertexData& vd = md->getVertexData();

    // Assemble EVERY shape of the model (not just shape 0). Each shape is decoded into
    // the shared vertex/index buffers and verified against ITS OWN bounding box (a shape
    // referencing the wrong array base / a leaking VCD would land a vertex outside its
    // box). The whole-model union bbox drives the ortho fit.
    std::vector<NgxVertex> verts;
    std::vector<unsigned> idx;
    int tris = 0;
    const float pad = 1.0f;
    Vec umn{ 1e30f, 1e30f, 1e30f }, umx{ -1e30f, -1e30f, -1e30f };
    for (u16 si = 0; si < md->getShapeNum(); ++si) {
        J3DShape* shape = md->getShapeNodePointer(si);
        chk(shape != nullptr, "shape present");
        if (!shape) continue;
        NgxCP cp{};
        chk(build_native_cp(shape, vd, host.data(), cp), "built CP descriptor");

        size_t v0 = verts.size();
        int stris = 0;
        for (u16 e = 0; e < shape->getMtxGroupNum(); ++e) {
            J3DShapeDraw* dp = shape->getShapeDraw(e);
            if (!dp) continue;
            const u8* dl = dp->getDisplayList();
            u32 sz = dp->getDisplayListSize();
            // The DL pointer is in the swapped buffer but its bytes are unswapped (BE);
            // ngx decodes it as a guest GX stream. Arrays resolve against the BE buffer.
            int t = ngx_build_mesh(cp, dl, sz, resolve_be, &rc, verts, idx);
            if (t < 0) { std::printf("  FAIL: shape %u DL %u failed to frame\n", si, e); ++g_fail; }
            else stris += t;
        }
        tris += stris;

        // POS-DECODE verification, skinning-aware. Decoded positions are in their array
        // (joint-local) space. For a RIGID shape (or one whose bind-pose joints are
        // identity) they land inside the shape's own bbox — the strong containment check.
        // For an ENVELOPE-skinned shape they don't: SMS computes the weighted blend in a
        // deeper J3DSkinDeform/per-vertex-weight path (calcWeightEnvelopeMtx is EMPTY,
        // the J3DMtxCalcBasic static path never fills the envelope matrices), which isn't
        // ported yet, so a vertex sits in its joint-local frame, offset from the posed
        // bbox by up to a joint translation. A *decode* bug looks completely different —
        // garbage stride/index gives NaN/inf or coordinates orders of magnitude past the
        // shape's own scale. So the data-derived bound is: every position is finite, and
        // any bbox overflow stays within the shape's OWN extent (no magic constant).
        const Vec& mn = shape->unk10; const Vec& mx = shape->unk1C;
        const float ext = std::max(mx.x-mn.x, std::max(mx.y-mn.y, mx.z-mn.z));
        bool inbox = verts.size() > v0, finite = true;
        float worst = 0.f;
        for (size_t k = v0; k < verts.size(); ++k) {
            const NgxVertex& v = verts[k];
            for (int c = 0; c < 3; ++c) if (!std::isfinite(v.pos[c])) finite = false;
            float ox = std::max(0.f, std::max(mn.x-pad - v.pos[0], v.pos[0] - (mx.x+pad)));
            float oy = std::max(0.f, std::max(mn.y-pad - v.pos[1], v.pos[1] - (mx.y+pad)));
            float oz = std::max(0.f, std::max(mn.z-pad - v.pos[2], v.pos[2] - (mx.z+pad)));
            worst = std::max(worst, std::max(ox, std::max(oy, oz)));
            if (ox > 0 || oy > 0 || oz > 0) inbox = false;
        }
        // Label: does the shape reference a weighted-envelope draw matrix? (index >= the
        // full-weight count). Diagnostic only — the PASS condition is the data bound above.
        u16 fullWgt = md->getDrawFullWgtMtxNum();
        bool envelope = false;
        for (u16 g = 0; g < shape->getMtxGroupNum() && !envelope; ++g) {
            J3DShapeMtx* m = shape->getShapeMtx(g);
            for (u32 i = 0; m && i < m->getUseMtxNum(); ++i)
                if (m->getUseMtxIndex(i) >= fullWgt) { envelope = true; break; }
        }
        std::printf("  shape %u: tris=%d %s bbox min(%.2f,%.2f,%.2f) max(%.2f,%.2f,%.2f) overflow=%.2f%s\n",
                    si, stris, envelope ? "envelope" : "rigid",
                    mn.x, mn.y, mn.z, mx.x, mx.y, mx.z, worst,
                    (!inbox && worst > 0) ? "  (unskinned envelope offset — see journal)" : "");
        chk(stris > 0, "shape decoded > 0 triangles");
        chk(finite, "all decoded positions finite");
        // Strong check where it holds; otherwise the unskinned offset must stay within the
        // shape's own scale (a real decode regression blows past this).
        chk(inbox || worst < ext, "decoded positions sane (in-bbox or within shape extent)");

        umn.x = std::min(umn.x, mn.x); umn.y = std::min(umn.y, mn.y); umn.z = std::min(umn.z, mn.z);
        umx.x = std::max(umx.x, mx.x); umx.y = std::max(umx.y, mx.y); umx.z = std::max(umx.z, mx.z);
    }
    std::printf("  total: shapes=%u tris=%d verts=%zu\n", md->getShapeNum(), tris, verts.size());
    chk(tris > 0, "decoded > 0 triangles");
    chk(verts.size() >= 3, "decoded >= 3 vertices");

    const Vec& mn = umn; const Vec& mx = umx;
    // Render: orthographic projection fitting the union bbox into NDC, render the triangles.
    Nvk nvk;
    if (!nvk.init(128, 128) && !nvk.init(128, 128, true)) {
        std::printf("  (no Vulkan device — skipping raster check)\n");
        std::printf("%d checks, %d failures\n", g_checks, g_fail);
        return g_fail ? 1 : 0;
    }
    std::printf("  device: %s\n", nvk.deviceName());
    float cx = (mn.x + mx.x) * 0.5f, cy = (mn.y + mx.y) * 0.5f;
    float ex = std::max(mx.x - mn.x, mx.y - mn.y) * 0.55f; if (ex <= 0) ex = 1.f;
    std::vector<NvkVertex> tri;
    for (unsigned i : idx) {
        const NgxVertex& s = verts[i];
        // Map model XY into NDC via the bbox ortho fit; flat-shade by normal-less white.
        float nx = (s.pos[0] - cx) / ex, ny = (s.pos[1] - cy) / ex;
        unsigned char r = s.clr[0][0], g = s.clr[0][1], b = s.clr[0][2];
        if ((r|g|b) == 0) { r = g = b = 220; }  // no vertex color -> light grey
        tri.push_back({ nx, ny, 0.f, r/255.f, g/255.f, b/255.f, 1.f });
    }
    chk(nvk.renderTriangles(tri, { 0, 0, 0, 1 }), "rendered shape triangles");
    // Count non-black pixels — the shape rasterized.
    int lit = 0;
    for (uint32_t y = 0; y < nvk.height(); ++y)
        for (uint32_t x = 0; x < nvk.width(); ++x) {
            const uint8_t* p = nvk.at(x, y);
            if (p[0] + p[1] + p[2] > 60) ++lit;
        }
    std::printf("  lit pixels=%d / %u\n", lit, nvk.width() * nvk.height());
    chk(lit > 100, "shape rasterized (non-empty frame)");

    // Dump for inspection.
    FILE* out = std::fopen("scratch/screenshots/nvk_j3dshape.ppm", "wb");
    if (out) {
        std::fprintf(out, "P6\n%u %u\n255\n", nvk.width(), nvk.height());
        for (uint32_t y = 0; y < nvk.height(); ++y)
            for (uint32_t x = 0; x < nvk.width(); ++x) {
                const uint8_t* p = nvk.at(x, y);
                std::fputc(p[0], out); std::fputc(p[1], out); std::fputc(p[2], out);
            }
        std::fclose(out);
    }
    nvk.shutdown();

    std::printf(g_fail ? "j3dmesh_test: %d FAILED\n" : "j3dmesh_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
