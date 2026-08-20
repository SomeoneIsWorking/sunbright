// runtime/hx_wipe.cpp — native PC port of the Nintendo "Hx_" wipe/transition middleware
// (the prebuilt library that is NOT in the SMS decomp source). Recovered from the
// 2026-06-21/26 STAGE-A port (commits 8ce2750, 7cabf27) that was dropped as dead weight
// during the 2026-07-07 one-runtime consolidation (d6d15d7) without being re-landed —
// it never actually depended on the deleted Path-B/oracle infrastructure. Replaces the
// no-op Hx_* stubs that used to live in sms-boot/runtime/sdk_stubs.cpp (Hx_StartWipe,
// Hx_ResetWipe, Hx_UpdateWipe, Hx_GetWipeType, Hx_MovieStartSyncEx, Hx_ProvideResource[Ex],
// Hx_RemoveResource — those 8 are the only externally-referenced symbols of the library;
// see decomp/sms/src/GC2D/ScrnFader.cpp and decomp/sms/src/System/MovieDirector.cpp).
//
// Reverse-engineered byte-for-byte from the original DOL (GMSE01) — see
// debug_journal/2026-06-21_session12_moviedir_crash_and_wipe_lib.md for the full RE and
// every source address. Every state write / phase transition / timer here is transcribed
// directly from the disassembly (verified with tools/re/ppcdis.py over scratch/bin/sms.dol):
//   Hx_StartWipe        0x80181fd8
//   Hx_UpdateWipe       0x80181e80
//   Hx_GetWipeType      0x80181fc0   (table2[type])
//   Hx_TimerCountDown   0x80181e58
//   Hx_MovieStartSyncEx 0x8017f6c0
//   Hx_ResetWipe        0x801821c0
//   Hx_ProvideResource  0x80182138 / ...Ex 0x801820e0 / Hx_RemoveResource 0x80182074
//   type-12 m-mark logo callback 0x8017f764 (9-phase jump table @0x803c1464)
//   m-mark stroke point list      @0x803c1320 (embedded verbatim below)
//   table2 in/out bytes           @0x803c12d8 (embedded verbatim below)
//   Hx_Circle (iris wipe, type 1/2) 0x80181ab4
//
// STAGE A (this file): the STATE / TIMER / PHASE machine is ported faithfully so the opening
// movie's wipe (startWipe(12)) advances 0->8 -> state DONE -> the fader reaches FADED_IN, and
// Hx_MovieStartSyncEx opens the THP gate (returns 2) at phase>=6 — exactly when the original
// does. The pure-rendering helpers the callback invokes (Hgx_ReadTexture / Hxs_Logo_* /
// Hxs_PenDraw / Frb2_* / Hx_GxInit / Hx_CameraInit / Hx_MotionSet/Update) are NOT control flow
// (the journal RE confirms every phase advance is driven by Hx_TimerCountDown / the point-list
// markers, NOT by pixel feedback) — they are stubbed no-ops here. STAGE B (TODO) would render
// them through Aurora's GX layer (immediate-mode draws) to show the "SUPER MARIO SUNSHINE"
// logo wipe on screen; the call sites and their (faithfully-computed) args are kept so that
// is a drop-in — nothing here references the deleted render_pc/GX-capture infrastructure.

#include <GC2D/hx_wiper.h>
#include <dolphin/types.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

// ---- env-gated trace (verification harness: "a number moves") ----
bool wipe_dbg() {
	static int v = -1;
	if (v < 0) { const char* e = getenv("SB_WIPE_DBG"); v = (e && *e && *e != '0') ? 1 : 0; }
	return v != 0;
}

