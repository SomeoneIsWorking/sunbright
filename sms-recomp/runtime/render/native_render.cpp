// native_render.cpp — the recomp's own GX renderer on SDL3 GPU (2026-07-23, user-directed).
//
// The end state: no Aurora, no Dolphin — the parsed GX stream (dev_gxfifo) rendered by code we own,
// on the SDL3 GPU API. Built INCREMENTALLY alongside aurora, which stays as the per-frame parity
// oracle until this reaches parity (CLAUDE.md RENDERER DOCTRINE, 2026-07-23).
//
// The SDL3-GPU device and the clear/target/readback plumbing are resurrected from the retired
// Path-B renderer (git 9283f44^:native/render/gx_sdlgpu.cpp), which reached real per-material TEV.
// Its BACKEND (batches -> GPU) resurrects here; its FRONTEND (GX state -> transformed verts + TEV
// shaders) has to be driven from dev_gxfifo's FIFO parse, which is the work ahead.
//
// The GX compatibility path is selected with the legacy SBR_RENDERER=native token. It owns the
// SDL3-GPU device, window swapchain, and displayed picture. Aurora consumes the same FIFO offscreen
// as the oracle until this renderer reaches parity.

#include "native_render.h"

#include "native_efb_copy_clear_draw.h"
#include "native_efb_copy_plan.h"
#include "native_gpu_admission.h"
#include "native_gpu_guard.h"
#include "native_gpu_pipeline.h"
#include "native_presenter.h"
#include "native_raster_state.h"
#include "native_render_pass.h"
#include "native_tev_uniform.h"

#include "app/settings.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>

#include "gx_texture.h"
#include "intrinsics.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>

// Aurora's GPU-side copy-surface registry. See the call site in sbr_render_recheck_black.
extern "C" int sbr_aurora_has_copy_texture(unsigned int guestAddr);
#include <vector>

namespace {

SDL_GPUDevice* g_dev = nullptr;
SDL_GPUTexture* g_color = nullptr;     // offscreen EFB-sized colour target
SDL_GPUTexture* g_depth = nullptr;     // depth target
SDL_GPUTransferBuffer* g_dl = nullptr; // download staging (w*h*4)
int g_w = 0, g_h = 0;
int g_lastBatches = 0;
uint64_t g_cullAllDrawsDropped = 0;
uint64_t g_cullAllVerticesDropped = 0;
bool g_tried = false, g_ok = false;
SDL_Window* g_presentWindow = nullptr;

std::vector<uint8_t> g_cpu; // last frame read back, top-left origin RGBA8

struct Batch {
    SbrDepthState st;
    uint32_t first, count;
    uint64_t copyEpoch;
    bool copyClear = false;
    // One entry per GX texture unit. 0 = untextured (a 1x1 white texel is bound so one shader
    // serves both cases and no branch is needed in the TEV loop).
    uint64_t texKey[8];
    uint32_t texAddr[8]; // guest address, so a bind can resolve to an EFB-copy surface
    uint32_t sampKey[8]; // wrap/filter modes: part of the material, not of the texture data
    SbrNativeTevUniform tev;
};

// ---- EFB copy -> texture ----
// Textures produced by resolving the render target, keyed by the guest destination address the
// game will bind. Guest memory is never written, exactly as on hardware+aurora; a bind of the
// address resolves here instead of decoding zeros.
// The SIZE each copy texture was created at is kept alongside it. It is not decoration: the cache
// is keyed by guest address alone, and the game is free to copy a different-sized region to the
// same address later in the run (a half-res reflection buffer reused at full res, a copy whose
// rect grows with the viewport). Blitting a 640x448 destination rect into a texture allocated at
// 256x256 writes past the end of that allocation — a GPU-side out-of-bounds WRITE, which is
// precisely the fault the driver reported (GPUVM fault, RW: 1) before it reset the card.
struct CopyTex {
    SDL_GPUTexture* tex = nullptr;
    int w = 0, h = 0;
};
std::unordered_map<uint32_t, CopyTex> g_copyTex;
struct CopyPoint {
    size_t batchIndex;
    NativeEfbCopyPlan plan;
};
std::vector<CopyPoint> g_copyPoints;
NativeEfbCopySequence g_copySequence;

enum class CopyResult : uint8_t { Encoded, NoOp, Failed };

// Resolve the current render target region into the texture for `dest`, creating it on demand.
// Runs BETWEEN render passes — a blit is not a render-pass operation, and the pass must have ended
// for the target's contents to be defined.
CopyResult perform_copy(SDL_GPUCommandBuffer* cmd, const CopyPoint& cp) {
    if (sbr_native_gpu_dead())
        return CopyResult::Failed;
    const NativeEfbCopyPlan& plan = cp.plan;
    if (!plan.has_copy()) {
        lucent::debug("nrender",
                      "EFB copy 0x{:08x} has no source intersection: [{},{} {}x{}] in {}x{} — "
                      "no GPU operation encoded",
                      plan.dest, plan.source.x, plan.source.y, plan.source.width,
                      plan.source.height, g_w, g_h);
        return CopyResult::NoOp;
    }
    const int wantW = std::max(plan.destWidth, 1), wantH = std::max(plan.destHeight, 1);
    CopyTex& slot = g_copyTex[plan.dest];
    // A cached texture that no longer fits the requested destination is REPLACED, not reused. The
    // alternative — clamping the blit to the old size — would silently resolve into a surface of
    // the wrong dimensions and hand the game a stretched reflection, trading a GPU fault for a
    // rendering defect nobody could trace back to here.
    if (slot.tex != nullptr && (slot.w != wantW || slot.h != wantH)) {
        lucent::info("nrender",
                     "EFB copy 0x{:08x}: destination changed size {}x{} -> {}x{}; reallocating. "
                     "Blitting the larger rect into the old allocation would have been an "
                     "out-of-bounds GPU write.",
                     plan.dest, slot.w, slot.h, wantW, wantH);
        SDL_ReleaseGPUTexture(g_dev, slot.tex);
        slot.tex = nullptr;
    }
    SDL_GPUTexture*& dst = slot.tex;
    if (dst == nullptr) {
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ci.width = (Uint32)wantW;
        ci.height = (Uint32)wantH;
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        // COLOR_TARGET as well as SAMPLER: a blit writes the destination as a render target.
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        dst = SDL_CreateGPUTexture(g_dev, &ci);
        if (dst == nullptr) {
            sbr_native_gpu_disable(
                std::string("EFB copy texture allocation failed: ").append(SDL_GetError()).c_str());
            return CopyResult::Failed;
        }
        slot.w = wantW;
        slot.h = wantH;
        lucent::info("nrender", "EFB copy -> texture 0x{:08x}: EFB [{},{} {}x{}] -> {}x{}",
                     plan.dest, plan.source.x, plan.source.y, plan.source.width, plan.source.height,
                     wantW, wantH);
    }
    SDL_GPUBlitInfo bi{};
    bi.source.texture = g_color;
    bi.source.x = static_cast<Uint32>(plan.source.x);
    bi.source.y = static_cast<Uint32>(plan.source.y);
    bi.source.w = static_cast<Uint32>(plan.source.width);
    bi.source.h = static_cast<Uint32>(plan.source.height);
    bi.destination.texture = dst;
    // Belt and braces: the rect is clamped to the allocation that is actually bound, so even if the
    // reallocation above were ever bypassed the blit cannot address memory outside the texture.
    bi.destination.w = (Uint32)std::min(wantW, slot.w);
    bi.destination.h = (Uint32)std::min(wantH, slot.h);
    bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
    bi.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &bi);
    return CopyResult::Encoded;
}

// Decoded textures, keyed by the guest description. Textures are immutable for a given
// (address, format, size), so decoding once and caching is not an optimisation but a requirement:
// decoding a 1024-texel CMPR image per draw would dominate the frame.
struct Tex {
    SDL_GPUTexture* tex = nullptr;
    float mean = -1.0f; // decoded brightness, so what is BOUND can be compared with what the
                        // per-draw state SAYS is bound — the last unin­strumented link
    SbrTexture desc{};  // kept so a cached-black texture can be re-decoded from guest memory
                        // later: the cache fills on FIRST SIGHT, and a texture seen before its
                        // data landed stays black forever with nothing to say so
};
std::unordered_map<uint64_t, Tex> g_texs;
std::unordered_map<uint64_t, SbrTexture> g_pendingTex; // descriptions seen this frame
SDL_GPUSampler* g_sampler = nullptr;                   // REPEAT/LINEAR, the fallback
// One sampler per distinct wrap/filter combination. Wrap mode is a property of the MATERIAL, not of
// the texture data — the same image is legitimately bound clamped by one material and repeated by
// another — so it keys the batch rather than the texture cache.
std::unordered_map<uint32_t, SDL_GPUSampler*> g_samplers;

uint32_t sampler_key(const SbrTexture& t) {
    return (uint32_t)(t.wrapS & 3) | (uint32_t)(t.wrapT & 3) << 2 |
           (uint32_t)(t.magLinear ? 1u : 0u) << 4 | (uint32_t)(t.minLinear ? 1u : 0u) << 5;
}

SDL_GPUSamplerAddressMode gx_wrap(uint32_t m) {
    switch (m & 3) {
    case 0:
        return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE; // GX_CLAMP
    case 2:
        return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT; // GX_MIRROR
    default:
        return SDL_GPU_SAMPLERADDRESSMODE_REPEAT; // GX_REPEAT (3 is undefined; GX
                                                  // treats it as repeat)
    }
}

