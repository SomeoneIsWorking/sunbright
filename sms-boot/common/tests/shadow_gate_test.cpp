// shadow_gate_test — spec-derived unit test for the MarioUtil/ShadowUtil port (sms cfdb5e4,
// sunbright f88fdbe). Pure logic, Dolphin-free / no ROM / no GPU / no j3dSys / no gpMap.
//
// Expected values are HAND-DERIVED from:
//   - scratch/decomp_shadow/8022ecec.c  (TMBindShadowManager::request  — gate logic)
//   - scratch/decomp_shadow/8022e0cc.c  (TMBindShadowManager::calcVtx  — ground projection)
//   - reference/sms/src/Strategic/liveactor.cpp:307 (TLiveActor::requestShadow — field semantics)
// and from the port's own documented deviations (see sms_boot_shadow_gate.h + ShadowUtil.cpp
// comments). The pure helpers below are the SAME functions the shipping code calls (ShadowUtil.cpp
// routes request()/calcVtx()/entryDrawShadow() through them), so a failure here is a real port bug.
//
// Sensitivity: each test also asserts what a WRONG constant/branch WOULD do, so a future change
// can't quietly regress — a "green because I changed the expected value" pattern shows up loud.

#include "sms_boot_shadow_gate.h"
#include <cstdio>
#include <cmath>
#include <limits>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); ++g_fail; } } while (0)
#define CHECK_EQ_F(a, b, tol, msg) do { \
    float _a=(a), _b=(b); if (std::fabs(_a - _b) > (tol)) { \
        std::fprintf(stderr, "FAIL: %s: %.6f vs %.6f (%s:%d)\n", msg, _a, _b, __FILE__, __LINE__); \
        ++g_fail; } } while (0)

// ─── 1. request()'s gate ─────────────────────────────────────────────────────
static void test_gate_accepts_ordinary_request() {
    // A grounded Mario-like request at world (100, 20, -50) inside the map area — SHOULD accept.
    sb::ShadowReq r{100.0f, 20.0f, -50.0f, 40.0f, 40.0f, 0};
    CHECK(sb::shadow_gate_accept(r, /*in_area=*/true) == true,
          "ordinary in-area request is accepted");
}

static void test_gate_rejects_out_of_area() {
    // The decompile @0x8022ecec branches on isInArea(x,z). Our port matches: rejected.
    sb::ShadowReq r{100.0f, 20.0f, -50.0f, 40.0f, 40.0f, 0};
    CHECK(sb::shadow_gate_accept(r, /*in_area=*/false) == false,
          "out-of-area request is rejected");
}

static void test_gate_rejects_NaN_x() {
    // The decompile's exponent-bit-twiddle for x rejects NaN (exp bits all set + non-zero mantissa).
    // Our port collapses that to std::isfinite(x); equivalent under IEEE-754.
    sb::ShadowReq r{std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 40.0f, 40.0f, 0};
    CHECK(sb::shadow_gate_accept(r, /*in_area=*/true) == false,
          "NaN x is rejected (isfinite gate)");
}

static void test_gate_rejects_NaN_z() {
    // Same for z (the decomp checks BOTH; the port's isfinite gate does too).
    sb::ShadowReq r{0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN(), 40.0f, 40.0f, 0};
    CHECK(sb::shadow_gate_accept(r, /*in_area=*/true) == false,
          "NaN z is rejected");
}

static void test_gate_rejects_Inf_x() {
    sb::ShadowReq r{std::numeric_limits<float>::infinity(), 0.0f, 0.0f, 40.0f, 40.0f, 0};
    CHECK(sb::shadow_gate_accept(r, /*in_area=*/true) == false, "+Inf x is rejected");
}

static void test_gate_does_NOT_reject_on_y() {
    // Sensitivity: the RE checks only x AND z for finite, NOT y. If a future change accidentally
    // widens the gate to include y (or narrows it), THIS test flags it. NaN y with finite x/z
    // MUST still be accepted (y is used as-is for grounded requests, or overridden by checkGround
    // for non-grounded ones — either way, not gated on).
    sb::ShadowReq r{0.0f, std::numeric_limits<float>::quiet_NaN(), 0.0f, 40.0f, 40.0f, 0};
    CHECK(sb::shadow_gate_accept(r, /*in_area=*/true) == true,
          "NaN y is NOT rejected (gate is x/z only, per the decompile)");
}