// ---------------------------------------------------------------------------
// Global wipe state.  In the original this is one struct at 0x803f43c0 addressed
// by byte offset; here it is plain host memory.  Field names carry the original
// offsets so the port stays auditable against the disassembly.
// ---------------------------------------------------------------------------
struct HxState {
	u32 w        = 0;     // +0x00  display width
	u32 h        = 0;     // +0x04  display height
	u32 halfW    = 0;     // +0x08  w>>1
	u32 halfH    = 0;     // +0x0c  h>>1
	// +0x00..+0x0f also overlap the fade colour read by the (no-op) Frb2 finish; unused here.
	u8  state    = 0;     // +0x10  0 idle, 1 start, 2 run, 3 done
	u8  type     = 0;     // +0x11  wipe type
	u8  wipeType = 0;     // +0x12  table2[type] (in/out byte)
	f32 progress = 0.0f;  // +0x14
	f32 rate     = 0.0f;  // +0x18
	s32 frames   = 0;     // +0x1c
	// +0x20 callback selector: original stores table1[type] (a fn-ptr); we dispatch on `type`.
	u32 initFlag = 0;     // +0x24  set by Hx_ProvideResource
	u32 resFlag  = 0;     // +0x28  set by Hx_ProvideResourceEx (resource loaded)
	u32 resType  = 0;     // +0x34  Hx_ProvideResource's type arg (orig also 0x3300 scratch)
	u32 phase    = 0;     // +0x38  m-mark phase 0..8
	u32 timer    = 0;     // +0x3c  frame countdown (Hx_TimerCountDown floor 0)
	// +0x40.. motion params (no-op here)
};
HxState G;

// ---- SDA scratch globals used by the m-mark callback (r13-relative in the original) ----
struct MMarkPoint { u32 xbits; u32 ybits; s32 marker; };
const MMarkPoint* g_pt   = nullptr;  // -0x6358  cursor into the stroke point list
s32  g_counter           = 0;        // -0x634c  points consumed
f32  g_curX              = 0.0f;     // -0x6354  current pen x
f32  g_curY              = 0.0f;     // -0x6350  current pen y
s32  g_flagThp           = 0;        // -0x6360  THP-gate latch (Hx_MovieStartSyncEx ph>=6)
s32  g_flagSe            = 0;        // -0x635c  title-SE latch (Hx_MovieStartSyncEx ph 2..5)

inline f32 fbits(u32 b) { f32 f; __builtin_memcpy(&f, &b, 4); return f; }

// ---- table2 in/out bytes @0x803c12d8 (types 0..14), verbatim ----
const u8 kTable2[15] = { 0,1,0,1,0,1,0,1,0,1,0,0,1,0,0 };

// ---- m-mark stroke point list @0x803c1320, verbatim (x,y IEEE bits; marker s32) ----
// markers: 0=move(pen up), 1=line, 3/4/8=phase markers, -1=end.
const MMarkPoint kMMark[] = {
	{0x42440000, 0x438a8000, 0},  {0x41a00000, 0x43930000, 1},
	{0x41b00000, 0x436a0000, 1},  {0x42480000, 0x432f0000, 1},
	{0x42dc0000, 0x42e00000, 1},  {0x43000000, 0x42d40000, 1},
	{0x430a0000, 0x42dc0000, 1},  {0x43060000, 0x42e20000, 1},
	{0x43070000, 0x43000000, 1},  {0x42ea0000, 0x43260000, 1},
	{0x42ee0000, 0x43490000, 1},  {0x43010000, 0x43650000, 1},
	{0x430c0000, 0x435b0000, 1},  {0x43150000, 0x432c0000, 1},
	{0x43320000, 0x43090000, 1},  {0x43400000, 0x42e40000, 1},
	{0x43580000, 0x42d00000, 1},  {0x43630000, 0x42da0000, 1},
	{0x43600000, 0x43020000, 1},  {0x43370000, 0x43848000, 3},
	{0x43210000, 0x41900000, 0},  {0x43210000, 0x4190147b, 8},
	{0x430d0000, 0x429e0000, 3},  {0x437f0000, 0x40400000, 0},
	{0x437f0000, 0x4040a3d7, 8},  {0x435c0000, 0x428e0000, 4},
	{0xbf800000, 0xbf800000, -1},
};

