// vk_mesh — N4/N5 (docs/native_port_plan.md §3): native 3D world rendering. Our
// own Vulkan pipeline rasterizes the game's real J3D geometry — captured,
// extracted, transformed (model→eye→clip) and projected natively by ngx
// (ngx_j3d_shape.cpp) — into an offscreen color+depth target, with ZERO Dolphin
// VideoCommon. N5 first slice: each draw BATCH carries its bound GX texmap-0
// texture, decoded natively by the N1 decoder (tex_decode) and sampled ×
// vertex-color0; batches with no/unsupported texture use a 1×1 white texel
// (→ flat vertex color). Full TEV combiner is the next step.
//
// On-demand self-test (/ngxrender) reusing Dolphin's VkDevice as bring-up
// scaffold (like vk_quad N2); render → read back → coverage + PPM dump. Geometry
// is a best-effort rolling snapshot of recent scene triangles.

#include "tex_decode.h"
#include "tev_shader.h"
#include "glsl_compile.h"
#include "../ngx/ngx_render_data.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#ifdef HAVE_DOLPHIN_MEMMAP
#include "VideoBackends/Vulkan/VulkanLoader.h"
#include "VideoBackends/Vulkan/VulkanContext.h"
#include "render/shaders/mesh_vert_spv.h"
#include "render/shaders/mesh_frag_spv.h"

#include "../intrinsics.h"   // sb_ram_fast (guest RAM → host pointer)

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

constexpr int   MAXTEX       = 1500;                 // unique textures cap
constexpr size_t TEX_STAGING = 96u * 1024 * 1024;    // total decoded-texel budget

}  // namespace

