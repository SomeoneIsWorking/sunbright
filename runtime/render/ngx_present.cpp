// ngx_present — N7: the native ngx frame as the LIVE on-screen image. Renders the
// captured J3D mesh (ngx_j3d_shape.cpp snapshot) into a PERSISTENT Dolphin
// AbstractTexture each frame, on the video thread; the fork's Presenter substitutes
// that texture for the XFB (RenderXFBToScreen + ProcessFrameDumping), so both the
// on-screen present AND the headless frame dump show the native frame, Dolphin-free
// in the render path.
//
// Unlike the one-shot /ngxrender probe (vk_mesh.cpp, which rebuilds everything per
// call), this is a PERSISTENT renderer: shaders are compiled ONCE and the pipeline +
// decoded-texture caches survive across frames (per-frame glslang compilation would
// be unusable). It owns its own command buffer + fence and submits to the graphics
// queue with a fence wait — isolated from Dolphin's StateTracker, correct on the
// video thread (ordered before Dolphin's later present submit), at the cost of a
// per-frame GPU bubble (acceptable for this bring-up path; gated by SUNBRIGHT_NGX_PRESENT).

#include "tex_decode.h"
#include "tev_shader.h"
#include "glsl_compile.h"
#include "j2d_types.h"
#include "../ngx/ngx_render_data.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
extern "C" unsigned ngx_tev_cc_dbg(int);
extern "C" void sb_ngx_get_clear(float out[4]);   // game's GXSetCopyClear colour (ngx_j3d_shape.cpp)
extern "C" int g_ngx_tevdbg;       // render-debug mode (tev_shader.cpp); /ngxdbg sets it
extern "C" int sb_ngx_tevdbg();
// Runtime tev_index isolation (/ngxonly, /ngxskip): -2 = use the env var, -1 = off, >=0 = a ti.
int g_ngx_only_ti = -2, g_ngx_skip_ti = -2;
extern "C" int sb_ngx_set_onlyti(int t) { g_ngx_only_ti = t; return t; }
extern "C" int sb_ngx_set_skipti(int t) { g_ngx_skip_ti = t; return t; }
// Multi-ti skip SET (/ngxskipset?ti=9,10,18) — skip several tev_indices at once, to test the
// combined removal of a multi-layer blend stack (e.g. the file-select haze layers) in one capture.
int g_ngx_skip_set[32]; int g_ngx_skip_set_n = 0;
extern "C" void sb_ngx_skipset_clear() { g_ngx_skip_set_n = 0; }
extern "C" void sb_ngx_skipset_add(int t) { if (g_ngx_skip_set_n < 32) g_ngx_skip_set[g_ngx_skip_set_n++] = t; }
static bool in_skipset(int ti) { for (int i = 0; i < g_ngx_skip_set_n; i++) if (g_ngx_skip_set[i] == ti) return true; return false; }
// Runtime EFB-copy epoch isolation (/ngxepoch): -1 = all (default); keep>=0 = only that epoch;
// drop>=0 = render everything EXCEPT that epoch. Diagnostic for the file-select multi-pass ghost.
int g_ngx_only_epoch = -1, g_ngx_drop_epoch = -1;
extern "C" int sb_ngx_set_onlyepoch(int e) { g_ngx_only_epoch = e; return e; }
extern "C" int sb_ngx_set_dropepoch(int e) { g_ngx_drop_epoch = e; return e; }
// DRAW-ORDER PREFIX (/ngxprefix?n=N): render only the first N batches actually drawn (in draw
// order, within the displayed epoch), then present. Sweeping N captures the composite building
// up pass-by-pass — the "in-between layer compare" so a divergence can be pinned to the exact
// pass that introduces it. -1 = render all (off).
int g_ngx_prefix_n = -1;
extern "C" int sb_ngx_set_prefix(int n) { g_ngx_prefix_n = n; return n; }
// Render-target-aware present (the FIX): drop batches from auxiliary offscreen EFB-copy epochs
// (reflections/shadows/file-slot thumbnails) below the display epoch — the file-select ghost-Mario
// fix. Default ON; SUNBRIGHT_NGX_RTFILTER=0 disables for A/B. /ngxrtfilter?on=N toggles live.
int g_ngx_rtfilter = -1;
extern "C" int sb_ngx_set_rtfilter(int v) { g_ngx_rtfilter = v; return v; }
static bool rtfilter_on() {
    if (g_ngx_rtfilter >= 0) return g_ngx_rtfilter != 0;        // runtime override
    const char* e = getenv("SUNBRIGHT_NGX_RTFILTER");
    return !(e && e[0] == '0' && e[1] == '\0');                  // default ON; only "0" disables
}
// Runtime blend override (/ngxnoblend): -1 = use per-material blend (default), 0 = force
// every material OPAQUE (blend off), 1 = leave blend on but force-disable depth-test is NOT
// here. Lets me A/B "is the dark water the annihilating dst=SRCCLR blend?" on a FROZEN frame
// without a relaunch. Bumps g_ngx_pipe_epoch so cached pipelines regenerate.
int g_ngx_noblend = -1;
int g_ngx_pipe_epoch = 0;
extern "C" int sb_ngx_set_noblend(int v) { if (v != g_ngx_noblend) { g_ngx_noblend = v; g_ngx_pipe_epoch++; } return v; }
#include <unordered_map>
#include <vector>

#ifdef HAVE_DOLPHIN_MEMMAP
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "VideoBackends/Vulkan/VKTexture.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/TextureConfig.h"
#include "render/shaders/mesh_vert_spv.h"
#include "render/shaders/quad_ortho_vert_spv.h"
#include "render/shaders/quad_modulate_frag_spv.h"
#include "render/shaders/quad_vert_spv.h"
#include "render/shaders/pollution_sphere_vert_spv.h"
#include "render/shaders/pollution_frag_spv.h"
#include "../intrinsics.h"