SDL_GPUSampler* sampler_for(uint32_t key) {
    if (const auto it = g_samplers.find(key); it != g_samplers.end())
        return it->second;
    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = (key & (1u << 5)) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    sci.mag_filter = (key & (1u << 4)) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = gx_wrap(key & 3);
    sci.address_mode_v = gx_wrap((key >> 2) & 3);
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler* s = SDL_CreateGPUSampler(g_dev, &sci);
    if (s == nullptr) {
        sbr_native_gpu_disable(std::string("sampler creation failed for key ")
                                   .append(std::to_string(key))
                                   .append(": ")
                                   .append(SDL_GetError())
                                   .c_str());
        return nullptr;
    }
    g_samplers.emplace(key, s);
    return s;
}

size_t g_texBytes = 0;
std::unordered_map<uint32_t, int> g_fmtHist;
SDL_GPUTexture* g_white = nullptr;

// SBR_TEX=1 opts INTO texture decode/upload. Default OFF: this path drives GPU allocations from
// guest data, and a defect here does not fail politely — it can take the device down for the whole
// process. It stays opt-in until it has run clean for a while.
bool textures_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_TEX");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

uint64_t tex_key(const SbrTexture& t) {
    if (!textures_enabled())
        return 0;
    if (t.addr == 0 || t.width == 0 || t.height == 0)
        return 0;
    return (uint64_t)t.addr << 24 ^ (uint64_t)t.format << 20 ^ (uint64_t)t.width << 10 ^ t.height;
}
std::vector<Batch> g_batches;
// SBR_BIND_LOG=<n>: log the first n batch binds, then stop.
long g_bindLog = [] {
    const char* e = std::getenv("SBR_BIND_LOG");
    return e != nullptr ? std::strtol(e, nullptr, 10) : 0L;
}();
SDL_GPUBuffer* g_vbuf = nullptr;
SDL_GPUTransferBuffer* g_vup = nullptr;
size_t g_vcap = 0;
std::vector<SbrVertex> g_verts; // accumulated this frame

bool ensure_vbuf(size_t bytes) {
    if (bytes <= g_vcap && g_vbuf != nullptr)
        return true;
    const size_t capacity = bytes + (bytes / 2) + 4096;
    SDL_GPUBufferCreateInfo bci{};
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = (Uint32)capacity;
    SDL_GPUBuffer* newBuffer = SDL_CreateGPUBuffer(g_dev, &bci);
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = (Uint32)capacity;
    SDL_GPUTransferBuffer* newUpload = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
    if (newBuffer == nullptr || newUpload == nullptr) {
        if (newBuffer != nullptr)
            SDL_ReleaseGPUBuffer(g_dev, newBuffer);
        if (newUpload != nullptr)
            SDL_ReleaseGPUTransferBuffer(g_dev, newUpload);
        sbr_native_gpu_disable(
            std::string("vertex buffer allocation failed: ").append(SDL_GetError()).c_str());
        return false;
    }
    if (g_vbuf != nullptr)
        SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
    if (g_vup != nullptr)
        SDL_ReleaseGPUTransferBuffer(g_dev, g_vup);
    g_vbuf = newBuffer;
    g_vup = newUpload;
    g_vcap = capacity;
    return true;
}

bool enabled() {
    return g_presentWindow != nullptr;
}

SDL_GPUTexture* upload_rgba(const uint8_t* rgba, uint32_t w, uint32_t h) {
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = kNativeColorFormat;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = w;
    ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(g_dev, &ci);
    if (t == nullptr) {
        sbr_native_gpu_disable(
            std::string("texture allocation failed: ").append(SDL_GetError()).c_str());
        return nullptr;
    }

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
    if (tb == nullptr) {
        sbr_native_gpu_disable(
            std::string("texture transfer allocation failed: ").append(SDL_GetError()).c_str());
        SDL_ReleaseGPUTexture(g_dev, t);
        return nullptr;
    }
    void* mapped = SDL_MapGPUTransferBuffer(g_dev, tb, false);
    if (mapped == nullptr) {
        sbr_native_gpu_disable(
            std::string("texture transfer map failed: ").append(SDL_GetError()).c_str());
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);
        SDL_ReleaseGPUTexture(g_dev, t);
        return nullptr;
    }
    std::memcpy(mapped, rgba, (size_t)w * h * 4);
    SDL_UnmapGPUTransferBuffer(g_dev, tb);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        sbr_native_gpu_disable(std::string("texture upload command acquisition failed: ")
                                   .append(SDL_GetError())
                                   .c_str());
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);
        SDL_ReleaseGPUTexture(g_dev, t);
        return nullptr;
    }
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row = w;
    src.rows_per_layer = h;
    SDL_GPUTextureRegion dst{};
    dst.texture = t;
    dst.w = w;
    dst.h = h;
    dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    // Wait so completion is bounded and a failed upload cannot masquerade as a usable cached
    // texture. SDL permits releasing a transfer buffer immediately after recording/submission and
    // retires it when safe; the fence is for failure detection, not resource-lifetime correctness.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        // A failed submit is a DEAD DEVICE, not a skipped frame. This used to log and continue,
        // which is how the run got fifteen consecutive VK_ERROR_DEVICE_LOST lines while the kernel
        // was resetting the card.
        sbr_native_gpu_disable(
            std::string("texture upload submit failed: ").append(SDL_GetError()).c_str());
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);
        SDL_ReleaseGPUTexture(g_dev, t);
        return nullptr;
    }
    const bool signalled = sbr_native_gpu_wait_fence(fence, "texture upload");
    SDL_ReleaseGPUFence(g_dev, fence);
    if (!signalled) {
        sbr_native_gpu_disable("the GPU stopped signalling fences during a texture upload");
        // The device has stopped making observable progress. Avoid issuing any more API work on
        // it; process teardown owns reclamation after the latch trips.
        return nullptr;
    }
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    return t;
}

// Decode-and-upload on first sight of a texture description.
SDL_GPUTexture* texture_for(uint64_t key, const SbrTexture& t) {
    if (key == 0)
        return g_white;
    if (const auto it = g_texs.find(key); it != g_texs.end())
        return it->second.tex;

    // Guest data is not trusted to size a GPU allocation. GX caps textures at 1024x1024, and an
    // uninitialised GXTexObj decodes to arbitrary dimensions.
    if (t.width == 0 || t.height == 0 || t.width > 1024 || t.height > 1024) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender", "implausible texture {}x{} fmt {} at 0x{:08x} — not uploaded",
                          t.width, t.height, t.format, t.addr);
        }
        g_texs.emplace(key, Tex{g_white});
        return g_white;
    }
    // Hard ceiling on how much VRAM this path may claim, so a key that varies when it should not
    // degrades into white rather than exhausting the device.
    constexpr size_t kMaxTexBytes = 192u << 20;
    if (g_texBytes + (size_t)t.width * t.height * 4 > kMaxTexBytes) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender",
                          "texture budget of {} MB exhausted at {} textures — binding "
                          "white from here",
                          kMaxTexBytes >> 20, g_texs.size());
        }
        g_texs.emplace(key, Tex{g_white});
        return g_white;
    }
    g_texBytes += (size_t)t.width * t.height * 4;
    ++g_fmtHist[t.format];

    // VALIDATE before allocating. GX caps textures at 1024x1024, and a GXTexObj that has not been
    // initialised yet decodes to arbitrary dimensions — allocating on those is how this took the
    // Vulkan device down (and, with two devices in the process, the failure surfaced on aurora's).
    if (t.width == 0 || t.height == 0 || t.width > 1024 || t.height > 1024) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            lucent::error("nrender",
                          "implausible texture {}x{} format {} at 0x{:08x} — not uploaded", t.width,
                          t.height, t.format, t.addr);
        }
        g_texs.emplace(key, Tex{g_white});
        return g_white;
    }

    std::vector<uint8_t> rgba((size_t)t.width * t.height * 4);
    SDL_GPUTexture* gt = nullptr;
    float decodedMean = -1.0f;
    if (gx_decode_texture(t.addr, t.width, t.height, t.format, t.tlut, rgba.data())) {
        // SBR_TEX_DUMP=<dir> writes every decoded texture as a PPM. A texture pipeline can be
        // "working" end to end and still be decoding garbage; looking at the decoded image is the
        // only way to tell a bad decoder from a bad material.
        if (const char* dir = std::getenv("SBR_TEX_DUMP")) {
            char path[512];
            std::snprintf(path, sizeof path, "%s/tex_%08x_%s_%ux%u.ppm", dir, t.addr,
                          gx_texture_format_name(t.format), t.width, t.height);
            if (std::FILE* f = std::fopen(path, "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", t.width, t.height);
                for (size_t i = 0; i < (size_t)t.width * t.height; ++i)
                    std::fwrite(&rgba[i * 4], 1, 3, f);
                std::fclose(f);
            }
        }
        // Log what each KEY actually decoded to. The cache is keyed by the guest description and
        // filled on FIRST sight, so a texture decoded before its data landed is black forever after
        // — and the per-draw state, which reports the description rather than the cached image,
        // cannot show that. This line is the only place the two can be compared.
        {
            uint64_t sum = 0;
            for (size_t i = 0; i < rgba.size(); i += 4)
                sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
            decodedMean = (float)((double)sum / (double)(rgba.size() / 4 * 3));
            lucent::debug("nrender", "decoded key {:016x} 0x{:08x} {} {}x{} mean {:.1f}", key,
                          t.addr, gx_texture_format_name(t.format), t.width, t.height, decodedMean);
        }
        gt = upload_rgba(rgba.data(), t.width, t.height);
    } else {
        // Report an undecodable format ONCE by name. Silently binding white here would look like a
        // lighting bug rather than a missing decoder (CLAUDE.md: no silent success-shaped stubs).
        static std::unordered_map<uint32_t, bool> warned;
        if (!warned[t.format]) {
            warned[t.format] = true;
            lucent::error("nrender", "no decoder for texture format {} ({}) at 0x{:08x} {}x{}",
                          t.format, gx_texture_format_name(t.format), t.addr, t.width, t.height);
        }
        gt = g_white;
    }
    g_texs.emplace(key, Tex{gt, decodedMean, t});
    return gt;
}

} // namespace