// Render the captured native geometry, textured. Returns covered-pixel count
// (>=0), -1 if unavailable / no geometry, -2 on a Vulkan failure.
int sb_ngx_render(char* outbuf, int cap) {
    Report rep{outbuf, cap};

    if (!Vulkan::g_vulkan_context) { rep("ngx_render: no g_vulkan_context (set SUNBRIGHT_NGX_SHAPE=1 + be in a 3D scene)\n"); return -1; }
    const VkDevice dev = Vulkan::g_vulkan_context->GetDevice();
    const VkPhysicalDevice phys = Vulkan::g_vulkan_context->GetPhysicalDevice();
    const VkQueue queue = Vulkan::g_vulkan_context->GetGraphicsQueue();
    const uint32_t qfam = Vulkan::g_vulkan_context->GetGraphicsQueueFamilyIndex();
    if (dev == VK_NULL_HANDLE) { rep("ngx_render: null device\n"); return -1; }

    // ── Snapshot geometry + batches (copy promptly — emu thread keeps writing).
    int nv = 0, nb = 0;
    const NgxRenderVertex* sv = ngx_snap_verts(&nv);
    const NgxRenderBatch* sb = ngx_snap_batches(&nb);
    if (!sv || nv < 3 || !sb || nb < 1) { rep("ngx_render: no geometry (nverts=%d nbatches=%d)\n", nv, nb); return -1; }
    std::vector<NgxRenderVertex> verts(sv, sv + nv);
    std::vector<NgxRenderBatch> batches(sb, sb + nb);
    const VkDeviceSize vbytes = (VkDeviceSize)nv * sizeof(NgxRenderVertex);

    // ── N5: snapshot the per-material TEV-state table (batches index into it) ────
    int ntev = 0;
    const NgxTevState* stv = ngx_snap_tevstates(&ntev);
    std::vector<NgxTevState> tevstates(stv, stv + (ntev > 0 ? ntev : 0));

    // Material push constants the generated TEV shaders consume (kcolor 0..255,
    // tevreg c0/c1/c2 as S10) — must match the push_constant block in tev_shader.
    struct MatPC { int32_t kcolor[4][4]; int32_t tevreg[3][4]; };
    auto fill_pc = [&](MatPC& pc, int tev_index) {
        std::memset(&pc, 0, sizeof pc);
        if (tev_index >= 0 && tev_index < (int)tevstates.size()) {
            const NgxTevState& s = tevstates[tev_index];
            for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) pc.kcolor[c][k] = s.kcolor[c][k];
            for (int c = 0; c < 3; c++) for (int k = 0; k < 4; k++) pc.tevreg[c][k] = s.tev_color[c][k];
        } else {  // fallback (modulate): white konst, zero tev regs
            for (int c = 0; c < 4; c++) for (int k = 0; k < 4; k++) pc.kcolor[c][k] = 255;
        }
    };

    // ── Decode the unique textures (N1 decoder) into one staging blob ───────────
    struct TexEntry { int w, h; size_t off; };           // off = byte offset in staging
    std::vector<TexEntry> texs;                           // index 0 = white
    std::unordered_map<uint64_t, int> tex_index;          // key(addr,fmt,w,h) → texs index
    std::vector<uint8_t> staging;                         // concatenated RGBA8
    auto white_off = staging.size();
    { uint32_t w = 0xFFFFFFFFu; staging.insert(staging.end(), (uint8_t*)&w, (uint8_t*)&w + 4); }
    texs.push_back({1, 1, white_off});                    // index 0: 1×1 white
    std::vector<int> batch_tex(batches.size(), 0);        // batch → texs index

    for (size_t b = 0; b < batches.size(); b++) {
        const NgxRenderBatch& B = batches[b];
        if (B.tex_addr == 0 || B.w == 0 || B.h == 0) { batch_tex[b] = 0; continue; }
        const uint64_t key = ((uint64_t)B.tex_addr << 16) ^ ((uint64_t)B.fmt << 56) ^
                             ((uint64_t)B.w << 40) ^ ((uint64_t)B.h << 28) ^ ((uint64_t)B.tlut_addr << 4);
        auto it = tex_index.find(key);
        if (it != tex_index.end()) { batch_tex[b] = it->second; continue; }
        if ((int)texs.size() >= MAXTEX) { batch_tex[b] = 0; continue; }
        const uint8_t* host = sb_ram_fast(B.tex_addr);
        if (!host) { batch_tex[b] = 0; continue; }
        const int w = B.w, h = B.h;
        // Guard: the tiled source must fit within the 24 MB main RAM window.
        const int srcbytes = sb_tex_size_bytes(w, h, B.fmt);
        if (srcbytes <= 0 || (B.tex_addr & 0x01FFFFFFu) + (uint32_t)srcbytes > 0x1800000u) { batch_tex[b] = 0; continue; }
        // CI formats need the palette; bound it (C4=16 / C8=256 / C14X2=16384 entries × 2 B).
        const uint8_t* tlut = nullptr;
        if (B.fmt == SB_TF_C4 || B.fmt == SB_TF_C8 || B.fmt == SB_TF_C14X2) {
            const uint32_t entries = B.fmt == SB_TF_C4 ? 16u : B.fmt == SB_TF_C8 ? 256u : 16384u;
            if (B.tlut_addr && (B.tlut_addr & 0x01FFFFFFu) + entries * 2u <= 0x1800000u)
                tlut = sb_ram_fast(B.tlut_addr);
            if (!tlut) { batch_tex[b] = 0; continue; }   // no palette → render flat
        }
        const size_t rgba = (size_t)w * h * 4;
        if (staging.size() + rgba > TEX_STAGING) { batch_tex[b] = 0; continue; }
        const size_t off = staging.size();
        staging.resize(off + rgba);
        sb_tex_decode((uint32_t*)(staging.data() + off), host, w, h, B.fmt, tlut, B.tlut_fmt);
        const int idx = (int)texs.size();
        texs.push_back({w, h, off});
        tex_index[key] = idx;
        batch_tex[b] = idx;
    }
    const int ntex = (int)texs.size();

    const int W = 640, H = 448;
    const VkDeviceSize img_bytes = (VkDeviceSize)W * H * 4;

    // Handles.
    VkImage rt_img = VK_NULL_HANDLE; VkDeviceMemory rt_mem = VK_NULL_HANDLE; VkImageView rt_view = VK_NULL_HANDLE;
    VkImage ds_img = VK_NULL_HANDLE; VkDeviceMemory ds_mem = VK_NULL_HANDLE; VkImageView ds_view = VK_NULL_HANDLE;
    VkBuffer vbuf = VK_NULL_HANDLE, rb_buf = VK_NULL_HANDLE, stg_buf = VK_NULL_HANDLE;
    VkDeviceMemory vmem = VK_NULL_HANDLE, rb_mem = VK_NULL_HANDLE, stg_mem = VK_NULL_HANDLE;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkPipelineLayout pll = VK_NULL_HANDLE;
    VkRenderPass rpass = VK_NULL_HANDLE;
    VkFramebuffer fbo = VK_NULL_HANDLE;
    // N5: one fragment shader + pipeline per distinct material TEV state, plus a
    // modulate fallback (slot 0) for batches with no material.
    std::vector<VkShaderModule> fsmods;
    std::vector<VkPipeline>     pipes;
    std::vector<int>            batch_pipe(batches.size(), 0);   // batch → pipes[] slot
    int frag_compiled = 0, frag_failed = 0;
    VkCommandPool cpool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::vector<VkImage> timg(ntex, VK_NULL_HANDLE);
    std::vector<VkDeviceMemory> tmem(ntex, VK_NULL_HANDLE);
    std::vector<VkImageView> tview(ntex, VK_NULL_HANDLE);
    std::vector<VkDescriptorSet> tset(ntex, VK_NULL_HANDLE);

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
    auto make_dev_image = [&](VkImage& img, VkDeviceMemory& mem, VkImageView& view,
                              VkFormat fmt, int w, int h, VkImageUsageFlags usage, VkImageAspectFlags asp) -> bool {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
        ici.extent = {(uint32_t)w, (uint32_t)h, 1}; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage; ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if ((vr = vkCreateImage(dev, &ici, nullptr, &img))) return false;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = find_mem(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if ((vr = vkAllocateMemory(dev, &ai, nullptr, &mem))) return false;
        if ((vr = vkBindImageMemory(dev, img, mem, 0))) return false;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = {asp, 0, 1, 0, 1};
        return (vr = vkCreateImageView(dev, &vci, nullptr, &view)) == VK_SUCCESS;
    };
    // ── Resources ───────────────────────────────────────────────────────────────
    if (!make_dev_image(rt_img, rt_mem, rt_view, VK_FORMAT_R8G8B8A8_UNORM, W, H,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) { FAIL("rt image"); goto done; }
    if (!make_dev_image(ds_img, ds_mem, ds_view, VK_FORMAT_D32_SFLOAT, W, H,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) { FAIL("ds image"); goto done; }
    if (!make_buffer(vbuf, vmem, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) { FAIL("vertex buf"); goto done; }
    if (!make_buffer(rb_buf, rb_mem, img_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) { FAIL("readback buf"); goto done; }
    if (!make_buffer(stg_buf, stg_mem, staging.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) { FAIL("staging buf"); goto done; }
    {
        void* p = nullptr;
        if ((vr = vkMapMemory(dev, vmem, 0, vbytes, 0, &p))) { FAIL("map vbuf"); goto done; }
        memcpy(p, verts.data(), vbytes); vkUnmapMemory(dev, vmem);
        if ((vr = vkMapMemory(dev, stg_mem, 0, staging.size(), 0, &p))) { FAIL("map staging"); goto done; }
        memcpy(p, staging.data(), staging.size()); vkUnmapMemory(dev, stg_mem);
    }
    {
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize = sizeof kMeshVertSpv; si.pCode = kMeshVertSpv;
        if ((vr = vkCreateShaderModule(dev, &si, nullptr, &vs))) { FAIL("vert shader"); goto done; }

        // Build one fragment-shader module per distinct material TEV state (the
        // per-material combiner generated by tev_shader, compiled by glsl_compile).
        // Slot 0 = GX_MODULATE fallback (texColor*rasColor) for batches w/o material.
        std::unordered_map<int, int> slot_of_tev;   // tev_index → pipes/fsmods slot
        auto build_module = [&](const std::string& glsl) -> int {
            auto spv = sb_compile_fragment_glsl(glsl);
            if (spv.empty()) { frag_failed++; return -1; }
            VkShaderModuleCreateInfo fi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            fi.codeSize = spv.size() * sizeof(uint32_t); fi.pCode = spv.data();
            VkShaderModule m = VK_NULL_HANDLE;
            if (vkCreateShaderModule(dev, &fi, nullptr, &m)) { frag_failed++; return -1; }
            fsmods.push_back(m); frag_compiled++;
            return (int)fsmods.size() - 1;
        };
        // Slot 0: modulate fallback.
        NgxTevState mod{}; mod.num_stages = 1;
        mod.stage[0].color_env = (15u<<12)|(8u<<8)|(10u<<4)|15u | (1u<<19);   // ZERO,TEXC,RASC,ZERO
        mod.stage[0].alpha_env = (7u<<13)|(4u<<10)|(5u<<7)|(7u<<4) | (1u<<19);
        mod.stage[0].texmap = 0; mod.stage[0].color_chan = 4;
        if (build_module(sb_tev_gen_fragment(mod)) != 0) { FAIL("modulate frag shader"); goto done; }
        // One module per material referenced by a batch.
        for (size_t b = 0; b < batches.size(); b++) {
            const int ti = batches[b].tev_index;
            if (ti < 0 || ti >= (int)tevstates.size()) { batch_pipe[b] = 0; continue; }
            auto it = slot_of_tev.find(ti);
            if (it != slot_of_tev.end()) { batch_pipe[b] = it->second; continue; }
            int slot = build_module(sb_tev_gen_fragment(tevstates[ti]));
            if (slot < 0) slot = 0;                       // compile failed → modulate fallback
            slot_of_tev[ti] = slot; batch_pipe[b] = slot;
        }
    }
    // Per-texture device images (decoded RGBA8 uploaded from staging in the cmd buf).
    for (int i = 0; i < ntex; i++)
        if (!make_dev_image(timg[i], tmem[i], tview[i], VK_FORMAT_R8G8B8A8_UNORM, texs[i].w, texs[i].h,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) { FAIL("tex image"); goto done; }

    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if ((vr = vkCreateSampler(dev, &sci, nullptr, &sampler))) { FAIL("sampler"); goto done; }

        VkDescriptorSetLayoutBinding b{};
        b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1; li.pBindings = &b;
        if ((vr = vkCreateDescriptorSetLayout(dev, &li, nullptr, &dsl))) { FAIL("dsl"); goto done; }

        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)ntex};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = (uint32_t)ntex; pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
        if ((vr = vkCreateDescriptorPool(dev, &pci, nullptr, &dpool))) { FAIL("dpool"); goto done; }
        for (int i = 0; i < ntex; i++) {
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl;
            if ((vr = vkAllocateDescriptorSets(dev, &dai, &tset[i]))) { FAIL("dset alloc"); goto done; }
        }

        VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(MatPC)};  // kcolor[4]+tevreg[3]
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1; plci.pSetLayouts = &dsl;
        plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
        if ((vr = vkCreatePipelineLayout(dev, &plci, nullptr, &pll))) { FAIL("pipeline layout"); goto done; }
    }

    {
        VkAttachmentDescription at[2] = {};
        at[0].format = VK_FORMAT_R8G8B8A8_UNORM; at[0].samples = VK_SAMPLE_COUNT_1_BIT;
        at[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        at[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; at[0].finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
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
        ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; ss[1].pName = "main";   // module set per-slot below

        VkVertexInputBindingDescription vib{0, sizeof(NgxRenderVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription via[3] = {
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},                  // clip
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)},  // rgba
            {2, 0, VK_FORMAT_R32G32_SFLOAT,       8 * sizeof(float)},  // uv
        };
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vib;
        vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = via;
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
        dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState cba{}; cba.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1; cb.pAttachments = &cba;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2; gp.pStages = ss; gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps; gp.pRasterizationState = &rs; gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &dss; gp.pColorBlendState = &cb; gp.layout = pll; gp.renderPass = rpass; gp.subpass = 0;
        pipes.assign(fsmods.size(), VK_NULL_HANDLE);
        for (size_t i = 0; i < fsmods.size(); i++) {
            ss[1].module = fsmods[i];
            if ((vr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &pipes[i]))) { FAIL("pipeline"); goto done; }
        }
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

    // Bind each texture image into its descriptor set.
    for (int i = 0; i < ntex; i++) {
        VkDescriptorImageInfo dii{sampler, tview[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = tset[i]; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    // ── Record ──────────────────────────────────────────────────────────────────
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ((vr = vkBeginCommandBuffer(cmd, &bi))) { FAIL("begin cmd"); goto done; }

        auto barrier = [&](VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa,
                           VkAccessFlags da, VkPipelineStageFlags s2, VkPipelineStageFlags ds) {
            VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            mb.oldLayout = from; mb.newLayout = to;
            mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mb.image = img; mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            mb.srcAccessMask = sa; mb.dstAccessMask = da;
            vkCmdPipelineBarrier(cmd, s2, ds, 0, 0, nullptr, 0, nullptr, 1, &mb);
        };
        // Upload every decoded texture from staging.
        for (int i = 0; i < ntex; i++) {
            barrier(timg[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy c{}; c.bufferOffset = texs[i].off;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {(uint32_t)texs[i].w, (uint32_t)texs[i].h, 1};
            vkCmdCopyBufferToImage(cmd, stg_buf, timg[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(timg[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        }

        VkClearValue clear[2]{};
        clear[0].color = {{0.10f, 0.12f, 0.18f, 1.f}};
        clear[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rpass; rbi.framebuffer = fbo;
        rbi.renderArea = {{0, 0}, {(uint32_t)W, (uint32_t)H}};
        rbi.clearValueCount = 2; rbi.pClearValues = clear;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        VkDeviceSize voff = 0; vkCmdBindVertexBuffers(cmd, 0, 1, &vbuf, &voff);
        int last_tex = -1, last_pipe = -1;
        for (size_t b = 0; b < batches.size(); b++) {
            const int pslot = batch_pipe[b] < (int)pipes.size() ? batch_pipe[b] : 0;
            if (pslot != last_pipe) { vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[pslot]); last_pipe = pslot; }
            const int ti = batch_tex[b];
            if (ti != last_tex) { vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pll, 0, 1, &tset[ti], 0, nullptr); last_tex = ti; }
            MatPC pc; fill_pc(pc, batches[b].tev_index);
            vkCmdPushConstants(cmd, pll, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            vkCmdDraw(cmd, batches[b].vcount, 1, batches[b].vstart, 0);
        }
        vkCmdEndRenderPass(cmd);

        VkBufferImageCopy rc{}; rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rc.imageExtent = {(uint32_t)W, (uint32_t)H, 1};
        vkCmdCopyImageToBuffer(cmd, rt_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb_buf, 1, &rc);

        if ((vr = vkEndCommandBuffer(cmd))) { FAIL("end cmd"); goto done; }
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        if ((vr = vkQueueSubmit(queue, 1, &si, fence))) { FAIL("submit"); goto done; }
        if ((vr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 10'000'000'000ull))) { FAIL("fence wait"); goto done; }
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
        rep("ngx_render: %d verts (%d tris), %d batches, %d unique textures -> %dx%d covered=%ld (%.1f%%)\n",
            nv, nv / 3, nb, ntex - 1, W, H, covered, pct);
        rep("  TEV shaders: %d compiled (+1 modulate), %d failed; %d material states\n",
            frag_compiled - 1, frag_failed, ntev);
        rep("  PPM: %s\n  verdict=%s\n", ppm, covered > 0 ? "PIXELS-OK" : "EMPTY");
        result = (int)covered;
        vkUnmapMemory(dev, rb_mem);
    }

done:
    vkDeviceWaitIdle(dev);
    if (fence)  vkDestroyFence(dev, fence, nullptr);
    if (cpool)  vkDestroyCommandPool(dev, cpool, nullptr);
    for (VkPipeline p : pipes) if (p) vkDestroyPipeline(dev, p, nullptr);
    if (fbo)    vkDestroyFramebuffer(dev, fbo, nullptr);
    if (rpass)  vkDestroyRenderPass(dev, rpass, nullptr);
    if (pll)    vkDestroyPipelineLayout(dev, pll, nullptr);
    if (dpool)  vkDestroyDescriptorPool(dev, dpool, nullptr);   // frees sets
    if (dsl)    vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    if (sampler) vkDestroySampler(dev, sampler, nullptr);
    for (int i = 0; i < ntex; i++) {
        if (tview[i]) vkDestroyImageView(dev, tview[i], nullptr);
        if (timg[i])  vkDestroyImage(dev, timg[i], nullptr);
        if (tmem[i])  vkFreeMemory(dev, tmem[i], nullptr);
    }
    if (vs)     vkDestroyShaderModule(dev, vs, nullptr);
    for (VkShaderModule m : fsmods) if (m) vkDestroyShaderModule(dev, m, nullptr);
    if (stg_buf) vkDestroyBuffer(dev, stg_buf, nullptr);
    if (stg_mem) vkFreeMemory(dev, stg_mem, nullptr);
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
