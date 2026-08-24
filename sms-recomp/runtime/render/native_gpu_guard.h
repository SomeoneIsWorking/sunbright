#pragma once

#include <SDL3/SDL_gpu.h>

#include <optional>
#include <string_view>

void sbr_native_gpu_guard_set_device(SDL_GPUDevice* device) noexcept;
void sbr_native_gpu_disable(const char* reason);
[[nodiscard]] bool sbr_native_gpu_dead() noexcept;
[[nodiscard]] bool sbr_native_gpu_wait_fence(SDL_GPUFence* fence, const char* operation);

void sbr_native_gpu_begin_frame() noexcept;
[[nodiscard]] bool sbr_native_gpu_admit_pass();
void sbr_native_gpu_complete_frame() noexcept;
void sbr_native_gpu_fail_frame() noexcept;
[[nodiscard]] bool sbr_native_gpu_frame_readable() noexcept;

void sbr_native_gpu_reset_passes() noexcept;
[[nodiscard]] int sbr_native_gpu_passes_attempted() noexcept;
[[nodiscard]] int sbr_native_gpu_max_passes() noexcept;
[[nodiscard]] double sbr_native_gpu_fence_timeout_secs() noexcept;
[[nodiscard]] std::optional<double>
sbr_native_gpu_parse_fence_timeout(std::string_view value) noexcept;
