// Sunbright launcher: SDL2 window + Dolphin Core boot + recomp JIT hook.
// Host_ callbacks live here (not in stubs) so focus/title/resize work for real.

#include <SDL2/SDL.h>
#include <SDL2/SDL_syswm.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <sys/stat.h>
#include <execinfo.h>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
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
#include "VideoCommon/VideoConfig.h"   // AspectMode
#include "probe_server.h"
#include "VideoCommon/Present.h"       // g_presenter (surface resize)
#include "UICommon/DiscordPresence.h"
#include "UICommon/UICommon.h"

#include "sunbright_bridge.h"
#include "native_threads.h"

#include <optional>
#include <string_view>

// ── Globals ───────────────────────────────────────────────────────────────────
static SDL_Window* g_window  = nullptr;
static std::atomic<bool> g_focused{false};
static std::atomic<bool> g_running{true};
// True only while the core is in State::Running. The main/SDL thread pokes guest
// RAM each frame (widescreen patch, movie-skip, watchers) via GetPointerForRange;
// during State::Starting the EmuThread is in HW::Init/MemoryManager::Init building
// the memory arena, so a concurrent GetPointerForRange can hand back a torn/stale
// base pointer → wild write. Gate every main-thread guest-memory poke on this.
static std::atomic<bool> g_core_running{false};
static inline bool guest_mem_ready() { return g_core_running.load(std::memory_order_acquire); }

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
// REPL-driven pad bits, OR'd with the keyboard bits so scripted input and the
// keyboard coexist (SUNBRIGHT_REPL).
static std::atomic<uint32_t> g_repl_bits{0};
// Real GC-pad Start as Dolphin sees it (from the input override's `base` value),
// independent of our SDL event loop — physical input reaches the game through
// Dolphin's own input path, not our SDL window. Used to arm the movie skip.
static std::atomic<bool> g_phys_start{false};

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
pad_override(std::string_view group, std::string_view control, ControlState base) {
    const uint32_t p = g_pad.load(std::memory_order_relaxed)
                     | g_repl_bits.load(std::memory_order_relaxed);
    auto on = [&](PadBit b) { return (p & b) ? std::optional<ControlState>(1.0) : std::nullopt; };
    if (group == "Buttons") {
        if (control == "Start") {
            // Capture the REAL Start state (our injection OR Dolphin's own mapping
            // in `base`) so the movie skip works regardless of input source.
            g_phys_start.store((p & P_START) || base >= 0.5, std::memory_order_relaxed);
            return on(P_START);
        }
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

// ── Movie skip (intro THP movie → Start) ────────────────────────────────────
// Starting a new file plays the branded intro as a THP video driven by
// TMovieDirector::direct (USA 0x802b5b30), which runs under Dolphin's JIT. In its
// "playing" mode it polls THPPlayerGetState() and advances ONLY when that returns
// an end state (3 or 5); it never checks the pad, so the movie can't be skipped.
//
// THPPlayerGetState (0x8001e994) just reads one byte — the THP player's `state`
// field — at 0x803EC204 (0=stop 1=ready 2=playing 4=paused; 3/5=end). A recomp
// override there is bypassed because the JIT'd director links the call directly,
// but BOTH the JIT and our recomp read the same RAM byte. So: while a movie is
// playing/paused and Start is held, poke the state byte to an end value. The
// director then runs its OWN teardown (decideNextMode + THPPlayerStop) on the
// next frame — a clean skip with no forced sequence state. Transparent otherwise
// (we only write when a movie is actually active).
namespace {
constexpr u32 THP_STATE_ADDR = 0x803EC204;
constexpr u8  THP_PLAYING = 2, THP_PAUSED = 4;
}
static void movie_skip_tick(uint32_t pad_bits) {
    static const bool off = getenv("SUNBRIGHT_NO_MOVIESKIP") != nullptr;
    static const u8 end_state = (u8)(getenv("SUNBRIGHT_MOVIESKIP_STATE")
        ? strtoul(getenv("SUNBRIGHT_MOVIESKIP_STATE"), nullptr, 0) : 3);
    static const bool log = getenv("SUNBRIGHT_MOVIESKIP_LOG") != nullptr;
    if (!guest_mem_ready()) return;   // arena may be mid-(re)init on the EmuThread
    u8* st = Core::System::GetInstance().GetMemory().GetPointerForRange(THP_STATE_ADDR, 1);
    if (!st) return;
    const u8 s = *st;
    if (log) { static u8 last = 0xFF; if (s != last) {
        fprintf(stderr, "[movieskip] THP state @%08x -> %u\n", THP_STATE_ADDR, s); last = s; } }
    if (off) return;

    // The THP file streams from disc before it plays, so state bounces 0/1 for a
    // second or two (loading) and only then reaches 2 (playing). Rather than
    // require Start to be held at the exact frame it's playing, LATCH: pressing
    // Start any time a movie session is active (state 1/2/4) arms a skip, and we
    // force the end state as soon as it's playing. We disarm only after the
    // player has been STOPPED (0) continuously for a while, so brief loading dips
    // to 0 don't drop the latch — but a real movie end (or no movie) does.
    static bool armed = false;
    static uint32_t zero_since = 0;
    const uint32_t now = SDL_GetTicks();
    const bool movie_active = (s == 1 || s == THP_PLAYING || s == THP_PAUSED);

    const bool start = g_phys_start.load(std::memory_order_relaxed)
                     || (pad_bits & P_START);
    if (log && movie_active) { static uint32_t lpb = 0; if (now - lpb > 1000) { lpb = now;
        fprintf(stderr, "[movieskip] movie active (state=%u) START=%d (phys=%d)\n",
                s, (int)start, (int)g_phys_start.load(std::memory_order_relaxed)); } }
    if (start && movie_active && !armed) {
        armed = true;
        if (log) fprintf(stderr, "[movieskip] armed by Start (state=%u)\n", s);
    }
    if (s != 0) zero_since = 0;
    else if (zero_since == 0) zero_since = now;
    if (armed && zero_since && now - zero_since > 1500) {
        armed = false;
        if (log) fprintf(stderr, "[movieskip] disarmed (player stopped)\n");
    }
    if (armed && (s == THP_PLAYING || s == THP_PAUSED)) {
        if (log) { static uint32_t lp = 0; if (now - lp > 500) { lp = now;
            fprintf(stderr, "[movieskip] skipping (state=%u -> %u)\n", s, end_state); } }
        *st = end_state;
    }
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

// ── REPL: live input control channel (SUNBRIGHT_REPL) ───────────────────────
// Drive the GC pad with timed commands from a FIFO (or stdin) instead of
// recompiling hardcoded timelines. A reader thread parses lines into a queue of
// timed actions; the main loop runs one action at a time, holding pad bits for
// the requested duration. Lets us script "get into the game" interactively.
//
// Command grammar (one per line):
//   <combo> [ms]   hold button combo for ms (default 150).  combo = a|b|x|y|z|
//                  start|l|r|up|down|left|right joined by '+', e.g. left+a 80
//   jump [ms]      alias for 'a'
//   wait <ms>      idle (no buttons) for ms;  also: w <ms>
//   snap           run the find-Mario cheat-search snapshot step
//   mtx <hexaddr>  print the 3x4 transform's translation at <addr> once
//   say <text>     echo text to stdout (progress markers in scripts)
//   quit           stop the game
enum ReplOp { OP_HOLD, OP_SNAP, OP_MTX, OP_QUIT, OP_SAY };
struct ReplAct { ReplOp op; uint32_t bits; uint32_t ms; std::string text; };
static std::deque<ReplAct> g_repl_q;
static std::mutex g_repl_mtx;

static uint32_t repl_combo_bits(const std::string& combo) {
    uint32_t bits = 0;
    std::stringstream ss(combo);
    std::string tok;
    while (std::getline(ss, tok, '+')) {
        if      (tok == "start") bits |= P_START;
        else if (tok == "a" || tok == "jump") bits |= P_A;
        else if (tok == "b")     bits |= P_B;
        else if (tok == "x")     bits |= P_X;
        else if (tok == "y")     bits |= P_Y;
        else if (tok == "z")     bits |= P_Z;
        else if (tok == "l")     bits |= P_L;
        else if (tok == "r")     bits |= P_R;
        else if (tok == "up")    bits |= P_UP;
        else if (tok == "down")  bits |= P_DOWN;
        else if (tok == "left")  bits |= P_LEFT;
        else if (tok == "right") bits |= P_RIGHT;
    }
    return bits;
}

static void repl_parse_line(const std::string& raw) {
    std::string line = raw;
    if (auto h = line.find('#'); h != std::string::npos) line.erase(h);  // comments
    std::stringstream ss(line);
    std::string cmd;
    if (!(ss >> cmd)) return;                       // blank
    ReplAct a{};
    if (cmd == "quit" || cmd == "exit" || cmd == "q") {
        a.op = OP_QUIT;
    } else if (cmd == "snap") {
        a.op = OP_SNAP;
    } else if (cmd == "wait" || cmd == "w") {
        a.op = OP_HOLD; a.bits = 0; ss >> a.ms; if (!a.ms) a.ms = 150;
    } else if (cmd == "mtx") {
        std::string h; ss >> h; a.op = OP_MTX; a.bits = (uint32_t)strtoul(h.c_str(), nullptr, 16);
    } else if (cmd == "say") {
        a.op = OP_SAY; std::getline(ss, a.text);
    } else {
        a.op = OP_HOLD; a.bits = repl_combo_bits(cmd);
        if (!(ss >> a.ms)) a.ms = 150;
        if (!a.bits) { printf("[repl] unknown: %s\n", raw.c_str()); fflush(stdout); return; }
    }
    std::lock_guard<std::mutex> lk(g_repl_mtx);
    g_repl_q.push_back(std::move(a));
}

// Reader thread: pull lines from the control source. A FIFO path is reopened on
// EOF so repeated `echo cmd > fifo` writers work (classic named-pipe pattern).
static void repl_reader(std::string src) {
    const bool is_fifo = (src != "-" && src != "1" && src != "stdin");
    if (is_fifo) {
        unlink(src.c_str());
        if (mkfifo(src.c_str(), 0666) != 0 && errno != EEXIST)
            fprintf(stderr, "[repl] mkfifo %s failed: %s\n", src.c_str(), strerror(errno));
        fprintf(stderr, "[repl] listening on FIFO %s\n", src.c_str());
    } else {
        fprintf(stderr, "[repl] listening on stdin\n");
    }
    while (g_running.load()) {
        FILE* in = is_fifo ? fopen(src.c_str(), "r") : stdin;
        if (!in) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
        char buf[512];
        while (g_running.load() && fgets(buf, sizeof buf, in)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            repl_parse_line(s);
        }
        if (is_fifo) fclose(in); else break;   // stdin EOF → stop reading
    }
}

// Called each main-loop iteration when REPL is active. Returns the pad bits to
// drive this frame (0 when idle). Runs one timed action at a time.
static uint32_t repl_tick() {
    static bool have_cur = false;
    static ReplAct cur{};
    static uint32_t end_ms = 0;
    const uint32_t now = SDL_GetTicks();

    if (have_cur) {
        if (cur.op == OP_HOLD) {
            if (now < end_ms) return cur.bits;     // still holding
        }
        have_cur = false;                          // action done
    }
    // Pop the next action.
    {
        std::lock_guard<std::mutex> lk(g_repl_mtx);
        if (g_repl_q.empty()) return 0;
        cur = std::move(g_repl_q.front());
        g_repl_q.pop_front();
    }
    switch (cur.op) {
    case OP_HOLD:
        have_cur = true; end_ms = now + cur.ms;
        return cur.bits;
    case OP_SNAP: findmario_step(); return 0;
    case OP_QUIT: g_running = false; return 0;
    case OP_SAY:  printf("[repl]%s\n", cur.text.c_str()); fflush(stdout); return 0;
    case OP_MTX: {
        u8* ram = Core::System::GetInstance().GetMemory().GetPointerForRange(cur.bits, 48);
        if (ram)
            printf("[repl] mtx %08x  pos=(%.2f, %.2f, %.2f)\n", cur.bits,
                   be_f32(ram + 12), be_f32(ram + 28), be_f32(ram + 44));
        else
            printf("[repl] mtx %08x  (unmapped)\n", cur.bits);
        fflush(stdout);
        return 0;
    }
    }
    return 0;
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
    // SUNBRIGHT_NTHR_SELFTEST: validate the native-threading CPU-token hand-off in
    // isolation, then exit — no Dolphin/SDL/game involved.
    if (getenv("SUNBRIGHT_NTHR_SELFTEST"))
        return nthr::self_test() ? 0 : 1;

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

    // SUNBRIGHT_HEADLESS=1: no window, Null video backend, muted audio — for fast,
    // unattended repro/CI. Implies turbo (unthrottled emulation). SUNBRIGHT_TURBO=1
    // unthrottles emulation speed without going headless (keeps the window).
    const bool headless = getenv("SUNBRIGHT_HEADLESS") != nullptr;
    const bool turbo    = headless || getenv("SUNBRIGHT_TURBO") != nullptr;

    // SDL — headless still needs the event/timer subsystem for SDL_GetTicks (autostart
    // timing) but no video device (which would require a display).
    if (SDL_Init(headless ? (SDL_INIT_TIMER | SDL_INIT_EVENTS) : SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!headless) {
        // Size the window to the output aspect so the game fills it (no letterbox bars).
        // 16:9 by default (matches the widescreen output), 4:3 if widescreen is off.
        // Dolphin scales the image to the window while preserving aspect; F11 toggles
        // fullscreen at runtime.
        const char* w = getenv("SUNBRIGHT_WIDESCREEN");
        const bool wide = !w || atoi(w) != 0;
        const int win_w = wide ? 1280 : 960, win_h = 720;
        g_window = SDL_CreateWindow(
            "Sunbright — Super Mario Sunshine",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
            SDL_WINDOW_RESIZABLE | SDL_WINDOW_SHOWN);
        if (!g_window) {
            fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            SDL_Quit();
            return 1;
        }
    } else {
        fprintf(stderr, "[sunbright] HEADLESS: Null video backend, no window\n");
    }
    g_focused = true;  // input override bypasses the focus gate; headless has no window

    // Load recompiled native functions into the JIT hook table
    if (!SunbrightBridge::Init(recomp_lib)) {
        fprintf(stderr,
                "[sunbright] Warning: could not load %s — game runs entirely via Dolphin JIT\n",
                recomp_lib);
    }

    WindowSystemInfo wsi;
    if (headless) wsi.type = WindowSystemType::Headless;
    else          wsi = build_wsi();
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
    } else if (headless) {
        Config::SetBase(Config::MAIN_GFX_BACKEND, std::string("Null"));
        fprintf(stderr, "[sunbright] Using backend: Null (headless)\n");
    }

    // Turbo: unthrottle the emulation speed (0 = run as fast as the host allows) and
    // drop vsync so nothing paces the CPU/GPU threads to real time. The intermittent
    // corruption reproduces far faster this way.
    if (turbo) {
        Config::SetBase(Config::MAIN_EMULATION_SPEED, 0.0f);
        Config::SetBase(Config::GFX_VSYNC, false);
        fprintf(stderr, "[sunbright] TURBO: emulation speed unthrottled, vsync off\n");
    }
    // Headless: no audio device (avoids needing a sound server) and muted.
    if (headless) {
        Config::SetBase(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_NULLSOUND));
        Config::SetBase(Config::MAIN_AUDIO_MUTED, true);
    }

    // Dual-core: run the GPU/FIFO on its own thread. In single-core mode Dolphin processes the GP
    // FIFO on the CPU thread (Fifo::RunGpuOnCpu) inline with the recomp — profiling showed ~all
    // CPU-thread time there, serializing rendering with game logic. A separate GPU thread
    // parallelizes it. The recomp runs on the CPU thread (JitTrampoline hook), independent of the
    // GPU thread, which reads the FIFO the gather pipe bursts to.
    Config::SetBase(Config::MAIN_CPU_THREAD, true);

    // ── Graphics quality ────────────────────────────────────────────────────
    // Internal resolution scale (SUNBRIGHT_RES_SCALE, default 3 = 3× native EFB).
    {
        const char* rs = getenv("SUNBRIGHT_RES_SCALE");
        int scale = rs ? atoi(rs) : 3;
        if (scale < 1) scale = 1;
        Config::SetBase(Config::GFX_EFB_SCALE, scale);
        fprintf(stderr, "[sunbright] Internal resolution scale: %d×\n", scale);
    }
    // Widescreen — handled NATIVELY in the port (runtime/overrides/scene_render.cpp).
    // We widen the game's own 3D projection aspect at its source (the JDrama camera's
    // JSGSetProjectionAspect) to 16:9, and let Dolphin's per-frame aspect auto-detection
    // (AspectMode::Auto) present each frame at its real aspect: the widened 3D → 16:9,
    // and the untouched 2D ortho (HUD/title) → 4:3, so 2D stays centered in its 4:3
    // composition instead of being stretched. NO ForceWide (which blindly stretches the
    // whole 4:3 EFB, 2D included) and NO .data constant patch (that was a reference hack).
    Config::SetBase(Config::GFX_ASPECT_RATIO, AspectMode::Auto);
    Config::SetBase(Config::GFX_WIDESCREEN_HACK, false);
    {
        const char* w = getenv("SUNBRIGHT_WIDESCREEN");
        const bool wide = !w || atoi(w) != 0;
        fprintf(stderr, "[sunbright] Widescreen: %s (Auto aspect, native projection)\n",
                wide ? "16:9 native" : "off (4:3)");
    }

    // Ensure a Standard Controller is on port 0 so the game polls pad input.
    Config::SetBase(Config::GetInfoForSIDevice(0), SerialInterface::SIDEVICE_GC_CONTROLLER);

    // SUNBRIGHT_DUMP=1: dump every presented frame as a PNG to the user Dump/Frames
    // dir. Definitive proof of what's rendered, independent of window capture (which
    // is unreliable under XWayland).
    // ALWAYS set this explicitly (not just when enabling): Dolphin PERSISTS these flags to
    // <home>/.config/dolphin-emu/{Dolphin,GFX}.ini, so a single past SUNBRIGHT_DUMP run leaves
    // DumpFrames=True on disk and EVERY later run silently re-enables the FrameDumper — which
    // PNG-encodes every frame on its own thread at ~95% CPU and throttles the whole emulator to
    // ~0.15× real-time. (This was the "game is too slow" report: stale persisted dump config,
    // not the recomp.) Force it off unless explicitly requested this run.
    const bool want_dump = getenv("SUNBRIGHT_DUMP") != nullptr;
    Config::SetBase(Config::MAIN_MOVIE_DUMP_FRAMES, want_dump);
    Config::SetBase(Config::GFX_DUMP_FRAMES_AS_IMAGES, want_dump);
    if (want_dump) fprintf(stderr, "[sunbright] Frame dumping enabled\n");

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
        g_core_running.store(s == Core::State::Running, std::memory_order_release);
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

    // SUNBRIGHT_REPL: start the live control-channel reader (FIFO path, or "-"/stdin).
    static const char* repl_src = getenv("SUNBRIGHT_REPL");
    std::thread repl_thread;
    if (repl_src && *repl_src) {
        repl_thread = std::thread(repl_reader, std::string(repl_src));
        repl_thread.detach();
    }

    // SUNBRIGHT_PROBE=1: bring up the HTTP/JSON perf probe (http://127.0.0.1:17654/metrics).
    // Lets a headless run be measured live (emulation speed, recomp-vs-interp call mix).
    probe_server_start();

    // SUNBRIGHT_RUN_SECONDS=N: auto-exit after N seconds of wall time (CI / repro).
    const char* run_secs_env = getenv("SUNBRIGHT_RUN_SECONDS");
    const uint32_t run_ms = run_secs_env ? (uint32_t)(atof(run_secs_env) * 1000.0) : 0;

    // Main event loop — Dolphin runs its CPU/GPU on its own threads
    while (g_running) {
        if (run_ms && SDL_GetTicks() >= run_ms) {
            fprintf(stderr, "[sunbright] RUN_SECONDS elapsed — exiting\n");
            g_running = false;
            break;
        }
        // SUNBRIGHT_DBG_SAMPLE: periodic guest-PC sampler — for a silent hang, the cluster of
        // sampled PCs shows the spin loop. Env-gated, kept (see memory keep-diagnostics).
        static const bool dbg_sample = getenv("SUNBRIGHT_DBG_SAMPLE") != nullptr;
        if (dbg_sample && guest_mem_ready()) {
            static uint32_t s_last = 0;
            uint32_t s_now = SDL_GetTicks();
            if (s_now - s_last >= 100) {
                s_last = s_now;
                auto& ppc = Core::System::GetInstance().GetPPCState();
                fprintf(stderr, "[sample] pc=%08x lr=%08x ctr=%08x r3=%08x\n",
                        ppc.pc, ppc.spr[SPR_LR], ppc.spr[SPR_CTR], ppc.gpr[3]);
            }
        }
        // Once the video backend's presenter exists, sync the swapchain to the
        // current window size once — the window may have been sized by the WM after
        // the swapchain was first created at boot (otherwise: black band).
        static bool surf_synced = false;
        if (!surf_synced && g_presenter) { g_presenter->ResizeSurface(); surf_synced = true; }

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
                // The window's pixel size changed (user/WM resize, fullscreen toggle,
                // or the WM sizing it at map time). Tell Dolphin's presenter to
                // recreate the swapchain at the new surface size — otherwise it keeps
                // rendering at the old (smaller) size anchored top-left, leaving a
                // black band. This is what DolphinQt's RenderWidget does on resize.
                else if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                         ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    if (g_presenter) g_presenter->ResizeSurface();
                }
                break;
            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_F11)
                    SDL_SetWindowFullscreen(g_window,
                        Host_RendererIsFullscreen() ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                else if (ev.key.keysym.sym == SDLK_F5 && getenv("SUNBRIGHT_FINDMARIO"))
                    findmario_step();   // cheat-search: snapshot on your cue
                else if (uint32_t b = key_to_padbit(ev.key.keysym.sym)) {
                    g_pad.fetch_or(b, std::memory_order_relaxed);
                    static const bool ilog = getenv("SUNBRIGHT_MOVIESKIP_LOG") != nullptr;
                    if (ilog) fprintf(stderr, "[input] keydown sym=0x%x -> padbit 0x%x (focused=%d)\n",
                                      ev.key.keysym.sym, b, (int)g_focused.load());
                }
                break;
            case SDL_KEYUP:
                if (uint32_t b = key_to_padbit(ev.key.keysym.sym))
                    g_pad.fetch_and(~b, std::memory_order_relaxed);
                break;
            default:
                break;
            }
        }

        // SUNBRIGHT_REPL: apply the current scripted action's pad bits this frame.
        if (repl_src && *repl_src)
            g_repl_bits.store(repl_tick(), std::memory_order_relaxed);

        // Start during a THP movie → skip it (see movie_skip_tick).
        movie_skip_tick(g_pad.load(std::memory_order_relaxed)
                        | g_repl_bits.load(std::memory_order_relaxed));

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
            if (t < 60000) {
                // Drive through the menus: Start (title + skip THP intro movie), A (select a file).
                if      ((t % 1000) < 200) bits = P_START;
                else if ((t % 1000) < 400) bits = P_A;
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

        // SUNBRIGHT_WATCHMTX=<hexaddr>: read the 3x4 matrix at <addr> each ~frame,
        // keep prev/cur, and print the N64Recomp-style midpoint (lerp translation,
        // average rotation) — the interpolation proof on a real game transform.
        static const char* wm = getenv("SUNBRIGHT_WATCHMTX");
        if (wm && guest_mem_ready()) {
            static const u32 maddr = (u32)strtoul(wm, nullptr, 16);
            static uint32_t lastlog = 0;
            const uint32_t t = SDL_GetTicks();
            if (t - lastlog > 250) {
                lastlog = t;
                u8* ram = Core::System::GetInstance().GetMemory().GetPointerForRange(maddr, 48);
                if (ram) {
                    static float prev[12]; static bool have = false;
                    float cur[12];
                    for (int i = 0; i < 12; i++) cur[i] = be_f32(ram + i*4);
                    // translation = column 3 of each row (indices 3,7,11)
                    if (have) {
                        float dx = cur[3]-prev[3], dy = cur[7]-prev[7], dz = cur[11]-prev[11];
                        if (std::abs(dx)+std::abs(dy)+std::abs(dz) > 0.3f) {
                            float mx=(prev[3]+cur[3])*0.5f, my=(prev[7]+cur[7])*0.5f, mz=(prev[11]+cur[11])*0.5f;
                            printf("[interp] prev=(%.1f,%.1f,%.1f) cur=(%.1f,%.1f,%.1f) MID=(%.1f,%.1f,%.1f)\n",
                                   prev[3],prev[7],prev[11], cur[3],cur[7],cur[11], mx,my,mz);
                            fflush(stdout);
                        }
                    }
                    std::memcpy(prev, cur, sizeof cur); have = true;
                }
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
