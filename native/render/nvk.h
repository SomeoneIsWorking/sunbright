// nvk.h — the native PC renderer's standalone Vulkan core ("Native VulKan").
//
// A from-scratch, headless, GameCube-free Vulkan renderer: it creates its OWN Vulkan
// instance/device/queue (NOT Dolphin's g_vulkan_context), renders to an offscreen
// color target, and reads the pixels back to host memory. This is the foundation the
// full native SMS renderer (ngx logic: J3D meshes + GXState) plugs into — proving we
// can produce a frame with no Dolphin and verify it by pixels.
//
// Headless by design (no surface/swapchain): the engine presents by handing the
// readback (or the offscreen image) to a host window/swapchain layer separately. For
// bring-up and tests we read pixels back and check them.
#pragma once
#include <cstdint>
#include <vector>
#include <string>

namespace sb::render {

// A vertex the native engine produces itself: NDC xyz + RGBA (0..1). z is the NDC
// depth ([0,1] in Vulkan) used for depth testing; 2D content sets z=0.
struct NvkVertex {
    float x, y, z;       // clip/NDC (z = depth)
    float r, g, b, a;
};

// A textured vertex: NDC xyz + UV. Used by the textured draw path.
struct NvkTexVertex {
    float x, y, z;
    float u, v;
};

// A TEV vertex: NDC xyz + both GX raster colour channels + the 8 GX texcoord UVs
// (texgen already applied on the CPU). Matches the inputs the TEV fragment shader
// (sb_tev_gen_fragment) consumes: vColor (color0), vColor1 (COLOR1A1), vUV[0..7].
struct NvkTevVertex {
    float x, y, z;        // NDC position (z = depth)
    float rgba[4];        // raster color0 (0..1)
    float rgba1[4];       // raster COLOR1A1 (0..1)
    float uv[8][2];       // per GX texcoord 0..7
};

// Push constants the generated TEV shader reads: GX TEV konst colours (0..255) and
// the S10 TEV colour registers (CPREV/C0/C1/C2). Matches the GLSL
// `layout(push_constant) Mat { ivec4 kcolor[4]; ivec4 tevreg[4]; }`.
struct NvkTevPush {
    int32_t kcolor[4][4];   // KONST0..3 RGBA
    int32_t tevreg[4][4];   // CPREV/C0/C1/C2 RGBA (S10)
};

struct NvkClear { float r, g, b, a; };

class Nvk {
public:
    // Create instance/device/offscreen target of the given size. preferCpu forces the
    // software (lavapipe) device — useful for deterministic headless CI. Returns false
    // if no Vulkan device is available.
    bool init(uint32_t width, uint32_t height, bool preferCpu = false);
    void shutdown();

    // Render one frame: clear, then draw the triangle list (3N verts). Reads the
    // result back into rgba() as tightly-packed RGBA8 (width*height*4). Returns false
    // on a device error.
    bool renderTriangles(const std::vector<NvkVertex>& verts, NvkClear clear);

    // Upload an RGBA8 texture (row-major, w*h*4 bytes) for the textured draw path.
    // Replaces any previous texture. Returns false on error.
    bool setTexture(const uint8_t* rgba, uint32_t w, uint32_t h);

    // Render a textured triangle list (3N verts) sampling the uploaded texture.
    // setTexture must have been called first. Reads pixels back like renderTriangles.
    bool renderTexturedTriangles(const std::vector<NvkTexVertex>& verts, NvkClear clear);

    // --- TEV combiner path: a generated per-material fragment shader (tev_shader) ---
    // Compile a complete GLSL 450 fragment shader (from sb_tev_gen_fragment) and build
    // the TEV pipeline from it. Must be called before renderTevTriangles; recompiles
    // when the material's TEV state changes. Returns false on a compile/build error.
    bool setTevFragment(const std::string& glslFragment);

    // Upload an RGBA8 texture into GX texmap `slot` (0..7) for the TEV path. Slots not
    // uploaded sample a 1x1 white default. Returns false on error.
    bool setTevTexture(int slot, const uint8_t* rgba, uint32_t w, uint32_t h);

    // Render a TEV triangle list (3N verts) through the generated fragment shader with
    // the given push constants. setTevFragment must have been called. Reads pixels back.
    bool renderTevTriangles(const std::vector<NvkTevVertex>& verts,
                            const NvkTevPush& push, NvkClear clear);

    // ── Multi-material TEV frame: clear ONCE, draw many material batches into the same
    // color+depth target (the real scene path), read back ONCE. Each batch carries its
    // own generated fragment shader, textures, push constants and depth/blend state.
    // Shaders + pipelines are cached across frames (keyed by NvkTevBatch::shaderKey and
    // the pipeline state); per-batch textures + descriptor sets are transient.
    struct NvkTevBatch {
        uint32_t vstart = 0, vcount = 0;
        NvkTevPush push{};
        const char* fragGlsl = nullptr;   // sb_tev_gen_fragment(...) source (owned by caller)
        uint64_t shaderKey = 0;           // unique per distinct fragGlsl (shader/pipeline cache key)
        struct Tex { const uint8_t* rgba = nullptr; uint32_t w = 0, h = 0;
                     uint8_t linear = 0;      // MAG filter linear (else nearest)
                     uint8_t min_filter = 1;  // GX min filter (encodes mip mode)
                     uint8_t max_aniso = 0;   // GX_ANISO_1/2/4 = 0/1/2
                     uint8_t wrap_s = 1, wrap_t = 1; } tex[8];
        uint8_t z_test = 1, z_func = 3 /*GX_LEQUAL*/, z_write = 1;
        uint8_t blend_mode = 0, src_factor = 1, dst_factor = 0;   // GX blend (0=none)
    };
    bool renderTevFrame(const std::vector<NvkTevVertex>& verts,
                        const std::vector<NvkTevBatch>& batches, NvkClear clear);

    const std::vector<uint8_t>& rgba() const { return pixels_; }
    uint32_t width() const  { return width_; }
    uint32_t height() const { return height_; }
    const char* deviceName() const { return deviceName_; }

    // Pixel at (x,y), as 4 bytes RGBA. Caller ensures in-bounds.
    const uint8_t* at(uint32_t x, uint32_t y) const {
        return &pixels_[(y * width_ + x) * 4];
    }

    ~Nvk() { shutdown(); }

private:
    struct Impl;
    Impl* d_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
    char deviceName_[256] = {0};
    std::vector<uint8_t> pixels_;
};

} // namespace sb::render
