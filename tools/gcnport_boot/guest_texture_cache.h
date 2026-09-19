// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include <sunbright/native_render/jut_texture.h>
#include <sunbright/native_render/res_timg_decode.h>
#include <sunbright/title_adapter/guest_jut_texture.h>

#include "gcnport/guest_context.h"

namespace sunbright::gcnport_boot {

// Decodes the images a `JUTTexture` names out of guest memory, once each.
//
// Every 2D publisher that draws with a texture needs the same three steps -- plan the encoded and
// palette extents, read those bytes out of the guest, decode them -- and a pane drawn every frame
// must not decode every frame. Two publishers hold one of these now, a picture's layers and a
// window's five named roles, so the steps and the cache live here rather than once per probe.
class GuestTextureCache {
  public:
    [[nodiscard]] bool resolve(gcnport::GuestContext& guest,
                               const sb::title_adapter::GuestJutTexture& texture,
                               const sb::native_render::DecodedTexture*& decoded);

    void report() const;

  private:
    // Keyed by the address of the encoded bytes together with the extent and format read beside
    // them: a `JUTTexture` that is repointed at another resource changes that key, and one whose
    // pixels are rewritten in place does not -- which is why `revision` is carried into the view
    // rather than assumed.
    struct Key {
        std::uint32_t data = 0;
        std::uint32_t format = 0;
        std::uint32_t extent = 0;
        std::uint32_t palette = 0;
        auto operator<=>(const Key&) const = default;
    };

    std::uint64_t decoded_ = 0;
    std::uint64_t bytes_ = 0;
    std::map<sb::native_render::JutTextureError, std::uint64_t> errors_;
    std::map<Key, sb::native_render::DecodedTexture> cache_;
    std::vector<std::uint8_t> encodedBytes_;
    std::vector<std::uint8_t> paletteBytes_;
};

} // namespace sunbright::gcnport_boot
