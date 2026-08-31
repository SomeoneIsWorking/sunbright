#include "native_j3d_adapter.h"

#include "host_allocation_scope.h"
#include "native_j3d_material_adapter.h"
#include "native_j3d_scene.h"

#include <sunbright/native_render/j3d_mesh_decode.h>
#include <sunbright/native_render/semantic_sink.h>

#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <dolphin/os.h>
#include <sb_log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {

constexpr std::uint32_t kColor0 = 11;
constexpr std::uint32_t kNormal = 10;

struct Stats {
    std::uint64_t considered = 0;
    std::uint64_t submittedModels = 0;
    std::uint64_t submittedLitModels = 0;
    std::uint64_t submittedAlphaMaskedModels = 0;
    std::uint64_t submittedVertices = 0;
    std::uint64_t layoutFailures = 0;
    std::uint64_t materialFailures = 0;
    std::uint64_t noPerspectiveContexts = 0;
    std::uint64_t nonRigidElements = 0;
    std::uint64_t decodeFailures = 0;
    std::array<std::uint64_t, 3> rasterFamilies{};
    std::array<std::uint64_t, 4> cullModes{};
};

Stats g_stats{};
std::vector<sb::native_render::J3dDecodedVertex> g_decoded;
std::vector<sb::native_render::MeshVertex> g_vertices;

bool read_native_memory(sb::native_render::ByteAddress address, std::span<std::uint8_t> output,
                        void*) {
    const std::uint8_t* source = address.native_pointer();
    if (source == nullptr || output.empty())
        return false;
    std::memcpy(output.data(), source, output.size());
    return true;
}

void record_raster(const sb::native_render::ModelMaterial& material) {
    const sb::native_render::ModelRasterPolicy& raster = sb::native_render::raster_policy(material);
    std::size_t family = 0;
    if (raster.alphaTest == sb::native_render::ModelAlphaTest::GreaterOrEqualHalf)
        family = 1;
    else if (raster.blend == sb::native_render::ModelBlendMode::SourceAlpha)
        family = 2;
    ++g_stats.rasterFamilies[family];
    ++g_stats.cullModes[static_cast<std::size_t>(raster.cull)];
}

bool build_layout(const J3DShape& shape, sb::native_render::J3dVertexLayout& layout) {
    if (shape.mVtxDescList == nullptr || shape.mVertexData == nullptr ||
        shape.mVertexData->getVtxAttrFmtList() == nullptr) {
        return false;
    }
    std::array<sb::native_render::J3dVertexDescriptor, 64> descriptors{};
    std::array<sb::native_render::J3dVertexFormat, 64> formats{};
    std::size_t descriptorCount = 0;
    while (descriptorCount < descriptors.size() &&
           shape.mVtxDescList[descriptorCount].attr != GX_VA_NULL) {
        const GXVtxDescList& descriptor = shape.mVtxDescList[descriptorCount];
        descriptors[descriptorCount] = {static_cast<std::uint32_t>(descriptor.attr),
                                        static_cast<std::uint32_t>(descriptor.type)};
        ++descriptorCount;
    }
    if (descriptorCount == descriptors.size())
        return false;
    const GXVtxAttrFmtList* sourceFormats = shape.mVertexData->getVtxAttrFmtList();
    std::size_t formatCount = 0;
    while (formatCount < formats.size() && sourceFormats[formatCount].attr != GX_VA_NULL) {
        const GXVtxAttrFmtList& format = sourceFormats[formatCount];
        formats[formatCount] = {static_cast<std::uint32_t>(format.attr),
                                static_cast<std::uint32_t>(format.cnt),
                                static_cast<std::uint32_t>(format.type), format.frac};
        ++formatCount;
    }
    if (formatCount == formats.size())
        return false;
    return sb::native_render::normalize_j3d_vertex_layout(
        std::span(descriptors).first(descriptorCount), std::span(formats).first(formatCount),
        shape.unk30, layout);
}

bool decode_element(const J3DShape& shape, std::uint32_t element,
                    const sb::native_render::J3dVertexLayout& layout) {
    J3DShapeDraw* draw = shape.getShapeDraw(element);
    if (draw == nullptr || shape.mVertexData == nullptr)
        return false;
    sb::native_render::J3dMeshElementSource source{
        .reader = {read_native_memory, nullptr},
        .layout = layout,
        .displayList = sb::native_render::ByteAddress::native(draw->getDisplayList()),
        .displayListSize = draw->getDisplayListSize(),
        .positions = sb::native_render::ByteAddress::native(j3dSys.getVtxPos()),
        .normals = sb::native_render::ByteAddress::native(j3dSys.getVtxNrm()),
        .colors = sb::native_render::ByteAddress::native(j3dSys.getVtxCol()),
        .arrayByteOrder = sb::native_render::J3dArrayByteOrder::Native,
    };
    for (std::uint32_t set = 0; set < sb::native_render::kJ3dTextureCoordinateSets; ++set) {
        source.textureCoordinates[set] =
            sb::native_render::ByteAddress::native(shape.mVertexData->getVtxTexCoordArray(set));
    }
    g_decoded.clear();
    return sb::native_render::decode_j3d_mesh_element(source, g_decoded).error ==
           sb::native_render::J3dMeshDecodeError::None;
}

} // namespace

