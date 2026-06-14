// j2d_render — N3 milestone: render the game's J2D HUD NATIVELY.
//
// Ties the native renderer together end-to-end with NO Dolphin VideoCommon and NO
// GX: take the live HUD draw list (sb_j2d_collect, from the J2D pane tree), for
// each picture read its raw GC texture bytes from guest RAM, decode with our
// native decoder (sb_tex_decode), upload, and draw a textured quad at the pane's
// screen rect through our own Vulkan ortho pipeline. Renders OFFSCREEN + reads
// back (+ dumps a PPM) so it is verifiable headless without touching the game's
// present. This is the first frame of actual GAME CONTENT drawn by our renderer.
//
// Bring-up scaffold (same as vk_quad): reuses Dolphin's VkDevice via
// g_vulkan_context; one-shot on-demand (the /j2drender probe), not per-frame —
// the production path integrates with the frame cycle later.

#include "tex_decode.h"
#include "j2d_types.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <vector>

#ifdef HAVE_DOLPHIN_MEMMAP
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "render/shaders/quad_ortho_vert_spv.h"
#include "render/shaders/quad_modulate_frag_spv.h"

extern unsigned mem_r32(unsigned);   // guest RAM word read (big-endian-interpreted); intrinsics.h

namespace {

struct Rep {
    char* out; int cap; int pos = 0;
    void operator()(const char* f, ...) { if (pos >= cap) return;
        va_list a; va_start(a, f); pos += vsnprintf(out + pos, cap - pos, f, a); va_end(a); }
};

uint32_t find_mem(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

// Copy `n` raw bytes from guest RAM (as stored, big-endian) into host buffer.
void read_guest(uint8_t* dst, uint32_t addr, int n) {
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t w = mem_r32(addr + i);
        dst[i] = w >> 24; dst[i+1] = w >> 16; dst[i+2] = w >> 8; dst[i+3] = w;
    }
    if (i < n) { uint32_t w = mem_r32(addr + i); for (int k = 0; i < n; i++, k++) dst[i] = w >> (24 - 8*k); }
}

struct Tex { VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE;
             VkImageView view = VK_NULL_HANDLE; VkDescriptorSet dset = VK_NULL_HANDLE; };

}  // namespace

