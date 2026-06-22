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
#include <JSystem/JDrama/JDRLighting.hpp>               // TLightMap (light probe)
#include <JSystem/JDrama/JDRDrawBufObj.hpp>             // TDrawBufObj (sky draw buffers)
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>          // j3dSys
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>   // J3DDrawBuffer
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

// Drive the sky group's draw into its dedicated buffers (the perform-list path never draws them).
// Mirrors TSmJ3DScn::perform's buffer mechanics (frameInit -> set active -> entry -> draw).
void drive_sky() {
	JDrama::TViewObj* skyGroup =
	    JDrama::TNameRefGen::search<JDrama::TViewObj>("空グループ");
	JDrama::TDrawBufObj* skyOpa =
	    JDrama::TNameRefGen::search<JDrama::TDrawBufObj>("DrawBuf Sky Opa");
	JDrama::TDrawBufObj* skyXlu =
	    JDrama::TNameRefGen::search<JDrama::TDrawBufObj>("DrawBuf Sky Xlu");
	if (dbg()) { static bool once=false; if(!once){once=true;
		std::fprintf(stderr, "[sky-drive] group=%p opa=%p xlu=%p\n",
		             (void*)skyGroup, (void*)skyOpa, (void*)skyXlu); } }
	if (!skyGroup || !skyOpa || !skyXlu) return;
	J3DDrawBuffer* b0 = skyOpa->getDrawBuffer();
	J3DDrawBuffer* b1 = skyXlu->getDrawBuffer();
	if (!b0 || !b1) return;

	MTXCopy(g_graphics.mViewMtx.mMtx, j3dSys.getViewMtx());
	j3dSys.drawInit();
	b0->frameInit();
	b1->frameInit();
	j3dSys.setDrawBuffer(b0, 0);
	j3dSys.setDrawBuffer(b1, 1);
	// flag 0x206 = bit0x2 (TSky::setBaseTRMtx — positions the dome relative to the camera; the
	// missing piece: without it viewCalc builds draw = view*0 and the whole sky collapses to one
	// off-screen point) | bit0x4 (MActor::viewCalc) | bit0x200 (MActor::entry). bit0x8 also makes
	// TSky draw its procedural GXDrawSphere backdrop dome.
	skyGroup->perform(0x20E, &g_graphics);
	j3dSys.setUnk4C(3); b0->draw();
	j3dSys.setUnk4C(4); b1->draw();
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

	if (dbg()) {   // one-shot: is the scene's light map present + resolved?
		static bool once = false;
		if (!once) { once = true;
			JDrama::TLightMap* lm = scene->mLightMap;
			std::fprintf(stderr, "[scene-light] mLightMap=%p count=%d\n",
			             (void*)lm, lm ? lm->mLightInfoCount : -1);
			if (lm) for (int i = 0; i < lm->mLightInfoCount && i < 8; ++i)
				std::fprintf(stderr, "  light[%d] slot=%u obj=%p\n",
				             i, lm->mLightInfos[i].unk0, (void*)lm->mLightInfos[i].unk4);

			// PROBE the REAL stage-light data source: TLightCommon::loadAfter searches the
			// NameRef tree for "Light Group" (TLightAry) + "Ambient Group" (TAmbAry). Those
			// hold the loaded lights (each TLight's GXLightObj already has its color/pos from
			// TLight::load). The stubbed TLightCommon never reads them -> nlights==0. Confirm
			// the data is present before porting the loader.
			JDrama::TLightAry* la =
			    JDrama::TNameRefGen::search<JDrama::TLightAry>("Light Group");
			std::fprintf(stderr, "[stage-light] LightGroup=%p count=%d\n",
			             (void*)la, la ? la->mLightCount : -1);
			if (la) for (int i = 0; i < la->mLightCount && i < 8; ++i) {
				GXColor c; GXGetLightColor(&la->mLights[i].unk24, &c);
				const JGeometry::TVec3<f32>& p = la->mLights[i].getPosition();
				std::fprintf(stderr,
				    "  L[%d] idx=%u col=%u,%u,%u,%u pos=%.1f,%.1f,%.1f\n",
				    i, la->mLights[i].unk68, c.r, c.g, c.b, c.a, p.x, p.y, p.z);
			}
			JDrama::TAmbAry* aa =
			    JDrama::TNameRefGen::search<JDrama::TAmbAry>("Ambient Group");
			std::fprintf(stderr, "[stage-light] AmbGroup=%p count=%d\n",
			             (void*)aa, aa ? aa->mAmbColorCount : -1);
			if (aa) for (int i = 0; i < aa->mAmbColorCount && i < 8; ++i) {
				const JUtility::TColor& m = aa->mAmbColors[i].mColor;
				std::fprintf(stderr, "  A[%d] amb=%u,%u,%u,%u\n",
				             i, m.r, m.g, m.b, m.a);
			}
		}
	}

	// OWN THE STAGE-LIGHT LOAD (port of the stubbed TLightCommon path).
	// The stage's diffuse sun lives in the "Light Group" (TLightAry) scene object — each
	// TIdxLight already carries a fully-loaded GXLightObj (colour via TLight::load) + a
	// world-space position (TPlacement::mPosition). The decomp's TLightCommon::setLight /
	// ::perform would view-transform that position and GXLoadLightObjImm it into GX_LIGHT0/1
	// (the slots the stage CLOF materials' cc0=0x68e enable), but those methods are EMPTY
	// STUBS in reference/sms (community decomp never implemented them) → nothing ever loads a
	// GX light → the (wired+tested) per-vertex lighting consumer is inert (nlights==0).
	// We drive that load ourselves here, faithfully: GX light i ← Light Group light[i], its
	// position transformed by the live view matrix (GX lighting runs in view space), colour
	// from the loaded GXLightObj, flat attenuation (matching the TLight ctor's
	// GXInitLightAttn(1,0,0,1,0,0)). Done every frame because the view matrix moves with the
	// camera. This must run BEFORE perform(0x8): the j3d capture reads sb_gx_get_lights()
	// during the shape draw inside perform.
	//
	// NOTE on index→slot mapping: we use the setLight model (GX_LIGHTi ← getLightColor(i) =
	// light[i]), NOT perform's "same obj in all 3 slots", to avoid double-counting light[0].
	// On the Delfino data the palette is L0=white, L1=black, so this yields a single white
	// diffuse light (L1 contributes nothing) — clean and unambiguous. The 2×-white ambiguity
	// the RE flagged (scratch/light_re_spec.md §5, handoff open-Q) is thereby moot here.
	// NOTE on position: every Light Group entry loads at world-origin (0,0,0) — the data is a
	// colour palette at the origin, with no runtime sun-direction setter on this path (grep
	// GXInitLightDir → none stage-specific). So this is faithfully a point light at the world
	// origin (radial shading), NOT a directional sun. If the look is wrong, the real sun dir
	// is applied elsewhere (JStage during cutscenes) — investigate THAT, do not fudge positions.
	{
		JDrama::TLightAry* la =
		    JDrama::TNameRefGen::search<JDrama::TLightAry>("Light Group");
		if (la && la->mLights) {
			const int n = la->mLightCount < 8 ? la->mLightCount : 8;
			for (int i = 0; i < n; ++i) {
				const JDrama::TIdxLight& src = la->mLights[i];
				GXColor col; GXGetLightColor(&src.unk24, &col);   // loaded by TLight::load
				const JGeometry::TVec3<f32>& wp = src.getPosition();
				Vec mp = { wp.x, wp.y, wp.z }, vp;                // world -> view space
				PSMTXMultVec(g_graphics.mViewMtx.mMtx, &mp, &vp);

				GXLightObj obj;
				GXInitLightPos(&obj, vp.x, vp.y, vp.z);
				GXInitLightColor(&obj, col);
				GXInitLightAttn(&obj, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);  // flat (TLight ctor)
				GXLoadLightObjImm(&obj, (GXLightID)(GX_LIGHT0 << i));
			}
		}
		// Ambient: GXSetChanAmbColor(GX_COLOR0A0, AmbGroup[0]) — faithful to the original.
		// (The per-vertex consumer currently sources ambient from the J3D material block,
		// which in J3D is the same value that loads this register, so this owns the register
		// for parity / a future GX-ambient consumer rather than changing the current output.)
		JDrama::TAmbAry* aa =
		    JDrama::TNameRefGen::search<JDrama::TAmbAry>("Ambient Group");
		if (aa && aa->mAmbColors && aa->mAmbColorCount > 0) {
			const JUtility::TColor& a = aa->mAmbColors[0].mColor;
			GXColor amb = { a.r, a.g, a.b, a.a };
			GXSetChanAmbColor(GX_COLOR0A0, amb);
		}
	}

	// OWN THE SKY DRAW. The sky model ("空グループ" / Sky Group) is NOT a child of 通常シーン;
	// the GC renders it through dedicated draw buffers ("DrawBuf Sky Opa/Xlu") sequenced by the
	// master GX perform list (MarDirectorPreEntry.cpp): set the sky buffers active → entry the
	// sky group (flag 0x204) → draw the buffers. But that perform-list draw never happens in
	// sms-boot: TPerformList AND's the call flag with each entry's stored flag, and the sky
	// buffers' stored flag is 0x480 (frameInit|setDrawBuffer) with NO draw bit (0x8) — the same
	// data-driven dispatch that drops bit 0x8 from 通常シーン (the blackbox scene_drive bypasses).
	// So the sky group is entered into its buffers but the buffers are never drawn → top half
	// stays at the black clear. We drive the full frameInit→entry→draw ourselves, exactly like
	// TSmJ3DScn::perform does for its own buffers (JDRSmJ3DScn.cpp), with our known-good view.
	// Done BEFORE the scene so sky batches land behind the map (capture order = present order).
	drive_sky();

	scene->perform(0x8, &g_graphics);

	if (dbg()) {
		static long n = 0;
		if ((++n % 200) == 0 || n <= 3)
			std::fprintf(stderr, "[scene-drive] n=%ld drove '通常シーン'->perform(8)\n", n);
	}
	return true;
}