extern "C" void sb_native_j3d_shape_submit(const void* shapePointer) {
    if (!sb::native_render::has_semantic_sink())
        return;
    ++g_stats.considered;
    if (shapePointer == nullptr)
        return;
    const sb::HostAllocationScope hostAllocations;
    const auto& shape = *static_cast<const J3DShape*>(shapePointer);

    sb::native_render::J3dVertexLayout layout{};
    if (!build_layout(shape, layout)) {
        ++g_stats.layoutFailures;
        return;
    }
    J3DMatPacket* materialPacket = j3dSys.getMatPacket();
    J3DMaterial* material = materialPacket != nullptr ? materialPacket->getMaterial() : nullptr;
    sb::CapturedNativeJ3dMaterial capturedMaterial{};
    sb::native_render::ResTimgDecodeError textureError{};
    if (material == nullptr ||
        sb::capture_native_j3d_material(
            *material, materialPacket->mTexture,
            layout.type[kColor0] !=
                static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::None),
            layout.type[kNormal] !=
                static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::None),
            capturedMaterial, textureError) != sb::NativeJ3dMaterialResult::Success) {
        ++g_stats.materialFailures;
        return;
    }

    const sb::native_render::ModelSceneContext* scene = sb::current_native_j3d_scene();
    if (scene == nullptr ||
        scene->projectionKind != sb::native_render::ProjectionKind::Perspective) {
        ++g_stats.noPerspectiveContexts;
        return;
    }
    if (shape.mElementCount == 0 || shape.mMatrices == nullptr || shape.mDrawMtxData == nullptr ||
        shape.mDrawMatrices == nullptr || shape.mCurrentViewNo == nullptr ||
        *shape.mCurrentViewNo > 16 || shape.mDrawMatrices[*shape.mCurrentViewNo] == nullptr) {
        ++g_stats.nonRigidElements;
        return;
    }

    std::array<sb::native_render::DecodedImageView, 4> image{};
    std::span<const sb::native_render::DecodedImageView> images;
    for (std::size_t index = 0; index < capturedMaterial.textureCount; ++index) {
        const auto& texture = capturedMaterial.textures[index];
        image[index] = {texture.texture.resource, texture.texture.revision, texture.texture.width,
                        texture.texture.height, texture.rgba8};
    }
    images = std::span(image).first(capturedMaterial.textureCount);
    for (std::uint32_t element = 0; element < shape.mElementCount; ++element) {
        J3DShapeMtx* matrixObject = shape.getShapeMtx(element);
        if (matrixObject == nullptr) {
            ++g_stats.nonRigidElements;
            continue;
        }
        std::array<sb::native_render::ModelMatrixBinding, sb::native_render::kMaxModelMatrices>
            poseBindings{};
        std::size_t poseBindingCount = 0;
        bool invalidPoseBinding = false;
        const std::uint32_t sourceMatrixCount = matrixObject->getUseMtxNum();
        for (std::uint32_t sourceSlot = 0; sourceSlot < sourceMatrixCount; ++sourceSlot) {
            const std::uint16_t matrixIndex = matrixObject->getUseMtxIndex(sourceSlot);
            if (matrixIndex == 0xFFFFU)
                continue;
            if (poseBindingCount == poseBindings.size()) {
                poseBindingCount = poseBindings.size() + 1;
                break;
            }
            if (matrixIndex >= shape.mDrawMtxData->mEntryNum) {
                invalidPoseBinding = true;
                break;
            }
            const Mtx& modelView = shape.mDrawMatrices[*shape.mCurrentViewNo][matrixIndex];
            auto& binding = poseBindings[poseBindingCount++];
            binding.sourceIndex = sourceSlot;
            std::copy_n(&modelView[0][0], binding.modelView.value.size(),
                        binding.modelView.value.begin());
        }
        std::array<std::uint8_t, 64> sourceToCompact{};
        sb::native_render::ModelPose pose{};
        if (invalidPoseBinding || poseBindingCount > poseBindings.size() ||
            sb::native_render::build_model_pose(std::span(poseBindings).first(poseBindingCount),
                                                pose, sourceToCompact) !=
                sb::native_render::ModelPoseBuildResult::Success) {
            ++g_stats.nonRigidElements;
            continue;
        }
        if (!decode_element(shape, element, layout) || g_decoded.empty()) {
            ++g_stats.decodeFailures;
            continue;
        }
        if (std::ranges::any_of(g_decoded, [&](const auto& vertex) {
                return vertex.positionMatrixSlot >= sourceToCompact.size() ||
                       sourceToCompact[vertex.positionMatrixSlot] == 0xFFU;
            })) {
            ++g_stats.nonRigidElements;
            continue;
        }

        g_vertices.clear();
        g_vertices.reserve(g_decoded.size());
        for (const auto& vertex : g_decoded) {
            g_vertices.push_back({.position = {vertex.x, vertex.y, vertex.z},
                                  .uv = {vertex.uv[0][0], vertex.uv[0][1]},
                                  .uv1 = {vertex.uv[1][0], vertex.uv[1][1]},
                                  .uv2 = {vertex.uv[2][0], vertex.uv[2][1]},
                                  .uv3 = {vertex.uv[3][0], vertex.uv[3][1]},
                                  .color = sb::native_render::color_from_rgba8(vertex.rgba),
                                  .normal = {vertex.nx, vertex.ny, vertex.nz},
                                  .matrixIndex = sourceToCompact[vertex.positionMatrixSlot]});
        }
        J3DShapeDraw* shapeDraw = shape.getShapeDraw(element);
        const std::uint64_t resource = reinterpret_cast<std::uintptr_t>(shapeDraw);
        const std::uint64_t revision = sb::native_render::mesh_revision(g_vertices);
        sb::native_render::ModelDraw draw{};
        draw.instance =
            reinterpret_cast<std::uintptr_t>(&shape) ^
            reinterpret_cast<std::uintptr_t>(shape.mDrawMatrices[*shape.mCurrentViewNo]);
        draw.mesh = {resource, revision, static_cast<std::uint32_t>(g_vertices.size())};
        draw.pose = pose;
        draw.projection = scene->projection;
        draw.material = capturedMaterial.material;
        draw.fog = capturedMaterial.fog;
        const sb::native_render::MeshResourceView mesh{resource, revision, g_vertices};
        if (!sb::native_render::submit_model(draw, mesh, images)) {
            OSPanic(__FILE__, __LINE__,
                    "native semantic renderer rejected validated J3D model shape=%p element=%u",
                    &shape, element);
        }
        record_raster(capturedMaterial.material);
        ++g_stats.submittedModels;
        if (std::holds_alternative<sb::native_render::LitColorMaterial>(
                capturedMaterial.material) ||
            std::holds_alternative<sb::native_render::LitTexturedMaterial>(
                capturedMaterial.material) ||
            std::holds_alternative<sb::native_render::LitTexturedAlphaMaskMaterial>(
                capturedMaterial.material) ||
            std::holds_alternative<sb::native_render::LitLayeredTexturedMaterial>(
                capturedMaterial.material) ||
            std::holds_alternative<sb::native_render::LitSpecularColorMaterial>(
                capturedMaterial.material) ||
            std::holds_alternative<sb::native_render::LitSpecularTexturedMaterial>(
                capturedMaterial.material)) {
            ++g_stats.submittedLitModels;
        }
        if (std::holds_alternative<sb::native_render::AlphaMaskedColorMaterial>(
                capturedMaterial.material)) {
            ++g_stats.submittedAlphaMaskedModels;
        }
        g_stats.submittedVertices += g_vertices.size();
    }
}

