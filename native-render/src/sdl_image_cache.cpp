#include "sdl_image_cache.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <unordered_map>

namespace sb::native_render {
namespace {

constexpr SDL_GPUTextureFormat kImageFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
constexpr std::size_t kMaximumResidentImages = 2048;

struct ImageKey {
    std::uint64_t resource = 0;
    std::uint64_t revision = 0;
    bool operator==(const ImageKey&) const = default;
};

struct ImageKeyHash {
    std::size_t operator()(ImageKey key) const noexcept {
        return std::hash<std::uint64_t>{}(key.resource) ^
               (std::hash<std::uint64_t>{}(key.revision) << 1U);
    }
};

struct SamplerKey {
    AddressMode addressU = AddressMode::Clamp;
    AddressMode addressV = AddressMode::Clamp;
    FilterMode minFilter = FilterMode::Nearest;
    FilterMode magFilter = FilterMode::Nearest;
    MipFilter mipFilter = MipFilter::None;
    std::uint8_t mipLevels = 1;
    bool operator==(const SamplerKey&) const = default;
};

struct SamplerKeyHash {
    std::size_t operator()(SamplerKey key) const noexcept {
        return static_cast<std::size_t>(key.addressU) |
               (static_cast<std::size_t>(key.addressV) << 2U) |
               (static_cast<std::size_t>(key.minFilter) << 4U) |
               (static_cast<std::size_t>(key.magFilter) << 5U) |
               (static_cast<std::size_t>(key.mipFilter) << 6U) |
               (static_cast<std::size_t>(key.mipLevels) << 8U);
    }
};

struct CachedImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t mipLevels = 1;
    SDL_GPUTexture* texture = nullptr;
    SDL_GPUTransferBuffer* upload = nullptr;
    std::uint64_t lastUsedFrame = 0;
};

bool image_bytes(std::uint32_t width, std::uint32_t height, std::size_t& bytes) noexcept {
    const std::uint64_t total = static_cast<std::uint64_t>(width) * height * 4U;
    if (width == 0 || height == 0 || total > std::numeric_limits<Uint32>::max())
        return false;
    bytes = static_cast<std::size_t>(total);
    return true;
}

bool image_storage_bytes(const DecodedImageView& image, std::size_t& bytes) noexcept {
    bytes = 0;
    if (!valid(image) || image.mipLevels.size() >= std::numeric_limits<std::uint8_t>::max())
        return false;
    std::uint32_t levelWidth = image.width;
    std::uint32_t levelHeight = image.height;
    for (std::size_t level = 0; level <= image.mipLevels.size(); ++level) {
        std::size_t levelBytes = 0;
        if (!image_bytes(levelWidth, levelHeight, levelBytes) ||
            levelBytes > std::numeric_limits<Uint32>::max() - bytes) {
            return false;
        }
        bytes += levelBytes;
        levelWidth = std::max(levelWidth >> 1U, 1U);
        levelHeight = std::max(levelHeight >> 1U, 1U);
    }
    return true;
}

SDL_GPUFilter filter(FilterMode value) noexcept {
    return value == FilterMode::Linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
}

SDL_GPUSamplerMipmapMode mip_filter(MipFilter value) noexcept {
    return value == MipFilter::Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
                                      : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
}

SDL_GPUSamplerAddressMode address_mode(AddressMode value) noexcept {
    switch (value) {
    case AddressMode::Repeat:
        return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    case AddressMode::Mirror:
        return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
    case AddressMode::Clamp:
        return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    }
    return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
}

void release_image(SDL_GPUDevice* device, CachedImage image) noexcept {
    if (image.upload != nullptr)
        SDL_ReleaseGPUTransferBuffer(device, image.upload);
    if (image.texture != nullptr)
        SDL_ReleaseGPUTexture(device, image.texture);
}

} // namespace

struct SdlImageCache::Impl {
    explicit Impl(SDL_GPUDevice* value) noexcept : device(value) {}

    SDL_GPUDevice* device = nullptr;
    std::unordered_map<ImageKey, const DecodedImageView*, ImageKeyHash> sources{};
    std::unordered_map<ImageKey, CachedImage, ImageKeyHash> images{};
    std::unordered_map<ImageKey, CachedImage, ImageKeyHash> pending{};
    std::unordered_map<SamplerKey, SDL_GPUSampler*, SamplerKeyHash> samplers{};
    bool active = false;
    bool uploadsEncoded = false;
    std::uint64_t frame = 0;
};

SdlImageCache::SdlImageCache(SDL_GPUDevice* device) : impl_(new Impl(device)) {}

