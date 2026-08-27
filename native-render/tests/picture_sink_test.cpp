#include <sunbright/native_render/picture_sink.h>

#include <cassert>

namespace {

void receive(const sb::native_render::PictureCommand& command, void* context) {
    *static_cast<std::uint64_t*>(context) = command.instance;
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
    sb::native_render::set_picture_sink({});
    assert(!sb::native_render::has_picture_sink());
    assert(!sb::native_render::submit_picture(valid_picture()));

    std::uint64_t received = 0;
    sb::native_render::set_picture_sink({receive, &received});
    assert(sb::native_render::has_picture_sink());
    assert(sb::native_render::submit_picture(valid_picture()));
    assert(received == 7);

    auto invalid = valid_picture();
    invalid.material.textureCount = 0;
    received = 0;
    assert(!sb::native_render::submit_picture(invalid));
    assert(received == 0);

    sb::native_render::set_picture_sink({});
}
