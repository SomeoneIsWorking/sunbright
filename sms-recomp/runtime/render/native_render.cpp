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
// MILESTONE LADDER (each A/B'd against aurora): [0] device + clear + readback  ->  pass-through geom
// ->  vertex transform  ->  TEV  ->  textures  ->  EFB. This file is at milestone 0.
//
//   SBR_SDLGPU=1        stand the device up and, once per frame, clear the native target to the GX
//                       copy-clear colour and read it back (proves the plumbing; nothing drawn yet).

#include "native_render.h"

#include <lucent/log.h>

#include <SDL3/SDL.h>

#include "gx_texture.h"
#include "intrinsics.h"

#include "shaders/geom_vert_spv.h"
#include "shaders/geom_frag_spv.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <map>
#include <unordered_map>

// Aurora's GPU-side copy-surface registry. See the call site in sbr_render_recheck_black.
extern "C" int sbr_aurora_has_copy_texture(unsigned int guestAddr);
#include <vector>
#include <vector>

namespace {

constexpr SDL_GPUTextureFormat kColorFmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
constexpr SDL_GPUTextureFormat kDepthFmt = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;

SDL_GPUDevice*         g_dev   = nullptr;
SDL_GPUTexture*        g_color = nullptr;   // offscreen EFB-sized colour target
SDL_GPUTexture*        g_depth = nullptr;   // depth target
SDL_GPUTransferBuffer* g_dl    = nullptr;   // download staging (w*h*4)
int  g_w = 0, g_h = 0;
int  g_lastBatches = 0;
bool g_tried = false, g_ok = false;

std::vector<uint8_t> g_cpu;   // last frame read back, top-left origin RGBA8

// Geometry path (milestone 1). Vertices arrive in CLIP space already (the frontend does posMtx +
// projection on the CPU), so one pipeline serves every draw for now — no per-material state yet.
SDL_GPUShader* g_vs = nullptr;
SDL_GPUShader* g_fs = nullptr;
// One pipeline per distinct depth state. GX varies depth state per MATERIAL, and SDL3 GPU has no
// dynamic depth state, so the state has to be baked into a pipeline and the scene split into runs.
std::unordered_map<uint32_t, SDL_GPUGraphicsPipeline*> g_pipes;

// Mirrors the TevBlock uniform in geom.frag.glsl. std140: every member is a 16-byte vector, so the
// selectors are stored one per component rather than packed.
struct TevUniform {
    int32_t cSel[16][4];
    int32_t cOp[16][4];
    int32_t aSel[16][4];
    int32_t aOp[16][4];
    int32_t dest[16][4];
    float   konst[16][4];
    float   regInit[4][4];
    int32_t control[4];
    float   alphaRef[4];
};

struct Batch {
    SbrDepthState st;
    uint32_t first, count;
    // One entry per GX texture unit. 0 = untextured (a 1x1 white texel is bound so one shader
    // serves both cases and no branch is needed in the TEV loop).
    uint64_t texKey[8];
    uint32_t texAddr[8];   // guest address, so a bind can resolve to an EFB-copy surface
    uint32_t sampKey[8];   // wrap/filter modes: part of the material, not of the texture data
    TevUniform tev;
};

// ---- EFB copy -> texture ----
// Textures produced by resolving the render target, keyed by the guest destination address the
// game will bind. Guest memory is never written, exactly as on hardware+aurora; a bind of the
// address resolves here instead of decoding zeros.
std::unordered_map<uint32_t, SDL_GPUTexture*> g_copyTex;
struct CopyPoint { size_t batchIndex; uint32_t dest; int sx, sy, sw, sh, dw, dh; };
std::vector<CopyPoint> g_copyPoints;

// Resolve the current render target region into the texture for `dest`, creating it on demand.
// Runs BETWEEN render passes — a blit is not a render-pass operation, and the pass must have ended
// for the target's contents to be defined.
void perform_copy(SDL_GPUCommandBuffer* cmd, const CopyPoint& cp) {
    SDL_GPUTexture*& dst = g_copyTex[cp.dest];
    if (dst == nullptr) {
        SDL_GPUTextureCreateInfo ci{};
        ci.type = SDL_GPU_TEXTURETYPE_2D;
        ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ci.width = (Uint32)std::max(cp.dw, 1);
        ci.height = (Uint32)std::max(cp.dh, 1);
        ci.layer_count_or_depth = 1;
        ci.num_levels = 1;
        // COLOR_TARGET as well as SAMPLER: a blit writes the destination as a render target.
        ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        dst = SDL_CreateGPUTexture(g_dev, &ci);
        if (dst == nullptr) {
            lucent::error("nrender", "EFB copy: CreateGPUTexture {}x{} failed: {}", cp.dw, cp.dh,
                          SDL_GetError());
            return;
        }
        lucent::info("nrender", "EFB copy -> texture 0x{:08x}: EFB [{},{} {}x{}] -> {}x{}",
                     cp.dest, cp.sx, cp.sy, cp.sw, cp.sh, cp.dw, cp.dh);
    }
    SDL_GPUBlitInfo bi{};
    bi.source.texture = g_color;
    bi.source.x = (Uint32)std::clamp(cp.sx, 0, g_w);
    bi.source.y = (Uint32)std::clamp(cp.sy, 0, g_h);
    bi.source.w = (Uint32)std::clamp(cp.sw, 1, g_w - (int)bi.source.x);
    bi.source.h = (Uint32)std::clamp(cp.sh, 1, g_h - (int)bi.source.y);
    bi.destination.texture = dst;
    bi.destination.w = (Uint32)std::max(cp.dw, 1);
    bi.destination.h = (Uint32)std::max(cp.dh, 1);
    bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
    bi.filter = SDL_GPU_FILTER_LINEAR;
    SDL_BlitGPUTexture(cmd, &bi);
}


// Decoded textures, keyed by the guest description. Textures are immutable for a given
// (address, format, size), so decoding once and caching is not an optimisation but a requirement:
// decoding a 1024-texel CMPR image per draw would dominate the frame.
struct Tex {
    SDL_GPUTexture* tex = nullptr;
    float mean = -1.0f;   // decoded brightness, so what is BOUND can be compared with what the
                          // per-draw state SAYS is bound — the last unin­strumented link
    SbrTexture desc{};    // kept so a cached-black texture can be re-decoded from guest memory
                          // later: the cache fills on FIRST SIGHT, and a texture seen before its
                          // data landed stays black forever with nothing to say so
};
std::unordered_map<uint64_t, Tex> g_texs;
std::unordered_map<uint64_t, SbrTexture> g_pendingTex;   // descriptions seen this frame
SDL_GPUSampler* g_sampler = nullptr;                     // REPEAT/LINEAR, the fallback
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
    case 0:  return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;   // GX_CLAMP
    case 2:  return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT; // GX_MIRROR
    default: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;          // GX_REPEAT (3 is undefined; GX
                                                                // treats it as repeat)
    }
}

