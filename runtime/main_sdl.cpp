// Sunbright launcher: SDL2 window + Dolphin Core boot + recomp JIT hook.
// Host_ callbacks live here (not in stubs) so focus/title/resize work for real.

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <execinfo.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Common/MsgHandler.h"
#include "Common/WindowSystemInfo.h"
#include "Core/Boot/Boot.h"
#include "Core/BootManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/DolphinAnalytics.h"
#include "Core/Host.h"
// X11 headers (pulled in by SDL_syswm.h) define these as macros, which collide
// with enum members inside Dolphin's PowerPC headers. Undef before including.
#undef None
#undef Bool
#undef Status
#undef Success
#undef Always
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/DiscordPresence.h"
#include "UICommon/UICommon.h"

#include "sunbright_bridge.h"

// ── Globals ───────────────────────────────────────────────────────────────────
static SDL_Window* g_window  = nullptr;
static std::atomic<bool> g_focused{false};
static std::atomic<bool> g_running{true};

// ── Host interface ────────────────────────────────────────────────────────────
std::vector<std::string> Host_GetPreferredLocales() { return {}; }
bool Host_UIBlocksControllerState()                  { return false; }
bool Host_RendererHasFocus()                         { return g_focused.load(); }
bool Host_RendererHasFullFocus()                     { return g_focused.load(); }
bool Host_RendererIsFullscreen() {
    if (!g_window) return false;
    const Uint32 flags = SDL_GetWindowFlags(g_window);
    return (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0;
}
bool Host_TASInputHasFocus() { return false; }
void Host_Message(HostMessageID id) {
    if (id == HostMessageID::WMUserStop) g_running = false;
}
void Host_PPCSymbolsChanged()     {}
void Host_PPCBreakpointsChanged() {}
void Host_RequestRenderWindowSize(int w, int h) {
    if (g_window) SDL_SetWindowSize(g_window, w, h);
}
void Host_UpdateDisasmDialog()    {}
void Host_JitCacheInvalidation()  {}
void Host_JitProfileDataWiped()   {}
void Host_UpdateTitle(const std::string& title) {
    if (g_window) SDL_SetWindowTitle(g_window, title.c_str());
}
void Host_YieldToUI()    { SDL_PumpEvents(); }
void Host_TitleChanged() {}

void Host_UpdateDiscordClientID(const std::string&) {}
bool Host_UpdateDiscordPresenceRaw(
    const std::string&, const std::string&, const std::string&, const std::string&,
    const std::string&, const std::string&, int64_t, int64_t, int, int) { return false; }

std::unique_ptr<GBAHostInterface>
Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>) { return nullptr; }

// ── Discord stubs (USE_DISCORD_PRESENCE is OFF — uicommon doesn't compile these) ──
namespace Discord {
struct Handler;
void Init() {}
void InitNetPlayFunctionality(Handler&) {}
void CallPendingCallbacks() {}
void UpdateClientID(const std::string&) {}
bool UpdateDiscordPresenceRaw(const std::string&, const std::string&,
    const std::string&, const std::string&, const std::string&, const std::string&,
    int64_t, int64_t, int, int) { return false; }
void UpdateDiscordPresence(int, SecretType, const std::string&, const std::string&, bool) {}
std::string CreateSecretFromIPAddress(const std::string&, int) { return {}; }
void Shutdown() {}
void SetDiscordPresenceEnabled(bool) {}
}  // namespace Discord

// ── SDL → WindowSystemInfo ────────────────────────────────────────────────────
static WindowSystemInfo build_wsi() {
    WindowSystemInfo wsi;
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
    if (!SDL_GetWindowWMInfo(g_window, &wm)) {
        fprintf(stderr, "[sunbright] SDL_GetWindowWMInfo: %s — running headless\n", SDL_GetError());
        wsi.type = WindowSystemType::Headless;
        return wsi;
    }
    switch (wm.subsystem) {
#if defined(SDL_VIDEO_DRIVER_X11)
    case SDL_SYSWM_X11:
        wsi.type               = WindowSystemType::X11;
        wsi.display_connection = wm.info.x11.display;
        wsi.render_window      = reinterpret_cast<void*>(wm.info.x11.window);
        wsi.render_surface     = wsi.render_window;
        break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
    case SDL_SYSWM_WAYLAND:
        wsi.type               = WindowSystemType::Wayland;
        wsi.display_connection = wm.info.wl.display;
        wsi.render_window      = wm.info.wl.surface;
        wsi.render_surface     = wsi.render_window;
        break;
#endif
    default:
        fprintf(stderr, "[sunbright] Unsupported SDL WM subsystem %d — headless\n",
                static_cast<int>(wm.subsystem));
        wsi.type = WindowSystemType::Headless;
    }
    return wsi;
}

// ── Signal handler ────────────────────────────────────────────────────────────
static void on_signal(int) { g_running = false; }