// ---------------------------------------------------------------------------
// Pure-rendering helpers the m-mark callback calls.  STAGE A: no-ops.  The args
// are still computed faithfully so STAGE B (TODO: draw through Aurora's GX layer,
// which now exists — this stage predates it) can render them in place.
// ---------------------------------------------------------------------------
inline void Hgx_ReadTexture(const void* /*res*/) {}
inline void Hxs_Logo_ExtraDraw(int /*alpha*/) {}
inline void Hxs_Logo_TexSetup(int /*a*/, int /*b*/, const void* /*scratch*/) {}
inline void Hxs_PenDraw(f32 /*x*/, f32 /*y*/, s32 /*counter*/, const MMarkPoint* /*pt*/) {}
inline void Hxs_Logo_MagDraw(f32 /*a*/, f32 /*b*/, f32 /*c*/) {}
inline void Hx_MotionSet_(f32 /*a*/, f32 /*b*/, f32 /*c*/, f32 /*d*/) {}
inline void Hx_MotionUpdate_() {}
inline void Hx_CameraInit_() {}
inline void Hx_GxInit_(int /*a*/, int /*b*/) {}
inline void Frb2_finish_() {}        // Frb2_InitBlackBox + Frb2_RendBox (no-op)
inline void ReInitializeGX_() {}     // GXDrawDone bracketing also folded away

// ---- Hx_TimerCountDown 0x80181e58: decrement G.timer (floor 0), return new value ----
u32 hx_timer_countdown() {
	if (G.timer != 0) G.timer -= 1;
	return G.timer;
}

// ---------------------------------------------------------------------------
// type-12 m-mark logo callback 0x8017f764.  Transcribed instruction-faithfully:
// the 9-entry jump table @0x803c1464 dispatches on G.phase; blocks fall through /
// goto exactly as the original `b`/`bctr` flow (labels named by source address).
// ---------------------------------------------------------------------------
void mmark_callback() {
	const u32 resFlag = G.resFlag;       // r5 = G[0x28]
	// r29 = G[0x2c] (scratch ptr) and r4 (=G[0x30] or &G[0x80]) are draw args only.
	const u32 phase = G.phase;
	if (phase > 8) return;

	switch (phase) {
	case 0: goto L_f7cc;
	case 1: goto L_f818;
	case 2: goto L_f864;
	case 3: goto L_f8c4;
	case 4: goto L_f930;
	case 5: goto L_f944;
	case 6: goto L_f994;
	case 7: goto L_fab8;
	case 8: goto L_fb28;
	default: return;
	}

L_f7cc:  // phase 0 — init
	if (resFlag == 0) Hgx_ReadTexture(nullptr);
	G.phase   = phase + 1;        // -> 1
	g_pt      = &kMMark[0];       // -0x6358 = 0x803c1320
	G.timer   = 0x100;            // 256
	g_counter = 0;                // -0x634c
	g_flagThp = 1;               // -0x6360
	g_flagSe  = 1;               // -0x635c
	return;

L_f818:  // phase 1 — logo fade in (timer 256, 4 ticks/frame ~= 64 frames)
	{
		u32 t = G.timer;
		if (t > 0xc0) {
			int alpha = static_cast<int>(((0x100u - t) << 2) & 0xfc);
			Hxs_Logo_ExtraDraw(alpha);
		} else {
			Hxs_Logo_ExtraDraw(0xff);
		}
		hx_timer_countdown(); hx_timer_countdown(); hx_timer_countdown();
		u32 r = hx_timer_countdown();
		if (r != 0) return;
		G.phase = G.phase + 1;    // -> 2
		return;
	}

L_f864:  // phase 2 — stroke trace: read marker, handle pen-up move
	{
		s32 marker = g_pt->marker;
		if (marker == -1) { G.phase = 3; goto L_f930; }   // end of strokes
		if (marker == 0) {                                 // pen-up move
			g_curX = fbits(g_pt->xbits);
			g_curY = fbits(g_pt->ybits);
			g_counter += 1;
			g_pt += 1;
		}
		// L_f8ac: timer = current entry's marker; advance phase 2->3 then draw (fallthrough)
		G.timer = static_cast<u32>(g_pt->marker);
		G.phase = G.phase + 1;
	}
	// fallthrough into phase-3 body

L_f8c4:  // phase 3 — draw the pen segment; when its timer expires, latch the point
	Hxs_Logo_ExtraDraw(0xff);
	Hxs_Logo_TexSetup(0xff, 0xff, nullptr);
	Hxs_PenDraw(g_curX, g_curY, g_counter, g_pt);
	if (hx_timer_countdown() != 0) return;
	g_curX = fbits(g_pt->xbits);
	g_curY = fbits(g_pt->ybits);
	g_pt += 1;
	G.phase = 2;                  // back to phase 2 for the next point
	g_counter += 1;
	return;

L_f930:  // phase 4 — one-frame transient: arm timer=5, advance, fall into phase 5
	G.timer = 5;
	G.phase = G.phase + 1;
	// fallthrough into phase-5 body

L_f944:  // phase 5 — short hold, then arm timer=255 and advance
	Hxs_Logo_ExtraDraw(0xff);
	Hxs_Logo_TexSetup(0xff, 0xff, nullptr);
	Hxs_PenDraw(g_curX, g_curY, g_counter, g_pt);
	if (hx_timer_countdown() != 0) return;
	G.timer = 0xff;
	G.phase = G.phase + 1;        // -> 6
	return;

L_f994:  // phase 6 — logo magnify fade (timer 255, 4 ticks/frame); opens THP gate
	{
		u32 t = G.timer;
		if (t >= 0xc0) {
			Hxs_Logo_ExtraDraw(0xff);
			int alpha = static_cast<int>(G.timer & 0xff);
			Hxs_Logo_TexSetup(alpha, alpha, nullptr);
			if (G.timer > 0xf8) Hxs_PenDraw(g_curX, g_curY, g_counter, g_pt);
			else                Hxs_Logo_MagDraw(0, 0, 0);
		} else {  // L_fa20
			int alpha = static_cast<int>(t & 0xff);
			Hxs_Logo_TexSetup(alpha, alpha, nullptr);
			Hxs_Logo_MagDraw(0, 0, 0);
		}
		// L_fa68: 4 ticks/frame
		hx_timer_countdown(); hx_timer_countdown(); hx_timer_countdown();
		if (hx_timer_countdown() != 0) return;
		G.timer = 0x19;           // 25
		Hx_MotionSet_(0, 0, 0, 0);
		G.phase = G.phase + 1;    // -> 7
		return;
	}

L_fab8:  // phase 7 — motion update hold (timer 25)
	Hxs_Logo_TexSetup(0, 0, nullptr);
	Hx_MotionUpdate_();
	Hxs_Logo_MagDraw(0, 0, 0);
	if (hx_timer_countdown() != 0) return;
	G.phase = G.phase + 1;        // -> 8
	return;

L_fb28:  // phase 8 — done
	G.state = 3;
	return;
}

