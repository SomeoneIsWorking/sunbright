// nvk.cpp — standalone headless Vulkan offscreen renderer (see nvk.h). No Dolphin, no
// surface/swapchain: render to an offscreen RGBA8 image and copy it back to host RAM.
#include "nvk.h"

#include <vulkan/vulkan.h>
#include <cstring>
#include <cstdio>

// Embedded SPIR-V (generated from native/render/shaders/*.{vert,frag} by CMake via
// glslangValidator --vn; the generated dir is on the include path).
#include "tri_vert_spv.h"
#include "tri_frag_spv.h"

namespace sb::render {

#define VKCHECK(expr) do { VkResult _r = (expr); if (_r != VK_SUCCESS) { \
    std::fprintf(stderr, "[nvk] %s -> VkResult %d\n", #expr, (int)_r); return false; } } while(0)

struct Nvk::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkImage color = VK_NULL_HANDLE;
    VkDeviceMemory colorMem = VK_NULL_HANDLE;
    VkImageView colorView = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;

    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    VkBuffer readback = VK_NULL_HANDLE;
    VkDeviceMemory readbackMem = VK_NULL_HANDLE;

    VkBuffer vbuf = VK_NULL_HANDLE;
    VkDeviceMemory vbufMem = VK_NULL_HANDLE;
    VkDeviceSize vbufCap = 0;

    uint32_t w = 0, h = 0;

    int findMemType(uint32_t typeBits, VkMemoryPropertyFlags props) {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(phys, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & props) == props)
                return (int)i;
        return -1;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props, VkBuffer* buf, VkDeviceMemory* mem) {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, buf) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, *buf, &req);
        int mt = findMemType(req.memoryTypeBits, props);
        if (mt < 0) return false;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)mt;
        if (vkAllocateMemory(device, &ai, nullptr, mem) != VK_SUCCESS) return false;
        return vkBindBufferMemory(device, *buf, *mem, 0) == VK_SUCCESS;
    }
};

