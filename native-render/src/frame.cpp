#include <sunbright/native_render/frame.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace sb::native_render {
namespace {

bool valid_color(Color color) noexcept {
    return std::isfinite(color.r) && std::isfinite(color.g) && std::isfinite(color.b) &&
           std::isfinite(color.a);
}

} // namespace

const char* semantic_frame_error_name(SemanticFrameError error) noexcept {
    switch (error) {
    case SemanticFrameError::None:
        return "none";
    case SemanticFrameError::InvalidLimits:
        return "invalid limits";
    case SemanticFrameError::InvalidFrame:
        return "invalid frame";
    case SemanticFrameError::AlreadyCollecting:
        return "already collecting";
    case SemanticFrameError::NotCollecting:
        return "not collecting";
    case SemanticFrameError::InvalidSubmission:
        return "invalid submission";
    case SemanticFrameError::CommandLimit:
        return "command limit";
    case SemanticFrameError::ImageLimit:
        return "image limit";
    case SemanticFrameError::ImageByteLimit:
        return "decoded image byte limit";
    case SemanticFrameError::ConflictingImage:
        return "conflicting image content";
    case SemanticFrameError::MeshLimit:
        return "mesh limit";
    case SemanticFrameError::MeshVertexLimit:
        return "mesh vertex limit";
    case SemanticFrameError::ConflictingMesh:
        return "conflicting mesh content";
    case SemanticFrameError::AllocationFailure:
        return "allocation failure";
    }
    return "unknown";
}

SemanticFrameCollector::SemanticFrameCollector(SemanticFrameLimits limits) : limits_(limits) {
    if (limits.commands == 0 || limits.images == 0 || limits.decodedImageBytes == 0 ||
        limits.meshes == 0 || limits.meshVertices == 0)
        error_ = SemanticFrameError::InvalidLimits;
}

bool SemanticFrameCollector::begin(std::uint32_t targetWidth, std::uint32_t targetHeight,
                                   Color clear) {
    if (limits_.commands == 0 || limits_.images == 0 || limits_.decodedImageBytes == 0 ||
        limits_.meshes == 0 || limits_.meshVertices == 0)
        return fail(SemanticFrameError::InvalidLimits);
    if (state_ == State::Collecting)
        return fail(SemanticFrameError::AlreadyCollecting);
    if (targetWidth == 0 || targetHeight == 0 || !valid_color(clear))
        return fail(SemanticFrameError::InvalidFrame);

    draws_.clear();
    models_.clear();
    meshes_.clear();
    meshViews_.clear();
    images_.clear();
    imageViews_.clear();
    decodedImageBytes_ = 0;
    meshVertices_ = 0;
    targetWidth_ = targetWidth;
    targetHeight_ = targetHeight;
    clear_ = clear;
    state_ = State::Collecting;
    error_ = SemanticFrameError::None;
    return true;
}

SemanticSink SemanticFrameCollector::sink() noexcept {
    return {receive, receive_model, this};
}

bool SemanticFrameCollector::append(const SemanticDraw& draw,
                                    std::span<const DecodedImageView> images) {
    if (const auto* picture = std::get_if<PictureDraw>(&draw))
        return append_picture(*picture, images);
    if (const auto* glyph = std::get_if<GlyphDraw>(&draw))
        return append_glyph(*glyph, images);
    if (!images.empty())
        return fail(SemanticFrameError::InvalidSubmission);
    return append_solid_rectangle(std::get<SolidRectangleDraw>(draw));
}

bool SemanticFrameCollector::append_picture(const PictureDraw& draw,
                                            std::span<const DecodedImageView> images) {
    return append_textured(SemanticDraw{draw}, draw.canvas, draw.picture, images);
}

bool SemanticFrameCollector::append_glyph(const GlyphDraw& draw,
                                          std::span<const DecodedImageView> images) {
    if (!valid(draw))
        return fail(SemanticFrameError::InvalidSubmission);
    return append_textured(SemanticDraw{draw}, draw.canvas, picture_from_glyph(draw.glyph), images);
}

