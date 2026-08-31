#include <sunbright/native_render/frame.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>

namespace {

using sb::native_render::Canvas;
using sb::native_render::Color;
using sb::native_render::DecodedImageView;
using sb::native_render::GlyphDraw;
using sb::native_render::LitTexturedAlphaMaskMaterial;
using sb::native_render::MeshResourceView;
using sb::native_render::MeshVertex;
using sb::native_render::ModelDraw;
using sb::native_render::PictureCommand;
using sb::native_render::PictureDraw;
using sb::native_render::PictureTexture;
using sb::native_render::SemanticFrame;
using sb::native_render::SemanticFrameCollector;
using sb::native_render::SemanticFrameError;
using sb::native_render::SemanticFrameLimits;
using sb::native_render::SolidRectangleDraw;
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

GlyphDraw glyph(std::uint64_t instance, std::uint64_t resource, std::uint64_t revision,
                Canvas canvas = kCanvas) {
    const PictureCommand picture = command(instance, resource, revision);
    return {canvas,
            {.instance = instance,
             .code = 'A',
             .positions = picture.positions,
             .uv = picture.uv,
             .corner = picture.corner,
             .atlas = picture.material.textures[0]}};
}

SolidRectangleDraw solid(std::uint64_t instance, Canvas canvas = kCanvas) {
    return {
        canvas,
        {.instance = instance,
         .source = sb::native_render::SolidRectangleSource::Gc2dFillRect,
         .positions = {Vec2{2, 2}, Vec2{14, 2}, Vec2{2, 14}, Vec2{14, 14}},
         .corner = {Color{1, 0, 0, 1}, Color{0, 1, 0, 1}, Color{0, 0, 1, 1}, Color{1, 1, 1, 1}}}};
}

DecodedImageView image(std::uint64_t resource, std::uint64_t revision,
                       const std::array<std::uint8_t, 4>& pixel) {
    return {resource, revision, 1, 1, pixel};
}

} // namespace