SdlImageCache::~SdlImageCache() {
    if (impl_ != nullptr && impl_->active)
        std::terminate();
    if (impl_ != nullptr && impl_->device != nullptr) {
        for (const auto& [key, sampler] : impl_->samplers) {
            (void)key;
            SDL_ReleaseGPUSampler(impl_->device, sampler);
        }
        for (const auto& [key, image] : impl_->images) {
            (void)key;
            release_image(impl_->device, image);
        }
        for (const auto& [key, image] : impl_->pending) {
            (void)key;
            release_image(impl_->device, image);
        }
    }
    delete impl_;
}

bool SdlImageCache::begin(std::span<const DecodedImageView> images, std::string& error) {
    error.clear();
    if (impl_ == nullptr || impl_->device == nullptr || impl_->active) {
        error = "decoded image cache is unavailable or already active";
        return false;
    }
    impl_->sources.clear();
    impl_->pending.clear();
    impl_->uploadsEncoded = false;
    ++impl_->frame;
    for (const DecodedImageView& image : images) {
        std::size_t bytes = 0;
        const ImageKey key{image.resource, image.revision};
        if (image.resource == 0 || !image_storage_bytes(image, bytes) ||
            !impl_->sources.emplace(key, &image).second) {
            error = "semantic frame contains an invalid or duplicate decoded image";
            return false;
        }
        const auto cached = impl_->images.find(key);
        if (cached != impl_->images.end() &&
            (cached->second.width != image.width || cached->second.height != image.height ||
             cached->second.mipLevels != image.mipLevels.size() + 1U)) {
            error = "decoded image revision changed dimensions without changing its semantic key";
            return false;
        }
    }
    impl_->active = true;
    return true;
}

bool SdlImageCache::resolve(const PictureTexture& texture, SDL_GPUTextureSamplerBinding& binding,
                            std::string& error) {
    error.clear();
    if (impl_ == nullptr || !impl_->active || impl_->uploadsEncoded) {
        error = "decoded image cache is not accepting texture resolutions";
        return false;
    }
    const ImageKey imageKey{texture.resource, texture.revision};
    const auto source = impl_->sources.find(imageKey);
    auto resident = impl_->images.find(imageKey);
    auto pending = impl_->pending.find(imageKey);
    CachedImage* image = resident != impl_->images.end() ? &resident->second : nullptr;
    if (image == nullptr && pending != impl_->pending.end())
        image = &pending->second;
    if (image == nullptr) {
        if (source == impl_->sources.end()) {
            error = "semantic command references an absent decoded image";
            return false;
        }
        std::size_t bytes = 0;
        if (!image_storage_bytes(*source->second, bytes)) {
            error = "semantic command references an invalid decoded image";
            return false;
        }
        const std::uint8_t mipLevels =
            static_cast<std::uint8_t>(source->second->mipLevels.size() + 1U);
        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = kImageFormat;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        textureInfo.width = source->second->width;
        textureInfo.height = source->second->height;
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = mipLevels;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        const SDL_GPUTransferBufferCreateInfo uploadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                         static_cast<Uint32>(bytes), 0};
        CachedImage created{source->second->width,
                            source->second->height,
                            mipLevels,
                            SDL_CreateGPUTexture(impl_->device, &textureInfo),
                            SDL_CreateGPUTransferBuffer(impl_->device, &uploadInfo),
                            impl_->frame};
        if (created.texture == nullptr || created.upload == nullptr) {
            release_image(impl_->device, created);
            error = std::string("decoded image resource allocation failed: ") + SDL_GetError();
            return false;
        }
        void* mapped = SDL_MapGPUTransferBuffer(impl_->device, created.upload, false);
        if (mapped == nullptr) {
            release_image(impl_->device, created);
            error = std::string("decoded image upload map failed: ") + SDL_GetError();
            return false;
        }
        auto* destination = static_cast<std::uint8_t*>(mapped);
        std::size_t offset = 0;
        std::memcpy(destination, source->second->rgba8.data(), source->second->rgba8.size());
        offset += source->second->rgba8.size();
        for (const DecodedImageMipLevel& level : source->second->mipLevels) {
            std::memcpy(destination + offset, level.rgba8.data(), level.rgba8.size());
            offset += level.rgba8.size();
        }
        if (offset != bytes) {
            SDL_UnmapGPUTransferBuffer(impl_->device, created.upload);
            release_image(impl_->device, created);
            error = "decoded image mip storage size is internally inconsistent";
            return false;
        }
        SDL_UnmapGPUTransferBuffer(impl_->device, created.upload);
        image = &impl_->pending.emplace(imageKey, created).first->second;
    }
    if (image->width != texture.width || image->height != texture.height) {
        error = "semantic texture dimensions do not match its decoded image";
        return false;
    }
    image->lastUsedFrame = impl_->frame;

    const SamplerKey samplerKey{texture.addressU,  texture.addressV,  texture.minFilter,
                                texture.magFilter, texture.mipFilter, image->mipLevels};
    SDL_GPUSampler* sampler = nullptr;
    if (const auto found = impl_->samplers.find(samplerKey); found != impl_->samplers.end()) {
        sampler = found->second;
    } else {
        SDL_GPUSamplerCreateInfo info{};
        info.min_filter = filter(texture.minFilter);
        info.mag_filter = filter(texture.magFilter);
        info.mipmap_mode = mip_filter(texture.mipFilter);
        info.address_mode_u = address_mode(texture.addressU);
        info.address_mode_v = address_mode(texture.addressV);
        info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        info.max_lod =
            texture.mipFilter == MipFilter::None ? 0.0F : static_cast<float>(image->mipLevels - 1U);
        sampler = SDL_CreateGPUSampler(impl_->device, &info);
        if (sampler == nullptr) {
            error = std::string("decoded image sampler creation failed: ") + SDL_GetError();
            return false;
        }
        impl_->samplers.emplace(samplerKey, sampler);
    }
    binding = {image->texture, sampler};
    return true;
}

