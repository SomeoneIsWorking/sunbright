#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/semantic_draw.h>

#include <cstdint>
#include <span>

namespace sb::native_render {

using SemanticSubmit = bool (*)(const SemanticDraw& draw, std::span<const DecodedImageView> images,
                                void* context);

struct SemanticSink {
    SemanticSubmit submit = nullptr;
    void* context = nullptr;
};

struct SemanticSinkLease {
    std::uint64_t value = 0;
    [[nodiscard]] explicit operator bool() const noexcept { return value != 0; }
    bool operator==(const SemanticSinkLease&) const = default;
};

// The game and renderer execute on one thread. A backend installs this sink for its frame lifetime;
// absent a sink, adapters stay inert and the retained renderer body remains the only output path.
[[nodiscard]] bool claim_semantic_sink(SemanticSink sink, SemanticSinkLease& lease) noexcept;
[[nodiscard]] bool release_semantic_sink(SemanticSinkLease lease) noexcept;
[[nodiscard]] bool owns_semantic_sink(SemanticSinkLease lease) noexcept;
[[nodiscard]] bool has_semantic_sink() noexcept;
// A command and every decoded resource it references are accepted as one operation. This prevents
// a deferred collector from observing a command whose guest/native pixel storage was reused before
// the image was copied.
[[nodiscard]] bool submit_picture(const PictureDraw& draw,
                                  std::span<const DecodedImageView> images) noexcept;
[[nodiscard]] bool submit_glyph(const GlyphDraw& draw,
                                std::span<const DecodedImageView> images) noexcept;
[[nodiscard]] bool submit_solid_rectangle(const SolidRectangleDraw& draw) noexcept;

} // namespace sb::native_render