namespace {

uint32_t find_mem(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

// Matches MatPC in vk_mesh / the tev_shader push_constant block.
struct MatPC { int32_t kcolor[4][4]; int32_t tevreg[3][4]; };

struct PresentRenderer {
    bool init_tried = false, init_ok = false;
    VkDevice dev = VK_NULL_HANDLE; VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE; uint32_t qfam = 0;

    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout pll = VK_NULL_HANDLE;
    VkRenderPass rpass = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE; uint32_t dpool_cap = 0;

    // Render target (Dolphin AbstractTexture, resized as the XFB dims change) + depth.
    int tw = 0, th = 0;
    std::unique_ptr<AbstractTexture> target;
    VkImage depth_img = VK_NULL_HANDLE; VkDeviceMemory depth_mem = VK_NULL_HANDLE; VkImageView depth_view = VK_NULL_HANDLE;
    VkFramebuffer fbo = VK_NULL_HANDLE;

    struct PipeEntry { VkShaderModule mod; VkPipeline pipe; };
    std::unordered_map<uint64_t, PipeEntry> pipes;        // NgxTevState.key → shader+pipeline
    struct TexEntry { VkImage img; VkDeviceMemory mem; VkImageView view; };
    std::unordered_map<uint64_t, TexEntry> texcache;      // tex key → GPU image
    VkImageView white_view = VK_NULL_HANDLE;              // 1×1 white fallback (texmap unused)
    VkImage white_img = VK_NULL_HANDLE; VkDeviceMemory white_mem = VK_NULL_HANDLE;

    VkBuffer vbuf = VK_NULL_HANDLE; VkDeviceMemory vmem = VK_NULL_HANDLE; VkDeviceSize vcap = 0;

    // ── J2D / HUD overlay (drawn over the 3D scene in the same render pass) ──
    VkSampler j2d_sampler = VK_NULL_HANDLE;        // clamp-to-edge for HUD textures
    VkDescriptorSetLayout j2d_dsl = VK_NULL_HANDLE;  // 1 combined image sampler
    VkPipelineLayout j2d_pll = VK_NULL_HANDLE;
    VkShaderModule j2d_vs = VK_NULL_HANDLE, j2d_fs = VK_NULL_HANDLE;
    VkPipeline j2d_pipe = VK_NULL_HANDLE;
    VkDescriptorPool j2d_dpool = VK_NULL_HANDLE; uint32_t j2d_dpool_cap = 0;
    // One prepared HUD quad draw (descriptor + push-constant payload).
    struct J2dDraw { VkDescriptorSet dset; float rect[4]; float misc[4]; uint32_t corners[4]; uint32_t bw[4]; float uvrect[4]; };

    // ── Delfino plaza POLLUTION darkening (TModelWaterManager::drawShineShadowVolume port) ──
    // A sphere-volume pass into the present colour-alpha + depth target: clear EFB-alpha, build a
    // volume coverage mask in alpha (sphere front-ADD / back-SUBTRACT, z-test GREATER vs scene depth),
    // then blend a dark-blue tint masked by it. Reproduces the unported plaza pollution darkening
    // (debug_journal/2026-06-18_delfino_wash_ROOT_CAUSE_shineshadowvolume.md).
    VkPipelineLayout poll_pll = VK_NULL_HANDLE;        // push: { mat4 mvp; vec4 color }
    VkShaderModule poll_vs = VK_NULL_HANDLE, poll_fs = VK_NULL_HANDLE, poll_qvs = VK_NULL_HANDLE;
    VkPipeline poll_clearA = VK_NULL_HANDLE, poll_volAdd = VK_NULL_HANDLE,
               poll_volSub = VK_NULL_HANDLE, poll_final = VK_NULL_HANDLE;
    VkBuffer sphere_vbuf = VK_NULL_HANDLE; VkDeviceMemory sphere_vmem = VK_NULL_HANDLE;
    uint32_t sphere_vcount = 0;
    bool poll_init_ok = false;
    struct PollPC { float mvp[16]; float color[4]; };

    unsigned long g_frames = 0, g_pipe_builds = 0, g_tex_decodes = 0, g_j2d_quads = 0;
    int dbg_mode_built = -2;    // tev-debug mode the cached pipelines were built for; mismatch → rebuild
    int pipe_epoch_built = 0;   // /ngxnoblend (etc.) epoch the cached pipelines were built for

    void clear_pipes() {        // drop all cached shader+pipeline (e.g. when the debug mode flips)
        vkDeviceWaitIdle(dev);
        for (auto& kv : pipes) {
            if (kv.second.pipe) vkDestroyPipeline(dev, kv.second.pipe, nullptr);
            if (kv.second.mod)  vkDestroyShaderModule(dev, kv.second.mod, nullptr);
        }
        pipes.clear();
    }

    bool init();
    bool init_j2d();
    bool init_pollution();
    void draw_pollution(VkCommandBuffer cmd, int w, int h);
    bool ensure_target(int w, int h);
    bool ensure_vbuf(VkDeviceSize bytes);
    bool ensure_dpool(uint32_t nsets);
    bool ensure_j2d_dpool(uint32_t nsets);
    VkPipeline pipeline_for(const NgxTevState& st);
    VkImageView texture_for(const NgxTexBind& t, VkCommandBuffer up_cmd,
                            std::vector<VkBuffer>& stg_bufs, std::vector<VkDeviceMemory>& stg_mems);
    void prepare_j2d(VkCommandBuffer up_cmd, std::vector<VkBuffer>& stg_bufs,
                     std::vector<VkDeviceMemory>& stg_mems, std::vector<J2dDraw>& out);
    AbstractTexture* render(int w, int h);
};

PresentRenderer g_pr;

bool make_buffer(VkDevice dev, VkPhysicalDevice phys, VkBuffer& b, VkDeviceMemory& m,
                 VkDeviceSize sz, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sz; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &bci, nullptr, &b)) return false;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, props);
    if (vkAllocateMemory(dev, &ai, nullptr, &m)) return false;
    return vkBindBufferMemory(dev, b, m, 0) == VK_SUCCESS;
}

bool make_dev_image(VkDevice dev, VkPhysicalDevice phys, VkImage& img, VkDeviceMemory& mem, VkImageView& view,
                    VkFormat fmt, int w, int h, VkImageUsageFlags usage, VkImageAspectFlags asp, uint32_t mips = 1) {
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent = {(uint32_t)w, (uint32_t)h, 1}; ici.mipLevels = mips; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage; ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev, &ici, nullptr, &img)) return false;
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size; ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, nullptr, &mem)) return false;
    if (vkBindImageMemory(dev, img, mem, 0)) return false;
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = {asp, 0, mips, 0, 1};
    return vkCreateImageView(dev, &vci, nullptr, &view) == VK_SUCCESS;
}