bool sbr_render_enabled() {
    return enabled();
}

void sbr_render_set_present_window(SDL_Window* window) {
    if (g_tried && window != g_presentWindow) {
        lucent::error("nrender", "native presentation window cannot change after initialization");
        std::abort();
    }
    g_presentWindow = window;
}

void sbr_render_set_present_aspect(unsigned width, unsigned height) {
    sbr_native_presenter_set_aspect(width, height);
}

bool sbr_render_init(int w, int h) {
    if (sbr_native_gpu_dead())
        return false; // never re-arm a path that has already faulted the device
    if (g_tried)
        return g_ok && g_w == w && g_h == h;

    if (g_presentWindow == nullptr) {
        lucent::error("nrender", "GX compatibility renderer has no presentation window");
        return false;
    }
    if (const char* approved = std::getenv("SBR_RENDER_APPROVED");
        approved == nullptr || std::strcmp(approved, "1") != 0) {
        g_tried = true;
        lucent::error("nrender", "GX compatibility renderer requires SBR_RENDER_APPROVED=1; use "
                                 "run-render.sh for the complete GPU safety policy");
        return false;
    }
    if (w <= 0 || h <= 0 || static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4 > UINT32_MAX) {
        g_tried = true;
        lucent::error("nrender", "invalid native render target extent {}x{}", w, h);
        return false;
    }
    g_tried = true;

    // Initialization mutates the process-wide renderer owner one resource at a time. Any failure
    // must unwind the entire ownership graph, including the presenter's window claim and the GPU
    // guard's device pointer, before returning to the host.
    struct InitializationAttempt {
        ~InitializationAttempt() {
            if (!committed)
                sbr_render_shutdown();
        }
        bool committed = false;
    } attempt;

    // Aurora has already SDL_Init'd video (it owns the window); this reuses that. SDL3 GPU runs its
    // own Vulkan instance independent of aurora's Dawn, so the two devices coexist in the process.
    if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        lucent::error("nrender", "SDL_InitSubSystem(VIDEO) failed: {}", SDL_GetError());
        return false;
    }
    g_dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, /*debug=*/true, nullptr);
    if (g_dev == nullptr) {
        lucent::error("nrender", "SDL_CreateGPUDevice failed: {}", SDL_GetError());
        return false;
    }
    sbr_native_gpu_guard_set_device(g_dev);
    if (!sbr_native_presenter_initialize(g_dev, g_presentWindow)) {
        lucent::error("nrender", "native swapchain claim failed: {}", SDL_GetError());
        return false;
    }

    SDL_GPUTextureCreateInfo cci{};
    cci.type = SDL_GPU_TEXTURETYPE_2D;
    cci.format = kNativeColorFormat;
    cci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    cci.width = (Uint32)w;
    cci.height = (Uint32)h;
    cci.layer_count_or_depth = 1;
    cci.num_levels = 1;
    cci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g_color = SDL_CreateGPUTexture(g_dev, &cci);

    SDL_GPUTextureCreateInfo dci = cci;
    dci.format = kNativeDepthFormat;
    dci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    g_depth = SDL_CreateGPUTexture(g_dev, &dci);

    if (g_color == nullptr || g_depth == nullptr) {
        lucent::error("nrender", "target texture create failed: {}", SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = (Uint32)(w * h * 4);
    g_dl = SDL_CreateGPUTransferBuffer(g_dev, &tbci);

    if (g_dl == nullptr) {
        lucent::error("nrender", "download transfer buffer create failed: {}", SDL_GetError());
        return false;
    }

    if (!sbr_native_gpu_pipeline_init(g_dev)) {
        lucent::error("nrender", "shader create failed: {}", SDL_GetError());
        return false;
    }

    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = SDL_GPU_FILTER_LINEAR;
    sci.mag_filter = SDL_GPU_FILTER_LINEAR;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    // GX's default wrap for J3D materials is REPEAT; per-material wrap modes arrive with the rest
    // of the material state.
    sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    g_sampler = SDL_CreateGPUSampler(g_dev, &sci);

    // One opaque white texel, bound for untextured draws so a single shader serves both cases.
    const uint8_t white[4] = {255, 255, 255, 255};
    g_white = upload_rgba(white, 1, 1);
    if (g_sampler == nullptr || g_white == nullptr) {
        lucent::error("nrender", "sampler/white texture create failed: {}", SDL_GetError());
        return false;
    }

    g_cpu.assign((size_t)w * h * 4, 0);
    g_w = w;
    g_h = h;
    g_ok = true;
    attempt.committed = true;
    lucent::info("nrender", "SDL3 GPU device and owned swapchain up ({}x{}), driver={}", w, h,
                 SDL_GetGPUDeviceDriver(g_dev));
    return true;
}

namespace {
SDL_FColor g_clear{};
int g_lastVerts = 0;
} // namespace

void sbr_render_note_copy(const NativeEfbCopyRequest& request) {
    if (!g_ok)
        return;
    const NativeEfbCopyPlan plan = sbr_native_efb_copy_plan(request, g_w, g_h);
    const size_t boundary = g_copySequence.note_copy(g_batches.size());
    g_copyPoints.push_back({boundary, plan});
    NativeEfbCopyClearDraw clearDraw{};
    if (!sbr_native_efb_copy_clear_draw(plan, clearDraw))
        return;

    // GXCopyTex(clear=true) resolves first and then clears the copy source rectangle under the
    // current PE color/alpha/depth update masks. The native backend records a frame before encoding
    // it, so represent that clear as the first ordered batch after the copy barrier. An oversized
    // clip-space triangle plus the exact clipped source scissor expresses both full and partial
    // clears without treating an empty rectangle as a full-target sentinel.
    sbr_render_tris(clearDraw.vertices, 3, clearDraw.state, clearDraw.textures, clearDraw.tev);
    if (g_batches.size() <= boundary)
        return;
    Batch& clearBatch = g_batches.back();
    clearBatch.copyClear = true;
    // TEV visualization and operation ablation diagnose game draws, not hardware maintenance.
    clearBatch.tev.alphaRef[2] = 0.0f;
    clearBatch.tev.alphaRef[3] = 0.0f;
}

// Dump an EFB-copy surface to a raw RGBA file. What the game samples from a copy destination has
// been argued about from its EFFECT on the frame; this reads the surface itself. Its ALPHA matters
// as much as its colour here — the compositing quad's TEV resolves to alpha = TEXA, so an opaque
// copy replaces the frame while a transparent one leaves it alone.
void sbr_render_dump_copy(uint32_t addr, const char* path) {
    if (!g_ok || sbr_native_gpu_dead() || path == nullptr || path[0] == '\0')
        return;
    const auto it = g_copyTex.find(addr);
    if (it == g_copyTex.end() || it->second.tex == nullptr) {
        lucent::info("nrender", "dump-copy 0x{:08x}: no copy surface registered", addr);
        return;
    }
    // The size comes from the ALLOCATION, not from a copy point. The old code scanned
    // g_copyPoints for a matching destination and fell back to a hardcoded 320x224 when it found
    // none — either of which can name a larger rect than the texture actually is, making the
    // download read past the end of it. The texture knows its own size; ask it.
    const int w = it->second.w, h = it->second.h;
    if (w <= 0 || h <= 0) {
        lucent::info("nrender", "dump-copy 0x{:08x}: surface registered with no recorded size",
                     addr);
        return;
    }
    SDL_GPUTransferBufferCreateInfo tci{};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tci.size = (Uint32)(w * h * 4);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_dev, &tci);
    if (tb == nullptr) {
        sbr_native_gpu_disable(std::string("EFB-copy dump transfer allocation failed: ")
                                   .append(SDL_GetError())
                                   .c_str());
        return;
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        sbr_native_gpu_disable(std::string("EFB-copy dump command acquisition failed: ")
                                   .append(SDL_GetError())
                                   .c_str());
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);
        return;
    }
    SDL_GPUCopyPass* cp2 = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{};
    reg.texture = it->second.tex;
    reg.w = (Uint32)w;
    reg.h = (Uint32)h;
    reg.d = 1;
    SDL_GPUTextureTransferInfo tti{};
    tti.transfer_buffer = tb;
    tti.pixels_per_row = (Uint32)w;
    tti.rows_per_layer = (Uint32)h;
    SDL_DownloadFromGPUTexture(cp2, &reg, &tti);
    SDL_EndGPUCopyPass(cp2);
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (f == nullptr) {
        sbr_native_gpu_disable("dump-copy submit failed");
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);
        return;
    }
    if (!sbr_native_gpu_wait_fence(f, "dump-copy")) {
        SDL_ReleaseGPUFence(g_dev, f);
        sbr_native_gpu_disable("the GPU stopped signalling fences during an EFB-copy dump");
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);
        return;
    }
    SDL_ReleaseGPUFence(g_dev, f);
    void* mapped = SDL_MapGPUTransferBuffer(g_dev, tb, false);
    if (mapped == nullptr) {
        sbr_native_gpu_disable(
            std::string("EFB-copy dump map failed: ").append(SDL_GetError()).c_str());
        SDL_ReleaseGPUTransferBuffer(g_dev, tb);
        return;
    }
    const uint8_t* px = static_cast<const uint8_t*>(mapped);
    double alphaSum = 0.0;
    for (int i = 0; i < w * h; ++i)
        alphaSum += px[i * 4 + 3];
    if (FILE* output = std::fopen(path, "wb")) {
        std::fwrite(px, 1, (size_t)w * h * 4, output);
        std::fclose(output);
    }
    lucent::info("nrender", "dump-copy 0x{:08x}: {}x{} -> {} (mean alpha {:.1f})", addr, w, h, path,
                 alphaSum / (double)(w * h));
    SDL_UnmapGPUTransferBuffer(g_dev, tb);
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
}

