// phase_track.cpp — per-frame TMarDirector::direct perform-list phase tracker.
//
// TMarDirector::direct (reference/sms/src/System/MarDirectorDirect.cpp) dispatches 6 named
// perform lists in a fixed order every frame:
//   1 = unk40             (ENTRY pass)
//   2 = unk38
//   3 = unk3C
//   4 = mPerformListGX    (main GX draw pass)
//   5 = mPerformListSilhouette (only when silhouette/camera-cutout active)
//   6 = mPerformListGXPost (GXPost / display composite)
// It stamps the current phase via sb_boot_capture_set_phase(int) before each list runs. That
// hook used to feed a Path-B capture-buffer pipeline (native/render/sms_boot_j3d_capture.cpp,
// scene_drive.cpp) which owned g_capture_phase and exposed it back out via
// sb_boot_capture_phase(). Both were deleted in the 2026-07-07 one-runtime consolidation, but
// three reference/sms diagnostics still read the phase back out:
//   - J3DDrawBuffer::drawHead  (SB_DBHEAD_DBG / SB_DBHEAD_MAT)
//   - JDRDrawBufObj::perform   (SB_ENTRY_MAT, already guarded against a missing provider)
//   - TMap::perform            (SB_MAPXLU_DBG)
// sb_boot_capture_phase was left declared `__attribute__((weak))` with no definition anywhere
// in sms-boot, so the two unguarded callsites (J3DDrawBuffer.cpp, Map.cpp) called through a
// null weak symbol and crashed. This file restores a REAL provider — no capture buffer, no
// lock, just the plain phase counter the diagnostics were always reading.
static int g_sb_capture_phase = 0;

extern "C" void sb_boot_capture_set_phase(int phase) { g_sb_capture_phase = phase; }
extern "C" int  sb_boot_capture_phase() { return g_sb_capture_phase; }
