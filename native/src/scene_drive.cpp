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
#include <JSystem/JDrama/JDRViewObjPtrList.hpp>         // TViewObjPtrListT (indirect scene walk)
#include <Strategic/HitActor.hpp>                       // THitActor (マップグループ children)
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>          // j3dSys
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>   // J3DDrawBuffer
#include <JSystem/JDrama/JDRGraphics.hpp>               // TGraphics
#include <Camera/Camera.hpp>                            // gpCamera (CPolarSubCamera)
#include <Camera/CameraOption.hpp>                       // gpCameraOption (title/load pan state)
#include <MoveBG/MapObjOption.hpp>                        // TFileLoadBlock
#include <MoveBG/MapObjWave.hpp>                          // TMapObjWave / gpMapObjWave (reflective sea)
#include <GC2D/GCConsole2.hpp>                            // TGCConsole2 (in-game HUD console)
#include <Player/Mario.hpp>                               // gpMarioOriginal (file-select Mario)
#include <M3DUtil/M3UModelMario.hpp>                      // M3UModelMario::getModel (Mario body J3DModel)
#include <M3DUtil/MActor.hpp>                             // MActor::getModel
#include <Map/Sky.hpp>                                    // TSky (sky anim probe)
#include <Strategic/ObjModel.hpp>                         // TMActorKeeper
#include <Enemy/Conductor.hpp>                             // gpConductor (NPC calc pass)
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>      // J3DModel / J3DModelData
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>         // J3DMatPacket / J3DShapePacket
#include <Map/Map.hpp>                                     // TMap / gpMap
#include <Map/MapModel.hpp>                                // TMapModelManager
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>          // J3DShape::getDisplayListSize
#include <dolphin/mtx.h>
#include <dolphin/gx.h>                                 // GXRenderModeObj, GXNtsc480Int
#include "sms_boot_setlight.h"                          // sb::build_stage_lights (TLightCommon::setLight port)
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <functional>

extern "C" GXRenderModeObj GXNtsc480Int;                // gx_fb_impl.cpp (640x480 NTSC mode)
extern "C" void sb_gx_latch_proj44(const float m[16]);  // gx_impl.cpp — latch the scene perspective
extern "C" void sb_gx_get_projection(int* type, float proj[6], float vp[6]);  // what the capture uses
extern "C" void sb_fi_watch_register(J3DDrawBuffer* opa, J3DDrawBuffer* xlu);  // J3DDrawBuffer.cpp (SB_FI_TRACE)

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
} // namespace

// SB_OWN_GXLIST=1: the OWNED single draw path. Instead of hand-driving the scene
// (drive_sky/scene->perform(0x8)/drive_chr), capture the REAL GC master GX perform-list render
// (MarDirectorDirect.cpp else-branch: mPerformListGX/Silhouette/GXPost) once per VI present. That
// path draws the FULL scene the way the game does (measured 2026-06-25: 140 displayed batches vs the
// hand-driven 60, oracle 155 — see debug_journal/2026-06-25_own_gxlist_draw.md). scene_drive still
// runs its per-frame SETUP (camera / projection latch / stage-light load / option-camera calc /
// camera-settle), because the real path leaves TLightCommon::setLight a stub (no GX light load) and
// needs the option-camera calc to advance the file-select transition. Opt-in while it is verified
// at the value level (the scene renders overbright-white, so eyeballing is invalid — parity only).
extern "C" int sb_own_gxlist() {
	static int v = -1;
	if (v < 0) { const char* e = std::getenv("SB_OWN_GXLIST"); v = (e && e[0] && e[0] != '0') ? 1 : 0; }
	return v;
}

namespace {
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
extern "C" int  sb_boot_capture_begin_scene();  // sms_boot_j3d_capture.cpp — capture-once-per-present
extern "C" void sb_boot_capture_end_scene();
extern "C" int  sb_boot_capture_vert_count(void);   // sms_boot_j3d_capture.cpp — SB_PASS_VERTS diag
extern "C" int  sb_boot_capture_batch_count(void);

// Camera-settle detector. The option camera (CPolarSubCamera) SMOOTHLY lerps toward its target
// over ~130 present frames AFTER the file-select choice state (unk10==2) is entered, so a frame
// captured AT unk10==2 catches the camera mid-pan (scene pitched down, cubes shoved to the top —
// these are NOT render bugs, just a premature capture). A faithful "settled file-select" capture
// must wait until the view matrix stops moving. We track the per-frame view-matrix delta here and
// expose sb_camera_view_settled(): true once the view has been ~stationary for several frames.
namespace { int g_cam_stable = 0; float g_prev_view[12] = {0}; bool g_prev_view_valid = false; }
extern "C" int sb_camera_view_settled() { return g_cam_stable >= 8; }

// sb_boot_get_scene_camera — expose the REAL scene view+proj (g_graphics), the matrices the 3D
// draw is composited with this frame. This is the authoritative camera for the rung-6 transplant
// (the present-time j3dSys.mViewMtx is whatever the LAST draw set — often a 2D/HUD matrix — so it
// is NOT a reliable camera source). Returns 1 if a valid (settle-tracked) scene view is published.
extern "C" int sb_boot_get_scene_camera(float view[12], float proj[16]) {
	if (view) for (int i = 0; i < 12; ++i) view[i] = g_graphics.mViewMtx.mMtx[0][i];
	if (proj) for (int i = 0; i < 16; ++i) proj[i] = g_graphics.mProjMtx.mMtx[0][i];
	return g_prev_view_valid ? 1 : 0;
}
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
void drive_sky() {
	// SB_SKY_FLAG=hex overrides the sky-group entry flag for A/B testing (real perform-list flag is
	// 0x204 per MarDirectorPreEntry.cpp; 0x20E adds 0x2 setBaseTRMtx + 0x8 GXDrawSphere backdrop).
	u32 f = 0x20E;
	if (const char* e = std::getenv("SB_SKY_FLAG"); e && e[0]) f = (u32)std::strtoul(e, nullptr, 16);
	if (const char* e = std::getenv("SB_SKY_ANM_PROBE"); e && e[0] && e[0] != '0') {
		static int pn = 0;
		if (pn < 2) { ++pn;
			TSky* sky = JDrama::TNameRefGen::search<TSky>("空");
			MActor* ma = sky ? sky->unk44 : nullptr;
			std::fprintf(stderr, "[sky-anm] sky=%p mactor=%p bck=%p bckList=%p bpk=%p btp=%p btk=%p brk=%p\n",
			             (void*)sky, (void*)ma,
			             ma?(void*)ma->unkC:nullptr, ma?(void*)ma->unk10:nullptr,
			             ma?(void*)ma->unk14:nullptr, ma?(void*)ma->unk18:nullptr,
			             ma?(void*)ma->unk1C:nullptr, ma?(void*)ma->unk20:nullptr);
		}
	}
	drive_group("空グループ", "DrawBuf Sky Opa", "DrawBuf Sky Xlu", f);
}

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
// packets (default hidden, unk30=0) → entry into the Chr buffer → draw. The gameplay player Mario
// (gpMarioOriginal) is entered via this same Chr buffer just below (faithful calcView/entryModels).
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