bool Nvk::init(uint32_t width, uint32_t height, bool preferCpu) {
    d_ = new Impl();
    Impl* d = d_;
    width_ = d->w = width;
    height_ = d->h = height;
    pixels_.assign((size_t)width * height * 4, 0);

    // --- instance ---
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "sunbright-native";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VKCHECK(vkCreateInstance(&ici, nullptr, &d->instance));

    // --- pick physical device (prefer GPU; preferCpu forces software) ---
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(d->instance, &n, nullptr);
    if (n == 0) { std::fprintf(stderr, "[nvk] no Vulkan devices\n"); return false; }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(d->instance, &n, devs.data());
    VkPhysicalDevice chosen = VK_NULL_HANDLE; int chosenScore = -1;
    for (auto pd : devs) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p);
        bool isCpu = (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU);
        int score = isCpu ? 1 : 2;
        if (preferCpu) score = isCpu ? 2 : 1;
        if (score > chosenScore) { chosenScore = score; chosen = pd; }
    }
    d->phys = chosen;
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(d->phys, &pp);
    size_t dnlen = std::strlen(pp.deviceName);
    if (dnlen >= sizeof(deviceName_)) dnlen = sizeof(deviceName_) - 1;
    std::memcpy(deviceName_, pp.deviceName, dnlen);
    deviceName_[dnlen] = '\0';

    // --- graphics queue family ---
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(d->phys, &qn, qf.data());
    bool found = false;
    for (uint32_t i = 0; i < qn; ++i)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { d->queueFamily = i; found = true; break; }
    if (!found) { std::fprintf(stderr, "[nvk] no graphics queue\n"); return false; }

    // --- device + queue ---
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = d->queueFamily; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    VKCHECK(vkCreateDevice(d->phys, &dci, nullptr, &d->device));
    vkGetDeviceQueue(d->device, d->queueFamily, 0, &d->queue);

    // --- offscreen color image (RGBA8) ---
    VkImageCreateInfo img{};
    img.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    img.imageType = VK_IMAGE_TYPE_2D;
    img.format = VK_FORMAT_R8G8B8A8_UNORM;
    img.extent = { width, height, 1 };
    img.mipLevels = 1; img.arrayLayers = 1;
    img.samples = VK_SAMPLE_COUNT_1_BIT;
    img.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(d->device, &img, nullptr, &d->color));
    VkMemoryRequirements ireq; vkGetImageMemoryRequirements(d->device, d->color, &ireq);
    int imt = d->findMemType(ireq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imt < 0) return false;
    VkMemoryAllocateInfo iai{};
    iai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    iai.allocationSize = ireq.size; iai.memoryTypeIndex = (uint32_t)imt;
    VKCHECK(vkAllocateMemory(d->device, &iai, nullptr, &d->colorMem));
    VKCHECK(vkBindImageMemory(d->device, d->color, d->colorMem, 0));

    VkImageViewCreateInfo iv{};
    iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv.image = d->color; iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format = VK_FORMAT_R8G8B8A8_UNORM;
    iv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VKCHECK(vkCreateImageView(d->device, &iv, nullptr, &d->colorView));

    // --- render pass (clear -> store, end as TRANSFER_SRC for the readback copy) ---
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8G8B8A8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 1; rp.pAttachments = &att;
    rp.subpassCount = 1; rp.pSubpasses = &sub;
    VKCHECK(vkCreateRenderPass(d->device, &rp, nullptr, &d->renderPass));

    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = d->renderPass; fb.attachmentCount = 1; fb.pAttachments = &d->colorView;
    fb.width = width; fb.height = height; fb.layers = 1;
    VKCHECK(vkCreateFramebuffer(d->device, &fb, nullptr, &d->framebuffer));

    // --- shaders ---
    auto mkShader = [&](const uint32_t* code, size_t bytes, VkShaderModule* out) {
        VkShaderModuleCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        si.codeSize = bytes; si.pCode = code;
        return vkCreateShaderModule(d->device, &si, nullptr, out) == VK_SUCCESS;
    };
    if (!mkShader(tri_vert_spv, sizeof(tri_vert_spv), &d->vs)) return false;
    if (!mkShader(tri_frag_spv, sizeof(tri_frag_spv), &d->fs)) return false;

    // --- pipeline ---
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = d->vs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = d->fs; stages[1].pName = "main";

    VkVertexInputBindingDescription bind{ 0, sizeof(NvkVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription attrs[2]{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };                       // pos
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(2 * sizeof(float)) }; // color
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 2; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{ 0, 0, (float)width, (float)height, 0.0f, 1.0f };
    VkRect2D sc{ {0, 0}, {width, height} };
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    VKCHECK(vkCreatePipelineLayout(d->device, &pl, nullptr, &d->pipeLayout));

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb;
    gp.layout = d->pipeLayout; gp.renderPass = d->renderPass; gp.subpass = 0;
    VKCHECK(vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &gp, nullptr, &d->pipeline));

    // --- command pool / buffer / fence ---
    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = d->queueFamily;
    VKCHECK(vkCreateCommandPool(d->device, &cp, nullptr, &d->cmdPool));
    VkCommandBufferAllocateInfo ca{};
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = d->cmdPool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(d->device, &ca, &d->cmd));
    VkFenceCreateInfo fi{}; fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VKCHECK(vkCreateFence(d->device, &fi, nullptr, &d->fence));

    // --- readback buffer (host-visible) ---
    if (!d->createBuffer((VkDeviceSize)width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &d->readback, &d->readbackMem))
        return false;
    return true;
}

