// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_material_probe.h"

#include <sunbright/native_render/j3d_lit_material.h>
#include <sunbright/native_render/j3d_stage_lighting.h>

#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <utility>

namespace sunbright::gcnport_boot {
namespace {

// Retail J3DMatPacket, from decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DPacket.hpp. Everything
// below these two pointers belongs to `title_adapter`, which owns those layouts.
constexpr sb::title_adapter::GuestAddress MAT_PACKET_MATERIAL = 0x38;
constexpr sb::title_adapter::GuestAddress MAT_PACKET_TEXTURE = 0x40;

// `this` is in r3 on entry to J3DMatPacket::draw, whose own first instructions read 0x34(r3).
constexpr std::size_t THIS_REGISTER = 3;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
}

// The texture decoder reads through a ByteAddress rather than a raw guest address. Both adapters
// end at `guest->read_memory`, so a range the runtime refuses is refused identically either way.
bool read_through_byte_address(sb::native_render::ByteAddress address,
                               std::span<std::uint8_t> destination, void* context) {
    std::uint64_t guestAddress = 0;
    if (!address.guest_value(guestAddress) ||
        guestAddress > std::numeric_limits<sb::title_adapter::GuestAddress>::max()) {
        return false;
    }
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(static_cast<sb::title_adapter::GuestAddress>(guestAddress),
                              std::as_writable_bytes(destination));
}

// The two encodings J3D's own `calcAlphaCmpID`/`calcZModeID` use to build the ids the tables are
// indexed by. Re-encoding what the table answered is what makes a zero from an unbuilt table
// distinguishable from a zero the game authored.
std::uint16_t encode_alpha_compare_id(std::uint8_t compare0, std::uint8_t operation,
                                      std::uint8_t compare1) {
    return static_cast<std::uint16_t>((compare0 << 5U) + (operation << 3U) + compare1);
}

std::uint16_t encode_depth_mode_id(bool compareEnable, std::uint8_t function, bool updateEnable) {
    return static_cast<std::uint16_t>((function * 2U) + (compareEnable ? 0x10U : 0U) +
                                      (updateEnable ? 1U : 0U));
}

void print_four_cc(std::uint32_t type, std::uint64_t count) {
    std::printf(" %c%c%c%c=%llu", static_cast<char>(type >> 24U), static_cast<char>(type >> 16U),
                static_cast<char>(type >> 8U), static_cast<char>(type),
                static_cast<unsigned long long>(count));
}

// Prints an error histogram, and says so when it is empty rather than printing an empty line: a
// blank where a denominator belongs cannot be told from a run in which nothing was ever read.
template <typename Error, typename Name>
void print_errors(const char* label, const std::map<Error, std::uint64_t>& histogram, Name naming) {
    std::printf("gmse01_boot:   %s:", label);
    if (histogram.empty()) {
        std::printf(" none recorded -- nothing was read");
    }
    for (const auto& [error, count] : histogram) {
        std::printf(" %s=%llu", naming(error), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
}

template <typename Error>
void print_errors(const char* label, const std::map<Error, std::uint64_t>& histogram) {
    print_errors(label, histogram, [](Error error) { return name(error); });
}

void print_histogram(const char* label, const std::map<std::uint32_t, std::uint64_t>& histogram,
                     std::uint64_t untracked) {
    if (histogram.empty()) {
        std::printf("gmse01_boot:   %s: none recorded\n", label);
        return;
    }
    std::printf("gmse01_boot:   %s:", label);
    for (const auto& [value, count] : histogram) {
        std::printf(" %u=%llu", value, static_cast<unsigned long long>(count));
    }
    if (untracked != 0) {
        std::printf(" (+%llu past the tracked distinct values -- truncated, not complete)",
                    static_cast<unsigned long long>(untracked));
    }
    std::printf("\n");
}

} // namespace

void GuestMaterialProbe::record(std::map<std::uint32_t, std::uint64_t>& histogram,
                                std::uint64_t& untracked, std::uint32_t value) {
    if (histogram.size() < MAX_DISTINCT_VALUES || histogram.contains(value)) {
        histogram[value] += 1;
        return;
    }
    untracked += 1;
}

bool GuestMaterialProbe::resolve_texture(gcnport::GuestContext& guest,
                                         const sb::title_adapter::GuestTextureTable& table,
                                         std::uint16_t number,
                                         sb::native_render::DecodedTexture& texture,
                                         sb::native_render::ResTimgDecodeError& error) {
    if (number == 0xFFFF) {
        return false;
    }
    if (number >= table.count) {
        texturesPastTheTable_ += 1;
        return false;
    }
    const sb::title_adapter::GuestAddress header =
        table.resources + static_cast<sb::title_adapter::GuestAddress>(number) *
                              sb::title_adapter::GUEST_RES_TIMG_BYTES;
    // A texture is the same bytes every time it is bound, so it is decoded once per resource. With
    // 78,380 binds of fewer than a hundred textures, decoding per bind would have measured the
    // cache rather than the decoder.
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
    record(textureSizes_, textureSizesUntracked_,
           decoded.texture.width * 0x10000U + decoded.texture.height);
    texture = decoded;
    textureCache_.emplace(header, std::move(decoded));
    return true;
}

bool GuestMaterialProbe::resolve_texture_thunk(std::uint16_t number,
                                               sb::native_render::DecodedTexture& texture,
                                               sb::native_render::ResTimgDecodeError& error,
                                               void* context) {
    auto& resolver = *static_cast<TextureResolver*>(context);
    return resolver.probe->resolve_texture(*resolver.guest, resolver.table, number, texture, error);
}

bool GuestMaterialProbe::read_texture_table(gcnport::GuestContext& guest,
                                            const sb::title_adapter::GuestMemory& memory,
                                            sb::title_adapter::GuestAddress packet,
                                            sb::title_adapter::GuestTextureTable& table) {
    const sb::title_adapter::GuestReader reader(memory);
    sb::title_adapter::GuestAddress tableAddress = 0;
    if (!reader.word(packet + MAT_PACKET_TEXTURE, tableAddress)) {
        textureErrors_[sb::title_adapter::GuestTextureError::UnreadableTable] += 1;
        return false;
    }
    const sb::title_adapter::GuestTextureError error =
        read_guest_texture_table(memory, tableAddress, table);
    textureErrors_[error] += 1;
    if (error != sb::title_adapter::GuestTextureError::None) {
        return false;
    }
    textureTablesRead_ += 1;
    if (table.padding != 0) {
        texturePaddingNonZero_ += 1;
    }
    record(textureTableSizes_, textureTableSizesUntracked_, table.count);
    static_cast<void>(guest);
    return true;
}

void GuestMaterialProbe::measure_bindings(gcnport::GuestContext& guest,
                                          const sb::native_render::J3dMaterialState& state,
                                          const sb::title_adapter::GuestTextureTable& table,
                                          std::uint8_t bindingCount) {
    for (std::uint8_t binding = 0; binding < bindingCount; ++binding) {
        const std::uint16_t number = state.textureBindings[binding].textureNumber;
        if (number == 0xFFFF) {
            continue;
        }
        texturesBound_ += 1;
        sb::native_render::DecodedTexture texture{};
        sb::native_render::ResTimgDecodeError error = sb::native_render::ResTimgDecodeError::None;
        static_cast<void>(resolve_texture(guest, table, number, texture, error));
    }
}

// Why a material was refused, asked of the shipping classifiers themselves rather than re-derived.
// A bare "unsupported program" count cannot be acted on: it says nothing about whether the gate is
// the raster policy, the colour program, or an input this hook cannot see.
void GuestMaterialProbe::measure_refusals(const sb::native_render::J3dMaterialState& state) {
    sb::native_render::ModelRasterPolicy policy{};
    rasterResults_[sb::native_render::classify_j3d_raster_policy(state, policy)] += 1;
    sb::native_render::UnlitColorMaterial unlitColor{};
    unlitResults_[sb::native_render::classify_j3d_unlit_material(state, unlitColor)] += 1;
    const sb::native_render::PictureTexture placeholder{.resource = 1, .width = 1, .height = 1};
    sb::native_render::UnlitTexturedMaterial unlitTextured{};
    unlitTexturedResults_[sb::native_render::classify_j3d_unlit_textured_material(
        state, placeholder, unlitTextured)] += 1;

    // The two lit families the scene's materials are shaped like. Asked only when a light has been
    // published, so a zero denominator here says "no relight had run yet" rather than "every lit
    // family refused".
    const sb::native_render::ModelLightingContext* const lighting =
        sb::native_render::current_j3d_stage_lighting();
    if (lighting == nullptr) {
        return;
    }
    sb::native_render::LitColorMaterial litColor{};
    litColorResults_[sb::native_render::classify_j3d_lit_color_material(state, *lighting,
                                                                        litColor)] += 1;
    sb::native_render::LitTexturedMaterial litTextured{};
    litTexturedResults_[sb::native_render::classify_j3d_lit_textured_material(
        state, placeholder, *lighting, litTextured)] += 1;
}

void GuestMaterialProbe::classify(gcnport::GuestContext& guest,
                                  const sb::native_render::J3dMaterialState& state,
                                  const sb::title_adapter::GuestTextureTable& table) {
    TextureResolver resolver{.probe = this, .guest = &guest, .table = table};
    sb::native_render::ClassifiedJ3dMaterial classified{};
    // Whatever the stage-light hook last published, read back through the shipping accessor rather
    // than kept here. Null until a relight has run, which is the honest answer for a material drawn
    // before one: every lit family is then out of reach and says so, instead of being classified
    // against an invented rig.
    const sb::native_render::ModelLightingContext* const lighting =
        sb::native_render::current_j3d_stage_lighting();
    litMaterialsClassified_ += lighting != nullptr ? 1 : 0;
    const sb::native_render::J3dMaterialFamilyResult result =
        sb::native_render::classify_j3d_material(state, lighting,
                                                 {resolve_texture_thunk, &resolver}, classified);
    classifyResults_[result] += 1;
    families_[classified.family] += 1;
}

gcnport::HookResult GuestMaterialProbe::operator()(gcnport::GuestContext& guest) {
    entries_ += 1;

    const auto packet =
        static_cast<sb::title_adapter::GuestAddress>(guest.general_register(THIS_REGISTER));
    const sb::title_adapter::GuestMemory memory{read_through_guest_context, &guest};
    const sb::title_adapter::GuestReader reader(memory);

    sb::title_adapter::GuestAddress material = 0;
    if (!reader.word(packet + MAT_PACKET_MATERIAL, material)) {
        unreadablePackets_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    if (material == 0) {
        packetsWithoutMaterial_ += 1;
        return gcnport::HookResult::call_original_once();
    }
    if (materials_.size() < MAX_DISTINCT_MATERIALS) {
        materials_.insert(material);
    } else if (!materials_.contains(material)) {
        materialsPastTheSet_ += 1;
    }

    sb::native_render::J3dMaterialState state{};
    sb::title_adapter::GuestMaterial read{};
    const sb::title_adapter::GuestMaterialError error = read_guest_material(
        memory, material, {}, {}, /*hasVertexColor=*/false, /*hasNormal=*/false, state, read);
    materialErrors_[error] += 1;
    if (error == sb::title_adapter::GuestMaterialError::UnreadableMaterial) {
        unreadableMaterials_ += 1;
    }
    // Each block's own error is recorded whether or not the material as a whole succeeded, so a
    // single failing block is attributable rather than hidden behind one count.
    colorErrors_[read.colorError] += 1;
    texGenErrors_[read.texGenError] += 1;
    tevErrors_[read.tevError] += 1;
    errors_[read.pixelEngineError] += 1;
    if (error != sb::title_adapter::GuestMaterialError::None) {
        return gcnport::HookResult::call_original_once();
    }
    materialsRead_ += 1;

    colorKinds_[read.color.kind] += 1;
    if (read.color.lightingEnabled) {
        litMaterials_ += 1;
    }
    record(colorChannelCounts_, colorChannelCountsUntracked_, read.color.colorChannelCount);
    // The lit families gate on an exact (colour, alpha) channel-control pair. Reporting the pairs
    // this scene authors is what turns "unsupported colour channels" into a list of the exact
    // controls a family would have to recognise.
    record(channelControls_, channelControlsUntracked_,
           static_cast<std::uint32_t>(state.colorChannelControl) << 16U |
               state.alphaChannelControl);
    record(cullModes_, cullModesUntracked_, state.cullMode);
    if (read.texGen.recognised) {
        record(texGenCounts_, texGenCountsUntracked_, read.texGen.texGenCount);
    } else {
        unrecognisedTexGenBlocks_ += 1;
    }
    tevKinds_[read.tev.kind] += 1;
    record(tevStageCounts_, tevStageCountsUntracked_, read.tev.stageCount);

    sb::title_adapter::GuestTextureTable table{};
    if (read_texture_table(guest, memory, packet, table)) {
        measure_bindings(guest, state, table, read.tev.textureBindingCount);
        classify(guest, state, table);
    }
    measure_refusals(state);

    const sb::title_adapter::GuestPixelEngineBlock& block = read.pixelEngine;
    blocksRead_ += 1;
    kinds_[block.kind] += 1;
    if (block.hasFog) {
        blocksWithFog_ += 1;
        record(fogTypes_, fogTypesUntracked_, state.fog.type);
    }
    if (block.hasExplicitPixelPolicy) {
        blocksWithExplicitPolicy_ += 1;
        record(alphaCompareIds_, alphaCompareIdsUntracked_, block.alphaCompareId);
        record(depthModeIds_, depthModeIdsUntracked_, block.depthModeId);
        record(alphaCompares_, alphaComparesUntracked_, state.alphaCompare0);
        record(blendModes_, blendModesUntracked_, state.blendMode);
        record(blendFactors_, blendFactorsUntracked_,
               state.blendSourceFactor * 16U + state.blendDestinationFactor);
        record(depthCompares_, depthComparesUntracked_, state.depthCompare);

        idsReencoded_ += 1;
        if (encode_alpha_compare_id(state.alphaCompare0, state.alphaOperation,
                                    state.alphaCompare1) != block.alphaCompareId) {
            alphaIdsDisagreeing_ += 1;
        }
        if (encode_depth_mode_id(state.depthTest, state.depthCompare, state.depthWrite) !=
            block.depthModeId) {
            depthIdsDisagreeing_ += 1;
        }
    }

    if (reports_ < maxReports_) {
        reports_ += 1;
        std::printf(
            "gmse01_boot: material 0x%08x colour=%c%c%c%c chans=%u lit=%d cull=%u "
            "texgen=%u tev=%c%c%c%c stages=%u/%u\n",
            material, static_cast<char>(read.color.blockType >> 24U),
            static_cast<char>(read.color.blockType >> 16U),
            static_cast<char>(read.color.blockType >> 8U), static_cast<char>(read.color.blockType),
            read.color.colorChannelCount, read.color.lightingEnabled ? 1 : 0, state.cullMode,
            state.textureCoordinateCount, static_cast<char>(read.tev.blockType >> 24U),
            static_cast<char>(read.tev.blockType >> 16U),
            static_cast<char>(read.tev.blockType >> 8U), static_cast<char>(read.tev.blockType),
            read.tev.stageCount, read.tev.stageCapacity);
        std::printf("gmse01_boot:   pe=0x%08x type=%c%c%c%c fog=%d policy=%d "
                    "alphaId=0x%04x zId=0x%04x\n",
                    read.pixelEngineBlock, static_cast<char>(block.blockType >> 24U),
                    static_cast<char>(block.blockType >> 16U),
                    static_cast<char>(block.blockType >> 8U), static_cast<char>(block.blockType),
                    block.hasFog ? 1 : 0, block.hasExplicitPixelPolicy ? 1 : 0,
                    block.alphaCompareId, block.depthModeId);
        if (block.hasExplicitPixelPolicy) {
            std::printf("gmse01_boot:   alpha comp0=%u ref0=%u op=%u comp1=%u ref1=%u; blend "
                        "mode=%u src=%u dst=%u logic=%u; depth test=%d func=%u write=%d\n",
                        state.alphaCompare0, state.alphaReference0, state.alphaOperation,
                        state.alphaCompare1, state.alphaReference1, state.blendMode,
                        state.blendSourceFactor, state.blendDestinationFactor,
                        state.blendLogicOperation, state.depthTest ? 1 : 0, state.depthCompare,
                        state.depthWrite ? 1 : 0);
        }
        if (block.hasFog) {
            std::printf("gmse01_boot:   fog type=%u adj=%d centre=%u start=%f end=%f near=%f "
                        "far=%f colour=0x%08x\n",
                        state.fog.type, state.fog.rangeAdjustmentEnabled ? 1 : 0, state.fog.center,
                        static_cast<double>(state.fog.start), static_cast<double>(state.fog.end),
                        static_cast<double>(state.fog.near), static_cast<double>(state.fog.far),
                        state.fog.colorRgba8);
        }
    }

    return gcnport::HookResult::call_original_once();
}

void GuestMaterialProbe::report() const {
    std::printf(
        "gmse01_boot: guest material probe: %llu material packet(s) drawn, %llu material(s) "
        "read whole, %zu distinct material(s)%s\n",
        static_cast<unsigned long long>(entries_), static_cast<unsigned long long>(materialsRead_),
        materials_.size(), materialsPastTheSet_ != 0 ? " (a floor -- the set is full)" : "");
    if (unreadablePackets_ != 0 || packetsWithoutMaterial_ != 0 || unreadableMaterials_ != 0) {
        std::printf("gmse01_boot:   unreadable packets=%llu, packets with no material=%llu, "
                    "unreadable materials=%llu\n",
                    static_cast<unsigned long long>(unreadablePackets_),
                    static_cast<unsigned long long>(packetsWithoutMaterial_),
                    static_cast<unsigned long long>(unreadableMaterials_));
    }

    print_errors("material errors", materialErrors_);
    print_errors("colour block errors", colorErrors_);
    print_errors("texture generation errors", texGenErrors_);
    print_errors("tev block errors", tevErrors_);
    print_errors("pixel engine errors", errors_);

    std::printf("gmse01_boot:   colour block kinds:");
    if (colorKinds_.empty()) {
        std::printf(" none recorded");
    }
    for (const auto& [kind, count] : colorKinds_) {
        print_four_cc(guest_color_block_type(kind), count);
    }
    std::printf("\n");
    std::printf("gmse01_boot:   %llu material(s) light their first colour channel\n",
                static_cast<unsigned long long>(litMaterials_));
    print_histogram("colour channel counts", colorChannelCounts_, colorChannelCountsUntracked_);
    print_histogram("cull modes", cullModes_, cullModesUntracked_);
    print_histogram("texture coordinate counts", texGenCounts_, texGenCountsUntracked_);
    if (unrecognisedTexGenBlocks_ != 0) {
        std::printf("gmse01_boot:   %llu texture generation block(s) were not J3DTexGenBlockBasic "
                    "and were reported unsupported\n",
                    static_cast<unsigned long long>(unrecognisedTexGenBlocks_));
    }

    std::printf("gmse01_boot:   tev block kinds:");
    if (tevKinds_.empty()) {
        std::printf(" none recorded");
    }
    for (const auto& [kind, count] : tevKinds_) {
        print_four_cc(guest_tev_block_type(kind), count);
    }
    std::printf("\n");
    print_histogram("tev stage counts", tevStageCounts_, tevStageCountsUntracked_);

    std::printf("gmse01_boot:   pixel engine block kinds:");
    if (kinds_.empty()) {
        std::printf(" none recorded");
    }
    for (const auto& [kind, count] : kinds_) {
        print_four_cc(guest_pixel_engine_block_type(kind), count);
    }
    std::printf("\n");

    std::printf("gmse01_boot:   %llu block(s) carry fog, %llu carry an explicit raster policy\n",
                static_cast<unsigned long long>(blocksWithFog_),
                static_cast<unsigned long long>(blocksWithExplicitPolicy_));
    print_histogram("fog types", fogTypes_, fogTypesUntracked_);
    print_histogram("alpha compare ids", alphaCompareIds_, alphaCompareIdsUntracked_);
    print_histogram("depth mode ids", depthModeIds_, depthModeIdsUntracked_);
    print_histogram("alpha compare functions", alphaCompares_, alphaComparesUntracked_);
    print_histogram("blend modes", blendModes_, blendModesUntracked_);
    print_histogram("blend src*16+dst", blendFactors_, blendFactorsUntracked_);
    print_histogram("depth compare functions", depthCompares_, depthComparesUntracked_);

    print_errors("texture table errors", textureErrors_);
    std::printf("gmse01_boot:   %llu texture table(s) resolved, %llu binding(s), %llu distinct "
                "texture(s) decoded, %llu byte(s) of RGBA\n",
                static_cast<unsigned long long>(textureTablesRead_),
                static_cast<unsigned long long>(texturesBound_),
                static_cast<unsigned long long>(texturesDecoded_),
                static_cast<unsigned long long>(textureBytes_));
    // Two checks on the table layout rather than on the decoder. `mResourceCount` is a u16 at 0x00
    // with two bytes of padding behind it, and a texture number a TEV block binds must name a
    // resource the table holds. A count read at the wrong offset breaks both.
    std::printf("gmse01_boot:   %llu binding(s) named a texture past the table's count, %llu "
                "table(s) had a non-zero halfword where the count's padding belongs\n",
                static_cast<unsigned long long>(texturesPastTheTable_),
                static_cast<unsigned long long>(texturePaddingNonZero_));
    print_histogram("texture table sizes", textureTableSizes_, textureTableSizesUntracked_);
    if (textureSizes_.empty()) {
        std::printf("gmse01_boot:   texture dimensions: none recorded -- nothing was decoded\n");
    } else {
        std::printf("gmse01_boot:   texture dimensions:");
        for (const auto& [size, count] : textureSizes_) {
            std::printf(" %ux%u=%llu", size >> 16U, size & 0xffffU,
                        static_cast<unsigned long long>(count));
        }
        if (textureSizesUntracked_ != 0) {
            std::printf(" (+%llu past the tracked distinct values)",
                        static_cast<unsigned long long>(textureSizesUntracked_));
        }
        std::printf("\n");
    }
    if (channelControls_.empty()) {
        std::printf("gmse01_boot:   channel controls: none recorded\n");
    } else {
        std::printf("gmse01_boot:   channel controls (colour/alpha):");
        for (const auto& [pair, count] : channelControls_) {
            std::printf(" %04x/%04x=%llu", pair >> 16U, pair & 0xffffU,
                        static_cast<unsigned long long>(count));
        }
        if (channelControlsUntracked_ != 0) {
            std::printf(" (+%llu past the tracked distinct values)",
                        static_cast<unsigned long long>(channelControlsUntracked_));
        }
        std::printf("\n");
    }
    print_errors("raster policy results", rasterResults_,
                 sb::native_render::j3d_raster_policy_result_name);
    print_errors("unlit colour results", unlitResults_,
                 sb::native_render::j3d_unlit_material_result_name);
    print_errors("unlit textured results", unlitTexturedResults_,
                 sb::native_render::j3d_unlit_textured_result_name);
    print_errors("lit colour results", litColorResults_,
                 sb::native_render::j3d_lit_color_result_name);
    print_errors("lit textured results", litTexturedResults_,
                 sb::native_render::j3d_lit_textured_result_name);
    std::printf("gmse01_boot:   %llu material(s) were classified with a published stage light\n",
                static_cast<unsigned long long>(litMaterialsClassified_));
    std::printf("gmse01_boot:   material classification:");
    if (classifyResults_.empty()) {
        std::printf(" none recorded -- no material reached the classifier");
    }
    for (const auto& [result, count] : classifyResults_) {
        std::printf(" %s=%llu", sb::native_render::j3d_material_family_result_name(result),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   material families:");
    if (families_.empty()) {
        std::printf(" none recorded -- no material reached the classifier");
    }
    for (const auto& [family, count] : families_) {
        std::printf(" %s=%llu", sb::native_render::j3d_material_family_name(family),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");
    std::printf("gmse01_boot:   texture decode results:");
    if (decodeErrors_.empty()) {
        std::printf(" none recorded -- nothing was decoded");
    }
    for (const auto& [error, count] : decodeErrors_) {
        std::printf(" %s=%llu", sb::native_render::res_timg_decode_error_name(error),
                    static_cast<unsigned long long>(count));
    }
    std::printf("\n");

    // The one check here that could fail on its own. A zero denominator is not a pass: it means no
    // block ever carried a policy to check, which is reported as such rather than as agreement.
    std::printf("gmse01_boot:   re-encoded %llu id(s) from the guest lookup tables: %llu alpha "
                "compare and %llu depth mode disagreed with the id they were fetched for%s\n",
                static_cast<unsigned long long>(idsReencoded_),
                static_cast<unsigned long long>(alphaIdsDisagreeing_),
                static_cast<unsigned long long>(depthIdsDisagreeing_),
                idsReencoded_ == 0 ? " -- nothing was checked" : "");
}

} // namespace sunbright::gcnport_boot