	// OWN THE GAMEPLAY PLAYER DRAW. In the GAMEPLAY map (not the option/file-select map) the
	// player Mario (gpMarioOriginal) is spawned and his logic runs (gpMarioPos updates), but his
	// MODEL is never entered into a draw buffer: the GC sequences TMario::entryModels through the
	// Chr draw buffers via the master perform list, and sms-boot's data-driven perform-list
	// dispatch drops the draw bit (the same blackbox that drops the scene/sky/map draw bits, owned
	// elsewhere in this file). So the protagonist is invisible. We drive the FAITHFUL entry here,
	// exactly as TMarDirector would: with ChrOpa/ChrXlu set as the active draw buffers (done above),
	// calcView() builds the per-view draw matrices (body + hands + cap), then entryModels() enters
	// the body (mModel->perform(0x200)) + hands + cap into the buffers, which b0/b1->draw() below
	// then render. This is distinct from the file-select "ghost" path above: that fired in the
	// OPTION map where a separate display-Mario already drew; the gameplay map has no such Mario.
	// SB_NO_DRIVE_MARIO=1 opts out (A/B bisection).
	if (gpMarioOriginal && gpMarioOriginal->mModel && !SMS_isOptionMap() &&
	    !(std::getenv("SB_NO_DRIVE_MARIO") && std::getenv("SB_NO_DRIVE_MARIO")[0] != '0')) {
		// POSE THE SKELETON. calcView builds the per-view draw matrices FROM the J3D node
		// matrices, but in sms-boot the perform-list never delivers bit 0x1 to Mario, so
		// calcAnim never runs and the node matrices sit at the rest/bind pose (an unposed
		// T-pose-ish mesh). Drive the faithful skeleton pass first — exactly TMario::perform's
		// 0x1 branch does (MarioMain.cpp:118): calcAnim(2) = calcBaseMtx -> considerWaist ->
		// mModel->perform(2) (the skeleton calc) -> pose hands + cap. This updates the node
		// matrices so calcView's draw matrices reflect the current animation pose. We do NOT run
		// playerControl (input/physics) — just the pose. SB_NO_MARIO_ANIM=1 opts out (A/B).
		if (!(std::getenv("SB_NO_MARIO_ANIM") && std::getenv("SB_NO_MARIO_ANIM")[0] != '0'))
			gpMarioOriginal->calcAnim(2, &g_graphics);
		gpMarioOriginal->calcView(&g_graphics);
		gpMarioOriginal->entryModels(&g_graphics);
		if (const char* e = getenv("SB_MARIO_DBG"); e && e[0] && e[0] != '0') {
			static int mn = 0;
			if (sb_present_frame() > 0 && mn < 6) { ++mn;
				J3DModel* bm = gpMarioOriginal->mModel->getModel();
				J3DModelData* md = bm ? bm->getModelData() : nullptr;
				Mtx& dm = bm->getDrawMtx(0);
				std::fprintf(stderr, "[mario-dbg] pos=(%.1f,%.1f,%.1f) status=%d model=%p shapes=%d "
				             "drawMtx.t=(%.1f,%.1f,%.1f)\n",
				             gpMarioOriginal->mPosition.x, gpMarioOriginal->mPosition.y,
				             gpMarioOriginal->mPosition.z, (int)gpMarioOriginal->mStatus, (void*)bm,
				             md ? (int)md->getShapeNum() : -1, dm[0][3], dm[1][3], dm[2][3]);
			}
		}
		// SB_MARIO_GEOM=1: per-frame geometry dump of the POSED skeleton — every joint's
		// node-matrix translation + a checksum over the full 3x4 set. Two uses: (1) self-check
		// across frames — a STATIC checksum means the animation frame is FROZEN (calcAnim poses
		// from a non-advancing anim controller); (2) joined to the oracle's same dump (real game
		// via the probe) for a value-level pose compare (NOT eyeballing the overbright frame).
		if (const char* e = getenv("SB_MARIO_GEOM"); e && e[0] && e[0] != '0') {
			static int gn = 0;
			if (sb_present_frame() > 0 && gn < 30) { ++gn;
				J3DModel* bm = gpMarioOriginal->mModel->getModel();
				J3DModelData* md = bm ? bm->getModelData() : nullptr;
				int nj = md ? (int)md->getJointNum() : 0;
				double cs = 0; int anim = (int)gpMarioOriginal->mAnimationId;
				for (int j = 0; j < nj; ++j) {
					MtxPtr m = bm->getAnmMtx(j);
					for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c) cs += (double)m[r][c] * (1.0 + 0.001*(r*4+c));
				}
				std::fprintf(stderr, "[mario-geom] f=%d anim=%d njoint=%d cksum=%.4f", sb_present_frame(), anim, nj, cs);
				const int probe[5] = {0, 26 /*head*/, 28 /*mhead*/, nj>10?10:0, nj>20?20:0};
				for (int pi = 0; pi < 5; ++pi) { int j = probe[pi]; if (j<0||j>=nj) continue;
					MtxPtr m = bm->getAnmMtx(j);
					std::fprintf(stderr, " j%d=(%.1f,%.1f,%.1f)", j, m[0][3], m[1][3], m[2][3]); }
				std::fprintf(stderr, "\n");
			}
		}
	}

	j3dSys.setUnk4C(3); b0->draw();
	j3dSys.setUnk4C(4); b1->draw();
}

