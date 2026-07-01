// sms_boot_shadow_gate.h — pure, Dolphin-free, no-GX unit of the MarioUtil/ShadowUtil port.
//
// Extracts the parts of TMBindShadowManager::request/calcVtx and TMBindShadowBody::entryDrawShadow
// that DON'T depend on GX / j3dSys / gpMap. Lets a unit test hand-derive expected values from RE
// (Ghidra decompile in scratch/decomp_shadow/) without a ROM or GPU.
//
// The header intentionally mirrors the semantics of `reference/sms/src/MarioUtil/ShadowUtil.cpp`
// so the test validates the SHIPPING function, not a fork.

#pragma once
#include <cmath>
#include <cstdint>

namespace sb {

// Mirrors TCircleShadowRequest (the fields our port reads).
struct ShadowReq {
    float x, y, z;   // unk0.xyz — world position
    float radX;      // unkC     — X radius
    float radZ;      // unk10    — Z radius
    uint8_t unk1D;   // 0 = grounded (use req.y as-is); non-zero = raycast down
};

struct Footprint {
    float x, y, z;   // ground-projected centre
    float radX, radZ;
    uint8_t alpha;
    bool   visible;  // false = hide this shadow (no ground under the actor)
};

// TMBindShadowManager::request()'s ACCEPT/REJECT gate (see decompile @0x8022ecec + the port).
// Returns true if the request should be appended to mRequests. Faithful to the ported behaviour:
// rejects NaN/Inf on x or z, rejects if the actor is outside the map bounds (isInArea false).
// The distance-to-camera LOD cutoff from the decompile is DELIBERATELY not modelled here — the
// port skips it as a documented residual; the test must match that skip.
inline bool shadow_gate_accept(const ShadowReq& r, bool in_area) {
    if (!std::isfinite(r.x) || !std::isfinite(r.z)) return false;
    if (!in_area) return false;
    return true;
}

// Sentinel returned by TMap::checkGround when no ground exists under the query. Our port treats
// anything strictly > -30000.0f as "valid ground found" (matches the ported ShadowUtil.cpp).
constexpr float kNoGroundSentinel = -32767.0f;

// Ground-project one request → footprint. Callers pass the checkGround result (or the sentinel);
// no gpMap dependency, so this is directly unit-testable. Matches ShadowUtil.cpp::calcVtx:
//   - unk1D == 0 → treat as already grounded, use req.y verbatim
//   - unk1D != 0 → use the raycast result IF > sentinel; else mark hidden
//   - visible footprints get a tiny +0.1f lift (avoid z-fighting with the ground poly)
//   - alpha is a fixed 180 (a documented tunable, refined against oracle later)
inline Footprint shadow_project(const ShadowReq& r, float ground_y_probe) {
    Footprint fp{};
    fp.radX = r.radX;
    fp.radZ = r.radZ;
    fp.alpha = 180;

    float y = r.y;
    if (r.unk1D != 0) {
        if (ground_y_probe > -30000.0f) {
            y = ground_y_probe;
        } else {
            fp.visible = false;
            return fp;
        }
    }
    fp.x = r.x;
    fp.y = y + 0.1f;
    fp.z = r.z;
    fp.visible = true;
    return fp;
}

// TMBindShadowBody::entryDrawShadow's request-construction (position + radius = 40 * mScale,
// unk1D = 1 so calcVtx will raycast down). Directly mirrors ShadowUtil.cpp.
inline ShadowReq shadow_body_make_request(float px, float py, float pz, float scale) {
    ShadowReq r{};
    r.x = px; r.y = py; r.z = pz;
    const float radius = 40.0f * scale;
    r.radX = radius;
    r.radZ = radius;
    r.unk1D = 1;
    return r;
}

}  // namespace sb
