// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>

#include <sunbright/native_render/j3d_material_family.h>

#include <sunbright/title_adapter/guest_j3d_display_list.h>
#include <sunbright/title_adapter/guest_j3d_material.h>
#include <sunbright/title_adapter/guest_j3d_shape.h>
#include <sunbright/title_adapter/guest_j3d_texture.h>

#include "guest_draw_publisher.h"

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// A native hook that composes one drawable unit of GMSE01 -- a shape, the material it is being
// drawn with, that material's textures, and the stage light in force -- and classifies it into a
// `native_render::ModelMaterial`.
//
// It sits on `J3DShape::draw` (0x802e0390) rather than on `J3DMatPacket::draw`, and that is the
// whole point of it. Two of the inputs the shared classifiers take, `hasVertexColor` and
// `hasNormal`, are properties of the geometry and not of the material: they come from the shape's
// own vertex layout. At the material packet there is no shape in hand, and passing them as false
// refused 9,394 material packets for `missing normal` alone -- a refusal the probe caused rather
// than measured. This is also where the decomp runtime composes its draw
// (`sb_native_j3d_shape_submit`), so the two runtimes reach the shared classifier the same way.
//
// The material comes from `j3dSys.mMatPacket`, which `J3DMatPacket::draw` publishes before it draws
// the shape packets beneath it -- the same place the decomp reads it. Nothing is replaced: every
// entry ends in `call_original_once`.
class GuestModelProbe {
  public:
    GuestModelProbe(sb::title_adapter::GuestAddress system, std::uint64_t max_reports,
                    DrawDiagnosticMode mode, FrameDrawBudget* budget,
                    std::uint64_t log_frame) noexcept
        : system_(system), maxReports_(max_reports), publisher_(system, mode, budget, log_frame) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    static constexpr std::size_t MAX_DISTINCT_VALUES = 32;

    // Draws are per shape per frame, so the distinct pairs are bounded by the scene's models.
    static constexpr std::size_t MAX_DISTINCT_DRAWS = 8192;

    void record(std::map<std::uint32_t, std::uint64_t>& histogram, std::uint64_t& untracked,
                std::uint32_t value);
    void record_refusals(const sb::native_render::J3dFamilyRefusals& refusals,
                         const sb::native_render::J3dMaterialState& state,
                         sb::title_adapter::GuestAddress material);
    void report_refused_materials() const;

    // Resolves a texture number the way the material did, decoding once per distinct image. The
    // classifier is handed this and cannot tell it is reading a guest.
    //
    // It carries both records a texture can come from, and the material state that maps a texture
    // number back to the texture map it was bound into -- which is what the two records are keyed
    // by on their respective sides.
    struct TextureResolver {
        GuestModelProbe* probe = nullptr;
        gcnport::GuestContext* guest = nullptr;
        sb::title_adapter::GuestTextureTable table{};
        const sb::title_adapter::GuestDisplayListTextures* displayList = nullptr;
        const sb::native_render::J3dMaterialState* state = nullptr;
    };

    [[nodiscard]] bool resolve_texture(TextureResolver& resolver, std::uint16_t number,
                                       sb::native_render::DecodedTexture& texture,
                                       sb::native_render::ResTimgDecodeError& error);
    static bool resolve_texture_thunk(std::uint16_t number,
                                      sb::native_render::DecodedTexture& texture,
                                      sb::native_render::ResTimgDecodeError& error, void* context);

    sb::title_adapter::GuestAddress system_ = 0;
    std::uint64_t maxReports_ = 0;
    std::uint64_t entries_ = 0;
    std::uint64_t reports_ = 0;
    std::uint64_t unreadableShapes_ = 0;
    std::uint64_t noMaterialPacket_ = 0;
    std::uint64_t unreadableMaterialPackets_ = 0;
    std::uint64_t unreadableMaterials_ = 0;
    std::uint64_t composed_ = 0;
    std::uint64_t withNormal_ = 0;
    std::uint64_t withVertexColor_ = 0;
    std::uint64_t withLighting_ = 0;
    std::uint64_t classified_ = 0;
    std::uint64_t texturesDecoded_ = 0;
    // The two records apart. A run that resolves every texture from one of them is saying something
    // about the scene; a run that never reaches the display list at all is saying something about
    // this hook, and the totals are what tell those two apart.
    std::uint64_t texturesFromDisplayList_ = 0;
    std::uint64_t texturesFromTable_ = 0;
    std::uint64_t textureNumbersWithoutMap_ = 0;
    std::uint64_t textureBytes_ = 0;
    std::uint64_t drawsPastTheSet_ = 0;

    std::set<std::uint64_t> draws_;
    // Keyed by where the image came from rather than by its texture number: the same number names
    // different images in different materials, and the two records this resolves from address
    // different things -- a resource header on one side, an image on the other.
    std::map<std::uint64_t, sb::native_render::DecodedTexture> textureCache_;
    std::map<sb::title_adapter::GuestShapeError, std::uint64_t> shapeErrors_;
    std::map<sb::title_adapter::GuestMaterialError, std::uint64_t> materialErrors_;
    std::map<sb::title_adapter::GuestTextureError, std::uint64_t> tableErrors_;
    std::map<sb::title_adapter::GuestDisplayListError, std::uint64_t> displayListErrors_;
    std::map<sb::native_render::ResTimgDecodeError, std::uint64_t> decodeErrors_;
    std::map<sb::native_render::J3dMaterialFamilyResult, std::uint64_t> results_;
    std::map<sb::native_render::J3dMaterialFamily, std::uint64_t> families_;
    // Why each family turned down a draw that no family accepted, keyed by the family and then by
    // that family's own refusal name. This is the measurement that names the next material port:
    // a count of successes alone cannot say what the failures are waiting on.
    std::map<sb::native_render::J3dMaterialFamily, std::map<std::string, std::uint64_t>> refusals_;
    // One record per distinct material that no family accepted. The scene holds only tens of
    // distinct materials, so every refused one fits -- and a full record of each says what a new
    // family would have to accept, which neither a gate name nor a value histogram can.
    struct RefusedMaterial {
        std::uint64_t draws = 0;
        sb::native_render::J3dMaterialState state{};
        // Kept per material, not only in aggregate: the aggregate says how many draws each family
        // turned away and for what, but not which gate a particular material fails in each family,
        // which is the question a port asks.
        sb::native_render::J3dFamilyRefusals refusals{};
    };

    // What the draws no family accepted are actually authored as. The refusal names say which
    // gate they fail; these say what value fails it, which is what a new family would be written
    // against. Both are needed: a gate name without its values cannot be ported from.
    std::map<sb::title_adapter::GuestAddress, RefusedMaterial> refusedMaterials_;
    std::uint64_t refusedMaterialsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> refusedChannels_;
    std::uint64_t refusedChannelsUntracked_ = 0;
    std::map<std::uint32_t, std::uint64_t> refusedStageCounts_;
    std::uint64_t refusedStageCountsUntracked_ = 0;
    GuestDrawPublisher publisher_;
    std::uint64_t drawsPublished_ = 0;
    std::map<std::uint32_t, std::uint64_t> textureCounts_;
    std::uint64_t textureCountsUntracked_ = 0;
};

} // namespace sunbright::gcnport_boot
