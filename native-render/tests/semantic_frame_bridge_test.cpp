#include <sunbright/native_render/semantic_frame_bridge.h>

#include <array>
#include <cassert>
#include <cstring>

namespace {

using namespace sb::native_render;

bool accept(const SemanticDraw&, std::span<const DecodedImageView>, void*) {
    return true;
}

bool unexpected_model(const ModelDraw&, const MeshResourceView&, std::span<const DecodedImageView>,
                      void*) {
    assert(false && "incumbent sink must not receive a model in this test");
    return false;
}

PictureDraw draw(std::uint64_t instance, std::uint64_t revision) {
    PictureCommand picture{};
    picture.instance = instance;
    picture.positions = {Vec2{0, 0}, Vec2{16, 0}, Vec2{0, 16}, Vec2{16, 16}};
    picture.uv = {Vec2{0, 0}, Vec2{1, 0}, Vec2{0, 1}, Vec2{1, 1}};
    picture.material.textureCount = 1;
    picture.material.textures[0] = PictureTexture{
        .resource = 7, .revision = revision, .width = 1, .height = 1, .hasAlpha = true};
    return {Canvas{{0, 0}, {640, 480}, {0, 0, 1280, 960}}, picture};
}

} // namespace

int main() {
    SemanticFrameBridge bridge;

    assert(bridge.begin());
    assert(bridge.seal());
    assert(!has_semantic_sink());
    assert(bridge.last_sealed_frame() == nullptr);

    assert(!bridge.activate({.targetWidth = 0, .targetHeight = 960}));
    assert(std::strcmp(bridge.last_error(), "invalid target extent") == 0);

    SemanticSinkLease incumbent;
    assert(claim_semantic_sink({.submit = accept, .submitModel = unexpected_model}, incumbent));
    SemanticSinkLease refused;
    assert(!claim_semantic_sink({.submit = accept, .submitModel = unexpected_model}, refused));
    assert(!refused);
    assert(!release_semantic_sink({incumbent.value + 1}));
    assert(owns_semantic_sink(incumbent));
    assert(!bridge.activate({.targetWidth = 1280, .targetHeight = 960}));
    assert(std::strcmp(bridge.last_error(), "semantic sink already owned") == 0);
    assert(release_semantic_sink(incumbent));

    assert(bridge.activate({.targetWidth = 1280, .targetHeight = 960}));
    assert(bridge.active());
    assert(bridge.begin());
    const std::uint64_t sequenceBeforeSeal = bridge.sealed_sequence();
    assert(has_semantic_sink());
    assert(!bridge.begin());
    assert(std::strcmp(bridge.last_error(), "frame already collecting") == 0);

    std::array<std::uint8_t, 4> pixel{255, 0, 0, 255};
    const PictureDraw first = draw(10, 1);
    DecodedImageView image{7, 1, 1, 1, pixel};
    assert(submit_picture(first, std::span(&image, 1)));
    pixel[0] = 0;

    assert(bridge.seal());
    assert(bridge.sealed_sequence() == sequenceBeforeSeal + 1);
    assert(!has_semantic_sink());
    const auto* sealed = bridge.last_sealed_frame();
    assert(sealed != nullptr);
    assert(sealed->targetWidth == 1280 && sealed->targetHeight == 960);
    assert(sealed->draws.size() == 1 &&
           std::get<PictureDraw>(sealed->draws[0]).picture.instance == 10);
    assert(sealed->images.size() == 1 && sealed->images[0].rgba8[0] == 255);
    assert(!bridge.seal());
    assert(std::strcmp(bridge.last_error(), "frame is not collecting") == 0);

    assert(bridge.begin());
    assert(bridge.last_sealed_frame() == nullptr);
    assert(bridge.seal());
    sealed = bridge.last_sealed_frame();
    assert(sealed != nullptr && sealed->draws.empty() && sealed->images.empty());

    assert(bridge.deactivate());
    assert(!bridge.active());
    assert(!has_semantic_sink());
    assert(bridge.last_sealed_frame() == nullptr);

    // Teardown while collecting releases the exact lease and allows a clean second activation.
    assert(bridge.activate({.targetWidth = 320, .targetHeight = 240}));
    assert(bridge.begin());
    assert(has_semantic_sink());
    assert(bridge.deactivate());
    assert(!has_semantic_sink());
    assert(bridge.activate({.targetWidth = 640, .targetHeight = 480}));
    assert(bridge.deactivate());
}
