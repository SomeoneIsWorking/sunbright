// Sunbright launcher: SDL2 window + Dolphin Core boot + recomp JIT hook.
// Host_ callbacks live here (not in stubs) so focus/title/resize work for real.

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <atomic>
#include <cmath>
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
#include "Core/Config/GraphicsSettings.h"
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
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/System.h"
#include "InputCommon/InputConfig.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "UICommon/DiscordPresence.h"
#include "UICommon/UICommon.h"

#include "sunbright_bridge.h"

#include <optional>
#include <string_view>

// ── Globals ───────────────────────────────────────────────────────────────────
static SDL_Window* g_window  = nullptr;
static std::atomic<bool> g_focused{false};
static std::atomic<bool> g_running{true};

// ── Keyboard → GameCube pad ─────────────────────────────────────────────────
// We inject controller state via Dolphin's per-controller input override (no
// device mappings needed). SDL key events set bits here; the override (called on
// the emulation thread when the SI device reads pad 0) turns them into button /
// stick states. Default GC controller; arrows = stick, Enter = Start.
enum PadBit : uint32_t {
    P_START = 1u<<0, P_A = 1u<<1, P_B = 1u<<2, P_X = 1u<<3, P_Y = 1u<<4,
    P_Z = 1u<<5, P_L = 1u<<6, P_R = 1u<<7,
    P_UP = 1u<<8, P_DOWN = 1u<<9, P_LEFT = 1u<<10, P_RIGHT = 1u<<11,
};
static std::atomic<uint32_t> g_pad{0};

static uint32_t key_to_padbit(SDL_Keycode k) {
    switch (k) {
    case SDLK_RETURN: return P_START;
    case SDLK_z:      return P_A;       // jump
    case SDLK_x:      return P_B;
    case SDLK_c:      return P_X;
    case SDLK_v:      return P_Y;
    case SDLK_q:      return P_Z;       // FLUDD swap
    case SDLK_a:      return P_L;
    case SDLK_s:      return P_R;       // spray
    case SDLK_UP:     return P_UP;
    case SDLK_DOWN:   return P_DOWN;
    case SDLK_LEFT:   return P_LEFT;
    case SDLK_RIGHT:  return P_RIGHT;
    default:          return 0;
    }
}

static std::optional<ControlState>
pad_override(std::string_view group, std::string_view control, ControlState /*base*/) {
    const uint32_t p = g_pad.load(std::memory_order_relaxed);
    auto on = [&](PadBit b) { return (p & b) ? std::optional<ControlState>(1.0) : std::nullopt; };
    if (group == "Buttons") {
        if (control == "Start") return on(P_START);
        if (control == "A")     return on(P_A);
        if (control == "B")     return on(P_B);
        if (control == "X")     return on(P_X);
        if (control == "Y")     return on(P_Y);
        if (control == "Z")     return on(P_Z);
    } else if (group == "Main Stick") {
        // The analog stick queries "X"/"Y" axes (-1..+1), not direction buttons.
        if (control == "X") {
            if (p & P_RIGHT) return  1.0;
            if (p & P_LEFT)  return -1.0;
        } else if (control == "Y") {
            if (p & P_UP)    return  1.0;
            if (p & P_DOWN)  return -1.0;
        }
    } else if (group == "Triggers") {
        if (control == "L" || control == "L-Analog") return on(P_L);
        if (control == "R" || control == "R-Analog") return on(P_R);
    }
    return std::nullopt;
}

