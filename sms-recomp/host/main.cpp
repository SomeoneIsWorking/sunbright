// main.cpp — host entry for the standalone recomp runtime.
//
// Loads the DOL into guest RAM and starts executing recompiled code at the DOL
// entry point. This is deliberately the smallest thing that can EXECUTE: no
// hardware routing yet, so it will run until it touches a device. Where it stops is
// the information we are after — it tells us which HW seam to route to aurora next.

#include "cpu_state.h"
#include "boot_env.h"
#include "guest_sched.h"
#include "intrinsics.h"
#include "../frame_interp/stream_interp.h"
#include "app/frame_rate.h"
#include "app/settings.h"
#include "ui/ui.h"

#include <aurora/aurora.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

#include <lucent/config.h>
#include <lucent/log.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" bool rt_mem_init();
bool disc_open(const char* path);
CPUState* g_cpu = nullptr;
void call_ppc(CPUState& cpu, u32 address);

namespace {

// DOL: 7 text + 11 data sections. Header is big-endian: offsets[18], addrs[18],
// sizes[18], bss addr/size, then the entry point at 0xE0.
struct Dol {
    u32 entry = 0;
    struct Sec { u32 off, addr, size; };
    std::vector<Sec> sections;
    u32 bss_addr = 0, bss_size = 0;
};

u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

bool load_dol(const std::string& path, Dol& out, std::vector<u8>& bytes) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { lucent::error("dol", "cannot open {}", path); return false; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    bytes.resize((size_t)n);
    size_t got = std::fread(bytes.data(), 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n || n < 0x100) { lucent::error("dol", "short read on {}", path); return false; }

    const u8* h = bytes.data();
    for (int i = 0; i < 18; i++) {
        u32 off = be32(h + 0x00 + i * 4);
        u32 addr = be32(h + 0x48 + i * 4);
        u32 size = be32(h + 0x90 + i * 4);
        if (off && size) out.sections.push_back({off, addr, size});
    }
    out.bss_addr = be32(h + 0xD8);
    out.bss_size = be32(h + 0xDC);
    out.entry    = be32(h + 0xE0);
    return true;
}

// Copy a guest range in. Writes go through the raw host pointer (not sb_w*) because
// the DOL image is already big-endian — byteswapping here would corrupt it.
void guest_write(u32 addr, const u8* src, u32 size) {
    for (u32 i = 0; i < size; i++)
        if (u8* p = sb_ram_fast(addr + i)) *p = src[i];
}

std::string user_path() {
    char* raw = SDL_GetPrefPath(nullptr, "sunbright-recomp");
    if (raw == nullptr) {
        lucent::error("settings", "SDL_GetPrefPath failed: {}", SDL_GetError());
        return {};
    }
    std::string path(raw);
    SDL_free(raw);
    return path;
}

std::string resource_path(const char* argv0) {
    std::error_code error;
    const auto executable = std::filesystem::absolute(argv0 != nullptr ? argv0 : "", error);
    if (!error) {
        const auto besideExecutable = executable.parent_path();
        if (std::filesystem::is_directory(besideExecutable / "res"))
            return besideExecutable.string() + '/';
        const auto sourceRoot = besideExecutable.parent_path();
        if (std::filesystem::is_directory(sourceRoot / "res")) return sourceRoot.string() + '/';
    }
    const auto working = std::filesystem::current_path(error);
    if (!error && std::filesystem::is_directory(working / "res")) return working.string() + '/';
    lucent::error("ui", "cannot locate the res/ directory from executable '{}' or cwd", argv0);
    return {};
}

} // namespace

// The interpolation pairing self-test lives in aurora and is pure — local buffers and the module's
// own tables — but it only ever ran from inside interpolate_recorded_frame, i.e. only during a real
// game frame with 60fps on. That put a CPU-only proof of the pairing maths behind a GPU run, which
// is the wrong dependency in both directions: it cannot be checked when the GPU is unavailable, and
// nothing in the test suite covers it. This entry point reaches it directly.
namespace aurora::gfx::interp { bool selftest(); }

