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

#include <cstdio>
#include <cstdlib>
#include <cstring>

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
    std::fprintf(stdout, "[sms-boot-gc] aurora up: backend=%d fb=%ux%u\n",
                 (int)info.backend, info.windowSize.fb_width, info.windowSize.fb_height);

    // TODO: spawn the game thread here (mirrors sms-boot/src/boot.cpp under
    // Path B). For MVP we run the SDL/present loop only.
    bool exiting = false;
    while (!exiting) {
        const AuroraEvent* event = aurora_update();
        while (event && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) exiting = true;
            ++event;
        }
        if (exiting) break;
        if (!aurora_begin_frame()) continue;
        draw_bringup_frame();
        aurora_end_frame();
    }

    aurora_shutdown();
    return 0;
}
