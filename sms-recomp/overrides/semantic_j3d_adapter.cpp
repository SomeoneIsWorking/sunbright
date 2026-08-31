#include "semantic_j3d_adapter.h"

#include "guest_j3d_texture_adapter.h"
#include "semantic_j3d_lighting.h"
#include "semantic_j3d_material_adapter.h"
#include "semantic_j3d_scene.h"

#include "../runtime/probe_server.h"
#include "../runtime/render/j3d_decode.h"
#include "../runtime/sb_assert.h"

#include <sunbright/native_render/j3d_lit_material.h>
#include <sunbright/native_render/j3d_stage_lighting.h>
#include <sunbright/native_render/model_context.h>
#include <sunbright/native_render/semantic_sink.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr u32 kJ3dSys = 0x804045DC;
constexpr u32 kJ3dSysMaterialPacket = 0x3C;
constexpr u32 kMaterialPacketMaterial = 0x38;
constexpr u32 kMaterialPacketTexture = 0x40;
constexpr u32 kShapeElementCount = 0x06;
constexpr u32 kShapeMatrices = 0x34;
constexpr u32 kShapeDrawMatrices = 0x50;
constexpr u32 kShapeCurrentView = 0x58;
// Retail US J3DShapeMtx constructor 0x802dfcc8 writes this exact CodeWarrior vtable address.
constexpr u32 kBaseShapeMatrixVptr = 0x803E125C;
constexpr std::uint32_t kColor0 = 11;
constexpr std::uint32_t kNormal = 10;

struct Stats {
    std::uint64_t shapeDraws = 0;
    std::uint64_t materialMemoryFailures = 0;
    std::array<std::uint64_t, 10> materialRejections{};
    std::uint64_t layoutFailures = 0;
    std::uint64_t noPerspectiveContexts = 0;
    std::uint64_t nonRigidElements = 0;
    std::uint64_t decodeFailures = 0;
    std::uint64_t textureTableFailures = 0;
    std::array<std::uint64_t, 12> textureDecodeFailures{};
    std::array<std::uint64_t, 11> texturedMaterialRejections{};
    std::array<std::uint64_t, 12> litTexturedMaterialRejections{};
    std::uint64_t submittedModels = 0;
    std::uint64_t submittedLitModels = 0;
    std::uint64_t submittedVertices = 0;
    std::uint64_t unlitTexturedCandidates = 0;
    std::uint64_t litUntexturedCandidates = 0;
    std::uint64_t litTexturedCandidates = 0;
    std::array<std::uint64_t, 3> rasterFamilies{};
    std::array<std::uint64_t, 4> cullModes{};
};

struct ProgramKey {
    bool lighting = false;
    bool hasNormal = false;
    std::uint16_t channelControl = 0;
    std::uint16_t alphaChannelControl = 0;
    std::uint8_t stageCount = 0;
    std::uint16_t textureNumber = 0;
    std::uint8_t textureCoordinate = 0;
    std::uint8_t textureMap = 0;
    std::uint8_t colorChannel = 0;
    std::array<std::uint8_t, 8> stage{};
    std::uint16_t textureNumber1 = 0;
    std::uint8_t textureCoordinate1 = 0;
    std::uint8_t textureMap1 = 0;
    std::uint8_t colorChannel1 = 0;
    std::array<std::uint8_t, 8> stage1{};
    auto operator<=>(const ProgramKey&) const = default;
};

Stats g_stats{};
std::map<ProgramKey, std::uint64_t> g_programs;
std::vector<J3DVert> g_decoded;
std::vector<sb::native_render::MeshVertex> g_vertices;
sb::native_render::DecodedTexture g_texture;

bool readable(u32 address) {
    return sb_ram_fast(address) != nullptr;
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

float guest_f32(u32 address) {
    const u32 bits = sb_r32(address);
    float value = 0.0F;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
}

const bool g_probe = [] {
    sb_probe_register("/semantic-j3d", "PC-native rigid unlit J3D model coverage",
                      [](const ProbeArgs&) { return semantic_j3d_stats_text(); });
    return true;
}();

} // namespace

