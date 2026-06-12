// SUPERSEDED (2026-06-12) by water_native.cpp — the water rendering port.
//
// This file was the RE draft for the screen-texture refraction widescreen fix. Its
// findings (seams 0x8022ba74 SMS_GetLightPerspectiveForEffectMtx and 0x8027c12c
// TModelWaterManager::drawRefracAndSpec → scoped 0x8034a17c C_MTXLightPerspective;
// mirror/JPA callers must not be touched) were absorbed into
// docs/decomp/water_rendering.md and implemented in water_native.cpp.
// Intentionally left with no overrides so the two files never double-register on the
// same addresses. (No rm policy — tombstone kept.)