int main(int argc, char** argv) {
    lucent::config::set_prefix("SBR_");   // SBR_LUCENT_DEBUG=mmio,rt,poll

    // SBR_INTERP_SELFTEST=1: run the interpolation pairing self-test and exit. No aurora, no
    // window, no GPU, no ROM — so it is runnable in a session where GPU work is not permitted, and
    // by ctest. Exits non-zero on failure so a harness can gate on it.
    if (const char* e = std::getenv("SBR_INTERP_SELFTEST"); e != nullptr && e[0] == '1') {
        const bool ok = aurora::gfx::interp::selftest();
        lucent::info("selftest", "interpolation pairing self-test: {}",
                     ok ? "PASSED" : "FAILED — see the lines above for which case");
        return ok ? 0 : 1;
    }

    std::string dol_path = argc > 1 ? argv[1] : "scratch/bin/sms.dol";
    const std::string userPath = user_path();
    const std::string resourcesPath = resource_path(argc > 0 ? argv[0] : nullptr);
    if (userPath.empty() || resourcesPath.empty() ||
        !sb::app::settings().load(std::filesystem::path(userPath) / "sunbright.ini"))
        return 1;

    // Aurora provides the GX implementation. mem1Size/mem2Size are 0: this runtime owns its
    // guest memory (rt_mem_init), and aurora is handed real host pointers for anything it
    // needs to read out of it.
    AuroraConfig acfg = {};
    acfg.appName        = "sunbright-recomp";
    acfg.userPath       = userPath.c_str();
    acfg.resourcesPath  = resourcesPath.c_str();
    acfg.desiredBackend = BACKEND_VULKAN;
    acfg.msaa           = 1;
    // PRESENT MODE, and why interpolated 60fps REQUIRES vsync on.
    //
    // aurora maps vsync=false to Mailbox (or Immediate). Mailbox's defining behaviour is that the
    // swapchain holds ONE pending image and a newer present REPLACES it — the older one is
    // discarded, never scanned out. That is the correct trade for a single-image-per-tick renderer
    // chasing lowest latency, and it is fatal for interpolation: a tick emits two images, and if
    // they are issued inside one display refresh — which is most ticks, measured at 94% on this
    // machine — Mailbox throws the in-between away BY DESIGN. Every counter still reads 60 fps
    // because both were presented; the display simply never saw the first.
    //
    // vsync=true maps to FifoRelaxed (else Fifo), where every presented image is QUEUED and shown
    // for at least one refresh. Nothing is discarded, and the display's own refresh does the
    // spacing that the frame loop was trying and failing to do with a sleep. This is the fix for
    // "it drops the interpolated frames"; the pacing work that preceded it was tuning a policy
    // whose output was being thrown away downstream.
    //
    // Off (Mailbox) when interpolation is off, because then there IS only one image per tick and
    // the latency trade goes the other way.
    // The prelaunch menu can change frame mode after the device is initialized. Strict FIFO works
    // for every mode and is REQUIRED for interpolation, so initialize once with the invariant
    // policy instead of making the menu and swapchain disagree until a restart.
    acfg.vsync          = true;
    acfg.windowWidth    = std::getenv("SB_W") ? (u32)std::strtoul(std::getenv("SB_W"), nullptr, 0) : 1280u;
    acfg.windowHeight   = std::getenv("SB_H") ? (u32)std::strtoul(std::getenv("SB_H"), nullptr, 0) : 960u;
    acfg.mem1Size       = 0;
    acfg.mem2Size       = 0;
    AuroraInfo ainfo = aurora_initialize(argc, argv, &acfg);
    lucent::info("rt", "aurora up: backend={} fb={}x{}", (int)ainfo.backend,
                 ainfo.windowSize.fb_width, ainfo.windowSize.fb_height);
    if (const char* value = std::getenv("SBR_UI_SELFTEST"); value != nullptr && value[0] != '0') {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        const unsigned frames = end != value && *end == '\0' && parsed > 0
                                    ? static_cast<unsigned>(parsed)
                                    : 2u;
        const bool ok = sb::ui::render_settings_control(frames);
        aurora_shutdown();
        return ok ? 0 : 1;
    }
    if (!sb::ui::run_prelaunch()) {
        aurora_shutdown();
        return 0;
    }
    if (!sb::app::frame_rate::is_supported(sb::app::settings().effective().frameRate)) {
        lucent::error("settings", "cannot launch with {}: {}",
                      sb::app::display_name(sb::app::settings().effective().frameRate),
                      sb::app::frame_rate::unsupported_reason(
                          sb::app::settings().effective().frameRate));
        aurora_shutdown();
        return 1;
    }
    lucent::info("settings", "renderer={} framerate={}",
                 sb::app::display_name(sb::app::settings().effective().renderer),
                 sb::app::display_name(sb::app::settings().effective().frameRate));
    // Arm interpolated 60fps BEFORE the first frame is recorded. sbr_lerp_enabled() configures
    // aurora on its first call, and leaving that to whichever seam happened to ask first is how a
    // mode ends up half-on for the opening frames.
    sbr_lerp_enabled();
    aurora_begin_frame();

    if (!rt_mem_init()) return 1;

    // Mount the disc the DI device serves. Same convention as the decomp runtime:
    // $SUNBRIGHT_ROM, else a rom.rvz drop-in beside the binary. Without it the game gets
    // no filesystem, so this is fatal rather than a warning.
    const char* rom = std::getenv("SUNBRIGHT_ROM");
    if (!rom || !*rom) rom = "rom.rvz";
    if (!disc_open(rom)) {
        lucent::error("main", "no disc mounted — set SUNBRIGHT_ROM or drop rom.rvz");
        return 1;
    }

    // Before anything can fault: a fatal signal must name itself. See rt_core.cpp.
    extern void rt_install_crash_handler();
    rt_install_crash_handler();

    Dol dol;
    std::vector<u8> bytes;
    if (!load_dol(dol_path, dol, bytes)) return 1;

    lucent::info("dol", "{}: {} sections, entry 0x{:08x}", dol_path, dol.sections.size(), dol.entry);

    // BSS IS CLEARED FIRST, THEN SECTIONS ARE LOADED OVER IT. The order matters: this DOL's
    // declared BSS range (0x803e9700 +0x25498) physically CONTAINS a loaded data section
    // (0x8040c1c0 +0xd40, the small-data area). Clearing BSS afterwards would erase real
    // initialised data — it erased __GXData, whose slot at 0x8040cec8 ships the pointer
    // 0x804036a0, leaving GX to dereference NULL far away in the boot. Loaded data must win.
    if (dol.bss_size) {
        for (u32 i = 0; i < dol.bss_size; i++)
            if (u8* p = sb_ram_fast(dol.bss_addr + i)) *p = 0;
        lucent::debug("dol", "bss cleared 0x{:08x} +0x{:x}", dol.bss_addr, dol.bss_size);
    }
    for (const auto& s : dol.sections) {
        guest_write(s.addr, bytes.data() + s.off, s.size);
        lucent::debug("dol", "section -> 0x{:08x} +0x{:x}", s.addr, s.size);
    }

    // Devices that must deliver an interrupt into guest code (DI completion) need the CPU
    // state; there is exactly one, so expose it rather than threading it through the MMIO
    // router, which is otherwise purely address-based.
    static CPUState cpu{};
    g_cpu = &cpu;
    // The GC boots with a stack near the top of MEM1; __start sets up its own, but a
    // sane initial r1 keeps any early prologue from writing through address 0.
    cpu.gpr[1] = 0x816FFFF0u;
    cpu.pc     = dol.entry;

    // After the DOL is in memory: the apploader's low-memory state must not be clobbered
    // by section loading, and the FST lives above the DOL's sections.
    u32 arena_lo = dol.bss_addr + dol.bss_size;
    for (const auto& s : dol.sections)
        if (s.addr + s.size > arena_lo) arena_lo = s.addr + s.size;
    if (!boot_env_setup(arena_lo)) return 1;

    // Adopt this host thread as guest thread 0 before any guest code runs, so the
    // scheduler owns threading from the first instruction.
    gsched_init(cpu, sb_r32(0x800000E4u));

    // Before the first guest instruction: prove the arena guard can fire (SBR_ARENA_SELFTEST=1).
    // It aborts on success — see overrides/guard_arena.cpp.
    extern void sbr_arena_guard_selftest();
    sbr_arena_guard_selftest();

    lucent::info("rt", "entering recompiled code at 0x{:08x}", dol.entry);
    // Run on the scheduler's copy, not the local one: gsched_create seeds new threads with
    // the creating thread's r2/r13 (the small-data bases), so thread 0's registers have to
    // be the ones the scheduler can see, not a stale snapshot taken before boot.
    call_ppc(gsched_cpu(), dol.entry);
    lucent::info("rt", "returned from entry (lr=0x{:08x})", gsched_cpu().lr);
    return 0;
}