// Pure-rendering helpers Hx_Circle calls (the iris effect).  STAGE A: no-ops — the
// phase/timer/state machine is the only control flow (every phase advance is driven by
// Hx_TimerCountDown, NOT pixel feedback), so the wipe COMPLETES without them; STAGE B
// (TODO) would draw the iris (Hx_FrBufferMorf morphs the captured framebuffer; Hxs1/2_Circle
// draw the expanding rings) through Aurora's GX layer.
inline void Hx_FrBufferMorf_(f32 /*t*/) {}
inline void Hx_SetVFilter_(f32 /*v*/) {}
inline f32  Hx_MotionUpdate_circle() { return 0.0f; }   // returns G_var (render-only)
inline void Hxs1_Circle_(f32 /*r*/) {}
inline void Hxs2_Circle_(f32 /*a*/, f32 /*b*/, int /*alpha*/) {}

// ---------------------------------------------------------------------------
// Hx_Circle 0x80181ab4 — the iris/circle wipe (table1[1] type 1, table1[2] type 2).
// Transcribed from the DOL: G.phase dispatch (phase 0 init / phase 1 run / phase>=2 done).
//   phase 0: Hx_MotionSet(wipeType consts); timer = (wipeType==1 ? 30 : 25); phase->1; FALL
//            INTO phase 1 (the original's init block falls through to the run block).
//   phase 1: render the iris (stubbed); Hx_TimerCountDown; when it reaches 0 -> phase->2 AND
//            G.state = 3 (DONE).  This is the gate that releases the scene-entry transition.
//   phase>=2 (or <0): G.state = 3 (DONE).
// The H / G_var / halfword-counter updates in the original feed ONLY the rendering helpers
// (Hx_FrBufferMorf / Hxs1_Circle / Hxs2_Circle), not the phase advance, so STAGE A omits
// them. Both wipeType 0 (type 2) and 1 (type 1) reach the same timer-countdown gate.
void circle_callback() {
	u32 phase = G.phase;
	if (phase >= 2) {                  // phase 2 (done) or any out-of-range
		G.state = 3;
		return;
	}
	if (phase == 0) {                  // init, then fall into the phase-1 run body
		Hx_MotionSet_(0, 0, 0, 0);     // wipeType-specific motion consts (render-only)
		G.timer = (G.wipeType == 1) ? 0x1e /*30*/ : 0x19 /*25*/;
		G.phase = 1;
	}
	// phase 1 run body (also reached by fall-through from phase 0):
	f32 var = Hx_MotionUpdate_circle();  // render arg only
	Hx_MotionUpdate_();
	Hx_FrBufferMorf_(0.0f);
	Hx_SetVFilter_(0.0f);
	Hxs1_Circle_(var);                 // iris rings (stubbed)
	if (hx_timer_countdown() == 0) {
		G.phase += 1;                  // 1 -> 2
		G.state = 3;                   // DONE — releases the wipe
	}
}

