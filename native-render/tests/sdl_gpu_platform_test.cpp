#include <sunbright/native_render/sdl_gpu_frame_target.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using sb::native_render::PresentResult;
using sb::native_render::SdlGpuCalls;
using sb::native_render::SdlGpuFrameTarget;
using sb::native_render::SdlGpuFrameTargetDesc;
using sb::native_render::SdlGpuPlatform;
using sb::native_render::SdlGpuPlatformConfig;

void require(bool condition) {
    if (!condition)
        std::abort();
}

enum class Event : std::uint8_t {
    CreateDevice,
    ClaimWindow,
    SetSwapchain,
    SetFramesInFlight,
    CreateTexture,
    ReleaseTexture,
    GetWindowSize,
    AcquireSwapchain,
    BeginRenderPass,
    EndRenderPass,
    Blit,
    ReleaseWindow,
    DestroyDevice,
};

struct FakeSdl {
    static FakeSdl* active;

    SDL_InitFlags initialized = SDL_INIT_VIDEO;
    bool createDevice = true;
    bool claimWindow = true;
    bool setSwapchain = true;
    bool setFramesInFlight = true;
    int failTextureCreation = -1;
    bool getWindowSize = true;
    bool acquireSwapchain = true;
    bool swapchainAvailable = true;
    bool beginRenderPass = true;
    SDL_WindowFlags windowFlags = 0;
    int windowWidth = 1280;
    int windowHeight = 720;
    Uint32 swapchainWidth = 1280;
    Uint32 swapchainHeight = 720;
    int textureCreateCount = 0;
    std::array<std::byte, 16> textureStorage{};
    std::size_t nextTexture = 0;
    SDL_GPUDevice* device = reinterpret_cast<SDL_GPUDevice*>(0x100);
    SDL_Window* window = reinterpret_cast<SDL_Window*>(0x200);
    SDL_Window* secondWindow = reinterpret_cast<SDL_Window*>(0x201);
    SDL_GPUTexture* swapchain = reinterpret_cast<SDL_GPUTexture*>(0x300);
    SDL_GPURenderPass* renderPass = reinterpret_cast<SDL_GPURenderPass*>(0x400);
    SDL_GPUCommandBuffer* commandBuffer = reinterpret_cast<SDL_GPUCommandBuffer*>(0x500);
    std::vector<Event> events{};
    std::vector<SDL_GPUDevice*> textureDevices{};
    std::vector<SDL_GPUTextureCreateInfo> textureInfos{};
    std::vector<SDL_GPUTexture*> releasedTextures{};
    SDL_GPUBlitInfo lastBlit{};
    SDL_GPUShaderFormat createdShaderFormats = 0;
    bool createdDebugMode = false;
    std::string createdDriver{};

    FakeSdl() { active = this; }
    ~FakeSdl() { active = nullptr; }

    [[nodiscard]] SdlGpuCalls calls() const {
        return {
            .wasInit = &fake_was_init,
            .createGpuDevice = &fake_create_device,
            .destroyGpuDevice = &fake_destroy_device,
            .claimWindow = &fake_claim_window,
            .releaseWindow = &fake_release_window,
            .setSwapchainParameters = &fake_set_swapchain,
            .setAllowedFramesInFlight = &fake_set_frames_in_flight,
            .getWindowSizeInPixels = &fake_get_window_size,
            .getWindowFlags = &fake_get_window_flags,
            .acquireSwapchainTexture = &fake_acquire_swapchain,
            .beginRenderPass = &fake_begin_render_pass,
            .endRenderPass = &fake_end_render_pass,
            .blitTexture = &fake_blit,
            .createTexture = &fake_create_texture,
            .releaseTexture = &fake_release_texture,
            .getError = &fake_get_error,
        };
    }

    static SDL_InitFlags SDLCALL fake_was_init(SDL_InitFlags) { return active->initialized; }

    static SDL_GPUDevice* SDLCALL fake_create_device(SDL_GPUShaderFormat formats, bool debugMode,
                                                     const char* driver) {
        active->events.push_back(Event::CreateDevice);
        active->createdShaderFormats = formats;
        active->createdDebugMode = debugMode;
        active->createdDriver = driver != nullptr ? driver : "";
        return active->createDevice ? active->device : nullptr;
    }

