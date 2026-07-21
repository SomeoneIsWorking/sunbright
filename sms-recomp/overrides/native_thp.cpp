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

namespace {

// THPPlayerOpen(const char* path) -> BOOL. 0 means the movie could not be opened.
void thp_player_open(CPUState& cpu) {
    static bool warned = false;
    if (!warned) {
        warned = true;
        lucent::warn("thp", "THP video is not decoded in this runtime — reporting movies as "
                            "unopenable so the game takes its own movie-setup-failure path. "
                            "Attract movies and cutscenes will not play.");
    }
    cpu.gpr[3] = 0;
}

} // namespace

SB_OVERRIDE(0x8001f6fcu, thp_player_open, "THPPlayerOpen",
            "no video decoding; the game's own failure path skips the movie")