// In-game HUD (TGCConsole2 = "GCコンソール"): the coin/shine/timer/life counters + FLUDD water
// gauge, drawn from the J2DSetScreen built off "standard_1.blo". On real HW the master GX
// perform-list delivers bit 0x8 to the console's perform → it draws. In sms-boot the perform-list
// masks the 2D draw bit (same dropped-draw-bit class as the sky/map/Chr groups), so the HUD never
// reaches the screen — the SAME reason drive_sky/drive_chr exist. Mirror them: find the console by
// its Shift-JIS name and call perform(0x8) directly. The freshly-ported TGCConsole2::perform
// (reference/sms GCConsole2.cpp) builds a J2DOrthoGraph from the viewport and draws unkB0, which
// flows through the J2D imm-capture path → SDL3-GPU. ON by default (SB_NO_HUD opts out).
void drive_hud() {
	TGCConsole2* console = JDrama::TNameRefGen::search<TGCConsole2>("GCコンソール");
	if (!console) {
		if (dbg()) { static long n=0; if((++n%200)==1)
			std::fprintf(stderr, "[drive-hud] no 'GCコンソール' found (not loaded this scene)\n"); }
		return;
	}
	// param_1 & 0x8 = the J2D draw branch (the only branch the stage-1 port implements).
	console->perform(0x8, &g_graphics);
	if (dbg()) { static long n=0; if((++n%200)==1)
		std::fprintf(stderr, "[drive-hud] drove TGCConsole2::perform(0x8) console=%p\n", (void*)console); }
}

// The REFLECTIVE SEA (TMapObjWave, "波の表現"). Ported in reference/sms/src/MoveBG/MapObjWave.cpp;
// oracle GX-draw attribution (SUNBRIGHT_GX_ATTRIB) pinned the turquoise sea to its draw(). Like
// the sky/HUD/Chr, sms-boot's perform-list dispatch never delivers the draw bit to it, so it must
// be hand-driven. Its draw() emits RAW immediate-mode GX (GXBegin/GXPosition — NOT a J3DShape), so
// it is captured by the gx_imm path (which is NOT per-present locked), and only needs the live
// PERSPECTIVE projection active at GXBegin (initDraw only loads the view pos-matrix). Driven in
// BOTH the OWN_GXLIST and hand-driven paths (the imm capture accumulates independent of the J3D
// scene lock). ON by default; SB_NO_WAVE opts out.
extern "C" int sb_boot_wave_begin();   // sms_boot_j3d_capture.cpp — once-per-present gate
void drive_wave() {
	if (const char* e = getenv("SB_NO_WAVE"); e && e[0] && e[0] != '0') return;
	if (!gpMapObjWave) return;
	// Draw the wave EXACTLY once per present interval. drive_wave runs on every direct(), which under
	// TURBO fires several times per shown frame; the imm buffer accumulates across the interval (it
	// only clears on the first GXBegin after a present drains), so an ungated wave triple-appended its
	// 1352-vert grid -> scene-vert overdraw (native 3900 imm vs oracle 1352). The gate matches the
	// game's once-per-VI draw. Value-verified: TMapObjWave::draw oracle imm = 1352 (26 strips × 52).
	if (!sb_boot_wave_begin()) return;
	// initDraw loads j3dSys.getViewMtx() as the pos matrix, but under OWN_GXLIST the scene's own
	// perform(0x8) (which normally copies g.mViewMtx -> j3dSys) has not run when we get here, so
	// j3dSys.mViewMtx is stale. Install the live camera view (computed above) exactly as
	// TSmJ3DScn::perform would, so the water grid transforms with the current camera.
	j3dSys.setViewMtx(g_graphics.mViewMtx.mMtx);
	// Make the scene's perspective the active GX projection so the immediate-mode capture
	// projects the water grid as 3D (not the HUD's ortho, which may be the last-set proj).
	GXSetProjection(g_graphics.mProjMtx.mMtx, GX_PERSPECTIVE);
	gpMapObjWave->perform(0x1, &g_graphics);  // updateTime — scroll the wave/tex phases
	gpMapObjWave->perform(0x8, &g_graphics);  // initDraw + draw (the sea grid)
	if (dbg()) { static long n=0; if((++n%200)==1)
		std::fprintf(stderr, "[drive-wave] drove TMapObjWave::perform(8) wave=%p\n", (void*)gpMapObjWave); }
}
} // namespace

// Drives the scene draw once per frame (the single owned render path). Returns true if a scene
// was driven. Called unconditionally from TMarDirector::direct (no env gate).
extern "C" void sb_boot_capture_frame_begin();   // native/render/sms_boot_j3d_capture.cpp

// SB_BLK_PROBE=1: one-shot inventory of the 3 file-select TFileLoadBlock cubes — found?, mState,
// the active MActor's J3DModel + its modelData shape count + first model-space vertex (tells cube
// vs shared-map model), and the MActorKeeper's MActor count (cube models are anim variants 0/1/2).
extern "C" void sb_blk_probe();

