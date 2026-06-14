// Owned C++ wrapper for the GameCube GD (display-list build) light helpers.
//
// reference/sms/src/dolphin/gd/GDLight.c is a pristine .c file; adding it to the
// CXX core target would make CMake compile it with the C compiler, WITHOUT the
// force-included compat shims (intrinsics.h / msl_shim.h / shadow types.h). The
// decomp body itself compiles cleanly under the C++ shim flags (verified via
// port/try_compile.sh), so we pull it in as C++ here to guarantee the shims and
// the int-based u32/s32 shadow types apply uniformly with the rest of core.
//
// Defines: GDSetLightAttn / GDSetLightColor / GDSetLightDir / GDSetLightPos
// (referenced by J3DTevs.cpp's per-light XF command build).
#include "../../reference/sms/src/dolphin/gd/GDLight.c"