// ─── 2. calcVtx()'s ground projection ────────────────────────────────────────
static void test_project_grounded_uses_req_y_unchanged_plus_lift() {
    // unk1D == 0 (grounded flag from the RE / TLiveActor::requestShadow default when standing on
    // the ground): calcVtx MUST use req.y directly, NOT raycast. The +0.1f is the port's tiny
    // z-fight lift.
    sb::ShadowReq r{100.0f, 20.0f, -50.0f, 40.0f, 40.0f, /*grounded=*/0};
    sb::Footprint fp = sb::shadow_project(r, /*ground_y_probe (unused)=*/-999999.0f);
    CHECK(fp.visible, "grounded footprint is visible");
    CHECK_EQ_F(fp.y, 20.0f + 0.1f, 1e-6f, "grounded uses req.y + 0.1 z-fight lift");
    CHECK_EQ_F(fp.x, 100.0f, 0.0f, "footprint.x = req.x");
    CHECK_EQ_F(fp.z, -50.0f, 0.0f, "footprint.z = req.z");
    CHECK_EQ_F(fp.radX, 40.0f, 0.0f, "radX passed through");
    CHECK_EQ_F(fp.radZ, 40.0f, 0.0f, "radZ passed through");
    CHECK(fp.alpha == 180, "alpha is the documented fixed 180 (port constant)");
}

static void test_project_non_grounded_uses_ground_probe_when_valid() {
    // unk1D != 0 (Mario airborne or default TLiveActor state): calcVtx MUST use the checkGround
    // result — NOT req.y. This is THE bug that a "just always use req.y" regression would produce.
    sb::ShadowReq r{100.0f, 500.0f, -50.0f, 40.0f, 40.0f, /*not-grounded=*/1};
    sb::Footprint fp = sb::shadow_project(r, /*ground_y_probe=*/12.5f);
    CHECK(fp.visible, "non-grounded with valid ground probe is visible");
    CHECK_EQ_F(fp.y, 12.5f + 0.1f, 1e-6f, "uses ground probe + z-fight lift (NOT req.y=500)");
    // Sensitivity: if a regression collapsed this to req.y, fp.y would be ~500.1 and this fires:
    CHECK(std::fabs(fp.y - 500.0f) > 100.0f,
          "sensitivity: NOT using req.y (which is 500) for airborne actors");
}

static void test_project_non_grounded_hides_when_no_ground_found() {
    // TMap::checkGround returns -32767.0f when nothing lies under the query. The port explicitly
    // hides those (rather than pinning the disc at Y=-32767, which would waste a decal at
    // infinite depth). This is a documented design decision — assert it.
    sb::ShadowReq r{100.0f, 500.0f, -50.0f, 40.0f, 40.0f, /*not-grounded=*/1};
    sb::Footprint fp = sb::shadow_project(r, sb::kNoGroundSentinel);
    CHECK(!fp.visible, "no-ground-under-actor sentinel hides the footprint");
}

static void test_project_non_grounded_ground_barely_valid() {
    // Boundary: the port treats anything strictly > -30000.0f as valid ground (matches the
    // ported ShadowUtil.cpp). -30000.0f itself is NOT valid — verify the boundary side.
    sb::ShadowReq r{0, 100, 0, 40, 40, 1};
    sb::Footprint fp_barely = sb::shadow_project(r, /*probe=*/-29999.9f);
    CHECK(fp_barely.visible, "-29999.9 > -30000.0 so this counts as valid ground");
    sb::Footprint fp_dead   = sb::shadow_project(r, /*probe=*/-30000.0f);
    CHECK(!fp_dead.visible, "-30000.0 is NOT valid (strict inequality)");
}