int sb_j2d_render(char* outbuf, int cap) {
    Rep rep{outbuf, cap};

    int sw = 0, sh = 0;
    J2dQuad quads[16];
    int nq = sb_j2d_collect(quads, 16, &sw, &sh);
    if (nq == 0) { rep("j2drender: no HUD quads (no live J2D root / no visible pictures yet)\n"); return -1; }
    if (sw <= 0 || sw > 2048) sw = 640;
    if (sh <= 0 || sh > 2048) sh = 480;
    rep("j2drender: %d quads, screen %dx%d\n", nq, sw, sh);

    if (!Vulkan::g_vulkan_context) { rep("j2drender: no Vulkan context\n"); return -2; }
    const VkDevice dev = Vulkan::g_vulkan_context->GetDevice();
    const VkPhysicalDevice phys = Vulkan::g_vulkan_context->GetPhysicalDevice();
    const VkQueue queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
    const uint32_t qfam = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();

    const VkDeviceSize rt_bytes = (VkDeviceSize)sw * sh * 4;

    VkImage rt = VK_NULL_HANDLE; VkDeviceMemory rt_mem = VK_NULL_HANDLE; VkImageView rt_view = VK_NULL_HANDLE;
    VkBuffer rb = VK_NULL_HANDLE; VkDeviceMemory rb_mem = VK_NULL_HANDLE;
    VkSampler samp = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE; VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkPipelineLayout pll = VK_NULL_HANDLE; VkRenderPass rpass = VK_NULL_HANDLE;
    VkFramebuffer fbo = VK_NULL_HANDLE; VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE; VkCommandBuffer cmd = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    Tex tex[16] = {};
    std::vector<VkBuffer> staging; std::vector<VkDeviceMemory> staging_mem;
    int result = -3; VkResult vr = VK_SUCCESS; const char* step = "start";
    auto FAIL = [&](const char* s){ step = s; rep("j2drender: FAIL at %s (VkResult=%d)\n", s, (int)vr); };

    auto mk_target = [&]() -> bool {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)sw,(uint32_t)sh,1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((vr = vkCreateImage(dev, &ici, nullptr, &rt))) return false;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, rt, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size; ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if ((vr = vkAllocateMemory(dev, &ai, nullptr, &rt_mem))) return false;
        if ((vr = vkBindImageMemory(dev, rt, rt_mem, 0))) return false;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = rt; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        return (vr = vkCreateImageView(dev, &vci, nullptr, &rt_view)) == VK_SUCCESS;
    };
    auto mk_buffer = [&](VkBuffer& b, VkDeviceMemory& m, VkDeviceSize sz, VkBufferUsageFlags u, VkMemoryPropertyFlags props) -> bool {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bci.size = sz; bci.usage = u;
        if ((vr = vkCreateBuffer(dev, &bci, nullptr, &b))) return false;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, props);
        if ((vr = vkAllocateMemory(dev, &ai, nullptr, &m))) return false;
        return (vr = vkBindBufferMemory(dev, b, m, 0)) == VK_SUCCESS;
    };

    // Decode each picture's texture (skip paletted for this milestone) and stage it.
    struct Decoded { int idx; int w, h; std::vector<uint32_t> rgba; };
    std::vector<Decoded> dec;
    for (int i = 0; i < nq; i++) {
        const J2dQuad& q = quads[i];
        if (sb_tex_is_paletted(q.fmt)) { rep("  quad %d: SKIP paletted fmt=%d (%dx%d)\n", i, q.fmt, q.w, q.h); continue; }
        // GC textures are stored padded to the format's block dims; pad to a
        // multiple of 8 (the largest block) so the tiled decode never over-runs.
        const int wp = (q.w + 7) & ~7, hp = (q.h + 7) & ~7;
        const int src_bytes = sb_tex_size_bytes(wp, hp, q.fmt);
        if (src_bytes <= 0 || src_bytes > (8 << 20)) { rep("  quad %d: bad size %d\n", i, src_bytes); continue; }
        std::vector<uint8_t> raw(src_bytes);
        read_guest(raw.data(), q.data, src_bytes);
        Decoded d; d.idx = i; d.w = wp; d.h = hp; d.rgba.assign((size_t)wp * hp, 0);
        sb_tex_decode(d.rgba.data(), raw.data(), wp, hp, q.fmt, nullptr, 0);
        rep("  quad %d: rect[%d,%d %dx%d] fmt=%d tex %dx%d colorAlpha=%u corner0=%08x\n", i, q.x0, q.y0,
            q.x1 - q.x0, q.y1 - q.y0, q.fmt, q.w, q.h, q.alpha, q.corner[0]);
        dec.push_back(std::move(d));
    }
    if (dec.empty()) { rep("j2drender: no non-paletted quads to draw\n"); return 0; }

    if (!mk_target()) { FAIL("target"); goto done; }
    if (!mk_buffer(rb, rb_mem, rt_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { FAIL("readback"); goto done; }
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if ((vr = vkCreateSampler(dev, &sci, nullptr, &samp))) { FAIL("sampler"); goto done; }
    }
    {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = sizeof kQuadOrthoVertSpv; si.pCode = kQuadOrthoVertSpv;
        if ((vr = vkCreateShaderModule(dev, &si, nullptr, &vs))) { FAIL("vs"); goto done; }
        si.codeSize = sizeof kQuadModulateFragSpv; si.pCode = kQuadModulateFragSpv;
        if ((vr = vkCreateShaderModule(dev, &si, nullptr, &fs))) { FAIL("fs"); goto done; }
    }
    {
        VkDescriptorSetLayoutBinding b{}; b.binding = 0; b.descriptorCount = 1;
        b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1; li.pBindings = &b;
        if ((vr = vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl))) { FAIL("dsl"); goto done; }
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)dec.size()};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = (uint32_t)dec.size(); pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if ((vr = vkCreateDescriptorPool(dev, &pci, nullptr, &dpool))) { FAIL("dpool"); goto done; }
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 48};  // rect + misc(target,colorAlpha) + corners
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1; plci.pSetLayouts = &dsl; plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if ((vr = vkCreatePipelineLayout(dev, &plci, nullptr, &pll))) { FAIL("pll"); goto done; }
    }
    {
        VkAttachmentDescription at{}; at.format = VK_FORMAT_R8G8B8A8_UNORM; at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sd{}; sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sd.colorAttachmentCount = 1; sd.pColorAttachments = &ar;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1; rpci.pAttachments = &at; rpci.subpassCount = 1; rpci.pSubpasses = &sd;
        if ((vr = vkCreateRenderPass(dev, &rpci, nullptr, &rpass))) { FAIL("rpass"); goto done; }
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rpass; fci.attachmentCount = 1; fci.pAttachments = &rt_view;
        fci.width = sw; fci.height = sh; fci.layers = 1;
        if ((vr = vkCreateFramebuffer(dev, &fci, nullptr, &fbo))) { FAIL("fbo"); goto done; }
    }
    {
        VkPipelineShaderStageCreateInfo ss[2] = {};
        ss[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT; ss[0].module = vs; ss[0].pName = "main";
        ss[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module = fs; ss[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        VkViewport vp{0,0,(float)sw,(float)sh,0,1}; VkRect2D scc{{0,0},{(uint32_t)sw,(uint32_t)sh}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scc;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{}; cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
        cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.alphaBlendOp = VK_BLEND_OP_ADD;
        cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = ss; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
        gp.layout = pll; gp.renderPass = rpass;
        if ((vr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe))) { FAIL("pipeline"); goto done; }
    }

    // Per-decoded-texture image + staging + descriptor set.
    for (size_t i = 0; i < dec.size(); i++) {
        const Decoded& d = dec[i];
        const VkDeviceSize sz = (VkDeviceSize)d.w * d.h * 4;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)d.w,(uint32_t)d.h,1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((vr = vkCreateImage(dev, &ici, nullptr, &tex[i].img))) { FAIL("tex image"); goto done; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, tex[i].img, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if ((vr = vkAllocateMemory(dev, &ai, nullptr, &tex[i].mem))) { FAIL("tex mem"); goto done; }
        vkBindImageMemory(dev, tex[i].img, tex[i].mem, 0);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = tex[i].img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if ((vr = vkCreateImageView(dev, &vci, nullptr, &tex[i].view))) { FAIL("tex view"); goto done; }

        VkBuffer sb; VkDeviceMemory sm;
        if (!mk_buffer(sb, sm, sz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { FAIL("tex staging"); goto done; }
        void* p = nullptr; vkMapMemory(dev, sm, 0, sz, 0, &p); memcpy(p, d.rgba.data(), sz); vkUnmapMemory(dev, sm);
        staging.push_back(sb); staging_mem.push_back(sm);

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
        if ((vr = vkAllocateDescriptorSets(dev, &dai, &tex[i].dset))) { FAIL("dset"); goto done; }
        VkDescriptorImageInfo dii{samp, tex[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = tex[i].dset; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    {
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex = qfam; cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if ((vr = vkCreateCommandPool(dev, &cpci, nullptr, &cpool))) { FAIL("cpool"); goto done; }
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool = cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        if ((vr = vkAllocateCommandBuffers(dev, &cbai, &cmd))) { FAIL("cmd"); goto done; }
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if ((vr = vkCreateFence(dev, &fci, nullptr, &fence))) { FAIL("fence"); goto done; }
    }
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ((vr = vkBeginCommandBuffer(cmd, &bi))) { FAIL("begin"); goto done; }
        auto barrier = [&](VkImage im, VkImageLayout f, VkImageLayout t, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ssg, VkPipelineStageFlags dsg) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.oldLayout = f; b.newLayout = t;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = im;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; b.srcAccessMask = sa; b.dstAccessMask = da;
            vkCmdPipelineBarrier(cmd, ssg, dsg, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        for (size_t i = 0; i < dec.size(); i++) {
            barrier(tex[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy cp{}; cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; cp.imageExtent = {(uint32_t)dec[i].w,(uint32_t)dec[i].h,1};
            vkCmdCopyBufferToImage(cmd, staging[i], tex[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            barrier(tex[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }
        VkClearValue clear{}; clear.color = {{0.f,0.f,0.f,1.f}};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rpass; rbi.framebuffer = fbo; rbi.renderArea = {{0,0},{(uint32_t)sw,(uint32_t)sh}};
        rbi.clearValueCount = 1; rbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        for (size_t i = 0; i < dec.size(); i++) {
            const J2dQuad& q = quads[dec[i].idx];
            // Push constant: vec4 rect | vec4 misc(target.xy, colorAlpha, pad) | uvec4 corners.
            struct { float rect[4]; float misc[4]; uint32_t corners[4]; } pc;
            pc.rect[0] = (float)q.x0; pc.rect[1] = (float)q.y0; pc.rect[2] = (float)q.x1; pc.rect[3] = (float)q.y1;
            pc.misc[0] = (float)sw; pc.misc[1] = (float)sh; pc.misc[2] = q.alpha / 255.0f; pc.misc[3] = 0;
            for (int c = 0; c < 4; c++) pc.corners[c] = q.corner[c];
            vkCmdPushConstants(cmd, pll, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, &pc);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pll, 0, 1, &tex[i].dset, 0, nullptr);
            vkCmdDraw(cmd, 4, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
        VkBufferImageCopy rc{}; rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; rc.imageExtent = {(uint32_t)sw,(uint32_t)sh,1};
        vkCmdCopyImageToBuffer(cmd, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &rc);
        if ((vr = vkEndCommandBuffer(cmd))) { FAIL("end"); goto done; }
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if ((vr = vkQueueSubmit(queue, 1, &si, fence))) { FAIL("submit"); goto done; }
        if ((vr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5'000'000'000ull))) { FAIL("wait"); goto done; }
    }
    {
        void* p = nullptr;
        if ((vr = vkMapMemory(dev, rb_mem, 0, rt_bytes, 0, &p))) { FAIL("map"); goto done; }
        const uint32_t* px = (const uint32_t*)p;
        long nonblack = 0; for (int i = 0; i < sw * sh; i++) if ((px[i] & 0x00FFFFFF) != 0) nonblack++;
        // dump a PPM for visual inspection
        mkdir("scratch", 0755); mkdir("scratch/render", 0755);
        char path[96]; snprintf(path, sizeof path, "scratch/render/hud_%dx%d.ppm", sw, sh);
        if (FILE* f = fopen(path, "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", sw, sh);
            std::vector<uint8_t> row(sw * 3);
            for (int y = 0; y < sh; y++) { for (int x = 0; x < sw; x++) { uint32_t v = px[y*sw+x]; row[x*3]=v; row[x*3+1]=v>>8; row[x*3+2]=v>>16; } fwrite(row.data(),1,row.size(),f); }
            fclose(f);
        }
        rep("j2drender: drew %zu quads -> %ld/%d non-black px (%.1f%%) -> %s\n",
            dec.size(), nonblack, sw*sh, 100.0*nonblack/(sw*sh), path);
        rep("verdict=%s\n", nonblack > 0 ? "RENDERED" : "EMPTY");
        result = (nonblack > 0) ? 0 : 1;
        vkUnmapMemory(dev, rb_mem);
    }

done:
    vkDeviceWaitIdle(dev);
    for (auto m : staging_mem) if (m) vkFreeMemory(dev, m, nullptr);
    for (auto b : staging) if (b) vkDestroyBuffer(dev, b, nullptr);
    for (auto& t : tex) { if (t.view) vkDestroyImageView(dev,t.view,nullptr); if (t.img) vkDestroyImage(dev,t.img,nullptr); if (t.mem) vkFreeMemory(dev,t.mem,nullptr); }
    if (fence) vkDestroyFence(dev,fence,nullptr);
    if (cpool) vkDestroyCommandPool(dev,cpool,nullptr);
    if (pipe) vkDestroyPipeline(dev,pipe,nullptr);
    if (fbo) vkDestroyFramebuffer(dev,fbo,nullptr);
    if (rpass) vkDestroyRenderPass(dev,rpass,nullptr);
    if (pll) vkDestroyPipelineLayout(dev,pll,nullptr);
    if (dpool) vkDestroyDescriptorPool(dev,dpool,nullptr);
    if (dsl) vkDestroyDescriptorSetLayout(dev,dsl,nullptr);
    if (vs) vkDestroyShaderModule(dev,vs,nullptr);
    if (fs) vkDestroyShaderModule(dev,fs,nullptr);
    if (samp) vkDestroySampler(dev,samp,nullptr);
    if (rb) vkDestroyBuffer(dev,rb,nullptr);
    if (rb_mem) vkFreeMemory(dev,rb_mem,nullptr);
    if (rt_view) vkDestroyImageView(dev,rt_view,nullptr);
    if (rt) vkDestroyImage(dev,rt,nullptr);
    if (rt_mem) vkFreeMemory(dev,rt_mem,nullptr);
    (void)step;
    return result;
}

#else
int sb_j2d_render(char* out, int cap) { snprintf(out, cap, "j2drender: unavailable\n"); return -1; }
#endif