// Hx_Test5 0x8017df74 — table1 callback for types 5 and 6. The substantial
// body tessellates the captured framebuffer into a sinusoidal grid; those GX
// writes are render-only. Its control flow is exact and independent of pixel
// feedback: phase 0 arms a 20-frame timer and falls into phase 1, phase 1
// counts down once per update, and expiry advances to phase 2 and marks DONE.
void test5_callback() {
	if (G.phase != 1) {
		if (G.phase != 0) {
			G.state = 3;
			return;
		}
		G.timer = 0x14;
		G.phase += 1;
	}
	// Retail performs the wave-grid GX draw here before decrementing the timer.
	if (hx_timer_countdown() == 0) {
		G.phase += 1;
		G.state = 3;
	}
}

// Hx_Test2 0x8017ef1c — type-10 Delfino/miss curtain. Its draw helpers consume
// motion values but never feed them back into control. Retail advances through
// four literal countdowns (11, 11, 10, 12) and then marks the wipe complete.
void test2_callback() {
	switch (G.phase) {
	case 0: G.phase = 1; G.timer = 0x0b; return;
	case 1:
		if (hx_timer_countdown() != 0) return;
		G.phase = 2; G.timer = 0x0b; return;
	case 2:
		if (hx_timer_countdown() != 0) return;
		G.phase = 3; G.timer = 0x0a; return;
	case 3:
		if (hx_timer_countdown() != 0) return;
		G.phase = 4; G.timer = 0x0c; return;
	case 4:
		if (hx_timer_countdown() != 0) return;
		G.phase = 5; G.state = 3; return;
	default: G.state = 3; return;
	}
}

// per-type wipe callback dispatch (original: table1[type] fn-ptr stored at G[0x20]).
void run_callback() {
	switch (G.type) {
	case 1:
	case 2:  circle_callback(); break;
	case 5:
	case 6:  test5_callback(); break;
	case 10: test2_callback(); break;
	case 12: mmark_callback(); break;
	default:
		// Only type 12 (the opening-movie m-mark wipe) and type 1/2 (the Delfino scene-entry
		// iris) are on the current boot path. Other wipe types (e.g. type 4 Hx_Test1 at
		// movie-end fade-out) are prebuilt-library callbacks not yet RE'd; they fire only
		// after THP decode lands. Honest non-completion (no faked phase advance) + a
		// one-time warning.
		static u8 warned[256] = {0};
		if (!warned[G.type]) {
			warned[G.type] = 1;
			std::fprintf(stderr, "[hx_wipe] UNIMPLEMENTED wipe type %u — port its callback "
			             "(table1[%u]) before this transition (RE from the DOL).\n",
			             G.type, G.type);
		}
		break;
	}
}

} // namespace

