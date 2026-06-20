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

namespace sb::render {

// A vertex the native engine produces itself: NDC xyz + RGBA (0..1). z is the NDC
// depth ([0,1] in Vulkan) used for depth testing; 2D content sets z=0.
struct NvkVertex {
    float x, y, z;       // clip/NDC (z = depth)
    float r, g, b, a;
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
