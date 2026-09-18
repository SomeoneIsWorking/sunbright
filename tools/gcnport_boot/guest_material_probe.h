// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <set>

#include <sunbright/native_render/j3d_material_family.h>

#include <sunbright/title_adapter/guest_j3d_material.h>
#include <sunbright/title_adapter/guest_j3d_texture.h>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// A native hook that reads the material GMSE01 is about to draw with, through the same
// `title-adapter` code the eventual override will use, and reports what it found.
//
// It reads the whole material -- colour, texture generation, colour stages and pixel engine -- into
// one `native_render::J3dMaterialState`, which is the struct the decomp adapter fills from its own
// object layout and the only input the shared material classifiers take. The two geometry flags
// that state carries, `hasVertexColor` and `hasNormal`, describe the shape rather than the material
// and are not resolvable at this hook, so they are passed as false and no measurement here depends
// on them.
//
// The hook sits on J3DMatPacket::draw (0x802edc38), which is where the title itself resolves a
// material: it publishes the packet's texture and the packet into j3dSys and then calls
// `mpMaterial->load()`. That makes the packet's own `mpMaterial` the material every shape packet
// beneath it is about to be drawn with -- one entry per material rather than one per shape, and no
// dependence on j3dSys having been written yet.
//
// Like the shape probe, it replaces nothing: every entry ends in `call_original_once`, so the
// title configures the rasteriser exactly as it did. `title-adapter`'s own test proves the offsets
// against a synthetic image it wrote itself, which cannot say whether those offsets describe the
// objects GMSE01 actually builds.
//
// One measurement here can fail on its own rather than merely agreeing with itself. J3D does not
// store an alpha-compare or depth-mode configuration; it stores a packed id and looks the fields up
// in a table built at boot, and the encoding that produced the id is known:
//
//     alphaCmpID = (comp0 << 5) + (op << 3) + comp1
//     zModeID    = compareEnable * 0x10 + func * 2 + updateEnable
//
// So every row read back out of the guest table can be re-encoded and compared with the id it was
// fetched for. A table that was never built, or a table read at the wrong address or the wrong
// stride, answers zeroes or a neighbour's row -- all of which re-encode to the wrong id. That check
// is what tells "the game's table says GX_NEVER" apart from "this read never reached the table".
// It deliberately does not classify: `hasVertexColor` and `hasNormal` belong to the shape, and
// there is no shape here. `GuestModelProbe`, on `J3DShape::draw`, owns that composition.
// The material's textures are resolved and decoded too, through `native_render::decode_res_timg` --
// the same decoder the decomp path has always used, reading the guest header directly. Decoding is
// per distinct resource rather than per draw: a texture is the same bytes every time it is bound,
// and decoding 44,000 binds of 50 textures would measure the cache rather than the decoder.
class GuestMaterialProbe {
  public:
    explicit GuestMaterialProbe(std::uint64_t max_reports) noexcept : maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    // Blend factors, compare functions and block kinds are drawn from small GX enumerations, so
    // the distinct values are few by construction. The cap means a misread cannot turn a histogram
    // into unbounded growth, and anything past it is counted rather than dropped.
    static constexpr std::size_t MAX_DISTINCT_VALUES = 32;

    // Materials are per-model and authored, so a stage's worth is thousands at most. Past this the
    // count keeps rising while the set stops growing, which is reported as a floor rather than a
    // total.
    static constexpr std::size_t MAX_DISTINCT_MATERIALS = 8192;

    void record(std::map<std::uint32_t, std::uint64_t>& histogram, std::uint64_t& untracked,
                std::uint32_t value);

    // What `classify_j3d_material` is given to turn a texture number into pixels. The probe's own
    // cache and counters live behind it, so the classifier never learns it is reading a guest.
    struct TextureResolver {
        GuestMaterialProbe* probe = nullptr;
        gcnport::GuestContext* guest = nullptr;
        sb::title_adapter::GuestTextureTable table{};
    };

