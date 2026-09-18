// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_material_probe.h"

#include <array>
#include <cstdio>
#include <span>

namespace sunbright::gcnport_boot {
namespace {

// Retail J3DMatPacket, from decomp/sms/include/JSystem/J3D/J3DGraphBase/J3DPacket.hpp. Everything
// below the material pointer belongs to `title_adapter`, which owns those layouts.
constexpr sb::title_adapter::GuestAddress MAT_PACKET_MATERIAL = 0x38;

// `this` is in r3 on entry to J3DMatPacket::draw, whose own first instructions read 0x34(r3).
constexpr std::size_t THIS_REGISTER = 3;

bool read_through_guest_context(sb::title_adapter::GuestAddress address,
                                std::span<std::uint8_t> destination, void* context) {
    auto* const guest = static_cast<gcnport::GuestContext*>(context);
    return guest->read_memory(address, std::as_writable_bytes(destination));
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
template <typename Error>
void print_errors(const char* label, const std::map<Error, std::uint64_t>& histogram) {
    std::printf("gmse01_boot:   %s:", label);
    if (histogram.empty()) {
        std::printf(" none recorded -- nothing was read");
    }
    for (const auto& [error, count] : histogram) {
        std::printf(" %s=%llu", name(error), static_cast<unsigned long long>(count));
    }
    std::printf("\n");
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
    record(cullModes_, cullModesUntracked_, state.cullMode);
    if (read.texGen.recognised) {
        record(texGenCounts_, texGenCountsUntracked_, read.texGen.texGenCount);
    } else {
        unrecognisedTexGenBlocks_ += 1;
    }
    tevKinds_[read.tev.kind] += 1;
    record(tevStageCounts_, tevStageCountsUntracked_, read.tev.stageCount);

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