// SB_DRAWBUF_INV=1: one-shot inventory — print each named TDrawBufObj's J3DDrawBuffer
// pointer so a cyclic buf=<ptr> (J3DDrawBuffer::drawHead) can be attributed to a name.
static void sb_drawbuf_inventory() {
	static const char* names[] = {
		"DrawBuf ChrOpa", "DrawBuf ChrXlu", "DrawBuf Graffito", "DrawBuf Indirect",
		"DrawBuf AfterIndirect Opa", "DrawBuf AfterIndirect Xlu",
		"DrawBuf StaticMapObj SunOpa", "DrawBuf StaticMapObj SunXlu",
		"DrawBuf LensFlare", "DrawBuf MapOpa", "DrawBuf MapXlu",
		"DrawBuf Map 半透明優先2 (opa)", "DrawBuf Map 半透明優先2 (xlu)",
		"DrawBuf Map 半透明優先 (opa)", "DrawBuf Map 半透明優先 (xlu)",
		"DrawBuf Sky Opa", "DrawBuf Sky Xlu",
		"DrawBuf Mirror Opa", "DrawBuf Mirror Xlu",
		"DrawBuf MirrorSky Opa", "DrawBuf MirrorSky Xlu",
		"DrawBuf MirrorAlways Opa", "DrawBuf MirrorAlways Xlu",
	};
	for (const char* nm : names) {
		JDrama::TDrawBufObj* o = JDrama::TNameRefGen::search<JDrama::TDrawBufObj>(nm);
		J3DDrawBuffer* b = o ? o->getDrawBuffer() : nullptr;
		if (o) {
			void** vt = *(void***)o;
			Dl_info info; const char* cls = (dladdr(vt[0], &info) && info.dli_sname) ? info.dli_sname : "?";
			std::fprintf(stderr, "[drawbuf-inv]   %s vtbl-class=%s\n", nm, cls);
		}
		// Walk the buffer's packet chains: heads = occupied slots, packets = total entered models.
		// A non-empty global Map/Chr buffer that native never DRAWS is the missing-geometry target.
		long heads = 0, packets = 0;
		// SB_DRAWBUF_MAT=1: also print each packet's J3DMaterial ptr (cast to J3DMatPacket), so the
		// overbright harness can NAME which buffer holds the sea-mask material (0x…c97c28, key
		// eb5c8e74) — GXPost's initECDisp has NO MapXlu, so if a Chr/LensFlare buffer holds it the
		// mask is erroneously entered into the display pass (the spurious ph6 white overdraw).
		const bool matdmp = [](){ const char* e = getenv("SB_DRAWBUF_MAT"); return e && e[0] && e[0] != '0'; }();
		char mats[256]; int mn = 0; mats[0] = 0;
		if (b && b->mBuffer)
			for (u32 s = 0; s < b->mSize; ++s) {
				J3DPacket* p = b->mBuffer[s];
				if (p) ++heads;
				for (; p; p = p->getNextPacket()) { ++packets;
					if (matdmp && mn < 220) {
						J3DMaterial* m = static_cast<J3DMatPacket*>(p)->getMaterial();
						mn += std::snprintf(mats + mn, sizeof(mats) - mn, " %06x", (unsigned)((uintptr_t)m & 0xffffff));
					}
				}
			}
		std::fprintf(stderr, "[drawbuf-inv] %-32s obj=%p buf=%p heads=%ld packets=%ld mats=%s\n",
		             nm, (void*)o, (void*)b, heads, packets, mats);
	}
}

// SB_IND_DBG=1: one-shot probe of the reflective-sea INDIRECT scene. Identifies the
// class of the "インダイレクトシーン" TViewObj (via dladdr on its vtable slots), whether the
// SeaIndirect model loaded (shape count), and the state of DrawBuf Indirect — so we can see
// WHY the buffer stays empty (object missing / model missing / perform enters nothing).
static void sb_indirect_probe() {
	auto dumpVtbl = [](const char* tag, void* obj){
		if (!obj) { std::fprintf(stderr, "[ind-dbg] %s = NULL\n", tag); return; }
		void** vt = *(void***)obj;
		std::fprintf(stderr, "[ind-dbg] %s=%p vtbl=%p", tag, obj, (void*)vt);
		Dl_info info;
		for (int i = 0; i < 8; ++i) {
			if (vt[i] && dladdr(vt[i], &info) && info.dli_sname)
				std::fprintf(stderr, "\n            vt[%d] %s", i, info.dli_sname);
		}
		std::fprintf(stderr, "\n");
	};
	JDrama::TViewObj* ind = JDrama::TNameRefGen::search<JDrama::TViewObj>("インダイレクトシーン");
	dumpVtbl("インダイレクトシーン", ind);
	// インダイレクトシーン is a TViewObjPtrListT<TViewObj> (vtable=TViewObjPtrListT). Walk its
	// children so we can see WHAT the indirect scene contains (the reflective-sea model?).
	if (ind) {
		auto* lst = static_cast<JDrama::TViewObjPtrListT<JDrama::TViewObj>*>(ind);
		int ci = 0;
		for (auto it = lst->getChildren().begin(); it != lst->getChildren().end(); ++it, ++ci) {
			JDrama::TViewObj* c = *it;
			void** cvt = c ? *(void***)c : nullptr;
			Dl_info di; const char* cls = "?";
			// resolve via nm-style: print the vtable ptr; class named offline
			std::fprintf(stderr, "[ind-dbg]   child[%d]=%p name='%s' vtbl=%p\n",
			             ci, (void*)c, c && c->getName() ? c->getName() : "?", (void*)cvt);
			(void)di; (void)cls;
		}
		std::fprintf(stderr, "[ind-dbg]   インダイレクトシーン child_count=%d\n", ci);
	}
	JDrama::TViewObj* seaI = JDrama::TNameRefGen::search<JDrama::TViewObj>("SeaIndirect");
	dumpVtbl("SeaIndirect", seaI);
	JDrama::TViewObj* refP = JDrama::TNameRefGen::search<JDrama::TViewObj>("ReflectParts");
	dumpVtbl("ReflectParts", refP);
	JDrama::TViewObj* refS = JDrama::TNameRefGen::search<JDrama::TViewObj>("ReflectSky");
	dumpVtbl("ReflectSky", refS);
	JDrama::TViewObj* scr = JDrama::TNameRefGen::search<JDrama::TViewObj>("スクリーンテクスチャ");
	dumpVtbl("スクリーンテクスチャ", scr);
}

