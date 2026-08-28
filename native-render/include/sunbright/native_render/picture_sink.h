#pragma once

#include <sunbright/native_render/image.h>
#include <sunbright/native_render/picture.h>

#include <span>

namespace sb::native_render {

using PictureSubmit = bool (*)(const PictureCommand& command,
                               std::span<const DecodedImageView> images, void* context);

struct PictureSink {
    PictureSubmit submit = nullptr;
    void* context = nullptr;
};

// The game and renderer execute on one thread. A backend installs this sink for its frame lifetime;
// absent a sink, adapters stay inert and the retained renderer body remains the only output path.
void set_picture_sink(PictureSink sink) noexcept;
[[nodiscard]] bool has_picture_sink() noexcept;
// A command and every decoded resource it references are accepted as one operation. This prevents
// a deferred collector from observing a command whose guest/native pixel storage was reused before
// the image was copied.
[[nodiscard]] bool submit_picture(const PictureCommand& command,
                                  std::span<const DecodedImageView> images) noexcept;

} // namespace sb::native_render
