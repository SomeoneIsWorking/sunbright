// j3dload_test — the PRODUCER milestone: load a REAL Super Mario Sunshine BMD model
// natively (no Dolphin, no PPC, no GameCube) through the game's OWN J3D loader, and
// verify the parsed J3DModelData has the expected shape/material/joint counts.
//
// This is handoff step 3's prerequisite: the renderer's consumers (geometry decode,
// transform, depth, textures, TEV, lighting) are all done; this is the producer that
// feeds them. The path here:
//   real BE BMD file  ->  bmd_swap_to_host (BE->host byteswap, sms-assets)
//                     ->  JKRExpHeap bringup (the game's own allocator)
//                     ->  J3DModelLoaderDataBase::load (the REAL decomp loader)
//                     ->  J3DModelData with shapes/materials/joints walked.
//
// VERIFY-FIRST: assertions are exact, spec-computed counts read from the BMD's own
// INF1/SHP1/MAT3/JNT1 block headers (cross-checked against the loader's output) — not
// "looks loaded". The BMD is a copyrighted game asset (gitignored scratch/bmd/); when
// it is absent the test SKIP-passes so ctest stays green on a fresh clone, but on this
// machine it runs against the real file and the numbers must match.
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include "bmd_swap.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <new>
#include <vector>

using smsport::assets::bmd_swap_to_host;
using smsport::assets::BmdSwapResult;

// --- explicit big-endian reads of the ORIGINAL file (oracle for the expected counts).
static uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Walk the BE BMD block table and read the per-block element count from the block
// whose tag matches. This is the independent oracle the loader's output is checked
// against (block header: u32 tag, u32 size; element count is the first u16 after the
// 8-byte JUTDataBlockHeader for INF1's flags-then-... layout we use the known offsets).
struct BmdCounts { uint16_t shapes, materials, joints; bool ok; };
static BmdCounts oracle_counts(const uint8_t* be, size_t len) {
    BmdCounts c{ 0, 0, 0, false };
    if (len < 0x20 || be32(be) != 0x4A334432 /* 'J3D2' */) return c;
    uint32_t nblk = be32(be + 0x0C);
    size_t off = 0x20;
    for (uint32_t i = 0; i < nblk && off + 8 <= len; ++i) {
        uint32_t tag = be32(be + off);
        uint32_t sz  = be32(be + off + 4);
        if (sz < 8 || off + sz > len) break;
        // Element count = first u16 of the block body (offset +8) for SHP1/MAT3/JNT1.
        uint16_t n = be16(be + off + 8);
        switch (tag) {
        case 0x53485031: c.shapes = n; break;    // 'SHP1'
        case 0x4D415433: c.materials = n; break;  // 'MAT3'
        case 0x4A4E5431: c.joints = n; break;     // 'JNT1'
        }
        off += sz;
    }
    c.ok = true;
    return c;
}

// Bring up the game's own allocator: construct a JKRExpHeap on a host block. The ctor
// (parent=nullptr) calls becomeCurrentHeap(), so the decomp's operator new -> JKRHeap::
// alloc(sCurrentHeap) works. (JKRExpHeap::createRoot's initArena reads the GC physical-0
// OS area, unsafe natively — so construct directly, as createRoot itself does.)
static void bringup_heap(size_t bytes) {
    void* block = nullptr;
    if (posix_memalign(&block, 32, bytes) != 0 || !block) { std::abort(); }
    std::memset(block, 0, bytes);
    u32 ehSize = (u32)((sizeof(JKRExpHeap) + 0xF) & ~0xFu);
    new (block) JKRExpHeap((u8*)block + ehSize, (u32)bytes - ehSize, nullptr, false);
}

int main(int argc, char** argv) {
    std::setbuf(stdout, nullptr);

    // The decomp's global `operator new` (JKRHeap::alloc) replaces the standard one
    // for the WHOLE executable — so even this test's std::vector/std::string allocate
    // from the current JKRHeap. Bring it up FIRST, before any C++ allocation.
    bringup_heap(16u * 1024 * 1024);

    const char* path = (argc > 1) ? argv[1] : std::getenv("SUNBRIGHT_BMD");
    if (!path) path = "scratch/bmd/default.bmd";

    FILE* f = std::fopen(path, "rb");
    if (!f) {
        printf("j3dload_test: SKIP (no BMD at %s — copyrighted asset, gitignored)\n", path);
        return 0;  // skip-pass: keep ctest green where the game asset is absent
    }
    std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> be(n);
    if (std::fread(be.data(), 1, n, f) != (size_t)n) { std::fclose(f); return 2; }
    std::fclose(f);

    // Independent oracle from the raw BE file.
    BmdCounts oc = oracle_counts(be.data(), be.size());

    // Swap BE -> host so the pristine decomp loader reads it correctly.
    std::vector<uint8_t> host;
    BmdSwapResult sr = bmd_swap_to_host(be.data(), be.size(), host);
    int fail = 0;
    if (!sr.ok || !sr.all_covered) {
        printf("FAIL swap incomplete: ok=%d covered=%u/%u err=%s\n",
               sr.ok, sr.blocks_covered, sr.block_num, sr.error ? sr.error : "-");
        return 1;
    }

    bringup_heap(8u * 1024 * 1024);

    J3DModelData* md = J3DModelLoaderDataBase::load(host.data(), 0);
    if (!md) { printf("FAIL load() returned NULL\n"); return 1; }

    u16 sn = md->getShapeNum(), mn = md->getMaterialNum(), jn = md->getJointNum();
    printf("%s: loaded  shapes=%u materials=%u joints=%u  (oracle s=%u m=%u j=%u)\n",
           path, sn, mn, jn, oc.shapes, oc.materials, oc.joints);

    #define CK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ++fail; } } while (0)
    // The loader's counts MUST equal the BMD's own block-header counts.
    CK(sn == oc.shapes,    "shapeNum == SHP1 count");
    CK(mn == oc.materials, "materialNum == MAT3 count");
    CK(jn == oc.joints,    "jointNum == JNT1 count");
    // Every shape node pointer is populated (non-null), confirming the walk.
    for (u16 i = 0; i < sn; ++i)
        CK(md->getShapeNodePointer(i) != nullptr, "shape node pointer non-null");
    // Materials likewise.
    for (u16 i = 0; i < mn; ++i)
        CK(md->getMaterialNodePointer(i) != nullptr, "material node pointer non-null");

    printf(fail ? "j3dload_test: %d FAILED\n" : "j3dload_test: all passed\n", fail);
    return fail ? 1 : 0;
}