bool Nvk::renderTriangles(const std::vector<NvkVertex>& verts, NvkClear clear) {
    Impl* d = d_;
    if (!d || !d->device) return false;
    const uint32_t vcount = (uint32_t)verts.size();

    // --- (re)create the vertex buffer if needed ---
    VkDeviceSize need = vcount ? vcount * sizeof(NvkVertex) : sizeof(NvkVertex);
    if (need > d->vbufCap) {
        if (d->vbuf) { vkDestroyBuffer(d->device, d->vbuf, nullptr); vkFreeMemory(d->device, d->vbufMem, nullptr); }
        if (!d->createBuffer(need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          &d->vbuf, &d->vbufMem))
            return false;
        d->vbufCap = need;
    }
    if (vcount) {
        void* p = nullptr;
        VKCHECK(vkMapMemory(d->device, d->vbufMem, 0, need, 0, &p));
        std::memcpy(p, verts.data(), vcount * sizeof(NvkVertex));
        vkUnmapMemory(d->device, d->vbufMem);
    }

    // --- record ---
    VKCHECK(vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(d->cmd, &bi));

    VkClearValue cv{}; cv.color = { { clear.r, clear.g, clear.b, clear.a } };
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = d->renderPass; rpb.framebuffer = d->framebuffer;
    rpb.renderArea = { {0, 0}, {d->w, d->h} };
    rpb.clearValueCount = 1; rpb.pClearValues = &cv;
    vkCmdBeginRenderPass(d->cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    if (vcount) {
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->pipeline);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(d->cmd, 0, 1, &d->vbuf, &off);
        vkCmdDraw(d->cmd, vcount, 1, 0, 0);
    }
    vkCmdEndRenderPass(d->cmd);  // image now in TRANSFER_SRC_OPTIMAL

    // --- copy image -> readback buffer ---
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { d->w, d->h, 1 };
    vkCmdCopyImageToBuffer(d->cmd, d->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           d->readback, 1, &region);
    VKCHECK(vkEndCommandBuffer(d->cmd));

    // --- submit + wait ---
    VKCHECK(vkResetFences(d->device, 1, &d->fence));
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &d->cmd;
    VKCHECK(vkQueueSubmit(d->queue, 1, &si, d->fence));
    VKCHECK(vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_MAX));

    // --- read pixels back ---
    void* mapped = nullptr;
    VKCHECK(vkMapMemory(d->device, d->readbackMem, 0, (VkDeviceSize)d->w * d->h * 4, 0, &mapped));
    std::memcpy(pixels_.data(), mapped, (size_t)d->w * d->h * 4);
    vkUnmapMemory(d->device, d->readbackMem);
    return true;
}

void Nvk::shutdown() {
    if (!d_) return;
    Impl* d = d_;
    if (d->device) {
        vkDeviceWaitIdle(d->device);
        if (d->vbuf) vkDestroyBuffer(d->device, d->vbuf, nullptr);
        if (d->vbufMem) vkFreeMemory(d->device, d->vbufMem, nullptr);
        if (d->readback) vkDestroyBuffer(d->device, d->readback, nullptr);
        if (d->readbackMem) vkFreeMemory(d->device, d->readbackMem, nullptr);
        if (d->fence) vkDestroyFence(d->device, d->fence, nullptr);
        if (d->cmdPool) vkDestroyCommandPool(d->device, d->cmdPool, nullptr);
        if (d->pipeline) vkDestroyPipeline(d->device, d->pipeline, nullptr);
        if (d->pipeLayout) vkDestroyPipelineLayout(d->device, d->pipeLayout, nullptr);
        if (d->vs) vkDestroyShaderModule(d->device, d->vs, nullptr);
        if (d->fs) vkDestroyShaderModule(d->device, d->fs, nullptr);
        if (d->framebuffer) vkDestroyFramebuffer(d->device, d->framebuffer, nullptr);
        if (d->renderPass) vkDestroyRenderPass(d->device, d->renderPass, nullptr);
        if (d->colorView) vkDestroyImageView(d->device, d->colorView, nullptr);
        if (d->color) vkDestroyImage(d->device, d->color, nullptr);
        if (d->colorMem) vkFreeMemory(d->device, d->colorMem, nullptr);
        vkDestroyDevice(d->device, nullptr);
    }
    if (d->instance) vkDestroyInstance(d->instance, nullptr);
    delete d_;
    d_ = nullptr;
}

} // namespace sb::render