// ── Entry point ───────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Auto-ignore Dolphin panic alerts (headless — no Qt dialog available).
    // The apploader boot sequence triggers a benign ISI alert on return-to-LR=0;
    // we log it and continue so the game can keep running.
    Common::RegisterMsgAlertHandler([](const char* caption, const char* text,
                                       bool /*yes_no*/, Common::MsgType style) -> bool {
        fprintf(stderr, "[dolphin/%s] %s: %s\n",
                style == Common::MsgType::Warning ? "warn" : "err", caption, text);
        return true;  // "yes" / ignore and continue
    });

    const char* rom_path   = (argc > 1) ? argv[1]
                                        : "$SUNBRIGHT_ROM";
    const char* recomp_lib = (argc > 2) ? argv[2] : "build/libsms_recomp.so";

    // SDL
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    g_window = SDL_CreateWindow(
        "Sunbright — Super Mario Sunshine",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
    if (!g_window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    g_focused = true;

    // Load recompiled native functions into the JIT hook table
    if (!SunbrightBridge::Init(recomp_lib)) {
        fprintf(stderr,
                "[sunbright] Warning: could not load %s — game runs entirely via Dolphin JIT\n",
                recomp_lib);
    }

    const WindowSystemInfo wsi = build_wsi();
    fprintf(stderr, "[sunbright] WSI type=%d render_window=%p display=%p\n",
            static_cast<int>(wsi.type), wsi.render_window, wsi.display_connection);

    // Dolphin init
    fprintf(stderr, "[sunbright] UICommon::SetUserDirectory...\n");
    UICommon::SetUserDirectory("");  // empty → default <home>/.dolphin-emu
    fprintf(stderr, "[sunbright] UICommon::Init...\n");
    UICommon::Init();
    fprintf(stderr, "[sunbright] UICommon::Init done\n");

    // Enable only critical Dolphin logs to stderr (not file — too slow during BS2 boot)
    {
        auto* lm = Common::Log::LogManager::GetInstance();
        const bool vbose = getenv("SUNBRIGHT_VLOG") != nullptr;
        lm->SetConfigLogLevel(vbose ? Common::Log::LogLevel::LINFO
                                    : Common::Log::LogLevel::LWARNING);
        lm->SetEnable(Common::Log::LogType::VIDEO, true);
        lm->SetEnable(Common::Log::LogType::CORE, true);
        if (vbose) {
            lm->SetEnable(Common::Log::LogType::VIDEO, true);
            lm->SetEnable(Common::Log::LogType::HOST_GPU, true);
        }
    }

    // Override backend via env (e.g. SUNBRIGHT_BACKEND=OGL, Vulkan, Software)
    const char* backend_env = getenv("SUNBRIGHT_BACKEND");
    if (backend_env) {
        Config::SetBase(Config::MAIN_GFX_BACKEND, std::string(backend_env));
        fprintf(stderr, "[sunbright] Using backend: %s\n", backend_env);
    }

    fprintf(stderr, "[sunbright] UICommon::InitControllers...\n");
    UICommon::InitControllers(wsi);
    fprintf(stderr, "[sunbright] UICommon::InitControllers done\n");

    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_RESETHAND;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Log core state changes
    auto state_hook = Core::AddOnStateChangedCallback([](Core::State s) {
        const char* names[] = {"Uninitialized", "Starting", "Running", "Paused", "Stopping"};
        auto& ppc = Core::System::GetInstance().GetPPCState();
        fprintf(stderr, "[sunbright] Core state → %s  (pc=%08x lr=%08x)\n",
                static_cast<int>(s) < 5 ? names[static_cast<int>(s)] : "?",
                ppc.pc, ppc.spr[8]);
        if (s == Core::State::Paused) {
            void* bt[32];
            int n = backtrace(bt, 32);
            char** syms = backtrace_symbols(bt, n);
            for (int i = 0; i < n; i++) fprintf(stderr, "  [bt] %s\n", syms[i]);
            free(syms);
        }
        if (s == Core::State::Uninitialized) g_running = false;
    });

    fprintf(stderr, "[sunbright] GenerateFromFile...\n");
    auto boot = BootParameters::GenerateFromFile(rom_path);
    if (!boot) {
        fprintf(stderr, "[sunbright] Could not create boot parameters for: %s\n", rom_path);
        return 1;
    }
    fprintf(stderr, "[sunbright] GenerateFromFile done\n");

    DolphinAnalytics::Instance().ReportDolphinStart("sunbright");

    fprintf(stderr, "[sunbright] BootCore...\n");
    if (!BootManager::BootCore(Core::System::GetInstance(), std::move(boot), wsi)) {
        fprintf(stderr, "[sunbright] BootCore failed\n");
        return 1;
    }
    fprintf(stderr, "[sunbright] BootCore returned — game running\n");

    // Main event loop — Dolphin runs its CPU/GPU on its own threads
    while (g_running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                g_running = false;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                    g_focused = true;
                else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                    g_focused = false;
                // Forward resize to Dolphin's renderer
                else if (ev.window.event == SDL_WINDOWEVENT_RESIZED)
                    Host_RequestRenderWindowSize(ev.window.data1, ev.window.data2);
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_F11)
                    SDL_SetWindowFullscreen(g_window,
                        Host_RendererIsFullscreen() ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                break;
            default:
                break;
            }
        }
        SDL_Delay(1);
    }

    Core::Stop(Core::System::GetInstance());
    Core::Shutdown(Core::System::GetInstance());

    UICommon::ShutdownControllers();
    UICommon::Shutdown();

    SDL_DestroyWindow(g_window);
    SDL_Quit();

    SunbrightBridge::Shutdown();
    return 0;
}