int main() {
    const std::array<std::uint8_t, 4> red{255, 0, 0, 255};
    const std::array<std::uint8_t, 4> blue{0, 0, 255, 255};

    SemanticFrameCollector collector{{.commands = 3, .images = 2, .decodedImageBytes = 8}};
    SemanticFrame frame{};
    assert(!collector.seal(frame));
    assert(collector.error() == SemanticFrameError::NotCollecting);
    assert(collector.begin(1280, 960, Color{0.1f, 0.2f, 0.3f, 1.0f}));
    assert(!collector.begin(1280, 960, {}));
    assert(collector.error() == SemanticFrameError::AlreadyCollecting);

    auto firstCommand = command(10, 7, 1);
    auto firstImage = image(7, 1, red);
    assert(collector.append(draw(firstCommand), std::span(&firstImage, 1)));

    // Duplicate resource content is coalesced while mixed command order remains exact.
    assert(collector.append_solid_rectangle(solid(20)));
    auto secondCommand = command(11, 7, 1);
    assert(collector.append_glyph(glyph(11, 7, 1), std::span(&firstImage, 1)));
    assert(collector.decoded_image_bytes() == 4);
    assert(collector.seal(frame));
    assert(frame.draws.size() == 3);
    assert(std::get<PictureDraw>(frame.draws[0]).picture.instance == 10);
    assert(std::get<SolidRectangleDraw>(frame.draws[1]).rectangle.instance == 20);
    assert(std::get<GlyphDraw>(frame.draws[2]).glyph.instance == 11);
    assert(frame.images.size() == 1);
    assert(frame.images[0].rgba8[0] == 255);

    // A valid operation wholly outside its clip remains an ordered no-op; clipping is evaluated by
    // the target, not misclassified as a malformed producer submission.
    auto clippedSolid = solid(21);
    clippedSolid.rectangle.clip = {.enabled = true, .x = 2000, .y = 2000, .width = 4, .height = 4};
    assert(collector.begin(1280, 960, {}));
    assert(collector.append_solid_rectangle(clippedSolid));
    assert(collector.seal(frame));
    assert(frame.draws.size() == 1);

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
    assert(collector.error() == SemanticFrameError::ConflictingImage);
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
    assert(sb::native_render::canvas(frame.draws[0]) != sb::native_render::canvas(frame.draws[1]));

    // A two-image model stores both resources atomically and preserves material order. This is the
    // production collector path used by independently sampled colour and alpha-mask textures.
    const std::array<MeshVertex, 3> modelVertices{MeshVertex{{0, 0, 0}}, MeshVertex{{1, 0, 0}},
                                                  MeshVertex{{0, 1, 0}}};
    const MeshResourceView modelMesh{30, 1, modelVertices};
    const ModelDraw litMaskModel{
        .instance = 31,
        .mesh = {.resource = 30, .revision = 1, .vertexCount = 3},
        .modelView = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
        .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
        .material =
            LitTexturedAlphaMaskMaterial{
                .colorTexture = {.resource = 7, .revision = 1, .width = 1, .height = 1},
                .alphaMaskTexture = {.resource = 8, .revision = 1, .width = 1, .height = 1},
                .lighting = {.pointLights = {{{.position = {0, 0, 1}}}}, .pointLightCount = 1}},
    };
    auto maskImage = image(8, 1, blue);
    const std::array<DecodedImageView, 2> modelImages{firstImage, maskImage};
    assert(collector.begin(1280, 960, {}));
    assert(collector.append_model(litMaskModel, modelMesh, modelImages));
    assert(collector.decoded_image_bytes() == 8);
    assert(collector.seal(frame));
    assert(frame.models.size() == 1 && frame.images.size() == 2);
    assert(frame.images[0].resource == 7 && frame.images[1].resource == 8);

    SemanticFrameCollector commandBound{{.commands = 1, .images = 2, .decodedImageBytes = 8}};
    assert(commandBound.begin(1280, 960, {}));
    assert(commandBound.append(draw(firstCommand), std::span(&firstImage, 1)));
    assert(!commandBound.append(draw(secondCommand), std::span(&firstImage, 1)));
    assert(commandBound.error() == SemanticFrameError::CommandLimit);

    SemanticFrameCollector imageBound{{.commands = 2, .images = 1, .decodedImageBytes = 8}};
    assert(imageBound.begin(1280, 960, {}));
    assert(imageBound.append(draw(firstCommand), std::span(&firstImage, 1)));
    assert(!imageBound.append(draw(changedCommand), std::span(&changedImage, 1)));
    assert(imageBound.error() == SemanticFrameError::ImageLimit);

    SemanticFrameCollector byteBound{{.commands = 2, .images = 2, .decodedImageBytes = 4}};
    assert(byteBound.begin(1280, 960, {}));
    assert(byteBound.append(draw(firstCommand), std::span(&firstImage, 1)));
    assert(!byteBound.append(draw(changedCommand), std::span(&changedImage, 1)));
    assert(byteBound.error() == SemanticFrameError::ImageByteLimit);

    SemanticFrameCollector invalid{};
    assert(!invalid.begin(0, 960, {}));
    assert(invalid.error() == SemanticFrameError::InvalidFrame);

    // The production sink callback exercises the same append implementation.
    assert(invalid.begin(1280, 960, {}));
    const auto sink = invalid.sink();
    const PictureDraw firstDraw = draw(firstCommand);
    assert(sink.submit(sb::native_render::SemanticDraw{firstDraw}, std::span(&firstImage, 1),
                       sink.context));
    assert(sink.submit(sb::native_render::SemanticDraw{solid(30)}, {}, sink.context));
    invalid.reset();
    assert(!invalid.append(firstDraw, std::span(&firstImage, 1)));
    assert(invalid.error() == SemanticFrameError::NotCollecting);
}
