// sms_boot_mamma_mirror.h — pure, Dolphin-free units for the TMammaMirrorMapOperator ports.
// This class runs on Sirena Beach (stage 6 / 7) as an 8-node visibility LOD over the mirror-
// interior terrain: `loadAfter` walks the mirror-terrain J3DModelData's joint tree and for
// each of 8 sibling joints computes a bbox-derived center + horizontal threshold radius, then
// `perform` toggles each joint's visibility based on the distance from the camera to the
// currently-active mirror node (`gp*->mCurrentIdx` = -1 → force all visible).
//
// The two pure pieces spec-derived from RE:
//   - mamma_node_center(min, max): 0.5 * (min + max), one component at a time.
//   - mamma_node_threshold(min_x, max_x, min_z, max_z): the RE clamps
//         max(0.5*(max.x-min.x), 0.5*(max.z-min.z)) + 2000  ≤  3000
//     Note the Y component is ignored (horizontal-only radius) — SDA2 constants at 0x80414354
//     (0.5), 0x80414358 (2000.0), 0x8041435c (3000.0).

#pragma once
#include <cstdint>

namespace sb {

// Half-sum of a min/max pair — the center of the axis span.
inline float mamma_node_center(float axis_min, float axis_max) {
    return 0.5f * (axis_max + axis_min);
}

// Visibility predicate for the operator's per-node hide/show decision (@0x801cf46c).
// Returns true when the joint should be HIDDEN — semantically, "the camera is within the
// node's LOD radius AND closer to this node than to the active mirror's origin," which is
// the RE's `<=` compound condition (inverted vs the `>` in the far/show branch). Extracted
// so a future perform port + this test share the same math. Comparison is on refined
// distances (NR-refined sqrt of the raw dist²), so units match mNodeRadius (linear meters).
inline bool mamma_node_should_be_hidden(float dist_to_node, float node_radius,
                                        float dist_to_mirror) {
    return (dist_to_node <= node_radius) && (dist_to_node <= dist_to_mirror);
}

// Compute the per-node visibility threshold radius. Uses XZ (horizontal) bbox extent only —
// Y is ignored. Result is `min(max(half_x, half_z) + PADDING, CAP)` where PADDING=2000 and
// CAP=3000 are SDA2 constants at -0x2848 and -0x2844 respectively.
inline float mamma_node_threshold(float bbox_min_x, float bbox_max_x,
                                  float bbox_min_z, float bbox_max_z) {
    const float half_x = 0.5f * (bbox_max_x - bbox_min_x);
    const float half_z = 0.5f * (bbox_max_z - bbox_min_z);
    float t = (half_x <= half_z) ? half_z : half_x;   // matches RE branch (fVar4 <= fVar5)
    t += 2000.0f;
    if (t > 3000.0f) t = 3000.0f;
    return t;
}

}  // namespace sb
