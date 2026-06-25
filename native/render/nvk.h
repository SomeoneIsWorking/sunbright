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
#include "gx_geom.h"   // the geometry types (NvkVertex/NvkTexVertex/NvkTevVertex/NvkTevPush/
                       // NvkClear/NvkTevBatch) now live here, renderer-agnostic. This header keeps
                       // ONLY the retired Nvk Vulkan renderer class, used by the render-test suite.

namespace sb::render {

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
    using NvkTevBatch = sb::render::NvkTevBatch;   // the type moved to gx_geom.h; alias for callers
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
