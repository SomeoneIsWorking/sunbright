#include "semantic_gpu_test_support.h"

#include <SDL3/SDL.h>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>

namespace sb::native_render::test {
namespace {

struct ReadbackResources {
    SDL_GPUCommandBuffer* commandBuffer = nullptr;
    SDL_GPUTransferBuffer* download = nullptr;
    std::size_t byteCount = 0;
};

ReadbackResources begin_readback(SDL_GPUDevice* device, const SemanticFrame& frame,
                                 std::string& error) {
    ReadbackResources resources{};
    resources.commandBuffer = SDL_AcquireGPUCommandBuffer(device);
    resources.byteCount = static_cast<std::size_t>(frame.targetWidth) * frame.targetHeight * 4;
    const SDL_GPUTransferBufferCreateInfo downloadInfo{SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
                                                       static_cast<Uint32>(resources.byteCount), 0};
    resources.download = SDL_CreateGPUTransferBuffer(device, &downloadInfo);
    if (resources.commandBuffer != nullptr && resources.download != nullptr)
        return resources;
    if (resources.commandBuffer != nullptr)
        SDL_CancelGPUCommandBuffer(resources.commandBuffer);
    if (resources.download != nullptr)
        SDL_ReleaseGPUTransferBuffer(device, resources.download);
    error = std::string("GPU control resource creation failed: ") + SDL_GetError();
    return {};
}

template <typename Pass>
bool finish_readback(Pass& pass, const SemanticFrame& frame, SDL_GPUDevice* device,
                     SDL_GPUTexture* texture, ReadbackResources resources,
                     SemanticFramePixels& output, std::string& error) {
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(resources.commandBuffer);
    if (copy == nullptr) {
        SDL_CancelGPUCommandBuffer(resources.commandBuffer);
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, resources.download);
        error = std::string("GPU control copy pass failed: ") + SDL_GetError();
        return false;
    }
    const SDL_GPUTextureRegion source{texture, 0, 0, 0, 0, 0, frame.targetWidth, frame.targetHeight,
                                      1};
    const SDL_GPUTextureTransferInfo destination{resources.download, 0, frame.targetWidth,
                                                 frame.targetHeight};
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(resources.commandBuffer);
    if (fence == nullptr) {
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, resources.download);
        error = std::string("GPU control submit failed: ") + SDL_GetError();
        return false;
    }
    if (!pass.complete_encode(true, error)) {
        SDL_ReleaseGPUFence(device, fence);
        SDL_ReleaseGPUTransferBuffer(device, resources.download);
        return false;
    }
    SDL_GPUFence* fences[] = {fence};
    const bool waited = SDL_WaitForGPUFences(device, true, fences, 1);
    SDL_ReleaseGPUFence(device, fence);
    if (!waited) {
        SDL_ReleaseGPUTransferBuffer(device, resources.download);
        error = SDL_GetError();
        return false;
    }
    void* mapped = SDL_MapGPUTransferBuffer(device, resources.download, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, resources.download);
        error = SDL_GetError();
        return false;
    }
    output = {frame.targetWidth, frame.targetHeight,
              std::vector<std::uint8_t>(resources.byteCount)};
    std::memcpy(output.rgba8.data(), mapped, resources.byteCount);
    SDL_UnmapGPUTransferBuffer(device, resources.download);
    SDL_ReleaseGPUTransferBuffer(device, resources.download);
    return true;
}

} // namespace

Color pixel(const SemanticFramePixels& frame, std::uint32_t x, std::uint32_t y) {
    const std::size_t offset = (static_cast<std::size_t>(y) * frame.width + x) * 4;
    constexpr float scale = 1.0F / 255.0F;
    return {frame.rgba8[offset] * scale, frame.rgba8[offset + 1] * scale,
            frame.rgba8[offset + 2] * scale, frame.rgba8[offset + 3] * scale};
}

bool near(float actual, float expected, float tolerance) {
    return actual >= expected - tolerance && actual <= expected + tolerance;
}

void require_color(Color actual, Color expected) {
    assert(near(actual.r, expected.r));
    assert(near(actual.g, expected.g));
    assert(near(actual.b, expected.b));
    assert(near(actual.a, expected.a));
}

std::uint64_t hash(const SemanticFramePixels& frame) {
    std::uint64_t value = 1469598103934665603ULL;
    for (std::uint8_t byte : frame.rgba8) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

bool encode_and_readback(Semantic2dPass& pass, const SemanticFrame& frame,
                         const SdlGpuFrameTarget& target, SemanticFramePixels& output,
                         std::string& error) {
    SDL_GPUDevice* device = target.device();
    const ReadbackResources resources = begin_readback(device, frame, error);
    if (resources.commandBuffer == nullptr)
        return false;
    const Semantic2dPassTarget passTarget{resources.commandBuffer, target.color(),
                                          target.desc().colorFormat, SDL_GPU_LOADOP_CLEAR,
                                          SDL_GPU_STOREOP_STORE};
    if (!pass.encode(frame, passTarget, error)) {
        SDL_CancelGPUCommandBuffer(resources.commandBuffer);
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, resources.download);
        return false;
    }
    return finish_readback(pass, frame, device, target.color(), resources, output, error);
}

bool encode_3d_and_readback(Semantic3dPass& pass, const SemanticFrame& frame,
                            const SdlGpuFrameTarget& target, SemanticFramePixels& output,
                            std::string& error) {
    SDL_GPUDevice* device = target.device();
    const ReadbackResources resources = begin_readback(device, frame, error);
    if (resources.commandBuffer == nullptr)
        return false;
    const Semantic3dPassTarget passTarget{resources.commandBuffer, target.color(),
                                          target.desc().colorFormat, target.depth(),
                                          target.desc().depthFormat};
    if (!pass.encode(frame, passTarget, error)) {
        SDL_CancelGPUCommandBuffer(resources.commandBuffer);
        std::string completionError;
        (void)pass.complete_encode(false, completionError);
        SDL_ReleaseGPUTransferBuffer(device, resources.download);
        return false;
    }
    return finish_readback(pass, frame, device, target.color(), resources, output, error);
}

} // namespace sb::native_render::test