// SB_MAP_MODEL_DBG=1: one-shot probe of gpMap's TMapModelManager — how many TOP-LEVEL joint-model
// .bmd files the option map is built from (mJointModelNum, each independently entered/drawn by
// TJointModelManager::perform), and per-file shape count + standing/hidden flag. If native only
// loaded/registered a SUBSET of the real file count, that alone explains a severe scene under-draw
// (each joint-model file is a wholly separate .bmd, not just a sub-mesh of one file).
static void sb_map_model_probe() {
	if (!gpMap) { std::fprintf(stderr, "[map-model-dbg] gpMap = NULL\n"); return; }
	TMapModelManager* mgr = gpMap->getModelManager();
	if (!mgr) { std::fprintf(stderr, "[map-model-dbg] gpMap->mModelManager = NULL\n"); return; }
	int n = mgr->getJointModelNum();
	std::fprintf(stderr, "[map-model-dbg] TMapModelManager jointModelNum=%d folder=%s\n",
	             n, mgr->getFolder() ? mgr->getFolder() : "?");
	for (int i = 0; i < n; ++i) {
		TJointModel* jm = mgr->getJointModel(i);
		if (!jm) { std::fprintf(stderr, "[map-model-dbg]   [%d] = NULL\n", i); continue; }
		J3DModelData* md = jm->getModelData();
		std::fprintf(stderr,
			"[map-model-dbg]   [%d] shapeNum=%d childrenNum=%d hidden(flag1)=%d modelData=%p matNum=%d\n",
			i, jm->getShapeNum(), jm->getChildrenNum(), (int)jm->checkFlag(1),
			(void*)md, md ? (int)md->getMaterialNum() : -1);
		// Recurse the WHOLE within-model joint tree (TJointObj::mChildren) — this is where the actual
		// shapes live (the root often has 0 shapes and just groups sub-joints). Sums total shapes +
		// counts how many joints are hidden (sit, flag bit 1) vs standing, so a truncated draw shows
		// up as either a low total-shape count or a large hidden fraction.
		std::function<void(TJointObj*, int, long&, long&, long&)> walk =
			[&](TJointObj* j, int depth, long& totalShapes, long& hiddenJoints, long& totalJoints) {
				if (!j || depth > 12) return;
				++totalJoints;
				bool hidden = j->checkFlag(1);
				if (hidden) ++hiddenJoints;
				totalShapes += j->getShapeNum();
				if (depth <= 2)
					std::fprintf(stderr, "[map-model-dbg]     %*sjoint shapes=%d children=%d hidden=%d\n",
					             depth * 2, "", j->getShapeNum(), j->getChildrenNum(), (int)hidden);
				// Per-shape raw GX display-list BYTE SIZE (summed over all matrix-group draws) — the
				// real geometry payload, independent of any decode/entry bug. A shape with a
				// plausible byte count but few tris drawn would point at a display-list DECODE
				// truncation, not a missing-shape problem.
				for (int s = 0; s < j->getShapeNum(); ++s) {
					J3DShape* sh = j->getShape(s);
					if (!sh) continue;
					u32 total = 0;
					for (u32 g = 0; g < sh->getMtxGroupNum(); ++g) {
						J3DShapeDraw* d = sh->getShapeDraw((u16)g);
						if (d) total += d->getDisplayListSize();
					}
					std::fprintf(stderr, "[map-model-dbg]       shape[%d] mtxGroups=%u displayListBytes=%u\n",
					             s, sh->getMtxGroupNum(), total);
				}
				for (int c = 0; c < j->getChildrenNum(); ++c)
					walk(j->getChild(c), depth + 1, totalShapes, hiddenJoints, totalJoints);
			};
		long totalShapes = 0, hiddenJoints = 0, totalJoints = 0;
		walk(jm, 0, totalShapes, hiddenJoints, totalJoints);
		std::fprintf(stderr, "[map-model-dbg]   [%d] TREE totalJoints=%ld totalShapes=%ld hiddenJoints=%ld\n",
		             i, totalJoints, totalShapes, hiddenJoints);
	}
}

// SB_PLAYERGROUP_DBG=1: one-shot probe of "プレーヤーグループ" (entered at MarDirectorPreEntry.cpp:73,
// flag 0x204 — the SAME faithful master-perform-list path that correctly enters map.bmd). This is
// almost certainly where the option-scene's "display Mario" (the standalone posed figure distinct
// from gpMarioOriginal, per drive_chr's comment above) gets entered. Names its children + walks any
// MActor/J3DModel found to report shape/material counts, so a truncated display-Mario draw shows up
// as a low shape count vs the model's own modelData.
static void sb_playergroup_probe() {
	JDrama::TViewObj* grp = JDrama::TNameRefGen::search<JDrama::TViewObj>("プレーヤーグループ");
	if (!grp) { std::fprintf(stderr, "[playergroup-dbg] プレーヤーグループ = NULL\n"); return; }
	void** vt = *(void***)grp;
	Dl_info info; const char* cls = (vt[0] && dladdr(vt[0], &info) && info.dli_sname) ? info.dli_sname : "?";
	std::fprintf(stderr, "[playergroup-dbg] プレーヤーグループ=%p class=%s\n", (void*)grp, cls);
	auto* lst = static_cast<JDrama::TViewObjPtrListT<JDrama::TViewObj>*>(grp);
	int ci = 0;
	for (auto it = lst->getChildren().begin(); it != lst->getChildren().end(); ++it, ++ci) {
		JDrama::TViewObj* c = *it;
		void** cvt = c ? *(void***)c : nullptr;
		Dl_info ci_info; const char* ccls = (cvt && cvt[0] && dladdr(cvt[0], &ci_info) && ci_info.dli_sname) ? ci_info.dli_sname : "?";
		std::fprintf(stderr, "[playergroup-dbg]   child[%d]=%p name='%s' class=%s\n",
		             ci, (void*)c, c && c->getName() ? c->getName() : "?", ccls);
		if (ccls && std::strstr(ccls, "TMario")) {
			TMario* ma = static_cast<TMario*>(c);
			std::fprintf(stderr, "[playergroup-dbg]     this-vs-gpMarioOriginal: %s\n",
			             (void*)ma == (void*)gpMarioOriginal ? "SAME" : "DIFFERENT");
			std::fprintf(stderr, "[playergroup-dbg]     mAnimationId=0x%x\n", (unsigned)ma->mAnimationId);
			if (ma->mModel) {
				J3DModel* bm = ma->mModel->getModel();
				if (bm && bm->getModelData()) {
					int jn = (int)bm->getModelData()->getJointNum();
					std::fprintf(stderr, "[playergroup-dbg]     model=%p shapeNum=%d matNum=%d jointNum=%d\n",
					             (void*)bm, (int)bm->getModelData()->getShapeNum(),
					             (int)bm->getModelData()->getMaterialNum(), jn);
					// Sanity check every joint's world (anm) matrix: NaN/inf or a wildly-scaled
					// translation would explain the visually mangled/twisted Mario seen at the
					// file-select settled picker (screenshot comparison, session 2026-07-01).
					int nanCount = 0, wildCount = 0;
					for (int j = 0; j < jn; ++j) {
						MtxPtr m = bm->getAnmMtx(j);
						if (!m) continue;
						bool nan = false, wild = false;
						for (int r = 0; r < 3 && !nan; ++r)
							for (int c2 = 0; c2 < 4; ++c2)
								if (!std::isfinite(m[r][c2])) { nan = true; break; }
						if (!nan && (std::fabs(m[0][3]) > 100000.f || std::fabs(m[1][3]) > 100000.f
						             || std::fabs(m[2][3]) > 100000.f)) wild = true;
						if (nan) ++nanCount;
						if (wild) ++wildCount;
						if (j < 29 || nan || wild)
							std::fprintf(stderr,
								"[playergroup-dbg]       joint[%d] t=(%.2f,%.2f,%.2f) nan=%d wild=%d\n",
								j, m[0][3], m[1][3], m[2][3], (int)nan, (int)wild);
					}
					std::fprintf(stderr, "[playergroup-dbg]     TOTAL nanJoints=%d wildJoints=%d / %d\n",
					             nanCount, wildCount, jn);
					// getAnmMtx is the raw joint WORLD matrix (checked above, all sane). The actual
					// render/skinning path uses a SEPARATE table (getDrawMtx, filled by
					// J3DModel::viewCalc as viewMtx*anmMtx per mDrawMtxData-mapped joint) — if
					// viewCalc() isn't running (or runs stale/out of order) for this model, getAnmMtx
					// can look perfect while getDrawMtx is garbage/stale. Dump both to compare.
					int dmn = bm->getModelData() ? (int)bm->getModelData()->getDrawMtxNum() : 0;
					std::fprintf(stderr, "[playergroup-dbg]     drawMtxNum=%d\n", dmn);
					for (int i = 0; i < dmn && i < 12; ++i) {
						Mtx& dm = bm->getDrawMtx(i);
						bool nan = false;
						for (int r = 0; r < 3 && !nan; ++r)
							for (int c2 = 0; c2 < 4; ++c2)
								if (!std::isfinite(dm[r][c2])) { nan = true; break; }
						std::fprintf(stderr,
							"[playergroup-dbg]       drawMtx[%d] t=(%.2f,%.2f,%.2f) nan=%d\n",
							i, dm[0][3], dm[1][3], dm[2][3], (int)nan);
					}
				} else {
					std::fprintf(stderr, "[playergroup-dbg]     model=%p (no modelData)\n", (void*)bm);
				}
			} else {
				std::fprintf(stderr, "[playergroup-dbg]     mModel = NULL\n");
			}
		}
	}
	std::fprintf(stderr, "[playergroup-dbg] プレーヤーグループ child_count=%d\n", ci);
}