    static void SDLCALL fake_destroy_device(SDL_GPUDevice* device) {
        require(device == active->device);
        active->events.push_back(Event::DestroyDevice);
    }

    static bool SDLCALL fake_claim_window(SDL_GPUDevice* device, SDL_Window* window) {
        require(device == active->device);
        require(window == active->window || window == active->secondWindow);
        active->events.push_back(Event::ClaimWindow);
        return active->claimWindow;
    }

    static void SDLCALL fake_release_window(SDL_GPUDevice* device, SDL_Window* window) {
        require(device == active->device);
        require(window == active->window || window == active->secondWindow);
        active->events.push_back(Event::ReleaseWindow);
    }

    static bool SDLCALL fake_set_swapchain(SDL_GPUDevice* device, SDL_Window* window,
                                           SDL_GPUSwapchainComposition, SDL_GPUPresentMode) {
        require(device == active->device && window == active->window);
        active->events.push_back(Event::SetSwapchain);
        return active->setSwapchain;
    }

    static bool SDLCALL fake_set_frames_in_flight(SDL_GPUDevice* device, Uint32) {
        require(device == active->device);
        active->events.push_back(Event::SetFramesInFlight);
        return active->setFramesInFlight;
    }

    static bool SDLCALL fake_get_window_size(SDL_Window* window, int* width, int* height) {
        require(window == active->window);
        active->events.push_back(Event::GetWindowSize);
        if (!active->getWindowSize)
            return false;
        *width = active->windowWidth;
        *height = active->windowHeight;
        return true;
    }

    static SDL_WindowFlags SDLCALL fake_get_window_flags(SDL_Window* window) {
        require(window == active->window);
        return active->windowFlags;
    }

    static bool SDLCALL fake_acquire_swapchain(SDL_GPUCommandBuffer* commandBuffer,
                                               SDL_Window* window, SDL_GPUTexture** texture,
                                               Uint32* width, Uint32* height) {
        require(commandBuffer == active->commandBuffer && window == active->window);
        active->events.push_back(Event::AcquireSwapchain);
        if (!active->acquireSwapchain)
            return false;
        *texture = active->swapchainAvailable ? active->swapchain : nullptr;
        *width = active->swapchainAvailable ? active->swapchainWidth : 0;
        *height = active->swapchainAvailable ? active->swapchainHeight : 0;
        return true;
    }

    static SDL_GPURenderPass* SDLCALL fake_begin_render_pass(
        SDL_GPUCommandBuffer* commandBuffer, const SDL_GPUColorTargetInfo* colorTargets,
        Uint32 colorTargetCount, const SDL_GPUDepthStencilTargetInfo*) {
        require(commandBuffer == active->commandBuffer);
        require(colorTargets != nullptr && colorTargetCount == 1);
        require(colorTargets[0].texture == active->swapchain);
        active->events.push_back(Event::BeginRenderPass);
        return active->beginRenderPass ? active->renderPass : nullptr;
    }

    static void SDLCALL fake_end_render_pass(SDL_GPURenderPass* pass) {
        require(pass == active->renderPass);
        active->events.push_back(Event::EndRenderPass);
    }

    static void SDLCALL fake_blit(SDL_GPUCommandBuffer* commandBuffer,
                                  const SDL_GPUBlitInfo* info) {
        require(commandBuffer == active->commandBuffer && info != nullptr);
        active->lastBlit = *info;
        active->events.push_back(Event::Blit);
    }

    static SDL_GPUTexture* SDLCALL fake_create_texture(SDL_GPUDevice* device,
                                                       const SDL_GPUTextureCreateInfo* info) {
        require(device == active->device && info != nullptr);
        const int creation = active->textureCreateCount++;
        active->events.push_back(Event::CreateTexture);
        active->textureDevices.push_back(device);
        active->textureInfos.push_back(*info);
        if (creation == active->failTextureCreation)
            return nullptr;
        require(active->nextTexture < active->textureStorage.size());
        return reinterpret_cast<SDL_GPUTexture*>(&active->textureStorage[active->nextTexture++]);
    }

    static void SDLCALL fake_release_texture(SDL_GPUDevice* device, SDL_GPUTexture* texture) {
        require(device == active->device && texture != nullptr);
        active->releasedTextures.push_back(texture);
        active->events.push_back(Event::ReleaseTexture);
    }

