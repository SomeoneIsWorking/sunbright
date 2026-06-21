// nvk.cpp — standalone headless Vulkan offscreen renderer (see nvk.h). No Dolphin, no
// surface/swapchain: render to an offscreen RGBA8 image and copy it back to host RAM.
#include "nvk.h"

#include <vulkan/vulkan.h>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <string>

#include "glsl_compile.h"   // runtime GLSL 450 -> SPIR-V (shipping ngx wrapper, glslang)

// Embedded SPIR-V (generated from native/render/shaders/*.{vert,frag} by CMake via
// glslangValidator --vn; the generated dir is on the include path).
#include "tri_vert_spv.h"
#include "tri_frag_spv.h"
#include "tex_vert_spv.h"
#include "tex_frag_spv.h"
#include "tev_vert_spv.h"

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
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory depthMem = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
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

    // textured draw path
    VkShaderModule texVs = VK_NULL_HANDLE, texFs = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkPipelineLayout texPipeLayout = VK_NULL_HANDLE;
    VkPipeline texPipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImage tex = VK_NULL_HANDLE;
    VkDeviceMemory texMem = VK_NULL_HANDLE;
    VkImageView texView = VK_NULL_HANDLE;
    VkBuffer texVbuf = VK_NULL_HANDLE;
    VkDeviceMemory texVbufMem = VK_NULL_HANDLE;
    VkDeviceSize texVbufCap = 0;

    // TEV combiner draw path: a generated (runtime-compiled) fragment shader, an
    // 8-element sampler array (one per GX texmap), and push constants (kcolor/tevreg).
    VkShaderModule tevVs = VK_NULL_HANDLE, tevFs = VK_NULL_HANDLE;
    VkDescriptorSetLayout tevDsl = VK_NULL_HANDLE;
    VkDescriptorPool tevDpool = VK_NULL_HANDLE;
    VkDescriptorSet tevDset = VK_NULL_HANDLE;
    VkPipelineLayout tevPipeLayout = VK_NULL_HANDLE;
    VkPipeline tevPipeline = VK_NULL_HANDLE;
    VkSampler tevSampler = VK_NULL_HANDLE;
    VkImage tevTex[8] = {}; VkDeviceMemory tevTexMem[8] = {}; VkImageView tevTexView[8] = {};
    VkImage tevWhite = VK_NULL_HANDLE; VkDeviceMemory tevWhiteMem = VK_NULL_HANDLE;
    VkImageView tevWhiteView = VK_NULL_HANDLE;
    VkBuffer tevVbuf = VK_NULL_HANDLE; VkDeviceMemory tevVbufMem = VK_NULL_HANDLE;
    VkDeviceSize tevVbufCap = 0;

    uint32_t w = 0, h = 0;

    // Create + upload an RGBA8 image (row-major, tw*th*4 bytes), returning its handles.
    // Leaves the image in SHADER_READ_ONLY_OPTIMAL. Used by the TEV texmap path.
    bool makeTexture(const uint8_t* rgba, uint32_t tw, uint32_t th,
                     VkImage* outImg, VkDeviceMemory* outMem, VkImageView* outView) {
        VkImageCreateInfo ic{};
        ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent = { tw, th, 1 }; ic.mipLevels = 1; ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ic, nullptr, outImg) != VK_SUCCESS) return false;
        VkMemoryRequirements req; vkGetImageMemoryRequirements(device, *outImg, &req);
        int mt = findMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt < 0) return false;
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)mt;
        if (vkAllocateMemory(device, &ai, nullptr, outMem) != VK_SUCCESS) return false;
        if (vkBindImageMemory(device, *outImg, *outMem, 0) != VK_SUCCESS) return false;

        VkBuffer stage; VkDeviceMemory stageMem;
        VkDeviceSize bytes = (VkDeviceSize)tw * th * 4;
        if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &stage, &stageMem)) return false;
        void* p = nullptr;
        if (vkMapMemory(device, stageMem, 0, bytes, 0, &p) != VK_SUCCESS) return false;
        std::memcpy(p, rgba, bytes);
        vkUnmapMemory(device, stageMem);

        if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) return false;
        VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) return false;
        auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                           VkAccessFlags da, VkPipelineStageFlags ss, VkPipelineStageFlags dd) {
            VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = from; b.newLayout = to; b.image = *outImg;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            b.srcAccessMask = sa; b.dstAccessMask = da;
            vkCmdPipelineBarrier(cmd, ss, dd, 0, 0, nullptr, 0, nullptr, 1, &b);
        };
        barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy bc{};
        bc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        bc.imageExtent = { tw, th, 1 };
        vkCmdCopyBufferToImage(cmd, stage, *outImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) return false;
        vkResetFences(device, 1, &fence);
        VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) return false;
        vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyBuffer(device, stage, nullptr);
        vkFreeMemory(device, stageMem, nullptr);

        VkImageViewCreateInfo iv{};
        iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image = *outImg; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = VK_FORMAT_R8G8B8A8_UNORM;
        iv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return vkCreateImageView(device, &iv, nullptr, outView) == VK_SUCCESS;
    }

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

    // --- depth image (D32_SFLOAT) for 3D depth testing ---
    VkImageCreateInfo dimg = img;
    dimg.format = VK_FORMAT_D32_SFLOAT;
    dimg.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VKCHECK(vkCreateImage(d->device, &dimg, nullptr, &d->depth));
    VkMemoryRequirements dreq; vkGetImageMemoryRequirements(d->device, d->depth, &dreq);
    int dmt = d->findMemType(dreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (dmt < 0) return false;
    VkMemoryAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    dai.allocationSize = dreq.size; dai.memoryTypeIndex = (uint32_t)dmt;
    VKCHECK(vkAllocateMemory(d->device, &dai, nullptr, &d->depthMem));
    VKCHECK(vkBindImageMemory(d->device, d->depth, d->depthMem, 0));
    VkImageViewCreateInfo div{};
    div.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    div.image = d->depth; div.viewType = VK_IMAGE_VIEW_TYPE_2D;
    div.format = VK_FORMAT_D32_SFLOAT;
    div.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
    VKCHECK(vkCreateImageView(d->device, &div, nullptr, &d->depthView));

    // --- render pass: color (clear->store, end TRANSFER_SRC) + depth (clear) ---
    VkAttachmentDescription att[2]{};
    att[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    att[1].format = VK_FORMAT_D32_SFLOAT;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference ref{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref{ 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    sub.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rp{};
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 2; rp.pAttachments = att;
    rp.subpassCount = 1; rp.pSubpasses = &sub;
    VKCHECK(vkCreateRenderPass(d->device, &rp, nullptr, &d->renderPass));

    VkImageView fbViews[2] = { d->colorView, d->depthView };
    VkFramebufferCreateInfo fb{};
    fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb.renderPass = d->renderPass; fb.attachmentCount = 2; fb.pAttachments = fbViews;
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
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };                    // pos xyz
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)(3 * sizeof(float)) }; // color
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

    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;  // smaller z = nearer

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    // Standard alpha-over blend (GX_BM_BLEND SRCALPHA / INVSRCALPHA) — what the 2D
    // fader / J2D overlays use. A no-op for opaque (a=1) geometry, so the 3D draw
    // path and opaque tests are unaffected; it lets a partial-alpha overlay composite.
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
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
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDepthStencilState = &ds;
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

    // --- textured pipeline (combined image sampler at binding 0) ---
    if (!mkShader(tex_vert_spv, sizeof(tex_vert_spv), &d->texVs)) return false;
    if (!mkShader(tex_frag_spv, sizeof(tex_frag_spv), &d->texFs)) return false;

    VkSamplerCreateInfo smp{};
    smp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    smp.magFilter = VK_FILTER_NEAREST; smp.minFilter = VK_FILTER_NEAREST;
    smp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VKCHECK(vkCreateSampler(d->device, &smp, nullptr, &d->sampler));

    VkDescriptorSetLayoutBinding dslb{};
    dslb.binding = 0; dslb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dslb.descriptorCount = 1; dslb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dslc{};
    dslc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslc.bindingCount = 1; dslc.pBindings = &dslb;
    VKCHECK(vkCreateDescriptorSetLayout(d->device, &dslc, nullptr, &d->dsl));

    VkDescriptorPoolSize dps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpc{};
    dpc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpc.maxSets = 1; dpc.poolSizeCount = 1; dpc.pPoolSizes = &dps;
    VKCHECK(vkCreateDescriptorPool(d->device, &dpc, nullptr, &d->dpool));
    VkDescriptorSetAllocateInfo dsa{};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = d->dpool; dsa.descriptorSetCount = 1; dsa.pSetLayouts = &d->dsl;
    VKCHECK(vkAllocateDescriptorSets(d->device, &dsa, &d->dset));

    VkPipelineShaderStageCreateInfo tstages[2]{};
    tstages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    tstages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; tstages[0].module = d->texVs; tstages[0].pName = "main";
    tstages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    tstages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; tstages[1].module = d->texFs; tstages[1].pName = "main";

    VkVertexInputBindingDescription tbind{ 0, sizeof(NvkTexVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription tattrs[2]{};
    tattrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };                       // pos xyz
    tattrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)(3 * sizeof(float)) }; // uv
    VkPipelineVertexInputStateCreateInfo tvi{};
    tvi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    tvi.vertexBindingDescriptionCount = 1; tvi.pVertexBindingDescriptions = &tbind;
    tvi.vertexAttributeDescriptionCount = 2; tvi.pVertexAttributeDescriptions = tattrs;

    VkPipelineLayoutCreateInfo tpl{};
    tpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tpl.setLayoutCount = 1; tpl.pSetLayouts = &d->dsl;
    VKCHECK(vkCreatePipelineLayout(d->device, &tpl, nullptr, &d->texPipeLayout));

    VkGraphicsPipelineCreateInfo tgp = gp;   // reuse ia/vps/rs/ms/cb/ds from above
    tgp.pStages = tstages; tgp.pVertexInputState = &tvi; tgp.layout = d->texPipeLayout;
    VKCHECK(vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &tgp, nullptr, &d->texPipeline));

    // --- TEV pipeline scaffolding (vertex module, sampler, 8-texmap descriptor, layout) ---
    // The fragment shader is generated per-material at runtime → the pipeline itself is
    // built in setTevFragment; here we set up everything that doesn't depend on it.
    if (!mkShader(tev_vert_spv, sizeof(tev_vert_spv), &d->tevVs)) return false;

    VkSamplerCreateInfo tsmp{};
    tsmp.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    tsmp.magFilter = VK_FILTER_NEAREST; tsmp.minFilter = VK_FILTER_NEAREST;
    tsmp.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    tsmp.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    tsmp.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VKCHECK(vkCreateSampler(d->device, &tsmp, nullptr, &d->tevSampler));

    // 1x1 white default for unbound texmaps (all 8 descriptor elements must be valid).
    const uint8_t white[4] = { 255, 255, 255, 255 };
    if (!d->makeTexture(white, 1, 1, &d->tevWhite, &d->tevWhiteMem, &d->tevWhiteView)) return false;
    for (int i = 0; i < 8; ++i) d->tevTexView[i] = d->tevWhiteView;

    VkDescriptorSetLayoutBinding tdslb{};
    tdslb.binding = 0; tdslb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tdslb.descriptorCount = 8; tdslb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo tdslc{};
    tdslc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tdslc.bindingCount = 1; tdslc.pBindings = &tdslb;
    VKCHECK(vkCreateDescriptorSetLayout(d->device, &tdslc, nullptr, &d->tevDsl));

    VkDescriptorPoolSize tdps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 };
    VkDescriptorPoolCreateInfo tdpc{};
    tdpc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    tdpc.maxSets = 1; tdpc.poolSizeCount = 1; tdpc.pPoolSizes = &tdps;
    VKCHECK(vkCreateDescriptorPool(d->device, &tdpc, nullptr, &d->tevDpool));
    VkDescriptorSetAllocateInfo tdsa{};
    tdsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    tdsa.descriptorPool = d->tevDpool; tdsa.descriptorSetCount = 1; tdsa.pSetLayouts = &d->tevDsl;
    VKCHECK(vkAllocateDescriptorSets(d->device, &tdsa, &d->tevDset));

    // Write all 8 array elements to the white default (overridden by setTevTexture).
    VkDescriptorImageInfo tdii[8];
    for (int i = 0; i < 8; ++i)
        tdii[i] = { d->tevSampler, d->tevWhiteView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet tw{}; tw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    tw.dstSet = d->tevDset; tw.dstBinding = 0; tw.dstArrayElement = 0; tw.descriptorCount = 8;
    tw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; tw.pImageInfo = tdii;
    vkUpdateDescriptorSets(d->device, 1, &tw, 0, nullptr);

    VkPushConstantRange tpcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(NvkTevPush) };
    VkPipelineLayoutCreateInfo tevpl{};
    tevpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tevpl.setLayoutCount = 1; tevpl.pSetLayouts = &d->tevDsl;
    tevpl.pushConstantRangeCount = 1; tevpl.pPushConstantRanges = &tpcr;
    VKCHECK(vkCreatePipelineLayout(d->device, &tevpl, nullptr, &d->tevPipeLayout));

    return true;
}

