#include "semantic_j3d_adapter.h"

#include "guest_j3d_texture_adapter.h"
#include "semantic_j3d_lighting.h"
#include "semantic_j3d_material_adapter.h"
#include "semantic_j3d_scene.h"

#include "../runtime/probe_server.h"
#include "../runtime/render/j3d_decode.h"
#include "../runtime/sb_assert.h"

#include <sunbright/native_render/j3d_alpha_masked_material.h>
#include <sunbright/native_render/j3d_effect_material.h>
#include <sunbright/native_render/j3d_layered_material.h>
#include <sunbright/native_render/j3d_lit_alpha_mask_material.h>
#include <sunbright/native_render/j3d_lit_alpha_tint_material.h>
#include <sunbright/native_render/j3d_lit_material.h>
#include <sunbright/native_render/j3d_masked_toon_material.h>
#include <sunbright/native_render/j3d_specular_material.h>
#include <sunbright/native_render/j3d_stage_lighting.h>
#include <sunbright/native_render/j3d_tinted_layered_material.h>
#include <sunbright/native_render/model_context.h>
#include <sunbright/native_render/semantic_sink.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr u32 kJ3dSys = 0x804045DC;
constexpr u32 kJ3dSysModel = 0x38;
constexpr u32 kJ3dSysMaterialPacket = 0x3C;
constexpr u32 kModelModelData = 0x04;
constexpr u32 kModelDataTextureNames = 0xA8;
constexpr u32 kModelDataMaterialNames = 0xB4;
constexpr u32 kMaterialIndex = 0x0C;
constexpr u32 kMaterialPacketMaterial = 0x38;
constexpr u32 kMaterialPacketTexture = 0x40;
constexpr u32 kShapeElementCount = 0x06;
constexpr u32 kShapeMatrices = 0x34;
constexpr u32 kShapeDrawMatrixData = 0x48;
constexpr u32 kShapeDrawMatrices = 0x50;
constexpr u32 kShapeCurrentView = 0x58;
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
    std::array<std::uint64_t, 12> litAlphaTintMaterialRejections{};
    std::array<std::uint64_t, 11> effectMaterialRejections{};
    std::array<std::uint64_t, 11> effectCandidateRejections{};
    std::array<std::uint64_t, 12> tintedLayeredMaterialRejections{};
    std::array<std::uint64_t, 15> maskedToonMaterialRejections{};
    std::array<std::uint64_t, 13> specularTexturedMaterialRejections{};
    std::uint64_t submittedModels = 0;
    std::uint64_t submittedLitModels = 0;
    std::uint64_t submittedAlphaMaskedModels = 0;
    std::uint64_t submittedVertices = 0;
    std::uint64_t unlitTexturedCandidates = 0;
    std::uint64_t litUntexturedCandidates = 0;
    std::uint64_t litTexturedCandidates = 0;
    std::uint64_t programNameAttempts = 0;
    std::uint64_t programNameFailures = 0;
    std::array<std::uint64_t, 4> rasterFamilies{};
    std::array<std::uint64_t, 4> cullModes{};
};

struct ProgramKey {
    bool lighting = false;
    bool hasNormal = false;
    std::uint8_t colorChannelCount = 0;
    std::uint16_t channelControl = 0;
    std::uint16_t alphaChannelControl = 0;
    std::uint16_t channelControl1 = 0;
    std::uint16_t alphaChannelControl1 = 0;
    std::uint8_t cullMode = 0xFF;
    std::uint32_t pixelEngineBlockType = 0;
    bool hasExplicitPixelPolicy = false;
    std::uint8_t alphaCompare0 = 0;
    std::uint8_t alphaReference0 = 0;
    std::uint8_t alphaOperation = 0;
    std::uint8_t alphaCompare1 = 0;
    std::uint8_t alphaReference1 = 0;
    std::uint8_t blendMode = 0;
    std::uint8_t blendSourceFactor = 0;
    std::uint8_t blendDestinationFactor = 0;
    std::uint8_t blendLogicOperation = 0;
    bool depthTest = false;
    std::uint8_t depthCompare = 0;
    bool depthWrite = false;
    std::uint8_t fogType = 0;
    bool fogRangeAdjustmentEnabled = false;
    std::uint32_t modelData = 0;
    std::uint16_t materialIndex = 0xFFFF;
    std::uint8_t stageCount = 0;
    std::array<sb::native_render::J3dTextureBinding, sb::native_render::kMaxJ3dTextureMaps>
        textureBindings{};
    std::array<sb::native_render::J3dTevStageState, sb::native_render::kMaxJ3dTevStages>
        tevStages{};
    auto operator<=>(const ProgramKey&) const = default;
};

struct ProgramObservation {
    std::uint64_t count = 0;
    std::uint64_t perspectiveObserved = 0;
    std::uint64_t materialAccepted = 0;
    std::uint64_t resourcesReady = 0;
    std::uint64_t perspectiveReady = 0;
    std::uint64_t submittedModels = 0;
    std::string materialName;
    std::array<std::string, sb::native_render::kMaxJ3dTextureMaps> textureNames;
    std::uint32_t firstMaterialColor = 0;
    std::uint32_t firstAmbientColor = 0;
    std::uint32_t firstMaterialColor1 = 0;
    std::uint32_t firstAmbientColor1 = 0;
    std::array<std::uint32_t, 4> firstKonstColors{};
    std::array<std::array<std::int16_t, 4>, sb::native_render::kJ3dTevColorRegisters>
        firstTevColors{};
    sb::native_render::J3dFogState firstFog{};
    bool primaryColorsVary = false;
    bool secondaryColorsVary = false;
    bool konstColorsVary = false;
    bool tevColorsVary = false;
    bool fogValuesVary = false;
};

Stats g_stats{};
std::map<ProgramKey, ProgramObservation> g_programs;
std::vector<J3DVert> g_decoded;
std::vector<sb::native_render::MeshVertex> g_vertices;
std::array<sb::native_render::DecodedTexture, 4> g_textures;

bool readable(u32 address) {
    return sb_ram_fast(address) != nullptr;
}

