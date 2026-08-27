#pragma once

#include <cstdint>

struct SDL_GPUCommandBuffer;
struct SDL_GPUDevice;
struct SDL_GPUTexture;
struct SDL_Window;

struct NativePresentViewport {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

NativePresentViewport sbr_native_present_viewport(std::uint32_t targetWidth,
                                                  std::uint32_t targetHeight,
                                                  std::uint32_t aspectWidth,
                                                  std::uint32_t aspectHeight) noexcept;

enum class NativePresenterLifecycle { Uninitialized, Claimed, Ready };
enum class NativePresenterInitializeAction { Claim, Reuse, Reject };

NativePresenterInitializeAction
sbr_native_presenter_initialize_action(NativePresenterLifecycle lifecycle, bool validRequest,
                                       bool sameOwner) noexcept;
[[nodiscard]] bool
sbr_native_presenter_shutdown_releases(NativePresenterLifecycle lifecycle) noexcept;

enum class NativePresenterAvailability { Ready, Unavailable, Failed };

NativePresenterAvailability sbr_native_presenter_window_availability(bool sizeQuerySucceeded,
                                                                     bool hiddenOrMinimized,
                                                                     int pixelWidth,
                                                                     int pixelHeight) noexcept;
NativePresenterAvailability
sbr_native_presenter_acquire_availability(bool acquireSucceeded, bool hasTexture,
                                          std::uint32_t textureWidth,
                                          std::uint32_t textureHeight) noexcept;

enum class NativePresentResult { Presented, WindowUnavailable, Failed };

// Claims Aurora's SDL window for the native SDL GPU device. Aurora must release its WSI surface
// before this is called; from that point the GX compatibility path is the only operating-system
// presenter.
bool sbr_native_presenter_initialize(SDL_GPUDevice* device, SDL_Window* window) noexcept;
void sbr_native_presenter_set_aspect(std::uint32_t width, std::uint32_t height) noexcept;
NativePresentResult sbr_native_presenter_encode(SDL_GPUCommandBuffer* commandBuffer,
                                                SDL_GPUTexture* source, std::uint32_t sourceWidth,
                                                std::uint32_t sourceHeight) noexcept;
void sbr_native_presenter_shutdown() noexcept;
