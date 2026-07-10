// mtx_lookat_test.cpp — verify-first unit test for the GC-convention sign of
// aurora's C_MTXLookAt / C_MTXPerspective (extern/aurora/lib/dolphin/mtx).
//
// Context (debug_journal/2026-07-10_phase_provider_restore_and_behind_camera_finding.md):
// at the stage-15 title every backdrop vertex (Sky/MapOpa/MapXlu) lands at
// POSITIVE camera-space Z, so clip.w = -mv.z goes negative and every vertex is
// discarded as "behind the camera". The GC convention is that the camera looks
// down NEGATIVE view-space Z, so a point in front of the camera must map to
// NEGATIVE view-space Z. This test builds a handful of known camera setups and
// asserts that a point on the look direction maps to view-space Z < 0, using
// aurora's actual C_MTXLookAt (linked from aurora::mtx, not a hand copy).
//
// No GPU / no ROM / pure math.

#include <dolphin/mtx.h>
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
	if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); g_fail = 1; } \
	else         { std::fprintf(stderr, "ok:   %s\n", msg); } \
} while (0)

// Apply a GC 3x4 row-major Mtx (view or model-view) to a point, giving
// view-space (x,y,z). This is exactly the mv[] computation the GX FIFO
// interpreter performs per-vertex (aurora lib/gx/command_processor.cpp).
static void applyMtx(const Mtx m, float x, float y, float z, float* out) {
	out[0] = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
	out[1] = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
	out[2] = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
}

// camPos=(0,0,100), target=(0,0,0), up=(0,1,0): looking down -Z toward the
// origin. The target itself must map to view-space z = -100 (distance in
// front of the camera, negated per GC convention).
static void test_lookat_straight_down_minus_z() {
	Mtx view;
	Point3d camPos = {0.0f, 0.0f, 100.0f};
	Vec up = {0.0f, 1.0f, 0.0f};
	Point3d target = {0.0f, 0.0f, 0.0f};
	C_MTXLookAt(view, &camPos, &up, &target);

	float mv[3];
	applyMtx(view, target.x, target.y, target.z, mv);
	std::fprintf(stderr, "info: target -> view-space (%.3f,%.3f,%.3f)\n", mv[0], mv[1], mv[2]);
	CHECK(std::fabs(mv[0]) < 1e-3f && std::fabs(mv[1]) < 1e-3f, "target maps to (0,0,z) in view space");
	CHECK(mv[2] < 0.0f, "target (in front of camera) has NEGATIVE view-space Z");
	CHECK(std::fabs(mv[2] + 100.0f) < 1e-2f, "target view-space Z == -100 (distance to camera, negated)");

	// The camera itself must map to the view-space origin.
	float camMv[3];
	applyMtx(view, camPos.x, camPos.y, camPos.z, camMv);
	CHECK(std::fabs(camMv[0]) < 1e-3f && std::fabs(camMv[1]) < 1e-3f && std::fabs(camMv[2]) < 1e-3f,
	      "camPos maps to view-space origin");

	// A point BEHIND the camera (further +Z, away from target) must map to
	// POSITIVE view-space Z.
	float behindMv[3];
	applyMtx(view, 0.0f, 0.0f, 200.0f, behindMv);
	CHECK(behindMv[2] > 0.0f, "a point behind the camera has POSITIVE view-space Z");
}

// camPos=(0,0,-50), target=(0,0,-150), up=(0,1,0): camera looking down -Z
// again but offset, to rule out an origin-relative special case.
static void test_lookat_offset_along_minus_z() {
	Mtx view;
	Point3d camPos = {0.0f, 0.0f, -50.0f};
	Vec up = {0.0f, 1.0f, 0.0f};
	Point3d target = {0.0f, 0.0f, -150.0f};
	C_MTXLookAt(view, &camPos, &up, &target);

	float mv[3];
	applyMtx(view, target.x, target.y, target.z, mv);
	std::fprintf(stderr, "info: offset target -> view-space (%.3f,%.3f,%.3f)\n", mv[0], mv[1], mv[2]);
	CHECK(mv[2] < 0.0f, "offset target still maps to NEGATIVE view-space Z");
	CHECK(std::fabs(mv[2] + 100.0f) < 1e-2f, "offset target view-space Z == -100");
}

// camPos looking down +X this time (target = camPos + (1,0,0)) to make sure
// the sign convention isn't accidentally axis-specific.
static void test_lookat_looking_down_x() {
	Mtx view;
	Point3d camPos = {10.0f, 0.0f, 0.0f};
	Vec up = {0.0f, 1.0f, 0.0f};
	Point3d target = {110.0f, 0.0f, 0.0f};
	C_MTXLookAt(view, &camPos, &up, &target);

	float mv[3];
	applyMtx(view, target.x, target.y, target.z, mv);
	std::fprintf(stderr, "info: +X-looking target -> view-space (%.3f,%.3f,%.3f)\n", mv[0], mv[1], mv[2]);
	CHECK(mv[2] < 0.0f, "target along +X view direction still maps to NEGATIVE view-space Z");
	CHECK(std::fabs(mv[2] + 100.0f) < 1e-2f, "+X-looking target view-space Z == -100");
}

// C_MTXPerspective: GC convention puts -1 in the w-row [3][2] so that
// clip.w = -mv.z (aurora command_processor.cpp computes clip[3] via the
// w-row of P concatenated with mv). A point at view-space z=-100 (in front,
// per the above) must produce clip.w > 0 (visible), not <= 0 (discarded).
static void test_perspective_w_row_sign() {
	Mtx44 proj;
	C_MTXPerspective(proj, 60.0f, 1.0f, 10.0f, 10000.0f);
	CHECK(proj[3][2] == -1.0f, "MTXPerspective w-row [3][2] == -1 (GC convention: clip.w = -mv.z)");

	// clip.w = P[3][0]*mv.x + P[3][1]*mv.y + P[3][2]*mv.z + P[3][3]
	float mv[3] = {0.0f, 0.0f, -100.0f};
	float clipW = proj[3][0] * mv[0] + proj[3][1] * mv[1] + proj[3][2] * mv[2] + proj[3][3];
	std::fprintf(stderr, "info: mv.z=-100 -> clip.w=%.3f\n", clipW);
	CHECK(clipW > 0.0f, "a point in front of the camera (mv.z<0) has POSITIVE clip.w (visible)");
}

int main() {
	test_lookat_straight_down_minus_z();
	test_lookat_offset_along_minus_z();
	test_lookat_looking_down_x();
	test_perspective_w_row_sign();
	if (g_fail) {
		std::fprintf(stderr, "MTX LookAt/Perspective sign-convention test: FAILED\n");
		return 1;
	}
	std::fprintf(stderr, "MTX LookAt/Perspective sign-convention test: PASSED\n");
	return 0;
}