std::string semantic_j3d_stats_text() {
    std::uint64_t textureDecodeFailures = 0;
    for (std::uint64_t count : g_stats.textureDecodeFailures)
        textureDecodeFailures += count;
    char output[1792];
    std::snprintf(
        output, sizeof(output),
        "J3D native-model coverage: considered=%llu submitted=%llu models/%llu vertices "
        "(%llu lit models); "
        "unreadable=%llu layout=%llu no-perspective-context=%llu non-rigid=%llu decode=%llu "
        "texture-table=%llu texture-decode=%llu; material "
        "rejections: colour-block=%llu lighting=%llu missing-channel=%llu texture=%llu "
        "tev-family=%llu multi-stage=%llu colour-program=%llu missing-vertex-colour=%llu "
        "raster-policy=%llu textured-raster-policy=%llu; published raster families: opaque=%llu "
        "cutout=%llu translucent=%llu "
        "cull(none=%llu front=%llu back=%llu all=%llu); "
        "exact next-family candidates: unlit+textured=%llu lit+untextured=%llu "
        "lit+textured=%llu",
        static_cast<unsigned long long>(g_stats.shapeDraws),
        static_cast<unsigned long long>(g_stats.submittedModels),
        static_cast<unsigned long long>(g_stats.submittedVertices),
        static_cast<unsigned long long>(g_stats.submittedLitModels),
        static_cast<unsigned long long>(g_stats.materialMemoryFailures),
        static_cast<unsigned long long>(g_stats.layoutFailures),
        static_cast<unsigned long long>(g_stats.noPerspectiveContexts),
        static_cast<unsigned long long>(g_stats.nonRigidElements),
        static_cast<unsigned long long>(g_stats.decodeFailures),
        static_cast<unsigned long long>(g_stats.textureTableFailures),
        static_cast<unsigned long long>(textureDecodeFailures),
        static_cast<unsigned long long>(g_stats.materialRejections[1]),
        static_cast<unsigned long long>(g_stats.materialRejections[2]),
        static_cast<unsigned long long>(g_stats.materialRejections[3]),
        static_cast<unsigned long long>(g_stats.materialRejections[4]),
        static_cast<unsigned long long>(g_stats.materialRejections[5]),
        static_cast<unsigned long long>(g_stats.materialRejections[6]),
        static_cast<unsigned long long>(g_stats.materialRejections[7]),
        static_cast<unsigned long long>(g_stats.materialRejections[8]),
        static_cast<unsigned long long>(g_stats.materialRejections[9]),
        static_cast<unsigned long long>(g_stats.texturedMaterialRejections[10]),
        static_cast<unsigned long long>(g_stats.rasterFamilies[0]),
        static_cast<unsigned long long>(g_stats.rasterFamilies[1]),
        static_cast<unsigned long long>(g_stats.rasterFamilies[2]),
        static_cast<unsigned long long>(g_stats.cullModes[0]),
        static_cast<unsigned long long>(g_stats.cullModes[1]),
        static_cast<unsigned long long>(g_stats.cullModes[2]),
        static_cast<unsigned long long>(g_stats.cullModes[3]),
        static_cast<unsigned long long>(g_stats.unlitTexturedCandidates),
        static_cast<unsigned long long>(g_stats.litUntexturedCandidates),
        static_cast<unsigned long long>(g_stats.litTexturedCandidates));
    std::string report(output);
    const sb::recomp::SemanticJ3dSceneStats sceneStats = sb::recomp::semantic_j3d_scene_stats();
    const sb::recomp::SemanticJ3dLightingStats lightingStats =
        sb::recomp::semantic_j3d_lighting_stats();
    char sceneLine[240];
    std::snprintf(sceneLine, sizeof(sceneLine),
                  "; high-level camera dispatches: perspective=%llu orthographic=%llu "
                  "unavailable-before-camera=%llu",
                  static_cast<unsigned long long>(sceneStats.perspectiveDispatches),
                  static_cast<unsigned long long>(sceneStats.orthographicDispatches),
                  static_cast<unsigned long long>(sceneStats.unavailableDispatches));
    report += sceneLine;
    char lightingLine[280];
    std::snprintf(lightingLine, sizeof(lightingLine),
                  "; high-level stage lights: published=%llu/%llu failures(view=%llu "
                  "primary-position=%llu manager=%llu effect=%llu)",
                  static_cast<unsigned long long>(lightingStats.published),
                  static_cast<unsigned long long>(lightingStats.attempts),
                  static_cast<unsigned long long>(lightingStats.viewFailures),
                  static_cast<unsigned long long>(lightingStats.primaryPositionFailures),
                  static_cast<unsigned long long>(lightingStats.managerFailures),
                  static_cast<unsigned long long>(lightingStats.effectFailures));
    report += lightingLine;
    char litRejectionLine[420];
    std::snprintf(
        litRejectionLine, sizeof(litRejectionLine),
        "; lit-textured rejections: colour-block=%llu channels=%llu tev-block=%llu stages=%llu "
        "texcoord=%llu binding=%llu program=%llu normal=%llu vertex-colour=%llu "
        "lighting-context=%llu raster=%llu",
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[1]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[2]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[3]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[4]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[5]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[6]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[7]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[8]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[9]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[10]),
        static_cast<unsigned long long>(g_stats.litTexturedMaterialRejections[11]));
    report += litRejectionLine;
    std::vector<std::pair<ProgramKey, std::uint64_t>> programs(g_programs.begin(),
                                                               g_programs.end());
    std::ranges::sort(programs, [](const auto& first, const auto& second) {
        return first.second > second.second;
    });
    const std::size_t shown = std::min<std::size_t>(programs.size(), 8);
    for (std::size_t index = 0; index < shown; ++index) {
        const ProgramKey& key = programs[index].first;
        char line[320];
        std::snprintf(line, sizeof(line),
                      "; top-program[%zu]=%llu lit=%u normal=%u chan=%04x/%04x stages=%u tex=%04x "
                      "order=%02x/%02x/%02x "
                      "stage=%02x%02x%02x%02x%02x%02x%02x%02x",
                      index, static_cast<unsigned long long>(programs[index].second),
                      key.lighting ? 1U : 0U, key.hasNormal ? 1U : 0U, key.channelControl,
                      key.alphaChannelControl, key.stageCount, key.textureNumber,
                      key.textureCoordinate, key.textureMap, key.colorChannel, key.stage[0],
                      key.stage[1], key.stage[2], key.stage[3], key.stage[4], key.stage[5],
                      key.stage[6], key.stage[7]);
        report += line;
    }
    std::size_t litShown = 0;
    for (const auto& [key, count] : programs) {
        if (!key.lighting)
            continue;
        char line[440];
        std::snprintf(
            line, sizeof(line),
            "; top-lit-program[%zu]=%llu normal=%u chan=%04x/%04x stages=%u tex=%04x/%04x "
            "order0=%02x/%02x/%02x stage0=%02x%02x%02x%02x%02x%02x%02x%02x "
            "order1=%02x/%02x/%02x stage1=%02x%02x%02x%02x%02x%02x%02x%02x",
            litShown, static_cast<unsigned long long>(count), key.hasNormal ? 1U : 0U,
            key.channelControl, key.alphaChannelControl, key.stageCount, key.textureNumber,
            key.textureNumber1, key.textureCoordinate, key.textureMap, key.colorChannel,
            key.stage[0], key.stage[1], key.stage[2], key.stage[3], key.stage[4], key.stage[5],
            key.stage[6], key.stage[7], key.textureCoordinate1, key.textureMap1, key.colorChannel1,
            key.stage1[0], key.stage1[1], key.stage1[2], key.stage1[3], key.stage1[4],
            key.stage1[5], key.stage1[6], key.stage1[7]);
        report += line;
        if (++litShown == 8)
            break;
    }
    return report;
}

