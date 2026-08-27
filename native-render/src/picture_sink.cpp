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

bool submit_picture(const PictureCommand& command) noexcept {
    if (g_sink.submit == nullptr || !valid(command))
        return false;
    g_sink.submit(command, g_sink.context);
    return true;
}

} // namespace sb::native_render
