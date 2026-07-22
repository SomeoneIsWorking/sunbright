// native_thp.cpp — THP (the GameCube's video format) policy for this runtime.
//
// THP DECODING WORKS HERE. The recompiled codec is the game's own and, given a correctly
// modelled machine, returns success and produces real frames (verified 2026-07-22: the plaza's
// /data/ex128x144_q0.thp decodes with THPVideoDecode -> 0). It looked unportable for a long
// time only because the codec requires the locked cache and read HID2 as 0 — SPRs used to be
// per-CPUState, so LCEnable() on one thread was invisible to the decode thread. See
// CPUState::SprFile.
//
// What is NOT solved is running a SECOND THP session after a first one is torn down: reopening
// after THPPlayerClose/Cancel leaves a decode thread receiving from a null message queue
// (observed: null read at OSMessageQueue+0x1c from PopReadedBuffer). Until that is fixed the
// policy below decides which opens are allowed:
//
//   SBR_THP=stage   (default) only the stage-resident player opens. Delfino Plaza REQUIRES it:
//                   TMarDirector::loadResource ends with `if (mMap == 1) thpInit()`, and a
//                   failure there returns nonzero all the way up to TApplication::proc, which
//                   treats it as APP_STATE_DONE -> mNextArea = stage 15. That is exactly the
//                   "loading a save file sends me back to the title screen" bug.
//   SBR_THP=all     open everything, including the attract movie and cutscenes. Plays them,
//                   but breaks on the next session (see above).
//   SBR_THP=none    the old behaviour: decline every movie. Delfino Plaza cannot be entered.
//
// A declined movie is not a silent stub: the game HAS a movie-setup-failure path, and declining
// takes it, plus the movies are marked already-seen so it stops asking.

#include "overrides.h"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cstdlib>
#include <cstring>

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
extern "C" void func_8001f6fc(CPUState&);   // the real THPPlayerOpen

// Which opens are allowed. "stage" (default) keeps Delfino Plaza working without pretending
// the attract movie can play; see the header.
const char* thp_policy() {
    static const char* p = nullptr;
    if (!p) {
        const char* e = std::getenv("SBR_THP");
        p = (e && e[0]) ? e : "stage";
        if (std::strcmp(p, "stage") != 0 && std::strcmp(p, "all") != 0 &&
            std::strcmp(p, "none") != 0) {
            lucent::error("thp", "SBR_THP='{}' is not one of stage|all|none", p);
            std::abort();
        }
    }
    return p;
}

void thp_player_open(CPUState& cpu) {
    char path[64];
    u32 n = 0;
    for (; n + 1 < sizeof path; n++) {
        const u8 c = sb_r8(cpu.gpr[3] + n);
        path[n] = (char)c;
        if (c == 0) break;
    }
    path[n] = '\0';

    const char* policy = thp_policy();
    // The stage-resident player is the one TMarDirector::thpInit opens; every other open comes
    // from the movie director. Matching it by name is how the runtime tells them apart — the
    // two go through the same SDK entry point.
    const bool is_stage_video = std::strstr(path, "ex128x144") != nullptr;
    if (std::strcmp(policy, "all") == 0 || (std::strcmp(policy, "stage") == 0 && is_stage_video)) {
        func_8001f6fc(cpu);
        lucent::info("thp", "opened {} -> {}", path, cpu.gpr[3]);
        return;
    }

    static bool warned = false;
    if (!warned) {
        warned = true;
        lucent::warn("thp", "movies are declined under SBR_THP={} — the game takes its own "
                            "movie-setup-failure path, so attract movies and cutscenes do not "
                            "play. SBR_THP=all plays them (breaks on the second movie).",
                     thp_policy());
        mark_movies_seen(cpu);
    }
    cpu.gpr[3] = 0;
}

} // namespace

SB_OVERRIDE(0x8001f6fcu, thp_player_open, "THPPlayerOpen",
            "no video decoding; the game's own failure path skips the movie")

// ── Diagnostic: what does the real codec actually say? ───────────────────────────────────
// THPVideoDecode returns 0 on success and a specific code otherwise (THPDec.c): 28 =
// locked cache not enabled, 29 = THPInit never ran, 3 = bad syntax, 25/26/27 = null
// input/work/output. Delfino Plaza's load hard-depends on this succeeding, so when it fails
// the code is the whole diagnosis — guessing which branch fired would cost a session.
namespace {
extern "C" void func_8036b644(CPUState&);   // THPVideoDecode

void thp_video_decode_trace(CPUState& cpu) {
    const u32 file = cpu.gpr[3], work = cpu.gpr[7];
    func_8036b644(cpu);
    static long n = 0;
    ++n;
    if (cpu.gpr[3] != 0)
        lucent::error("thp", "THPVideoDecode(file=0x{:08x} work=0x{:08x}) FAILED with {} on call "
                             "{} (HID2=0x{:08x}). 28 = locked cache disabled, 29 = THPInit never "
                             "ran, 3 = bad syntax, 25/26/27 = null input/output/work.",
                      file, work, (s32)cpu.gpr[3], n, cpu.spr[920]);
    else
        lucent::debug("thp", "THPVideoDecode call {} ok", n);
}
} // namespace

SB_OVERRIDE(0x8036b644u, thp_video_decode_trace, "THPVideoDecode",
            "diagnostic only: reports the codec's own error code, always runs the real body")
