// boot.cpp — the native PC bring-up entry. The game's own int main() (reference/sms/
// src/main.cpp: gpApplication.initialize/proc/finalize) is THE program main; this wraps
// it (linker --wrap=main) so the platform layer comes up first — the PC replacement for
// the GameCube __start that ran PlatformInit-equivalent before main.
//
// Links sms-native (the game logic) + sms-platform (the SDK seams). Until the remaining
// seams + game C++ stubs resolve this WON'T link; the undefined-reference list is the
// true critical-path stub set to fill scene-by-scene (handoff step 3/boot).
#include <cstdio>

namespace sb::platform { bool PlatformInit(int argc, char** argv); void PlatformShutdown(); }

extern "C" int __real_main(int argc, char** argv);
extern "C" void sb_watchdog_init(void);  // stuck-process watchdog (watchdog_impl.cpp)

#ifdef SMS_HAVE_RENDER
// native/render/sms_boot_present.cpp — installs the VI present hook (nvk render + frame dump).
extern "C" void sb_boot_present_install();
#endif

extern "C" int __wrap_main(int argc, char** argv) {
    std::printf("[boot] native SMS bring-up: PlatformInit...\n");
    if (!sb::platform::PlatformInit(argc, argv)) {
        std::fprintf(stderr, "[boot] PlatformInit failed (no disc? set SUNBRIGHT_ROM)\n");
        return 1;
    }
#ifdef SMS_HAVE_RENDER
    sb_boot_present_install();
#endif
    sb_watchdog_init();  // arm the stuck-process watchdog (SB_WATCHDOG_SECS, 0=off)
    std::printf("[boot] PlatformInit OK -> game main()\n");
    int rc = __real_main(argc, argv);
    sb::platform::PlatformShutdown();
    return rc;
}