bool SdlImageCache::encode_uploads(SDL_GPUCommandBuffer* commandBuffer, std::string& error) {
    error.clear();
    if (impl_ == nullptr || !impl_->active || impl_->uploadsEncoded || commandBuffer == nullptr) {
        error = "decoded image uploads require one active frame and command buffer";
        return false;
    }
    if (!impl_->pending.empty()) {
        SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commandBuffer);
        if (copy == nullptr) {
            error = std::string("decoded image upload pass creation failed: ") + SDL_GetError();
            return false;
        }
        for (const auto& [key, image] : impl_->pending) {
            (void)key;
            std::uint32_t levelWidth = image.width;
            std::uint32_t levelHeight = image.height;
            Uint32 offset = 0;
            for (std::uint32_t level = 0; level < image.mipLevels; ++level) {
                std::size_t levelBytes = 0;
                const bool validLevel = image_bytes(levelWidth, levelHeight, levelBytes) &&
                                        levelBytes <= std::numeric_limits<Uint32>::max() - offset;
                if (!validLevel) {
                    SDL_EndGPUCopyPass(copy);
                    error = "decoded image mip upload dimensions are invalid";
                    return false;
                }
                const SDL_GPUTextureTransferInfo source{image.upload, offset, levelWidth,
                                                        levelHeight};
                const SDL_GPUTextureRegion destination{image.texture, level,       0, 0, 0, 0,
                                                       levelWidth,    levelHeight, 1};
                SDL_UploadToGPUTexture(copy, &source, &destination, false);
                offset += static_cast<Uint32>(levelBytes);
                levelWidth = std::max(levelWidth >> 1U, 1U);
                levelHeight = std::max(levelHeight >> 1U, 1U);
            }
        }
        SDL_EndGPUCopyPass(copy);
    }
    impl_->uploadsEncoded = true;
    return true;
}

bool SdlImageCache::complete(bool submitted, std::string& error) noexcept {
    error.clear();
    if (impl_ == nullptr || !impl_->active) {
        error = "decoded image cache has no active frame";
        return false;
    }
    if (submitted && !impl_->uploadsEncoded) {
        error = "decoded image cache cannot commit before uploads are encoded";
        return false;
    }
    if (submitted) {
        for (auto& [key, image] : impl_->pending) {
            (void)key;
            SDL_ReleaseGPUTransferBuffer(impl_->device, image.upload);
            image.upload = nullptr;
        }
        impl_->images.merge(impl_->pending);
        while (impl_->images.size() > kMaximumResidentImages) {
            auto oldest = impl_->images.end();
            for (auto candidate = impl_->images.begin(); candidate != impl_->images.end();
                 ++candidate) {
                if (candidate->second.lastUsedFrame == impl_->frame)
                    continue;
                if (oldest == impl_->images.end() ||
                    candidate->second.lastUsedFrame < oldest->second.lastUsedFrame) {
                    oldest = candidate;
                }
            }
            // One frame may legitimately reference more than the steady-state cap. Those images
            // cannot be evicted before its submitted command buffer has consumed them.
            if (oldest == impl_->images.end())
                break;
            release_image(impl_->device, oldest->second);
            impl_->images.erase(oldest);
        }
    } else {
        for (const auto& [key, image] : impl_->pending) {
            (void)key;
            release_image(impl_->device, image);
        }
        impl_->pending.clear();
    }
    impl_->sources.clear();
    impl_->active = false;
    impl_->uploadsEncoded = false;
    return true;
}

std::size_t SdlImageCache::resident_count() const noexcept {
    return impl_ != nullptr ? impl_->images.size() : 0;
}

} // namespace sb::native_render