bool sbr_render_is_copy_surface(uint32_t addr) {
    const auto it = g_copyTex.find(addr);
    return it != g_copyTex.end() && it->second.tex != nullptr;
}

void sbr_render_begin(float r, float g, float b, float a) {
    if (!g_ok || sbr_native_gpu_dead())
        return;
    sbr_native_gpu_begin_frame();
    g_clear = SDL_FColor{r, g, b, a};
    g_verts.clear();
    g_batches.clear();
    g_copyPoints.clear();
    g_copySequence.reset();
}

std::map<uint32_t, long> g_unit1Use;

void sbr_render_tris(const SbrVertex* verts, int count, SbrDepthState depth,
                     const SbrTexture tex[8], const SbrTevState& tevState) {
    if (!g_ok || sbr_native_gpu_dead() || verts == nullptr || count < 3)
        return;
    if (!sbr_native_raster_submits_triangles(depth)) {
        ++g_cullAllDrawsDropped;
        g_cullAllVerticesDropped += static_cast<uint64_t>(count);
        return;
    }
    for (unsigned st = 0; st < tevState.numStages && st < 16; ++st)
        if (tevState.stage[st].texEnable && (tevState.stage[st].texmap & 7) == 1) {
            g_unit1Use[tex[1].addr] += count;
            break;
        }
    count -= count % 3;
    g_verts.insert(g_verts.end(), verts, verts + count);
    // Merge into the previous run when the state is unchanged, so honouring per-material depth
    // costs draws only where the state actually changes.
    const uint32_t first = (uint32_t)(g_verts.size() - (size_t)count);
    Batch b{depth, first, static_cast<uint32_t>(count), g_copySequence.epoch()};
    // SBR_TEX_MIRROR=1 binds unit 0's texture to ALL FOUR slots. Combined with SBR_TEXMAP_FORCE it
    // is the only clean test of the slots themselves: with identical content in every slot, forcing
    // the frame through slot 0 and through slot 1 must produce IDENTICAL images. Any difference is
    // the binding or the shader's decorations, not the material's choice of unit — which forcing
    // alone cannot separate, because a forced unit usually holds a texture the material never
    // wanted.
    static const bool mirror = [] {
        const char* e = std::getenv("SBR_TEX_MIRROR");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    for (int m = 0; m < 8; ++m) {
        const SbrTexture& t = mirror ? tex[0] : tex[m];
        b.texKey[m] = tex_key(t);
        b.texAddr[m] = t.addr;
        b.sampKey[m] = sampler_key(t);
    }
    sbr_native_pack_tev_uniform(tevState, b.tev);
    // TEV state is part of a batch's identity: two draws sharing a texture and depth state but
    // different combiners are different materials and must not merge.
    const bool same =
        !g_batches.empty() && g_copySequence.may_merge(g_batches.back().copyEpoch) &&
        g_batches.back().copyClear == b.copyClear &&
        sbr_native_gpu_pipeline_key(g_batches.back().st) == sbr_native_gpu_pipeline_key(depth) &&
        std::memcmp(g_batches.back().st.scissor, depth.scissor, sizeof depth.scissor) == 0 &&
        std::memcmp(g_batches.back().texKey, b.texKey, sizeof b.texKey) == 0 &&
        std::memcmp(g_batches.back().sampKey, b.sampKey, sizeof b.sampKey) == 0 &&
        std::memcmp(&g_batches.back().tev, &b.tev, sizeof b.tev) == 0 &&
        g_batches.back().first + g_batches.back().count == first;
    if (same) {
        g_batches.back().count += (uint32_t)count;
    } else {
        g_batches.push_back(b);
        for (int m = 0; m < 8; ++m)
            g_pendingTex[b.texKey[m]] = mirror ? tex[0] : tex[m];
    }
}

bool render_pass(uint32_t ablation, bool download, bool present);
bool render_pass_into_cpu(uint32_t ablation);

// Upload the frame's geometry, render it in one pass over a cleared target, then download for
// readback / the A/B against aurora. The whole sequence — acquire, copy pass, render pass, copy
// pass, fence, map — is the shape every later milestone keeps; only what goes INSIDE the render
// pass grows (per-material pipelines, textures, TEV).
void sbr_render_end() {
    if (!g_ok || sbr_native_gpu_dead())
        return;
    g_lastVerts = (int)g_verts.size();
    g_lastBatches = (int)g_batches.size();

    // Decode and upload any NEW textures FIRST, before this frame's command buffer exists.
    // texture_for acquires and submits its own command buffer and waits on a fence; doing that
    // while another command buffer is open (let alone inside a render pass) is what cost a
    // VK_ERROR_DEVICE_LOST. Uploads are one-time per texture, so this is not a per-frame cost.
    if (textures_enabled())
        for (const Batch& b : g_batches)
            for (int m = 0; m < 8 && !sbr_native_gpu_dead(); ++m)
                texture_for(b.texKey[m], g_pendingTex[b.texKey[m]]);
    if (sbr_native_gpu_dead())
        return;
    // Samplers too: creating one is not a command-buffer operation, but keeping every resource
    // creation outside the frame's command buffer is the rule that stopped the device losses.
    for (const Batch& b : g_batches)
        for (int m = 0; m < 8 && !sbr_native_gpu_dead(); ++m)
            sampler_for(b.sampKey[m]);
    if (sbr_native_gpu_dead())
        return;

    const size_t vbytes = g_verts.size() * sizeof(SbrVertex);
    if (vbytes > 0) {
        if (!ensure_vbuf(vbytes))
            return;
        // This upload runs every presented frame. Cycling is what makes reusing the transfer and
        // destination buffers legal while an earlier frame may still be in flight.
        void* mapped = SDL_MapGPUTransferBuffer(g_dev, g_vup, true);
        if (mapped == nullptr) {
            sbr_native_gpu_disable(
                std::string("vertex upload map failed: ").append(SDL_GetError()).c_str());
            return;
        }
        std::memcpy(mapped, g_verts.data(), vbytes);
        SDL_UnmapGPUTransferBuffer(g_dev, g_vup);
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        sbr_native_gpu_disable(std::string("vertex upload command acquisition failed: ")
                                   .append(SDL_GetError())
                                   .c_str());
        return;
    }
    if (vbytes > 0) {
        SDL_GPUCopyPass* up = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{};
        src.transfer_buffer = g_vup;
        src.offset = 0;
        SDL_GPUBufferRegion dst{};
        dst.buffer = g_vbuf;
        dst.offset = 0;
        dst.size = (Uint32)vbytes;
        SDL_UploadToGPUBuffer(up, &src, &dst, true);
        SDL_EndGPUCopyPass(up);
    }

    if (!SDL_SubmitGPUCommandBuffer(cmd)) {
        // There is no fence after a failed upload submit. Latch the owned renderer off rather than
        // issuing more work to a device whose submission path has failed.
        sbr_native_gpu_disable(
            std::string("vertex upload submit failed: ").append(SDL_GetError()).c_str());
        return;
    }
    (void)render_pass(0, false, true);
}

// ONE render of the already-uploaded geometry into g_color, downloaded into g_cpu. `ablation`
// reaches the shader as control[1] and replaces exactly one GX operation with a neutral
// reference; 0 is the real pipeline. Factored out of sbr_render_end so the attribution sweep can
// re-render the SAME frame per operation and score every variant against the SAME aurora frame —
// which is what makes the comparison drift-free (see render_compare.h).
// Draw only the first g_batchLimit batches (-1 = all). Used by the black-owner bisect, and settable
// from SBR_MAX_BATCH so a frame that renders NOTHING can be narrowed to the batch (or the batch
// TRANSITION) that eats it — with the whole rest of the pipeline untouched.
int g_batchLimit = -1;
const int g_batchLimitEnv = [] {
    const char* e = std::getenv("SBR_MAX_BATCH");
    return e != nullptr ? (int)std::strtol(e, nullptr, 10) : -1;
}();

bool render_pass(uint32_t ablation, bool download, bool present) {
    if (sbr_native_gpu_dead()) {
        sbr_native_gpu_fail_frame();
        return false;
    }
    if (download && !sbr_native_gpu_admit_pass())
        return false;
    const size_t vbytes = g_verts.size() * sizeof(SbrVertex);
    // Is the texture cache the SAME on a re-render as it was on the first render of this frame?
    // The control ablation renders untextured, and an empty/short g_texs is the only way this pass
    // binds g_white everywhere. Measured, because guessing at this cost several runs already.
    {
        static long tell = 0;
        if (!g_batches.empty() && g_verts.size() > 1000 && tell < 12) {
            ++tell;
            size_t found = 0;
            for (const Batch& b : g_batches)
                if (g_texs.find(b.texKey[0]) != g_texs.end())
                    ++found;
            lucent::info("nrender", "pass ablation={} : g_texs={} batches={} with slot0 texture={}",
                         ablation, g_texs.size(), g_batches.size(), found);
        }
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        sbr_native_gpu_disable(
            std::string("render command acquisition failed: ").append(SDL_GetError()).c_str());
        return false;
    }

    SDL_GPUColorTargetInfo cti{};
    cti.texture = g_color;
    cti.clear_color = g_clear;
    cti.load_op = SDL_GPU_LOADOP_CLEAR;
    cti.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo dsi{};
    dsi.texture = g_depth;
    dsi.clear_depth = 1.0f;
    dsi.load_op = SDL_GPU_LOADOP_CLEAR;
    // Any ordered EFB copy can split this pass and resume drawing with LOAD. The first segment must
    // therefore preserve depth; DONT_CARE followed by LOAD made post-copy depth undefined.
    dsi.store_op = SDL_GPU_STOREOP_STORE;

    const auto begin_pass = [&] {
        return sbr_native_begin_render_pass(
            [&] { return SDL_BeginGPURenderPass(cmd, &cti, 1, &dsi); },
            [&](SDL_GPURenderPass* pass) {
                const SDL_GPUBufferBinding binding{g_vbuf, 0};
                SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
            },
            vbytes > 0);
    };
    SDL_GPURenderPass* rp = begin_pass();
    size_t nextCopy = 0;
    if (vbytes > 0) {
        int drawn = 0;
        if (g_batchLimit < 0 && g_batchLimitEnv >= 0)
            g_batchLimit = g_batchLimitEnv;
        for (size_t bi = 0; bi < g_batches.size(); ++bi) {
            const Batch& b = g_batches[bi];
            if (g_batchLimit >= 0 && drawn++ >= g_batchLimit)
                break;
            // A copy captures only earlier draws, so end the pass, resolve, and resume with LOAD.
            while (nextCopy < g_copyPoints.size() && g_copyPoints[nextCopy].batchIndex <= bi) {
                { // Report the copy boundary on the same ordered batch stream.
                    static long tell = 0;
                    if (tell < 8) {
                        ++tell;
                        lucent::info("nrender", "  copy 0x{:08x} performed at batch {} of {}",
                                     g_copyPoints[nextCopy].plan.dest, bi, g_batches.size());
                    }
                }
                SDL_EndGPURenderPass(rp);
                if (perform_copy(cmd, g_copyPoints[nextCopy]) == CopyResult::Failed) {
                    SDL_CancelGPUCommandBuffer(cmd);
                    return false;
                }
                ++nextCopy;
                cti.load_op = SDL_GPU_LOADOP_LOAD;
                dsi.load_op = SDL_GPU_LOADOP_LOAD;
                rp = begin_pass();
            }
            SDL_BindGPUGraphicsPipeline(rp, sbr_native_gpu_pipeline_for(b.st));
            // The hardware clips each draw to its own scissor rect. Clamped to the target because
            // the guest rect can legitimately extend past it and SDL rejects an out-of-bounds one.
            // SBR_NO_SCISSOR=1 (DIAGNOSTIC): ignore the per-draw scissor and clip to the full
            // target. If a frame that renders NOTHING returns with this on, the scissor carried by
            // some batch is what is killing it — which is a confirm, not a correlation, because no
            // other state changes.
            static const bool noScissor = [] {
                const char* e = std::getenv("SBR_NO_SCISSOR");
                return e != nullptr && e[0] != '\0' && e[0] != '0';
            }();
            {
                SDL_Rect sc{};
                sc.x = std::clamp<int>(b.st.scissor[0], 0, g_w);
                sc.y = std::clamp<int>(b.st.scissor[1], 0, g_h);
                sc.w = std::clamp<int>(b.st.scissor[2], 0, g_w - sc.x);
                sc.h = std::clamp<int>(b.st.scissor[3], 0, g_h - sc.y);
                if (noScissor && !b.copyClear) {
                    sc.x = 0;
                    sc.y = 0;
                    sc.w = g_w;
                    sc.h = g_h;
                }
                SDL_SetGPUScissor(rp, &sc);
            }
            // All eight units every draw: the shader's sampler set is fixed by the pipeline
            // layout, so a unit a material does not use is bound to the white texel rather than
            // left dangling (an unbound descriptor is what takes the device down).
            SDL_GPUTextureSamplerBinding tsb[8]{};
            float slotMean[8] = {-2, -2, -2, -2, -2, -2, -2, -2};
            for (int m = 0; m < 8; ++m) {
                // An EFB-copy destination resolves to the surface we rendered into, not to guest
                // memory (which the hardware never writes and which decodes to zeros).
                const auto cpIt = g_copyTex.find(b.texAddr[m]);
                const auto texIt = g_texs.find(b.texKey[m]);
                tsb[m].texture = cpIt != g_copyTex.end() && cpIt->second.tex != nullptr
                                     ? cpIt->second.tex
                                     : (texIt != g_texs.end() ? texIt->second.tex : g_white);
                slotMean[m] = (texIt != g_texs.end()) ? texIt->second.mean : -2.0f;
                const auto sampIt = g_samplers.find(b.sampKey[m]);
                tsb[m].sampler = (sampIt != g_samplers.end()) ? sampIt->second : g_sampler;
            }
            // What is BOUND, per slot, for the first batches of a chosen tick. The per-draw state
            // reports the DESCRIPTION the game gave; this reports the cached image that description
            // resolved to. When the two disagree the defect is the cache key, not the parser.
            if (g_bindLog > 0) {
                --g_bindLog;
                lucent::info("nrender",
                             "bind: slot0 key={:016x} mean={:.1f} | slot1 key={:016x} "
                             "mean={:.1f} | slot2 key={:016x} mean={:.1f} | verts={}",
                             b.texKey[0], slotMean[0], b.texKey[1], slotMean[1], b.texKey[2],
                             slotMean[2], b.count);
            }
            SDL_BindGPUFragmentSamplers(rp, 0, tsb, 8);
            SbrNativeTevUniform tu = b.tev;
            // alphaRef.w — the only free component. control.y is alphaOp0, and alphaRef.z is
            // already the SBR_TEV_VIZ selector: writing the ablation id there turned every variant
            // into a visualisation mode that returns early, which is exactly what the
            // control:no-op ablation caught (it rendered untextured instead of matching baseline).
            tu.alphaRef[3] = b.copyClear ? 0.0f : static_cast<float>(ablation);
            SDL_PushGPUFragmentUniformData(cmd, 0, &tu, sizeof tu);
            SDL_DrawGPUPrimitives(rp, b.count, 1, b.first, 0);
        }
    }
    SDL_EndGPURenderPass(rp);
    // A copy emitted after the final draw has boundary == batch count, so no loop iteration can
    // encounter it. Drain that ordered suffix before readback instead of silently dropping it.
    while (nextCopy < g_copyPoints.size()) {
        if (perform_copy(cmd, g_copyPoints[nextCopy]) == CopyResult::Failed) {
            SDL_CancelGPUCommandBuffer(cmd);
            return false;
        }
        ++nextCopy;
    }

    if (present) {
        const NativePresentResult result = sbr_native_presenter_encode(
            cmd, g_color, static_cast<unsigned>(g_w), static_cast<unsigned>(g_h));
        if (result == NativePresentResult::Failed) {
            SDL_CancelGPUCommandBuffer(cmd);
            sbr_native_gpu_disable(
                std::string("native swapchain acquire failed: ").append(SDL_GetError()).c_str());
            return false;
        }
    }

    if (download) {
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureRegion reg{};
        reg.texture = g_color;
        reg.w = (Uint32)g_w;
        reg.h = (Uint32)g_h;
        reg.d = 1;
        SDL_GPUTextureTransferInfo tti{};
        tti.transfer_buffer = g_dl;
        tti.pixels_per_row = (Uint32)g_w;
        tti.rows_per_layer = (Uint32)g_h;
        SDL_DownloadFromGPUTexture(cp, &reg, &tti);
        SDL_EndGPUCopyPass(cp);
    }

    if (!download) {
        if (!SDL_SubmitGPUCommandBuffer(cmd)) {
            sbr_native_gpu_disable(
                std::string("native frame submit failed: ").append(SDL_GetError()).c_str());
            return false;
        }
        return true;
    }

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        // NEVER silent, and never continue. The old version logged and fell through: the wait was
        // skipped, g_dl was mapped anyway, and its STALE contents were copied into g_cpu, so a
        // failing submit looked exactly like a frame that legitimately rendered nothing — forever.
        // It also kept submitting, which is the part that turned a fault into a card reset.
        sbr_native_gpu_disable(
            std::string("render pass submit failed: ").append(SDL_GetError()).c_str());
        return false;
    }
    const bool signalled = sbr_native_gpu_wait_fence(fence, "render pass");
    SDL_ReleaseGPUFence(g_dev, fence);
    if (!signalled) {
        sbr_native_gpu_disable("the GPU stopped signalling fences during an offscreen render pass");
        return false; // g_cpu keeps its previous contents; nothing may treat them as fresh
    }
    void* mapped = SDL_MapGPUTransferBuffer(g_dev, g_dl, false);
    if (mapped == nullptr) {
        sbr_native_gpu_disable(
            std::string("frame download map failed: ").append(SDL_GetError()).c_str());
        return false;
    }
    std::memcpy(g_cpu.data(), mapped, g_cpu.size());
    SDL_UnmapGPUTransferBuffer(g_dev, g_dl);
    sbr_native_gpu_complete_frame();
    return true;
}

