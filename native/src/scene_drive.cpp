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
#include <System/StageUtil.hpp>                         // SMS_isOptionMap
#include <JSystem/JDrama/JDRNameRefGen.hpp>             // TNameRefGen::search
#include <JSystem/JDrama/JDRSmJ3DScn.hpp>               // TSmJ3DScn
#include <JSystem/JDrama/JDRLighting.hpp>               // TLightMap (light probe)
#include <JSystem/JDrama/JDRDrawBufObj.hpp>             // TDrawBufObj (sky draw buffers)
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>          // j3dSys
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>   // J3DDrawBuffer
#include <JSystem/JDrama/JDRGraphics.hpp>               // TGraphics
#include <Camera/Camera.hpp>                            // gpCamera (CPolarSubCamera)
#include <Camera/CameraOption.hpp>                       // gpCameraOption (title/load pan state)
#include <MoveBG/MapObjOption.hpp>                        // TFileLoadBlock
#include <Player/Mario.hpp>                               // gpMarioOriginal (file-select Mario)
#include <M3DUtil/M3UModelMario.hpp>                      // M3UModelMario::getModel (Mario body J3DModel)
#include <M3DUtil/MActor.hpp>                             // MActor::getModel
#include <Strategic/ObjModel.hpp>                         // TMActorKeeper
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>      // J3DModel / J3DModelData
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>         // J3DMatPacket / J3DShapePacket
#include <dolphin/mtx.h>
#include <dolphin/gx.h>                                 // GXRenderModeObj, GXNtsc480Int
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>

extern "C" GXRenderModeObj GXNtsc480Int;                // gx_fb_impl.cpp (640x480 NTSC mode)
extern "C" void sb_gx_latch_proj44(const float m[16]);  // gx_impl.cpp — latch the scene perspective
extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]);  // what the capture uses