bool SemanticFrameCollector::append_textured(const SemanticDraw& draw, const Canvas& canvas,
                                             const PictureCommand& picture,
                                             std::span<const DecodedImageView> images) {
    if (state_ != State::Collecting)
        return fail(SemanticFrameError::NotCollecting);
    if (!valid(draw) || images.size() != picture.material.textureCount)
        return fail(SemanticFrameError::InvalidSubmission);
    PixelRect viewport{};
    if (!resolve_scissor(canvas, {}, targetWidth_, targetHeight_, viewport))
        return fail(SemanticFrameError::InvalidSubmission);
    if (draws_.size() + models_.size() >= limits_.commands)
        return fail(SemanticFrameError::CommandLimit);

    std::vector<StoredImage> pending;
    std::size_t pendingBytes = 0;
    try {
        pending.reserve(images.size());
        for (std::size_t index = 0; index < images.size(); ++index) {
            const DecodedImageView& image = images[index];
            const PictureTexture& texture = picture.material.textures[index];
            if (!valid(image) || image.resource != texture.resource ||
                image.revision != texture.revision || image.width != texture.width ||
                image.height != texture.height) {
                return fail(SemanticFrameError::InvalidSubmission);
            }

            const auto stored =
                std::find_if(images_.begin(), images_.end(), [&](const StoredImage& candidate) {
                    return candidate.resource == image.resource &&
                           candidate.revision == image.revision;
                });
            if (stored != images_.end()) {
                if (stored->width != image.width || stored->height != image.height ||
                    !std::equal(stored->rgba8.begin(), stored->rgba8.end(), image.rgba8.begin(),
                                image.rgba8.end()))
                    return fail(SemanticFrameError::ConflictingImage);
                continue;
            }
            const auto staged =
                std::find_if(pending.begin(), pending.end(), [&](const StoredImage& candidate) {
                    return candidate.resource == image.resource &&
                           candidate.revision == image.revision;
                });
            if (staged != pending.end()) {
                if (staged->width != image.width || staged->height != image.height ||
                    !std::equal(staged->rgba8.begin(), staged->rgba8.end(), image.rgba8.begin(),
                                image.rgba8.end()))
                    return fail(SemanticFrameError::ConflictingImage);
                continue;
            }
            if (images_.size() + pending.size() >= limits_.images)
                return fail(SemanticFrameError::ImageLimit);
            if (pendingBytes > limits_.decodedImageBytes - decodedImageBytes_ ||
                image.rgba8.size() >
                    limits_.decodedImageBytes - decodedImageBytes_ - pendingBytes) {
                return fail(SemanticFrameError::ImageByteLimit);
            }

            StoredImage owned{image.resource, image.revision, image.width, image.height,
                              std::vector<std::uint8_t>(image.rgba8.begin(), image.rgba8.end())};
            pendingBytes += owned.rgba8.size();
            pending.push_back(std::move(owned));
        }

        images_.reserve(images_.size() + pending.size());
        draws_.reserve(draws_.size() + 1);
        for (StoredImage& image : pending)
            images_.push_back(std::move(image));
        decodedImageBytes_ += pendingBytes;
        draws_.push_back(draw);
    } catch (const std::bad_alloc&) {
        return fail(SemanticFrameError::AllocationFailure);
    }
    error_ = SemanticFrameError::None;
    return true;
}

bool SemanticFrameCollector::append_solid_rectangle(const SolidRectangleDraw& draw) {
    if (state_ != State::Collecting)
        return fail(SemanticFrameError::NotCollecting);
    if (!valid(draw))
        return fail(SemanticFrameError::InvalidSubmission);
    PixelRect viewport{};
    if (!resolve_scissor(draw.canvas, {}, targetWidth_, targetHeight_, viewport))
        return fail(SemanticFrameError::InvalidSubmission);
    if (draws_.size() + models_.size() >= limits_.commands)
        return fail(SemanticFrameError::CommandLimit);
    try {
        draws_.emplace_back(draw);
    } catch (const std::bad_alloc&) {
        return fail(SemanticFrameError::AllocationFailure);
    }
    error_ = SemanticFrameError::None;
    return true;
}

