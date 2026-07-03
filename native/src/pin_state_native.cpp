// pin_state_native.cpp — populate SbPinGameState from the native-side C++ objects
// (gpApplication / gpMarioOriginal / gpCamera etc.). The symmetric oracle-side
// populator lives in runtime/gx_capture.cpp (reads guest RAM at the SAME field
// offsets, from runtime/pin_state_schema.h).
//
// Zero-verdict on nullptr: fields are optional. pin_diff.py knows to skip fields
// whose `have_*` flag is 0 on either side (with a WARN).

#include "../../runtime/pin_state_schema.h"

#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <Camera/Camera.hpp>
#include <Camera/CameraOption.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>

#include <cstring>

extern "C" int sb_pin_state_populate(SbPinGameState* out) {
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));

    // TApplication: gpApplication is a global instance (not a pointer), always live.
    out->app_state       = (unsigned)gpApplication.mAppState;
    // TGameSequence at +0x12: stage(u8)+scenario(u8)+flags(u16). Pack into u32 for compare.
    // Access via the getter methods since fields are named unk0/unk1 in the header.
    out->next_area_raw   = ((unsigned)gpApplication.mNextArea.getStage()    << 24)
                         | ((unsigned)gpApplication.mNextArea.getScenario() << 16);
    // Flags are private in TGameSequence — pack stage+scenario only. Flags will be captured
    // implicitly via mMovie / mAppState — enough to detect a real state divergence.
    out->movie           = (unsigned)gpApplication.getMovie();
    out->have_app        = 1;

    // gpMarDirector — mostly the pointer + mMap, enough to know the director exists.
    out->mardirector_ptr = (unsigned)(unsigned long)gpMarDirector;
    out->have_mardirector = gpMarDirector ? 1 : 0;

    // gpMarioPos — TVec3<f32>* pointer to Mario's world-space position (Vec3 in Mario struct).
    if (gpMarioPos) {
        out->mario_pos[0] = gpMarioPos->x;
        out->mario_pos[1] = gpMarioPos->y;
        out->mario_pos[2] = gpMarioPos->z;
        out->have_mario_pos = 1;
    }

    // gpMarioOriginal + fields.
    out->mario_ptr = (unsigned)(unsigned long)gpMarioOriginal;
    if (gpMarioOriginal) {
        out->mario_status       = (unsigned)gpMarioOriginal->mStatus;
        out->mario_anim_id      = (unsigned)gpMarioOriginal->mAnimationId;
        out->mario_status_state = (unsigned)gpMarioOriginal->mStatusState;
        out->mario_status_timer = (unsigned)gpMarioOriginal->mStatusTimer;
        out->mario_motion_frame = gpMarioOriginal->getMotionFrameCtrl().getFrame();
        out->have_mario = 1;
    }

    // gpCamera (CPolarSubCamera*) — pose + intro timers via mCurrentParams.
    out->camera_ptr = (unsigned)(unsigned long)gpCamera;
    if (gpCamera) {
        // Use the JSG* virtual accessors — the SAME entry points scene_drive.cpp uses to
        // pull the view for C_MTXLookAt. That's the RENDERED camera pose, which is what
        // matters for parity (and what the oracle-side reads too, since Dolphin's game
        // code hits the same virtual accessors at draw time).
        Vec pos{}, up{}, tgt{};
        gpCamera->JSGGetViewPosition(&pos);
        gpCamera->JSGGetViewUpVector(&up);
        gpCamera->JSGGetViewTargetPosition(&tgt);
        out->camera_pos[0]    = pos.x;   out->camera_pos[1]    = pos.y;   out->camera_pos[2]    = pos.z;
        out->camera_target[0] = tgt.x;   out->camera_target[1] = tgt.y;   out->camera_target[2] = tgt.z;
        out->camera_up[0]     = up.x;    out->camera_up[1]     = up.y;    out->camera_up[2]     = up.z;
        out->camera_mode     = gpCamera->mMode;
        // The JDrama fovy on the camera itself (getFovy() returns TLookAtCamera mFovy @ +0x48).
        // This is the fovy the renderer's C_MTXPerspective uses.
        out->camera_fovy     = gpCamera->getFovy();
        // camera_params_ptr — kept for cross-engine reference but not asserted. Intro timers
        // actually live in gpCameraOption (a separate global), NOT in mCurrentParams.
        out->camera_params_ptr = (unsigned)(unsigned long)gpCamera->mCurrentParams;
        out->have_camera = 1;
    }
    // Intro / pan timers come from gpCameraOption (TCameraOption), NOT gpCamera->mCurrentParams.
    // Confirmed by decomp of ctrlOptionCamera_ @ 0x800322a4 which reads
    // *(short *)(gpCameraOption + 0xA) as mIntroChaseTimer. This is the game-tick anchor for the
    // title/file-select transition — if it diverges, the two engines are at different game states.
    if (gpCameraOption) {
        out->camera_intro_timer     = (int)gpCameraOption->mIntroChaseTimer;
        out->camera_load_pan_frames = (int)gpCameraOption->mLoadPanFrames;
        out->camera_load_pan_timer  = (int)gpCameraOption->mLoadPanTimer;
    }
    return 1;
}