std::string read_j3d_name(const sb::recomp::BigEndianGuestReader& reader, u32 nameTab,
                          std::uint16_t index) {
    if (index == 0xFFFFU)
        return "<none>";
    ++g_stats.programNameAttempts;
    if (nameTab == 0) {
        ++g_stats.programNameFailures;
        return "<unavailable>";
    }
    u32 resource = 0;
    std::uint16_t count = 0;
    if (!reader.u32(nameTab, resource) || resource == 0 || !reader.u16(resource, count) ||
        index >= count) {
        ++g_stats.programNameFailures;
        return "<unavailable>";
    }
    std::uint16_t nameOffset = 0;
    if (!reader.u16(resource + 4U + static_cast<u32>(index) * 4U + 2U, nameOffset)) {
        ++g_stats.programNameFailures;
        return "<unavailable>";
    }
    std::array<char, 64> name{};
    for (std::size_t character = 0; character + 1 < name.size(); ++character) {
        std::uint8_t byte = 0;
        if (!reader.u8(resource + nameOffset + static_cast<u32>(character), byte)) {
            ++g_stats.programNameFailures;
            return "<unavailable>";
        }
        if (byte == 0)
            return std::string(name.data(), character);
        name[character] = static_cast<char>(byte);
    }
    ++g_stats.programNameFailures;
    return "<name-exceeds-63-bytes>";
}

void capture_program_owner(u32 material, ProgramKey& key) {
    const sb::recomp::BigEndianGuestReader reader(sb::recomp::live_guest_byte_reader());
    u32 model = 0;
    if (!reader.u16(material + kMaterialIndex, key.materialIndex) ||
        !reader.u32(kJ3dSys + kJ3dSysModel, model) || model == 0 ||
        !reader.u32(model + kModelModelData, key.modelData)) {
        key.modelData = 0;
    }
}

void capture_program_names(const ProgramKey& key, ProgramObservation& observation) {
    const sb::recomp::BigEndianGuestReader reader(sb::recomp::live_guest_byte_reader());
    u32 materialNames = 0;
    u32 textureNames = 0;
    if (key.modelData == 0 || !reader.u32(key.modelData + kModelDataMaterialNames, materialNames) ||
        !reader.u32(key.modelData + kModelDataTextureNames, textureNames)) {
        const std::uint64_t unavailableNames =
            1U + std::ranges::count_if(key.textureBindings, [](const auto& binding) {
                return binding.textureNumber != 0xFFFFU;
            });
        g_stats.programNameAttempts += unavailableNames;
        g_stats.programNameFailures += unavailableNames;
        observation.materialName = "<unavailable>";
        for (std::size_t textureMap = 0; textureMap < key.textureBindings.size(); ++textureMap)
            observation.textureNames[textureMap] =
                key.textureBindings[textureMap].textureNumber == 0xFFFFU ? "<none>"
                                                                         : "<unavailable>";
        return;
    }
    observation.materialName = read_j3d_name(reader, materialNames, key.materialIndex);
    for (std::size_t textureMap = 0; textureMap < key.textureBindings.size(); ++textureMap)
        observation.textureNames[textureMap] =
            read_j3d_name(reader, textureNames, key.textureBindings[textureMap].textureNumber);
}

