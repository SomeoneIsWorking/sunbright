#include <sunbright/native_render/semantic_sink.h>

namespace sb::native_render {
namespace {

SemanticSink g_sink{};
SemanticSinkLease g_lease{};
std::uint64_t g_nextLease = 1;

bool submit_textured(const SemanticDraw& draw, const PictureCommand& picture,
                     std::span<const DecodedImageView> images) noexcept {
    if (g_sink.submit == nullptr || !valid(draw) || images.size() != picture.material.textureCount)
        return false;
    for (std::size_t index = 0; index < images.size(); ++index) {
        const PictureTexture& texture = picture.material.textures[index];
        const DecodedImageView& image = images[index];
        if (!valid(image) || image.resource != texture.resource ||
            image.revision != texture.revision || image.width != texture.width ||
            image.height != texture.height) {
            return false;
        }
    }
    return g_sink.submit(draw, images, g_sink.context);
}

} // namespace

bool claim_semantic_sink(SemanticSink sink, SemanticSinkLease& lease) noexcept {
    lease = {};
    if (sink.submit == nullptr || sink.submitModel == nullptr || g_sink.submit != nullptr)
        return false;
    g_sink = sink;
    g_lease.value = g_nextLease++;
    if (g_nextLease == 0)
        g_nextLease = 1;
    lease = g_lease;
    return true;
}

bool release_semantic_sink(SemanticSinkLease lease) noexcept {
    if (!lease || lease != g_lease)
        return false;
    g_sink = {};
    g_lease = {};
    return true;
}

bool owns_semantic_sink(SemanticSinkLease lease) noexcept {
    return lease && lease == g_lease;
}

bool has_semantic_sink() noexcept {
    return g_sink.submit != nullptr;
}

bool submit_picture(const PictureDraw& draw, std::span<const DecodedImageView> images) noexcept {
    return submit_textured(SemanticDraw{draw}, draw.picture, images);
}

bool submit_glyph(const GlyphDraw& draw, std::span<const DecodedImageView> images) noexcept {
    if (!valid(draw))
        return false;
    return submit_textured(SemanticDraw{draw}, picture_from_glyph(draw.glyph), images);
}

bool submit_solid_rectangle(const SolidRectangleDraw& draw) noexcept {
    if (g_sink.submit == nullptr || !valid(draw))
        return false;
    return g_sink.submit(SemanticDraw{draw}, {}, g_sink.context);
}

bool submit_model(const ModelDraw& draw, const MeshResourceView& mesh,
                  std::span<const DecodedImageView> images) noexcept {
    if (g_sink.submitModel == nullptr || !model_mesh_matches(draw, mesh)) {
        return false;
    }
    if (!material_images_match(draw.material, images))
        return false;
    return g_sink.submitModel(draw, mesh, images, g_sink.context);
}

} // namespace sb::native_render
