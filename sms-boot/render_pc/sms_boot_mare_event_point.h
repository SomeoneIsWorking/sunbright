// sms_boot_mare_event_point.h — pure spec-derived constants for the TMareEventPoint::load
// port (@0x801d77b4). No math to test, but the 4 numeric literals in the RE's initHitActor
// call ARE spec — extract them so a wrong-slot / digit-swap / value-drift regression trips
// a specific unit test rather than silently making trigger volumes the wrong size.
//
// SDA2 references (via tools/dol_sda.py):
//   SDA2[-0x267c] = 0.0    → attack radius AND attack height
//   SDA2[-0x2678] = 300.0  → damage radius (XZ trigger extent)
//   SDA2[-0x2674] = 600.0  → damage height (Y trigger extent)

#pragma once
#include <cstdint>

namespace sb {

// The specific actor-type bit-mask event points advertise so game code can enumerate
// them from a THitActor list. `0x40000236` is the exact 32-bit word the RE encodes as
// `lis r4, 0x4000; ori r4, r4, 0x236`.
constexpr uint32_t kMareEventPointActorType     = 0x40000236u;

// Trigger has no attack cylinder — designers wanted these to be passive detectors.
constexpr float    kMareEventPointAttackRadius  = 0.0f;
constexpr float    kMareEventPointAttackHeight  = 0.0f;

// 300 × 600 cylinder is the docs-authoritative trigger extent; used by all TMareEventPoint
// instances (per-instance placement is via mPosition, size is fixed here at load time).
constexpr float    kMareEventPointDamageRadius  = 300.0f;
constexpr float    kMareEventPointDamageHeight  = 600.0f;

}  // namespace sb
