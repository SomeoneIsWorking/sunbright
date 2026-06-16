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
extern "C" unsigned ngx_tev_cc_dbg(int);
extern "C" int g_ngx_tevdbg;       // render-debug mode (tev_shader.cpp); /ngxdbg sets it
extern "C" int sb_ngx_tevdbg();
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

    unsigned long g_frames = 0, g_pipe_builds = 0, g_tex_decodes = 0, g_j2d_quads = 0;
    int dbg_mode_built = -2;    // tev-debug mode the cached pipelines were built for; mismatch → rebuild

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
    rs.cullMode = kCull[st.pe.cull & 3];
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE; rs.lineWidth = 1.0f;
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
    static const bool s_noblend = getenv("SUNBRIGHT_NGX_NOBLEND") && atoi(getenv("SUNBRIGHT_NGX_NOBLEND")) != 0;
    if (s_noblend) cba.blendEnable = VK_FALSE;
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
    const uint64_t key = ((uint64_t)t.addr << 16) ^ ((uint64_t)t.fmt << 56) ^
                         ((uint64_t)t.w << 40) ^ ((uint64_t)t.h << 28) ^ ((uint64_t)t.tlut_addr << 4);
    auto it = texcache.find(key);
    if (it != texcache.end()) return it->second.view;

    const uint8_t* host = sb_ram_fast(t.addr);
    if (!host) return white_view;
    const int srcbytes = sb_tex_size_bytes(t.w, t.h, t.fmt);
    if (srcbytes <= 0 || (t.addr & 0x01FFFFFFu) + (uint32_t)srcbytes > 0x1800000u) return white_view;
    const uint8_t* tlut = nullptr;
    if (t.fmt == SB_TF_C4 || t.fmt == SB_TF_C8 || t.fmt == SB_TF_C14X2) {
        const uint32_t entries = t.fmt == SB_TF_C4 ? 16u : t.fmt == SB_TF_C8 ? 256u : 16384u;
        if (t.tlut_addr && (t.tlut_addr & 0x01FFFFFFu) + entries * 2u <= 0x1800000u) tlut = sb_ram_fast(t.tlut_addr);
        if (!tlut) return white_view;
    }
    const size_t rgba = (size_t)t.w * t.h * 4;
    VkBuffer sbuf; VkDeviceMemory smem;
    if (!make_buffer(dev, phys, sbuf, smem, rgba, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return white_view;
    void* p = nullptr; vkMapMemory(dev, smem, 0, rgba, 0, &p);
    sb_tex_decode((uint32_t*)p, host, t.w, t.h, t.fmt, tlut, t.tlut_fmt);
    vkUnmapMemory(dev, smem);
    stg_bufs.push_back(sbuf); stg_mems.push_back(smem);

    // Mip chain: GameCube samples a texture's mips; a heavily-tiled/minified surface (e.g. the
    // plaza floor) averages to a darker value, while a single mip0 + bilinear ALIASES toward the
    // bright tile faces → washed-out. Generate a box-filtered mip chain (vkCmdBlitImage, linear)
    // and sample it trilinearly. (GC TIMGs can store mips; box-generated mips approximate them and
    // fix the aliasing — the dominant cause of the ngx 3D wash.)
    uint32_t mips = 1; { int m = t.w > t.h ? t.w : t.h; while (m > 1) { m >>= 1; mips++; } }
    TexEntry te{};
    if (!make_dev_image(dev, phys, te.img, te.mem, te.view, VK_FORMAT_R8G8B8A8_UNORM, t.w, t.h,
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
    VkBufferImageCopy c{}; c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; c.imageExtent = {(uint32_t)t.w, (uint32_t)t.h, 1};
    vkCmdCopyBufferToImage(up_cmd, sbuf, te.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    // Generate mips by successive linear blits (i-1 → i), box-filtering down the chain.
    int mw = t.w, mh = t.h;
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
        // GC textures store rows padded to the format's block dims; pad to a multiple
        // of 8 (the largest block) so the tiled decode never over-runs (same as j2d_render).
        NgxTexBind tb{};
        tb.addr = q.data; tb.w = (uint16_t)((q.w + 7) & ~7); tb.h = (uint16_t)((q.h + 7) & ~7);
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

    if (w < 1 || h < 1) return nullptr;
    // Render-debug mode changed (via /ngxdbg)? Drop cached pipelines so shaders regenerate.
    if (int m = sb_ngx_tevdbg(); m != dbg_mode_built) { if (init_ok && dev) clear_pipes(); dbg_mode_built = m; }
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

    VkClearValue clear[2]{}; clear[0].color = {{0.10f, 0.12f, 0.18f, 1.f}}; clear[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rpass; rbi.framebuffer = fbo; rbi.renderArea = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    rbi.clearValueCount = 2; rbi.pClearValues = clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    VkDeviceSize voff = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &voff);
    VkPipeline last = VK_NULL_HANDLE;
    for (size_t b = 0; b < batches.size(); b++) {
        const int ti = batches[b].tev_index;
        const NgxTevState& st = (ti >= 0 && ti < (int)tevstates.size()) ? tevstates[ti] : mod;
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
    }

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
extern "C" const void* sb_ngx_present_xfb(int w, int h) {
    if (!g_pr.init_tried) { g_pr.init_tried = true; g_pr.init_ok = g_pr.init(); }
    if (!g_pr.init_ok) return nullptr;
    return g_pr.render(w, h);
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