bool SemanticFrameCollector::append_model(const ModelDraw& draw, const MeshResourceView& mesh,
                                          std::span<const DecodedImageView> images) {
    if (state_ != State::Collecting)
        return fail(SemanticFrameError::NotCollecting);
    if (!model_mesh_matches(draw, mesh)) {
        return fail(SemanticFrameError::InvalidSubmission);
    }
    if (draws_.size() + models_.size() >= limits_.commands)
        return fail(SemanticFrameError::CommandLimit);

    if (!material_images_match(draw.material, images))
        return fail(SemanticFrameError::InvalidSubmission);

    std::vector<StoredImage> pendingImages;
    std::size_t pendingImageBytes = 0;
    for (const DecodedImageView& image : images) {
        const auto storedImage =
            std::find_if(images_.begin(), images_.end(), [&](const StoredImage& candidate) {
                return candidate.resource == image.resource && candidate.revision == image.revision;
            });
        if (storedImage != images_.end()) {
            if (storedImage->width != image.width || storedImage->height != image.height ||
                !std::equal(storedImage->rgba8.begin(), storedImage->rgba8.end(),
                            image.rgba8.begin(), image.rgba8.end())) {
                return fail(SemanticFrameError::ConflictingImage);
            }
        } else if (const auto pendingImage =
                       std::find_if(pendingImages.begin(), pendingImages.end(),
                                    [&](const StoredImage& candidate) {
                                        return candidate.resource == image.resource &&
                                               candidate.revision == image.revision;
                                    });
                   pendingImage != pendingImages.end()) {
            if (pendingImage->width != image.width || pendingImage->height != image.height ||
                !std::equal(pendingImage->rgba8.begin(), pendingImage->rgba8.end(),
                            image.rgba8.begin(), image.rgba8.end())) {
                return fail(SemanticFrameError::ConflictingImage);
            }
        } else {
            if (images_.size() + pendingImages.size() >= limits_.images)
                return fail(SemanticFrameError::ImageLimit);
            if (decodedImageBytes_ > limits_.decodedImageBytes ||
                pendingImageBytes > limits_.decodedImageBytes - decodedImageBytes_ ||
                image.rgba8.size() >
                    limits_.decodedImageBytes - decodedImageBytes_ - pendingImageBytes) {
                return fail(SemanticFrameError::ImageByteLimit);
            }
            try {
                pendingImages.push_back(
                    {image.resource, image.revision, image.width, image.height,
                     std::vector<std::uint8_t>(image.rgba8.begin(), image.rgba8.end())});
                pendingImageBytes += image.rgba8.size();
            } catch (const std::bad_alloc&) {
                return fail(SemanticFrameError::AllocationFailure);
            }
        }
    }

    const auto stored = std::find_if(meshes_.begin(), meshes_.end(), [&](const StoredMesh& item) {
        return item.resource == mesh.resource && item.revision == mesh.revision;
    });
    const bool needsMesh = stored == meshes_.end();
    if (stored != meshes_.end()) {
        if (!std::equal(stored->vertices.begin(), stored->vertices.end(), mesh.vertices.begin(),
                        mesh.vertices.end())) {
            return fail(SemanticFrameError::ConflictingMesh);
        }
    } else {
        if (meshes_.size() >= limits_.meshes)
            return fail(SemanticFrameError::MeshLimit);
        if (mesh.vertices.size() > limits_.meshVertices - meshVertices_)
            return fail(SemanticFrameError::MeshVertexLimit);
    }

    std::vector<MeshVertex> pendingMesh;
    try {
        if (needsMesh)
            pendingMesh.assign(mesh.vertices.begin(), mesh.vertices.end());
        images_.reserve(images_.size() + pendingImages.size());
        meshes_.reserve(meshes_.size() + (needsMesh ? 1U : 0U));
        models_.reserve(models_.size() + 1U);
        for (StoredImage& image : pendingImages)
            images_.push_back(std::move(image));
        decodedImageBytes_ += pendingImageBytes;
        if (needsMesh) {
            meshes_.push_back({mesh.resource, mesh.revision, std::move(pendingMesh)});
            meshVertices_ += mesh.vertices.size();
        }
        models_.push_back(draw);
    } catch (const std::bad_alloc&) {
        return fail(SemanticFrameError::AllocationFailure);
    }
    error_ = SemanticFrameError::None;
    return true;
}

bool SemanticFrameCollector::seal(SemanticFrame& frame) {
    if (state_ != State::Collecting)
        return fail(SemanticFrameError::NotCollecting);
    try {
        meshViews_.clear();
        meshViews_.reserve(meshes_.size());
        for (const StoredMesh& mesh : meshes_)
            meshViews_.push_back({mesh.resource, mesh.revision, mesh.vertices});
        imageViews_.clear();
        imageViews_.reserve(images_.size());
        for (const StoredImage& image : images_) {
            imageViews_.push_back(
                {image.resource, image.revision, image.width, image.height, image.rgba8});
        }
    } catch (const std::bad_alloc&) {
        return fail(SemanticFrameError::AllocationFailure);
    }
    state_ = State::Sealed;
    error_ = SemanticFrameError::None;
    frame = {targetWidth_, targetHeight_, draws_, models_, meshViews_, imageViews_, clear_};
    return true;
}

void SemanticFrameCollector::reset() noexcept {
    draws_.clear();
    models_.clear();
    meshes_.clear();
    meshViews_.clear();
    images_.clear();
    imageViews_.clear();
    decodedImageBytes_ = 0;
    meshVertices_ = 0;
    error_ = SemanticFrameError::None;
    state_ = State::Idle;
}

SemanticFrameError SemanticFrameCollector::error() const noexcept {
    return error_;
}

std::size_t SemanticFrameCollector::decoded_image_bytes() const noexcept {
    return decodedImageBytes_;
}

bool SemanticFrameCollector::receive(const SemanticDraw& draw,
                                     std::span<const DecodedImageView> images, void* context) {
    return static_cast<SemanticFrameCollector*>(context)->append(draw, images);
}

bool SemanticFrameCollector::receive_model(const ModelDraw& draw, const MeshResourceView& mesh,
                                           std::span<const DecodedImageView> images,
                                           void* context) {
    return static_cast<SemanticFrameCollector*>(context)->append_model(draw, mesh, images);
}

bool SemanticFrameCollector::fail(SemanticFrameError error) noexcept {
    error_ = error;
    return false;
}

} // namespace sb::native_render
