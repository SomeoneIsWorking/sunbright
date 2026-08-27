#pragma once

#include <sunbright/native_render/picture.h>

namespace sb::native_render {

using PictureSubmit = void (*)(const PictureCommand& command, void* context);

struct PictureSink {
    PictureSubmit submit = nullptr;
    void* context = nullptr;
};

// The game and renderer execute on one thread. A backend installs this sink for its frame lifetime;
// absent a sink, adapters stay inert and the retained renderer body remains the only output path.
void set_picture_sink(PictureSink sink) noexcept;
[[nodiscard]] bool has_picture_sink() noexcept;
[[nodiscard]] bool submit_picture(const PictureCommand& command) noexcept;

} // namespace sb::native_render
