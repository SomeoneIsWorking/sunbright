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

const char* picture_frame_error_name(PictureFrameError error) noexcept {
    switch (error) {
    case PictureFrameError::None:
        return "none";
    case PictureFrameError::InvalidLimits:
        return "invalid limits";
    case PictureFrameError::InvalidFrame:
        return "invalid frame";
    case PictureFrameError::AlreadyCollecting:
        return "already collecting";
    case PictureFrameError::NotCollecting:
        return "not collecting";
    case PictureFrameError::InvalidSubmission:
        return "invalid submission";
    case PictureFrameError::CommandLimit:
        return "command limit";
    case PictureFrameError::ImageLimit:
        return "image limit";
    case PictureFrameError::ImageByteLimit:
        return "decoded image byte limit";
    case PictureFrameError::ConflictingImage:
        return "conflicting image content";
    case PictureFrameError::AllocationFailure:
        return "allocation failure";
    }
    return "unknown";
}

PictureFrameCollector::PictureFrameCollector(PictureFrameLimits limits) : limits_(limits) {
    if (limits.commands == 0 || limits.images == 0 || limits.decodedImageBytes == 0)
        error_ = PictureFrameError::InvalidLimits;
}

bool PictureFrameCollector::begin(std::uint32_t targetWidth, std::uint32_t targetHeight,
                                  Color clear) {
    if (limits_.commands == 0 || limits_.images == 0 || limits_.decodedImageBytes == 0)
        return fail(PictureFrameError::InvalidLimits);
    if (state_ == State::Collecting)
        return fail(PictureFrameError::AlreadyCollecting);
    if (targetWidth == 0 || targetHeight == 0 || !valid_color(clear))
        return fail(PictureFrameError::InvalidFrame);

    draws_.clear();
    images_.clear();
    imageViews_.clear();
    decodedImageBytes_ = 0;
    targetWidth_ = targetWidth;
    targetHeight_ = targetHeight;
    clear_ = clear;
    state_ = State::Collecting;
    error_ = PictureFrameError::None;
    return true;
}

PictureSink PictureFrameCollector::sink() noexcept {
    return {receive, this};
}

bool PictureFrameCollector::append(const PictureDraw& draw,
                                   std::span<const DecodedImageView> images) {
    if (state_ != State::Collecting)
        return fail(PictureFrameError::NotCollecting);
    if (!valid(draw) || images.size() != draw.picture.material.textureCount)
        return fail(PictureFrameError::InvalidSubmission);
    PixelRect viewport{};
    if (!resolve_scissor(draw.canvas, {}, targetWidth_, targetHeight_, viewport))
        return fail(PictureFrameError::InvalidSubmission);
    if (draws_.size() >= limits_.commands)
        return fail(PictureFrameError::CommandLimit);

    std::vector<StoredImage> pending;
    std::size_t pendingBytes = 0;
    try {
        pending.reserve(images.size());
        for (std::size_t index = 0; index < images.size(); ++index) {
            const DecodedImageView& image = images[index];
            const PictureTexture& texture = draw.picture.material.textures[index];
            if (!valid(image) || image.resource != texture.resource ||
                image.revision != texture.revision || image.width != texture.width ||
                image.height != texture.height) {
                return fail(PictureFrameError::InvalidSubmission);
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
                    return fail(PictureFrameError::ConflictingImage);
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
                    return fail(PictureFrameError::ConflictingImage);
                continue;
            }
            if (images_.size() + pending.size() >= limits_.images)
                return fail(PictureFrameError::ImageLimit);
            if (pendingBytes > limits_.decodedImageBytes - decodedImageBytes_ ||
                image.rgba8.size() >
                    limits_.decodedImageBytes - decodedImageBytes_ - pendingBytes) {
                return fail(PictureFrameError::ImageByteLimit);
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
        return fail(PictureFrameError::AllocationFailure);
    }
    error_ = PictureFrameError::None;
    return true;
}

bool PictureFrameCollector::seal(PictureFrame& frame) {
    if (state_ != State::Collecting)
        return fail(PictureFrameError::NotCollecting);
    try {
        imageViews_.clear();
        imageViews_.reserve(images_.size());
        for (const StoredImage& image : images_) {
            imageViews_.push_back(
                {image.resource, image.revision, image.width, image.height, image.rgba8});
        }
    } catch (const std::bad_alloc&) {
        return fail(PictureFrameError::AllocationFailure);
    }
    state_ = State::Sealed;
    error_ = PictureFrameError::None;
    frame = {targetWidth_, targetHeight_, draws_, imageViews_, clear_};
    return true;
}

void PictureFrameCollector::reset() noexcept {
    draws_.clear();
    images_.clear();
    imageViews_.clear();
    decodedImageBytes_ = 0;
    error_ = PictureFrameError::None;
    state_ = State::Idle;
}

PictureFrameError PictureFrameCollector::error() const noexcept {
    return error_;
}

std::size_t PictureFrameCollector::decoded_image_bytes() const noexcept {
    return decodedImageBytes_;
}

bool PictureFrameCollector::receive(const PictureDraw& draw,
                                    std::span<const DecodedImageView> images, void* context) {
    return static_cast<PictureFrameCollector*>(context)->append(draw, images);
}

bool PictureFrameCollector::fail(PictureFrameError error) noexcept {
    error_ = error;
    return false;
}

} // namespace sb::native_render