bool PresentRenderer::init() {
    if (!Vulkan::g_vulkan_context || !g_gfx) return false;
    dev = Vulkan::g_vulkan_context->GetDevice();
    phys = Vulkan::g_vulkan_context->GetPhysicalDevice();
    queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
    qfam = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();
    if (dev == VK_NULL_HANDLE) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = VK_LOD_CLAMP_NONE;   // sample the full mip chain (trilinear); maxLod=0 (default)
                                      // would force mip0 only → minified tiled textures alias bright
    // A/B diag: SUNBRIGHT_NGX_TEXNEAREST=1 forces point sampling + no mips — tests whether the
    // cloud wash is LINEAR/mip averaging of the high-contrast noise toward its mean.
    if (getenv("SUNBRIGHT_NGX_TEXNEAREST") && atoi(getenv("SUNBRIGHT_NGX_TEXNEAREST"))) {
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; sci.maxLod = 0.25f;
    }
    if (vkCreateSampler(dev, &sci, nullptr, &sampler)) return false;

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 8; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1; li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl)) return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(MatPC)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &dsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &pll)) return false;

    // Render pass: color (Dolphin RGBA8 texture, → SHADER_READ for the present sample) + depth.
    VkAttachmentDescription at[2] = {};
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM; at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1].format = VK_FORMAT_D32_SFLOAT; at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dr{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sd{}; sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sd.colorAttachmentCount = 1; sd.pColorAttachments = &ar; sd.pDepthStencilAttachment = &dr;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 2; rpci.pAttachments = at; rpci.subpassCount = 1; rpci.pSubpasses = &sd;
    if (vkCreateRenderPass(dev, &rpci, nullptr, &rpass)) return false;

    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = sizeof kMeshVertSpv; si.pCode = kMeshVertSpv;
    if (vkCreateShaderModule(dev, &si, nullptr, &vs)) return false;

    VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpci.queueFamilyIndex = qfam; cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(dev, &cpci, nullptr, &cpool)) return false;
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &cbai, &cmd)) return false;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(dev, &fci, nullptr, &fence)) return false;

    // 1×1 white fallback texture for unused/unsupported texmaps.
    if (!make_dev_image(dev, phys, white_img, white_mem, white_view, VK_FORMAT_R8G8B8A8_UNORM, 1, 1,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    {   // upload the white texel + leave it SHADER_READ
        VkBuffer sbuf; VkDeviceMemory smem;
        if (!make_buffer(dev, phys, sbuf, smem, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return false;
        void* p = nullptr; vkMapMemory(dev, smem, 0, 4, 0, &p); uint32_t w = 0xFFFFFFFFu; memcpy(p, &w, 4); vkUnmapMemory(dev, smem);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        auto bar = [&](VkImageLayout f, VkImageLayout t, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags ds) {
            VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; mb.oldLayout = f; mb.newLayout = t;
            mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; mb.image = white_img;
            mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}; mb.srcAccessMask = sa; mb.dstAccessMask = da;
            vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &mb);
        };
        bar(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy c{}; c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cmd, sbuf, white_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        bar(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        vkEndCommandBuffer(cmd);
        VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO}; su.commandBufferCount = 1; su.pCommandBuffers = &cmd;
        vkResetFences(dev, 1, &fence); vkQueueSubmit(queue, 1, &su, fence); vkWaitForFences(dev, 1, &fence, VK_TRUE, 5'000'000'000ull);
        vkDestroyBuffer(dev, sbuf, nullptr); vkFreeMemory(dev, smem, nullptr);
    }
    if (!init_j2d()) return false;
    poll_init_ok = init_pollution();   // pollution darkening (best-effort; off if it fails)
    return true;
}

// J2D/HUD overlay pipeline: an ortho textured-quad (quad_ortho vert + quad_modulate
// frag) drawn over the 3D scene in the SAME render pass (depth test off). Built ONCE;
// the pipeline is fixed (the render pass / target format never change shape). The
// viewport/scissor are dynamic so it survives target-size changes without a rebuild.
bool PresentRenderer::init_j2d() {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(dev, &sci, nullptr, &j2d_sampler)) return false;

    VkDescriptorSetLayoutBinding b{};
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1; li.pBindings = &b;
    if (vkCreateDescriptorSetLayout(dev, &li, nullptr, &j2d_dsl)) return false;

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 80};  // rect | misc | corners | bw | uvrect
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1; plci.pSetLayouts = &j2d_dsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &j2d_pll)) return false;

    VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    si.codeSize = sizeof kQuadOrthoVertSpv; si.pCode = kQuadOrthoVertSpv;
    if (vkCreateShaderModule(dev, &si, nullptr, &j2d_vs)) return false;
    si.codeSize = sizeof kQuadModulateFragSpv; si.pCode = kQuadModulateFragSpv;
    if (vkCreateShaderModule(dev, &si, nullptr, &j2d_fs)) return false;

    VkPipelineShaderStageCreateInfo ss[2] = {};
    ss[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT; ss[0].module = j2d_vs; ss[0].pName = "main";
    ss[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module = j2d_fs; ss[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};  // none (gl_VertexIndex)
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;   // dynamic
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // The render pass has a depth attachment (shared with the 3D pass) — the HUD must
    // provide a depth-stencil state but draws on top with test/write OFF.
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = VK_FALSE; dss.depthWriteEnable = VK_FALSE; dss.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF; cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = ss; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
    gp.layout = j2d_pll; gp.renderPass = rpass; gp.subpass = 0;
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &j2d_pipe) == VK_SUCCESS;
}

extern "C" int sb_ngx_get_pollution(float center[3], float* radius, float color[3], float* clearAlpha);
extern "C" int sb_ngx_get_proj(float out[16]);

