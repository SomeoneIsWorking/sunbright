// vk_quad — N2 of the native-renderer plan (docs/native_port_plan.md §3): the
// first native pixels. Our OWN Vulkan pipeline (our shaders, pipeline, sampler,
// vertex generation) renders a textured quad — proving we can drive the GPU
// ourselves, end-to-end, independent of Dolphin's VideoCommon renderer.
//
// Bring-up scaffold: we reuse Dolphin's already-created VkDevice/queue/physical-
// device (Vulkan::g_vulkan_context) rather than standing up a second device — the
// plan sanctions sharing the device during bring-up; it is removed when Dolphin's
// video is unlinked. We do NOT use any of Dolphin's VideoCommon renderer objects;
// only the raw device + the loader's vk* entry points.
//
// This first milestone renders OFFSCREEN and reads the result back, so it is
// fully verifiable HEADLESS (no swapchain/present, no fight with the game's frame
// cycle): decode a GameCube texture with our native decoder (tex_decode), upload
// it, render a fullscreen-triangle quad sampling it (NEAREST) into a same-sized
// target, read the target back, and require it to equal the decoded texels
// exactly. That exercises the whole native path: texture upload → sampler →
// pipeline → fragment output → readback.
//
// THREADING CAVEAT (bring-up only): vkQueueSubmit here shares the graphics queue
// with Dolphin's render thread without external sync. It is a one-shot, on-demand
// self-test (the /vkquad probe endpoint), typically triggered early; the
// production renderer (N3+) integrates with the frame cycle / command-buffer
// manager properly. Do not promote this submit path to per-frame use as-is.

#include "tex_decode.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef HAVE_DOLPHIN_MEMMAP
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "render/shaders/quad_vert_spv.h"
#include "render/shaders/quad_frag_spv.h"

namespace {

struct Report {
    char* out; int cap; int pos = 0;
    void operator()(const char* fmt, ...) {
        if (pos >= cap) return;
        va_list ap; va_start(ap, fmt);
        pos += vsnprintf(out + pos, cap - pos, fmt, ap);
        va_end(ap);
    }
};

uint32_t find_mem(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    return UINT32_MAX;
}

}  // namespace

