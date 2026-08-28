#include <sunbright/native_render/picture_sink.h>

#include <array>
#include <cassert>

namespace {

bool receive(const sb::native_render::PictureCommand& command,
             std::span<const sb::native_render::DecodedImageView> images, void* context) {
    assert(images.size() == 1);
    *static_cast<std::uint64_t*>(context) = command.instance;
    return true;
}

bool reject(const sb::native_render::PictureCommand&,
            std::span<const sb::native_render::DecodedImageView>, void*) {
    return false;
}

sb::native_render::PictureCommand valid_picture() {
    sb::native_render::PictureCommand picture{};
    picture.instance = 7;
    picture.positions = {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
    picture.uv = {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}};
    picture.material.textureCount = 1;
    picture.material.textures[0].resource = 1;
    picture.material.textures[0].width = 1;
    picture.material.textures[0].height = 1;
    return picture;
}

} // namespace

int main() {
    const std::array<std::uint8_t, 4> rgba{1, 2, 3, 4};
    const sb::native_render::DecodedImageView image{
        .resource = 1, .width = 1, .height = 1, .rgba8 = rgba};
    sb::native_render::set_picture_sink({});
    assert(!sb::native_render::has_picture_sink());
    assert(!sb::native_render::submit_picture(valid_picture(), std::span(&image, 1)));

    std::uint64_t received = 0;
    sb::native_render::set_picture_sink({receive, &received});
    assert(sb::native_render::has_picture_sink());
    assert(sb::native_render::submit_picture(valid_picture(), std::span(&image, 1)));
    assert(received == 7);

    auto invalid = valid_picture();
    invalid.material.textureCount = 0;
    received = 0;
    assert(!sb::native_render::submit_picture(invalid, std::span(&image, 1)));
    assert(received == 0);

    auto mismatched = image;
    mismatched.resource = 2;
    assert(!sb::native_render::submit_picture(valid_picture(), std::span(&mismatched, 1)));
    assert(received == 0);

    auto shortImage = image;
    shortImage.rgba8 = std::span(rgba).first(3);
    assert(!sb::native_render::submit_picture(valid_picture(), std::span(&shortImage, 1)));
    assert(received == 0);

    sb::native_render::set_picture_sink({reject, nullptr});
    assert(!sb::native_render::submit_picture(valid_picture(), std::span(&image, 1)));

    sb::native_render::set_picture_sink({});
}