extern "C" void sb_native_j3d_report_stats(void) {
    const sb::NativeJ3dSceneStats sceneStats = sb::native_j3d_scene_stats();
    sb_logf("semantic",
            "native J3D models: considered=%llu submitted=%llu models/%llu vertices "
            "(%llu lit models, %llu solid-colour alpha-mask models) "
            "rejected(layout=%llu material=%llu no-perspective-context=%llu non-rigid=%llu "
            "decode=%llu); high-level camera dispatches: perspective=%llu orthographic=%llu "
            "unavailable-before-camera=%llu; published raster families: opaque=%llu cutout=%llu "
            "translucent=%llu cull(none=%llu front=%llu back=%llu all=%llu)",
            static_cast<unsigned long long>(g_stats.considered),
            static_cast<unsigned long long>(g_stats.submittedModels),
            static_cast<unsigned long long>(g_stats.submittedVertices),
            static_cast<unsigned long long>(g_stats.submittedLitModels),
            static_cast<unsigned long long>(g_stats.submittedAlphaMaskedModels),
            static_cast<unsigned long long>(g_stats.layoutFailures),
            static_cast<unsigned long long>(g_stats.materialFailures),
            static_cast<unsigned long long>(g_stats.noPerspectiveContexts),
            static_cast<unsigned long long>(g_stats.nonRigidElements),
            static_cast<unsigned long long>(g_stats.decodeFailures),
            static_cast<unsigned long long>(sceneStats.perspectiveDispatches),
            static_cast<unsigned long long>(sceneStats.orthographicDispatches),
            static_cast<unsigned long long>(sceneStats.unavailableDispatches),
            static_cast<unsigned long long>(g_stats.rasterFamilies[0]),
            static_cast<unsigned long long>(g_stats.rasterFamilies[1]),
            static_cast<unsigned long long>(g_stats.rasterFamilies[2]),
            static_cast<unsigned long long>(g_stats.cullModes[0]),
            static_cast<unsigned long long>(g_stats.cullModes[1]),
            static_cast<unsigned long long>(g_stats.cullModes[2]),
            static_cast<unsigned long long>(g_stats.cullModes[3]));
}