// SB_MAPGROUP_DBG=1: one-shot probe of "マップグループ" (TIdxGroupObj<THitActor>) — the group
// TMapStaticObj instances (palm/parrot/island/decorative static map objects) register into, entered
// into buffers each frame at MarDirectorPreEntry.cpp:37 (flag 0x204). Names/classes every child so a
// missing decorative object shows up as either absent from this list, or present but not drawing.
static void sb_mapgroup_probe() {
	JDrama::TViewObj* grp = JDrama::TNameRefGen::search<JDrama::TViewObj>("マップグループ");
	if (!grp) { std::fprintf(stderr, "[mapgroup-dbg] マップグループ = NULL\n"); return; }
	auto* lst = static_cast<JDrama::TViewObjPtrListT<THitActor>*>(grp);
	int ci = 0;
	for (auto it = lst->getChildren().begin(); it != lst->getChildren().end(); ++it, ++ci) {
		THitActor* c = *it;
		void** cvt = c ? *(void***)c : nullptr;
		Dl_info info; const char* cls = (cvt && cvt[0] && dladdr(cvt[0], &info) && info.dli_sname) ? info.dli_sname : "?";
		std::fprintf(stderr, "[mapgroup-dbg]   child[%d]=%p name='%s' class=%s\n",
		             ci, (void*)c, c && c->getName() ? c->getName() : "?", cls);
	}
	std::fprintf(stderr, "[mapgroup-dbg] マップグループ child_count=%d\n", ci);
}