// Rebuild the TEV graphics pipeline from a runtime-compiled fragment shader. The
// pipeline state (vertex layout, depth, blend, viewport) mirrors the other paths.
bool Nvk::setTevFragment(const std::string& glslFragment) {
    Impl* d = d_;
    if (!d || !d->device) return false;
    std::vector<uint32_t> spv = sb_compile_fragment_glsl(glslFragment);
    if (spv.empty()) { std::fprintf(stderr, "[nvk] TEV fragment compile failed\n"); return false; }

    vkDeviceWaitIdle(d->device);
    if (d->tevPipeline) { vkDestroyPipeline(d->device, d->tevPipeline, nullptr); d->tevPipeline = VK_NULL_HANDLE; }
    if (d->tevFs) { vkDestroyShaderModule(d->device, d->tevFs, nullptr); d->tevFs = VK_NULL_HANDLE; }

    VkShaderModuleCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    si.codeSize = spv.size() * sizeof(uint32_t); si.pCode = spv.data();
    VKCHECK(vkCreateShaderModule(d->device, &si, nullptr, &d->tevFs));

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = d->tevVs; stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = d->tevFs; stages[1].pName = "main";

    // Vertex input: pos, color0, color1, then the 8 UVs packed as 4 vec4 attributes.
    VkVertexInputBindingDescription bind{ 0, sizeof(NvkTevVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription a[7]{};
    a[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    (uint32_t)offsetof(NvkTevVertex, x) };
    a[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(NvkTevVertex, rgba) };
    a[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(NvkTevVertex, rgba1) };
    a[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(NvkTevVertex, uv[0]) };
    a[4] = { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(NvkTevVertex, uv[2]) };
    a[5] = { 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(NvkTevVertex, uv[4]) };
    a[6] = { 6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(NvkTevVertex, uv[6]) };
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 7; vi.pVertexAttributeDescriptions = a;

    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{ 0, 0, (float)d->w, (float)d->h, 0.0f, 1.0f };
    VkRect2D sc{ {0, 0}, {d->w, d->h} };
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
    VkPipelineDepthStencilStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cba.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vps; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState = &cb; gp.pDepthStencilState = &ds;
    gp.layout = d->tevPipeLayout; gp.renderPass = d->renderPass; gp.subpass = 0;
    VKCHECK(vkCreateGraphicsPipelines(d->device, VK_NULL_HANDLE, 1, &gp, nullptr, &d->tevPipeline));
    return true;
}

bool Nvk::setTevTexture(int slot, const uint8_t* rgba, uint32_t tw, uint32_t th) {
    Impl* d = d_;
    if (!d || !d->device || slot < 0 || slot >= 8) return false;
    if (d->tevTex[slot]) {
        vkDeviceWaitIdle(d->device);
        vkDestroyImageView(d->device, d->tevTexView[slot], nullptr);
        vkDestroyImage(d->device, d->tevTex[slot], nullptr);
        vkFreeMemory(d->device, d->tevTexMem[slot], nullptr);
        d->tevTex[slot] = VK_NULL_HANDLE;
    }
    if (!d->makeTexture(rgba, tw, th, &d->tevTex[slot], &d->tevTexMem[slot], &d->tevTexView[slot]))
        return false;
    VkDescriptorImageInfo dii{ d->tevSampler, d->tevTexView[slot], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = d->tevDset; w.dstBinding = 0; w.dstArrayElement = (uint32_t)slot;
    w.descriptorCount = 1; w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(d->device, 1, &w, 0, nullptr);
    return true;
}

bool Nvk::renderTevTriangles(const std::vector<NvkTevVertex>& verts,
                             const NvkTevPush& push, NvkClear clear) {
    Impl* d = d_;
    if (!d || !d->device || !d->tevPipeline) return false;
    const uint32_t vcount = (uint32_t)verts.size();
    VkDeviceSize need = vcount ? vcount * sizeof(NvkTevVertex) : sizeof(NvkTevVertex);
    if (need > d->tevVbufCap) {
        if (d->tevVbuf) { vkDestroyBuffer(d->device, d->tevVbuf, nullptr); vkFreeMemory(d->device, d->tevVbufMem, nullptr); }
        if (!d->createBuffer(need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &d->tevVbuf, &d->tevVbufMem)) return false;
        d->tevVbufCap = need;
    }
    if (vcount) {
        void* p = nullptr;
        VKCHECK(vkMapMemory(d->device, d->tevVbufMem, 0, need, 0, &p));
        std::memcpy(p, verts.data(), vcount * sizeof(NvkTevVertex));
        vkUnmapMemory(d->device, d->tevVbufMem);
    }
    VKCHECK(vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(d->cmd, &bi));
    VkClearValue cv[2]{};
    cv[0].color = { { clear.r, clear.g, clear.b, clear.a } };
    cv[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = d->renderPass; rpb.framebuffer = d->framebuffer;
    rpb.renderArea = { {0, 0}, {d->w, d->h} };
    rpb.clearValueCount = 2; rpb.pClearValues = cv;
    vkCmdBeginRenderPass(d->cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    if (vcount) {
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->tevPipeline);
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->tevPipeLayout,
                                0, 1, &d->tevDset, 0, nullptr);
        vkCmdPushConstants(d->cmd, d->tevPipeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, (uint32_t)sizeof(NvkTevPush), &push);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(d->cmd, 0, 1, &d->tevVbuf, &off);
        vkCmdDraw(d->cmd, vcount, 1, 0, 0);
    }
    vkCmdEndRenderPass(d->cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { d->w, d->h, 1 };
    vkCmdCopyImageToBuffer(d->cmd, d->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           d->readback, 1, &region);
    VKCHECK(vkEndCommandBuffer(d->cmd));
    VKCHECK(vkResetFences(d->device, 1, &d->fence));
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &d->cmd;
    VKCHECK(vkQueueSubmit(d->queue, 1, &si, d->fence));
    VKCHECK(vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_MAX));
    void* mapped = nullptr;
    VKCHECK(vkMapMemory(d->device, d->readbackMem, 0, (VkDeviceSize)d->w * d->h * 4, 0, &mapped));
    std::memcpy(pixels_.data(), mapped, (size_t)d->w * d->h * 4);
    vkUnmapMemory(d->device, d->readbackMem);
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

    VkClearValue cv[2]{};
    cv[0].color = { { clear.r, clear.g, clear.b, clear.a } };
    cv[1].depthStencil = { 1.0f, 0 };   // far plane
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = d->renderPass; rpb.framebuffer = d->framebuffer;
    rpb.renderArea = { {0, 0}, {d->w, d->h} };
    rpb.clearValueCount = 2; rpb.pClearValues = cv;
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

bool Nvk::setTexture(const uint8_t* rgba, uint32_t tw, uint32_t th) {
    Impl* d = d_;
    if (!d || !d->device) return false;
    if (d->tex) { vkDestroyImageView(d->device, d->texView, nullptr);
                  vkDestroyImage(d->device, d->tex, nullptr);
                  vkFreeMemory(d->device, d->texMem, nullptr);
                  d->tex = VK_NULL_HANDLE; }

    VkImageCreateInfo ic{};
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = VK_FORMAT_R8G8B8A8_UNORM;
    ic.extent = { tw, th, 1 }; ic.mipLevels = 1; ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(d->device, &ic, nullptr, &d->tex));
    VkMemoryRequirements req; vkGetImageMemoryRequirements(d->device, d->tex, &req);
    int mt = d->findMemType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) return false;
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size; ai.memoryTypeIndex = (uint32_t)mt;
    VKCHECK(vkAllocateMemory(d->device, &ai, nullptr, &d->texMem));
    VKCHECK(vkBindImageMemory(d->device, d->tex, d->texMem, 0));

    // staging buffer with the texel data
    VkBuffer stage; VkDeviceMemory stageMem;
    VkDeviceSize bytes = (VkDeviceSize)tw * th * 4;
    if (!d->createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            &stage, &stageMem)) return false;
    void* p = nullptr;
    VKCHECK(vkMapMemory(d->device, stageMem, 0, bytes, 0, &p));
    std::memcpy(p, rgba, bytes);
    vkUnmapMemory(d->device, stageMem);

    // upload: UNDEFINED -> TRANSFER_DST -> copy -> SHADER_READ_ONLY
    VKCHECK(vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(d->cmd, &bi));
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
                       VkPipelineStageFlags ss, VkPipelineStageFlags ds_) {
        VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = from; b.newLayout = to; b.image = d->tex;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.srcAccessMask = sa; b.dstAccessMask = da;
        vkCmdPipelineBarrier(d->cmd, ss, ds_, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy cp{};
    cp.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cp.imageExtent = { tw, th, 1 };
    vkCmdCopyBufferToImage(d->cmd, stage, d->tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VKCHECK(vkEndCommandBuffer(d->cmd));
    VKCHECK(vkResetFences(d->device, 1, &d->fence));
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &d->cmd;
    VKCHECK(vkQueueSubmit(d->queue, 1, &si, d->fence));
    VKCHECK(vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_MAX));
    vkDestroyBuffer(d->device, stage, nullptr);
    vkFreeMemory(d->device, stageMem, nullptr);

    // image view + descriptor
    VkImageViewCreateInfo iv{};
    iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv.image = d->tex; iv.viewType = VK_IMAGE_VIEW_TYPE_2D; iv.format = VK_FORMAT_R8G8B8A8_UNORM;
    iv.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VKCHECK(vkCreateImageView(d->device, &iv, nullptr, &d->texView));
    VkDescriptorImageInfo dii{ d->sampler, d->texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w{}; w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = d->dset; w.dstBinding = 0; w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
    vkUpdateDescriptorSets(d->device, 1, &w, 0, nullptr);
    return true;
}

bool Nvk::renderTexturedTriangles(const std::vector<NvkTexVertex>& verts, NvkClear clear) {
    Impl* d = d_;
    if (!d || !d->device || !d->tex) return false;
    const uint32_t vcount = (uint32_t)verts.size();
    VkDeviceSize need = vcount ? vcount * sizeof(NvkTexVertex) : sizeof(NvkTexVertex);
    if (need > d->texVbufCap) {
        if (d->texVbuf) { vkDestroyBuffer(d->device, d->texVbuf, nullptr); vkFreeMemory(d->device, d->texVbufMem, nullptr); }
        if (!d->createBuffer(need, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &d->texVbuf, &d->texVbufMem)) return false;
        d->texVbufCap = need;
    }
    if (vcount) {
        void* p = nullptr;
        VKCHECK(vkMapMemory(d->device, d->texVbufMem, 0, need, 0, &p));
        std::memcpy(p, verts.data(), vcount * sizeof(NvkTexVertex));
        vkUnmapMemory(d->device, d->texVbufMem);
    }
    VKCHECK(vkResetCommandBuffer(d->cmd, 0));
    VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(d->cmd, &bi));
    VkClearValue cv[2]{};
    cv[0].color = { { clear.r, clear.g, clear.b, clear.a } };
    cv[1].depthStencil = { 1.0f, 0 };
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = d->renderPass; rpb.framebuffer = d->framebuffer;
    rpb.renderArea = { {0, 0}, {d->w, d->h} };
    rpb.clearValueCount = 2; rpb.pClearValues = cv;
    vkCmdBeginRenderPass(d->cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    if (vcount) {
        vkCmdBindPipeline(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->texPipeline);
        vkCmdBindDescriptorSets(d->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, d->texPipeLayout,
                                0, 1, &d->dset, 0, nullptr);
        VkDeviceSize off = 0;
        vkCmdBindVertexBuffers(d->cmd, 0, 1, &d->texVbuf, &off);
        vkCmdDraw(d->cmd, vcount, 1, 0, 0);
    }
    vkCmdEndRenderPass(d->cmd);
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { d->w, d->h, 1 };
    vkCmdCopyImageToBuffer(d->cmd, d->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           d->readback, 1, &region);
    VKCHECK(vkEndCommandBuffer(d->cmd));
    VKCHECK(vkResetFences(d->device, 1, &d->fence));
    VkSubmitInfo si{}; si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &d->cmd;
    VKCHECK(vkQueueSubmit(d->queue, 1, &si, d->fence));
    VKCHECK(vkWaitForFences(d->device, 1, &d->fence, VK_TRUE, UINT64_MAX));
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
        if (d->texVbuf) vkDestroyBuffer(d->device, d->texVbuf, nullptr);
        if (d->texVbufMem) vkFreeMemory(d->device, d->texVbufMem, nullptr);
        // TEV path
        if (d->tevVbuf) vkDestroyBuffer(d->device, d->tevVbuf, nullptr);
        if (d->tevVbufMem) vkFreeMemory(d->device, d->tevVbufMem, nullptr);
        for (int i = 0; i < 8; ++i) {
            if (d->tevTex[i]) {
                vkDestroyImageView(d->device, d->tevTexView[i], nullptr);
                vkDestroyImage(d->device, d->tevTex[i], nullptr);
                vkFreeMemory(d->device, d->tevTexMem[i], nullptr);
            }
        }
        if (d->tevWhiteView) vkDestroyImageView(d->device, d->tevWhiteView, nullptr);
        if (d->tevWhite) vkDestroyImage(d->device, d->tevWhite, nullptr);
        if (d->tevWhiteMem) vkFreeMemory(d->device, d->tevWhiteMem, nullptr);
        if (d->tevSampler) vkDestroySampler(d->device, d->tevSampler, nullptr);
        if (d->tevDpool) vkDestroyDescriptorPool(d->device, d->tevDpool, nullptr);
        if (d->tevDsl) vkDestroyDescriptorSetLayout(d->device, d->tevDsl, nullptr);
        if (d->tevPipeline) vkDestroyPipeline(d->device, d->tevPipeline, nullptr);
        if (d->tevPipeLayout) vkDestroyPipelineLayout(d->device, d->tevPipeLayout, nullptr);
        if (d->tevVs) vkDestroyShaderModule(d->device, d->tevVs, nullptr);
        if (d->tevFs) vkDestroyShaderModule(d->device, d->tevFs, nullptr);
        if (d->texView) vkDestroyImageView(d->device, d->texView, nullptr);
        if (d->tex) vkDestroyImage(d->device, d->tex, nullptr);
        if (d->texMem) vkFreeMemory(d->device, d->texMem, nullptr);
        if (d->sampler) vkDestroySampler(d->device, d->sampler, nullptr);
        if (d->dpool) vkDestroyDescriptorPool(d->device, d->dpool, nullptr);
        if (d->dsl) vkDestroyDescriptorSetLayout(d->device, d->dsl, nullptr);
        if (d->texPipeline) vkDestroyPipeline(d->device, d->texPipeline, nullptr);
        if (d->texPipeLayout) vkDestroyPipelineLayout(d->device, d->texPipeLayout, nullptr);
        if (d->texVs) vkDestroyShaderModule(d->device, d->texVs, nullptr);
        if (d->texFs) vkDestroyShaderModule(d->device, d->texFs, nullptr);
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
        if (d->depthView) vkDestroyImageView(d->device, d->depthView, nullptr);
        if (d->depth) vkDestroyImage(d->device, d->depth, nullptr);
        if (d->depthMem) vkFreeMemory(d->device, d->depthMem, nullptr);
        vkDestroyDevice(d->device, nullptr);
    }
    if (d->instance) vkDestroyInstance(d->instance, nullptr);
    delete d_;
    d_ = nullptr;
}

} // namespace sb::render