// ─── 3. TMBindShadowBody::entryDrawShadow's request construction ─────────────
static void test_body_request_uses_actor_position_and_scaled_radius() {
    // A body at world (10, 5, -3) with mScale=1.0 (Mario's ctor value @MarioInit.cpp:429):
    //   radius = 40.0f * mScale = 40.0f
    //   unk1D  = 1 (documented: let calcVtx raycast down; the actor position's Y isn't the ground)
    sb::ShadowReq r = sb::shadow_body_make_request(10.0f, 5.0f, -3.0f, 1.0f);
    CHECK_EQ_F(r.x, 10.0f, 0.0f, "request.x = actor.pos.x");
    CHECK_EQ_F(r.y, 5.0f,  0.0f, "request.y = actor.pos.y");
    CHECK_EQ_F(r.z, -3.0f, 0.0f, "request.z = actor.pos.z");
    CHECK_EQ_F(r.radX, 40.0f, 0.0f, "radX = 40 * mScale (scale=1)");
    CHECK_EQ_F(r.radZ, 40.0f, 0.0f, "radZ = 40 * mScale (scale=1)");
    CHECK(r.unk1D == 1, "unk1D=1 forces calcVtx's raycast (NOT trusting actor's Y as ground)");
}

static void test_body_request_scales_radius_by_mScale() {
    // Sensitivity: a smaller actor (mScale=0.5) MUST get radius=20; a bigger one (mScale=2) → 80.
    // If a future change dropped the scale multiplier, radius=40 always and this fires.
    sb::ShadowReq small = sb::shadow_body_make_request(0, 0, 0, 0.5f);
    CHECK_EQ_F(small.radX, 20.0f, 0.0f, "mScale=0.5 → radius=20 (radius scales with mScale)");
    sb::ShadowReq big   = sb::shadow_body_make_request(0, 0, 0, 2.0f);
    CHECK_EQ_F(big.radX,   80.0f, 0.0f, "mScale=2.0 → radius=80");
}

// ─── 4. End-to-end (spec derivation) — request → project → footprint ────────
static void test_end_to_end_body_shadow_settles_on_ground() {
    // Full spec trace: Mario at (846, 0, -1000) at file-select (matches the memory-recorded
    // display-Mario position); mScale=1.0; ground at y=0 (the option-scene floor).
    //   step 1: shadow_body_make_request → r = { (846,0,-1000), rad=40, unk1D=1 }
    //   step 2: request() gate → NaN-free + in-area → accepted
    //   step 3: shadow_project(r, ground=0.0) → footprint {(846, 0.1, -1000), rad=40, alpha=180}
    sb::ShadowReq r = sb::shadow_body_make_request(846.0f, 0.0f, -1000.0f, 1.0f);
    CHECK(sb::shadow_gate_accept(r, /*in_area=*/true), "e2e: request accepted");
    sb::Footprint fp = sb::shadow_project(r, /*ground=*/0.0f);
    CHECK(fp.visible, "e2e: footprint visible");
    CHECK_EQ_F(fp.x, 846.0f, 0.0f, "e2e footprint.x");
    CHECK_EQ_F(fp.y, 0.1f,  1e-6f, "e2e footprint.y = ground(0) + 0.1 lift");
    CHECK_EQ_F(fp.z, -1000.0f, 0.0f, "e2e footprint.z");
    CHECK_EQ_F(fp.radX, 40.0f, 0.0f, "e2e footprint.radX");
    CHECK(fp.alpha == 180, "e2e footprint.alpha");
}

int main() {
    test_gate_accepts_ordinary_request();
    test_gate_rejects_out_of_area();
    test_gate_rejects_NaN_x();
    test_gate_rejects_NaN_z();
    test_gate_rejects_Inf_x();
    test_gate_does_NOT_reject_on_y();

    test_project_grounded_uses_req_y_unchanged_plus_lift();
    test_project_non_grounded_uses_ground_probe_when_valid();
    test_project_non_grounded_hides_when_no_ground_found();
    test_project_non_grounded_ground_barely_valid();

    test_body_request_uses_actor_position_and_scaled_radius();
    test_body_request_scales_radius_by_mScale();

    test_end_to_end_body_shadow_settles_on_ground();

    if (g_fail) { std::fprintf(stderr, "shadow_gate_test: %d FAILURE(S)\n", g_fail); return 1; }
    std::printf("shadow_gate_test: all passed\n");
    return 0;
}
