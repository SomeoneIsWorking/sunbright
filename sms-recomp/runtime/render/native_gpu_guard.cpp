#include "native_gpu_guard.h"

#include "native_gpu_frame_state.h"

#include <SDL3/SDL.h>

#include <lucent/log.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr int kMaximumPassesPerFrame = 4;
constexpr double kMaximumFenceTimeoutSeconds = 60.0;

SDL_GPUDevice* g_device = nullptr;
bool g_dead = false;
int g_passesThisFrame = 0;
NativeGpuFrameState g_frameState;

} // namespace

void sbr_native_gpu_guard_set_device(SDL_GPUDevice* device) noexcept {
    g_device = device;
}

void sbr_native_gpu_disable(const char* reason) {
    if (g_dead)
        return;
    g_dead = true;
    g_frameState.fail_frame();
    if (FILE* stamp = std::fopen("scratch/gpu_fault.stamp", "wb")) {
        std::fprintf(stamp, "%s\n", reason);
        std::fclose(stamp);
    }
    lucent::error("nrender",
                  "NATIVE RENDERER DISABLED FOR THE REST OF THIS RUN: {}. Everything this path "
                  "would submit from here is dropped. It renders offscreen and is only scored "
                  "against aurora, so the displayed frame is unaffected.",
                  reason);
}

bool sbr_native_gpu_dead() noexcept {
    return g_dead;
}

std::optional<double> sbr_native_gpu_parse_fence_timeout(std::string_view value) noexcept {
    if (value.empty())
        return std::nullopt;
    const std::string text(value);
    char* end = nullptr;
    errno = 0;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' || !std::isfinite(parsed) ||
        parsed < 0.0 || parsed > kMaximumFenceTimeoutSeconds)
        return std::nullopt;
    return parsed;
}

double sbr_native_gpu_fence_timeout_secs() noexcept {
    static const double timeout = [] {
        const char* value = std::getenv("SBR_GPU_FENCE_TIMEOUT");
        if (value == nullptr)
            return 5.0;
        const auto parsed = sbr_native_gpu_parse_fence_timeout(value);
        if (!parsed) {
            lucent::error("nrender",
                          "SBR_GPU_FENCE_TIMEOUT='{}' is invalid; expected a finite value from 0 "
                          "through {} seconds",
                          value, kMaximumFenceTimeoutSeconds);
            std::abort();
        }
        return *parsed;
    }();
    return timeout;
}

bool sbr_native_gpu_wait_fence(SDL_GPUFence* fence, const char* operation) {
    const Uint64 start = SDL_GetTicksNS();
    for (;;) {
        if (SDL_QueryGPUFence(g_device, fence))
            return true;
        const double waited = static_cast<double>(SDL_GetTicksNS() - start) / 1e9;
        if (waited > sbr_native_gpu_fence_timeout_secs()) {
            lucent::error("nrender",
                          "{}: the GPU has not signalled its fence in {:.1f}s; treating the device "
                          "as hung instead of waiting without a bound",
                          operation, waited);
            return false;
        }
        SDL_DelayNS(200000);
    }
}

void sbr_native_gpu_begin_frame() noexcept {
    g_passesThisFrame = 0;
    g_frameState.begin_frame();
}

bool sbr_native_gpu_admit_pass() {
    ++g_passesThisFrame;
    if (g_passesThisFrame <= kMaximumPassesPerFrame)
        return true;
    g_frameState.fail_frame();
    if (g_passesThisFrame == kMaximumPassesPerFrame + 1) {
        lucent::error("nrender",
                      "REFUSING a {}th offscreen pass this frame (cap {}). Each pass is a full "
                      "re-render plus a fenced readback; diagnostic work must be spread across "
                      "frames.",
                      g_passesThisFrame, kMaximumPassesPerFrame);
    }
    return false;
}

void sbr_native_gpu_complete_frame() noexcept {
    g_frameState.complete_frame();
}

void sbr_native_gpu_fail_frame() noexcept {
    g_frameState.fail_frame();
}

bool sbr_native_gpu_frame_readable() noexcept {
    return g_frameState.readable();
}

void sbr_native_gpu_reset_passes() noexcept {
    g_passesThisFrame = 0;
}

int sbr_native_gpu_passes_attempted() noexcept {
    return g_passesThisFrame;
}

int sbr_native_gpu_max_passes() noexcept {
    return kMaximumPassesPerFrame;
}