bool render_pass_into_cpu(uint32_t ablation) {
    return render_pass(ablation, true, false);
}

bool sbr_render_capture() {
    if (!g_ok || sbr_native_gpu_dead() || !sbr_native_gpu_admit_frame())
        return false;
    return render_pass_into_cpu(0);
}

int sbr_render_last_vertex_count() {
    return g_lastVerts;
}
int sbr_render_last_batch_count() {
    return g_lastBatches;
}
// Re-decode every texture that cached BLACK and report whether its guest bytes have since changed.
// Distinguishes a first-sight caching race (bytes now non-zero) from memory that is genuinely zero
// (a render target this port never writes, or a wrong address).
void sbr_render_recheck_black() {
    if (!textures_enabled())
        return;
    // STALE-CACHE SCAN, all entries: the cache fills on FIRST SIGHT and is never revalidated, while
    // the CPU tev trace decodes fresh — so a texture whose guest bytes changed after first sight
    // makes the reference predict one frame and the GPU render another. Report every entry whose
    // fresh decode no longer matches what was uploaded.
    {
        int stale = 0, scanned = 0;
        for (auto& [key, tex] : g_texs) {
            if (tex.desc.addr == 0 || tex.mean < 0.0f)
                continue;
            std::vector<uint8_t> rgba((size_t)tex.desc.width * tex.desc.height * 4);
            if (!gx_decode_texture(tex.desc.addr, tex.desc.width, tex.desc.height, tex.desc.format,
                                   tex.desc.tlut, rgba.data()))
                continue;
            ++scanned;
            uint64_t sum = 0;
            for (size_t i = 0; i < rgba.size(); i += 4)
                sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
            const float now = (float)((double)sum / (double)(rgba.size() / 4 * 3));
            if (std::fabs(now - tex.mean) > 2.0f) {
                ++stale;
                if (stale <= 12)
                    lucent::info("nrender",
                                 "STALE cache 0x{:08x} {} {}x{}: uploaded mean {:.1f}, "
                                 "guest memory now {:.1f}",
                                 tex.desc.addr, gx_texture_format_name(tex.desc.format),
                                 tex.desc.width, tex.desc.height, tex.mean, now);
            }
        }
        { // Top unit-1 bindings by vertex count, with what each decodes to.
            std::vector<std::pair<uint32_t, long>> v(g_unit1Use.begin(), g_unit1Use.end());
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            long total = 0;
            for (auto& [a, n] : v)
                total += n;
            for (size_t i = 0; i < v.size() && i < 8; ++i) {
                float mean = -1.0f;
                const char* fmt = "?";
                int w = 0, h = 0;
                for (auto& [key, t] : g_texs)
                    if (t.desc.addr == v[i].first) {
                        mean = t.mean;
                        fmt = gx_texture_format_name(t.desc.format);
                        w = t.desc.width;
                        h = t.desc.height;
                        break;
                    }
                lucent::info("nrender",
                             "  unit1 bind 0x{:08x} {} {}x{}: {} verts ({:.1f}% of unit-1 "
                             "work), decoded mean {:.1f}{}",
                             v[i].first, fmt, w, h, v[i].second,
                             total ? 100.0 * (double)v[i].second / (double)total : 0.0, mean,
                             (mean >= 0.0f && mean <= 1.0f) ? "   <- BLACK" : "");
            }
        }
        { // WHO OWNS a genuinely-empty texture buffer. J3D keeps its textures in a TEX1 section
          // with
            // a name table, and archives carry their own magics, so scanning back from the buffer
            // for the nearest section magic and the strings around it identifies the texture BY
            // NAME — which turns "some I4 128x128 is empty" into a specific asset that can be
            // looked up.
            static bool scanned = false;
            if (!scanned)
                for (auto& [key, tex] : g_texs) {
                    if (tex.desc.addr != 0x80cf0ac0u && tex.desc.addr != 0x80cfafa0u)
                        continue;
                    scanned = true;
                    static const char* const kMagic[] = {"TEX1", "BMD3", "BDL4", "RARC", "Yaz0",
                                                         "INF1", "MAT3", "SHP1", "J3D2"};
                    for (const char* m : kMagic) {
                        uint32_t best = 0;
                        for (uint32_t back = 4; back < 0x200000; back += 4) {
                            const uint32_t a = tex.desc.addr - back;
                            if (!sb_ram_fast(a))
                                break;
                            if (sb_r8(a) == (uint8_t)m[0] && sb_r8(a + 1) == (uint8_t)m[1] &&
                                sb_r8(a + 2) == (uint8_t)m[2] && sb_r8(a + 3) == (uint8_t)m[3]) {
                                best = a;
                                break;
                            }
                        }
                        if (best != 0)
                            lucent::info("nrender",
                                         "  0x{:08x}: nearest '{}' at 0x{:08x} (-0x{:x})",
                                         tex.desc.addr, m, best, tex.desc.addr - best);
                    }
                    // Printable names in the 8 KB before the buffer — J3D texture name tables sit
                    // with the headers, so the asset's own name is usually reachable from here.
                    lucent::Line l;
                    l.add("  0x{:08x} nearby strings:", tex.desc.addr);
                    std::string cur;
                    for (uint32_t back = 0x2000; back > 0; --back) {
                        const uint8_t c = sb_r8(tex.desc.addr - back);
                        if (c >= 32 && c < 127) {
                            cur.push_back((char)c);
                            continue;
                        }
                        if (cur.size() >= 4)
                            l.add(" '{}'", cur);
                        cur.clear();
                    }
                    l.flush(lucent::Level::Info, "nrender");
                }
        }
        { // What SURROUNDS a zero texture in guest memory. These buffers sit inside live
            // allocations (the zero run stops immediately at their edges), so the bytes on either
            // side belong to whatever owns them — a J3D TEX1 block, an archive header, a name
            // table. That is the cheapest available handle on WHO should have filled them.
            static bool once = false;
            for (auto& [key, tex] : g_texs) {
                if (once)
                    break;
                if (tex.mean < 0.0f || tex.mean > 1.0f || tex.desc.addr == 0)
                    continue;
                if (tex.desc.addr != 0x80cf0ac0u && tex.desc.addr != 0x80cfafa0u)
                    continue;
                for (int side = 0; side < 2; ++side) {
                    const uint32_t base =
                        side == 0 ? tex.desc.addr - 64
                                  : tex.desc.addr + (uint32_t)gx_texture_data_size(tex.desc.width,
                                                                                   tex.desc.height,
                                                                                   tex.desc.format);
                    lucent::Line l;
                    l.add("  0x{:08x} {}: {} ", tex.desc.addr,
                          gx_texture_format_name(tex.desc.format), side == 0 ? "BEFORE" : "AFTER ");
                    for (int i = 0; i < 48; ++i)
                        l.add("{:02x} ", sb_r8(base + (uint32_t)i));
                    l.add(" |");
                    for (int i = 0; i < 48; ++i) {
                        const uint8_t c = sb_r8(base + (uint32_t)i);
                        l.add("{}", (c >= 32 && c < 127) ? (char)c : '.');
                    }
                    l.flush(lucent::Level::Info, "nrender");
                }
                once = true;
            }
        }
        { // Ask AURORA whether it holds a GPU-side copy surface for each empty buffer. Aurora
            // services EFB copy destinations on the GPU and never writes guest memory, so it can
            // sample a real rendered surface at an address whose RAM is all zeros — which is
            // exactly what this port decodes. That difference is invisible in every state
            // comparison.
            static bool asked = false;
            if (!asked) {
                asked = true;
                for (uint32_t a : {0x80cf0ac0u, 0x80cfafa0u, 0x80fea480u}) {
                    const int r = sbr_aurora_has_copy_texture(a);
                    lucent::info("nrender", "  aurora copy-surface for 0x{:08x}: {}", a,
                                 r == 1 ? "YES — aurora samples a rendered surface here, this port "
                                          "decodes guest memory (zeros)"
                                 : r == 0 ? "no"
                                          : "no copy textures registered at all");
                }
            }
        }
        { // The BLACK unit-1 bindings specifically. The by-volume ranking above is dominated by
          // near
            // geometry and showed none, yet the background is black — so the population that causes
            // the defect has to be listed on its own terms rather than found in a top-N by vertex
            // count.
            std::vector<std::pair<uint32_t, long>> v(g_unit1Use.begin(), g_unit1Use.end());
            long total = 0, blackTotal = 0;
            for (auto& [a, n] : v)
                total += n;
            std::vector<std::tuple<long, uint32_t, const SbrTexture*>> black;
            for (auto& [addr, n] : v)
                for (auto& [key, t] : g_texs)
                    if (t.desc.addr == addr && t.mean >= 0.0f && t.mean <= 1.0f) {
                        black.emplace_back(n, addr, &t.desc);
                        blackTotal += n;
                        break;
                    }
            std::sort(black.begin(), black.end(),
                      [](auto& a, auto& b) { return std::get<0>(a) > std::get<0>(b); });
            lucent::info("nrender",
                         "  unit1 BLACK bindings: {} of {} distinct addresses, {:.2f}% of "
                         "unit-1 vertex work",
                         black.size(), v.size(),
                         total ? 100.0 * (double)blackTotal / (double)total : 0.0);
            for (size_t i = 0; i < black.size() && i < 8; ++i) {
                const SbrTexture* d = std::get<2>(black[i]);
                lucent::info("nrender", "    0x{:08x} {} {}x{}: {} verts", std::get<1>(black[i]),
                             gx_texture_format_name(d->format), d->width, d->height,
                             std::get<0>(black[i]));
            }
        }
        lucent::info("nrender",
                     "stale-cache scan: {} of {} decodable cached textures no longer "
                     "match their upload",
                     stale, scanned);
    }
    int rechecked = 0;
    for (auto& [key, tex] : g_texs) {
        if (tex.mean > 0.5f || tex.desc.addr == 0)
            continue;
        std::vector<uint8_t> rgba((size_t)tex.desc.width * tex.desc.height * 4);
        if (!gx_decode_texture(tex.desc.addr, tex.desc.width, tex.desc.height, tex.desc.format,
                               tex.desc.tlut, rgba.data()))
            continue;
        uint64_t sum = 0, sumA = 0;
        for (size_t i = 0; i < rgba.size(); i += 4) {
            sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
            sumA += rgba[i + 3];
        }
        const double now = (double)sum / (double)(rgba.size() / 4 * 3);
        const double nowA = (double)sumA / (double)(rgba.size() / 4);
        ++rechecked;
        // Raw source bytes too: an all-zero DECODE can mean zero source bytes or a decoder that
        // read the wrong place. Only the raw bytes separate the two.
        // How far the zero run extends either side. A zero region exactly the size of the texture,
        // surrounded by data, means the ADDRESS is right and the data was never written; a zero
        // region far larger means the buffer was never allocated or lives somewhere else entirely.
        // IS THE SOURCE ACTUALLY EMPTY? "mean" is an RGB average, so an IA-format alpha MASK —
        // legitimate content whose intensity is zero and whose alpha carries the picture — reads
        // as a black texture and would be reported as a missing one. Decoded ALPHA cannot settle
        // it either: RGB565 has no alpha channel and all-zero CMPR decodes opaque, so both report
        // alpha 255 whether or not they hold data. The only format-independent answer is the RAW
        // SOURCE BYTES, which is also what texwatch samples — so the two instruments agree by
        // construction instead of contradicting each other.
        const uint32_t needBytes =
            (uint32_t)gx_texture_data_size(tex.desc.width, tex.desc.height, tex.desc.format);
        const uint32_t need = needBytes;
        bool rawEmpty = true;
        for (uint32_t o = 0; o < needBytes && rawEmpty; ++o)
            if (sb_r8(tex.desc.addr + o) != 0)
                rawEmpty = false;
        if (!rawEmpty) {
            lucent::info("nrender",
                         "0x{:08x} {} {}x{}: decodes to RGB {:.1f} / alpha {:.1f} but "
                         "its SOURCE BYTES ARE NON-ZERO — real content (an alpha mask "
                         "or a dark texture), not a missing one",
                         tex.desc.addr, gx_texture_format_name(tex.desc.format), tex.desc.width,
                         tex.desc.height, now, nowA);
            continue;
        }
        uint32_t before = 0, after = 0;
        while (before < 0x20000 && sb_r8(tex.desc.addr - before - 1) == 0)
            ++before;
        while (after < 0x20000 && sb_r8(tex.desc.addr + need + after) == 0)
            ++after;
        // TWO read paths, side by side. This decoder reads through sb_r8 (guest EA); aurora is
        // handed a raw host pointer g_ram_base + phys for the SAME texture. Aurora renders the
        // plaza correctly from those bytes, so if sb_r8 disagrees with the raw pointer the
        // INSTRUMENT is what is zero, not the memory.
        extern u8* g_ram_base;
        const u32 phys = tex.desc.addr & 0x01FFFFFFu;
        char hex[64] = {0}, hexRaw[64] = {0};
        for (int i = 0; i < 16; ++i) {
            std::snprintf(hex + i * 3, 4, "%02x ", (unsigned)sb_r8(tex.desc.addr + i));
            std::snprintf(hexRaw + i * 3, 4, "%02x ", (unsigned)g_ram_base[phys + i]);
        }
        lucent::info("nrender",
                     "cached-black 0x{:08x} {} {}x{}: {} bytes, zero run -{}/+{}, sb_r8 [{}] "
                     "rawptr [{}] cached mean {:.1f}, "
                     "guest memory NOW decodes to {:.1f}",
                     tex.desc.addr, gx_texture_format_name(tex.desc.format), tex.desc.width,
                     tex.desc.height, need, before, after, hex, hexRaw, tex.mean, now);
    }
    if (rechecked == 0)
        lucent::info("nrender", "no cached-black textures to recheck");
}