SDL_GPUSampler* sampler_for(uint32_t key) {
    if (const auto it = g_samplers.find(key); it != g_samplers.end()) return it->second;
    SDL_GPUSamplerCreateInfo sci{};
    sci.min_filter = (key & (1u << 5)) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    sci.mag_filter = (key & (1u << 4)) ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sci.address_mode_u = gx_wrap(key & 3);
    sci.address_mode_v = gx_wrap((key >> 2) & 3);
    sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    SDL_GPUSampler* s = SDL_CreateGPUSampler(g_dev, &sci);
    if (s == nullptr) {
        // Falling back silently would sample every material with the wrong wrap and read as a UV
        // bug, so say which combination failed (CLAUDE.md: no silent success-shaped stubs).
        lucent::error("nrender", "sampler create failed for wrapS={} wrapT={} mag={} min={}: {}",
                      key & 3, (key >> 2) & 3, (key >> 4) & 1, (key >> 5) & 1, SDL_GetError());
        s = g_sampler;
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
// GX konst selector -> colour. Values 0..7 are the constant ramp 1.0, 7/8 ... 1/8; 12..31 select a
// component or channel of one of the four konst registers. The ramp is exact, not approximated.
void pack_konst(const SbrTevState& tev, unsigned stage, float out[4]) {
    sbr_tev_konst(tev, stage, out);
}

bool textures_enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_TEX");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

uint64_t tex_key(const SbrTexture& t) {
    if (!textures_enabled()) return 0;
    if (t.addr == 0 || t.width == 0 || t.height == 0) return 0;
    return (uint64_t)t.addr << 24 ^ (uint64_t)t.format << 20 ^ (uint64_t)t.width << 10 ^ t.height;
}
std::vector<Batch> g_batches;
// SBR_BIND_LOG=<n>: log the first n batch binds, then stop.
long g_bindLog = [] { const char* e = std::getenv("SBR_BIND_LOG"); return e != nullptr ? std::strtol(e, nullptr, 10) : 0L; }();
SDL_GPUBuffer*           g_vbuf = nullptr;
SDL_GPUTransferBuffer*   g_vup  = nullptr;
size_t                   g_vcap = 0;
std::vector<SbrVertex>   g_verts;   // accumulated this frame

// The resource counts are NOT optional. SDL3 GPU builds the pipeline layout from them, not by
// reflecting the SPIR-V, so a fragment shader that declares a sampler while the create-info says
// zero produces a descriptor-set mismatch — which manifests as VK_ERROR_DEVICE_LOST, and (with
// aurora's Dawn device in the same process) surfaces on aurora's device, blaming the wrong
// subsystem. This was the actual cause of the device losses, not the texture data.
SDL_GPUShader* make_shader(const void* code, size_t bytes, SDL_GPUShaderStage stage,
                           Uint32 numSamplers, Uint32 numUniformBuffers) {
    SDL_GPUShaderCreateInfo ci{};
    ci.code = (const Uint8*)code;
    ci.code_size = bytes;
    ci.entrypoint = "main";
    ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
    ci.stage = stage;
    ci.num_samplers = numSamplers;
    ci.num_storage_textures = 0;
    ci.num_storage_buffers = 0;
    ci.num_uniform_buffers = numUniformBuffers;
    return SDL_CreateGPUShader(g_dev, &ci);
}

SDL_GPUCompareOp gx_compare(uint8_t f) {
    // GXCompare -> backend compare. The transform in scene.cpp maps GC's [-1,0] clip depth to
    // [0,1] PRESERVING order (nearer stays smaller), so each GX function keeps its meaning.
    switch (f & 7) {
    case 0: return SDL_GPU_COMPAREOP_NEVER;
    case 1: return SDL_GPU_COMPAREOP_LESS;
    case 2: return SDL_GPU_COMPAREOP_EQUAL;
    case 3: return SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    case 4: return SDL_GPU_COMPAREOP_GREATER;
    case 5: return SDL_GPU_COMPAREOP_NOT_EQUAL;
    case 6: return SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;
    default: return SDL_GPU_COMPAREOP_ALWAYS;
    }
}

uint32_t depth_key(SbrDepthState d) {
    return (uint32_t)(d.test ? 1 : 0) << 24 | (uint32_t)(d.func & 7) << 20 |
           (uint32_t)(d.write ? 1 : 0) << 16 | (uint32_t)(d.blend & 3) << 8 |
           (uint32_t)(d.srcFac & 7) << 4 | (uint32_t)(d.dstFac & 7) |
           (uint32_t)(d.colorUpdate ? 1 : 0) << 28 | (uint32_t)(d.alphaUpdate ? 1 : 0) << 29 |
           (uint32_t)(d.cull & 3) << 30;
}

// GXBlendFactor -> backend factor. GX names the factors in terms of the SOURCE and DESTINATION
// colours exactly as the backend does, so this is a direct mapping, not an approximation.
SDL_GPUBlendFactor gx_blend_factor(uint8_t f) {
    switch (f & 7) {
    case 0: return SDL_GPU_BLENDFACTOR_ZERO;
    case 1: return SDL_GPU_BLENDFACTOR_ONE;
    case 2: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
    case 3: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    case 5: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
    default: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
    }
}

SDL_GPUGraphicsPipeline* pipeline_for(SbrDepthState d);

void ensure_vbuf(size_t bytes) {
    if (bytes <= g_vcap && g_vbuf != nullptr) return;
    if (g_vbuf) SDL_ReleaseGPUBuffer(g_dev, g_vbuf);
    if (g_vup)  SDL_ReleaseGPUTransferBuffer(g_dev, g_vup);
    g_vcap = bytes + (bytes / 2) + 4096;
    SDL_GPUBufferCreateInfo bci{};
    bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bci.size = (Uint32)g_vcap;
    g_vbuf = SDL_CreateGPUBuffer(g_dev, &bci);
    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = (Uint32)g_vcap;
    g_vup = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
}

bool enabled() {
    static int v = -1;
    if (v < 0) {
        const char* e = std::getenv("SBR_SDLGPU");
        v = (e != nullptr && e[0] != '\0' && e[0] != '0') ? 1 : 0;
    }
    return v == 1;
}

SDL_GPUTexture* upload_rgba(const uint8_t* rgba, uint32_t w, uint32_t h) {
    SDL_GPUTextureCreateInfo ci{};
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = kColorFmt;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width = w; ci.height = h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* t = SDL_CreateGPUTexture(g_dev, &ci);
    if (t == nullptr) return nullptr;

    SDL_GPUTransferBufferCreateInfo tbci{};
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = w * h * 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_dev, &tbci);
    if (void* m = SDL_MapGPUTransferBuffer(g_dev, tb, false)) {
        std::memcpy(m, rgba, (size_t)w * h * 4);
        SDL_UnmapGPUTransferBuffer(g_dev, tb);
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src{};
    src.transfer_buffer = tb;
    src.pixels_per_row = w;
    src.rows_per_layer = h;
    SDL_GPUTextureRegion dst{};
    dst.texture = t; dst.w = w; dst.h = h; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    // WAIT before freeing the staging buffer. Releasing it straight after submit frees memory the
    // GPU is still reading, which showed up as a VK_ERROR_DEVICE_LOST — and, because two Vulkan
    // devices share this process, the loss surfaced on AURORA's device, pointing at the wrong
    // subsystem entirely. Uploads are one-time per texture, so the wait costs nothing steady-state.
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        // NEVER silent. This path used to fall through with no else: the wait was skipped, g_dl was
        // mapped anyway, and its STALE contents were copied into g_cpu — so a failing submit looked
        // exactly like a frame that legitimately rendered nothing, forever. sbr_render_readback is
        // only a memcpy of g_cpu, so every downstream consumer (coverage, the A/B, frame dumps)
        // reports a frozen picture with no indication anything failed.
        lucent::error("nrender", "submit+fence FAILED: {} — g_cpu keeps its previous contents, so "
                                 "coverage/dumps from here are STALE, not empty",
                      SDL_GetError());
    }
    if (fence != nullptr) {
        SDL_WaitForGPUFences(g_dev, true, &fence, 1);
        SDL_ReleaseGPUFence(g_dev, fence);
    }
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
    return t;
}

// Decode-and-upload on first sight of a texture description.
SDL_GPUTexture* texture_for(uint64_t key, const SbrTexture& t) {
    if (key == 0) return g_white;
    if (const auto it = g_texs.find(key); it != g_texs.end()) return it->second.tex;

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
            lucent::error("nrender", "texture budget of {} MB exhausted at {} textures — binding "
                                     "white from here", kMaxTexBytes >> 20, g_texs.size());
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
            lucent::error("nrender", "implausible texture {}x{} format {} at 0x{:08x} — not uploaded",
                          t.width, t.height, t.format, t.addr);
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
            for (size_t i = 0; i < rgba.size(); i += 4) sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
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

void pack_tev(const SbrTevState& tev, TevUniform& u, const SbrTexture* tex) {
    const int n = (int)std::min<uint32_t>(tev.numStages, 16);
    u.control[0] = n;
    for (int i = 0; i < 16; ++i) {
        const SbrTevStage& st = tev.stage[i];
        u.cSel[i][0] = st.cA; u.cSel[i][1] = st.cB; u.cSel[i][2] = st.cC; u.cSel[i][3] = st.cD;
        u.cOp[i][0] = st.cBias; u.cOp[i][1] = st.cSub; u.cOp[i][2] = st.cClamp; u.cOp[i][3] = st.cScale;
        u.aSel[i][0] = st.aA; u.aSel[i][1] = st.aB; u.aSel[i][2] = st.aC; u.aSel[i][3] = st.aD;
        u.aOp[i][0] = st.aBias; u.aOp[i][1] = st.aSub; u.aOp[i][2] = st.aClamp; u.aOp[i][3] = st.aScale;
        u.dest[i][0] = st.cDest; u.dest[i][1] = st.aDest; u.dest[i][2] = st.texEnable;
        // Which unit this stage samples and which generated coordinate it samples with — two
        // independent selectors from RAS1_TREF, not one.
        // Stages name units 1-3, not just 0, and routing them there is the correct mechanism —
        // but it is still OPT-IN because six textures decode BLACK (their guest memory is zero) and
        // five of them sit on unit 1 across the terrain, so honouring the name blacks the scene.
        // Pinning to unit 0 is NOT a fix; it is the better-looking of two known-wrong behaviours
        // until those buffers are filled. See debug_journal/2026-07-23_native_texgen_and_texmap_bisect.md.
        // Which units are routed to the unit the stage NAMES, as a bitmask, so the question "which
        // unit blacks the frame" is one run per bit instead of one run per theory. SBR_TEXMAP_UNITS
        // is the mask (bit m = honour stages naming unit m); SBR_TEXMAP_NAMED=1 is the shorthand for
        // all eight. A unit not in the mask falls back to 0, which is the pinned behaviour.
        static const uint32_t unitMask = [] {
            if (const char* m = std::getenv("SBR_TEXMAP_UNITS"))
                return (uint32_t)std::strtoul(m, nullptr, 0);
            const char* e = std::getenv("SBR_TEXMAP_NAMED");
            return (e != nullptr && e[0] != '\0' && e[0] != '0') ? 0xFFu : 0x1u;
        }();
        // SBR_TEXMAP_FORCE=<unit> routes EVERY stage to one unit. That asks a different question
        // from the mask: not "which stages are wrong" but "does this SLOT sample at all". A slot
        // whose binding or SPIR-V decoration is wrong returns the same value everywhere, so forcing
        // the whole frame through it separates a broken slot from a wrongly-routed stage.
        static const int32_t forceUnit = [] {
            const char* f = std::getenv("SBR_TEXMAP_FORCE");
            return f != nullptr ? (int32_t)std::strtol(f, nullptr, 0) : -1;
        }();
        const int32_t map = (int32_t)(st.texmap & 7);
        int32_t unit = forceUnit >= 0 ? (forceUnit & 7)
                                      : (((unitMask >> map) & 1) ? map : 0);
        u.dest[i][3] = unit | (int32_t)(st.texcoord & 3) << 8 |
                       (int32_t)(st.rasChannel & 7) << 16;
        pack_konst(tev, (unsigned)i, u.konst[i]);
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) u.regInit[r][c] = tev.reg[r][c];
    // SBR_ALPHATEST=0 forces both comparisons to ALWAYS — the measurement switch for the cutout
    // path, so a landed feature has a before/after number rather than an argument.
    static const bool alphaTest = [] {
        const char* e = std::getenv("SBR_ALPHATEST");
        return !(e != nullptr && e[0] == '0');
    }();
    u.control[1] = alphaTest ? tev.alphaOp0 : 7;
    u.control[2] = alphaTest ? tev.alphaOp1 : 7;
    u.control[3] = tev.alphaLogic;
    u.alphaRef[0] = (float)tev.alphaRef0;
    u.alphaRef[1] = (float)tev.alphaRef1;
    // SBR_TEV_VIZ replaces the shaded output with an intermediate, per pixel, so a defect can be
    // SEEN where it happens instead of inferred from a whole-frame score. 1 = the raw sample of the
    // unit stage 0 names; 2 = that sample's alpha as grey; 3 = the coordinate stage 0 samples with.
    // A frame that is black under viz 1 says the SAMPLE is black; one that is black only in the
    // shaded output says the combiner is.
    static const float viz = [] {
        const char* e = std::getenv("SBR_TEV_VIZ");
        return e != nullptr ? (float)std::strtol(e, nullptr, 10) : 0.0f;
    }();
    u.alphaRef[2] = viz;
}

SDL_GPUGraphicsPipeline* pipeline_for(SbrDepthState d) {
    const uint32_t key = depth_key(d);
    if (const auto it = g_pipes.find(key); it != g_pipes.end()) return it->second;

    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot = 0;
    vbd.pitch = sizeof(SbrVertex);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    // Position, colour, then the four generated coordinates as two vec4s (uv0|uv1, uv2|uv3) —
    // packing them in pairs keeps the attribute count down without changing what the shader reads.
    SDL_GPUVertexAttribute va[5]{};
    va[0].location = 0; va[0].buffer_slot = 0;
    va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[0].offset = 0;
    va[1].location = 1; va[1].buffer_slot = 0;
    va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[1].offset = 16;
    va[2].location = 2; va[2].buffer_slot = 0;
    va[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[2].offset = 32;
    va[3].location = 3; va[3].buffer_slot = 0;
    va[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[3].offset = 48;
    va[4].location = 4; va[4].buffer_slot = 0;
    va[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[4].offset = 64;   // colour channel 1

    SDL_GPUColorTargetDescription ctd{};
    ctd.format = kColorFmt;
    // GX_BM_BLEND is the only blend mode the scene actually uses; LOGIC/SUBTRACT are left
    // unblended rather than approximated, and will announce themselves if they ever appear.
    // cmode0's colour/alpha write enables. GX runs the pipeline and then discards the write; the
    // backend equivalent is a write mask, NOT skipping the draw (depth still updates).
    ctd.blend_state.enable_color_write_mask = true;
    ctd.blend_state.color_write_mask =
        (SDL_GPUColorComponentFlags)((d.colorUpdate ? (SDL_GPU_COLORCOMPONENT_R |
                                                       SDL_GPU_COLORCOMPONENT_G |
                                                       SDL_GPU_COLORCOMPONENT_B) : 0) |
                                     (d.alphaUpdate ? SDL_GPU_COLORCOMPONENT_A : 0));
    if (d.blend == 1) {
        ctd.blend_state.enable_blend = true;
        ctd.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        ctd.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        ctd.blend_state.src_color_blendfactor = gx_blend_factor(d.srcFac);
        ctd.blend_state.dst_color_blendfactor = gx_blend_factor(d.dstFac);
        ctd.blend_state.src_alpha_blendfactor = gx_blend_factor(d.srcFac);
        ctd.blend_state.dst_alpha_blendfactor = gx_blend_factor(d.dstFac);
    }

    SDL_GPUGraphicsPipelineCreateInfo pci{};
    pci.vertex_shader = g_vs;
    pci.fragment_shader = g_fs;
    pci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pci.vertex_input_state.vertex_buffer_descriptions = &vbd;
    pci.vertex_input_state.num_vertex_buffers = 1;
    pci.vertex_input_state.vertex_attributes = va;
    pci.vertex_input_state.num_vertex_attributes = 5;
    // SBR_RENDER_WIREFRAME=1 draws edges instead of filled triangles. A few large planes can
    // occlude an entire correct scene behind them, which is indistinguishable from "the scene is
    // missing" in a filled render — wireframe separates those two cases, so it is a diagnostic
    // worth keeping rather than a one-off.
    const char* wf = std::getenv("SBR_RENDER_WIREFRAME");
    pci.rasterizer_state.fill_mode = (wf != nullptr && wf[0] != '\0' && wf[0] != '0')
                                         ? SDL_GPU_FILLMODE_LINE : SDL_GPU_FILLMODE_FILL;
    // GX cull, from GENMODE. CULL_ALL cannot be expressed as a rasterizer state — such draws are
    // dropped before they reach a pipeline (see sbr_render_tris), so the mode chosen here for it is
    // irrelevant. GX winding is CLOCKWISE (aurora: gx.cpp:720).
    pci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    pci.rasterizer_state.cull_mode = d.cull == 1   ? SDL_GPU_CULLMODE_FRONT
                                     : d.cull == 2 ? SDL_GPU_CULLMODE_BACK
                                     : d.cull == 3 ? SDL_GPU_CULLMODE_BACK
                                                   : SDL_GPU_CULLMODE_NONE;
    pci.target_info.color_target_descriptions = &ctd;
    pci.target_info.num_color_targets = 1;
    pci.target_info.depth_stencil_format = kDepthFmt;
    pci.target_info.has_depth_stencil_target = true;
    // The GAME's depth state, not an assumed one. The sky is drawn with WRITE DISABLED; honouring
    // that is what stops it stamping depth over the whole scene.
    pci.depth_stencil_state.enable_depth_test = d.test != 0;
    pci.depth_stencil_state.enable_depth_write = d.write != 0;
    pci.depth_stencil_state.compare_op = gx_compare(d.func);

    SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(g_dev, &pci);
    if (p == nullptr) {
        // A pipeline that fails to compile must not be silently skipped: the batch would vanish and
        // read as a render bug (CLAUDE.md FAIL FAST).
        lucent::error("nrender", "pipeline create failed for test={} func={} write={} blend={} "
                                 "src={} dst={}: {}",
                      d.test, d.func, d.write, d.blend, d.srcFac, d.dstFac, SDL_GetError());
        std::abort();
    }
    g_pipes.emplace(key, p);
    return p;
}

} // namespace

bool sbr_render_enabled() { return enabled(); }

bool sbr_render_init(int w, int h) {
    if (g_tried) return g_ok && g_w == w && g_h == h;
    g_tried = true;

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

    SDL_GPUTextureCreateInfo cci{};
    cci.type = SDL_GPU_TEXTURETYPE_2D;
    cci.format = kColorFmt;
    cci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    cci.width = (Uint32)w;
    cci.height = (Uint32)h;
    cci.layer_count_or_depth = 1;
    cci.num_levels = 1;
    cci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    g_color = SDL_CreateGPUTexture(g_dev, &cci);

    SDL_GPUTextureCreateInfo dci = cci;
    dci.format = kDepthFmt;
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

    // One pipeline: pass-through clip-space verts, depth test on, alpha blending off. The per-GX
    // z-mode and blend state become per-material state as the TEV milestone lands; the depth test
    // itself is not optional, because without it the last drawable submitted paints over the whole
    // scene (measured: a uniform 100%-coverage fill of one object's colour).
    g_vs = make_shader(kGeomVertSpv, sizeof(kGeomVertSpv), SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    g_fs = make_shader(kGeomFragSpv, sizeof(kGeomFragSpv), SDL_GPU_SHADERSTAGE_FRAGMENT, 8, 1);
    if (g_vs == nullptr || g_fs == nullptr) {
        lucent::error("nrender", "shader create failed: {}", SDL_GetError());
        return false;
    }
    SDL_GPUVertexBufferDescription vbd{};
    vbd.slot = 0;
    vbd.pitch = sizeof(SbrVertex);
    vbd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    // Position, colour, then the four generated coordinates as two vec4s (uv0|uv1, uv2|uv3) —
    // packing them in pairs keeps the attribute count down without changing what the shader reads.
    SDL_GPUVertexAttribute va[5]{};
    va[0].location = 0; va[0].buffer_slot = 0;
    va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[0].offset = 0;
    va[1].location = 1; va[1].buffer_slot = 0;
    va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[1].offset = 16;
    va[2].location = 2; va[2].buffer_slot = 0;
    va[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[2].offset = 32;
    va[3].location = 3; va[3].buffer_slot = 0;
    va[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[3].offset = 48;
    va[4].location = 4; va[4].buffer_slot = 0;
    va[4].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4; va[4].offset = 64;   // colour channel 1

    // Pipelines are created ON DEMAND per depth state (pipeline_for), not once here: GX varies
    // depth state per material and the backend cannot change it dynamically.
    g_pipes.clear();

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
    lucent::info("nrender", "SDL3 GPU device up ({}x{}), driver={}", w, h,
                 SDL_GetGPUDeviceDriver(g_dev));
    return true;
}

namespace { SDL_FColor g_clear{}; int g_lastVerts = 0; }

void sbr_render_note_copy(uint32_t dest, int sx, int sy, int sw, int sh, int dw, int dh) {
    if (!g_ok) return;
    g_copyPoints.push_back({g_batches.size(), dest, sx, sy, sw, sh, dw, dh});
}

// Dump an EFB-copy surface to a raw RGBA file. What the game samples from a copy destination has
// been argued about from its EFFECT on the frame; this reads the surface itself. Its ALPHA matters
// as much as its colour here — the compositing quad's TEV resolves to alpha = TEXA, so an opaque
// copy replaces the frame while a transparent one leaves it alone.
void sbr_render_dump_copy(uint32_t addr, const char* path) {
    const auto it = g_copyTex.find(addr);
    if (it == g_copyTex.end() || it->second == nullptr) {
        lucent::info("nrender", "dump-copy 0x{:08x}: no copy surface registered", addr);
        return;
    }
    int w = 0, h = 0;
    for (const CopyPoint& cp : g_copyPoints)
        if (cp.dest == addr) { w = cp.dw; h = cp.dh; }
    if (w <= 0 || h <= 0) { w = 320; h = 224; }
    SDL_GPUTransferBufferCreateInfo tci{};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tci.size = (Uint32)(w * h * 4);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_dev, &tci);
    if (tb == nullptr) return;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    SDL_GPUCopyPass* cp2 = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{};
    reg.texture = it->second; reg.w = (Uint32)w; reg.h = (Uint32)h; reg.d = 1;
    SDL_GPUTextureTransferInfo tti{};
    tti.transfer_buffer = tb; tti.pixels_per_row = (Uint32)w; tti.rows_per_layer = (Uint32)h;
    SDL_DownloadFromGPUTexture(cp2, &reg, &tti);
    SDL_EndGPUCopyPass(cp2);
    SDL_GPUFence* f = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (f != nullptr) { SDL_WaitForGPUFences(g_dev, true, &f, 1); SDL_ReleaseGPUFence(g_dev, f); }
    if (void* m = SDL_MapGPUTransferBuffer(g_dev, tb, false)) {
        const uint8_t* px = (const uint8_t*)m;
        double sa = 0.0;
        for (int i = 0; i < w * h; ++i) sa += px[i * 4 + 3];
        if (FILE* fp = std::fopen(path, "wb")) {
            std::fwrite(px, 1, (size_t)w * h * 4, fp);
            std::fclose(fp);
        }
        lucent::info("nrender", "dump-copy 0x{:08x}: {}x{} -> {} (mean alpha {:.1f})", addr, w, h,
                     path, sa / (double)(w * h));
        SDL_UnmapGPUTransferBuffer(g_dev, tb);
    }
    SDL_ReleaseGPUTransferBuffer(g_dev, tb);
}

bool sbr_render_is_copy_surface(uint32_t addr) {
    const auto it = g_copyTex.find(addr);
    return it != g_copyTex.end() && it->second != nullptr;
}

void sbr_render_begin(float r, float g, float b, float a) {
    if (!g_ok) return;
    g_clear = SDL_FColor{r, g, b, a};
    g_verts.clear();
    g_batches.clear();
    g_copyPoints.clear();
}

// Which textures the stages that NAME unit 1 actually bind, weighted by vertices. Unit 1 is the
// whole routing deficit (per-unit ablation: pinning it alone recovers +6.0, units 2-7 recover
// nothing), and we bind the same address aurora does for 99.35% of those stages — so the question
// is which CONTENT lands there. Vertices, not draws, because a 60-vertex horizon strip and a
// 26k-vertex batch are not equally responsible for a black region.
std::map<uint32_t, long> g_unit1Use;

void sbr_render_tris(const SbrVertex* verts, int count, SbrDepthState depth, const SbrTexture tex[8],
                     const SbrTevState& tevState) {
    if (!g_ok || verts == nullptr || count < 3) return;
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
    Batch b{depth, first, (uint32_t)count, {}, {}, {}};
    // SBR_TEX_MIRROR=1 binds unit 0's texture to ALL FOUR slots. Combined with SBR_TEXMAP_FORCE it
    // is the only clean test of the slots themselves: with identical content in every slot, forcing
    // the frame through slot 0 and through slot 1 must produce IDENTICAL images. Any difference is
    // the binding or the shader's decorations, not the material's choice of unit — which forcing
    // alone cannot separate, because a forced unit usually holds a texture the material never wanted.
    static const bool mirror = [] {
        const char* e = std::getenv("SBR_TEX_MIRROR");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    for (int m = 0; m < 8; ++m) {
        const SbrTexture& t = mirror ? tex[0] : tex[m];
        b.texKey[m]  = tex_key(t);
        b.texAddr[m] = t.addr;
        b.sampKey[m] = sampler_key(t);
    }
    pack_tev(tevState, b.tev, mirror ? nullptr : tex);
    // TEV state is part of a batch's identity: two draws sharing a texture and depth state but
    // different combiners are different materials and must not merge.
    const bool same = !g_batches.empty() &&
                      depth_key(g_batches.back().st) == depth_key(depth) &&
                      std::memcmp(g_batches.back().st.scissor, depth.scissor,
                                  sizeof depth.scissor) == 0 &&
                      std::memcmp(g_batches.back().texKey, b.texKey, sizeof b.texKey) == 0 &&
                      std::memcmp(g_batches.back().sampKey, b.sampKey, sizeof b.sampKey) == 0 &&
                      std::memcmp(&g_batches.back().tev, &b.tev, sizeof b.tev) == 0 &&
                      g_batches.back().first + g_batches.back().count == first;
    if (same) {
        g_batches.back().count += (uint32_t)count;
    } else {
        g_batches.push_back(b);
        for (int m = 0; m < 8; ++m) g_pendingTex[b.texKey[m]] = mirror ? tex[0] : tex[m];
    }
}

void render_pass_into_cpu(uint32_t ablation);

// Upload the frame's geometry, render it in one pass over a cleared target, then download for
// readback / the A/B against aurora. The whole sequence — acquire, copy pass, render pass, copy
// pass, fence, map — is the shape every later milestone keeps; only what goes INSIDE the render
// pass grows (per-material pipelines, textures, TEV).
void sbr_render_end() {
    if (!g_ok) return;
    g_lastVerts = (int)g_verts.size();
    g_lastBatches = (int)g_batches.size();

    // Decode and upload any NEW textures FIRST, before this frame's command buffer exists.
    // texture_for acquires and submits its own command buffer and waits on a fence; doing that
    // while another command buffer is open (let alone inside a render pass) is what cost a
    // VK_ERROR_DEVICE_LOST. Uploads are one-time per texture, so this is not a per-frame cost.
    if (textures_enabled())
        for (const Batch& b : g_batches)
            for (int m = 0; m < 8; ++m) texture_for(b.texKey[m], g_pendingTex[b.texKey[m]]);
    // Samplers too: creating one is not a command-buffer operation, but keeping every resource
    // creation outside the frame's command buffer is the rule that stopped the device losses.
    for (const Batch& b : g_batches)
        for (int m = 0; m < 8; ++m) sampler_for(b.sampKey[m]);

    const size_t vbytes = g_verts.size() * sizeof(SbrVertex);
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        lucent::error("nrender", "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return;
    }
    if (vbytes > 0) {
        ensure_vbuf(vbytes);
        if (void* m = SDL_MapGPUTransferBuffer(g_dev, g_vup, false)) {
            std::memcpy(m, g_verts.data(), vbytes);
            SDL_UnmapGPUTransferBuffer(g_dev, g_vup);
        }
        SDL_GPUCopyPass* up = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src{}; src.transfer_buffer = g_vup; src.offset = 0;
        SDL_GPUBufferRegion dst{}; dst.buffer = g_vbuf; dst.offset = 0; dst.size = (Uint32)vbytes;
        SDL_UploadToGPUBuffer(up, &src, &dst, false);
        SDL_EndGPUCopyPass(up);
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    render_pass_into_cpu(0);
    // IS THE PASS REPRODUCIBLE? Render the identical pass twice and compare. This separates "the
    // ablation changed the picture" from "re-rendering changes the picture", which the sweep's
    // control caught but could not localise.
    {
        static long tell = 0;
        if (g_verts.size() > 1000 && tell < 3) {
            ++tell;
            auto sum = [](const std::vector<uint8_t>& p) {
                unsigned long long h = 1469598103934665603ULL;
                for (size_t i = 0; i < p.size(); i += 4)
                    h = (h ^ (p[i] + 3u * p[i + 1] + 7u * p[i + 2])) * 1099511628211ULL;
                return h;
            };
            const unsigned long long a = sum(g_cpu);
            render_pass_into_cpu(0);
            const unsigned long long b = sum(g_cpu);
            lucent::info("nrender", "pass reproducibility: first {:016x} second {:016x} -> {}",
                         a, b, a == b ? "IDENTICAL" : "DIFFERENT (re-render is not reproducible)");
        }
    }
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


void render_pass_into_cpu(uint32_t ablation) {
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
                if (g_texs.find(b.texKey[0]) != g_texs.end()) ++found;
            lucent::info("nrender", "pass ablation={} : g_texs={} batches={} with slot0 texture={}",
                         ablation, g_texs.size(), g_batches.size(), found);
        }
    }
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(g_dev);
    if (cmd == nullptr) {
        lucent::error("nrender", "AcquireGPUCommandBuffer failed: {}", SDL_GetError());
        return;
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
    dsi.store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &cti, 1, &dsi);
    if (vbytes > 0) {
        SDL_GPUBufferBinding vbTmp{};
        (void)vbTmp;
        SDL_GPUBufferBinding vb{}; vb.buffer = g_vbuf; vb.offset = 0;
        SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
        int drawn = 0;
        if (g_batchLimit < 0 && g_batchLimitEnv >= 0) g_batchLimit = g_batchLimitEnv;
        size_t nextCopy = 0;
        for (size_t bi = 0; bi < g_batches.size(); ++bi) {
            const Batch& b = g_batches[bi];
            if (g_batchLimit >= 0 && drawn++ >= g_batchLimit) break;
            // A copy captures only what was drawn BEFORE it, and a blit is not a render-pass
            // operation — so the pass ends here, the resolve happens, and a new pass RESUMES over
            // the same target (LOAD, not CLEAR, or everything drawn so far is thrown away).
            while (nextCopy < g_copyPoints.size() && g_copyPoints[nextCopy].batchIndex <= bi) {
                {   // WHEN is the copy taken? A copy at batch 0 of 146 captures an essentially
                    // empty target, so the surface the game then samples is whatever the clear
                    // left — the composite can look plausible while the copy is meaningless.
                    static long tell = 0;
                    if (tell < 8) {
                        ++tell;
                        lucent::info("nrender", "  copy 0x{:08x} performed at batch {} of {}",
                                     g_copyPoints[nextCopy].dest, bi, g_batches.size());
                    }
                }
                SDL_EndGPURenderPass(rp);
                perform_copy(cmd, g_copyPoints[nextCopy]);
                ++nextCopy;
                cti.load_op = SDL_GPU_LOADOP_LOAD;
                dsi.load_op = SDL_GPU_LOADOP_LOAD;
                dsi.store_op = SDL_GPU_STOREOP_STORE;
                rp = SDL_BeginGPURenderPass(cmd, &cti, 1, &dsi);
            }
            SDL_BindGPUGraphicsPipeline(rp, pipeline_for(b.st));
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
                if (noScissor) { sc.x = 0; sc.y = 0; sc.w = g_w; sc.h = g_h; }
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
                tsb[m].texture = cpIt != g_copyTex.end() && cpIt->second != nullptr
                                     ? cpIt->second
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
                lucent::info("nrender", "bind: slot0 key={:016x} mean={:.1f} | slot1 key={:016x} "
                                        "mean={:.1f} | slot2 key={:016x} mean={:.1f} | verts={}",
                             b.texKey[0], slotMean[0], b.texKey[1], slotMean[1],
                             b.texKey[2], slotMean[2], b.count);
            }
            SDL_BindGPUFragmentSamplers(rp, 0, tsb, 8);
            TevUniform tu = b.tev;
            // alphaRef.w — the only free component. control.y is alphaOp0, and alphaRef.z is
            // already the SBR_TEV_VIZ selector: writing the ablation id there turned every variant
            // into a visualisation mode that returns early, which is exactly what the
            // control:no-op ablation caught (it rendered untextured instead of matching baseline).
            tu.alphaRef[3] = (float)ablation;
            SDL_PushGPUFragmentUniformData(cmd, 0, &tu, sizeof tu);
            SDL_DrawGPUPrimitives(rp, b.count, 1, b.first, 0);
        }
    }
    SDL_EndGPURenderPass(rp);

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion reg{};
    reg.texture = g_color; reg.w = (Uint32)g_w; reg.h = (Uint32)g_h; reg.d = 1;
    SDL_GPUTextureTransferInfo tti{};
    tti.transfer_buffer = g_dl; tti.pixels_per_row = (Uint32)g_w; tti.rows_per_layer = (Uint32)g_h;
    SDL_DownloadFromGPUTexture(cp, &reg, &tti);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence == nullptr) {
        // NEVER silent. This path used to fall through with no else: the wait was skipped, g_dl was
        // mapped anyway, and its STALE contents were copied into g_cpu — so a failing submit looked
        // exactly like a frame that legitimately rendered nothing, forever. sbr_render_readback is
        // only a memcpy of g_cpu, so every downstream consumer (coverage, the A/B, frame dumps)
        // reports a frozen picture with no indication anything failed.
        lucent::error("nrender", "submit+fence FAILED: {} — g_cpu keeps its previous contents, so "
                                 "coverage/dumps from here are STALE, not empty",
                      SDL_GetError());
    }
    if (fence != nullptr) {
        SDL_WaitForGPUFences(g_dev, true, &fence, 1);
        SDL_ReleaseGPUFence(g_dev, fence);
    }
    if (void* mapped = SDL_MapGPUTransferBuffer(g_dev, g_dl, false)) {
        std::memcpy(g_cpu.data(), mapped, g_cpu.size());
        SDL_UnmapGPUTransferBuffer(g_dev, g_dl);
    }
}

int sbr_render_last_vertex_count() { return g_lastVerts; }
int sbr_render_last_batch_count() { return g_lastBatches; }
// Re-decode every texture that cached BLACK and report whether its guest bytes have since changed.
// Distinguishes a first-sight caching race (bytes now non-zero) from memory that is genuinely zero
// (a render target this port never writes, or a wrong address).
void sbr_render_recheck_black() {
    if (!textures_enabled()) return;
    // STALE-CACHE SCAN, all entries: the cache fills on FIRST SIGHT and is never revalidated, while
    // the CPU tev trace decodes fresh — so a texture whose guest bytes changed after first sight
    // makes the reference predict one frame and the GPU render another. Report every entry whose
    // fresh decode no longer matches what was uploaded.
    {
        int stale = 0, scanned = 0;
        for (auto& [key, tex] : g_texs) {
            if (tex.desc.addr == 0 || tex.mean < 0.0f) continue;
            std::vector<uint8_t> rgba((size_t)tex.desc.width * tex.desc.height * 4);
            if (!gx_decode_texture(tex.desc.addr, tex.desc.width, tex.desc.height, tex.desc.format,
                                   tex.desc.tlut, rgba.data()))
                continue;
            ++scanned;
            uint64_t sum = 0;
            for (size_t i = 0; i < rgba.size(); i += 4) sum += rgba[i] + rgba[i + 1] + rgba[i + 2];
            const float now = (float)((double)sum / (double)(rgba.size() / 4 * 3));
            if (std::fabs(now - tex.mean) > 2.0f) {
                ++stale;
                if (stale <= 12)
                    lucent::info("nrender", "STALE cache 0x{:08x} {} {}x{}: uploaded mean {:.1f}, "
                                            "guest memory now {:.1f}",
                                 tex.desc.addr, gx_texture_format_name(tex.desc.format),
                                 tex.desc.width, tex.desc.height, tex.mean, now);
            }
        }
        {   // Top unit-1 bindings by vertex count, with what each decodes to.
        std::vector<std::pair<uint32_t, long>> v(g_unit1Use.begin(), g_unit1Use.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        long total = 0;
        for (auto& [a, n] : v) total += n;
        for (size_t i = 0; i < v.size() && i < 8; ++i) {
            float mean = -1.0f;
            const char* fmt = "?";
            int w = 0, h = 0;
            for (auto& [key, t] : g_texs)
                if (t.desc.addr == v[i].first) {
                    mean = t.mean; fmt = gx_texture_format_name(t.desc.format);
                    w = t.desc.width; h = t.desc.height; break;
                }
            lucent::info("nrender", "  unit1 bind 0x{:08x} {} {}x{}: {} verts ({:.1f}% of unit-1 "
                                    "work), decoded mean {:.1f}{}",
                         v[i].first, fmt, w, h, v[i].second,
                         total ? 100.0 * (double)v[i].second / (double)total : 0.0, mean,
                         (mean >= 0.0f && mean <= 1.0f) ? "   <- BLACK" : "");
        }
    }
    {   // WHO OWNS a genuinely-empty texture buffer. J3D keeps its textures in a TEX1 section with
        // a name table, and archives carry their own magics, so scanning back from the buffer for
        // the nearest section magic and the strings around it identifies the texture BY NAME —
        // which turns "some I4 128x128 is empty" into a specific asset that can be looked up.
        static bool scanned = false;
        if (!scanned)
            for (auto& [key, tex] : g_texs) {
                if (tex.desc.addr != 0x80cf0ac0u && tex.desc.addr != 0x80cfafa0u) continue;
                scanned = true;
                static const char* const kMagic[] = {"TEX1", "BMD3", "BDL4", "RARC", "Yaz0",
                                                     "INF1", "MAT3", "SHP1", "J3D2"};
                for (const char* m : kMagic) {
                    uint32_t best = 0;
                    for (uint32_t back = 4; back < 0x200000; back += 4) {
                        const uint32_t a = tex.desc.addr - back;
                        if (!sb_ram_fast(a)) break;
                        if (sb_r8(a) == (uint8_t)m[0] && sb_r8(a + 1) == (uint8_t)m[1] &&
                            sb_r8(a + 2) == (uint8_t)m[2] && sb_r8(a + 3) == (uint8_t)m[3]) {
                            best = a; break;
                        }
                    }
                    if (best != 0)
                        lucent::info("nrender", "  0x{:08x}: nearest '{}' at 0x{:08x} (-0x{:x})",
                                     tex.desc.addr, m, best, tex.desc.addr - best);
                }
                // Printable names in the 8 KB before the buffer — J3D texture name tables sit with
                // the headers, so the asset's own name is usually reachable from here.
                lucent::Line l;
                l.add("  0x{:08x} nearby strings:", tex.desc.addr);
                std::string cur;
                for (uint32_t back = 0x2000; back > 0; --back) {
                    const uint8_t c = sb_r8(tex.desc.addr - back);
                    if (c >= 32 && c < 127) { cur.push_back((char)c); continue; }
                    if (cur.size() >= 4) l.add(" '{}'", cur);
                    cur.clear();
                }
                l.flush(lucent::Level::Info, "nrender");
            }
    }
    {   // What SURROUNDS a zero texture in guest memory. These buffers sit inside live
        // allocations (the zero run stops immediately at their edges), so the bytes on either side
        // belong to whatever owns them — a J3D TEX1 block, an archive header, a name table. That is
        // the cheapest available handle on WHO should have filled them.
        static bool once = false;
        for (auto& [key, tex] : g_texs) {
            if (once) break;
            if (tex.mean < 0.0f || tex.mean > 1.0f || tex.desc.addr == 0) continue;
            if (tex.desc.addr != 0x80cf0ac0u && tex.desc.addr != 0x80cfafa0u) continue;
            for (int side = 0; side < 2; ++side) {
                const uint32_t base = side == 0 ? tex.desc.addr - 64
                                                : tex.desc.addr + (uint32_t)gx_texture_data_size(
                                                      tex.desc.width, tex.desc.height,
                                                      tex.desc.format);
                lucent::Line l;
                l.add("  0x{:08x} {}: {} ", tex.desc.addr,
                      gx_texture_format_name(tex.desc.format), side == 0 ? "BEFORE" : "AFTER ");
                for (int i = 0; i < 48; ++i) l.add("{:02x} ", sb_r8(base + (uint32_t)i));
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
    {   // Ask AURORA whether it holds a GPU-side copy surface for each empty buffer. Aurora
        // services EFB copy destinations on the GPU and never writes guest memory, so it can
        // sample a real rendered surface at an address whose RAM is all zeros — which is exactly
        // what this port decodes. That difference is invisible in every state comparison.
        static bool asked = false;
        if (!asked) {
            asked = true;
            for (uint32_t a : {0x80cf0ac0u, 0x80cfafa0u, 0x80fea480u}) {
                const int r = sbr_aurora_has_copy_texture(a);
                lucent::info("nrender", "  aurora copy-surface for 0x{:08x}: {}", a,
                             r == 1 ? "YES — aurora samples a rendered surface here, this port "
                                      "decodes guest memory (zeros)"
                                    : r == 0 ? "no" : "no copy textures registered at all");
            }
        }
    }
    {   // The BLACK unit-1 bindings specifically. The by-volume ranking above is dominated by near
        // geometry and showed none, yet the background is black — so the population that causes the
        // defect has to be listed on its own terms rather than found in a top-N by vertex count.
        std::vector<std::pair<uint32_t, long>> v(g_unit1Use.begin(), g_unit1Use.end());
        long total = 0, blackTotal = 0;
        for (auto& [a, n] : v) total += n;
        std::vector<std::tuple<long, uint32_t, const SbrTexture*>> black;
        for (auto& [addr, n] : v)
            for (auto& [key, t] : g_texs)
                if (t.desc.addr == addr && t.mean >= 0.0f && t.mean <= 1.0f) {
                    black.emplace_back(n, addr, &t.desc); blackTotal += n; break;
                }
        std::sort(black.begin(), black.end(), [](auto& a, auto& b) {
            return std::get<0>(a) > std::get<0>(b);
        });
        lucent::info("nrender", "  unit1 BLACK bindings: {} of {} distinct addresses, {:.2f}% of "
                                "unit-1 vertex work", black.size(), v.size(),
                     total ? 100.0 * (double)blackTotal / (double)total : 0.0);
        for (size_t i = 0; i < black.size() && i < 8; ++i) {
            const SbrTexture* d = std::get<2>(black[i]);
            lucent::info("nrender", "    0x{:08x} {} {}x{}: {} verts", std::get<1>(black[i]),
                         gx_texture_format_name(d->format), d->width, d->height,
                         std::get<0>(black[i]));
        }
    }
    lucent::info("nrender", "stale-cache scan: {} of {} decodable cached textures no longer "
                                "match their upload", stale, scanned);
    }
    int rechecked = 0;
    for (auto& [key, tex] : g_texs) {
        if (tex.mean > 0.5f || tex.desc.addr == 0) continue;
        std::vector<uint8_t> rgba((size_t)tex.desc.width * tex.desc.height * 4);
        if (!gx_decode_texture(tex.desc.addr, tex.desc.width, tex.desc.height, tex.desc.format,
                               tex.desc.tlut, rgba.data()))
            continue;
        uint64_t sum = 0, sumA = 0;
        for (size_t i = 0; i < rgba.size(); i += 4) {
            sum  += rgba[i] + rgba[i + 1] + rgba[i + 2];
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
        const uint32_t needBytes = (uint32_t)gx_texture_data_size(tex.desc.width, tex.desc.height,
                                                                  tex.desc.format);
        const uint32_t need = needBytes;
        bool rawEmpty = true;
        for (uint32_t o = 0; o < needBytes && rawEmpty; ++o)
            if (sb_r8(tex.desc.addr + o) != 0) rawEmpty = false;
        if (!rawEmpty) {
            lucent::info("nrender", "0x{:08x} {} {}x{}: decodes to RGB {:.1f} / alpha {:.1f} but "
                                    "its SOURCE BYTES ARE NON-ZERO — real content (an alpha mask "
                                    "or a dark texture), not a missing one",
                         tex.desc.addr, gx_texture_format_name(tex.desc.format), tex.desc.width,
                         tex.desc.height, now, nowA);
            continue;
        }
        uint32_t before = 0, after = 0;
        while (before < 0x20000 && sb_r8(tex.desc.addr - before - 1) == 0) ++before;
        while (after < 0x20000 && sb_r8(tex.desc.addr + need + after) == 0) ++after;
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
        lucent::info("nrender", "cached-black 0x{:08x} {} {}x{}: {} bytes, zero run -{}/+{}, sb_r8 [{}] rawptr [{}] cached mean {:.1f}, "
                                "guest memory NOW decodes to {:.1f}", tex.desc.addr,
                     gx_texture_format_name(tex.desc.format), tex.desc.width, tex.desc.height,
                     need, before, after, hex, hexRaw, tex.mean, now);
    }
    if (rechecked == 0) lucent::info("nrender", "no cached-black textures to recheck");
}

int sbr_render_texture_count() { return (int)g_texs.size(); }

void sbr_render_report_formats() {
    static bool done = false;
    if (done || g_fmtHist.empty()) return;
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
    if (!g_ok || path == nullptr || path[0] == '\0') return false;
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
    if (!g_ok || w != g_w || h != g_h || rgba == nullptr) return false;
    std::memcpy(rgba, g_cpu.data(), (size_t)w * h * 4);
    return true;
}


// ---- Operation attribution -----------------------------------------------------------------
// Each entry replaces exactly ONE GX operation with a neutral reference. The names are the
// operations themselves, so the attribution table reads as a diagnosis rather than a list of
// flags. Keep in sync with the ablation switch in shaders/geom.frag.glsl.
namespace {
const char* const kAblationName[] = {
    "baseline",             // 0 — the real pipeline
    "texgen->raw uv0",      // 1 — coordinate generation
    "texfetch->white",      // 2 — texture identity, decode, wrap and filter
    "ras->channel0",        // 3 — rasterised colour channel selection
    "tev->passthrough",     // 4 — the combiner chain
    "konst->one",           // 5 — konstant selection
    "alphatest->pass",      // 6 — the alpha test
    "texmap->unit0",        // 7 — per-stage texmap routing
    // INSTRUMENT CONTROL, not an operation. The shader has no branch for this id, so it renders
    // the real pipeline. It MUST score exactly the baseline; if it does not, the sweep machinery
    // (re-render, readback, pairing) is lying and no row in the table can be believed.
    "control:no-op",        // 8
    // PER-UNIT decomposition of the texmap-routing row. "texmap->unit0" pins ALL stages and so
    // reports one aggregate number; these pin exactly ONE unit and leave the rest named, so the
    // +5.9 can be attributed to the unit whose CONTENT is actually wrong instead of to routing as
    // a whole. Drift-free because every one is scored against the same aurora frame.
    "pin unit1->0",         // 9
    "pin unit2->0",         // 10
    "pin unit3->0",         // 11
    "pin unit4->0",         // 12
    "pin unit5->0",         // 13
    "pin unit6->0",         // 14
    "pin unit7->0",         // 15
};
}

int sbr_render_ablation_count() { return (int)(sizeof kAblationName / sizeof kAblationName[0]); }
const char* sbr_render_ablation_name(int id) {
    return (id >= 0 && id < sbr_render_ablation_count()) ? kAblationName[id] : "?";
}

// Re-render the frame already uploaded by sbr_render_end with one operation ablated. The result
// lands in g_cpu, so sbr_render_readback returns it exactly as for the baseline.
bool sbr_render_ablation_render(int id) {
    if (!g_ok || id <= 0 || id >= sbr_render_ablation_count()) return false;
    render_pass_into_cpu((uint32_t)id);
    return true;
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
    if (!g_ok || g_batches.empty()) return;
    const size_t off = ((size_t)py * (size_t)g_w + (size_t)px) * 4;
    const auto isBlack = [&] {
        return g_cpu[off] < 16 && g_cpu[off + 1] < 16 && g_cpu[off + 2] < 16;
    };
    const int total = (int)g_batches.size();

    g_batchLimit = 0;
    render_pass_into_cpu(0);
    const bool emptyBlack = isBlack();
    const uint8_t c0 = g_cpu[off], c1 = g_cpu[off + 1], c2 = g_cpu[off + 2];
    g_batchLimit = total;
    render_pass_into_cpu(0);
    const bool fullBlack = isBlack();
    if (emptyBlack || !fullBlack) {
        lucent::info("nrender", "black-owner bisect INVALID at ({},{}): with no batches the pixel "
                                "is [{} {} {}] (black={}), with all {} batches black={} — the "
                                "instrument cannot attribute anything here",
                     px, py, c0, c1, c2, emptyBlack, total, fullBlack);
        g_batchLimit = -1;
        return;
    }
    int lo = 0, hi = total;          // lo: not black yet, hi: black
    while (hi - lo > 1) {
        const int mid = (lo + hi) / 2;
        g_batchLimit = mid;
        render_pass_into_cpu(0);
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