extern "C" bool sb_boot_drive_scene() {
	if (const char* e = getenv("SB_IND_DBG"); e && e[0] && e[0] != '0') {
		static int n = 0; if (n < 1 && sb_present_frame() > 250) { ++n; sb_indirect_probe(); }
	}
	if (const char* e = getenv("SB_DRAWBUF_INV"); e && e[0] && e[0] != '0') {
		static int n = 0; if (n < 1 && sb_camera_view_settled()) { ++n; sb_drawbuf_inventory(); }
	}
	if (const char* e = getenv("SB_MAPGROUP_DBG"); e && e[0] && e[0] != '0') {
		static int n = 0; if (n < 1 && sb_camera_view_settled()) { ++n; sb_mapgroup_probe(); }
	}
	if (const char* e = getenv("SB_MAP_MODEL_DBG"); e && e[0] && e[0] != '0') {
		static int n = 0; if (n < 1 && sb_camera_view_settled()) { ++n; sb_map_model_probe(); }
	}
	if (const char* e = getenv("SB_PLAYERGROUP_DBG"); e && e[0] && e[0] != '0') {
		static int n = 0; if (n < 1 && sb_camera_view_settled()) { ++n; sb_playergroup_probe(); }
	}
	// SB_NO_DRIVE_SCENE=1: bisection gate — skip the entire native scene drive so the
	// only draw path is the real GC perform list. Used to attribute draw-buffer cycles.
	if (const char* e = getenv("SB_NO_DRIVE_SCENE"); e && e[0] && e[0] != '0')
		return false;
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

	// SB_FI_TRACE: register the scene's draw buffers so J3DDrawBuffer::frameInit backtraces any
	// reset of them (finding what clears the map mid-conductor-walk).
	sb_fi_watch_register(scene->mDrawBuffers[0], scene->mDrawBuffers[1]);

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

	// OWN THE STAGE-LIGHT LOAD — faithful port of TLightCommon::setLight (the decomp left it
	// an empty stub; LightUtil.cpp). The real game loads the stage's GX lights from ONE
	// Light-Group entry (the sun at index unk24, = Light-Group[0] for the option scene) via
	// setLight, NOT by blasting the whole palette. setLight loads THREE GX lights:
	//   GX_LIGHT0 positional (view-transformed sun) + GX_LIGHT1 effect/specular (gpLightManager,
	//   conditional) + GX_LIGHT2 directional (-normalize(sun) as a specular dir). All share the
	// Light-Group[0] colour (white) → 3 white lights in slots 0/1/2. Decompiled from GMSE01
	// @0x80229a30 / TLightMario @0x80229610; the pure selection math + its tests live in
	// native/render/sms_boot_setlight.h + native/platform/tests/setlight_test.cpp.
	//
	// VALUE-VERIFIED (GX-command-stream oracle, 2026-06-30): stage-15 file-select loads exactly
	// 3 white lights — proving the effect light is ON. The previous code here loaded 8 Light-Group
	// entries into GX_LIGHT0..7 (a pre-oracle guess that diverged 3→8). See memory
	// [[fileselect-lighting-3-vs-8-divergence]]. This must run BEFORE perform(0x8): the j3d capture
	// reads sb_gx_get_lights() during the shape draw.
	//
	// effectOn: setLight gates GX_LIGHT1 on gpLightManager->unk54 && unk55, set by an inlined
	// calcLightBorder we have not located; native's gpLightManager ctor defaults them to 0. The
	// oracle proves the effect light is ON for file-select, and its data (gpLightManager loadAfter)
	// is Light-Group[0] — so we drive it from Light-Group[0] here. SB_NO_EFFECT_LIGHT forces it off
	// (→ 2 lights) for A/B. Refinement: port the calcLightBorder gate to source effectOn live.
	{
		JDrama::TLightAry* la =
		    JDrama::TNameRefGen::search<JDrama::TLightAry>("Light Group");
		if (la && la->mLights && la->mLightCount > 0) {
			const JDrama::TIdxLight& sun = la->mLights[0];  // index unk24 (=0 for option scene)
			GXColor col; GXGetLightColor(&sun.unk24, &col);
			const JGeometry::TVec3<f32>& wp = sun.getPosition();

			sb::SetLightIn lin{};
			lin.lgColor[0]=col.r/255.f; lin.lgColor[1]=col.g/255.f;
			lin.lgColor[2]=col.b/255.f; lin.lgColor[3]=col.a/255.f;
			lin.lgPos[0]=wp.x; lin.lgPos[1]=wp.y; lin.lgPos[2]=wp.z;
			for (int r = 0; r < 3; ++r) for (int c = 0; c < 4; ++c)
				lin.view[r*4+c] = g_graphics.mViewMtx.mMtx[r][c];
			lin.effectOn =
			    !(std::getenv("SB_NO_EFFECT_LIGHT") && std::getenv("SB_NO_EFFECT_LIGHT")[0] != '0');
			// Effect light data = gpLightManager's, which loadAfter fills from Light-Group[0].
			for (int i = 0; i < 3; ++i) lin.effPos[i]   = lin.lgPos[i];
			for (int i = 0; i < 4; ++i) lin.effColor[i] = lin.lgColor[i];

			sb::OutLight lo[3];
			sb::build_stage_lights(lin, lo);
			for (int i = 0; i < 3; ++i) {
				if (!lo[i].present) continue;
				GXLightObj obj{};   // zero unset fields (GXInitSpecularDir leaves pos unwritten)
				if (lo[i].specular) GXInitSpecularDir(&obj, lo[i].pos[0], lo[i].pos[1], lo[i].pos[2]);
				else                GXInitLightPos(&obj, lo[i].pos[0], lo[i].pos[1], lo[i].pos[2]);
				GXColor lc = { (u8)(lo[i].color[0]*255.f+0.5f), (u8)(lo[i].color[1]*255.f+0.5f),
				               (u8)(lo[i].color[2]*255.f+0.5f), (u8)(lo[i].color[3]*255.f+0.5f) };
				GXInitLightColor(&obj, lc);
				GXInitLightAttn(&obj, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);  // GXInitLightObj default
				GXLoadLightObjImm(&obj, (GXLightID)(GX_LIGHT0 << i));
			}
		}
		// Ambient: GXSetChanAmbColor(GX_COLOR0A0, AmbGroup[0]) — faithful to the original.
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
	// CAPTURE ONCE PER PRESENT. The expensive scene/sky/chr walk (J3DShape::draw -> the native
	// capture) only needs to land in the buffer once per shown frame. Under TURBO direct() runs many
	// times per VI present, and the capture also taps the game's own real perform-list draw — so the
	// scene was being walked O(shapes) several times per shown frame (fatal once the NPC population
	// inflates the shape count: a frame never finished). begin_scene returns 1 only on the first
	// drive_scene of each present interval (it resets the buffer + unlocks the capture); end_scene
	// re-locks so every other capture this interval — the redundant repeats AND the always-discarded
	// real-path draws — is skipped. The camera + light setup above still runs every direct() (cheap;
	// keeps the file-select option-camera timer + view matrix current). The present re-arms the next
	// capture. (SB_NO_DRIVE_SCENE returns earlier, never locking, so its real-path-only mode is intact.)
	// SB_OWN_GXLIST: the real GX perform-list render (driven from TMarDirector::direct) is the
	// owned draw path and is captured there (bracketed by begin/end_scene). scene_drive then runs
	// SETUP ONLY (camera/proj/lights above) and skips the hand-driven draw — so the capture holds
	// the full perform-list scene, not the partial hand-wired subset. Must NOT call begin_scene here
	// (it would consume the once-per-present capture arm before the real render runs).
	if (sb_own_gxlist()) {
		// The real master GX perform list ALREADY dispatches TMapObjWave::perform(0x8), so its
		// 1352-vert (+ 26 strips × 96 flush verts = ~3900 raw imm) sea grid is captured through
		// the gx_imm path from within that master render. Calling drive_wave() here as well was
		// a duplicate — value-verified via SB_NO_WAVE bisect: OWN_GXLIST=1 with drive_wave = 26949
		// scene verts vs oracle 22723 (relΔ 0.18 divergence), without drive_wave = 23049 (relΔ 0.01
		// within tolerance). Under OWN_GXLIST=0 the hand-driven perform(0x8) does NOT reach
		// TMapObjWave (hand-wired subset), so drive_wave stays live in that path only.
		// [[fileselect-scene-underdraw-not-overdraw]] — 4226-vert gap resolved 2026-07-02.
		if (dbg()) { static long n=0; if((++n%200)==0||n<=2)
			std::fprintf(stderr, "[scene-drive] SB_OWN_GXLIST: setup-only (real GX perform list owns the draw)\n"); }
		return true;
	}
	// The reflective sea (TMapObjWave) draws via raw immediate-mode GX; the hand-driven scene
	// perform(0x8) does not reach it, so drive it here in the hand-driven path only.
	drive_wave();

	if (sb_boot_capture_begin_scene()) {
		// SB_PASS_VERTS: attribute the captured vertex/batch count to each native sub-pass (sky /
		// scene-perform / chr / hud), so a geometry gap vs the per-pass GX-stream oracle (scene verts
		// oracle 22723 vs native ~6531) can be localized to the pass that's short — likely the scene
		// OBJECTS (palm/parrot/island) that perform(0x8) doesn't enter. Diagnostic-only; once/run.
		const bool passDbg = [](){ const char* e = std::getenv("SB_PASS_VERTS"); return e && e[0] && e[0] != '0'; }();
		static int s_passOnce = 0;
		const bool passLog = passDbg && sb_camera_view_settled() && s_passOnce < 3;
		auto passMark = [&](const char* name, int v0, int b0){
			if (passLog) std::fprintf(stderr, "[pass-verts] %-14s +%6d verts  +%4d batches  (total %d/%d)\n",
			    name, sb_boot_capture_vert_count()-v0, sb_boot_capture_batch_count()-b0,
			    sb_boot_capture_vert_count(), sb_boot_capture_batch_count());
		};
		// Frame starts logically EMPTY: the capture buffer is cleared lazily on the first append of
		// this present (g_consumed), so the live size still holds last frame here — baseline at 0 so
		// the sky pass's delta == its own contribution, not a spurious negative.
		int pv = 0, pb = 0;
		drive_sky();
		passMark("sky", pv, pb); pv = sb_boot_capture_vert_count(); pb = sb_boot_capture_batch_count();

		// NPC CALC PASS (opt-in, WIP — SB_NPC_CALC=1). The conductor's live actors (NPCs) set their
		// base TR matrix from mPosition only in TLiveActor::perform's calc branch (bit 0x2 →
		// calcRootMatrix). On real HW that runs every frame via mPerformListCalcAnim BEFORE the draw
		// list; sms-boot only drives the draw (scene->perform(0x8) = flags 0x20C, no 0x2), so the NPCs
		// are entered+drawn with an unset/identity root matrix → projected to extreme NDC (x[±239008]).
		// Driving the conductor calc here (0x2) sets the matrices BUT TLiveManager::perform(0x2) also
		// runs clipActors → NPCs get clipped out (batches 210→66) and some still project extreme. So a
		// bare calc drive is NOT yet correct — the faithful fix is the full perform-list calc+camera+
		// clip flow (the "own the perform-list dispatch" task). Left opt-in for that next investigation.
		if (gpConductor && getenv("SB_NPC_CALC"))
			gpConductor->perform(0x2, &g_graphics);

		scene->perform(0x8, &g_graphics);
		passMark("scene-perform", pv, pb); pv = sb_boot_capture_vert_count(); pb = sb_boot_capture_batch_count();

		// SB_SCENE_BUF=1: one-shot — count packets entered into the scene's own draw buffers
		// (mDrawBuffers[0]=opa, [1]=xlu) after the children walk. Discriminates "map never
		// entered into the buffer" (entry/child-walk problem) from "buffer populated but draws
		// nothing" (draw/capture problem). Chains persist after perform until the next frameInit.
		if (const char* e = getenv("SB_SCENE_BUF"); e && e[0] && e[0] != '0') {
			static int once = 0;
			if (once < 4) { ++once;
				for (int bi = 0; bi < scene->mDrawBufferCount; ++bi) {
					J3DDrawBuffer* b = scene->mDrawBuffers[bi];
					long heads = 0, packets = 0;
					if (b && b->mBuffer)
						for (u32 s = 0; s < b->mSize; ++s) {
							J3DPacket* p = b->mBuffer[s];
							if (p) ++heads;
							for (; p; p = p->getNextPacket()) ++packets;
						}
					std::fprintf(stderr, "[scene-buf] frame#%d buf[%d]=%p size=%u heads=%ld packets=%ld\n",
					             once, bi, (void*)b, b ? b->mSize : 0, heads, packets);
				}
				// Also dump the scene's direct children (gpConductor, gpLightManager, ...).
				long ci = 0;
				for (auto it = scene->getChildren().begin(); it != scene->getChildren().end(); ++it, ++ci)
					std::fprintf(stderr, "[scene-child] #%ld %s\n", ci, it->getName());
			}
		}

		// File-select A/B/C cubes (TFileLoadBlock) draw into DrawBuf ChrOpa/ChrXlu, which the perform
		// list fills but never draws (the same dropped-draw-bit class as sky/map). drive_chr drives
		// the 3 cubes' models directly (calc → viewCalc → entry → draw). ON by default (SB_NO_DRIVE_CHR
		// opts out). NOTE the old naive マネージャーグループ perform(0x204) re-drew the whole map (b31..b45
		// building-atlas dup) — drive_chr now enters ONLY the 3 file blocks (file-select) plus, in the
		// gameplay map, the player Mario (gpMarioOriginal) via the faithful TMario::calcView/entryModels.
		if (const char* e = getenv("SB_NO_DRIVE_CHR"); !(e && e[0] && e[0] != '0'))
			drive_chr();
		passMark("chr", pv, pb); pv = sb_boot_capture_vert_count(); pb = sb_boot_capture_batch_count();

		// In-game HUD overlay (coin/shine/timer/FLUDD). Drawn LAST so the 2D ortho overlay
		// composites on top of the 3D scene. ON by default; SB_NO_HUD opts out.
		if (const char* e = getenv("SB_NO_HUD"); !(e && e[0] && e[0] != '0'))
			drive_hud();
		passMark("hud", pv, pb);
		if (passLog) ++s_passOnce;

		sb_boot_capture_end_scene();
	}

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