// Build the four pollution pipelines + the unit-sphere vbuf. Faithful to drawShineShadowVolume:
// clear EFB alpha, sphere volume (front-ADD/back-SUBTRACT in alpha, z-test GREATER vs scene depth),
// dark-tint masked blend. One sphere with alpha 1.0 saturates inside (the 5 game spheres only soften
// the edge; net inside = bright, outside = clearAlpha → darkened). See the journal for the derivation.
bool PresentRenderer::init_pollution() {
    // 1) unit-sphere mesh (positions only), CCW outward triangles (UV-sphere).
    const int STK = 12, SLI = 18; const float PI = 3.14159265358979f;
    std::vector<float> pos;
    auto vtx = [&](int st, int sl) {
        float th = (float)st / STK * PI, ph = (float)sl / SLI * 2.f * PI;
        pos.push_back(std::sin(th) * std::cos(ph));
        pos.push_back(std::cos(th));
        pos.push_back(std::sin(th) * std::sin(ph));
    };
    for (int st = 0; st < STK; st++) for (int sl = 0; sl < SLI; sl++) {
        vtx(st, sl); vtx(st + 1, sl); vtx(st, sl + 1);          // CCW outward
        vtx(st, sl + 1); vtx(st + 1, sl); vtx(st + 1, sl + 1);
    }
    sphere_vcount = (uint32_t)(pos.size() / 3);
    VkDeviceSize bytes = (VkDeviceSize)pos.size() * sizeof(float);
    if (!make_buffer(dev, phys, sphere_vbuf, sphere_vmem, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return false;
    { void* p = nullptr; vkMapMemory(dev, sphere_vmem, 0, bytes, 0, &p);
      memcpy(p, pos.data(), (size_t)bytes); vkUnmapMemory(dev, sphere_vmem); }

    // 2) pipeline layout: one push range (mat4 mvp + vec4 color), no descriptor sets.
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PollPC)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(dev, &plci, nullptr, &poll_pll)) return false;

    auto mkmod = [&](const uint32_t* code, size_t len) -> VkShaderModule {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = len; si.pCode = code; VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &si, nullptr, &m); return m;
    };
    poll_qvs = mkmod(kQuadVertSpv, sizeof kQuadVertSpv);
    poll_vs  = mkmod(kPollutionSphereVertSpv, sizeof kPollutionSphereVertSpv);
    poll_fs  = mkmod(kPollutionFragSpv, sizeof kPollutionFragSpv);
    if (!poll_qvs || !poll_vs || !poll_fs) return false;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkDynamicState dyns[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyns;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    auto build = [&](VkShaderModule vsm, bool hasVtx, VkCullModeFlags cull, VkBool32 depthTest,
                     VkBool32 depthWrite, VkCompareOp dop, VkColorComponentFlags wmask, VkBool32 blend,
                     VkBlendFactor sc, VkBlendFactor dc, VkBlendOp cop,
                     VkBlendFactor sa, VkBlendFactor da, VkBlendOp aop, VkPipeline& out) -> bool {
        VkPipelineShaderStageCreateInfo ss[2]{};
        ss[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   ss[0].module = vsm;     ss[0].pName = "main";
        ss[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module = poll_fs; ss[1].pName = "main";
        VkVertexInputBindingDescription vib{0, 3 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription via{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        if (hasVtx) { vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vib;
                      vi.vertexAttributeDescriptionCount = 1; vi.pVertexAttributeDescriptions = &via; }
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = cull;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = depthTest; dss.depthWriteEnable = depthWrite; dss.depthCompareOp = dop;
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = wmask; cba.blendEnable = blend;
        cba.srcColorBlendFactor = sc; cba.dstColorBlendFactor = dc; cba.colorBlendOp = cop;
        cba.srcAlphaBlendFactor = sa; cba.dstAlphaBlendFactor = da; cba.alphaBlendOp = aop;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = ss; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.pDynamicState = &ds;
        gp.layout = poll_pll; gp.renderPass = rpass; gp.subpass = 0;
        return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &out) == VK_SUCCESS;
    };
    const VkColorComponentFlags A = VK_COLOR_COMPONENT_A_BIT;
    const VkColorComponentFlags RGB = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    using F = VkBlendFactor; const VkBlendOp ADD = VK_BLEND_OP_ADD;
    // clear alpha (fullscreen): write A, no blend (replace), no depth, no cull.
    if (!build(poll_qvs, false, VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS, A, VK_FALSE,
               VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, ADD, poll_clearA)) return false;
    // volume ADD (back faces): write A + DEPTH (game uses ZMode GREATER,WRITE), z-test GREATER, ONE/ONE.
    if (!build(poll_vs, true, VK_CULL_MODE_BACK_BIT, VK_TRUE, VK_TRUE, VK_COMPARE_OP_GREATER, A, VK_TRUE,
               VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, ADD, poll_volAdd)) return false;
    // volume SUBTRACT (front faces): cull FRONT, alpha dst - src (REVERSE_SUBTRACT), z-test GREATER + write.
    if (!build(poll_vs, true, VK_CULL_MODE_FRONT_BIT, VK_TRUE, VK_TRUE, VK_COMPARE_OP_GREATER, A, VK_TRUE,
               VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT, poll_volSub)) return false;
    // final masked dark blend (fullscreen): write RGB, INVDSTALPHA/DSTALPHA.
    if (!build(poll_qvs, false, VK_CULL_MODE_NONE, VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS, RGB, VK_TRUE,
               VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, ADD, poll_final)) return false;
    return true;
}

// Draw the pollution darkening inside the present render pass, after the 3D scene (depth valid),
// before the HUD. Reads the live captured params (recede as the plaza is cleaned). Env A/B:
// SUNBRIGHT_NGX_NOPOLLUTION=1 disables it.
void PresentRenderer::draw_pollution(VkCommandBuffer cmd, int w, int h) {
    if (!poll_init_ok) return;
    static const bool off = getenv("SUNBRIGHT_NGX_NOPOLLUTION") != nullptr;
    if (off) return;
    float center[3], radius, color[3], clearA;
    if (!sb_ngx_get_pollution(center, &radius, color, &clearA)) return;   // not active this frame
    float P[16]; if (!sb_ngx_get_proj(P)) return;
    if (getenv("SUNBRIGHT_DBG_POLL")) {
        static int n = 0;
        if (n++ % 60 == 0)
            fprintf(stderr, "[poll] eyeCenter=(%.1f,%.1f,%.1f) radius=%.1f clearA=%.3f color=(%.3f,%.3f,%.3f)\n",
                    center[0], center[1], center[2], radius, clearA, color[0], color[1], color[2]);
    }
    // model (unit->eye), row-major: translate(center) * scale(radius).
    const float M[16] = { radius,0,0,center[0],  0,radius,0,center[1],  0,0,radius,center[2],  0,0,0,1 };
    float MVP[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        float s = 0; for (int k = 0; k < 4; k++) s += P[r*4+k] * M[k*4+c]; MVP[r*4+c] = s;
    }
    PollPC pc{};
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) pc.mvp[c*4+r] = MVP[r*4+c];  // → GLSL column-major

    VkViewport vpv{0, 0, (float)w, (float)h, 0, 1}; vkCmdSetViewport(cmd, 0, 1, &vpv);
    VkRect2D scr{{0, 0}, {(uint32_t)w, (uint32_t)h}}; vkCmdSetScissor(cmd, 0, 1, &scr);
    const VkShaderStageFlags SS = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // pass 1: clear EFB alpha to clearAlpha (fullscreen).
    pc.color[0] = pc.color[1] = pc.color[2] = 0.f; pc.color[3] = clearA;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, poll_clearA);
    vkCmdPushConstants(cmd, poll_pll, SS, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // pass 2: sphere volume → alpha saturates inside the clean dome.
    static const bool novol = getenv("SUNBRIGHT_NGX_POLL_NOVOL") != nullptr;   // DBG: skip the spheres
    if (!novol) {
    pc.color[3] = 1.0f;
    VkDeviceSize voff = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &sphere_vbuf, &voff);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, poll_volAdd);
    vkCmdPushConstants(cmd, poll_pll, SS, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, sphere_vcount, 1, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, poll_volSub);
    vkCmdDraw(cmd, sphere_vcount, 1, 0, 0);
    }

    // pass 3: blend the dark tint where alpha is low (outside the dome).
    pc.color[0] = color[0]; pc.color[1] = color[1]; pc.color[2] = color[2]; pc.color[3] = 1.0f;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, poll_final);
    vkCmdPushConstants(cmd, poll_pll, SS, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

bool PresentRenderer::ensure_target(int w, int h) {
    if (target && tw == w && th == h) return true;
    vkDeviceWaitIdle(dev);
    if (fbo) { vkDestroyFramebuffer(dev, fbo, nullptr); fbo = VK_NULL_HANDLE; }
    if (depth_view) { vkDestroyImageView(dev, depth_view, nullptr); depth_view = VK_NULL_HANDLE; }
    if (depth_img) { vkDestroyImage(dev, depth_img, nullptr); depth_img = VK_NULL_HANDLE; }
    if (depth_mem) { vkFreeMemory(dev, depth_mem, nullptr); depth_mem = VK_NULL_HANDLE; }
    target.reset();
    target = g_gfx->CreateTexture(
        TextureConfig((u32)w, (u32)h, 1, 1, 1, AbstractTextureFormat::RGBA8,
                      AbstractTextureFlag_RenderTarget, AbstractTextureType::Texture_2DArray),
        "ngx_present");
    if (!target) return false;
    if (!make_dev_image(dev, phys, depth_img, depth_mem, depth_view, VK_FORMAT_D32_SFLOAT, w, h,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    VkImageView views[2] = {static_cast<Vulkan::VKTexture*>(target.get())->GetView(), depth_view};
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = rpass; fci.attachmentCount = 2; fci.pAttachments = views;
    fci.width = (u32)w; fci.height = (u32)h; fci.layers = 1;
    if (vkCreateFramebuffer(dev, &fci, nullptr, &fbo)) return false;
    tw = w; th = h;
    return true;
}

bool PresentRenderer::ensure_vbuf(VkDeviceSize bytes) {
    if (vbuf && vcap >= bytes) return true;
    vkDeviceWaitIdle(dev);
    if (vbuf) { vkDestroyBuffer(dev, vbuf, nullptr); vbuf = VK_NULL_HANDLE; }
    if (vmem) { vkFreeMemory(dev, vmem, nullptr); vmem = VK_NULL_HANDLE; }
    VkDeviceSize cap = bytes + bytes / 2 + 4096;
    if (!make_buffer(dev, phys, vbuf, vmem, cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return false;
    vcap = cap; return true;
}

bool PresentRenderer::ensure_dpool(uint32_t nsets) {
    if (dpool && dpool_cap >= nsets) { vkResetDescriptorPool(dev, dpool, 0); return true; }
    vkDeviceWaitIdle(dev);
    if (dpool) { vkDestroyDescriptorPool(dev, dpool, nullptr); dpool = VK_NULL_HANDLE; }
    uint32_t cap = nsets + nsets / 2 + 16;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, cap * 8};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = cap; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &dpool)) return false;
    dpool_cap = cap; return true;
}

bool PresentRenderer::ensure_j2d_dpool(uint32_t nsets) {
    if (j2d_dpool && j2d_dpool_cap >= nsets) { vkResetDescriptorPool(dev, j2d_dpool, 0); return true; }
    vkDeviceWaitIdle(dev);
    if (j2d_dpool) { vkDestroyDescriptorPool(dev, j2d_dpool, nullptr); j2d_dpool = VK_NULL_HANDLE; }
    uint32_t cap = nsets + nsets / 2 + 8;
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, cap};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = cap; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(dev, &pci, nullptr, &j2d_dpool)) return false;
    j2d_dpool_cap = cap; return true;
}