void record_raster(const sb::native_render::ModelMaterial& material) {
    const sb::native_render::ModelRasterPolicy& raster = sb::native_render::raster_policy(material);
    std::size_t family = 0;
    if (raster.alphaTest == sb::native_render::ModelAlphaTest::GreaterOrEqualHalf)
        family = 1;
    else if (raster.blend == sb::native_render::ModelBlendMode::SourceAlpha)
        family = 2;
    else if (raster.blend == sb::native_render::ModelBlendMode::Additive)
        family = 3;
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
        "(%llu lit models, %llu solid-colour alpha-mask models); "
        "unreadable=%llu layout=%llu no-perspective-context=%llu non-rigid=%llu decode=%llu "
        "texture-table=%llu texture-decode=%llu; material "
        "rejections: colour-block=%llu lighting=%llu missing-channel=%llu texture=%llu "
        "tev-family=%llu multi-stage=%llu colour-program=%llu missing-vertex-colour=%llu "
        "raster-policy=%llu textured-raster-policy=%llu; published raster families: opaque=%llu "
        "cutout=%llu translucent=%llu additive=%llu "
        "cull(none=%llu front=%llu back=%llu all=%llu); "
        "exact next-family candidates: unlit+textured=%llu lit+untextured=%llu "
        "lit+textured=%llu",
        static_cast<unsigned long long>(g_stats.shapeDraws),
        static_cast<unsigned long long>(g_stats.submittedModels),
        static_cast<unsigned long long>(g_stats.submittedVertices),
        static_cast<unsigned long long>(g_stats.submittedLitModels),
        static_cast<unsigned long long>(g_stats.submittedAlphaMaskedModels),
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
        static_cast<unsigned long long>(g_stats.rasterFamilies[3]),
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
    char lightingLine[352];
    std::snprintf(lightingLine, sizeof(lightingLine),
                  "; high-level stage lights: published=%llu/%llu failures(view=%llu "
                  "shininess=%llu primary-position=%llu manager=%llu effect=%llu); program-name "
                  "failures=%llu/%llu",
                  static_cast<unsigned long long>(lightingStats.published),
                  static_cast<unsigned long long>(lightingStats.attempts),
                  static_cast<unsigned long long>(lightingStats.viewFailures),
                  static_cast<unsigned long long>(lightingStats.shininessFailures),
                  static_cast<unsigned long long>(lightingStats.primaryPositionFailures),
                  static_cast<unsigned long long>(lightingStats.managerFailures),
                  static_cast<unsigned long long>(lightingStats.effectFailures),
                  static_cast<unsigned long long>(g_stats.programNameFailures),
                  static_cast<unsigned long long>(g_stats.programNameAttempts));
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
    char litAlphaTintRejectionLine[420];
    std::snprintf(litAlphaTintRejectionLine, sizeof(litAlphaTintRejectionLine),
                  "; lit-alpha-tint rejections: colour-block=%llu channels=%llu tev-block=%llu "
                  "stages=%llu texcoord=%llu binding=%llu program=%llu tev-colour=%llu normal=%llu "
                  "lighting-context=%llu raster=%llu",
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[1]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[2]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[3]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[4]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[5]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[6]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[7]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[8]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[9]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[10]),
                  static_cast<unsigned long long>(g_stats.litAlphaTintMaterialRejections[11]));
    report += litAlphaTintRejectionLine;
    char tintedLayeredRejectionLine[360];
    std::snprintf(
        tintedLayeredRejectionLine, sizeof(tintedLayeredRejectionLine),
        "; tinted-layered rejections: colour-block=%llu channels=%llu secondary-colours=%llu "
        "tev-block=%llu stages=%llu texcoord=%llu binding=%llu program=%llu normal=%llu "
        "lighting-context=%llu raster=%llu",
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[1]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[2]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[3]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[4]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[5]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[6]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[7]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[8]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[9]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[10]),
        static_cast<unsigned long long>(g_stats.tintedLayeredMaterialRejections[11]));
    report += tintedLayeredRejectionLine;
    char effectRejectionLine[420];
    std::snprintf(effectRejectionLine, sizeof(effectRejectionLine),
                  "; effect-material rejections: colour-block=%llu channels=%llu tev-block=%llu "
                  "stages=%llu texcoord=%llu binding=%llu program=%llu tev-colour=%llu normal=%llu "
                  "raster=%llu",
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[1]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[2]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[3]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[4]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[5]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[6]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[7]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[8]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[9]),
                  static_cast<unsigned long long>(g_stats.effectMaterialRejections[10]));
    report += effectRejectionLine;
    char effectCandidateLine[420];
    std::snprintf(
        effectCandidateLine, sizeof(effectCandidateLine),
        "; effect-shape candidate rejections: colour-block=%llu channels=%llu tev-block=%llu "
        "stages=%llu texcoord=%llu binding=%llu program=%llu tev-colour=%llu normal=%llu "
        "raster=%llu",
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[1]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[2]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[3]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[4]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[5]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[6]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[7]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[8]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[9]),
        static_cast<unsigned long long>(g_stats.effectCandidateRejections[10]));
    report += effectCandidateLine;
    char maskedToonRejectionLine[512];
    std::snprintf(
        maskedToonRejectionLine, sizeof(maskedToonRejectionLine),
        "; masked-toon rejections: colour-block=%llu channels=%llu secondary-colours=%llu "
        "tev-block=%llu stages=%llu texcoord=%llu binding=%llu program=%llu register-colour=%llu "
        "normal=%llu lighting-context=%llu raster=%llu missing-binding=%llu resource=%llu",
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[1]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[2]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[3]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[4]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[5]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[6]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[7]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[8]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[9]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[10]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[11]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[12]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[13]),
        static_cast<unsigned long long>(g_stats.maskedToonMaterialRejections[14]));
    report += maskedToonRejectionLine;
    char specularRejectionLine[480];
    std::snprintf(
        specularRejectionLine, sizeof(specularRejectionLine),
        "; specular-textured rejections: colour-block=%llu channels=%llu secondary-colours=%llu "
        "tev-block=%llu stages=%llu texcoord=%llu binding=%llu program=%llu normal=%llu "
        "lighting-context=%llu raster=%llu",
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[1]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[2]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[3]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[4]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[5]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[6]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[7]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[8]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[9]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[10]),
        static_cast<unsigned long long>(g_stats.specularTexturedMaterialRejections[11]));
    report += specularRejectionLine;
    std::vector<std::pair<ProgramKey, ProgramObservation>> programs(g_programs.begin(),
                                                                    g_programs.end());
    std::ranges::sort(programs, [](const auto& first, const auto& second) {
        return first.second.count > second.second.count;
    });
    const std::size_t shown = std::min<std::size_t>(programs.size(), 8);
    for (std::size_t index = 0; index < shown; ++index) {
        const ProgramKey& key = programs[index].first;
        const ProgramObservation& observation = programs[index].second;
        char line[768];
        std::snprintf(
            line, sizeof(line),
            "; top-program[%zu]=%llu mat=%u:\"%s\" lit=%u normal=%u chan=%04x/%04x "
            "stages=%u tex=%04x:\"%s\" order=%02x/%02x/%02x "
            "stage=%02x%02x%02x%02x%02x%02x%02x%02x path=%llu/%llu/%llu/%llu",
            index, static_cast<unsigned long long>(observation.count), key.materialIndex,
            observation.materialName.c_str(), key.lighting ? 1U : 0U, key.hasNormal ? 1U : 0U,
            key.channelControl, key.alphaChannelControl, key.stageCount,
            key.textureBindings[0].textureNumber, observation.textureNames[0].c_str(),
            key.tevStages[0].textureCoordinate, key.tevStages[0].textureMap,
            key.tevStages[0].colorChannel, key.tevStages[0].program[0], key.tevStages[0].program[1],
            key.tevStages[0].program[2], key.tevStages[0].program[3], key.tevStages[0].program[4],
            key.tevStages[0].program[5], key.tevStages[0].program[6], key.tevStages[0].program[7],
            static_cast<unsigned long long>(observation.perspectiveObserved),
            static_cast<unsigned long long>(observation.materialAccepted),
            static_cast<unsigned long long>(observation.resourcesReady),
            static_cast<unsigned long long>(observation.submittedModels));
        report += line;
    }
    std::vector<std::pair<ProgramKey, ProgramObservation>> litPrograms;
    std::ranges::copy_if(programs, std::back_inserter(litPrograms),
                         [](const auto& program) { return program.first.lighting; });
    std::ranges::sort(litPrograms, [](const auto& first, const auto& second) {
        if (first.second.perspectiveObserved != second.second.perspectiveObserved)
            return first.second.perspectiveObserved > second.second.perspectiveObserved;
        return first.second.count > second.second.count;
    });
    std::size_t litCount = std::min<std::size_t>(litPrograms.size(), 16);
    if (litCount != 0 && litCount < litPrograms.size()) {
        const std::uint64_t cutoffPerspectiveCount =
            litPrograms[litCount - 1].second.perspectiveObserved;
        while (cutoffPerspectiveCount != 0 && litCount < litPrograms.size() &&
               litPrograms[litCount].second.perspectiveObserved == cutoffPerspectiveCount) {
            ++litCount;
        }
    }
    for (std::size_t litIndex = 0; litIndex < litCount; ++litIndex) {
        const auto& [key, observation] = litPrograms[litIndex];
        char line[3072];
        std::snprintf(
            line, sizeof(line),
            "; top-lit-program[%zu]=%llu mat=%u:\"%s\" normal=%u "
            "channels=%u:%04x/%04x,%04x/%04x stages=%u "
            "pe=%08x cull=%u explicit=%u alpha=%u/%u/%u/%u/%u "
            "blend=%u/%u/%u/%u depth=%u/%u/%u fog=%u/%u "
            "fogRange=%.1f/%.1f/%.1f/%.1f fogColor=%08x varies=%u "
            "tex0=%04x:\"%s\" tex1=%04x:\"%s\" tex2=%04x:\"%s\" tex3=%04x:\"%s\" "
            "tex4=%04x:\"%s\" tex5=%04x:\"%s\" tex6=%04x:\"%s\" tex7=%04x:\"%s\" "
            "order0=%02x/%02x/%02x stage0=%02x%02x%02x%02x%02x%02x%02x%02x "
            "order1=%02x/%02x/%02x stage1=%02x%02x%02x%02x%02x%02x%02x%02x "
            "order2=%02x/%02x/%02x stage2=%02x%02x%02x%02x%02x%02x%02x%02x "
            "order3=%02x/%02x/%02x stage3=%02x%02x%02x%02x%02x%02x%02x%02x "
            "order4=%02x/%02x/%02x stage4=%02x%02x%02x%02x%02x%02x%02x%02x "
            "path=%llu-observed/%llu-perspective/%llu-accepted/%llu-resources/%llu-ready/"
            "%llu-models "
            "primaryColor=%08x/%08x varies=%u "
            "secondColor=%08x/%08x varies=%u "
            "tevColor0=%d/%d/%d/%d tevColor1=%d/%d/%d/%d tevColor2=%d/%d/%d/%d varies=%u "
            "konstSel=%02x/%02x/%02x/%02x/%02x alphaSel=%02x/%02x/%02x/%02x/%02x "
            "konst=%08x/%08x/%08x/%08x varies=%u",
            litIndex, static_cast<unsigned long long>(observation.count), key.materialIndex,
            observation.materialName.c_str(), key.hasNormal ? 1U : 0U, key.colorChannelCount,
            key.channelControl, key.alphaChannelControl, key.channelControl1,
            key.alphaChannelControl1, key.stageCount, key.pixelEngineBlockType, key.cullMode,
            key.hasExplicitPixelPolicy ? 1U : 0U, key.alphaCompare0, key.alphaReference0,
            key.alphaOperation, key.alphaCompare1, key.alphaReference1, key.blendMode,
            key.blendSourceFactor, key.blendDestinationFactor, key.blendLogicOperation,
            key.depthTest ? 1U : 0U, key.depthCompare, key.depthWrite ? 1U : 0U, key.fogType,
            key.fogRangeAdjustmentEnabled ? 1U : 0U, observation.firstFog.start,
            observation.firstFog.end, observation.firstFog.near, observation.firstFog.far,
            observation.firstFog.colorRgba8, observation.fogValuesVary ? 1U : 0U,
            key.textureBindings[0].textureNumber, observation.textureNames[0].c_str(),
            key.textureBindings[1].textureNumber, observation.textureNames[1].c_str(),
            key.textureBindings[2].textureNumber, observation.textureNames[2].c_str(),
            key.textureBindings[3].textureNumber, observation.textureNames[3].c_str(),
            key.textureBindings[4].textureNumber, observation.textureNames[4].c_str(),
            key.textureBindings[5].textureNumber, observation.textureNames[5].c_str(),
            key.textureBindings[6].textureNumber, observation.textureNames[6].c_str(),
            key.textureBindings[7].textureNumber, observation.textureNames[7].c_str(),
            key.tevStages[0].textureCoordinate, key.tevStages[0].textureMap,
            key.tevStages[0].colorChannel, key.tevStages[0].program[0], key.tevStages[0].program[1],
            key.tevStages[0].program[2], key.tevStages[0].program[3], key.tevStages[0].program[4],
            key.tevStages[0].program[5], key.tevStages[0].program[6], key.tevStages[0].program[7],
            key.tevStages[1].textureCoordinate, key.tevStages[1].textureMap,
            key.tevStages[1].colorChannel, key.tevStages[1].program[0], key.tevStages[1].program[1],
            key.tevStages[1].program[2], key.tevStages[1].program[3], key.tevStages[1].program[4],
            key.tevStages[1].program[5], key.tevStages[1].program[6], key.tevStages[1].program[7],
            key.tevStages[2].textureCoordinate, key.tevStages[2].textureMap,
            key.tevStages[2].colorChannel, key.tevStages[2].program[0], key.tevStages[2].program[1],
            key.tevStages[2].program[2], key.tevStages[2].program[3], key.tevStages[2].program[4],
            key.tevStages[2].program[5], key.tevStages[2].program[6], key.tevStages[2].program[7],
            key.tevStages[3].textureCoordinate, key.tevStages[3].textureMap,
            key.tevStages[3].colorChannel, key.tevStages[3].program[0], key.tevStages[3].program[1],
            key.tevStages[3].program[2], key.tevStages[3].program[3], key.tevStages[3].program[4],
            key.tevStages[3].program[5], key.tevStages[3].program[6], key.tevStages[3].program[7],
            key.tevStages[4].textureCoordinate, key.tevStages[4].textureMap,
            key.tevStages[4].colorChannel, key.tevStages[4].program[0], key.tevStages[4].program[1],
            key.tevStages[4].program[2], key.tevStages[4].program[3], key.tevStages[4].program[4],
            key.tevStages[4].program[5], key.tevStages[4].program[6], key.tevStages[4].program[7],
            static_cast<unsigned long long>(observation.count),
            static_cast<unsigned long long>(observation.perspectiveObserved),
            static_cast<unsigned long long>(observation.materialAccepted),
            static_cast<unsigned long long>(observation.resourcesReady),
            static_cast<unsigned long long>(observation.perspectiveReady),
            static_cast<unsigned long long>(observation.submittedModels),
            observation.firstMaterialColor, observation.firstAmbientColor,
            observation.primaryColorsVary ? 1U : 0U, observation.firstMaterialColor1,
            observation.firstAmbientColor1, observation.secondaryColorsVary ? 1U : 0U,
            observation.firstTevColors[0][0], observation.firstTevColors[0][1],
            observation.firstTevColors[0][2], observation.firstTevColors[0][3],
            observation.firstTevColors[1][0], observation.firstTevColors[1][1],
            observation.firstTevColors[1][2], observation.firstTevColors[1][3],
            observation.firstTevColors[2][0], observation.firstTevColors[2][1],
            observation.firstTevColors[2][2], observation.firstTevColors[2][3],
            observation.tevColorsVary ? 1U : 0U, key.tevStages[0].konstColorSelection,
            key.tevStages[1].konstColorSelection, key.tevStages[2].konstColorSelection,
            key.tevStages[3].konstColorSelection, key.tevStages[4].konstColorSelection,
            key.tevStages[0].konstAlphaSelection, key.tevStages[1].konstAlphaSelection,
            key.tevStages[2].konstAlphaSelection, key.tevStages[3].konstAlphaSelection,
            key.tevStages[4].konstAlphaSelection, observation.firstKonstColors[0],
            observation.firstKonstColors[1], observation.firstKonstColors[2],
            observation.firstKonstColors[3], observation.konstColorsVary ? 1U : 0U);
        report += line;
    }
    std::vector<std::pair<ProgramKey, ProgramObservation>> rejectedLitPrograms;
    std::ranges::copy_if(
        programs, std::back_inserter(rejectedLitPrograms), [](const auto& program) {
            const ProgramObservation& observation = program.second;
            return program.first.lighting && observation.count > observation.materialAccepted &&
                   observation.perspectiveObserved != 0;
        });
    std::ranges::sort(rejectedLitPrograms, [](const auto& first, const auto& second) {
        if (first.second.perspectiveObserved != second.second.perspectiveObserved)
            return first.second.perspectiveObserved > second.second.perspectiveObserved;
        return first.second.count > second.second.count;
    });
    const std::size_t rejectedLitCount = std::min<std::size_t>(rejectedLitPrograms.size(), 8);
    for (std::size_t rejectedIndex = 0; rejectedIndex < rejectedLitCount; ++rejectedIndex) {
        const auto& [key, observation] = rejectedLitPrograms[rejectedIndex];
        char line[512];
        std::snprintf(
            line, sizeof(line),
            "; top-rejected-lit[%zu]=%llu rejected=%llu perspective=%llu mat=%u:%s "
            "channels=%u:%04x/%04x,%04x/%04x stages=%u tex0=%04x "
            "stage0=%02x%02x%02x%02x%02x%02x%02x%02x",
            rejectedIndex, static_cast<unsigned long long>(observation.count),
            static_cast<unsigned long long>(observation.count - observation.materialAccepted),
            static_cast<unsigned long long>(observation.perspectiveObserved), key.materialIndex,
            observation.materialName.c_str(), key.colorChannelCount, key.channelControl,
            key.alphaChannelControl, key.channelControl1, key.alphaChannelControl1, key.stageCount,
            key.textureBindings[0].textureNumber, key.tevStages[0].program[0],
            key.tevStages[0].program[1], key.tevStages[0].program[2], key.tevStages[0].program[3],
            key.tevStages[0].program[4], key.tevStages[0].program[5], key.tevStages[0].program[6],
            key.tevStages[0].program[7]);
        report += line;
    }
    return report;
}