int sbr_render_texture_count() {
    return (int)g_texs.size();
}

void sbr_render_report_formats() {
    static bool done = false;
    if (done || g_fmtHist.empty())
        return;
    done = true;
    for (const auto& [f, n] : g_fmtHist)
        lucent::info("nrender", "  texture format {} ({}) -> {} textures", f,
                     gx_texture_format_name(f), n);
}

// SBR_RENDER_DUMP=/path.rgba — write the native frame out so it can be diffed against aurora's
// SB_DUMP_FRAME of the same moment (tools/render/compare_native.py). This is the parity harness the
// whole native-render arc is verified through; without it "looks about right" is all we would have.
// Written top-left origin RGBA8, same convention as aurora's dump, so the comparison is apples to
// apples rather than a flip away from nonsense.
bool sbr_render_dump(const char* path) {
    if (!g_ok || !sbr_native_gpu_frame_readable() || path == nullptr || path[0] == '\0')
        return false;
    std::FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        lucent::error("nrender", "cannot open {} for write", path);
        return false;
    }
    const size_t n = std::fwrite(g_cpu.data(), 1, g_cpu.size(), f);
    std::fclose(f);
    lucent::info("nrender", "dumped {}x{} native frame to {} ({} bytes)", g_w, g_h, path, n);
    return n == g_cpu.size();
}