namespace {
bool dbg() {
	static int v = -1;
	if (v < 0) { const char* e = std::getenv("SB_J3D_DBG"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
	return v != 0;
}
bool dbg_cam() {
	static int v = -1;
	if (v < 0) { const char* e = std::getenv("SB_CAM_DBG"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
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

extern "C" int sb_present_frame();   // native/render/sms_boot_present.cpp (settled-frame gate)

// Camera-settle detector. The option camera (CPolarSubCamera) SMOOTHLY lerps toward its target
// over ~130 present frames AFTER the file-select choice state (unk10==2) is entered, so a frame
// captured AT unk10==2 catches the camera mid-pan (scene pitched down, cubes shoved to the top —
// these are NOT render bugs, just a premature capture). A faithful "settled file-select" capture
// must wait until the view matrix stops moving. We track the per-frame view-matrix delta here and
// expose sb_camera_view_settled(): true once the view has been ~stationary for several frames.
namespace { int g_cam_stable = 0; float g_prev_view[12] = {0}; bool g_prev_view_valid = false; }
extern "C" int sb_camera_view_settled() { return g_cam_stable >= 8; }
static void sb_track_camera_settle(const Mtx view) {
	float d = 0.f;
	const float* v = &view[0][0];
	if (g_prev_view_valid)
		for (int i = 0; i < 12; ++i) { float e = std::fabs(v[i] - g_prev_view[i]); if (e > d) d = e; }
	for (int i = 0; i < 12; ++i) g_prev_view[i] = v[i];
	g_prev_view_valid = true;
	// Rotation elems are ~[-1,1]; translation elems are ~hundreds. Use a relative-ish threshold:
	// the largest per-element move below 0.30 (covers a ≤0.3-unit translation creep / tiny rotation)
	// counts as "still". The settle is asymptotic, so a small floor avoids never-settling.
	if (d < 0.30f) { if (g_cam_stable < 1000) ++g_cam_stable; } else g_cam_stable = 0;
}

// Drive a named group's draw into its dedicated opa/xlu draw buffers (the GC perform-list path
// enters models into these buffers but masks off the draw bit, so they never draw — see
// CLAUDE.md sky/map notes). Mirrors TSmJ3DScn::perform's buffer mechanics: copy view -> drawInit
// -> frameInit -> set the two buffers active -> group->perform(flag) (entry) -> draw both buffers.
// `flag` is the per-group entry flag from MarDirectorPreEntry.cpp (sky 0x20E incl. TSky bits; map
// 0x204 = viewCalc|entry). Called BEFORE the scene so layers composite back-to-front.
void drive_group(const char* groupName, const char* opaName, const char* xluName, u32 flag) {
	JDrama::TViewObj*    group = JDrama::TNameRefGen::search<JDrama::TViewObj>(groupName);
	JDrama::TDrawBufObj* opa   = JDrama::TNameRefGen::search<JDrama::TDrawBufObj>(opaName);
	JDrama::TDrawBufObj* xlu   = JDrama::TNameRefGen::search<JDrama::TDrawBufObj>(xluName);
	if (dbg()) { static int n=0; if(n<4){++n;
		std::fprintf(stderr, "[drive-group] '%s' group=%p opa=%p xlu=%p\n",
		             groupName, (void*)group, (void*)opa, (void*)xlu); } }
	if (!group || !opa || !xlu) return;
	J3DDrawBuffer* b0 = opa->getDrawBuffer();
	J3DDrawBuffer* b1 = xlu->getDrawBuffer();
	if (!b0 || !b1) return;

	MTXCopy(g_graphics.mViewMtx.mMtx, j3dSys.getViewMtx());
	j3dSys.drawInit();
	b0->frameInit();
	b1->frameInit();
	j3dSys.setDrawBuffer(b0, 0);
	j3dSys.setDrawBuffer(b1, 1);
	group->perform(flag, &g_graphics);
	j3dSys.setUnk4C(3); b0->draw();
	j3dSys.setUnk4C(4); b1->draw();
}

// Sky group → DrawBuf Sky Opa/Xlu. flag 0x20E = bit0x2 (TSky::setBaseTRMtx — positions the dome at
// the camera; without it viewCalc builds draw=view*0 and the sky collapses to one point) | 0x4
// (MActor::viewCalc) | 0x8 (TSky GXDrawSphere backdrop) | 0x200 (MActor::entry).
void drive_sky() { drive_group("空グループ", "DrawBuf Sky Opa", "DrawBuf Sky Xlu", 0x20E); }

// Map group (the plaza BUILDINGS = TMapModel actors) → DrawBuf MapOpa/MapXlu. The perform-list
// (MarDirectorPreEntry.cpp) enters マップグループ with 0x204 (= MActor::viewCalc 0x4 | entry 0x200)
// then never draws the buffers — so the whole map was missing (oracle: vanilla shows a full plaza,
// sms-boot shows only a palm). Drive it like the sky.
// Kept for reference / future re-enable: the scene's own perform(0x8) now draws the map, so this
// is no longer called (driving it duplicated every map surface → z-fight dither). See drive_scene.
[[maybe_unused]] void drive_map() { drive_group("マップグループ", "DrawBuf MapOpa", "DrawBuf MapXlu", 0x204); }

// File-select A/B/C cubes → DrawBuf ChrOpa/ChrXlu. The master perform list
// (MarDirectorPreEntry.cpp lines 44-46) fills the Chr buffers via the MANAGER group
// (マネージャーグループ → map-object manager → the 3 TFileLoadBlock cubes) at flag 0x204 but, like
// the sky/map groups, never sets the draw bit — so the cubes are loaded + correctly positioned
// (840/1080/1320,300,-1000) yet never drawn. Entering the WHOLE manager group re-draws the entire
// map (building-atlas dup / z-fight), so instead we enter ONLY the 3 file-block cube models.
//
// Each cube is a TMapObjBase whose perform() is EMPTY (map objects don't draw via perform; their
// model is drawn from a draw buffer), and whose calc never runs in sms-boot (the calc-anim perform
// list calls that same empty perform), so the model has degenerate draw matrices. We reproduce the
// minimal draw setup ourselves per cube: calcRootMatrix (seed base TR from mPosition) + a unit base
// scale (mScaling/mInitialScaling are 0 — the scale-setting startAnim/makeObjAppeared are empty
// decomp stubs) → calc (joint matrices) → viewCalc (per-view draw matrices) → show the shape
// packets (default hidden, unk30=0) → entry into the Chr buffer → draw. Mario (player group) is a
// separate TODO via this same mechanism.
void drive_chr() {
	JDrama::TDrawBufObj* opa = JDrama::TNameRefGen::search<JDrama::TDrawBufObj>("DrawBuf ChrOpa");
	JDrama::TDrawBufObj* xlu = JDrama::TNameRefGen::search<JDrama::TDrawBufObj>("DrawBuf ChrXlu");
	if (dbg()) { static int n=0; if(n<4){++n;
		std::fprintf(stderr, "[drive-chr] opa=%p xlu=%p\n", (void*)opa, (void*)xlu); } }
	if (!opa || !xlu) return;
	J3DDrawBuffer* b0 = opa->getDrawBuffer();
	J3DDrawBuffer* b1 = xlu->getDrawBuffer();
	if (!b0 || !b1) return;

	MTXCopy(g_graphics.mViewMtx.mMtx, j3dSys.getViewMtx());
	j3dSys.drawInit();
	b0->frameInit();
	b1->frameInit();
	j3dSys.setDrawBuffer(b0, 0);
	j3dSys.setDrawBuffer(b1, 1);
	// Each block's primary MActor (anim variant 0) is the A/B/C cube (2 shapes / 2 mats, verified
	// by SB_BLK_PROBE — variants 1/2 are the rock/no-card models).
	const char* names[3] = { "ロードブロックＡ", "ロードブロックＢ", "ロードブロックＣ" };
	for (int i = 0; i < 3; ++i) {
		TFileLoadBlock* b = JDrama::TNameRefGen::search<TFileLoadBlock>(names[i]);
		if (!b) continue;
		MActor* ma = b->getMActor();
		if (!ma || !ma->getModel()) continue;
		J3DModel* m = ma->getModel();
		// The block's calc never runs (TMapObjBase::perform is empty AND the calc-anim perform list
		// calls that same empty perform for map objects), so the model's draw matrices are
		// degenerate → the cube collapses to one NDC point. Reproduce TLiveActor::perform's calc
		// path: calcRootMatrix() seeds the base TR from mPosition (840/1080/1320,300,-1000), then
		// the MActor calc propagates the joints, viewCalc builds the per-view draw matrices.
		// Both mScaling AND mInitialScaling are 0 for these blocks: the scale-setting paths are
		// empty decomp stubs — TMapObjBase::startAnim() (called by makeBlockNormal to play the
		// "appear" BCK that scales the cube 0→1) and makeObjAppeared() are both `{ }`. So the cube
		// never gets a non-zero scale and collapses to a point. The blocks are unit-scale in the
		// game; 1.0 is the appeared/settled scale shown on the (static) file-select screen. The
		// pop-in scale-up animation itself is a decomp gap (the BCK isn't driven) — left for later.
		b->calcRootMatrix();
		{ Vec bs = { 1.0f, 1.0f, 1.0f }; m->setBaseScale(bs); }
		ma->calc();
		ma->viewCalc();
		// SB_CHR_DBG: dump the cube's actual DRAWN position. baseTR = the model's base TR matrix
		// (world transform from calcRootMatrix: mPosition.y - mYOffset). anm0 = joint-0 world matrix
		// after calc. drawMtx[0] = viewMtx*anm0 = the cube's VIEW-SPACE position. j3dSysView = the view
		// viewCalc multiplied by. This is what PROVED the "cube too high" was the option camera mid-pan
		// (drawMtx vy swung +252 mid-pan → −99 once settled), NOT a cube placement/render bug. Pair with
		// the camera-settle gate on the dump (sb_camera_view_settled) for a clean settled read.
		if (const char* e = getenv("SB_CHR_DBG"); e && e[0] && e[0] != '0') {
			static int cn = 0;
			if (sb_present_frame() > 0 && cn < 9) { ++cn;
				MtxPtr btr = m->getBaseTRMtx();
				const JGeometry::TVec3<f32>& bp = b->getPosition();
				Mtx& dm = m->getDrawMtx(0);
				MtxPtr anm0 = m->getAnmMtx(0);  // joint-0 world matrix after calc (baseTR*jointLocal)
				MtxPtr jv = j3dSys.getViewMtx();   // the view matrix viewCalc actually multiplied by
				std::fprintf(stderr, "[chr-dbg] j3dSysView r0[%.3f,%.3f,%.3f,%.1f] r1[%.3f,%.3f,%.3f,%.1f] r2[%.3f,%.3f,%.3f,%.1f]\n",
				             jv[0][0],jv[0][1],jv[0][2],jv[0][3], jv[1][0],jv[1][1],jv[1][2],jv[1][3], jv[2][0],jv[2][1],jv[2][2],jv[2][3]);
				std::fprintf(stderr, "[chr-dbg] cube%d pos=(%.1f,%.1f,%.1f) baseTR.t=(%.1f,%.1f,%.1f) "
				             "anm0.t=(%.1f,%.1f,%.1f) drawMtx.t=(%.1f,%.1f,%.1f) joints=%d\n",
				             i, bp.x, bp.y, bp.z,
				             btr[0][3], btr[1][3], btr[2][3],
				             anm0 ? anm0[0][3] : 0.f, anm0 ? anm0[1][3] : 0.f, anm0 ? anm0[2][3] : 0.f,
				             dm[0][3], dm[1][3], dm[2][3],
				             m->getModelData() ? (int)m->getModelData()->getJointNum() : -1);
			}
		}
		// The cube shape packets default to HIDDEN (unk30=0) until the normal draw path shows them;
		// our manual entry skips that, so show them so J3DMatPacket::draw won't checkThing-skip them.
		if (m->getModelData()) {
			int ns = (int)m->getModelData()->getShapeNum();
			for (int s = 0; s < ns; ++s) m->getShapePacket((u16)s)->show();
		}
		m->entry();
	}

	// DO NOT drive gpMarioOriginal here — it is a now-redundant, WRONG-TEXTURED DUPLICATE (the
	// drive_map class of stale workaround). Commit 9a9cee7 entered gpMarioOriginal->mModel->getModel()
	// because the file-select Mario looked absent then; since then the real file-select DISPLAY Mario
	// draws on its own (independent of this Chr buffer — SB_NO_DRIVE_MARIO proved it survives), so
	// entering gpMarioOriginal added a SECOND figure. PROVEN 2026-06-25 (SB_MARIO_XF, settled choice):
	// two Mario instances were captured — the real display Mario `mdl=4` (its OWN 4-entry texture
	// table, full materials incl. eyes/cap pkt=12, ndcX≈-0.41) AND gpMarioOriginal `mdl=59` (the
	// gameplay singleton, whose texture table is the map's shared 59-entry table → every body texmap
	// resolved against the building-atlas → garbled white, ndcX≈-0.18). Removing this enter leaves the
	// single correctly-textured display Mario (oracle: one front-centre Mario). The display Mario's
	// residual LEFT/LOW placement vs the oracle is a separate option-scene-floor issue, not this.
	// `SB_DRIVE_MARIO_GHOST=1` re-enables the old duplicate enter for A/B bisection only.
	if (gpMarioOriginal && gpMarioOriginal->mModel &&
	    (std::getenv("SB_DRIVE_MARIO_GHOST") && std::getenv("SB_DRIVE_MARIO_GHOST")[0] != '0')) {
		J3DModel* bm = gpMarioOriginal->mModel->getModel();
		if (bm) {
			bm->viewCalc();
			if (bm->getModelData()) {
				int ns = (int)bm->getModelData()->getShapeNum();
				for (int s = 0; s < ns; ++s) bm->getShapePacket((u16)s)->show();
			}
			bm->entry();
		}
	}

	j3dSys.setUnk4C(3); b0->draw();
	j3dSys.setUnk4C(4); b1->draw();
}
} // namespace

// Drives the scene draw once per frame (the single owned render path). Returns true if a scene
// was driven. Called unconditionally from TMarDirector::direct (no env gate).
extern "C" void sb_boot_capture_frame_begin();   // native/render/sms_boot_j3d_capture.cpp

// SB_BLK_PROBE=1: one-shot inventory of the 3 file-select TFileLoadBlock cubes — found?, mState,
// the active MActor's J3DModel + its modelData shape count + first model-space vertex (tells cube
// vs shared-map model), and the MActorKeeper's MActor count (cube models are anim variants 0/1/2).
extern "C" void sb_blk_probe();

extern "C" bool sb_boot_drive_scene() {
	init_graphics();
	if (const char* e = getenv("SB_BLK_PROBE"); e && e[0] && e[0] != '0') {
		static int n = 0;
		if (n < 3) { ++n; sb_blk_probe(); }
	}

	// Start a fresh capture frame: one drawn scene == one captured frame. direct() can run
	// multiple times between two VI presents (logic loop > retrace under TURBO); without this
	// reset the duplicate scene copies accumulate and interleave at the horizon (the dithered
	// sky/sea band). See sb_boot_capture_frame_begin in sms_boot_j3d_capture.cpp.
	sb_boot_capture_frame_begin();

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
		// DO NOT drive gpCamera->perform(1) for the GAMEPLAY camera. That runs ctrlGameCamera_
		// which RECOMPUTES the camera — and MEASURED via the vanilla oracle (/ngxproj: guest
		// GXSetProjection), the real plaza projection is fovy=50; driving perform(1) changed
		// sms-boot's camera from its correct loaded fovy=50 to a wrong 40. The camera's LOADED
		// state (from the map/TCameraMapTool) is the right establishing shot; we just read it.
		//
		// EXCEPTION — the OPTION/TITLE map (stage 15): here the camera MUST get its calc pass.
		// The title→file-select transition (TCardLoad::perform case 3) is gated on
		// gpCameraOption->mIntroChaseTimer == 0, and that only counts down inside CPolarSubCamera::
		// ctrlOptionCamera_(), which runs from perform(0x1) when SMS_isOptionMap(). sms-boot's
		// GC perform-list dispatch drops the camera's calc flag (the same blackbox that drops the
		// scene draw bit, owned above), so unkA stays pinned at 300 and the file-select is
		// unreachable. Drive the option-camera calc ourselves — it runs ctrlOptionCamera_ (NOT
		// ctrlGameCamera_), so the gameplay-fovy concern above does not apply.
		if (SMS_isOptionMap())
			gpCamera->perform(0x1, &g_graphics);

		const f32 fovy = gpCamera->getFovy(), aspect = gpCamera->getAspect();
		if (fovy > 1.0f && fovy < 179.0f && aspect > 0.01f) {
			Vec pos, up, target;
			gpCamera->JSGGetViewPosition(&pos);
			gpCamera->JSGGetViewUpVector(&up);
			gpCamera->JSGGetViewTargetPosition(&target);
			// The eye (y=328) is BELOW the target (y=828) so the camera looks UP ~30deg and the
			// plaza buildings (eye-space ey~-780, below) fall off the bottom (ndc.y~2.2). An
			// establishing shot should look DOWN at the plaza. (Ruled out: a simple eye/target
			// swap projects them even further off — ndc.y~7. The loaded camera target itself is
			// wrong/too-high. NEXT: read vanilla's gpCamera eye/target for ground truth.)
			C_MTXLookAt(g_graphics.mViewMtx.mMtx, &pos, &up, &target);
			C_MTXPerspective(g_graphics.mProjMtx.mMtx, fovy, aspect,
			                 gpCamera->getNear(), gpCamera->getFar());
			g_graphics.mNearPlane = gpCamera->getNear();
			g_graphics.mFarPlane  = gpCamera->getFar();
			// Latch the perspective for the capture's clip-space project (robust against the
			// HUD's ortho overwriting the live GX projection between now and the shape tap).
			sb_gx_latch_proj44(g_graphics.mProjMtx.mMtx[0]);

			// Feed the camera-settle detector (the view the scene/cubes will draw with this frame).
			sb_track_camera_settle(g_graphics.mViewMtx.mMtx);

			// PROJECTION DIVERGENCE DETECTOR (value-based, not visual). The reference is the
			// camera's own C_MTXPerspective (6-element GX form); the actual is what the capture
			// reads via sb_gx_get_projection. Any per-element gap means the capture is using a
			// stale/wrong projection (it was — a stale GXSetProjection with fovy 50 + a degenerate
			// z-row). Logs the worst-diverging element so a regression is caught as a NUMBER.
			if (dbg_cam()) {
				const float* P = g_graphics.mProjMtx.mMtx[0];   // row-major 4x4
				const float ref[6] = { P[0], P[2], P[5], P[6], P[10], P[11] };
				int aty; float act[6], avp[6]; sb_gx_get_projection(&aty, act, avp);
				int worst = -1; float wd = 0.f;
				for (int i = 0; i < 6; ++i) { float d = std::fabs(ref[i]-act[i]); if (d > wd) { wd = d; worst = i; } }
				std::fprintf(stderr, "[proj-diverge] worst elem=%d |ref-act|=%.5f  ref[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f] act[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f] type ref=0 act=%d\n",
				             worst, wd, ref[0],ref[1],ref[2],ref[3],ref[4],ref[5],
				             act[0],act[1],act[2],act[3],act[4],act[5], aty);
			}

			// CAMERA ORACLE (SB_CAM_DBG): dump the full camera state so we can verify it
			// numerically instead of eyeballing. The sky-dome positioning question reduces to
			// "is the camera pitch right?": a camera-centered sky dome's equator projects to
			// ndc_y = tan(pitch)/tan(fovy/2). pitch = angle of the look vector (target-pos) below
			// the horizontal plane. If pitch is steep (~30°) the dome equator lands above the
			// top edge (matches the observed black top) and the camera is plausibly faithful;
			// if pitch is gentle the dome should be visible and the camera/transform is wrong.
			if (dbg_cam()) { static long cn=0; if((cn++ % 60)==0){
				// Dump the option-camera state machine (intro pan / load pan / flags) so we can
				// see which phase the SETTLED file-select camera is in. gpCameraOption holds the
				// timers that gate title→load transition (CameraOption.cpp).
				if (gpCameraOption) std::fprintf(stderr,
				    "[cam-opt] n=%ld intro=%d loadPan=%d cubePan=%d upDown=%d flags=0x%x fovy=%.1f\n",
				    cn, gpCameraOption->mIntroChaseTimer, gpCameraOption->mLoadPanTimer,
				    gpCameraOption->mCubePanTimer, gpCameraOption->mUpDownPanTimer,
				    gpCameraOption->mFlags, gpCameraOption->mFovy);
				else std::fprintf(stderr, "[cam-opt] n=%ld gpCameraOption=NULL\n", cn);
				// The camera target tracks Mario (cameragc: mTarget = gpCameraMario pos). Dump
				// Mario's actual position to see if a wrong view = wrong Mario placement.
				if (gpMarioPos) std::fprintf(stderr, "[cam-mario] gpMarioPos=%.1f,%.1f,%.1f\n",
				                             gpMarioPos->x, gpMarioPos->y, gpMarioPos->z);
				else std::fprintf(stderr, "[cam-mario] gpMarioPos=NULL (Mario not spawned!)\n");
				const Vec look = { target.x-pos.x, target.y-pos.y, target.z-pos.z };
				const float horiz = std::sqrt(look.x*look.x + look.z*look.z);
				const float pitch_deg = std::atan2(-look.y, horiz) * 57.29578f;  // + = looking down
				const float halffov = fovy * 0.5f * 0.01745329f;
				const float eq_ndc_y = std::tan(pitch_deg * 0.01745329f) / std::tan(halffov);
				std::fprintf(stderr,
				    "[cam-oracle] pos=%.1f,%.1f,%.1f target=%.1f,%.1f,%.1f up=%.2f,%.2f,%.2f\n"
				    "             fovy=%.2f aspect=%.3f near=%.1f far=%.1f pitch=%.1fdeg(down+) "
				    "dome_equator_ndc_y=%.3f\n"
				    "             view r0[%.3f,%.3f,%.3f,%.1f] r1[%.3f,%.3f,%.3f,%.1f] r2[%.3f,%.3f,%.3f,%.1f]\n",
				    pos.x,pos.y,pos.z, target.x,target.y,target.z, up.x,up.y,up.z,
				    fovy, aspect, gpCamera->getNear(), gpCamera->getFar(), pitch_deg, eq_ndc_y,
				    g_graphics.mViewMtx.mMtx[0][0],g_graphics.mViewMtx.mMtx[0][1],g_graphics.mViewMtx.mMtx[0][2],g_graphics.mViewMtx.mMtx[0][3],
				    g_graphics.mViewMtx.mMtx[1][0],g_graphics.mViewMtx.mMtx[1][1],g_graphics.mViewMtx.mMtx[1][2],g_graphics.mViewMtx.mMtx[1][3],
				    g_graphics.mViewMtx.mMtx[2][0],g_graphics.mViewMtx.mMtx[2][1],g_graphics.mViewMtx.mMtx[2][2],g_graphics.mViewMtx.mMtx[2][3]); } }
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
	// Own the SKY backdrop only (see drive_sky). The map group is NOT driven here: the scene's
	// own perform(0x8) below draws the full map (ground/sea/buildings) — it didn't when drive_map
	// was first added (the map appeared empty), but the option-camera calc pass (gpCamera->
	// perform(0x1) above) and the POS-array sentinel fix made TSmJ3DScn::perform draw the complete
	// map. Driving drive_map() too then drew every map surface TWICE: two identical opaque
	// surfaces (z_write=1, GX_LEQUAL) at equal depth z-fight per pixel — measured (SB_BATCH_DBG)
	// as duplicate batches (sea/white/ground keys appearing 2×) and seen as the diagonal blue/white
	// dither over the sea+beach. perform(0x8) is the faithful single draw; drive_map was a now-stale
	// workaround. drive_sky stays — perform does NOT draw the sky backdrop (no vc=228 full-screen
	// blue batch in a perform-only capture).
	drive_sky();

	scene->perform(0x8, &g_graphics);

	// File-select A/B/C cubes (TFileLoadBlock) draw into DrawBuf ChrOpa/ChrXlu, which the perform
	// list fills but never draws (the same dropped-draw-bit class as sky/map). drive_chr drives
	// the 3 cubes' models directly (calc → viewCalc → entry → draw). ON by default (SB_NO_DRIVE_CHR
	// opts out). NOTE the old naive マネージャーグループ perform(0x204) re-drew the whole map (b31..b45
	// building-atlas dup) — drive_chr now enters ONLY the 3 file blocks. Mario (player group) is a
	// separate TODO via this same path.
	if (const char* e = getenv("SB_NO_DRIVE_CHR"); !(e && e[0] && e[0] != '0'))
		drive_chr();

	if (dbg()) {
		static long n = 0;
		if ((++n % 200) == 0 || n <= 3)
			std::fprintf(stderr, "[scene-drive] n=%ld drove '通常シーン'->perform(8)\n", n);
	}
	return true;
}

// ── SB_BLK_PROBE: file-select TFileLoadBlock cube inventory (diagnostic) ───────────────────────
// Answers: did each block's own cube model (FileLoadBlockA/B/C.bmd) load, or is getMActor()
// returning the shared option-map model? Prints, per block: mState, the actor-keeper MActor count
// and EACH MActor's J3DModel + modelData shape/material counts (a cube bmd has few shapes; the
// option map has hundreds — the discriminator).
extern "C" void sb_blk_probe() {
	const char* names[3] = { "ロードブロックＡ", "ロードブロックＢ", "ロードブロックＣ" };
	for (int i = 0; i < 3; ++i) {
		TFileLoadBlock* b = JDrama::TNameRefGen::search<TFileLoadBlock>(names[i]);
		if (!b) { std::fprintf(stderr, "[blkprobe] block%d NOT FOUND\n", i); continue; }
		TMActorKeeper* k = b->getActorKeeper();
		MActor* pm = b->getMActor();
		J3DModel* pmodel = pm ? pm->getModel() : nullptr;
		std::fprintf(stderr, "[blkprobe] block%d state=%d primaryMActor=%p model=%p keeper=%p actorNum=%d\n",
		             i, (int)b->mState, (void*)pm, (void*)pmodel, (void*)k, k ? (int)k->mActorNum : -1);
		if (k && k->mActors) {
			for (int a = 0; a < (int)k->mActorNum; ++a) {
				MActor* ma = k->mActors[a];
				J3DModel* m = ma ? ma->getModel() : nullptr;
				J3DModelData* md = m ? m->getModelData() : nullptr;
				std::fprintf(stderr, "[blkprobe]   mactor[%d]=%p model=%p modelData=%p shapes=%d mats=%d\n",
				             a, (void*)ma, (void*)m, (void*)md,
				             md ? (int)md->getShapeNum() : -1, md ? (int)md->getMaterialNum() : -1);
				// Mat-packet → shape-packet linkage + shape-packet draw gate (J3DMatPacket::draw
				// only draws when unk34 chain has a packet with unk30!=0; J3DShapePacket::draw
				// also gates on unk14!=0 && unk30!=0). This tells whether the cube models carry
				// drawable draw-packets at all.
				if (a == 0 && m && md) {
					int nm = (int)md->getMaterialNum();
					for (int mi = 0; mi < nm && mi < 4; ++mi) {
						J3DMatPacket* mpk = m->getMatPacket((u16)mi);
						J3DShapePacket* sp = mpk ? mpk->getShapePacket() : nullptr;
						std::fprintf(stderr, "[blkprobe]     matpkt[%d]=%p shapePkt(unk34)=%p\n",
						             mi, (void*)mpk, (void*)sp);
					}
				}
			}
		}
	}
}