    static const char* SDLCALL fake_get_error() { return "planted SDL failure"; }
};

FakeSdl* FakeSdl::active = nullptr;

std::size_t event_index(const std::vector<Event>& events, Event event) {
    const auto found = std::find(events.begin(), events.end(), event);
    require(found != events.end());
    return static_cast<std::size_t>(found - events.begin());
}

void platform_and_two_clients_share_one_owner() {
    FakeSdl fake;
    std::string error;
    SdlGpuPlatform platform(fake.calls());
    SdlGpuPlatformConfig config{.debugMode = false, .driverName = "planted-driver"};
    require(platform.initialize(fake.window, config, error));
    require(error.empty());
    require(platform.ready() && platform.device() == fake.device &&
            platform.window() == fake.window);
    require(fake.createdShaderFormats == SDL_GPU_SHADERFORMAT_SPIRV);
    require(!fake.createdDebugMode && fake.createdDriver == "planted-driver");

    // An identical request reuses the established owner. A changed window cannot silently steal
    // the device or swapchain from the first owner.
    require(platform.initialize(fake.window, config, error));
    require(std::count(fake.events.begin(), fake.events.end(), Event::CreateDevice) == 1);
    require(!platform.initialize(fake.secondWindow, config, error));
    require(!error.empty());

    SdlGpuFrameTarget first;
    SdlGpuFrameTarget second;
    SdlGpuFrameTargetDesc firstDesc{.width = 640, .height = 448};
    SdlGpuFrameTargetDesc secondDesc{.width = 1280, .height = 720, .hasDepth = false};
    require(first.initialize(platform, firstDesc, error));
    require(second.initialize(platform, secondDesc, error));
    require(first.device() == platform.device() && second.device() == platform.device());
    require(first.color() != second.color());
    require(first.depth() != nullptr && second.depth() == nullptr);
    require(fake.textureCreateCount == 3);
    require(fake.textureInfos[0].format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB);
    require(std::all_of(fake.textureDevices.begin(), fake.textureDevices.end(),
                        [&](SDL_GPUDevice* device) { return device == platform.device(); }));

    platform.set_present_aspect(4, 3);
    require(platform.encode_present(fake.commandBuffer, first.color(), first.desc().width,
                                    first.desc().height, error) == PresentResult::Presented);
    require(error.empty());
    require(fake.lastBlit.source.texture == first.color());
    require(fake.lastBlit.destination.texture == fake.swapchain);
    require(fake.lastBlit.destination.x == 160 && fake.lastBlit.destination.y == 0);
    require(fake.lastBlit.destination.w == 960 && fake.lastBlit.destination.h == 720);

    require(!platform.shutdown(error));
    require(error.find("live frame targets") != std::string::npos);
    require(platform.ready());
    first.shutdown();
    second.shutdown();
    require(platform.shutdown(error));
    require(platform.shutdown(error));
    require(fake.releasedTextures.size() == 3);
    require(std::count(fake.events.begin(), fake.events.end(), Event::ReleaseWindow) == 1);
    require(std::count(fake.events.begin(), fake.events.end(), Event::DestroyDevice) == 1);
    const std::size_t lastTextureRelease = static_cast<std::size_t>(
        std::find(fake.events.rbegin(), fake.events.rend(), Event::ReleaseTexture).base() -
        fake.events.begin() - 1);
    require(lastTextureRelease < event_index(fake.events, Event::ReleaseWindow));
    require(event_index(fake.events, Event::ReleaseWindow) <
            event_index(fake.events, Event::DestroyDevice));
}