bool sbr_render_readback(uint8_t* rgba, int w, int h) {
    // The guard matters here even though this is a plain memcpy: g_cpu holds the LAST frame that
    // read back successfully, and once the device is gone that content never refreshes. Returning
    // it would feed the A/B comparator a frozen picture scored as a live one.
    if (!g_ok || sbr_native_gpu_dead() || !sbr_native_gpu_frame_readable() || w != g_w ||
        h != g_h || rgba == nullptr)
        return false;
    std::memcpy(rgba, g_cpu.data(), (size_t)w * h * 4);
    return true;
}

// ---- Operation attribution -----------------------------------------------------------------
// Each entry replaces exactly ONE GX operation with a neutral reference. The names are the
// operations themselves, so the attribution table reads as a diagnosis rather than a list of
// flags. Keep in sync with the ablation switch in shaders/geom.frag.glsl.
namespace {
const char* const kAblationName[] = {
    "baseline",         // 0 — the real pipeline
    "texgen->raw uv0",  // 1 — coordinate generation
    "texfetch->white",  // 2 — texture identity, decode, wrap and filter
    "ras->channel0",    // 3 — rasterised colour channel selection
    "tev->passthrough", // 4 — the combiner chain
    "konst->one",       // 5 — konstant selection
    "alphatest->pass",  // 6 — the alpha test
    "texmap->unit0",    // 7 — per-stage texmap routing
    // INSTRUMENT CONTROL, not an operation. The shader has no branch for this id, so it renders
    // the real pipeline. It MUST score exactly the baseline; if it does not, the sweep machinery
    // (re-render, readback, pairing) is lying and no row in the table can be believed.
    "control:no-op", // 8
    // PER-UNIT decomposition of the texmap-routing row. "texmap->unit0" pins ALL stages and so
    // reports one aggregate number; these pin exactly ONE unit and leave the rest named, so the
    // +5.9 can be attributed to the unit whose CONTENT is actually wrong instead of to routing as
    // a whole. Drift-free because every one is scored against the same aurora frame.
    "pin unit1->0", // 9
    "pin unit2->0", // 10
    "pin unit3->0", // 11
    "pin unit4->0", // 12
    "pin unit5->0", // 13
    "pin unit6->0", // 14
    "pin unit7->0", // 15
};
}