// ── Runtime "find Mario" (SUNBRIGHT_FINDMARIO=1) ─────────────────────────────
// Symbol-free cheat-search for a moving object. Skip the intro to the file-select,
// then drive Mario right, then left, snapshotting RAM at each phase. A float that
// goes up (moved right) then down (moved left) — i.e. an X coordinate under our
// control — is almost certainly Mario's position. Reports candidate addresses; we
// then read the surrounding 3x4 matrix and watch it across frames to interpolate.
static float be_f32(const u8* p) {  // GC RAM is big-endian
    u32 b = ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3];
    float f; std::memcpy(&f, &b, 4); return f;
}
// Keyboard-triggered (press F5): you drive Mario, I snapshot on your cue. The two
// "still" snapshots give a noise baseline — anything that changes while Mario stands
// still is animation/timer noise and is discarded. 5-step sequence:
//   F5 #1: standing still            → S1
//   F5 #2: still standing still      → S2   (S1==S2 ⇒ that float is noise-free)
//   F5 #3: after moving RIGHT        → S3
//   F5 #4: standing still again      → S4   (S3==S4 ⇒ Mario stopped)
//   F5 #5: after a TINY move LEFT    → S5 → diff
// Survivors: stable when still (S1≈S2, S3≈S4) but changed with input and reversed
// (S2→S3 one way, S4→S5 the other). Prompts go to stdout; candidates to a file so
// they don't drown in Dolphin's logs.
static void findmario_step() {
    constexpr u32 RAM_BASE = 0x80000000, RAM_SIZE = 0x1800000;
    static std::vector<u8> S[5];
    static int phase = 0;
    auto snap = [&](std::vector<u8>& dst) {
        u8* ram = Core::System::GetInstance().GetMemory().GetPointerForRange(RAM_BASE, RAM_SIZE);
        if (ram) { dst.resize(RAM_SIZE); std::memcpy(dst.data(), ram, RAM_SIZE); }
    };
    static const char* prompts[5] = {
        "S1 captured (still). Press F5 again while STILL.",
        "S2 captured (still). Now MOVE RIGHT, stop, press F5.",
        "S3 captured (right). Stand STILL, press F5.",
        "S4 captured (still). Move LEFT a tiny bit, stop, press F5 to finish.",
        "S5 captured (left). Diffing → see mario_candidates.txt",
    };
    snap(S[phase]);
    printf("[mario] %s\n", prompts[phase]); fflush(stdout);
    if (++phase < 5) return;
    phase = 0;  // re-runnable

    FILE* f = fopen("mario_candidates.txt", "w");
    if (!f) return;
    int found = 0;
    for (u32 i = 0; i + 4 <= RAM_SIZE && found < 60; i += 4) {
        float s1 = be_f32(&S[0][i]), s2 = be_f32(&S[1][i]), s3 = be_f32(&S[2][i]),
              s4 = be_f32(&S[3][i]), s5 = be_f32(&S[4][i]);
        bool fin = std::isfinite(s1) && std::isfinite(s2) && std::isfinite(s3) &&
                   std::isfinite(s4) && std::isfinite(s5);
        if (!fin || std::abs(s1) > 1e5f) continue;
        float dR = s3 - s2, dL = s5 - s4;        // moved right, moved left
        bool stillStable = std::abs(s2 - s1) < 1.0f && std::abs(s4 - s3) < 1.0f;
        bool movedRight  = std::abs(dR) > 2.0f;
        bool reversed    = std::abs(dL) > 0.5f && (dR > 0) != (dL > 0);
        if (stillStable && movedRight && reversed) {
            fprintf(f, "cand %08x  still=%.2f right=%.2f left=%.2f\n",
                    RAM_BASE + i, s2, s3, s5);
            if (i >= 16) {
                fprintf(f, "     ctx:");
                for (int k = -4; k <= 8; k++) fprintf(f, " [%+d]%.2f", k*4, be_f32(&S[2][i + k*4]));
                fprintf(f, "\n");
            }
            ++found;
        }
    }
    fprintf(f, "== %d candidate(s) ==\n", found);
    fclose(f);
    printf("[mario] %d candidate(s) written to mario_candidates.txt\n", found); fflush(stdout);
}

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
    // Dolphin's title carries the FPS/VPS counters; logging it confirms whether
    // frames are actually being presented (VPS > 0 = the GPU pipeline is alive).
    static bool log = getenv("SUNBRIGHT_VLOG") != nullptr;
    if (log) fprintf(stderr, "[title] %s\n", title.c_str());
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

    // Ensure a Standard Controller is on port 0 so the game polls pad input.
    Config::SetBase(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);

    // SUNBRIGHT_DUMP=1: dump every presented frame as a PNG to the user Dump/Frames
    // dir. Definitive proof of what's rendered, independent of window capture (which
    // is unreliable under XWayland).
    if (getenv("SUNBRIGHT_DUMP")) {
        Config::SetBase(Config::MAIN_MOVIE_DUMP_FRAMES, true);
        Config::SetBase(Config::GFX_DUMP_FRAMES_AS_IMAGES, true);
        fprintf(stderr, "[sunbright] Frame dumping enabled\n");
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

    // Route keyboard → GameCube pad 0 via the input override (Pad is initialized
    // by BootCore). No device mapping needed — the override supplies state directly.
    if (InputConfig* pad_cfg = Pad::GetConfig(); pad_cfg && pad_cfg->GetControllerCount() > 0) {
        pad_cfg->GetController(0)->SetInputOverrideFunction(pad_override);
        fprintf(stderr, "[sunbright] Pad 0 input override installed "
                        "(Enter=Start, Z=A, X=B, arrows=stick, S=R-spray, Q=Z)\n");
    } else {
        fprintf(stderr, "[sunbright] Warning: no GC pad config — input disabled\n");
    }

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
                else if (ev.key.keysym.sym == SDLK_F5 && getenv("SUNBRIGHT_FINDMARIO"))
                    findmario_step();   // cheat-search: snapshot on your cue
                else if (uint32_t b = key_to_padbit(ev.key.keysym.sym))
                    g_pad.fetch_or(b, std::memory_order_relaxed);
                break;
            case SDL_KEYUP:
                if (uint32_t b = key_to_padbit(ev.key.keysym.sym))
                    g_pad.fetch_and(~b, std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }

        // SUNBRIGHT_AUTOSTART: with no physical keyboard (headless/CI), pulse
        // Start then A on a timer to drive through the title/file-select into
        // gameplay — also exercises the input-override path end to end.
        // SUNBRIGHT_AUTOSTART: input self-test. Spam Start for the first 15s, then
        // alternate holding RIGHT / LEFT every 2s — watch whether Mario walks back
        // and forth (confirms the keyboard→GCPad override actually drives the game).
        static const bool autostart = getenv("SUNBRIGHT_AUTOSTART") != nullptr;
        if (autostart) {
            const uint32_t t = SDL_GetTicks();
            uint32_t bits = 0;
            if (t < 15000) {
                if ((t % 1000) < 200) bits = P_START;     // spam Start ~5x/sec
            } else {
                bits = ((t / 2000) & 1) ? P_RIGHT : P_LEFT;  // hold right 2s, left 2s, …
                static uint32_t last = 0;
                if (t - last > 2000) { last = t;
                    fprintf(stdout, "[autotest] %s\n", (bits & P_RIGHT) ? "RIGHT" : "LEFT");
                    fflush(stdout);
                }
            }
            g_pad.store(bits, std::memory_order_relaxed);
        }

        // SUNBRIGHT_AUTOCAP: fully automated cheat-search (no F5). Skip intro, then
        // still/still/right/still/left while snapshotting → mario_candidates.txt.
        static const bool autocap = getenv("SUNBRIGHT_AUTOCAP") != nullptr;
        if (autocap) {
            const uint32_t t = SDL_GetTicks();
            uint32_t bits = 0;
            if (t < 15000)      { if ((t % 1000) < 200) bits = P_START; }  // skip intro
            else if (t < 18000) {}                                         // settle (still)
            else if (t < 22000) { bits = P_RIGHT; }                        // walk right
            else if (t < 24000) {}                                         // still
            else if (t < 28000) { bits = P_LEFT; }                         // walk left
            g_pad.store(bits, std::memory_order_relaxed);
            static int step = 0;
            static const uint32_t when[5] = {16000, 17500, 21500, 23500, 27500};
            if (step < 5 && t >= when[step]) { findmario_step(); ++step; }
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
