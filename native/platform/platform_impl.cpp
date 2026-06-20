// platform_impl.cpp — the native PC platform bring-up. Replaces GC __start -> OSInit
// -> subsystem inits, standing up the seams so the game's main() can run as a real PC
// program. NOT a GameCube emulator: each subsystem is native (host threads, host
// files, host clock); this file just sequences their init in dependency order and
// owns the game's memory arena + the PC-native disc reader.
//
// Boot order (native/platform/README.md): os(arena/clock) -> dvd(disc/FST) ->
// [card -> audio] -> vi(heartbeat) -> [gx] -> pad. card/audio/gx are not yet
// implemented; PlatformInit brings up what exists and is the integration point where
// the remaining seams wire in (VI present hook, PAD input feed) as they land.

#include "platform.h"
#include "dvd_disc.h"

#include <dolphin/os.h>
#include <dolphin/dvd.h>
#include <dolphin/vi.h>
#include <dolphin/pad.h>
#include <JSystem/JKernel/JKRExpHeap.hpp>

#include <cstdio>
#include <cstdlib>

namespace {

// The game's memory arena. On GC this was the 24 MB main RAM handed to the OS; here
// it's one native block the game's JKR heaps sub-allocate. 64 MB is comfortable for
// the host (SMS used ~24 MB + 16 MB ARAM).
constexpr size_t kArenaSize = 64u << 20;
void* g_arena = nullptr;

// PC-native disc backend: a plain GCM/ISO read through host stdio.
FILE* g_disc_fp = nullptr;

bool gcm_read(void* dst, u32 length, u32 offset, void* /*user*/) {
    if (!g_disc_fp) return false;
    if (std::fseek(g_disc_fp, (long)offset, SEEK_SET) != 0) return false;
    return std::fread(dst, 1, length, g_disc_fp) == length;
}

} // namespace

extern "C" bool sb_platform_open_gcm(const char* path) {
    if (g_disc_fp) { std::fclose(g_disc_fp); g_disc_fp = nullptr; }
    g_disc_fp = std::fopen(path, "rb");
    if (!g_disc_fp) return false;
    sb_dvd_set_disc_source(gcm_read, nullptr);
    return sb_dvd_init_from_disc();
}

namespace sb::platform {

bool PlatformInit(int argc, char** argv) {
    // --- os: arena + heap. The game creates its JKR heaps within this block. ---
    g_arena = std::malloc(kArenaSize);
    if (!g_arena) return false;
    void* lo = g_arena;
    void* hi = (char*)g_arena + kArenaSize;
    OSInitAlloc(lo, hi, 16);
    OSSetArenaLo(lo);
    OSSetArenaHi(hi);
    OSCreateHeap(lo, hi);

    // Create the JKR root heap NOW. The decomp's global operator new is JKRHeap::
    // alloc(sCurrentHeap), so EVERY C++ allocation (incl. std::vector in the platform
    // seams below, e.g. the DVD FST read) routes through it. The game would otherwise
    // not create the root heap until TApplication::initialize (long after PlatformInit),
    // leaving sCurrentHeap null -> operator new returns null -> SEGV. createRoot sets
    // sRootHeap/sCurrentHeap; the game's later createRoot(1,false) finds sRootHeap set
    // and returns this same root (a no-op). Args match Application.cpp:204.
    JKRExpHeap::createRoot(1, false);

    // --- dvd: open the disc image if one was given (argv[1] or $SUNBRIGHT_DISC /
    // $SUNBRIGHT_ROM). Plain GCM only here; an RVZ needs the decompressing reader.
    // Absence is non-fatal at bring-up (a test / tool may install its own source). ---
    const char* disc = (argc > 1) ? argv[1] : nullptr;
    if (!disc) disc = std::getenv("SUNBRIGHT_DISC");
    if (!disc) disc = std::getenv("SUNBRIGHT_ROM");
    if (disc) {
        if (!sb_platform_open_gcm(disc))
            std::fprintf(stderr, "[platform] could not open disc image: %s "
                                 "(plain GCM/ISO only)\n", disc);
    }
    DVDInit();

    // --- vi: the 60 Hz frame heartbeat. (gx/audio not yet up; the present hook is
    // wired by the GX/VI integration when it lands.) ---
    VIInit();

    // --- pad: host input (the integration layer feeds sb_pad_set_state). ---
    PADInit();

    return true;
}

void PlatformShutdown() {
    if (g_disc_fp) { std::fclose(g_disc_fp); g_disc_fp = nullptr; }
    if (g_arena) { std::free(g_arena); g_arena = nullptr; }
}

void PlatformPumpFrame() {
    // Host event pump (window/input) lands with the VI/GX integration. The frame
    // heartbeat itself is the game's VIWaitForRetrace call (vi_impl.cpp), which paces
    // and invokes the present hook.
}

} // namespace sb::platform
