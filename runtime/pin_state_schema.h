// pin_state_schema.h — shared US GMSE01 guest addresses + field offsets for the
// scene-sync pin fingerprint. Consumed by:
//   • native/render/sms_boot_j3d_capture.cpp (sb_boot_dump_pin_json — reads native
//     C++ objects; only uses the field-offset constants for oracle-side parity).
//   • runtime/gx_capture.cpp                 (sb_gx_capture_frame_boundary pin
//     path — reads guest RAM at these addresses).
//
// Addresses RE'd from:
//   • SDA_BASE (r13) = 0x804141C0 per tools/dol_sda.py
//   • gpMarioOriginal @ r13-0x60d8 per Ghidra decomp of isMario__6TMarioFv @ 0x8024db0c
//   • gpCamera @ r13-0x7118 per tools/render/read_vanilla_camera.py:11-13
//   • gpMarDirector @ r13-0x6048 per tools/dol_sda.py:36
//   • gpMarioPos @ r13-0x60b4 per tools/dol_sda.py:37 (IN-PLACE Vec3f, not a pointer)
//   • gpApplication @ 0x803E9700 per runtime/overrides/fastboot_native.cpp:43
//
// Field offsets from reference/sms/include headers (identical layout across regions
// because the C++ source is regionless — only which region's linker placed the
// globals differs).
#ifndef SUNBRIGHT_PIN_STATE_SCHEMA_H
#define SUNBRIGHT_PIN_STATE_SCHEMA_H

// ── Guest RAM addresses (US GMSE01) ─────────────────────────────────────────
#define SMS_US_GPAPPLICATION     0x803E9700u   // TApplication (embedded, not a ptr)
#define SMS_US_GPMARIOORIGINAL   0x8040E0E8u   // TMario* (pointer slot)
#define SMS_US_GPMARIOPOS        0x8040E10Cu   // TVec3<f32>* (pointer to Mario's world position Vec3)
#define SMS_US_GPCAMERA          0x8040D0A8u   // CPolarSubCamera* (pointer slot)
#define SMS_US_GPMARDIRECTOR     0x8040E178u   // TMarDirector* (pointer slot)
// gpCameraOption at SDA offset -0x70b8, RE'd via decomp of ctrlOptionCamera_ @ 0x800322a4:
//   `pbVar3 = *(byte **)(unaff_r13 + -0x70b8);`
// This is the TCameraOption* where mIntroChaseTimer / mLoadPanTimer / mUpDownPanTimer
// actually live — NOT gpCamera->mCurrentParams (which is TCameraKindParam, different struct).
#define SMS_US_GPCAMERAOPTION    0x8040D108u   // TCameraOption* (pointer slot)

// ── TApplication field offsets ──────────────────────────────────────────────
#define TAPP_OFF_APPSTATE        0x08          // u8
#define TAPP_OFF_NEXTAREA        0x12          // TGameSequence: stage(u8)+scenario(u8)+flags(u16)
#define TAPP_OFF_CURRAREA        0x0E
#define TAPP_OFF_MOVIE           0x18          // u32

// ── TMario field offsets ────────────────────────────────────────────────────
#define TMARIO_OFF_STATUS        0x7C          // u32
#define TMARIO_OFF_STATUSSTATE   0x84          // u16
#define TMARIO_OFF_STATUSTIMER   0x86          // u16
#define TMARIO_OFF_ANIMID        0xFA          // u16

// ── CPolarSubCamera field offsets (JDrama-derived) ──────────────────────────
// Confirmed via decomp of ctrlOptionCamera_ writes at param_1+0x10 (mPos) and +0x3C (mTarget).
// TPlacement mPosition @ +0x10, TLookAtCamera mUp @ +0x30, mTarget @ +0x3C, mFovy @ +0x48.
#define TCAMERA_OFF_POSITION     0x10          // Vec3f (TPlacement)
#define TCAMERA_OFF_UPVEC        0x30          // Vec3f (TLookAtCamera mUp)
#define TCAMERA_OFF_TARGET       0x3C          // Vec3f (TLookAtCamera mTarget)
#define TCAMERA_OFF_FOVY         0x48          // f32   (TLookAtCamera mFovy — same as getFovy())
#define TCAMERA_OFF_MODE         0x50          // int   (CPolarSubCamera mMode)
#define TCAMERA_OFF_CURRPARAMS   0x68          // TCameraKindParam* (NOT where intro timers live)

// TCameraOption field offsets (via SMS_US_GPCAMERAOPTION pointer). Confirmed from header
// reference/sms/include/Camera/CameraOption.hpp AND ctrlOptionCamera_ decomp above.
#define TCAMOPT_OFF_FLAGS        0x00          // u8 (bit2 = title-intro phase, bit1 = cube handoff)
#define TCAMOPT_OFF_FOVY         0x04          // f32 (option-camera fovy — 40° at title)
#define TCAMOPT_OFF_INTROTIMER   0x0A          // s16 (mIntroChaseTimer 300→0 — the ANCHOR)
#define TCAMOPT_OFF_LOADPANFR    0x0C          // s16 (mLoadPanFrames — const 120)
#define TCAMOPT_OFF_LOADPANTIMER 0x0E          // s16 (mLoadPanTimer 120→0)

// ── The pin fingerprint struct populated on BOTH sides ──────────────────────
// Populated by:
//   • native/render/sms_boot_j3d_capture.cpp — reads C++ objects directly.
//   • runtime/gx_capture.cpp                 — reads guest RAM at the addresses above.
// Serialized into pin_NNNN.json / pin_NNNN_oracle.json with the same key names
// so tools/render/pin_diff.py can compare byte-exact per field.
#ifdef __cplusplus
struct SbPinGameState {
    // TApplication (fixed globally at SMS_US_GPAPPLICATION on oracle side; embedded C++
    // global on native side).
    unsigned app_state;      // u8 → widened
    unsigned next_area_raw;  // TGameSequence 4 bytes at +0x12, native-endian widened
    unsigned movie;          // u32
    unsigned char have_app;
    // gpMarDirector (pointer + a field or two we care about)
    unsigned mardirector_ptr;
    unsigned char have_mardirector;
    // gpMarioPos (Vec3f, in-place — no indirection)
    float mario_pos[3];
    unsigned char have_mario_pos;
    // gpMarioOriginal (pointer) and TMario fields
    unsigned mario_ptr;
    unsigned mario_status;
    unsigned mario_anim_id;
    unsigned mario_status_state;
    unsigned mario_status_timer;
    float    mario_motion_frame;   // native-only (getMotionFrameCtrl().getFrame())
    unsigned char have_mario;
    // gpCamera (pointer) and pose + intro timers
    unsigned camera_ptr;
    float camera_pos[3];
    float camera_target[3];
    float camera_up[3];
    int   camera_mode;
    unsigned camera_params_ptr;
    float camera_fovy;
    int   camera_intro_timer;
    int   camera_load_pan_frames;
    int   camera_load_pan_timer;
    unsigned char have_camera;
};
#endif

#endif  // SUNBRIGHT_PIN_STATE_SCHEMA_H
