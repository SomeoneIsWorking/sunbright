// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_model_probe.h"

#include <sunbright/native_render/j3d_mesh_decode.h>
#include <sunbright/native_render/j3d_stage_lighting.h>

#include <cstdio>
#include <limits>
#include <span>
#include <utility>

namespace sunbright::gcnport_boot {
namespace {

// `this` is in r3 on entry to J3DShape::draw, whose own first instructions read 0x28(r3).
constexpr std::size_t THIS_REGISTER = 3;

// J3DSys, from the offsets J3DMatPacket::draw itself writes: the material packet it is drawing with
// at 0x3c, and that packet's texture table at 0x54.
constexpr sb::title_adapter::GuestAddress SYSTEM_MAT_PACKET = 0x3c;
// J3DMatPacket.
constexpr sb::title_adapter::GuestAddress MAT_PACKET_MATERIAL = 0x38;
constexpr sb::title_adapter::GuestAddress MAT_PACKET_TEXTURE = 0x40;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

bool read_through_byte_address(sb::native_render::ByteAddress address,
                               std::span<std::uint8_t> destination, void* context) {
    std::uint64_t guestAddress = 0;
    if (!address.guest_value(guestAddress) ||
        guestAddress > std::numeric_limits<sb::title_adapter::GuestAddress>::max()) {
        return false;
    }
    return read_through_guest_context(static_cast<sb::title_adapter::GuestAddress>(guestAddress),
                                      destination, context);
}

[[nodiscard]] bool carries(const sb::native_render::J3dVertexLayout& layout,
                           std::uint32_t attribute) noexcept {
    return layout.type[attribute] !=
           static_cast<std::uint8_t>(sb::native_render::J3dAttributeType::None);
}

template <typename Key, typename Name>
void print_histogram(const char* label, const std::map<Key, std::uint64_t>& histogram,
                     Name naming) {
    std::printf("gmse01_boot:   %s:", label);
    if (histogram.empty()) {
        std::printf(" none recorded -- nothing reached this stage");
    }
    for (const auto& [key, count] : histogram) {
        std::printf(" %s=%llu", naming(key), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
}

} // namespace

void GuestModelProbe::record(std::map<std::uint32_t, std::uint64_t>& histogram,
                             std::uint64_t& untracked, std::uint32_t value) {
    if (histogram.size() < MAX_DISTINCT_VALUES || histogram.contains(value)) {
        histogram[value] += 1;
        return;
    }
    untracked += 1;
}

bool GuestModelProbe::resolve_texture(gcnport::GuestContext& guest,
                                      const sb::title_adapter::GuestTextureTable& table,
                                      std::uint16_t number,
                                      sb::native_render::DecodedTexture& texture,
                                      sb::native_render::ResTimgDecodeError& error) {
    if (number == 0xFFFF || number >= table.count) {
        return false;
    }
    const sb::title_adapter::GuestAddress header =
        table.resources + static_cast<sb::title_adapter::GuestAddress>(number) *
                              sb::title_adapter::GUEST_RES_TIMG_BYTES;
    if (const auto cached = textureCache_.find(header); cached != textureCache_.end()) {
        texture = cached->second;
        return true;
    }
    sb::native_render::DecodedTexture decoded{};
    const sb::title_adapter::GuestTextureError textureError =
        decode_guest_texture({read_through_byte_address, &guest}, table, number, decoded, error);
    decodeErrors_[error] += 1;
    if (textureError != sb::title_adapter::GuestTextureError::None) {
        return false;
    }
    texturesDecoded_ += 1;
    textureBytes_ += decoded.rgba8.size();
    texture = decoded;
    textureCache_.emplace(header, std::move(decoded));
    return true;
}

bool GuestModelProbe::resolve_texture_thunk(std::uint16_t number,
                                            sb::native_render::DecodedTexture& texture,
                                            sb::native_render::ResTimgDecodeError& error,
                                            void* context) {
    auto& resolver = *static_cast<TextureResolver*>(context);
    return resolver.probe->resolve_texture(*resolver.guest, resolver.table, number, texture, error);
}

void GuestModelProbe::record_refusals(const sb::native_render::J3dFamilyRefusals& refusals,
                                      const sb::native_render::J3dMaterialState& state,
                                      sb::title_adapter::GuestAddress material) {
    const auto existing = refusedMaterials_.find(material);
    if (existing != refusedMaterials_.end()) {
        existing->second.draws += 1;
    } else if (refusedMaterials_.size() < MAX_DISTINCT_DRAWS) {
        refusedMaterials_.emplace(
            material, RefusedMaterial{.draws = 1, .state = state, .refusals = refusals});
    } else {
        refusedMaterialsUntracked_ += 1;
    }
    record(refusedChannels_, refusedChannelsUntracked_,
           (static_cast<std::uint32_t>(state.colorChannelControl) << 16U) |
               state.alphaChannelControl);
    record(refusedStageCounts_, refusedStageCountsUntracked_, state.tevStageCount);
    for (std::size_t index = 1; index < sb::native_render::kJ3dMaterialFamilyCount; ++index) {
        const char* const reason = refusals.reason[index];
        if (reason == nullptr) {
            continue;
        }
        refusals_[static_cast<sb::native_render::J3dMaterialFamily>(index)][reason] += 1;
    }
}

gcnport::HookResult GuestModelProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;

    const auto shapeAddress =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(THIS_REGISTER));
    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    const sb::title_adapter::GuestReader reader(memory);

