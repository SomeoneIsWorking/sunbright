#include <sunbright/native_render/picture_sink.h>

namespace sb::native_render {
namespace {

PictureSink g_sink{};

} // namespace

void set_picture_sink(PictureSink sink) noexcept {
    g_sink = sink;
}

bool has_picture_sink() noexcept {
    return g_sink.submit != nullptr;
}

bool submit_picture(const PictureCommand& command,
                    std::span<const DecodedImageView> images) noexcept {
    if (g_sink.submit == nullptr || !valid(command) ||
        images.size() != command.material.textureCount)
        return false;
    for (std::size_t index = 0; index < images.size(); ++index) {
        const PictureTexture& texture = command.material.textures[index];
        const DecodedImageView& image = images[index];
        if (!valid(image) || image.resource != texture.resource ||
            image.revision != texture.revision || image.width != texture.width ||
            image.height != texture.height) {
            return false;
        }
    }
    return g_sink.submit(command, images, g_sink.context);
}

} // namespace sb::native_render