void initialization_failures_unwind_exact_ownership() {
    {
        FakeSdl fake;
        fake.initialized = 0;
        const SdlGpuCalls calls = fake.calls();
        SdlGpuPlatform platform(calls);
        std::string error;
        require(!platform.initialize(fake.window, {}, error));
        require(fake.events.empty());
        require(error.find("window owner") != std::string::npos);
    }
    {
        FakeSdl fake;
        fake.claimWindow = false;
        const SdlGpuCalls calls = fake.calls();
        SdlGpuPlatform platform(calls);
        std::string error;
        require(!platform.initialize(fake.window, {}, error));
        require(std::count(fake.events.begin(), fake.events.end(), Event::ReleaseWindow) == 0);
        require(fake.events.back() == Event::DestroyDevice);
    }
    for (const bool failSwapchain : {true, false}) {
        FakeSdl fake;
        fake.setSwapchain = !failSwapchain;
        fake.setFramesInFlight = failSwapchain;
        const SdlGpuCalls calls = fake.calls();
        SdlGpuPlatform platform(calls);
        std::string error;
        require(!platform.initialize(fake.window, {}, error));
        require(std::count(fake.events.begin(), fake.events.end(), Event::ReleaseWindow) == 1);
        require(fake.events[fake.events.size() - 2] == Event::ReleaseWindow);
        require(fake.events.back() == Event::DestroyDevice);
    }
}

void frame_target_failure_releases_partial_allocation() {
    FakeSdl fake;
    const SdlGpuCalls calls = fake.calls();
    SdlGpuPlatform platform(calls);
    std::string error;
    require(platform.initialize(fake.window, {}, error));

    fake.failTextureCreation = 1;
    SdlGpuFrameTarget target;
    require(!target.initialize(platform, {.width = 320, .height = 240}, error));
    require(!target.ready());
    require(fake.releasedTextures.size() == 1);
    require(fake.textureInfos.size() == 2);
    require(fake.textureInfos[0].usage ==
            (SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER));
    require(fake.textureInfos[1].usage == SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);

    const int createsBeforeInvalid = fake.textureCreateCount;
    require(!target.initialize(platform, {.width = 0, .height = 240}, error));
    require(fake.textureCreateCount == createsBeforeInvalid);
}

void presenter_reports_unavailable_and_failed_states() {
    FakeSdl fake;
    const SdlGpuCalls calls = fake.calls();
    SdlGpuPlatform platform(calls);
    std::string error;
    require(platform.initialize(fake.window, {}, error));
    SDL_GPUTexture* source = reinterpret_cast<SDL_GPUTexture*>(0x900);

    fake.windowFlags = SDL_WINDOW_HIDDEN;
    require(platform.encode_present(fake.commandBuffer, source, 640, 448, error) ==
            PresentResult::WindowUnavailable);
    require(std::count(fake.events.begin(), fake.events.end(), Event::AcquireSwapchain) == 0);

    fake.windowFlags = 0;
    fake.swapchainAvailable = false;
    require(platform.encode_present(fake.commandBuffer, source, 640, 448, error) ==
            PresentResult::WindowUnavailable);

    fake.swapchainAvailable = true;
    fake.acquireSwapchain = false;
    require(platform.encode_present(fake.commandBuffer, source, 640, 448, error) ==
            PresentResult::Failed);
    require(error.find("planted SDL failure") != std::string::npos);

    fake.acquireSwapchain = true;
    fake.beginRenderPass = false;
    require(platform.encode_present(fake.commandBuffer, source, 640, 448, error) ==
            PresentResult::Failed);
    require(std::count(fake.events.begin(), fake.events.end(), Event::Blit) == 0);
}

void controls_reject_an_incomplete_call_table() {
    FakeSdl fake;
    SdlGpuCalls calls = fake.calls();
    require(sb::native_render::valid(calls));
    calls.createGpuDevice = nullptr;
    require(!sb::native_render::valid(calls));
    SdlGpuPlatform platform(calls);
    std::string error;
    require(!platform.initialize(fake.window, {}, error));
    require(fake.events.empty());
}

void viewport_policy_controls() {
    using sb::native_render::present_viewport;
    using sb::native_render::PresentViewport;
    require(present_viewport(1280, 960, 4, 3) == PresentViewport{0, 0, 1280, 960});
    require(present_viewport(1920, 1080, 4, 3) == PresentViewport{240, 0, 1440, 1080});
    require(present_viewport(1024, 1024, 16, 9) == PresentViewport{0, 224, 1024, 576});
    require(present_viewport(0, 1080, 4, 3) == PresentViewport{});
}

} // namespace

int main() {
    viewport_policy_controls();
    controls_reject_an_incomplete_call_table();
    initialization_failures_unwind_exact_ownership();
    frame_target_failure_releases_partial_allocation();
    presenter_reports_unavailable_and_failed_states();
    platform_and_two_clients_share_one_owner();
    return 0;
}
