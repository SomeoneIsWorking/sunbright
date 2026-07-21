// native_thp.cpp — THP (the GameCube's video format) is not decoded here.
//
// SMS plays an attract movie (Entrance.thp, 112 MB) and a few in-game cutscenes through the
// THP player, which runs its own decode threads and DMAs YUV frames into the GX pipeline.
// None of that is ported: this runtime has no video decoding, exactly as it has no DSP.
//
// Left alone the player starts anyway and faults — its decode thread dereferences null
// because no frame state was ever produced (observed: NULL read at +0x00 inside the thread
// body at 0x800200d8).
//
// Rather than stub the decode path piecemeal, report that the movie cannot be OPENED. The
// game already has a path for that — movie setup failing is a case it handles, so this uses
// the game's own control flow instead of inventing one. The cost is that attract movies and
// cutscenes do not play, which is the same "absent by omission" state as audio.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

extern void call_ppc(CPUState& cpu, u32 address);

namespace {

// TFlagManager::setFlag(u32 flag, long value) and the singleton's SDA slot. Addresses from
// the retired fastboot override, which RE'd them against this DOL.
constexpr u32 FM_SET_FLAG   = 0x80294b1cu;
constexpr u32 FM_SDA_OFFSET = 0x6060u;      // TFlagManager::smInstance at r13 - 0x6060

// "Movie already seen" flags. checkAdditionalMovie() consults these, and the game skips a
// movie it thinks the player has watched. setBool(true, f) for these is exactly
// setFlag(f, 1) (FlagManager.cpp case 3, all below 0x3001D).
constexpr u32 kMovieSeen[] = {
    0x30009,   // airport opening
    0x3000B,   // plaza scenario-0 intro
    0x3000C,   // plaza scenario-1 intro
    0x3000D,   // shine gate
};

// Reporting movies as unopenable is not enough on its own: the game re-enters the MOVIE
// state, fails again, and oscillates GAMEPLAY <-> MOVIE forever (observed). Marking them
// seen is the game's OWN way of not playing a movie, so it stops asking.
void mark_movies_seen(CPUState& cpu) {
    const u32 fm = sb_r32(cpu.gpr[13] - FM_SDA_OFFSET);
    if (!fm) {
        lucent::warn("thp", "TFlagManager not constructed yet; movies not marked seen");
        return;
    }
    for (u32 flag : kMovieSeen) {
        const CPUState saved = cpu;
        cpu.gpr[3] = fm;
        cpu.gpr[4] = flag;
        cpu.gpr[5] = 1;
        call_ppc(cpu, FM_SET_FLAG);
        cpu = saved;
    }
    // VERIFY rather than assume: setFlag's address came from the retired override and was
    // never confirmed against this DOL. TFlagManager::setFlag case 3 writes
    // mGameBools[low >> 3] bit (low & 7), and mGameBools sits at +0xCC (after
    // mCardBools[119] and mCardInts[21]).
    constexpr u32 FM_GAME_BOOLS = 0xCC;
    for (u32 flag : kMovieSeen) {
        const u32 low = flag & 0xFFFF;
        const u8  b   = sb_r8(fm + FM_GAME_BOOLS + (low >> 3));
        const bool set = (b >> (low & 7)) & 1;
        if (!set) {
            lucent::error("thp", "setFlag(0x{:05x}) did not take — mGameBools[{}] = 0x{:02x}. "
                                 "The setFlag address (0x{:08x}) is wrong.",
                          flag, low >> 3, b, FM_SET_FLAG);
            std::abort();
        }
    }
    lucent::info("thp", "marked {} attract/cutscene movies as already seen (verified)",
                 sizeof(kMovieSeen) / sizeof(*kMovieSeen));
}

// THPPlayerOpen(const char* path) -> BOOL. 0 means the movie could not be opened.
void thp_player_open(CPUState& cpu) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        lucent::warn("thp", "THP video is not decoded in this runtime — reporting movies as "
                            "unopenable so the game takes its own movie-setup-failure path. "
                            "Attract movies and cutscenes will not play.");
        mark_movies_seen(cpu);
    }
    cpu.gpr[3] = 0;
}

} // namespace

SB_OVERRIDE(0x8001f6fcu, thp_player_open, "THPPlayerOpen",
            "no video decoding; the game's own failure path skips the movie")
