// vk_mesh — N4 (docs/native_port_plan.md §3): the FIRST native 3D WORLD PIXELS.
// Our own Vulkan pipeline rasterizes the game's real J3D geometry — captured,
// extracted and transformed natively by ngx (model→eye→clip, ngx_j3d_shape.cpp)
// — into an offscreen color target, with ZERO Dolphin VideoCommon. Flat-shaded
// with the extracted vertex color0 (materials/TEV are N5). Verifiable headless:
// render → read back → report pixel coverage + dump a PPM.
//
// Like vk_quad (N2) this is an on-demand self-test (/ngxrender) reusing Dolphin's
// VkDevice as bring-up scaffold (sanctioned until Dolphin video is unlinked); the
// production renderer integrates with the frame cycle properly. The geometry is a
// best-effort snapshot of recent scene triangles published by the J3DShape hook.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

// Clip-space triangle snapshot from the J3DShape hook (8 floats/vertex:
// clip.xyzw + rgba0). Defined in runtime/overrides/ngx_j3d_shape.cpp.
const float* ngx_mesh_snapshot(int* nverts);

#ifdef HAVE_DOLPHIN_MEMMAP
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "render/shaders/mesh_vert_spv.h"
#include "render/shaders/mesh_frag_spv.h"

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

// Render the captured native geometry. Returns covered-pixel count (>=0), -1 if
// Vulkan/device unavailable or no geometry captured, -2 on a Vulkan failure.
int sb_ngx_render(char* outbuf, int cap) {
    Report rep{outbuf, cap};

    if (!Vulkan::g_vulkan_context) { rep("ngx_render: no g_vulkan_context (set SUNBRIGHT_NGX_SHAPE=1 + be in a 3D scene)\n"); return -1; }
    const VkDevice dev = Vulkan::g_vulkan_context->GetDevice();
    const VkPhysicalDevice phys = Vulkan::g_vulkan_context->GetPhysicalDevice();
    const VkQueue queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
    const uint32_t qfam = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();
    if (dev == VK_NULL_HANDLE) { rep("ngx_render: null device\n"); return -1; }

    // Snapshot the geometry (copy promptly — the emu thread keeps writing).
    int nv = 0;
    const float* src = ngx_mesh_snapshot(&nv);
    if (!src || nv < 3) { rep("ngx_render: no geometry captured (nverts=%d)\n", nv); return -1; }
    std::vector<float> verts(src, src + (size_t)nv * 8);
    const VkDeviceSize vbytes = (VkDeviceSize)nv * 8 * sizeof(float);

    const int W = 640, H = 448;
    const VkDeviceSize img_bytes = (VkDeviceSize)W * H * 4;

    VkImage rt_img = VK_NULL_HANDLE; VkDeviceMemory rt_mem = VK_NULL_HANDLE; VkImageView rt_view = VK_NULL_HANDLE;
    VkImage ds_img = VK_NULL_HANDLE; VkDeviceMemory ds_mem = VK_NULL_HANDLE; VkImageView ds_view = VK_NULL_HANDLE;
    VkBuffer vbuf = VK_NULL_HANDLE, rb_buf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE, rb_mem = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout pll = VK_NULL_HANDLE;
    VkRenderPass rpass = VK_NULL_HANDLE;
    VkFramebuffer fbo = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;

    int result = -2;
    VkResult vr = VK_SUCCESS;
    auto FAIL = [&](const char* s) { rep("ngx_render: FAIL at %s (VkResult=%d)\n", s, (int)vr); };

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
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {(uint32_t)W, (uint32_t)H, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((vr = vkCreateImage(dev, &ici, nullptr, &rt_img))) { FAIL("rt image"); goto done; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, rt_img, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if ((vr = vkAllocateMemory(dev, &ai, nullptr, &rt_mem))) { FAIL("rt mem"); goto done; }
        if ((vr = vkBindImageMemory(dev, rt_img, rt_mem, 0))) { FAIL("rt bind"); goto done; }
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = rt_img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if ((vr = vkCreateImageView(dev, &vci, nullptr, &rt_view))) { FAIL("rt view"); goto done; }
    }
    {   // depth attachment (D32) — occlusion sort, GC z mapped to [0,1] in the vert shader
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = VK_FORMAT_D32_SFLOAT;
        ici.extent = {(uint32_t)W, (uint32_t)H, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((vr = vkCreateImage(dev, &ici, nullptr, &ds_img))) { FAIL("ds image"); goto done; }
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, ds_img, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if ((vr = vkAllocateMemory(dev, &ai, nullptr, &ds_mem))) { FAIL("ds mem"); goto done; }
        if ((vr = vkBindImageMemory(dev, ds_img, ds_mem, 0))) { FAIL("ds bind"); goto done; }
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = ds_img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = VK_FORMAT_D32_SFLOAT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if ((vr = vkCreateImageView(dev, &vci, nullptr, &ds_view))) { FAIL("ds view"); goto done; }
    }
    if (!make_buffer(vbuf, vmem, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) { FAIL("vertex buf"); goto done; }
    if (!make_buffer(rb_buf, rb_mem, img_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) { FAIL("readback buf"); goto done; }
    {
        void* p = nullptr;
        if ((vr = vkMapMemory(dev, vmem, 0, vbytes, 0, &p))) { FAIL("map vbuf"); goto done; }
        memcpy(p, verts.data(), vbytes);
        vkUnmapMemory(dev, vmem);
    }
    if (!make_shader(vs, kMeshVertSpv, sizeof kMeshVertSpv)) { FAIL("vert shader"); goto done; }
    if (!make_shader(fs, kMeshFragSpv, sizeof kMeshFragSpv)) { FAIL("frag shader"); goto done; }

    {
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        if ((vr = vkCreatePipelineLayout(dev, &plci, nullptr, &pll))) { FAIL("pipeline layout"); goto done; }

        VkAttachmentDescription at[2] = {};
        at[0].format = VK_FORMAT_R8G8B8A8_UNORM; at[0].samples = VK_SAMPLE_COUNT_1_BIT;
        at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        at[1].format = VK_FORMAT_D32_SFLOAT; at[1].samples = VK_SAMPLE_COUNT_1_BIT;
        at[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference ar{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference dr{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sd{}; sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sd.colorAttachmentCount = 1; sd.pColorAttachments = &ar; sd.pDepthStencilAttachment = &dr;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 2; rpci.pAttachments = at; rpci.subpassCount = 1; rpci.pSubpasses = &sd;
        if ((vr = vkCreateRenderPass(dev, &rpci, nullptr, &rpass))) { FAIL("render pass"); goto done; }

        VkImageView fbatt[2] = {rt_view, ds_view};
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = rpass; fci.attachmentCount = 2; fci.pAttachments = fbatt;
        fci.width = W; fci.height = H; fci.layers = 1;
        if ((vr = vkCreateFramebuffer(dev, &fci, nullptr, &fbo))) { FAIL("framebuffer"); goto done; }
    }

    {
        VkPipelineShaderStageCreateInfo ss[2] = {};
        ss[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT; ss[0].module = vs; ss[0].pName = "main";
        ss[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].module = fs; ss[1].pName = "main";

        VkVertexInputBindingDescription vib{0, 8 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription via[2] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},                 // clip.xyzw
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)}, // rgba0
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vib;
        vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = via;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport vp{0, 0, (float)W, (float)H, 0, 1};
        VkRect2D scr{{0, 0}, {(uint32_t)W, (uint32_t)H}};
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scr;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;   // near (z=0) occludes far (z=1)
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = ss; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &dss;
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

    // ── Record ──────────────────────────────────────────────────────────────────
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ((vr = vkBeginCommandBuffer(cmd, &bi))) { FAIL("begin cmd"); goto done; }

        VkClearValue clear[2]{};
        clear[0].color = {{0.10f, 0.12f, 0.18f, 1.f}};   // dark slate background
        clear[1].depthStencil = {1.0f, 0};               // far
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rpass; rbi.framebuffer = fbo;
        rbi.renderArea = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
        rbi.clearValueCount = 2; rbi.pClearValues = clear;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        VkDeviceSize voff = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &voff);
        vkCmdDraw(cmd, (uint32_t)nv, 1, 0, 0);
        vkCmdEndRenderPass(cmd);   // rt_img now TRANSFER_SRC_OPTIMAL

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

    // ── Coverage + PPM dump ───────────────────────────────────────────────────────
    {
        void* p = nullptr;
        if ((vr = vkMapMemory(dev, rb_mem, 0, img_bytes, 0, &p))) { FAIL("map readback"); goto done; }
        const uint8_t* px = (const uint8_t*)p;
        const uint8_t bg[3] = {(uint8_t)(0.10f*255+0.5f), (uint8_t)(0.12f*255+0.5f), (uint8_t)(0.18f*255+0.5f)};
        long covered = 0;
        for (int i = 0; i < W * H; i++) {
            const uint8_t* q = px + (size_t)i * 4;
            if (q[0] != bg[0] || q[1] != bg[1] || q[2] != bg[2]) covered++;
        }
        // PPM (P6 RGB) for eyeballing.
        const char* ppm = "scratch/screenshots/ngx_mesh.ppm";
        if (FILE* f = fopen(ppm, "wb")) {
            fprintf(f, "P6\n%d %d\n255\n", W, H);
            std::vector<uint8_t> row((size_t)W * 3);
            for (int y = 0; y < H; y++) {
                for (int x = 0; x < W; x++) {
                    const uint8_t* q = px + ((size_t)y * W + x) * 4;
                    row[x*3+0]=q[0]; row[x*3+1]=q[1]; row[x*3+2]=q[2];
                }
                fwrite(row.data(), 1, row.size(), f);
            }
            fclose(f);
        }
        const double pct = 100.0 * (double)covered / (double)(W * H);
        rep("ngx_render: drew %d verts (%d tris) -> %ldx%d, covered=%ld px (%.1f%%)\n",
            nv, nv / 3, (long)W, H, covered, pct);
        rep("  PPM: %s\n  verdict=%s\n", ppm, covered > 0 ? "PIXELS-OK" : "EMPTY");
        result = (int)covered;
        vkUnmapMemory(dev, rb_mem);
    }

done:
    vkDeviceWaitIdle(dev);
    if (fence)  vkDestroyFence(dev, fence, nullptr);
    if (cpool)  vkDestroyCommandPool(dev, cpool, nullptr);
    if (pipe)   vkDestroyPipeline(dev, pipe, nullptr);
    if (fbo)    vkDestroyFramebuffer(dev, fbo, nullptr);
    if (rpass)  vkDestroyRenderPass(dev, rpass, nullptr);
    if (pll)    vkDestroyPipelineLayout(dev, pll, nullptr);
    if (vs)     vkDestroyShaderModule(dev, vs, nullptr);
    if (fs)     vkDestroyShaderModule(dev, fs, nullptr);
    if (rb_buf) vkDestroyBuffer(dev, rb_buf, nullptr);
    if (rb_mem) vkFreeMemory(dev, rb_mem, nullptr);
    if (vbuf)   vkDestroyBuffer(dev, vbuf, nullptr);
    if (vmem)   vkFreeMemory(dev, vmem, nullptr);
    if (rt_view) vkDestroyImageView(dev, rt_view, nullptr);
    if (rt_img)  vkDestroyImage(dev, rt_img, nullptr);
    if (rt_mem)  vkFreeMemory(dev, rt_mem, nullptr);
    if (ds_view) vkDestroyImageView(dev, ds_view, nullptr);
    if (ds_img)  vkDestroyImage(dev, ds_img, nullptr);
    if (ds_mem)  vkFreeMemory(dev, ds_mem, nullptr);
    return result;
}

#else
int sb_ngx_render(char* out, int cap) {
    snprintf(out, cap, "ngx_render: Dolphin/Vulkan unavailable (no HAVE_DOLPHIN_MEMMAP)\n");
    return -1;
}
#endif