    [[nodiscard]] bool read_texture_table(gcnport::GuestContext& guest,
                                          const sb::title_adapter::GuestMemory& memory,
                                          sb::title_adapter::GuestAddress packet,
                                          sb::title_adapter::GuestTextureTable& table);
    [[nodiscard]] bool resolve_texture(gcnport::GuestContext& guest,
                                       const sb::title_adapter::GuestTextureTable& table,
                                       std::uint16_t number,
                                       sb::native_render::DecodedTexture& texture,
                                       sb::native_render::ResTimgDecodeError& error);
    static bool resolve_texture_thunk(std::uint16_t number,
                                      sb::native_render::DecodedTexture& texture,
                                      sb::native_render::ResTimgDecodeError& error, void* context);
    void measure_bindings(gcnport::GuestContext& guest,
                          const sb::native_render::J3dMaterialState& state,
                          const sb::title_adapter::GuestTextureTable& table,
                          std::uint8_t bindingCount);

    std::uint64_t maxReports_ = 0;
    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t packetsWithoutMaterial_ = 0;
    std::uint64_t unreadablePackets_ = 0;
    std::uint64_t unreadableMaterials_ = 0;
    std::uint64_t materialsRead_ = 0;
    std::uint64_t blocksRead_ = 0;
    std::uint64_t blocksWithFog_ = 0;
    std::uint64_t blocksWithExplicitPolicy_ = 0;
    std::uint64_t idsReencoded_ = 0;
    std::uint64_t alphaIdsDisagreeing_ = 0;
    std::uint64_t depthIdsDisagreeing_ = 0;
    std::uint64_t materialsPastTheSet_ = 0;

    std::set<std::uint32_t> materials_;
    std::map<sb::title_adapter::GuestAddress, sb::native_render::DecodedTexture> textureCache_;
    std::uint64_t textureTablesRead_ = 0;
    std::uint64_t texturesBound_ = 0;
    std::uint64_t texturesPastTheTable_ = 0;
    std::uint64_t texturesDecoded_ = 0;
    std::uint64_t textureBytes_ = 0;
    std::uint64_t texturePaddingNonZero_ = 0;
    std::map<sb::title_adapter::GuestTextureError, std::uint64_t> textureErrors_;
    std::map<std::uint32_t, std::uint64_t> textureTableSizes_;
    std::uint64_t textureTableSizesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> textureSizes_;
    std::uint64_t textureSizesUntracked_ = 0;
    std::map<sb::native_render::ResTimgDecodeError, std::uint64_t> decodeErrors_;
    std::map<sb::title_adapter::GuestMaterialError, std::uint64_t> materialErrors_;
    std::map<sb::title_adapter::GuestColorError, std::uint64_t> colorErrors_;
    std::map<sb::title_adapter::GuestTexGenError, std::uint64_t> texGenErrors_;
    std::map<sb::title_adapter::GuestTevError, std::uint64_t> tevErrors_;
    std::map<sb::title_adapter::GuestPixelEngineError, std::uint64_t> errors_;
    std::map<sb::title_adapter::GuestColorBlockKind, std::uint64_t> colorKinds_;
    std::map<sb::title_adapter::GuestTevBlockKind, std::uint64_t> tevKinds_;
    std::map<sb::title_adapter::GuestPixelEngineKind, std::uint64_t> kinds_;
    std::uint64_t litMaterials_ = 0;
    std::uint64_t unrecognisedTexGenBlocks_ = 0;
    std::map<std::uint32_t, std::uint64_t> channelControls_;
    std::uint64_t channelControlsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> colorChannelCounts_;
    std::uint64_t colorChannelCountsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> texGenCounts_;
    std::uint64_t texGenCountsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> tevStageCounts_;
    std::uint64_t tevStageCountsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> cullModes_;
    std::uint64_t cullModesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> alphaCompareIds_;
    std::uint64_t alphaCompareIdsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> depthModeIds_;
    std::uint64_t depthModeIdsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> alphaCompares_;
    std::uint64_t alphaComparesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> blendModes_;
    std::uint64_t blendModesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> blendFactors_;
    std::uint64_t blendFactorsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> depthCompares_;
    std::uint64_t depthComparesUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> fogTypes_;
    std::uint64_t fogTypesUntracked_ = 0;
};

} // namespace sunbright::gcnport_boot
