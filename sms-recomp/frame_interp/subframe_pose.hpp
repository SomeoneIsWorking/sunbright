#pragma once

#include "../overrides/overrides.h"

namespace sb::interp60 {

inline constexpr u32 kPositionOffset = 0x10;
inline constexpr u32 kCameraUpOffset = 0x30;
inline constexpr u32 kPolarEyeOffset = 0x124;
inline constexpr u32 kPolarPreviousEyeOffset = 0x13C;
inline constexpr u32 kPolarTargetOffset = 0x148;
inline constexpr u32 kPolarPreviousTargetOffset = 0x160;
inline constexpr u32 kPolarViewMatrixOffset = 0x1EC;

struct CameraSave {
    u32 object = 0;
    float eye[3] = {0, 0, 0};
    float target[3] = {0, 0, 0};
    float view[12] = {0};
};

CameraSave apply_camera(CPUState& cpu, u32 object, float alpha, long trace_start_present);
void restore_camera(const CameraSave& save);
float camera_separation();

void apply_player(CPUState& cpu, u32 graphics, bool save_derived_state);
void restore_player();

} // namespace sb::interp60