// PROVE THE GUARDS FIRE. A safety latch nobody has watched trip is indistinguishable from one
// that cannot, and the whole reason these exist is that the renderer took the machine's GPU down
// while every log line said things were fine. Run with SBR_GPU_GUARD_SELFTEST=1.
//
//   fence   — sets the wait budget to 0s, so the very first pass reports a hung GPU and latches
//             the renderer off. The run MUST continue afterwards with aurora still presenting.
//   passcap — asks for more offscreen passes in one frame than the cap allows and requires the
//             surplus to be refused.
//
// Both are checked, and a self-test that fails to trip its guard is itself a failure — it reports
// that the guard is inert rather than passing quietly.
void sbr_render_guard_selftest() {
    const char* e = std::getenv("SBR_GPU_GUARD_SELFTEST");
    if (e == nullptr || e[0] != '1')
        return;
    if (!g_ok || sbr_native_gpu_dead()) {
        lucent::error("nrender",
                      "GUARD SELF-TEST cannot run: the renderer is not initialised "
                      "(g_ok={}, gpuDead={}). This is not a pass.",
                      g_ok, sbr_native_gpu_dead());
        return;
    }

    // The pass cap, on a live device: ask for more offscreen passes in one frame than the cap
    // allows and require the surplus to be refused.
    sbr_native_gpu_reset_passes();
    int accepted = 0;
    for (int i = 0; i < sbr_native_gpu_max_passes() + 2; ++i)
        accepted += render_pass_into_cpu(0) ? 1 : 0;
    const bool capHeld = accepted == sbr_native_gpu_max_passes();
    lucent::info("nrender",
                 "GUARD SELF-TEST passcap: asked for {} passes with a cap of {}; "
                 "counter reached {}, accepted {} -> {}",
                 sbr_native_gpu_max_passes() + 2, sbr_native_gpu_max_passes(),
                 sbr_native_gpu_passes_attempted(), accepted,
                 capHeld ? "REFUSED the surplus, as required" : "NO REFUSAL — THE CAP IS INERT");
    sbr_native_gpu_reset_passes();

    // The fence timeout and the latch are NOT exercised from here, and this says so rather than
    // implying coverage it does not have. They cannot be: the budget is read once and cached, and
    // the first thing that waits on a fence is the white-texture upload inside sbr_render_init —
    // long before this runs. Exercising them is a whole run of its own:
    //
    //     SBR_GPU_FENCE_TIMEOUT=0 SBR_RENDERER=native ...
    //
    // which must print "the GPU has not signalled its fence in 0.0s" followed by "NATIVE RENDERER
    // DISABLED FOR THE REST OF THIS RUN", and must then complete normally with aurora presenting.
    // Verified on 2026-08-12; see debug_journal/2026-08-12_gpu_hang_guards.md.
    lucent::info("nrender", "GUARD SELF-TEST fence: NOT COVERED by this test — the budget is "
                            "cached before this point. Run with SBR_GPU_FENCE_TIMEOUT=0 to "
                            "exercise the timeout and the latch; the run must disable the "
                            "renderer at init and still finish.");
}

// Reported at shutdown so a run that quietly lost its GPU is distinguishable from one that never
// used it. Silence would otherwise read as success.
void sbr_render_gpu_report() {
    if (!enabled())
        return;
    lucent::info("nrender",
                 "GX_CULL_ALL admission dropped {} draw call(s), {} vertices before GPU "
                 "submission (zero means this run did not exercise the fix).",
                 g_cullAllDrawsDropped, g_cullAllVerticesDropped);
    if (sbr_native_gpu_dead())
        lucent::warn("nrender",
                     "the GX compatibility renderer was DISABLED mid-run after a GPU fault. Every "
                     "native measurement after that point is missing, not zero.");
    else if (g_ok)
        lucent::info("nrender",
                     "GX compatibility renderer ran to the end with no GPU fault; fence budget "
                     "{:.1f}s, at most {} offscreen passes per frame, rate limit "
                     "{:.1f} Hz ({} frame(s) skipped to stay under it — those are gaps "
                     "in the measurement, not zeroes).",
                     sbr_native_gpu_fence_timeout_secs(), sbr_native_gpu_max_passes(),
                     sbr_native_gpu_maximum_hz(), sbr_native_gpu_skipped_frames());
}

int sbr_render_ablation_count() {
    return (int)(sizeof kAblationName / sizeof kAblationName[0]);
}
const char* sbr_render_ablation_name(int id) {
    return (id >= 0 && id < sbr_render_ablation_count()) ? kAblationName[id] : "?";
}

// Re-render the frame already uploaded by sbr_render_end with one operation ablated. The result
// lands in g_cpu, so sbr_render_readback returns it exactly as for the baseline.
bool sbr_render_ablation_render(int id) {
    if (!g_ok || sbr_native_gpu_dead() || id <= 0 || id >= sbr_render_ablation_count())
        return false;
    return render_pass_into_cpu((uint32_t)id);
}

// WHICH BATCH PAINTS THIS PIXEL BLACK? Bisect the batch list, re-rendering the already-uploaded
// frame with a prefix of it, until the first batch that turns the sample pixel black is found.
// This answers the question directly instead of inferring it from state — everything in this arc
// that reasoned indirectly about "the draw responsible" was eventually retracted.
//
// It proves itself: with the FULL batch list the sample must be black (or there is nothing to
// attribute), and with ZERO batches it must be the clear colour. Both are asserted before the
// bisect runs, so an instrument that is simply reporting a constant cannot pass silently.
void sbr_render_report_black_owner(int px, int py) {
    if (!g_ok || g_batches.empty())
        return;
    const size_t off = ((size_t)py * (size_t)g_w + (size_t)px) * 4;
    const auto isBlack = [&] {
        return g_cpu[off] < 16 && g_cpu[off + 1] < 16 && g_cpu[off + 2] < 16;
    };
    const int total = (int)g_batches.size();

    g_batchLimit = 0;
    if (!render_pass_into_cpu(0)) {
        g_batchLimit = -1;
        return;
    }
    const bool emptyBlack = isBlack();
    const uint8_t c0 = g_cpu[off], c1 = g_cpu[off + 1], c2 = g_cpu[off + 2];
    g_batchLimit = total;
    if (!render_pass_into_cpu(0)) {
        g_batchLimit = -1;
        return;
    }
    const bool fullBlack = isBlack();
    if (emptyBlack || !fullBlack) {
        lucent::info("nrender",
                     "black-owner bisect INVALID at ({},{}): with no batches the pixel "
                     "is [{} {} {}] (black={}), with all {} batches black={} — the "
                     "instrument cannot attribute anything here",
                     px, py, c0, c1, c2, emptyBlack, total, fullBlack);
        g_batchLimit = -1;
        return;
    }
    int lo = 0, hi = total; // lo: not black yet, hi: black
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        g_batchLimit = mid;
        if (!render_pass_into_cpu(0)) {
            g_batchLimit = -1;
            return;
        }
        (isBlack() ? hi : lo) = mid;
    }
    g_batchLimit = -1;
    const Batch& b = g_batches[(size_t)hi - 1];
    lucent::Line l;
    l.add("black-owner at ({},{}): batch {} of {} — verts {} stages {} zt{}w{}f{} blend{}/{}/{} "
          "cull{} scis[{} {} {} {}] tex",
          px, py, hi - 1, total, b.count, b.tev.control[0], b.st.test, b.st.write, b.st.func,
          b.st.blend, b.st.srcFac, b.st.dstFac, b.st.cull, b.st.scissor[0], b.st.scissor[1],
          b.st.scissor[2], b.st.scissor[3]);
    for (int m = 0; m < 4; ++m) {
        const auto it = g_texs.find(b.texKey[m]);
        l.add(" u{}=0x{:08x}/mean{:.0f}", m, it != g_texs.end() ? it->second.desc.addr : 0u,
              it != g_texs.end() ? it->second.mean : -1.0f);
    }
    l.flush(lucent::Level::Info, "nrender");
}

void sbr_render_shutdown() noexcept {
    // Mark the renderer unavailable before releasing anything. This makes repeated calls no-ops
    // from every public rendering entry point even if shutdown is reached after a partial init.
    g_ok = false;

    if (g_dev != nullptr) {
        for (auto& [address, copy] : g_copyTex) {
            (void)address;
            if (copy.tex != nullptr)
                SDL_ReleaseGPUTexture(g_dev, copy.tex);
        }
        for (auto& [key, texture] : g_texs) {
            (void)key;
            // Failed/empty decodes intentionally alias the singleton white texture.
            if (texture.tex != nullptr && texture.tex != g_white)
                SDL_ReleaseGPUTexture(g_dev, texture.tex);
        }
        for (auto& [key, sampler] : g_samplers) {
            (void)key;
            if (sampler != nullptr)
                SDL_ReleaseGPUSampler(g_dev, sampler);
        }
        if (g_white != nullptr)
            SDL_ReleaseGPUTexture(g_dev, g_white);
        if (g_sampler != nullptr)
            SDL_ReleaseGPUSampler(g_dev, g_sampler);
        if (g_vup != nullptr)
            SDL_ReleaseGPUTransferBuffer(g_dev, g_vup);
        if (g_vbuf != nullptr)
            SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
        if (g_dl != nullptr)
            SDL_ReleaseGPUTransferBuffer(g_dev, g_dl);
        if (g_depth != nullptr)
            SDL_ReleaseGPUTexture(g_dev, g_depth);
        if (g_color != nullptr)
            SDL_ReleaseGPUTexture(g_dev, g_color);
    }

    sbr_native_gpu_pipeline_shutdown();
    sbr_native_presenter_shutdown();
    sbr_native_gpu_guard_set_device(nullptr);
    if (g_dev != nullptr)
        SDL_DestroyGPUDevice(g_dev);

    g_dev = nullptr;
    g_color = nullptr;
    g_depth = nullptr;
    g_dl = nullptr;
    g_white = nullptr;
    g_sampler = nullptr;
    g_vbuf = nullptr;
    g_vup = nullptr;
    g_vcap = 0;
    g_w = 0;
    g_h = 0;
    g_lastBatches = 0;
    g_lastVerts = 0;
    g_cullAllDrawsDropped = 0;
    g_cullAllVerticesDropped = 0;
    g_batchLimit = -1;
    g_presentWindow = nullptr;

    g_copyTex.clear();
    g_copyPoints.clear();
    g_copySequence.reset();
    g_texs.clear();
    g_pendingTex.clear();
    g_samplers.clear();
    g_texBytes = 0;
    g_fmtHist.clear();
    g_batches.clear();
    g_verts.clear();
    g_cpu.clear();
    g_unit1Use.clear();
}
