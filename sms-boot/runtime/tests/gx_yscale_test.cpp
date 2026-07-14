// gx_yscale_test.cpp — unit test from RE for aurora's GXGetNumXfbLines /
// GXGetYScaleFactor (extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp), ported
// 2026-07-14 from the SDK bodies in reference/sms/src/dolphin/gx/GXFrameBuf.c.
//
// Why this matters: JDrama::CalcRenderModeXFBHeight computes every render
// mode's xfbHeight (= GXGetNumXfbLines(efbHeight, GXGetYScaleFactor(...)))
// and viHeight from these. They were silent 0-returning stubs in
// runtime/sdk_stubs.cpp, which zeroed every computed xfbHeight/viHeight
// (surfaced by the VIConfigure fail-fast on 2026-07-14).
//
// Strategy: (1) pin the hand-verified anchor values, including the exact
// numbers observed in the Dolphin title capture (yscale reg 0x100 = unity,
// 448 XFB lines from a 448-line EFB source); (2) sweep the linked aurora
// implementations against an independent verbatim transcription of the SDK
// bodies below. No GPU calls, no ROM — pure math, but linked against the
// real aurora::gx objects (full link set, like jaudio_release_test).

#include <dolphin/gx.h>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); g_fail = 1; } \
	else         { std::fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

// ---- reference transcription (verbatim math from GXFrameBuf.c) ------------
static u32 ref__GXGetNumXfbLines(u32 height, u32 scale)
{
	u32 numLines     = (height - 1) * 0x100;
	u32 actualHeight = (numLines / scale) + 1;
	u32 newScale     = scale;
	if (newScale > 0x80 && newScale < 0x100) {
		while (newScale % 2 == 0)
			newScale /= 2;
		if (height % newScale == 0)
			actualHeight++;
	}
	if (actualHeight > 0x400)
		actualHeight = 0x400;
	return actualHeight;
}

static u16 refGXGetNumXfbLines(u16 efbHeight, float yScale)
{
	u32 scale = (u32)(256.0f / yScale) & 0x1FF;
	return (u16)ref__GXGetNumXfbLines(efbHeight, scale);
}

static f32 refGXGetYScaleFactor(u16 efbHeight, u16 xfbHeight)
{
	u32 height1 = xfbHeight;
	f32 scale1  = (f32)xfbHeight / (f32)efbHeight;
	u32 scale   = (u32)(256.0f / scale1) & 0x1FF;
	u32 height2 = ref__GXGetNumXfbLines(efbHeight, scale);
	while (height2 > xfbHeight) {
		height1--;
		scale1  = (f32)height1 / (f32)efbHeight;
		scale   = (u32)(256.0f / scale1) & 0x1FF;
		height2 = ref__GXGetNumXfbLines(efbHeight, scale);
	}
	f32 scale2 = scale1;
	while (height2 < xfbHeight) {
		scale2 = scale1;
		height1++;
		scale1  = (f32)height1 / (f32)efbHeight;
		scale   = (u32)(256.0f / scale1) & 0x1FF;
		height2 = ref__GXGetNumXfbLines(efbHeight, scale);
	}
	return scale2;
}
// ---------------------------------------------------------------------------

int main()
{
	// Anchors. SMS NTSC render modes: efbHeight=448, video height 448
	// (System/Resolution.cpp) -> unity scale, 448 XFB lines. This matches
	// the captured title display copy (BP 0x4E scale=0x100, 448 lines).
	CHECK(GXGetNumXfbLines(448, 1.0f) == 448, "NumXfbLines(448, 1.0) == 448 (title capture)");
	CHECK(GXGetNumXfbLines(480, 1.0f) == 480, "NumXfbLines(480, 1.0) == 480");
	CHECK(GXGetYScaleFactor(448, 448) == 1.0f, "YScaleFactor(448, 448) == 1.0");
	CHECK(GXGetYScaleFactor(480, 480) == 1.0f, "YScaleFactor(480, 480) == 1.0");

	// The CalcRenderModeXFBHeight flow must reproduce the requested line
	// count for the standard identity modes (this is the exact chain that
	// produced xfbHeight=0/viHeight=0 with the old stubs).
	CHECK(GXGetNumXfbLines(448, GXGetYScaleFactor(448, 448)) == 448,
	      "roundtrip 448 EFB -> 448 XFB");
	CHECK(GXGetNumXfbLines(528, GXGetYScaleFactor(528, 528)) == 528,
	      "roundtrip 528 EFB -> 528 XFB (PAL)");

	// Sweep the linked implementation against the verbatim SDK transcription,
	// over the HARDWARE-VALID y-scale domain only: the scale register is 9
	// bits (256/yScale & 0x1FF), so yScale < ~0.5 wraps it to 0 — PPC's divwu
	// by zero yields garbage silently, x86 traps SIGFPE. Real callers
	// (GXSetDispCopyYScale asserts, JDrama passes xfb ~== efb) never leave
	// [0.5, 2.0]; inputs outside it are undefined on the SDK too.
	for (u16 efb = 224; efb <= 528; efb += 16) {
		const u16 xfbLo = static_cast<u16>(efb / 2 + 16);
		const u16 xfbHi = static_cast<u16>(efb * 2 - 16);
		for (u16 xfb = xfbLo; xfb <= xfbHi && xfb <= 528; xfb += 16) {
			if (xfb < 224)
				continue;
			if (refGXGetYScaleFactor(efb, xfb) != GXGetYScaleFactor(efb, xfb)) {
				std::fprintf(stderr, "FAIL: YScaleFactor(%u, %u) diverges from SDK\n", efb, xfb);
				g_fail = 1;
			}
		}
		// yScale 0.75 .. 2.0: exactly 0.5 is already OUT of domain (256/0.5 =
		// 512 wraps the 9-bit register to 0 -> divide by zero).
		for (int s = 3; s <= 8; ++s) {
			const float yScale = s * 0.25f;
			if (refGXGetNumXfbLines(efb, yScale) != GXGetNumXfbLines(efb, yScale)) {
				std::fprintf(stderr, "FAIL: NumXfbLines(%u, %.2f) diverges from SDK\n", efb, yScale);
				g_fail = 1;
			}
		}
	}
	if (!g_fail)
		std::fprintf(stderr, "ok:   sweep vs SDK transcription (efb/xfb 224..528)\n");

	return g_fail;
}