// Build (or fetch cached) the pipeline for a material TEV+PE state. Compiles the
// generated TEV fragment shader ONCE (cached by NgxTevState.key); sets blend + depth
// from the PE block. Returns VK_NULL_HANDLE on compile/create failure.
VkPipeline PresentRenderer::pipeline_for(const NgxTevState& st) {
    auto it = pipes.find(st.key);
    if (it != pipes.end()) return it->second.pipe;

    auto spv = sb_compile_fragment_glsl(sb_tev_gen_fragment(st));
    if (spv.empty()) { pipes[st.key] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; return VK_NULL_HANDLE; }
    VkShaderModuleCreateInfo fi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fi.codeSize = spv.size() * sizeof(uint32_t); fi.pCode = spv.data();
    VkShaderModule fs = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &fi, nullptr, &fs)) { pipes[st.key] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; return VK_NULL_HANDLE; }

    VkPipelineShaderStageCreateInfo ss[2] = {};
    ss[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT; ss[0].module = vs; ss[0].pName = "main";
    ss[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module = fs; ss[1].pName = "main";

    VkVertexInputBindingDescription vib{0, sizeof(NgxRenderVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription via[10] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)},
    };
    for (uint32_t m = 0; m < 8; m++) via[2 + m] = {2 + m, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)((8 + m * 2) * sizeof(float))};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vib;
    vi.vertexAttributeDescriptionCount = 10; vi.pVertexAttributeDescriptions = via;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, (float)tw, (float)th, 0, 1};
    VkRect2D scr{{0, 0}, {(uint32_t)tw, (uint32_t)th}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scr;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // Backface culling per the material's GXCullMode (color block mCullMode). GXCullMode→VK is
    // {NONE,FRONT,BACK,FRONT_AND_BACK}; Dolphin's GX render uses frontFace=CLOCKWISE — match it, else
    // (no cull) translucent surfaces draw both faces (overdraw → too bright) and back faces show through.
    static const VkCullModeFlags kCull[4] = {
        VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT, VK_CULL_MODE_FRONT_AND_BACK };
    static const int force_cull = getenv("SUNBRIGHT_NGX_FORCECULL") ? atoi(getenv("SUNBRIGHT_NGX_FORCECULL")) : -1;
    rs.cullMode = force_cull >= 0 ? kCull[force_cull & 3] : kCull[st.pe.cull & 3];
    static const bool ccw = getenv("SUNBRIGHT_NGX_CCW") && atoi(getenv("SUNBRIGHT_NGX_CCW")) != 0;
    rs.frontFace = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // Depth + blend from the PE block (same mapping as vk_mesh).
    auto vk_src = [](uint8_t f) -> VkBlendFactor { static const VkBlendFactor s[8] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA }; return s[f & 7]; };
    auto vk_dst = [](uint8_t f) -> VkBlendFactor { static const VkBlendFactor d[8] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA }; return d[f & 7]; };
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = st.pe.z_test ? VK_TRUE : VK_FALSE;
    dss.depthWriteEnable = st.pe.z_write ? VK_TRUE : VK_FALSE;
    dss.depthCompareOp = (VkCompareOp)(st.pe.z_func & 7);
    VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF; cba.blendEnable = VK_FALSE;
    if (st.pe.blend_mode == 1) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = vk_src(st.pe.src_factor);
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = vk_dst(st.pe.dst_factor);
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (st.pe.blend_mode == 3) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
    }
    // A/B diag: SUNBRIGHT_NGX_NOBLEND=1 forces every material opaque (blend off) — isolates
    // whether the wash is blend overdraw/accumulation vs the combiner/raster source.
    static const bool s_noblend_env = getenv("SUNBRIGHT_NGX_NOBLEND") && atoi(getenv("SUNBRIGHT_NGX_NOBLEND")) != 0;
    if (::g_ngx_noblend == 0 || (::g_ngx_noblend < 0 && s_noblend_env)) cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2; gp.pStages = ss; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.layout = pll; gp.renderPass = rpass; gp.subpass = 0;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe)) { vkDestroyShaderModule(dev, fs, nullptr); pipes[st.key] = {VK_NULL_HANDLE, VK_NULL_HANDLE}; return VK_NULL_HANDLE; }
    pipes[st.key] = {fs, pipe}; g_pipe_builds++;
    return pipe;
}

