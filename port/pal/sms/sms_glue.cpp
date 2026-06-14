// port/pal/sms/sms_glue.cpp
//
// SMS engine "platform glue" — native definitions for the small set of
// SMS*/SMS_* configuration / render-dimension query symbols the decompiled
// engine references but whose owning translation units are not yet in the port
// core build.
//
// These are NOT game logic. Every function here is a pure configuration /
// render-dimension query (or a tex-cache region hint) with a fixed, faithful
// value lifted directly from the pristine decomp definition. Defining them here
// lets `libsmsport_core.a` (and `smsport_main`) link without dragging in the
// still-uncompiled `src/System/{Application,Resolution,TexCache}.cpp` (which pull
// the full GameSequence / TApplication graph, GX, ARAM, etc. — not yet ported).
//
// SIGNATURE SOURCING: signatures are taken verbatim from the pristine decomp
// headers so linkage (C++, NOT extern "C") and types match bit-for-bit, the same
// discipline as port/pal/gx and port/pal/gd. Resolution.hpp / TexCache.hpp are
// lightweight, so we #include them (single source of truth for the prototypes).
// The two f32 queries are declared in System/Application.hpp, which is heavy
// (pulls Strategic/GameSequence); we re-declare those two prototypes here exactly
// as Application.hpp / cameralib.hpp spell them (`extern f32 SMSGetAnmFrameRate();`
// is already how the engine forward-declares it in cameralib.hpp), avoiding the
// heavy include while keeping the C++ mangling identical.
//
// FAITHFULNESS: values match reference/sms exactly. Where the decomp branches on
// VIGetTvFormat() (NTSC/PAL), we pin the NTSC/GMSE01 (US) path — this is the SMSE
// GMSE01 port; PAL is out of scope and VI is not yet wired in the native build.

#include <dolphin/types.h>
#include <System/Resolution.hpp> // u16 SMSGetGameRenderWidth/Height();
#include <System/TexCache.hpp>   // void SMS_ResetTexCacheRegion();

// ---------------------------------------------------------------------------
// Frame-rate / vsync queries (System/Application.cpp)
// ---------------------------------------------------------------------------
//
// reference/sms/src/System/Application.cpp:
//   f32 SMSGetVSyncTimesPerSec() {
//       f32 result = 60.0f;            // NTSC/MPAL/EURGB60
//       switch (VIGetTvFormat()) { ... case VI_PAL: result = 50.0f; ... }
//       return result / 2.0f;          // SMS runs game logic at half the field rate
//   }
//   f32 SMSGetAnmFrameRate() { return 60.0f / SMSGetVSyncTimesPerSec(); }
//
// NTSC path: result = 60.0f -> SMSGetVSyncTimesPerSec() == 30.0f (the engine's
// game-logic tick rate: 60 fields/s, game logic every other field = 30 Hz).
// SMSGetAnmFrameRate() == 60.0f / 30.0f == 2.0f (animation advances 2.0 "anm
// frames" per 30 Hz game tick, i.e. 60 anm-frames/s of content played at 30 Hz).
//
// Both values are the literal NTSC results of the decomp formulas — not guesses.

f32 SMSGetVSyncTimesPerSec() { return 30.0f; }

f32 SMSGetAnmFrameRate() { return 60.0f / SMSGetVSyncTimesPerSec(); } // == 2.0f

// ---------------------------------------------------------------------------
// Render-dimension queries (System/Resolution.cpp)
// ---------------------------------------------------------------------------
//
// reference/sms/src/System/Resolution.cpp:
//   u16 SMSGetGameRenderWidth()  { return 640; }
//   u16 SMSGetGameRenderHeight() { return 448; }
//
// The GameCube game render (EFB) dimensions, constant across TV formats. Used
// for screen-space projection, HUD layout, lens-flare center, render-target
// sizing, etc. The faithful values are the decomp constants.

u16 SMSGetGameRenderWidth() { return 640; }

u16 SMSGetGameRenderHeight() { return 448; }

// ---------------------------------------------------------------------------
// Texture-cache region hint (System/TexCache.cpp)
// ---------------------------------------------------------------------------
//
// reference/sms/src/System/TexCache.cpp:
//   void SMS_ResetTexCacheRegion() {
//       j3dSys.mFlags |= 0x80000000;
//       j3dSys.setTexCacheRegion(GX_TEXCACHE_128K);
//   }
//
// This configures the Gekko/Flipper GX texture-cache region allocator (a 1 MB
// TMEM partitioning hint). The native PC renderer does not emulate GX TMEM or
// its region allocator, so the faithful behavior in the port is a no-op: there
// is no GC tex-cache region to reset. (Stubbing it as a no-op avoids pulling the
// J3DSys global / GX into the link; when the native renderer is wired this can
// become a real region-config call if ever needed, but for SMS it is purely a
// TMEM-budget hint with no observable game-state effect.)

void SMS_ResetTexCacheRegion()
{
	// no-op: native renderer has no GX TMEM region allocator to reset.
}