// ===========================================================================
// The 8 externally-referenced Hx_ symbols (extern "C" via hx_wiper.h).
// ===========================================================================

// Hx_ResetWipe 0x801821c0
void Hx_ResetWipe(u32 w, u32 h) {
	G.state    = 0;
	G.w        = w;
	G.h        = h;
	G.halfW    = w >> 1;
	G.halfH    = h >> 1;
	G.initFlag = 0;
	G.resFlag  = 0;
}

// Hx_StartWipe 0x80181fd8
void Hx_StartWipe(int type, int frames) {
	if (G.initFlag == 0) {
		// orig: G[0x2c]=&G[0x80] (scratch base), G[0x34]=0x3300 — both draw-only here.
		G.resType = 0x3300;
	}
	// orig calls Hx_Warning(1) if a wipe is already running (G.state==2) — diagnostic only.
	G.state    = 1;
	G.type     = static_cast<u8>(type);
	G.progress = 0.0f;
	G.frames   = frames;
}

// Hx_GetWipeType 0x80181fc0 — table2[type] (in/out byte).
int Hx_GetWipeType(int type) {
	if (type < 0 || type >= 15) return 0;
	return kTable2[type];
}

// Hx_UpdateWipe 0x80181e80 — drive the active wipe; returns the wipe state (3 == done).
u32 Hx_UpdateWipe(f32 rate) {
	ReInitializeGX_();
	switch (G.state) {
	case 0:  // idle
		return 0;
	case 1:  // start -> latch callback selector + in/out byte, reset phase, fall into run
		G.wipeType = (G.type < 15) ? kTable2[G.type] : 0;
		G.state    = 2;
		G.phase    = 0;
		[[fallthrough]];
	case 2:  // run
		G.rate = rate;
		run_callback();                 // may flip G.state 2->3 at phase 8
		G.progress += rate;
		break;
	case 3:  // done -> finish render (no-op unless wipeType==1)
		if (G.wipeType != 1) { Hx_CameraInit_(); Hx_GxInit_(0, 0); Frb2_finish_(); }
		break;
	default:
		break;
	}
	if (wipe_dbg()) {
		static u32 last = 0xffffffff, lastPhase = 0xffffffff;
		if (G.state != last || G.phase != lastPhase) {
			std::fprintf(stderr, "[hx_wipe] type=%u state=%u phase=%u timer=%u "
			             "thpGate=%d seGate=%d\n",
			             G.type, G.state, G.phase, G.timer, g_flagThp, g_flagSe);
			last = G.state; lastPhase = G.phase;
		}
	}
	return G.state;
}

// Hx_MovieStartSyncEx 0x8017f6c0 — 0 normally; 1 = play title SE (phase 2..5);
// 2 = THP gate open (phase>=6, with the phase-6 timer<=0xc0 guard).
int Hx_MovieStartSyncEx() {
	if (G.type != 12) return 0;
	u32 ph = G.phase;
	if (ph >= 2 && ph <= 5) {          // SE latch
		if (g_flagSe == 0) return 0;
		g_flagSe = 0;
		return 1;
	}
	if (ph < 6) return 0;
	if (g_flagThp == 0) return 0;      // THP latch
	if (ph == 6 && G.timer > 0xc0) return 0;
	g_flagThp = 0;
	return 2;
}

// Hx_ProvideResource 0x80182138
void Hx_ProvideResource(void* /*res*/, int type) {
	G.initFlag = 1;
	G.resType  = static_cast<u32>(type);   // orig also stores res ptr at G[0x2c] (draw-only)
}

// Hx_ProvideResourceEx 0x801820e0
void Hx_ProvideResourceEx(void* /*res*/) {
	G.resFlag = 1;                          // orig also stores res ptr at G[0x30] (draw-only)
}

// Hx_RemoveResource 0x80182074
void Hx_RemoveResource() {
	G.initFlag = 0;
	G.resFlag  = 0;
}
