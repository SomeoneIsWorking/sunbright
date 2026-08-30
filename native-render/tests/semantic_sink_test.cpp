#include <sunbright/native_render/semantic_sink.h>

#include <array>
#include <cassert>

namespace {

bool receive(const sb::native_render::SemanticDraw& draw,
             std::span<const sb::native_render::DecodedImageView> images, void* context) {
    if (const auto* picture = std::get_if<sb::native_render::PictureDraw>(&draw)) {
        assert(images.size() == 1);
        *static_cast<std::uint64_t*>(context) = picture->picture.instance;
    } else if (const auto* glyph = std::get_if<sb::native_render::GlyphDraw>(&draw)) {
        assert(images.size() == 1);
        *static_cast<std::uint64_t*>(context) = glyph->glyph.instance;
    } else {
        assert(images.empty());
        *static_cast<std::uint64_t*>(context) =
            std::get<sb::native_render::SolidRectangleDraw>(draw).rectangle.instance;
    }
    return true;
}

bool reject(const sb::native_render::SemanticDraw&,
            std::span<const sb::native_render::DecodedImageView>, void*) {
    return false;
}

bool receive_model(const sb::native_render::ModelDraw& draw,
                   const sb::native_render::MeshResourceView&,
                   std::span<const sb::native_render::DecodedImageView> images, void* context) {
    assert(images.empty());
    *static_cast<std::uint64_t*>(context) = draw.instance;
    return true;
}

bool reject_model(const sb::native_render::ModelDraw&, const sb::native_render::MeshResourceView&,
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

sb::native_render::PictureDraw valid_draw() {
    return {{.origin = {0, 0}, .extent = {1, 1}, .viewport = {0, 0, 1, 1}}, valid_picture()};
}

sb::native_render::GlyphDraw valid_glyph() {
    const auto picture = valid_picture();
    return {{.origin = {0, 0}, .extent = {1, 1}, .viewport = {0, 0, 1, 1}},
            {.instance = 9,
             .code = 'A',
             .positions = picture.positions,
             .uv = picture.uv,
             .corner = picture.corner,
             .atlas = picture.material.textures[0]}};
}

sb::native_render::SolidRectangleDraw valid_solid() {
    return {{.origin = {0, 0}, .extent = {1, 1}, .viewport = {0, 0, 1, 1}},
            {.instance = 8,
             .source = sb::native_render::SolidRectangleSource::Gc2dFillRect,
             .positions = {{{0, 0}, {1, 0}, {0, 1}, {1, 1}}},
             .corner = {{{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}}}}};
}

} // namespace

int main() {
    const std::array<std::uint8_t, 4> rgba{1, 2, 3, 4};
    const sb::native_render::DecodedImageView image{
        .resource = 1, .width = 1, .height = 1, .rgba8 = rgba};
    assert(!sb::native_render::has_semantic_sink());
    assert(!sb::native_render::submit_picture(valid_draw(), std::span(&image, 1)));

    std::uint64_t received = 0;
    sb::native_render::SemanticSinkLease lease;
    assert(sb::native_render::claim_semantic_sink(
        {.submit = receive, .submitModel = receive_model, .context = &received}, lease));
    assert(sb::native_render::has_semantic_sink());
    assert(sb::native_render::submit_picture(valid_draw(), std::span(&image, 1)));
    assert(received == 7);
    assert(sb::native_render::submit_solid_rectangle(valid_solid()));
    assert(received == 8);
    assert(sb::native_render::submit_glyph(valid_glyph(), std::span(&image, 1)));
    assert(received == 9);
    const std::array<sb::native_render::MeshVertex, 3> vertices{
        sb::native_render::MeshVertex{{0, 0, 0}}, sb::native_render::MeshVertex{{1, 0, 0}},
        sb::native_render::MeshVertex{{0, 1, 0}}};
    const sb::native_render::MeshResourceView mesh{2, 1, vertices};
    const sb::native_render::ModelDraw model{
        .instance = 10,
        .mesh = {.resource = 2, .revision = 1, .vertexCount = 3},
        .modelView = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}},
        .projection = {.value = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}},
    };
    assert(sb::native_render::submit_model(model, mesh));
    assert(received == 10);

    auto invalid = valid_draw();
    invalid.picture.material.textureCount = 0;
    received = 0;
    assert(!sb::native_render::submit_picture(invalid, std::span(&image, 1)));
    assert(received == 0);

    auto mismatched = image;
    mismatched.resource = 2;
    assert(!sb::native_render::submit_picture(valid_draw(), std::span(&mismatched, 1)));
    assert(received == 0);

    auto shortImage = image;
    shortImage.rgba8 = std::span(rgba).first(3);
    assert(!sb::native_render::submit_picture(valid_draw(), std::span(&shortImage, 1)));
    assert(received == 0);

    assert(sb::native_render::release_semantic_sink(lease));
    assert(sb::native_render::claim_semantic_sink(
        {.submit = reject, .submitModel = reject_model, .context = nullptr}, lease));
    assert(!sb::native_render::submit_picture(valid_draw(), std::span(&image, 1)));

    assert(sb::native_render::release_semantic_sink(lease));
}