// Render the textured-quad self-test. Returns mismatching-texel count (0 = OK),
// or -1 if Vulkan/device unavailable, or -2 on a Vulkan API failure (see report).
int sb_vk_quad_selftest(char* outbuf, int cap) {
    Report rep{outbuf, cap};

    if (!Vulkan::g_vulkan_context) { rep("vk_quad: no g_vulkan_context (Vulkan backend not up)\n"); return -1; }
    const VkDevice dev = Vulkan::g_vulkan_context->GetDevice();
    const VkPhysicalDevice phys = Vulkan::g_vulkan_context->GetPhysicalDevice();
    const VkQueue queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
    const uint32_t qfam = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();
    if (dev == VK_NULL_HANDLE) { rep("vk_quad: null device\n"); return -1; }

    const int W = 64, H = 64;

    // ── Expected image: decode a GameCube RGB5A3 texture with our native decoder.
    // Its decoded RGBA8 is BOTH the texture we upload AND the expected output.
    std::vector<uint8_t> gc(sb_tex_size_bytes(W, H, SB_TF_RGB5A3));
    for (size_t i = 0; i < gc.size(); i++) gc[i] = (uint8_t)(i * 7 + (i >> 3) * 131 + 1);  // deterministic
    std::vector<uint32_t> expected((size_t)W * H);
    sb_tex_decode(expected.data(), gc.data(), W, H, SB_TF_RGB5A3, nullptr, 0);
    const VkDeviceSize img_bytes = (VkDeviceSize)W * H * 4;

    // Handles (destroyed at the end; VK_NULL_HANDLE-safe).
    VkImage tex_img = VK_NULL_HANDLE, rt_img = VK_NULL_HANDLE;
    VkDeviceMemory tex_mem = VK_NULL_HANDLE, rt_mem = VK_NULL_HANDLE;
    VkImageView tex_view = VK_NULL_HANDLE, rt_view = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkBuffer up_buf = VK_NULL_HANDLE, rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory up_mem = VK_NULL_HANDLE, rb_mem = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkPipelineLayout pll = VK_NULL_HANDLE;
    VkRenderPass rpass = VK_NULL_HANDLE;
    VkFramebuffer fbo = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    int result = -2;
    const char* step = "start";
    VkResult vr = VK_SUCCESS;
    auto FAIL = [&](const char* s) { step = s; rep("vk_quad: FAIL at %s (VkResult=%d)\n", s, (int)vr); };

    auto make_image = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view, VkImageUsageFlags usage) -> bool {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)W, (uint32_t)H, 1};
        ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((vr = vkCreateImage(dev, &ici, nullptr, &img))) return false;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if ((vr = vkAllocateMemory(dev, &ai, nullptr, &mem))) return false;
        if ((vr = vkBindImageMemory(dev, img, mem, 0))) return false;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return (vr = vkCreateImageView(dev, &vci, nullptr, &view)) == VK_SUCCESS;
    };
    auto make_buffer = [&](VkBuffer& b, VkDeviceMemory& m, VkDeviceSize sz, VkBufferUsageFlags usage) -> bool {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sz; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if ((vr = vkCreateBuffer(dev, &bci, nullptr, &b))) return false;
        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if ((vr = vkAllocateMemory(dev, &ai, nullptr, &m))) return false;
        return (vr = vkBindBufferMemory(dev, b, m, 0)) == VK_SUCCESS;
    };
    auto make_shader = [&](VkShaderModule& sm, const uint32_t* code, size_t bytes) -> bool {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = bytes; si.pCode = code;
        return (vr = vkCreateShaderModule(dev, &si, nullptr, &sm)) == VK_SUCCESS;
    };

    // ── Resources ───────────────────────────────────────────────────────────────
    if (!make_image(tex_img, tex_mem, tex_view, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { FAIL("tex image"); goto done; }
    if (!make_image(rt_img, rt_mem, rt_view, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) { FAIL("rt image"); goto done; }
    if (!make_buffer(up_buf, up_mem, img_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) { FAIL("upload buf"); goto done; }
    if (!make_buffer(rb_buf, rb_mem, img_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) { FAIL("readback buf"); goto done; }
    {
        void* p = nullptr;
        if ((vr = vkMapMemory(dev, up_mem, 0, img_bytes, 0, &p))) { FAIL("map upload"); goto done; }
        memcpy(p, expected.data(), img_bytes);
        vkUnmapMemory(dev, up_mem);
    }

    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if ((vr = vkCreateSampler(dev, &sci, nullptr, &sampler))) { FAIL("sampler"); goto done; }
    }
    if (!make_shader(vs, kQuadVertSpv, sizeof kQuadVertSpv)) { FAIL("vert shader"); goto done; }
    if (!make_shader(fs, kQuadFragSpv, sizeof kQuadFragSpv)) { FAIL("frag shader"); goto done; }

    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1; li.pBindings = &b;
        if ((vr = vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl))) { FAIL("dsl"); goto done; }

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if ((vr = vkCreateDescriptorPool(dev, &pci, nullptr, &dpool))) { FAIL("dpool"); goto done; }

        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
        if ((vr = vkAllocateDescriptorSets(dev, &dai, &dset))) { FAIL("dset alloc"); goto done; }

        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
        if ((vr = vkCreatePipelineLayout(dev, &plci, nullptr, &pll))) { FAIL("pipeline layout"); goto done; }
    }

    {
        VkAttachmentDescription at{};
        at.format = VK_FORMAT_R8G8B8A8_UNORM; at.samples = VK_SAMPLE_COUNT_1_BIT;
        at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sd{}; sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sd.colorAttachmentCount = 1; sd.pColorAttachments = &ar;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1; rpci.pAttachments = &at; rpci.subpassCount = 1; rpci.pSubpasses = &sd;
        if ((vr = vkCreateRenderPass(dev, &rpci, nullptr, &rpass))) { FAIL("render pass"); goto done; }

        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rpass; fci.attachmentCount = 1; fci.pAttachments = &rt_view;
        fci.width = W; fci.height = H; fci.layers = 1;
        if ((vr = vkCreateFramebuffer(dev, &fci, nullptr, &fbo))) { FAIL("framebuffer"); goto done; }
    }

    {
        VkPipelineShaderStageCreateInfo ss[2] = {};
        ss[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT; ss[0].module = vs; ss[0].pName = "main";
        ss[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module = fs; ss[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, (float)W, (float)H, 0, 1};
        VkRect2D sc{{0, 0}, {(uint32_t)W, (uint32_t)H}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = ss; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
        gp.pColorBlendState = &cb; gp.layout = pll; gp.renderPass = rpass; gp.subpass = 0;
        if ((vr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe))) { FAIL("pipeline"); goto done; }
    }

    {
        VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cpci.queueFamilyIndex = qfam; cpci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        if ((vr = vkCreateCommandPool(dev, &cpci, nullptr, &cpool))) { FAIL("cmd pool"); goto done; }
        VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cbai.commandPool = cpool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
        if ((vr = vkAllocateCommandBuffers(dev, &cbai, &cmd))) { FAIL("cmd alloc"); goto done; }
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if ((vr = vkCreateFence(dev, &fci, nullptr, &fence))) { FAIL("fence"); goto done; }
    }

    // Bind the sampled texture into the descriptor set.
    {
        VkDescriptorImageInfo dii{sampler, tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = dset; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    // ── Record ──────────────────────────────────────────────────────────────────
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ((vr = vkBeginCommandBuffer(cmd, &bi))) { FAIL("begin cmd"); goto done; }

        auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to,
                           VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss2, VkPipelineStageFlags ds) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = from; b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = img; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = sa; b.dstAccessMask = da;
            vkCmdPipelineBarrier(cmd, ss2, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
        };

        // upload buffer → texture image
        barrier(tex_img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy cp{};
        cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyBufferToImage(cmd, up_buf, tex_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        barrier(tex_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        // render pass: clear + draw the fullscreen triangle
        VkClearValue clear{}; clear.color = {{0.f, 0.f, 0.f, 1.f}};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rpass; rbi.framebuffer = fbo;
        rbi.renderArea = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
        rbi.clearValueCount = 1; rbi.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pll, 0, 1, &dset, 0, nullptr);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);   // rt_img now TRANSFER_SRC_OPTIMAL (finalLayout)

        // render target → readback buffer
        VkBufferImageCopy rc{};
        rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rc.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyImageToBuffer(cmd, rt_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &rc);

        if ((vr = vkEndCommandBuffer(cmd))) { FAIL("end cmd"); goto done; }

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if ((vr = vkQueueSubmit(queue, 1, &si, fence))) { FAIL("submit"); goto done; }
        if ((vr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5'000'000'000ull))) { FAIL("fence wait"); goto done; }
    }

    // ── Verify: readback must equal the decoded texels exactly ───────────────────
    {
        void* p = nullptr;
        if ((vr = vkMapMemory(dev, rb_mem, 0, img_bytes, 0, &p))) { FAIL("map readback"); goto done; }
        const uint32_t* got = (const uint32_t*)p;
        int bad = 0, firstbad = -1, badflip = 0;
        for (int i = 0; i < W * H; i++) {
            if (got[i] != expected[i]) { if (firstbad < 0) firstbad = i; bad++; }
            int fy = H - 1 - (i / W), fx = i % W;       // flipped-Y comparison (orientation hint)
            if (got[i] != expected[fy * W + fx]) badflip++;
        }
        if (bad == 0) {
            rep("vk_quad: %dx%d textured quad rendered + read back -> EXACT match (%d texels)\n", W, H, W * H);
            rep("verdict=PIXELS-OK\n");
            result = 0;
        } else {
            rep("vk_quad: %d/%d texels differ (first @%d: got=%08x exp=%08x); flipped-Y diff=%d%s\n",
                bad, W * H, firstbad, got[firstbad], expected[firstbad], badflip,
                badflip == 0 ? " (output is Y-FLIPPED)" : "");
            rep("verdict=MISMATCH\n");
            result = bad;
        }
        vkUnmapMemory(dev, rb_mem);
    }

done:
    vkDeviceWaitIdle(dev);
    if (fence)    vkDestroyFence(dev, fence, nullptr);
    if (cpool)    vkDestroyCommandPool(dev, cpool, nullptr);   // frees cmd
    if (pipe)     vkDestroyPipeline(dev, pipe, nullptr);
    if (fbo)      vkDestroyFramebuffer(dev, fbo, nullptr);
    if (rpass)    vkDestroyRenderPass(dev, rpass, nullptr);
    if (pll)      vkDestroyPipelineLayout(dev, pll, nullptr);
    if (dpool)    vkDestroyDescriptorPool(dev, dpool, nullptr); // frees dset
    if (dsl)      vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    if (vs)       vkDestroyShaderModule(dev, vs, nullptr);
    if (fs)       vkDestroyShaderModule(dev, fs, nullptr);
    if (sampler)  vkDestroySampler(dev, sampler, nullptr);
    if (rb_buf)   vkDestroyBuffer(dev, rb_buf, nullptr);
    if (rb_mem)   vkFreeMemory(dev, rb_mem, nullptr);
    if (up_buf)   vkDestroyBuffer(dev, up_buf, nullptr);
    if (up_mem)   vkFreeMemory(dev, up_mem, nullptr);
    if (tex_view) vkDestroyImageView(dev, tex_view, nullptr);
    if (tex_img)  vkDestroyImage(dev, tex_img, nullptr);
    if (tex_mem)  vkFreeMemory(dev, tex_mem, nullptr);
    if (rt_view)  vkDestroyImageView(dev, rt_view, nullptr);
    if (rt_img)   vkDestroyImage(dev, rt_img, nullptr);
    if (rt_mem)   vkFreeMemory(dev, rt_mem, nullptr);
    (void)step;
    return result;
}

#else
int sb_vk_quad_selftest(char* out, int cap) {
    snprintf(out, cap, "vk_quad: Dolphin/Vulkan unavailable (no HAVE_DOLPHIN_MEMMAP)\n");
    return -1;
}
#endif