void submit_semantic_j3d_shape(u32 shape) {
    if (!sb::native_render::has_semantic_sink())
        return;
    ++g_stats.shapeDraws;
    if (!readable(shape)) {
        ++g_stats.materialMemoryFailures;
        return;
    }

    J3DVertexLayout layout{};
    if (!j3d_build_layout(shape, layout)) {
        ++g_stats.layoutFailures;
        return;
    }
    const u32 materialPacket = sb_r32(kJ3dSys + kJ3dSysMaterialPacket);
    const u32 material =
        readable(materialPacket) ? sb_r32(materialPacket + kMaterialPacketMaterial) : 0;
    sb::native_render::J3dMaterialState materialState{};
    if (!sb::recomp::capture_guest_j3d_material_state(
            sb::recomp::live_guest_byte_reader(), material,
            layout.type[kColor0] !=
                static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::None),
            layout.type[kNormal] !=
                static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::None),
            materialState)) {
        ++g_stats.materialMemoryFailures;
        return;
    }
    ++g_programs[{.lighting = materialState.lightingEnabled,
                  .hasNormal = layout.type[kNormal] !=
                               static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::None),
                  .channelControl = materialState.colorChannelControl,
                  .alphaChannelControl = materialState.alphaChannelControl,
                  .stageCount = materialState.tevStageCount,
                  .textureNumber = materialState.textureNumber0,
                  .textureCoordinate = materialState.textureCoordinate0,
                  .textureMap = materialState.textureMap0,
                  .colorChannel = materialState.colorChannel0,
                  .stage = materialState.tevStage0,
                  .textureNumber1 = materialState.textureNumber1,
                  .textureCoordinate1 = materialState.textureCoordinate1,
                  .textureMap1 = materialState.textureMap1,
                  .colorChannel1 = materialState.colorChannel1,
                  .stage1 = materialState.tevStage1}];
    const sb::native_render::J3dUnlitMaterialFeatures features =
        sb::native_render::inspect_j3d_unlit_material(materialState);
    const bool otherwiseExact = features.supportedColorBlock && features.hasColorChannel &&
                                features.supportedTevBlock && features.singleTevStage &&
                                features.rasterColorPassThrough &&
                                features.requiredVertexColorPresent;
    if (otherwiseExact && !features.lightingEnabled && features.textureBound)
        ++g_stats.unlitTexturedCandidates;
    if (otherwiseExact && features.lightingEnabled && !features.textureBound)
        ++g_stats.litUntexturedCandidates;
    if (otherwiseExact && features.lightingEnabled && features.textureBound)
        ++g_stats.litTexturedCandidates;
    sb::native_render::ModelMaterial semanticMaterial{};
    bool submittedLitMaterial = false;
    std::array<sb::native_render::DecodedImageView, 1> semanticImages{};
    std::span<const sb::native_render::DecodedImageView> images;
    sb::native_render::UnlitColorMaterial colorMaterial{};
    const sb::native_render::J3dUnlitMaterialResult colorResult =
        sb::native_render::classify_j3d_unlit_material(materialState, colorMaterial);
    if (colorResult == sb::native_render::J3dUnlitMaterialResult::Success) {
        semanticMaterial = colorMaterial;
    } else {
        const sb::native_render::PictureTexture placeholder{.resource = 1, .width = 1, .height = 1};
        sb::native_render::UnlitTexturedMaterial texturedMaterial{};
        const sb::native_render::J3dUnlitTexturedResult unlitFamily =
            sb::native_render::classify_j3d_unlit_textured_material(materialState, placeholder,
                                                                    texturedMaterial);
        const sb::native_render::ModelLightingContext* lighting =
            sb::native_render::current_j3d_stage_lighting();
        sb::native_render::LitTexturedMaterial litMaterial{};
        const sb::native_render::J3dLitTexturedResult litFamily =
            lighting != nullptr ? sb::native_render::classify_j3d_lit_textured_material(
                                      materialState, placeholder, *lighting, litMaterial)
                                : sb::native_render::J3dLitTexturedResult::MissingLightingContext;
        const bool isUnlitTextured =
            unlitFamily == sb::native_render::J3dUnlitTexturedResult::Success;
        const bool isLitTextured = litFamily == sb::native_render::J3dLitTexturedResult::Success;
        if (!isUnlitTextured && !isLitTextured) {
            ++g_stats.materialRejections[static_cast<std::size_t>(colorResult)];
            ++g_stats.texturedMaterialRejections[static_cast<std::size_t>(unlitFamily)];
            ++g_stats.litTexturedMaterialRejections[static_cast<std::size_t>(litFamily)];
            return;
        }
        const u32 textureTable = sb_r32(materialPacket + kMaterialPacketTexture);
        if (!readable(textureTable)) {
            ++g_stats.textureTableFailures;
            return;
        }
        sb::native_render::ResTimgDecodeError textureError{};
        if (!sb::recomp::capture_guest_j3d_texture(sb::recomp::live_guest_byte_reader(),
                                                   textureTable, materialState.textureNumber0,
                                                   g_texture, textureError)) {
            const std::size_t errorIndex = static_cast<std::size_t>(textureError);
            if (errorIndex < g_stats.textureDecodeFailures.size())
                ++g_stats.textureDecodeFailures[errorIndex];
            return;
        }
        if (isLitTextured) {
            const sb::native_render::J3dLitTexturedResult classified =
                sb::native_render::classify_j3d_lit_textured_material(
                    materialState, g_texture.texture, *lighting, litMaterial);
            SB_ASSERT(classified == sb::native_render::J3dLitTexturedResult::Success,
                      "decoded J3D texture invalidated a preclassified lit material: result=%s",
                      sb::native_render::j3d_lit_textured_result_name(classified));
            semanticMaterial = litMaterial;
            submittedLitMaterial = true;
        } else {
            const sb::native_render::J3dUnlitTexturedResult classified =
                sb::native_render::classify_j3d_unlit_textured_material(
                    materialState, g_texture.texture, texturedMaterial);
            SB_ASSERT(classified == sb::native_render::J3dUnlitTexturedResult::Success,
                      "decoded J3D texture invalidated a preclassified material: result=%s",
                      sb::native_render::j3d_unlit_textured_result_name(classified));
            semanticMaterial = texturedMaterial;
        }
        semanticImages[0] = {g_texture.texture.resource, g_texture.texture.revision,
                             g_texture.texture.width, g_texture.texture.height, g_texture.rgba8};
        images = semanticImages;
    }

    const sb::native_render::ModelSceneContext* scene = sb::recomp::current_semantic_j3d_scene();
    if (scene == nullptr ||
        scene->projectionKind != sb::native_render::ProjectionKind::Perspective) {
        ++g_stats.noPerspectiveContexts;
        return;
    }
    const u32 matrixObjects = sb_r32(shape + kShapeMatrices);
    const u32 drawMatrices = sb_r32(shape + kShapeDrawMatrices);
    const u32 currentViewPointer = sb_r32(shape + kShapeCurrentView);
    const u32 currentView = readable(currentViewPointer) ? sb_r32(currentViewPointer) : 0;
    const u32 drawMatrixArray =
        readable(drawMatrices) && currentView <= 16 ? sb_r32(drawMatrices + currentView * 4) : 0;
    const u32 elementCount = sb_r16(shape + kShapeElementCount);
    if (!readable(matrixObjects) || !readable(drawMatrixArray) || elementCount == 0 ||
        elementCount >= 4096) {
        ++g_stats.nonRigidElements;
        return;
    }

    for (u32 element = 0; element < elementCount; ++element) {
        const u32 matrixObject = sb_r32(matrixObjects + element * 4);
        if (!readable(matrixObject) || sb_r32(matrixObject) != kBaseShapeMatrixVptr) {
            ++g_stats.nonRigidElements;
            continue;
        }
        const u32 matrixIndex = sb_r16(matrixObject + 4);
        const u32 matrixAddress = drawMatrixArray + matrixIndex * 48;
        if (!readable(matrixAddress) || !readable(matrixAddress + 47)) {
            ++g_stats.nonRigidElements;
            continue;
        }

        g_decoded.clear();
        if (!j3d_decode_element(shape, element, layout, g_decoded) || g_decoded.empty()) {
            ++g_stats.decodeFailures;
            continue;
        }
        if (std::ranges::any_of(
                g_decoded, [](const J3DVert& vertex) { return vertex.positionMatrixSlot != 0; })) {
            ++g_stats.nonRigidElements;
            continue;
        }

        g_vertices.clear();
        g_vertices.reserve(g_decoded.size());
        for (const J3DVert& vertex : g_decoded) {
            g_vertices.push_back({.position = {vertex.x, vertex.y, vertex.z},
                                  .uv = {vertex.uv[0][0], vertex.uv[0][1]},
                                  .color = sb::native_render::color_from_rgba8(vertex.rgba),
                                  .normal = {vertex.nx, vertex.ny, vertex.nz}});
        }
        const std::uint64_t resource = (static_cast<std::uint64_t>(shape) << 16U) | element;
        const std::uint64_t revision = sb::native_render::mesh_revision(g_vertices);
        sb::native_render::ModelDraw draw{};
        draw.instance = (static_cast<std::uint64_t>(shape) << 32U) | drawMatrixArray;
        draw.mesh = {resource, revision, static_cast<std::uint32_t>(g_vertices.size())};
        for (std::size_t index = 0; index < draw.modelView.value.size(); ++index)
            draw.modelView.value[index] = guest_f32(matrixAddress + index * 4);
        draw.projection = scene->projection;
        draw.material = semanticMaterial;
        const sb::native_render::MeshResourceView mesh{resource, revision, g_vertices};
        SB_ASSERT(sb::native_render::submit_model(draw, mesh, images),
                  "semantic J3D sink rejected validated rigid model: shape=%08x element=%u "
                  "vertices=%zu",
                  shape, element, g_vertices.size());
        record_raster(semanticMaterial);
        ++g_stats.submittedModels;
        if (submittedLitMaterial)
            ++g_stats.submittedLitModels;
        g_stats.submittedVertices += g_vertices.size();
    }
}
