// aurora_bridge.cpp — Path A entry point.
//
// sms-boot's GC-faithful render path. The reference/sms decomp emits its
// original GameCube GX SDK calls (GXSetChanCtrl, GXSetProjection, GXBegin,
// ...) and Aurora's <dolphin/gx.h> implementation drives WebGPU/Dawn under
// them. No emulation-of-Dolphin translation layer, no FIFO byte replay — the
// game's own SDK calls are the interface.
//
// Include layering (per-TU, controlled by CMake include-dir order):
//   Path A (this target): extern/aurora/include -> reference/sms/include.
//     Aurora's <dolphin/types.h> wins → SMS_NATIVE_PLATFORM is NOT defined →
//     reference/sms takes the original GC paths through Aurora.
//   Path B (sms-boot target): sms-boot/shim -> reference/sms/include.
//     Our shim's <dolphin/types.h> wins → SMS_NATIVE_PLATFORM=1 → reference/sms
//     takes the native-C++ branches routing into sms-boot/render_pc/.
//
// This file is Path A ONLY. Aurora's <aurora/main.h> `#define main aurora_main`
// takes our int main() over as the app entry called from libaurora_core's real
// main(). We initialize Aurora then hand control to the game thread.

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>

#include <System/Application.hpp>
#include <dolphin/os.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <execinfo.h>
#include <unistd.h>

extern TApplication gpApplication;

// TODO: the real game entry, hooked from reference/sms/src/main.cpp. For MVP
// bring-up we just clear to a distinctive colour every frame so we can prove
// the Aurora pipeline is alive end-to-end before wiring in the game thread.
static void draw_bringup_frame() {
    GXSetCopyClear(
        (GXColor){
            .r = 32,
            .g = 96,
            .b = 200,
            .a = 255,
        },
        GX_MAX_Z24);
}

// ---- Watchdog: SIGALRM handler dumps backtrace and aborts if the boot loop
// stalls. Kick sb_watchdog_kick() from progress checkpoints to reset the timer.
// SB_WATCHDOG_SECS overrides the default (5 s).
static constexpr unsigned kWatchdogDefaultSecs = 5;
static void sb_watchdog_handler(int) {
    const char* msg = "\n=== WATCHDOG: no progress within timeout, aborting ===\n";
    write(2, msg, std::strlen(msg));
    void* frames[64];
    int n = backtrace(frames, 64);
    backtrace_symbols_fd(frames, n, 2);
    write(2, "\n", 1);
    _exit(134);
}
extern "C" void sb_watchdog_kick(void) {
    const char* env = std::getenv("SB_WATCHDOG_SECS");
    unsigned secs = env ? (unsigned)std::atoi(env) : kWatchdogDefaultSecs;
    if (secs == 0) secs = kWatchdogDefaultSecs;
    alarm(secs);
}
static void sb_watchdog_install(void) {
    struct sigaction sa{};
    sa.sa_handler = sb_watchdog_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, nullptr);
    sb_watchdog_kick();
}

static void log_callback(AuroraLogLevel level, const char* module,
                         const char* message, unsigned int) {
    const char* tag = "?";
    FILE* out = stdout;
    switch (level) {
        case LOG_DEBUG:   tag = "DEBUG";   break;
        case LOG_INFO:    tag = "INFO";    break;
        case LOG_WARNING: tag = "WARN";    break;
        case LOG_ERROR:   tag = "ERROR";   out = stderr; break;
        case LOG_FATAL:   tag = "FATAL";   out = stderr; break;
    }
    std::fprintf(out, "[aurora %s %s] %s\n", tag, module, message);
    if (level == LOG_FATAL) { std::fflush(out); std::abort(); }
}

int main(int argc, char* argv[]) {
    AuroraConfig config = {};
    config.appName        = "Sunbright (Path A, GC-faithful)";
    config.desiredBackend = BACKEND_VULKAN;
    config.logCallback    = &log_callback;
    config.logLevel       = LOG_INFO;
    config.msaa           = 1;
    config.vsync          = true;
    config.mem1Size       = MEM1_DEFAULT_SIZE;
    config.mem2Size       = ARAM_DEFAULT_SIZE;

    AuroraInfo info = aurora_initialize(argc, argv, &config);
    std::fprintf(stdout, "[sms-boot] aurora up: backend=%d fb=%ux%u\n",
                 (int)info.backend, info.windowSize.fb_width, info.windowSize.fb_height);
    std::fflush(stdout);

    // OSInit() sets up the emulated MEM1 arena Aurora simulates. On the real
    // console it's called by system firmware before main; games (and reference/sms)
    // don't call it themselves, so do it here before any JKR heap allocs.
    OSInit();
    sb_watchdog_install();

    // Hand off to the game. TApplication::initialize is the decomp's own
    // GC entry; under SMS_NATIVE_PLATFORM=1 its GC-side threading/CD paths
    // short-circuit to sync/single-thread. proc() drives the frame loop
    // (which must also pump aurora_update / begin_frame / end_frame — that
    // integration comes next).
    gpApplication.initialize();
    gpApplication.proc();
    gpApplication.finalize();

    aurora_shutdown();
    return 0;
}
