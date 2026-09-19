// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <sunbright/native_render/jut_texture.h>
#include <sunbright/native_render/picture.h>
#include <sunbright/native_render/res_timg_decode.h>
#include <sunbright/title_adapter/guest_j2d_picture.h>

#include "frame_draw_budget.h"
#include "guest_j2d_context_probe.h"

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

namespace sunbright::gcnport_boot {

// Publishes GMSE01's 2D composite: the quads a `J2DPicture` draws.
//
// The model probe publishes what the title draws as geometry, which at the title screen is the sky
// and the sea. Everything a player would call the title screen -- the logo, the letters that fly
// into it, the shine, "PRESS START!", the copyright line -- is not geometry at all. Each is one
// textured quad drawn by a pane, through a path no model hook is on, and a run that published only
// models was not rendering a partly-wrong title screen but a complete one with a whole pass
// missing.
//
// It sits on `J2DPicture::drawSelf(int, int, Mtx*)` (0x802cc7c0), which is where `J2DPane::draw`
// hands a pane the transform of the context it resolved. Entering there is deliberate: by then the
// pane's global matrix, clip rectangle and inherited opacity are final and the parent transform is
// the argument in hand, so everything the quad needs is readable without running any of the body.
//
// Nothing here decides what a pane means. `resolve_picture_layout` owns the crop, binding, mirror
// and wrap contract and `decode_jut_texture` owns the image; this reads the guest's fields and
// hands them over.
class GuestPictureProbe {
  public:
    // `context` may be null, which is a run that did not ask for the screen to be followed; every
    // picture is then counted as having no canvas rather than published into a guessed one.
    // `budget` may be null, which is an unbounded run.
    GuestPictureProbe(const GuestJ2dContextProbe* context, FrameDrawBudget* budget,
                      std::uint64_t max_reports) noexcept
        : context_(context), budget_(budget), maxReports_(max_reports) {}

    gcnport::HookResult operator()(gcnport::GuestContext& guest);

    void report() const;

  private:
    // One decoded image, kept so a pane drawn every frame decodes once. Keyed by the address of
    // the encoded bytes together with the extent and format read beside them: a `JUTTexture` that
    // is repointed at another resource changes that key, and one whose pixels are rewritten in
    // place does not -- which is why `revision` is carried into the view rather than assumed.
    struct TextureKey {
        std::uint32_t data = 0;
        std::uint32_t format = 0;
        std::uint32_t extent = 0;
        std::uint32_t palette = 0;
        auto operator<=>(const TextureKey&) const = default;
    };

    [[nodiscard]] bool resolve_texture(gcnport::GuestContext& guest,
                                       const sb::title_adapter::GuestJutTexture& texture,
                                       const sb::native_render::DecodedTexture*& decoded);

    const GuestJ2dContextProbe* context_ = nullptr;
    FrameDrawBudget* budget_ = nullptr;
    std::uint64_t maxReports_ = 0;
    std::uint64_t reports_ = 0;

    std::uint64_t entries_ = 0;
    std::uint64_t withoutCanvas_ = 0;
    std::uint64_t unreadableTransform_ = 0;
    std::uint64_t withheldByBudget_ = 0;
    std::uint64_t unresolvedLayout_ = 0;
    std::uint64_t withoutSink_ = 0;
    std::uint64_t invalidBlendFactor_ = 0;
    std::uint64_t submitted_ = 0;
    std::uint64_t acceptedBySink_ = 0;
    std::uint64_t texturesDecoded_ = 0;
    std::uint64_t textureBytes_ = 0;
    // Panes whose clip rectangle is smaller than their bounds. `J2DScreen::draw` is what decides
    // whether a subtree clips to its parent and this probe is not on it, so the clip is not
    // applied; this counts the panes for which that could have mattered. Zero is the answer that
    // says the gap is empty rather than unexamined.
    std::uint64_t panesWithNarrowerClip_ = 0;
    std::map<sb::title_adapter::GuestPictureError, std::uint64_t> pictureErrors_;
    std::map<sb::native_render::JutTextureError, std::uint64_t> textureErrors_;
    std::map<std::uint32_t, std::uint64_t> textureCounts_;
    std::map<TextureKey, sb::native_render::DecodedTexture> textureCache_;
    std::vector<std::uint8_t> encodedBytes_;
    std::vector<std::uint8_t> paletteBytes_;
};

} // namespace sunbright::gcnport_boot
