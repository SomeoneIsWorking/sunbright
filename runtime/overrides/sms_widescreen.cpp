// Heat-haze / heat-distortion removal.
//
// TShimmer::perform (USA 0x8019f83c) draws the full-screen heat-distortion. The
// known SMS Gecko code "0419f83c 4e800020" disables it by writing `blr` to its
// entry — confirming this is the heatwave function. We do the equivalent in a
// PC-port way: override it to return immediately. (It glitches badly under the
// 16:9 patch and is an unwanted effect anyway.) SUNBRIGHT_KEEP_SHIMMER=1 keeps it.
//
// The actual 16:9 widescreen is done by patching the game's aspect constant in
// .data — see widescreen_patch_tick() in runtime/main_sdl.cpp.

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>

static void ov_heatwave_skip(CPUState& cpu) {
    call_ppc(cpu, cpu.lr);   // return without drawing the heat-distortion
}
static const bool s_heatwave_registered = [] {
    if (!getenv("SUNBRIGHT_KEEP_SHIMMER"))
        register_override(0x8019f83c, &ov_heatwave_skip);
    return true;
}();
