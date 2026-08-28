#include <sunbright/native_render/frame.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

namespace {

using sb::native_render::Canvas;
using sb::native_render::Color;
using sb::native_render::DecodedImageView;
using sb::native_render::PictureCommand;
using sb::native_render::PictureDraw;
using sb::native_render::PictureFrame;
using sb::native_render::PictureFrameCollector;
using sb::native_render::PictureFrameError;
using sb::native_render::PictureFrameLimits;
using sb::native_render::PictureTexture;
using sb::native_render::Vec2;

constexpr Canvas kCanvas{{0, 0}, {640, 480}, {0, 0, 1280, 960}};

PictureCommand command(std::uint64_t instance, std::uint64_t resource, std::uint64_t revision) {
    PictureCommand picture{};
    picture.instance = instance;
    picture.positions = {Vec2{0, 0}, Vec2{16, 0}, Vec2{0, 16}, Vec2{16, 16}};
    picture.uv = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}};
    picture.material.textureCount = 1;
    picture.material.textures[0] = PictureTexture{
        .resource = resource, .revision = revision, .width = 1, .height = 1, .hasAlpha = true};
    return picture;
}

PictureDraw draw(PictureCommand picture, Canvas canvas = kCanvas) {
    return {canvas, picture};
}

DecodedImageView image(std::uint64_t resource, std::uint64_t revision,
                       const std::array<std::uint8_t, 4>& pixel) {
    return {resource, revision, 1, 1, pixel};
}

} // namespace

int main() {
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<std::uint8_t, 4> blue{0, 0, 255, 255};

    PictureFrameCollector collector{{.commands = 3, .images = 2, .decodedImageBytes = 8}};
    PictureFrame frame{};
    assert(!collector.seal(frame));
    assert(collector.error() == PictureFrameError::NotCollecting);
    assert(collector.begin(1280, 960, Color{0.1f, 0.2f, 0.3f, 1.0f}));
    assert(!collector.begin(1280, 960, {}));
    assert(collector.error() == PictureFrameError::AlreadyCollecting);

    auto firstCommand = command(10, 7, 1);
    auto firstImage = image(7, 1, red);
    assert(collector.append(draw(firstCommand), std::span(&firstImage, 1)));

    // Duplicate resource content is coalesced while command order remains exact.
    auto secondCommand = command(11, 7, 1);
    assert(collector.append(draw(secondCommand), std::span(&firstImage, 1)));
    assert(collector.decoded_image_bytes() == 4);
    assert(collector.seal(frame));
    assert(frame.draws.size() == 2);
    assert(frame.draws[0].picture.instance == 10);
    assert(frame.draws[1].picture.instance == 11);
    assert(frame.images.size() == 1);
    assert(frame.images[0].rgba8[0] == 255);

    // The collector owns source data: mutating the transient input after seal cannot change it.
    auto mutableRed = red;
    assert(collector.begin(1280, 960, {}));
    auto transientImage = image(7, 1, mutableRed);
    assert(collector.append(draw(firstCommand), std::span(&transientImage, 1)));
    mutableRed[0] = 0;
    assert(collector.seal(frame));
    assert(frame.images[0].rgba8[0] == 255);

    // Same semantic cache key with different bytes is an invalid producer, not a new image.
    assert(collector.begin(1280, 960, {}));
    assert(collector.append(draw(firstCommand), std::span(&firstImage, 1)));
    auto conflicting = image(7, 1, blue);
    assert(!collector.append(draw(secondCommand), std::span(&conflicting, 1)));
    assert(collector.error() == PictureFrameError::ConflictingImage);
    assert(collector.seal(frame));
    assert(frame.draws.size() == 1);
    assert(frame.images.size() == 1);

    // A changed revision is intentionally distinct and forms the known-different control.
    assert(collector.begin(1280, 960, {}));
    assert(collector.append(draw(firstCommand), std::span(&firstImage, 1)));
    auto changedCommand = command(12, 7, 2);
    auto changedImage = image(7, 2, blue);
    const Canvas inset{{0, 0}, {320, 240}, {100, 50, 640, 480}};
    assert(collector.append(draw(changedCommand, inset), std::span(&changedImage, 1)));
    assert(collector.seal(frame));
    assert(frame.images.size() == 2);
    assert(frame.images[0].rgba8[0] != frame.images[1].rgba8[0]);
    assert(frame.draws[0].canvas != frame.draws[1].canvas);

    PictureFrameCollector commandBound{{.commands = 1, .images = 2, .decodedImageBytes = 8}};
    assert(commandBound.begin(1280, 960, {}));
    assert(commandBound.append(draw(firstCommand), std::span(&firstImage, 1)));
    assert(!commandBound.append(draw(secondCommand), std::span(&firstImage, 1)));
    assert(commandBound.error() == PictureFrameError::CommandLimit);

    PictureFrameCollector imageBound{{.commands = 2, .images = 1, .decodedImageBytes = 8}};
    assert(imageBound.begin(1280, 960, {}));
    assert(imageBound.append(draw(firstCommand), std::span(&firstImage, 1)));
    assert(!imageBound.append(draw(changedCommand), std::span(&changedImage, 1)));
    assert(imageBound.error() == PictureFrameError::ImageLimit);

    PictureFrameCollector byteBound{{.commands = 2, .images = 2, .decodedImageBytes = 4}};
    assert(byteBound.begin(1280, 960, {}));
    assert(byteBound.append(draw(firstCommand), std::span(&firstImage, 1)));
    assert(!byteBound.append(draw(changedCommand), std::span(&changedImage, 1)));
    assert(byteBound.error() == PictureFrameError::ImageByteLimit);

    PictureFrameCollector invalid{};
    assert(!invalid.begin(0, 960, {}));
    assert(invalid.error() == PictureFrameError::InvalidFrame);

    // The production sink callback exercises the same append implementation.
    assert(invalid.begin(1280, 960, {}));
    const auto sink = invalid.sink();
    const PictureDraw firstDraw = draw(firstCommand);
    assert(sink.submit(firstDraw, std::span(&firstImage, 1), sink.context));
    invalid.reset();
    assert(!invalid.append(firstDraw, std::span(&firstImage, 1)));
    assert(invalid.error() == PictureFrameError::NotCollecting);
}