void submit_semantic_j3d_shape(u32 shape, std::span<const GuestJ3dMatrixBinding> matrices) {
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
    ProgramKey program{.lighting = materialState.lightingEnabled,
                       .hasNormal =
                           layout.type[kNormal] !=
                           static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::None),
                       .colorChannelCount = materialState.colorChannelCount,
                       .channelControl = materialState.colorChannelControl,
                       .alphaChannelControl = materialState.alphaChannelControl,
                       .channelControl1 = materialState.colorChannelControl1,
                       .alphaChannelControl1 = materialState.alphaChannelControl1,
                       .cullMode = materialState.cullMode,
                       .pixelEngineBlockType = materialState.pixelEngineBlockType,
                       .hasExplicitPixelPolicy = materialState.hasExplicitPixelPolicy,
                       .alphaCompare0 = materialState.alphaCompare0,
                       .alphaReference0 = materialState.alphaReference0,
                       .alphaOperation = materialState.alphaOperation,
                       .alphaCompare1 = materialState.alphaCompare1,
                       .alphaReference1 = materialState.alphaReference1,
                       .blendMode = materialState.blendMode,
                       .blendSourceFactor = materialState.blendSourceFactor,
                       .blendDestinationFactor = materialState.blendDestinationFactor,
                       .blendLogicOperation = materialState.blendLogicOperation,
                       .depthTest = materialState.depthTest,
                       .depthCompare = materialState.depthCompare,
                       .depthWrite = materialState.depthWrite,
                       .fogType = materialState.fog.type,
                       .fogRangeAdjustmentEnabled = materialState.fog.rangeAdjustmentEnabled,
                       .stageCount = materialState.tevStageCount,
                       .textureBindings = materialState.textureBindings,
                       .tevStages = materialState.tevStages};
    capture_program_owner(material, program);
    auto [programEntry, inserted] = g_programs.try_emplace(program);
    const ProgramKey& capturedProgram = programEntry->first;
    ProgramObservation& observation = programEntry->second;
    if (inserted) {
        capture_program_names(capturedProgram, observation);
        observation.firstMaterialColor = materialState.materialColorRgba8;
        observation.firstAmbientColor = materialState.ambientColorRgba8;
        observation.firstMaterialColor1 = materialState.materialColor1Rgba8;
        observation.firstAmbientColor1 = materialState.ambientColor1Rgba8;
        observation.firstKonstColors = materialState.konstColorRgba8;
        observation.firstTevColors = materialState.tevColorsS10;
        observation.firstFog = materialState.fog;
    } else {
        if (observation.firstMaterialColor != materialState.materialColorRgba8 ||
            observation.firstAmbientColor != materialState.ambientColorRgba8) {
            observation.primaryColorsVary = true;
        }
        if (observation.firstMaterialColor1 != materialState.materialColor1Rgba8 ||
            observation.firstAmbientColor1 != materialState.ambientColor1Rgba8) {
            observation.secondaryColorsVary = true;
        }
        if (observation.firstKonstColors != materialState.konstColorRgba8)
            observation.konstColorsVary = true;
        if (observation.firstTevColors != materialState.tevColorsS10)
            observation.tevColorsVary = true;
        if (observation.firstFog != materialState.fog)
            observation.fogValuesVary = true;
    }
    ++observation.count;
    const sb::native_render::ModelSceneContext* scene = sb::recomp::current_semantic_j3d_scene();
    if (scene != nullptr &&
        scene->projectionKind == sb::native_render::ProjectionKind::Perspective) {
        ++observation.perspectiveObserved;
    }
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
    bool submittedAlphaMaskedMaterial = false;
    std::array<sb::native_render::DecodedImageView, 4> semanticImages{};
    std::span<const sb::native_render::DecodedImageView> images;
    sb::native_render::UnlitColorMaterial colorMaterial{};
    const sb::native_render::J3dUnlitMaterialResult colorResult =
        sb::native_render::classify_j3d_unlit_material(materialState, colorMaterial);
    const sb::native_render::ModelLightingContext* lighting =
        sb::native_render::current_j3d_stage_lighting();
    sb::native_render::LitColorMaterial litColorMaterial{};
    const sb::native_render::J3dLitColorResult litColorResult =
        lighting != nullptr ? sb::native_render::classify_j3d_lit_color_material(
                                  materialState, *lighting, litColorMaterial)
                            : sb::native_render::J3dLitColorResult::MissingLightingContext;
    sb::native_render::LitSpecularColorMaterial specularColorMaterial{};
    const sb::native_render::J3dSpecularColorResult specularColorResult =
        lighting != nullptr ? sb::native_render::classify_j3d_specular_color_material(
                                  materialState, *lighting, specularColorMaterial)
                            : sb::native_render::J3dSpecularColorResult::MissingLightingContext;
    if (colorResult == sb::native_render::J3dUnlitMaterialResult::Success) {
        semanticMaterial = colorMaterial;
        ++observation.materialAccepted;
    } else if (litColorResult == sb::native_render::J3dLitColorResult::Success) {
        semanticMaterial = litColorMaterial;
        submittedLitMaterial = true;
        ++observation.materialAccepted;
    } else if (specularColorResult == sb::native_render::J3dSpecularColorResult::Success) {
        semanticMaterial = specularColorMaterial;
        submittedLitMaterial = true;
        ++observation.materialAccepted;
    } else {
        const sb::native_render::PictureTexture placeholder{.resource = 1, .width = 1, .height = 1};
        sb::native_render::UnlitTexturedMaterial texturedMaterial{};
        const sb::native_render::J3dUnlitTexturedResult unlitFamily =
            sb::native_render::classify_j3d_unlit_textured_material(materialState, placeholder,
                                                                    texturedMaterial);
        sb::native_render::LitTexturedMaterial litMaterial{};
        const sb::native_render::J3dLitTexturedResult litFamily =
            lighting != nullptr ? sb::native_render::classify_j3d_lit_textured_material(
                                      materialState, placeholder, *lighting, litMaterial)
                                : sb::native_render::J3dLitTexturedResult::MissingLightingContext;
        sb::native_render::TexturedEffectMaterial effectMaterial{};
        const sb::native_render::J3dEffectMaterialResult effectFamily =
            lighting != nullptr
                ? sb::native_render::classify_j3d_effect_material(materialState, placeholder,
                                                                  effectMaterial)
                : sb::native_render::J3dEffectMaterialResult::UnsupportedColorChannels;
        const bool isUnlitTextured =
            unlitFamily == sb::native_render::J3dUnlitTexturedResult::Success;
        sb::native_render::AlphaMaskedColorMaterial alphaMaskedMaterial{};
        const sb::native_render::J3dAlphaMaskedMaterialResult alphaMaskedFamily =
            sb::native_render::classify_j3d_alpha_masked_material(materialState, placeholder,
                                                                  alphaMaskedMaterial);
        const bool isAlphaMasked =
            alphaMaskedFamily == sb::native_render::J3dAlphaMaskedMaterialResult::Success;
        const bool isLitTextured = litFamily == sb::native_render::J3dLitTexturedResult::Success;
        const bool isEffect = effectFamily == sb::native_render::J3dEffectMaterialResult::Success;
        const bool effectShapeCandidate =
            materialState.lightingEnabled && materialState.colorChannelCount == 1 &&
            materialState.colorChannelControl == 0x0706 &&
            materialState.alphaChannelControl == 0x0700 && materialState.tevStageCount == 1 &&
            sb::native_render::is_j3d_effect_material_program(materialState.tevStages[0]);
        if (effectShapeCandidate && !isEffect)
            ++g_stats.effectCandidateRejections[static_cast<std::size_t>(effectFamily)];
        sb::native_render::LitTexturedAlphaMaskMaterial litAlphaMaskMaterial{};
        const sb::native_render::J3dLitAlphaMaskResult litAlphaMaskFamily =
            lighting != nullptr
                ? sb::native_render::classify_j3d_lit_alpha_mask_material(
                      materialState, placeholder, placeholder, *lighting, litAlphaMaskMaterial)
                : sb::native_render::J3dLitAlphaMaskResult::MissingLightingContext;
        const bool isLitAlphaMask =
            litAlphaMaskFamily == sb::native_render::J3dLitAlphaMaskResult::Success;
        sb::native_render::LitAlphaTintMaterial litAlphaTintMaterial{};
        const sb::native_render::J3dLitAlphaTintResult litAlphaTintFamily =
            lighting != nullptr ? sb::native_render::classify_j3d_lit_alpha_tint_material(
                                      materialState, placeholder, *lighting, litAlphaTintMaterial)
                                : sb::native_render::J3dLitAlphaTintResult::MissingLightingContext;
        const bool isLitAlphaTint =
            litAlphaTintFamily == sb::native_render::J3dLitAlphaTintResult::Success;
        sb::native_render::LitLayeredTexturedMaterial layeredMaterial{};
        const sb::native_render::J3dLayeredMaterialResult layeredFamily =
            lighting != nullptr
                ? sb::native_render::classify_j3d_layered_material(
                      materialState, placeholder, placeholder, *lighting, layeredMaterial)
                : sb::native_render::J3dLayeredMaterialResult::MissingLightingContext;
        const bool isLayered =
            layeredFamily == sb::native_render::J3dLayeredMaterialResult::Success;
        sb::native_render::LitTintedLayeredSpecularMaterial tintedLayeredMaterial{};
        const sb::native_render::J3dTintedLayeredMaterialResult tintedLayeredFamily =
            lighting != nullptr
                ? sb::native_render::classify_j3d_tinted_layered_material(
                      materialState, placeholder, placeholder, *lighting, tintedLayeredMaterial)
                : sb::native_render::J3dTintedLayeredMaterialResult::MissingLightingContext;
        const bool isTintedLayered =
            tintedLayeredFamily == sb::native_render::J3dTintedLayeredMaterialResult::Success;
        sb::native_render::LitMaskedToonMaterial maskedToonMaterial{};
        const sb::native_render::J3dMaskedToonMaterialResult maskedToonFamily =
            lighting != nullptr
                ? sb::native_render::classify_j3d_masked_toon_material(
                      materialState, placeholder, placeholder, placeholder, placeholder, *lighting,
                      maskedToonMaterial)
                : sb::native_render::J3dMaskedToonMaterialResult::MissingLightingContext;
        const bool isMaskedToon =
            maskedToonFamily == sb::native_render::J3dMaskedToonMaterialResult::Success;
        sb::native_render::LitSpecularTexturedMaterial specularMaterial{};
        const sb::native_render::J3dSpecularTexturedResult specularFamily =
            lighting != nullptr
                ? sb::native_render::classify_j3d_specular_textured_material(
                      materialState, placeholder, *lighting, specularMaterial)
                : sb::native_render::J3dSpecularTexturedResult::MissingLightingContext;
        const bool isSpecularTextured =
            specularFamily == sb::native_render::J3dSpecularTexturedResult::Success;
        if (!isUnlitTextured && !isAlphaMasked && !isLitTextured && !isEffect && !isLitAlphaMask &&
            !isLitAlphaTint && !isLayered && !isTintedLayered && !isMaskedToon &&
            !isSpecularTextured) {
            ++g_stats.materialRejections[static_cast<std::size_t>(colorResult)];
            ++g_stats.texturedMaterialRejections[static_cast<std::size_t>(unlitFamily)];
            ++g_stats.litTexturedMaterialRejections[static_cast<std::size_t>(litFamily)];
            ++g_stats.litAlphaTintMaterialRejections[static_cast<std::size_t>(litAlphaTintFamily)];
            ++g_stats.effectMaterialRejections[static_cast<std::size_t>(effectFamily)];
            ++g_stats
                  .tintedLayeredMaterialRejections[static_cast<std::size_t>(tintedLayeredFamily)];
            ++g_stats.maskedToonMaterialRejections[static_cast<std::size_t>(maskedToonFamily)];
            ++g_stats.specularTexturedMaterialRejections[static_cast<std::size_t>(specularFamily)];
            return;
        }
        ++observation.materialAccepted;
        const u32 textureTable = sb_r32(materialPacket + kMaterialPacketTexture);
        if (!readable(textureTable)) {
            ++g_stats.textureTableFailures;
            return;
        }
        sb::native_render::ResTimgDecodeError textureError{};
        const std::uint16_t firstTextureNumber =
            isUnlitTextured ? sb::native_render::j3d_texture_number_for_map(
                                  materialState, materialState.tevStages[0].textureMap)
                            : materialState.textureBindings[0].textureNumber;
        if (!sb::recomp::capture_guest_j3d_texture(sb::recomp::live_guest_byte_reader(),
                                                   textureTable, firstTextureNumber, g_textures[0],
                                                   textureError)) {
            const std::size_t errorIndex = static_cast<std::size_t>(textureError);
            if (errorIndex < g_stats.textureDecodeFailures.size())
                ++g_stats.textureDecodeFailures[errorIndex];
            return;
        }
        const std::size_t textureCount =
            isMaskedToon ? 4 : (isLitAlphaMask || isLayered || isTintedLayered ? 2 : 1);
        for (std::size_t index = 1; index < textureCount; ++index) {
            if (!sb::recomp::capture_guest_j3d_texture(
                    sb::recomp::live_guest_byte_reader(), textureTable,
                    materialState.textureBindings[index].textureNumber, g_textures[index],
                    textureError)) {
                const std::size_t errorIndex = static_cast<std::size_t>(textureError);
                if (errorIndex < g_stats.textureDecodeFailures.size())
                    ++g_stats.textureDecodeFailures[errorIndex];
                return;
            }
        }
        if (isAlphaMasked) {
            const sb::native_render::J3dAlphaMaskedMaterialResult classified =
                sb::native_render::classify_j3d_alpha_masked_material(
                    materialState, g_textures[0].texture, alphaMaskedMaterial);
            SB_ASSERT(classified == sb::native_render::J3dAlphaMaskedMaterialResult::Success,
                      "decoded J3D texture invalidated a preclassified alpha-mask material: "
                      "result=%s",
                      sb::native_render::j3d_alpha_masked_material_result_name(classified));
            semanticMaterial = alphaMaskedMaterial;
            submittedAlphaMaskedMaterial = true;
        } else if (isSpecularTextured) {
            const sb::native_render::J3dSpecularTexturedResult classified =
                sb::native_render::classify_j3d_specular_textured_material(
                    materialState, g_textures[0].texture, *lighting, specularMaterial);
            SB_ASSERT(classified == sb::native_render::J3dSpecularTexturedResult::Success,
                      "decoded J3D texture invalidated a preclassified specular material: "
                      "result=%s",
                      sb::native_render::j3d_specular_textured_result_name(classified));
            semanticMaterial = specularMaterial;
            submittedLitMaterial = true;
        } else if (isLitAlphaMask) {
            const sb::native_render::J3dLitAlphaMaskResult classified =
                sb::native_render::classify_j3d_lit_alpha_mask_material(
                    materialState, g_textures[0].texture, g_textures[1].texture, *lighting,
                    litAlphaMaskMaterial);
            SB_ASSERT(classified == sb::native_render::J3dLitAlphaMaskResult::Success,
                      "decoded J3D textures invalidated a preclassified lit alpha-mask material: "
                      "result=%s",
                      sb::native_render::j3d_lit_alpha_mask_result_name(classified));
            semanticMaterial = litAlphaMaskMaterial;
            submittedLitMaterial = true;
        } else if (isLitAlphaTint) {
            const sb::native_render::J3dLitAlphaTintResult classified =
                sb::native_render::classify_j3d_lit_alpha_tint_material(
                    materialState, g_textures[0].texture, *lighting, litAlphaTintMaterial);
            SB_ASSERT(classified == sb::native_render::J3dLitAlphaTintResult::Success,
                      "decoded J3D texture invalidated a preclassified lit alpha-tint material: "
                      "result=%s",
                      sb::native_render::j3d_lit_alpha_tint_result_name(classified));
            semanticMaterial = litAlphaTintMaterial;
            submittedLitMaterial = true;
        } else if (isLayered) {
            const sb::native_render::J3dLayeredMaterialResult classified =
                sb::native_render::classify_j3d_layered_material(
                    materialState, g_textures[0].texture, g_textures[1].texture, *lighting,
                    layeredMaterial);
            SB_ASSERT(classified == sb::native_render::J3dLayeredMaterialResult::Success,
                      "decoded J3D textures invalidated a preclassified layered material: "
                      "result=%s",
                      sb::native_render::j3d_layered_material_result_name(classified));
            semanticMaterial = layeredMaterial;
            submittedLitMaterial = true;
        } else if (isTintedLayered) {
            const sb::native_render::J3dTintedLayeredMaterialResult classified =
                sb::native_render::classify_j3d_tinted_layered_material(
                    materialState, g_textures[0].texture, g_textures[1].texture, *lighting,
                    tintedLayeredMaterial);
            SB_ASSERT(classified == sb::native_render::J3dTintedLayeredMaterialResult::Success,
                      "decoded J3D textures invalidated a preclassified tinted layered material: "
                      "result=%s",
                      sb::native_render::j3d_tinted_layered_material_result_name(classified));
            semanticMaterial = tintedLayeredMaterial;
            submittedLitMaterial = true;
        } else if (isMaskedToon) {
            const sb::native_render::J3dMaskedToonMaterialResult classified =
                sb::native_render::classify_j3d_masked_toon_material(
                    materialState, g_textures[0].texture, g_textures[1].texture,
                    g_textures[2].texture, g_textures[3].texture, *lighting, maskedToonMaterial);
            SB_ASSERT(classified == sb::native_render::J3dMaskedToonMaterialResult::Success,
                      "decoded J3D textures invalidated a preclassified masked-toon material: "
                      "result=%s",
                      sb::native_render::j3d_masked_toon_material_result_name(classified));
            semanticMaterial = maskedToonMaterial;
            submittedLitMaterial = true;
        } else if (isLitTextured) {
            const sb::native_render::J3dLitTexturedResult classified =
                sb::native_render::classify_j3d_lit_textured_material(
                    materialState, g_textures[0].texture, *lighting, litMaterial);
            SB_ASSERT(classified == sb::native_render::J3dLitTexturedResult::Success,
                      "decoded J3D texture invalidated a preclassified lit material: result=%s",
                      sb::native_render::j3d_lit_textured_result_name(classified));
            semanticMaterial = litMaterial;
            submittedLitMaterial = true;
        } else if (isEffect) {
            const sb::native_render::J3dEffectMaterialResult classified =
                sb::native_render::classify_j3d_effect_material(
                    materialState, g_textures[0].texture, effectMaterial);
            SB_ASSERT(classified == sb::native_render::J3dEffectMaterialResult::Success,
                      "decoded J3D texture invalidated a preclassified effect material: result=%s",
                      sb::native_render::j3d_effect_material_result_name(classified));
            semanticMaterial = effectMaterial;
            submittedLitMaterial = true;
        } else {
            const sb::native_render::J3dUnlitTexturedResult classified =
                sb::native_render::classify_j3d_unlit_textured_material(
                    materialState, g_textures[0].texture, texturedMaterial);
            SB_ASSERT(classified == sb::native_render::J3dUnlitTexturedResult::Success,
                      "decoded J3D texture invalidated a preclassified material: result=%s",
                      sb::native_render::j3d_unlit_textured_result_name(classified));
            semanticMaterial = texturedMaterial;
        }
        for (std::size_t index = 0; index < textureCount; ++index) {
            const auto& texture = g_textures[index];
            semanticImages[index] = {texture.texture.resource,
                                     texture.texture.revision,
                                     texture.texture.width,
                                     texture.texture.height,
                                     texture.rgba8,
                                     texture.mipLevels};
        }
        images = std::span(semanticImages).first(textureCount);
    }
    sb::native_render::ModelFog semanticFog{};
    SB_ASSERT(sb::native_render::build_model_fog(materialState.fog, semanticFog),
              "accepted J3D material has no semantic fog representation: type=%u adjusted=%u",
              materialState.fog.type, materialState.fog.rangeAdjustmentEnabled ? 1U : 0U);
    ++observation.resourcesReady;

    if (scene == nullptr ||
        scene->projectionKind != sb::native_render::ProjectionKind::Perspective) {
        ++g_stats.noPerspectiveContexts;
        return;
    }
    ++observation.perspectiveReady;
    const u32 matrixObjects = sb_r32(shape + kShapeMatrices);
    const u32 drawMatrixData = sb_r32(shape + kShapeDrawMatrixData);
    const u32 drawMatrices = sb_r32(shape + kShapeDrawMatrices);
    const u32 currentViewPointer = sb_r32(shape + kShapeCurrentView);
    const u32 currentView = readable(currentViewPointer) ? sb_r32(currentViewPointer) : 0;
    const u32 drawMatrixArray =
        readable(drawMatrices) && currentView <= 16 ? sb_r32(drawMatrices + currentView * 4) : 0;
    const u32 elementCount = sb_r16(shape + kShapeElementCount);
    if (!readable(matrixObjects) || !readable(drawMatrixData) || !readable(drawMatrixArray) ||
        elementCount == 0 || elementCount >= 4096) {
        ++g_stats.nonRigidElements;
        return;
    }
    const u32 drawMatrixCount = sb_r16(drawMatrixData);

    for (u32 element = 0; element < elementCount; ++element) {
        const u32 matrixObject = sb_r32(matrixObjects + element * 4);
        if (!readable(matrixObject)) {
            ++g_stats.nonRigidElements;
            continue;
        }

        std::array<sb::native_render::ModelMatrixBinding, sb::native_render::kMaxModelMatrices>
            poseBindings{};
        std::size_t poseBindingCount = 0;
        bool invalidPoseBinding = false;
        for (const GuestJ3dMatrixBinding& binding : matrices) {
            if (binding.matrixObject != matrixObject)
                continue;
            if (poseBindingCount == poseBindings.size()) {
                poseBindingCount = poseBindings.size() + 1;
                break;
            }
            if (binding.matrixIndex >= drawMatrixCount) {
                invalidPoseBinding = true;
                break;
            }
            const u32 matrixAddress = drawMatrixArray + binding.matrixIndex * 48U;
            if (!readable(matrixAddress) || !readable(matrixAddress + 47)) {
                invalidPoseBinding = true;
                break;
            }
            sb::native_render::ModelMatrixBinding poseBinding{.sourceIndex = binding.sourceSlot};
            for (std::size_t index = 0; index < poseBinding.modelView.value.size(); ++index)
                poseBinding.modelView.value[index] = guest_f32(matrixAddress + index * 4U);
            poseBindings[poseBindingCount++] = poseBinding;
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

        g_decoded.clear();
        if (!j3d_decode_element(shape, element, layout, g_decoded) || g_decoded.empty()) {
            ++g_stats.decodeFailures;
            continue;
        }
        if (std::ranges::any_of(g_decoded, [&](const J3DVert& vertex) {
                return vertex.positionMatrixSlot >= sourceToCompact.size() ||
                       sourceToCompact[vertex.positionMatrixSlot] == 0xFFU;
            })) {
            ++g_stats.nonRigidElements;
            continue;
        }

        g_vertices.clear();
        g_vertices.reserve(g_decoded.size());
        for (const J3DVert& vertex : g_decoded) {
            g_vertices.push_back({.position = {vertex.x, vertex.y, vertex.z},
                                  .uv = {vertex.uv[0][0], vertex.uv[0][1]},
                                  .uv1 = {vertex.uv[1][0], vertex.uv[1][1]},
                                  .uv2 = {vertex.uv[2][0], vertex.uv[2][1]},
                                  .uv3 = {vertex.uv[3][0], vertex.uv[3][1]},
                                  .color = sb::native_render::color_from_rgba8(vertex.rgba),
                                  .normal = {vertex.nx, vertex.ny, vertex.nz},
                                  .matrixIndex = sourceToCompact[vertex.positionMatrixSlot]});
        }
        const std::uint64_t resource = (static_cast<std::uint64_t>(shape) << 16U) | element;
        const std::uint64_t revision = sb::native_render::mesh_revision(g_vertices);
        sb::native_render::ModelDraw draw{};
        draw.instance = (static_cast<std::uint64_t>(shape) << 32U) | drawMatrixArray;
        draw.mesh = {resource, revision, static_cast<std::uint32_t>(g_vertices.size())};
        draw.pose = pose;
        draw.projection = scene->projection;
        draw.material = semanticMaterial;
        draw.fog = semanticFog;
        const sb::native_render::MeshResourceView mesh{resource, revision, g_vertices};
        SB_ASSERT(sb::native_render::submit_model(draw, mesh, images),
                  "semantic J3D sink rejected validated model: shape=%08x element=%u "
                  "vertices=%zu",
                  shape, element, g_vertices.size());
        record_raster(semanticMaterial);
        ++observation.submittedModels;
        ++g_stats.submittedModels;
        if (submittedLitMaterial)
            ++g_stats.submittedLitModels;
        if (submittedAlphaMaskedMaterial)
            ++g_stats.submittedAlphaMaskedModels;
        g_stats.submittedVertices += g_vertices.size();
    }
}
