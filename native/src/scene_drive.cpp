// scene_drive.cpp — OWN the scene-draw flow natively (port, not blackbox-debug).
//
// In sms-boot the GameCube perform-list flag dispatch never delivers bit 0x8 to the renderable
// scene, so TSmJ3DScn::perform's draw block (set view -> light maps -> frameInit buffers ->
// recursively entry() the active children -> draw the buffers) never runs; only per-frame calc()
// executes. Rather than reverse-engineer WHY the GC dispatch drops the flag (the blackbox the user
// said to stop debugging), we DRIVE the real draw flow ourselves: each frame find the scene and
// call its perform(0x8) directly, with the live camera view AND a valid render mode installed in
// the TGraphics. TSmJ3DScn::perform then does the faithful GC draw — recursing into its children
// with (flags | 0x204) so every active TSmJ3DAct runs entry()+viewCalc (only models actually in
// the scene graph, so the NaN/garbage matrices from indiscriminately capturing every calc()'d
// model never arise), and the draw buffers' draw() drives J3DShape::draw (tapped by the ngx capture).
//
// CRITICAL (learned 2026-06-22): the TGraphics MUST carry a valid render mode. perform(8) calls
// mLightMap->perform(0x20) first, which does an EFB->texture copy via GXGetYScaleFactor(efbHeight,
// ..); a zeroed render mode -> efbHeight==0 -> OSPanic. We seed mRenderMode from GXNtsc480Int (the
// faithful 640x480 NTSC mode) + sane rects so the copy path computes a valid scale.
//
// (Native-only TU; only built into sms-native, so no platform guard needed.)
#include <System/MarDirector.hpp>                       // gpMarDirector
#include <JSystem/JDrama/JDRNameRefGen.hpp>             // TNameRefGen::search
#include <JSystem/JDrama/JDRSmJ3DScn.hpp>               // TSmJ3DScn
#include <JSystem/JDrama/JDRGraphics.hpp>               // TGraphics
#include <Camera/Camera.hpp>                            // gpCamera (CPolarSubCamera)
#include <dolphin/mtx.h>
#include <dolphin/gx.h>                                 // GXRenderModeObj, GXNtsc480Int
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" GXRenderModeObj GXNtsc480Int;                // gx_fb_impl.cpp (640x480 NTSC mode)
extern "C" void sb_gx_latch_proj44(const float m[16]);  // gx_impl.cpp — latch the scene perspective

namespace {
bool dbg() {
	static int v = -1;
	if (v < 0) { const char* e = std::getenv("SB_J3D_DBG"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
	return v != 0;
}
JDrama::TGraphics g_graphics;
bool g_graphics_init = false;

void init_graphics() {
	if (g_graphics_init) return;
	g_graphics_init = true;
	std::memset(&g_graphics, 0, sizeof(g_graphics));
	// Valid render mode so the light-map / copy path's GXGetYScaleFactor(efbHeight,..) is sane.
	g_graphics.mRenderMode = GXNtsc480Int;
	const u16 w = GXNtsc480Int.fbWidth, h = GXNtsc480Int.efbHeight;
	g_graphics.mDisplayRect.set(0, 0, w, h);
	g_graphics.mViewportRect.set(0, 0, w, h);
	g_graphics.mScissorRect.set(0, 0, w, h);
	g_graphics.mNearPlane = 1.0f;
	g_graphics.mFarPlane  = 100000.0f;
}
} // namespace

// Drives the scene draw once per frame (the single owned render path). Returns true if a scene
// was driven. Called unconditionally from TMarDirector::direct (no env gate).
extern "C" bool sb_boot_drive_scene() {
	init_graphics();

	// Plain literal: sms-native compiles with -fexec-charset=SHIFT_JIS, so this matches the
	// registered Shift-JIS name (same as MarDirectorSetupObjects.cpp's search).
	JDrama::TSmJ3DScn* scene =
	    JDrama::TNameRefGen::search<JDrama::TSmJ3DScn>("通常シーン");
	if (!scene) {
		if (dbg()) { static bool once=false; if(!once){once=true;
			std::fprintf(stderr, "[scene-drive] '通常シーン' NOT FOUND\n"); } }
		return false;
	}

	// Install the live camera view + perspective. TSmJ3DScn::perform copies g.mViewMtx -> j3dSys
	// before entry(), so this is the view every model is drawn with.
	if (gpCamera) {
		const f32 fovy = gpCamera->getFovy(), aspect = gpCamera->getAspect();
		if (fovy > 1.0f && fovy < 179.0f && aspect > 0.01f) {
			Vec pos, up, target;
			gpCamera->JSGGetViewPosition(&pos);
			gpCamera->JSGGetViewUpVector(&up);
			gpCamera->JSGGetViewTargetPosition(&target);
			C_MTXLookAt(g_graphics.mViewMtx.mMtx, &pos, &up, &target);
			C_MTXPerspective(g_graphics.mProjMtx.mMtx, fovy, aspect,
			                 gpCamera->getNear(), gpCamera->getFar());
			g_graphics.mNearPlane = gpCamera->getNear();
			g_graphics.mFarPlane  = gpCamera->getFar();
			// Latch the perspective for the capture's clip-space project (robust against the
			// HUD's ortho overwriting the live GX projection between now and the shape tap).
			sb_gx_latch_proj44(g_graphics.mProjMtx.mMtx[0]);
		}
	}

	scene->perform(0x8, &g_graphics);

	if (dbg()) {
		static long n = 0;
		if ((++n % 200) == 0 || n <= 3)
			std::fprintf(stderr, "[scene-drive] n=%ld drove '通常シーン'->perform(8)\n", n);
	}
	return true;
}