    sb::title_adapter::GuestShape shape{};
    const sb::title_adapter::GuestShapeError shapeError =
        read_guest_shape(memory, shapeAddress, system_, shape);
    shapeErrors_[shapeError] += 1;
    if (shapeError != sb::title_adapter::GuestShapeError::None) {
        unreadableShapes_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    // The two geometry facts the material classifiers need, taken from the shape's own layout --
    // the reason this hook exists at all.
    const bool hasNormal = carries(shape.layout, sb::native_render::kJ3dNormalAttribute);
    const bool hasVertexColor = carries(shape.layout, sb::native_render::kJ3dColor0Attribute);
    withNormal_ += hasNormal ? 1 : 0;
    withVertexColor_ += hasVertexColor ? 1 : 0;

    sb::title_adapter::GuestAddress packet = 0;
    if (!reader.word(system_ + SYSTEM_MAT_PACKET, packet)) {
        unreadableMaterialPackets_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    if (packet == 0) {
        noMaterialPacket_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    sb::title_adapter::GuestAddress material = 0;
    sb::title_adapter::GuestAddress tableAddress = 0;
    if (!reader.word(packet + MAT_PACKET_MATERIAL, material) ||
        !reader.word(packet + MAT_PACKET_TEXTURE, tableAddress)) {
        unreadableMaterialPackets_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    if (material == 0) {
        noMaterialPacket_ += 1;
        return gcnport::HookResult::call_original_once();
    }

    sb::native_render::J3dMaterialState state{};
    sb::title_adapter::GuestMaterial read{};
    const sb::title_adapter::GuestMaterialError materialError =
        read_guest_material(memory, material, {}, {}, hasVertexColor, hasNormal, state, read);
    materialErrors_[materialError] += 1;
    if (materialError != sb::title_adapter::GuestMaterialError::None) {
        unreadableMaterials_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    composed_ += 1;

    // One draw is one (shape, material) pair: the same shape drawn with two materials is two, and
    // the same pair redrawn every frame is one.
    const std::uint64_t draw = static_cast<std::uint64_t>(shapeAddress) << 32U | material;
    if (draws_.size() < MAX_DISTINCT_DRAWS) {
        draws_.insert(draw);
    } else if (!draws_.contains(draw)) {
        drawsPastTheSet_ += 1;
    }

    sb::title_adapter::GuestTextureTable table{};
    const sb::title_adapter::GuestTextureError tableError =
        read_guest_texture_table(memory, tableAddress, table);
    tableErrors_[tableError] += 1;

    const sb::native_render::ModelLightingContext* const lighting =
        sb::native_render::current_j3d_stage_lighting();
    withLighting_ += lighting != nullptr ? 1 : 0;

    TextureResolver resolver{.probe = this, .guest = &guest, .table = table};
    sb::native_render::ClassifiedJ3dMaterial classified{};
    sb::native_render::J3dFamilyRefusals refusals{};
    const sb::native_render::J3dMaterialFamilyResult result =
        sb::native_render::classify_j3d_material(
            state, lighting, {resolve_texture_thunk, &resolver}, classified, &refusals);
    results_[result] += 1;
    families_[classified.family] += 1;
    if (result != sb::native_render::J3dMaterialFamilyResult::Success) {
        record_refusals(refusals, state, material);
        return gcnport::HookResult::call_original_once();
    }
    classified_ += 1;
    record(textureCounts_, textureCountsUntracked_, classified.textureCount);
    drawsPublished_ += publisher_.publish(guest, shape, shapeAddress, classified);

    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf("gmse01_boot:   draw %llu: shape=0x%08x material=0x%08x %s normal=%d "
                    "vertexColour=%d textures=%u\n",
                    static_cast<unsigned long long>(reports_), shapeAddress, material,
                    sb::native_render::j3d_material_family_name(classified.family),
                    hasNormal ? 1 : 0, hasVertexColor ? 1 : 0, classified.textureCount);
    }
    return gcnport::HookResult::call_original_once();
}

// Prints one histogram of refused authored values. `paired` splits the key into the colour and
// alpha channel controls it was packed from.
void print_refused_values(const char* label, const std::map<std::uint32_t, std::uint64_t>& values,
                          std::uint64_t untracked, bool paired) {
    if (values.empty()) {
        std::printf("gmse01_boot:   %s: none (nothing was refused)\n", label);
        return;
    }
    std::printf("gmse01_boot:   %s:", label);
    for (const auto& [value, count] : values) {
        if (paired) {
            std::printf(" %04x/%04x=%llu", value >> 16U, value & 0xFFFFU,
                        static_cast<unsigned long long>(count));
        } else {
            std::printf(" %u=%llu", value, static_cast<unsigned long long>(count));
        }
    }
    if (untracked != 0) {
        std::printf(" (+%llu past the tracked distinct values)",
                    static_cast<unsigned long long>(untracked));
    }
    std::printf("\n");
}

void GuestModelProbe::report_refused_materials() const {
    std::printf("gmse01_boot:   %llu distinct material(s) no family accepted%s\n",
                static_cast<unsigned long long>(refusedMaterials_.size()),
                refusedMaterialsUntracked_ != 0 ? " (a floor: more were refused than recorded)"
                                                : "");
    for (const auto& [address, refused] : refusedMaterials_) {
        const sb::native_render::J3dMaterialState& state = refused.state;
        std::printf(
            "gmse01_boot:     0x%08x x%llu chans=%u lit=%d %04x/%04x %04x/%04x "
            "stages=%u texcoords=%u normal=%d vcolor=%d\n",
            address, static_cast<unsigned long long>(refused.draws), state.colorChannelCount,
            state.lightingEnabled ? 1 : 0, state.colorChannelControl, state.alphaChannelControl,
            state.colorChannelControl1, state.alphaChannelControl1, state.tevStageCount,
            state.textureCoordinateCount, state.hasNormal ? 1 : 0, state.hasVertexColor ? 1 : 0);
        // The pixel policy is matched against an enumeration of exact authored combinations, so a
        // material refused for its raster policy can only be ported once these values are read.
        std::printf("gmse01_boot:       pixel engine block=%08x explicit=%d cull=%u "
                    "alpha=%u:%02x %u %u:%02x blend=%u src=%u dst=%u logic=%u "
                    "depth=%d cmp=%u write=%d\n",
                    state.pixelEngineBlockType, state.hasExplicitPixelPolicy ? 1 : 0,
                    state.cullMode, state.alphaCompare0, state.alphaReference0,
                    state.alphaOperation, state.alphaCompare1, state.alphaReference1,
                    state.blendMode, state.blendSourceFactor, state.blendDestinationFactor,
                    state.blendLogicOperation, state.depthTest ? 1 : 0, state.depthCompare,
                    state.depthWrite ? 1 : 0);
        for (std::uint8_t stage = 0;
             stage < state.tevStageCount && stage < sb::native_render::kMaxJ3dTevStages; ++stage) {
            const sb::native_render::J3dTevStageState& tev = state.tevStages[stage];
            std::printf("gmse01_boot:       stage %u coord=%u map=%u chan=%u konst=%u/%u prog=",
                        stage, tev.textureCoordinate, tev.textureMap, tev.colorChannel,
                        tev.konstColorSelection, tev.konstAlphaSelection);
            for (const std::uint8_t byte : tev.program) {
                std::printf("%02x", byte);
            }
            std::printf("\n");
        }
        for (std::size_t family = 0; family < sb::native_render::kJ3dMaterialFamilyCount;
             ++family) {
            const char* const reason = refused.refusals.reason[family];
            if (reason == nullptr) {
                continue;
            }
            std::printf("gmse01_boot:       %s: %s\n",
                        sb::native_render::j3d_material_family_name(
                            static_cast<sb::native_render::J3dMaterialFamily>(family)),
                        reason);
        }
    }
}

void GuestModelProbe::report() const {
    std::printf(
        "gmse01_boot: guest model probe: %llu shape draw(s), %llu composed with a material, "
        "%llu classified, %llu published, %llu distinct (shape, material) pair(s)%s\n",
        static_cast<unsigned long long>(entries_), static_cast<unsigned long long>(composed_),
        static_cast<unsigned long long>(classified_),
        static_cast<unsigned long long>(drawsPublished_),
        static_cast<unsigned long long>(draws_.size()),
        drawsPastTheSet_ != 0 ? " (a floor: more were seen than tracked)" : "");
    std::printf("gmse01_boot:   %llu unreadable shape(s), %llu without a material packet, %llu "
                "unreadable packet(s), %llu unreadable material(s)\n",
                static_cast<unsigned long long>(unreadableShapes_),
                static_cast<unsigned long long>(noMaterialPacket_),
                static_cast<unsigned long long>(unreadableMaterialPackets_),
                static_cast<unsigned long long>(unreadableMaterials_));
    std::printf("gmse01_boot:   %llu shape(s) carry normals, %llu carry vertex colour, %llu were "
                "classified against a published stage light\n",
                static_cast<unsigned long long>(withNormal_),
                static_cast<unsigned long long>(withVertexColor_),
                static_cast<unsigned long long>(withLighting_));
    print_histogram("shape errors", shapeErrors_, sb::title_adapter::guest_shape_error_name);
    print_histogram("material errors", materialErrors_,
                    [](sb::title_adapter::GuestMaterialError error) { return name(error); });
    print_histogram("texture table errors", tableErrors_,
                    [](sb::title_adapter::GuestTextureError error) { return name(error); });
    print_histogram("texture decode results", decodeErrors_,
                    sb::native_render::res_timg_decode_error_name);
    print_histogram("classification", results_, sb::native_render::j3d_material_family_result_name);
    print_histogram("families", families_, sb::native_render::j3d_material_family_name);
    if (refusals_.empty()) {
        std::printf("gmse01_boot:   no family refused a draw\n");
    }
    for (const auto& [family, reasons] : refusals_) {
        std::printf("gmse01_boot:   %s refused:",
                    sb::native_render::j3d_material_family_name(family));
        for (const auto& [reason, count] : reasons) {
            std::printf(" %s=%llu", reason.c_str(), static_cast<unsigned long long>(count));
        }
        std::printf("\n");
    }
    publisher_.report();
    report_refused_materials();
    print_refused_values("refused colour/alpha channels", refusedChannels_,
                         refusedChannelsUntracked_, true);
    print_refused_values("refused colour-stage counts", refusedStageCounts_,
                         refusedStageCountsUntracked_, false);
    std::printf("gmse01_boot:   %llu distinct texture(s) decoded, %llu byte(s) of RGBA\n",
                static_cast<unsigned long long>(texturesDecoded_),
                static_cast<unsigned long long>(textureBytes_));
    if (textureCounts_.empty()) {
        std::printf("gmse01_boot:   textures per classified draw: none recorded\n");
        return;
    }
    std::printf("gmse01_boot:   textures per classified draw:");
    for (const auto& [count, draws] : textureCounts_) {
        std::printf(" %u=%llu", count, static_cast<unsigned long long>(draws));
    }
    if (textureCountsUntracked_ != 0) {
        std::printf(" (+%llu past the tracked distinct values)",
                    static_cast<unsigned long long>(textureCountsUntracked_));
    }
    std::printf("\n");
}

} // namespace sunbright::gcnport_boot