// Decode + cache a texture; on a cache miss, decode into a transient staging buffer
// (recorded into up_cmd, freed after the frame's fence). Returns the white view if
// the binding is empty/unsupported.
VkImageView PresentRenderer::texture_for(const NgxTexBind& t, VkCommandBuffer up_cmd,
                                         std::vector<VkBuffer>& stg_bufs, std::vector<VkDeviceMemory>& stg_mems) {
    if (t.addr == 0 || t.w == 0 || t.h == 0) return white_view;
    // GC textures tile at the FORMAT's block dims and the decode uses width as both the
    // tile-iteration bound and the dst row stride — so it must run on the block-padded
    // size (a fixed 8 over-strides the 4-wide formats → diagonal shear; the title-logo
    // RGB5A3 w=460 bug). Pad here so every caller (3D batches + J2D quads) is correct.
    const int pw = sb_tex_pad_w(t.w, t.fmt), ph = sb_tex_pad_h(t.h, t.fmt);
    const uint64_t key = ((uint64_t)t.addr << 16) ^ ((uint64_t)t.fmt << 56) ^
                         ((uint64_t)t.w << 40) ^ ((uint64_t)t.h << 28) ^ ((uint64_t)t.tlut_addr << 4);
    auto it = texcache.find(key);
    if (it != texcache.end()) return it->second.view;

    const uint8_t* host = sb_ram_fast(t.addr);
    if (!host) return white_view;
    const int srcbytes = sb_tex_size_bytes(pw, ph, t.fmt);
    if (srcbytes <= 0 || (t.addr & 0x01FFFFFFu) + (uint32_t)srcbytes > 0x1800000u) return white_view;
    const uint8_t* tlut = nullptr;
    if (t.fmt == SB_TF_C4 || t.fmt == SB_TF_C8 || t.fmt == SB_TF_C14X2) {
        const uint32_t entries = t.fmt == SB_TF_C4 ? 16u : t.fmt == SB_TF_C8 ? 256u : 16384u;
        if (t.tlut_addr && (t.tlut_addr & 0x01FFFFFFu) + entries * 2u <= 0x1800000u) tlut = sb_ram_fast(t.tlut_addr);
        if (!tlut) return white_view;
    }
    const size_t rgba = (size_t)pw * ph * 4;
    VkBuffer sbuf; VkDeviceMemory smem;
    if (!make_buffer(dev, phys, sbuf, smem, rgba, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return white_view;
    void* p = nullptr; vkMapMemory(dev, smem, 0, rgba, 0, &p);
    sb_tex_decode((uint32_t*)p, host, pw, ph, t.fmt, tlut, t.tlut_fmt);
    vkUnmapMemory(dev, smem);
    stg_bufs.push_back(sbuf); stg_mems.push_back(smem);

    // Mip chain: GameCube samples a texture's mips; a heavily-tiled/minified surface (e.g. the
    // plaza floor) averages to a darker value, while a single mip0 + bilinear ALIASES toward the
    // bright tile faces → washed-out. Generate a box-filtered mip chain (vkCmdBlitImage, linear)
    // and sample it trilinearly. (GC TIMGs can store mips; box-generated mips approximate them and
    // fix the aliasing — the dominant cause of the ngx 3D wash.)
    // The image is the LOGICAL size (t.w×t.h), NOT the block-padded decode size (pw×ph):
    // GC textures tile at the format's block, so a non-block-multiple texture (e.g. a 20×20
    // IA4 window-border corner → padded 24×20) has GARBAGE in the padding columns/rows. If
    // the image were pw×ph, u/v ∈ [0,1] would sample that padding — most visibly the window
    // 9-slice border edge quads (degenerate U=1.0) land squarely in the white padding → a
    // white frame instead of blue. Uploading only the w×h region (with bufferRowLength=pw as
    // the decode stride) makes [0,1] map to the real texels. (UV analog of the title-logo
    // block-padding fix; without it every non-block-multiple texture leaks padding at u/v≈1.)
    const int iw = (int)t.w, ih = (int)t.h;
    uint32_t mips = 1; { int m = iw > ih ? iw : ih; while (m > 1) { m >>= 1; mips++; } }
    TexEntry te{};
    if (!make_dev_image(dev, phys, te.img, te.mem, te.view, VK_FORMAT_R8G8B8A8_UNORM, iw, ih,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_ASPECT_COLOR_BIT, mips)) return white_view;
    auto bar = [&](uint32_t lvl, VkImageLayout f, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ssf, VkPipelineStageFlags dsf) {
        VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; mb.oldLayout = f; mb.newLayout = to;
        mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; mb.image = te.img;
        mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, 1, 0, 1}; mb.srcAccessMask = sa; mb.dstAccessMask = da;
        vkCmdPipelineBarrier(up_cmd, ssf, dsf, 0, 0, nullptr, 0, nullptr, 1, &mb);
    };
    // Upload mip0.
    bar(0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Copy only the logical w×h region; bufferRowLength=pw keeps the decode's padded stride.
    VkBufferImageCopy c{}; c.bufferRowLength = (uint32_t)pw; c.bufferImageHeight = (uint32_t)ph;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c.imageExtent = {(uint32_t)iw, (uint32_t)ih, 1};
    vkCmdCopyBufferToImage(up_cmd, sbuf, te.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    // Generate mips by successive linear blits (i-1 → i), box-filtering down the chain.
    int mw = iw, mh = ih;
    for (uint32_t i = 1; i < mips; i++) {
        bar(i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        bar(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        int nw = mw > 1 ? mw >> 1 : 1, nh = mh > 1 ? mh >> 1 : 1;
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.srcOffsets[1] = {mw, mh, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(up_cmd, te.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, te.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        // The just-blitted source level (i-1) is now done → SHADER_READ.
        bar(i - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        mw = nw; mh = nh;
    }
    // Final (smallest) level is in TRANSFER_DST → SHADER_READ.
    bar(mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    texcache[key] = te; g_tex_decodes++;
    return te.view;
}

// Collect the live J2D HUD draw list and prepare a per-quad draw (texture uploaded +
// cached, descriptor set written). MUST run BEFORE vkCmdBeginRenderPass (texture
// uploads record copies/barriers, illegal inside a render pass). The actual draws are
// issued from render() inside the pass. Paletted quads are skipped (no TLUT resolved
// in the walker yet) so an unresolved palette can't paint a white box.
void PresentRenderer::prepare_j2d(VkCommandBuffer up_cmd, std::vector<VkBuffer>& stg_bufs,
                                  std::vector<VkDeviceMemory>& stg_mems, std::vector<J2dDraw>& out) {
    static J2dQuad quads[1024];   // pictures + textbox glyphs
    int sw = 0, sh = 0;
    int nq = sb_j2d_snapshot(quads, 1024, &sw, &sh);   // consistent draw-time snapshot
    if (nq <= 0) return;
    if (sw <= 0 || sw > 4096) sw = 640;
    if (sh <= 0 || sh > 4096) sh = 480;
    if (!ensure_j2d_dpool((uint32_t)nq)) return;

    for (int i = 0; i < nq; i++) {
        const J2dQuad& q = quads[i];
        if (sb_tex_is_paletted(q.fmt)) continue;
        if (q.x1 <= q.x0 || q.y1 <= q.y0) continue;
        // Pass the logical texture dims; texture_for() pads to the format's block dims
        // (a fixed-8 pad over-strides the 4-wide formats → diagonal shear, the logo bug).
        NgxTexBind tb{};
        tb.addr = q.data; tb.w = (uint16_t)q.w; tb.h = (uint16_t)q.h;
        tb.fmt = (uint8_t)q.fmt; tb.tlut_addr = 0; tb.tlut_fmt = 0;
        VkImageView view = texture_for(tb, up_cmd, stg_bufs, stg_mems);
        if (view == VK_NULL_HANDLE) continue;

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = j2d_dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &j2d_dsl;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(dev, &dai, &set)) return;
        VkDescriptorImageInfo dii{j2d_sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = set; wr.dstBinding = 0; wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);

        J2dDraw d{};
        d.dset = set;
        d.rect[0] = (float)q.x0; d.rect[1] = (float)q.y0; d.rect[2] = (float)q.x1; d.rect[3] = (float)q.y1;
        d.misc[0] = (float)sw; d.misc[1] = (float)sh; d.misc[2] = q.alpha / 255.0f; d.misc[3] = 0;
        for (int c = 0; c < 4; c++) d.corners[c] = q.corner[c];
        d.bw[0] = q.white; d.bw[1] = q.black; d.bw[2] = 0; d.bw[3] = 0;
        d.uvrect[0] = q.u0; d.uvrect[1] = q.v0; d.uvrect[2] = q.u1; d.uvrect[3] = q.v1;
        out.push_back(d);
    }
    g_j2d_quads = out.size();
}

AbstractTexture* PresentRenderer::render(int w, int h) {
    int nv = 0, nb = 0, ntev = 0;
    const NgxRenderVertex* sv = ngx_snap_verts(&nv);
    const NgxRenderBatch* sb = ngx_snap_batches(&nb);
    const NgxTevState* stv = ngx_snap_tevstates(&ntev);
    if (!sv || nv < 3 || !sb || nb < 1) return nullptr;
    std::vector<NgxRenderVertex> verts(sv, sv + nv);
    std::vector<NgxRenderBatch> batches(sb, sb + nb);
    std::vector<NgxTevState> tevstates(stv, stv + (ntev > 0 ? ntev : 0));

    // ── interp60 (PC-native frame interpolation) ─────────────────────────────────────────────
    // On an in-between field (the driver set interp mode 1), present a clip-space lerp of the
    // previous frame (N-1) toward this one (N) instead of N itself — doubling distinct frames to
    // 60fps from the 30Hz game, entirely from ngx's own object-model snapshots (no GX replay). The
    // lerp is valid only when N-1 and N share topology (same vertex count + per-batch structure), so
    // vertex i in N corresponds to vertex i in N-1 (a model moving → same shapes, same vert order,
    // different transforms). If a shape spawned/despawned or the draw order changed, correspondence
    // breaks → fall back to presenting N unblended (interpolation can never corrupt the frame).
    if (ngx_interp60_enabled() && ngx_interp_mode() == 1) {
        int npv = 0, npb = 0;
        const NgxRenderVertex* pv = ngx_snap_verts_prev(&npv);
        const NgxRenderBatch*  pb = ngx_snap_batches_prev(&npb);
        bool match = pv && pb && npv == nv && npb == nb;
        for (int b = 0; match && b < nb; b++)
            match = pb[b].vstart == batches[b].vstart && pb[b].vcount == batches[b].vcount &&
                    pb[b].tev_index == batches[b].tev_index && pb[b].epoch == batches[b].epoch;
        if (match) {
            const float a = sb_ngx_interp_alpha();
            for (int i = 0; i < nv; i++) {
                const NgxRenderVertex& A = pv[i]; NgxRenderVertex& B = verts[i];
                for (int k = 0; k < 4; k++) B.clip[k] = A.clip[k] + (B.clip[k] - A.clip[k]) * a;
                for (int k = 0; k < 4; k++) B.rgba[k] = A.rgba[k] + (B.rgba[k] - A.rgba[k]) * a;
                for (int t = 0; t < 8; t++) for (int k = 0; k < 2; k++)
                    B.uv[t][k] = A.uv[t][k] + (B.uv[t][k] - A.uv[t][k]) * a;
            }
        }
    }
    // Render-target-aware filter: present only the display epoch (main scene) onward.
    const int display_epoch = rtfilter_on() ? ngx_snap_display_epoch() : 0;

    if (w < 1 || h < 1) return nullptr;
    // Render-debug mode changed (via /ngxdbg)? Drop cached pipelines so shaders regenerate.
    if (int m = sb_ngx_tevdbg(); m != dbg_mode_built) { if (init_ok && dev) clear_pipes(); dbg_mode_built = m; }
    if (::g_ngx_pipe_epoch != pipe_epoch_built) { if (init_ok && dev) clear_pipes(); pipe_epoch_built = ::g_ngx_pipe_epoch; }
    if (!ensure_target(w, h)) return nullptr;
    if (!ensure_vbuf((VkDeviceSize)nv * sizeof(NgxRenderVertex))) return nullptr;
    if (!ensure_dpool((uint32_t)batches.size())) return nullptr;

    // Upload vertices.
    { void* p = nullptr; vkMapMemory(dev, vmem, 0, (VkDeviceSize)nv * sizeof(NgxRenderVertex), 0, &p);
      memcpy(p, verts.data(), (size_t)nv * sizeof(NgxRenderVertex)); vkUnmapMemory(dev, vmem); }

    // Pipelines (compiled once + cached) + per-batch slot 0 (modulate fallback).
    static uint64_t s_modkey = 0; static bool s_modinit = false;
    NgxTevState mod{};
    if (!s_modinit) {
        mod.num_stages = 1;
        mod.stage[0].color_env = (15u<<12)|(8u<<8)|(10u<<4)|15u | (1u<<19);
        mod.stage[0].alpha_env = (7u<<13)|(4u<<10)|(5u<<7)|(7u<<4) | (1u<<19);
        mod.stage[0].texmap = 0; mod.stage[0].color_chan = 4;
        mod.pe.z_test = 1; mod.pe.z_func = 3; mod.pe.z_write = 1;
        uint64_t hkey = 1469598103934665603ull; const uint8_t* mp = (const uint8_t*)&mod;
        for (size_t i = 0; i < offsetof(NgxTevState, key); i++) { hkey ^= mp[i]; hkey *= 1099511628211ull; }
        mod.key = s_modkey = hkey; s_modinit = true;
    } else { mod.key = s_modkey; }
    pipeline_for(mod);  // ensure fallback exists
    for (auto& st : tevstates) pipeline_for(st);

    // Begin command buffer: upload new textures, render the scene.
    std::vector<VkBuffer> stg_bufs; std::vector<VkDeviceMemory> stg_mems;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkResetCommandBuffer(cmd, 0); vkBeginCommandBuffer(cmd, &bi);

    // Per-batch descriptor sets (8 texmaps each) — resolved/uploaded as needed.
    std::vector<VkDescriptorSet> bset(batches.size(), VK_NULL_HANDLE);
    std::vector<VkImageView> bviews(batches.size() * 8, white_view);
    for (size_t b = 0; b < batches.size(); b++)
        for (int m = 0; m < 8; m++) bviews[b * 8 + m] = texture_for(batches[b].tex[m], cmd, stg_bufs, stg_mems);
    for (size_t b = 0; b < batches.size(); b++) {
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
        if (vkAllocateDescriptorSets(dev, &dai, &bset[b])) return nullptr;
        VkDescriptorImageInfo dii[8];
        for (int m = 0; m < 8; m++) dii[m] = {sampler, bviews[b * 8 + m], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = bset[b]; wr.dstBinding = 0; wr.descriptorCount = 8;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = dii;
        vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);
    }

    // Prepare the HUD/J2D overlay (texture uploads recorded into `cmd`) BEFORE the
    // render pass; the quads are drawn over the 3D scene inside it.
    std::vector<J2dDraw> j2d;
    prepare_j2d(cmd, stg_bufs, stg_mems, j2d);

    float cc[4]; sb_ngx_get_clear(cc);   // the game's EFB copy-clear (screen-blend sky depends on it)
    VkClearValue clear[2]{}; clear[0].color = {{cc[0], cc[1], cc[2], cc[3]}}; clear[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rpass; rbi.framebuffer = fbo; rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 2; rbi.pClearValues = clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkDeviceSize voff = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &voff);
    VkPipeline last = VK_NULL_HANDLE;
    int drawn = 0;   // count of batches actually emitted (for the /ngxprefix draw-order limit)
    // DBG: count drawn batches + total verts for a target tev_index (overdraw check for the cloud wash).
    static const int s_cloudcount = getenv("SUNBRIGHT_NGX_CLOUDCOUNT") ? atoi(getenv("SUNBRIGHT_NGX_CLOUDCOUNT")) : -1;
    int cc_batches = 0; long cc_verts = 0; static unsigned cc_frame = 0;
    for (size_t b = 0; b < batches.size(); b++) {
        if (g_ngx_prefix_n >= 0 && drawn >= g_ngx_prefix_n) break;   // draw-order prefix: stop after N
        const int ti = batches[b].tev_index;
        const NgxTevState& st = (ti >= 0 && ti < (int)tevstates.size()) ? tevstates[ti] : mod;
        // DBG: skip alpha-tested (cloud/foliage) batches to reveal the background behind them.
        static const int skip_alpha = getenv("SUNBRIGHT_NGX_SKIPALPHA") ? atoi(getenv("SUNBRIGHT_NGX_SKIPALPHA")) : 0;
        if (skip_alpha && st.pe.alpha_test) continue;
        // DBG: render ONLY a specific tev_index (-1=all) to isolate one material's on-screen output.
        static const int env_only = getenv("SUNBRIGHT_NGX_ONLYTI") ? atoi(getenv("SUNBRIGHT_NGX_ONLYTI")) : -1;
        const int only_ti = g_ngx_only_ti != -2 ? g_ngx_only_ti : env_only;   // runtime /ngxonly overrides env
        if (only_ti >= 0 && ti != only_ti) continue;
        static const int env_skip = getenv("SUNBRIGHT_NGX_SKIPTI") ? atoi(getenv("SUNBRIGHT_NGX_SKIPTI")) : -1;
        const int skip_ti = g_ngx_skip_ti != -2 ? g_ngx_skip_ti : env_skip;   // runtime /ngxskip overrides env
        if (skip_ti >= 0 && ti == skip_ti) continue;
        if (g_ngx_skip_set_n && in_skipset(ti)) continue;   // /ngxskipset multi-ti removal
        // EFB-copy epoch isolation (/ngxepoch) — render only / all-but a given offscreen epoch.
        if (g_ngx_only_epoch >= 0 && (int)batches[b].epoch != g_ngx_only_epoch) continue;
        if (g_ngx_drop_epoch >= 0 && (int)batches[b].epoch == g_ngx_drop_epoch) continue;
        if ((int)batches[b].epoch < display_epoch) continue;   // auxiliary offscreen render — not displayed
        VkPipeline pipe = pipeline_for(st);
        if (pipe == VK_NULL_HANDLE) pipe = pipeline_for(mod);
        if (pipe == VK_NULL_HANDLE) continue;
        if (pipe != last) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe); last = pipe; }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pll, 0, 1, &bset[b], 0, nullptr);
        MatPC pc; std::memset(&pc, 0, sizeof pc);
        if (ti >= 0 && ti < (int)tevstates.size()) {
            for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) pc.kcolor[c][k] = st.kcolor[c][k];
            for (int c = 0; c < 3; c++) for (int k = 0; k < 4; k++) pc.tevreg[c][k] = st.tev_color[c][k];
        } else for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) pc.kcolor[c][k] = 255;
        // DBG cat mode: override kcolor[0] with a category colour (the cat shader outputs it).
        const int s_dbg = sb_ngx_tevdbg();
        if (s_dbg == 3) {
            unsigned cc = ngx_tev_cc_dbg(ti);
            int rC, gC, bC;
            if (cc == 0xFFFF)      { rC=128; gC=128; bC=128; }   // no block = gray
            else if (cc & 1)       { rC=0;   gC=255; bC=(cc&2)?255:0; }  // VTX: green (lit=cyan)
            else                   { rC=255; gC=0;   bC=(cc&2)?255:0; }  // REG: red (lit=magenta)
            pc.kcolor[0][0]=rC; pc.kcolor[0][1]=gC; pc.kcolor[0][2]=bC; pc.kcolor[0][3]=255;
        } else if (s_dbg == 4) {   // encode tev_index as colour (R=lo,G=hi)
            pc.kcolor[0][0]=ti & 0xFF; pc.kcolor[0][1]=(ti>>8)&0xFF; pc.kcolor[0][2]=0; pc.kcolor[0][3]=255;
        }
        vkCmdPushConstants(cmd, pll, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        vkCmdDraw(cmd, batches[b].vcount, 1, batches[b].vstart, 0);
        drawn++;
        if (s_cloudcount >= 0 && ti == s_cloudcount) { cc_batches++; cc_verts += batches[b].vcount; }
    }
    if (s_cloudcount >= 0 && (cc_frame++ % 120) == 0)
        fprintf(stderr, "[cloudcount] ti=%d DRAWN batches=%d verts=%ld (overdraw if batches>>1)\n",
                s_cloudcount, cc_batches, cc_verts);

    // Delfino plaza pollution darkening — over the 3D scene (depth valid), before the HUD.
    draw_pollution(cmd, w, h);

    // HUD/J2D overlay over the 3D scene (alpha-blended, depth off, dynamic viewport).
    if (j2d_pipe != VK_NULL_HANDLE && !j2d.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, j2d_pipe);
        VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
        VkRect2D scr{{0, 0}, {(uint32_t)w, (uint32_t)h}};
        vkCmdSetViewport(cmd, 0, 1, &vp); vkCmdSetScissor(cmd, 0, 1, &scr);
        for (const J2dDraw& d : j2d) {
            struct { float rect[4]; float misc[4]; uint32_t corners[4]; uint32_t bw[4]; float uvrect[4]; } pc;
            std::memcpy(pc.rect, d.rect, sizeof pc.rect);
            std::memcpy(pc.misc, d.misc, sizeof pc.misc);
            std::memcpy(pc.corners, d.corners, sizeof pc.corners);
            std::memcpy(pc.bw, d.bw, sizeof pc.bw);
            std::memcpy(pc.uvrect, d.uvrect, sizeof pc.uvrect);
            vkCmdPushConstants(cmd, j2d_pll, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, j2d_pll, 0, 1, &d.dset, 0, nullptr);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
    }

    vkCmdEndRenderPass(cmd);   // target now SHADER_READ_ONLY (render-pass finalLayout)
    vkEndCommandBuffer(cmd);

    VkSubmitInfo su{VK_STRUCTURE_TYPE_SUBMIT_INFO}; su.commandBufferCount = 1; su.pCommandBuffers = &cmd;
    vkResetFences(dev, 1, &fence);
    if (vkQueueSubmit(queue, 1, &su, fence)) { for (size_t i = 0; i < stg_bufs.size(); i++) { vkDestroyBuffer(dev, stg_bufs[i], nullptr); vkFreeMemory(dev, stg_mems[i], nullptr); } return nullptr; }
    vkWaitForFences(dev, 1, &fence, VK_TRUE, 10'000'000'000ull);
    for (size_t i = 0; i < stg_bufs.size(); i++) { vkDestroyBuffer(dev, stg_bufs[i], nullptr); vkFreeMemory(dev, stg_mems[i], nullptr); }

    // Dolphin's tracked layout now matches the render pass's final layout.
    static_cast<Vulkan::VKTexture*>(target.get())->OverrideImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    g_frames++;
    return target.get();
}

}  // namespace

// Called on the video thread by the fork's Presenter when SUNBRIGHT_NGX_PRESENT is on.
// Returns the AbstractTexture holding the freshly-rendered ngx frame at w×h (the XFB
// dims), or nullptr if not ready / no geometry (caller keeps the real XFB).
// interp60 present cadence: the fork's VI presents once per FIELD (~60/s) while the 30 Hz game
// publishes a new ngx snapshot once per frame (~30/s) — so every other present is a "repeat" field
// showing the same frame id. On a repeat field we render the N-1↔N blend (interp mode 1) instead of
// re-showing N → 60 fps from the engine's own snapshots, no guest driver, no GX replay. Counters
// feed /interp60.
extern "C" unsigned long sb_ngx_front_frame();      // ngx_j3d_shape.cpp — published snapshot frame id
extern "C" { extern volatile int g_sb_ab_capture; } // Present.cpp — /abshot2 wants a stable (non-blend) frame
unsigned long g_interp_presents = 0, g_interp_blends = 0;
extern "C" const void* sb_ngx_present_xfb(int w, int h) {
    if (!g_pr.init_tried) { g_pr.init_tried = true; g_pr.init_ok = g_pr.init(); }
    if (!g_pr.init_ok) return nullptr;
    if (ngx_interp60_enabled() && !g_sb_ab_capture) {
        static unsigned long s_last_ff = ~0ul;
        const unsigned long ff = sb_ngx_front_frame();
        const bool repeat = (ff == s_last_ff);
        s_last_ff = ff;
        g_interp_presents++;
        // PHASE: at frame N (just published) we have N-1 and N. For MONOTONIC motion the in-between
        // (N-1↔N midpoint) must be shown BEFORE N, not after — so the NEW-frame field shows the
        // blend (=N-0.5) and the following REPEAT field shows N. That gives display positions
        // 0.5,1.0,1.5,2.0,… (smooth). The reverse (N then blend) is a sawtooth judder = the bug.
        sb_ngx_set_interp_mode(repeat ? 0 : 1);
        if (!repeat) g_interp_blends++;
    } else {
        sb_ngx_set_interp_mode(0);
    }
    return g_pr.render(w, h);
}

extern "C" void sb_ngx_interp_stats(unsigned long* presents, unsigned long* blends) {
    *presents = g_interp_presents; *blends = g_interp_blends;
}

// Runtime render-debug toggle (/ngxdbg): set the TEV-debug mode on a LIVE scene; the next
// frame clears the pipeline cache and regenerates shaders for the new mode. m: 0=normal
// 1=tex 2=ras 3=cat 4=bid. Returns the mode set.
extern "C" int sb_ngx_set_dbg(int m) { g_ngx_tevdbg = m; return m; }

// Diagnostics for the /ngxpresentlive probe.
extern "C" void sb_ngx_present_stats(unsigned long* frames, unsigned long* pipes, unsigned long* texes,
                                     int* w, int* h, int* ok, unsigned long* j2d_quads) {
    *frames = g_pr.g_frames; *pipes = g_pr.g_pipe_builds; *texes = g_pr.g_tex_decodes;
    *w = g_pr.tw; *h = g_pr.th; *ok = g_pr.init_ok ? 1 : 0; *j2d_quads = g_pr.g_j2d_quads;
}

#else
extern "C" const void* sb_ngx_present_xfb(int, int) { return nullptr; }
extern "C" void sb_ngx_present_stats(unsigned long* f, unsigned long* p, unsigned long* t, int* w, int* h, int* ok, unsigned long* j2d) {
    *f = *p = *t = 0; *w = *h = 0; *ok = 0; *j2d = 0;
}
#endif
