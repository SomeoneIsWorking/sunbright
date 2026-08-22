// Native sub-frame pose synthesis for the gameplay camera and Mario.
//
// These two objects do not reach the generic TLiveActor matrix path during a draw-only re-issue.
// This module owns their exact guest layouts, calls the game's own matrix/skeleton functions after
// substituting a pose, and restores every persistent field bit-exactly after presentation.

#include "subframe_pose.hpp"

#include "subframe_guest.hpp"

#include <intrinsics.h>
#include <lucent/log.h>

#include <cmath>
#include <cstdlib>

extern "C" void func_80244800(CPUState&); // TMario::calcAnim(u32, JDrama::TGraphics*)
extern "C" void func_80349f5c(CPUState&); // C_MTXLookAt(Mtx, const Vec*, const Vec*, const Vec*)
extern "C" unsigned VIGetRetraceCount(void);

namespace sb::interp60 {
namespace {

constexpr u32 kPolarCameraPerform = 0x80023004u;
constexpr u32 kMarioWaistRollOffset = 0x3D8;
constexpr u32 kMarioWaistPitchOffset = 0x3DC;
constexpr u32 kMarioScreenPositionOffset = 0x450;
constexpr u32 kPlayerDiffBytes = 0x800;

float g_cameraSeparation = 0.0f;
u32 g_playerObject = 0;
float g_playerDerivedState[5] = {0, 0, 0, 0, 0};
u8 g_playerSnapshot[kPlayerDiffBytes];
bool g_playerSnapped = false;

bool is_polar_camera(u32 object) {
    if (!sb_ram_fast(object))
        return false;
    const u32 vtable = sb_r32(object);
    if (!sb_ram_fast(vtable + 0x20))
        return false;
    return sb_r32(vtable + 8 + 6 * 4) == kPolarCameraPerform;
}

bool player_enabled() {
    static const bool enabled = std::getenv("SBR_INTERP60_PLAYER") != nullptr;
    return enabled;
}

u32 find_player() {
    if (!sb_ram_fast(0x8040E10Cu))
        return 0;
    const u32 position = sb_r32(0x8040E10Cu);
    if (!sb_ram_fast(position))
        return 0;
    const u32 object = position - kPositionOffset;
    if (!sb_ram_fast(object))
        return 0;
    const u32 vtable = sb_r32(object);
    if (vtable < 0x80000000u || !sb_ram_fast(vtable))
        return 0;
    return object;
}

void snapshot_player(u32 object) {
    if (!std::getenv("SBR_INTERP60_PLAYER_DIFF") || !object)
        return;
    for (u32 index = 0; index < kPlayerDiffBytes; ++index)
        g_playerSnapshot[index] = sb_r8(object + index);
    g_playerSnapped = true;
}

void report_player_diff(u32 object) {
    if (!g_playerSnapped || !object)
        return;
    g_playerSnapped = false;
    static int reports = 0;
    if (reports >= 3)
        return;
    ++reports;
    int differences = 0;
    lucent::info("i60pdiff",
                 "=== player object bytes changed by the sub-frame (first 0x{:x} bytes "
                 "only; a leak past that is NOT covered) ===",
                 kPlayerDiffBytes);
    for (u32 index = 0; index < kPlayerDiffBytes; index += 4) {
        u32 before = 0;
        u32 after = 0;
        for (int byte = 0; byte < 4; ++byte) {
            before = (before << 8) | g_playerSnapshot[index + byte];
            after = (after << 8) | sb_r8(object + index + byte);
        }
        if (before == after)
            continue;
        if (++differences > 24) {
            lucent::info("i60pdiff", "  ... more than 24 differing words; stopping");
            break;
        }
        float beforeFloat;
        float afterFloat;
        __builtin_memcpy(&beforeFloat, &before, sizeof beforeFloat);
        __builtin_memcpy(&afterFloat, &after, sizeof afterFloat);
        lucent::info("i60pdiff", "  +0x{:03x}: 0x{:08x} -> 0x{:08x}   (as f32: {:.4f} -> {:.4f})",
                     index, before, after, static_cast<double>(beforeFloat),
                     static_cast<double>(afterFloat));
    }
    if (differences == 0)
        lucent::info("i60pdiff", "  NOTHING changed in this range -- the player leak, if real, "
                                 "is outside it or is not on the player object at all.");
}

} // namespace

// CPolarSubCamera::perform saves eye 0x124 -> 0x13c and target 0x148 -> 0x160 during movement,
// then derives the cached 3x4 view at 0x1ec with C_MTXLookAt. A draw-only sub-frame cannot run
// that movement path, so interpolate the same source vectors and call the same retail matrix
// function. The exact slot-6 identity above prevents these offsets from being used on a merely
// camera-shaped object.
CameraSave apply_camera(CPUState& cpu, u32 object, float alpha, long traceStartPresent) {
    CameraSave save;
    if (!is_polar_camera(object)) {
        static long refusals = 0;
        if (++refusals <= 3 || (refusals % 900) == 0) {
            const u32 vtable = sb_ram_fast(object) ? sb_r32(object) : 0;
            const u32 slot6 = sb_ram_fast(vtable + 0x20) ? sb_r32(vtable + 8 + 6 * 4) : 0;
            char name[48];
            guest_name(object, name, sizeof name);
            lucent::info("i60cam",
                         "camera_apply REFUSED #{}: object 0x{:08x} \"{}\" vptr=0x{:08x} "
                         "slot6=0x{:08x} != CPolarSubCamera::perform 0x{:08x} -- NO camera "
                         "interpolation happened this sub-frame, and every alpha will render the "
                         "same view.",
                         refusals, object, name, vtable, slot6, kPolarCameraPerform);
        }
        return save;
    }

    save.object = object;
    static const bool cameraTrace = std::getenv("SBR_INTERP60_CAMTRACE") != nullptr;
    float viewBefore[3] = {0, 0, 0};
    const bool trace = cameraTrace && static_cast<long>(VIGetRetraceCount()) >= traceStartPresent;
    if (trace) {
        for (int component = 0; component < 3; ++component) {
            viewBefore[component] = guest_f32(object + kPolarViewMatrixOffset +
                                              static_cast<u32>(3 + 4 * component) * 4);
        }
    }
    for (int component = 0; component < 3; ++component) {
        save.eye[component] = guest_f32(object + kPolarEyeOffset + static_cast<u32>(component) * 4);
        save.target[component] =
            guest_f32(object + kPolarTargetOffset + static_cast<u32>(component) * 4);
    }
    for (int index = 0; index < 12; ++index)
        save.view[index] = guest_f32(object + kPolarViewMatrixOffset + static_cast<u32>(index) * 4);

    for (int component = 0; component < 3; ++component) {
        const float previousEye =
            guest_f32(object + kPolarPreviousEyeOffset + static_cast<u32>(component) * 4);
        const float previousTarget =
            guest_f32(object + kPolarPreviousTargetOffset + static_cast<u32>(component) * 4);
        guest_w_f32(object + kPolarEyeOffset + static_cast<u32>(component) * 4,
                    (1.0f - alpha) * previousEye + alpha * save.eye[component]);
        guest_w_f32(object + kPolarTargetOffset + static_cast<u32>(component) * 4,
                    (1.0f - alpha) * previousTarget + alpha * save.target[component]);
    }

    const u32 savedStack = static_cast<u32>(cpu.gpr[1]);
    cpu.gpr[3] = object + kPolarViewMatrixOffset;
    cpu.gpr[4] = object + kPolarEyeOffset;
    cpu.gpr[5] = object + kCameraUpOffset;
    cpu.gpr[6] = object + kPolarTargetOffset;
    func_80349f5c(cpu);
    cpu.gpr[1] = savedStack;

    float squaredSeparation = 0.0f;
    for (int component = 0; component < 3; ++component) {
        const float previousEye =
            guest_f32(object + kPolarPreviousEyeOffset + static_cast<u32>(component) * 4);
        const float difference = save.eye[component] - previousEye;
        squaredSeparation += difference * difference;
    }
    g_cameraSeparation = std::sqrt(squaredSeparation);
    if (trace) {
        static const float fast = [] {
            const char* value = std::getenv("SBR_INTERP60_CAMFAST");
            return value ? static_cast<float>(std::atof(value)) : -1.0f;
        }();
        static int lines = 0;
        const bool report = fast >= 0.0f ? g_cameraSeparation >= fast : lines < 12;
        if (report && lines < 40) {
            ++lines;
            lucent::info("i60cam",
                         "CAMTRACE present {} alpha={:.2f}: cached view t BEFORE=({:.2f},"
                         "{:.2f},{:.2f})  AFTER=({:.2f},{:.2f},{:.2f})  |eye cur-prev|={:.3f}",
                         static_cast<long>(VIGetRetraceCount()), static_cast<double>(alpha),
                         static_cast<double>(viewBefore[0]), static_cast<double>(viewBefore[1]),
                         static_cast<double>(viewBefore[2]),
                         static_cast<double>(guest_f32(object + kPolarViewMatrixOffset + 3 * 4)),
                         static_cast<double>(guest_f32(object + kPolarViewMatrixOffset + 7 * 4)),
                         static_cast<double>(guest_f32(object + kPolarViewMatrixOffset + 11 * 4)),
                         static_cast<double>(g_cameraSeparation));
        }
    }
    static long applications = 0;
    if (++applications <= 3 || (applications % 900) == 0) {
        lucent::info("i60cam",
                     "camera_apply #{} on 0x{:08x} alpha={:.2f}: eye prev=({:.2f},{:.2f},{:.2f})"
                     " cur=({:.2f},{:.2f},{:.2f}) |cur-prev|={:.3f}{}",
                     applications, object, static_cast<double>(alpha),
                     static_cast<double>(guest_f32(object + kPolarPreviousEyeOffset)),
                     static_cast<double>(guest_f32(object + kPolarPreviousEyeOffset + 4)),
                     static_cast<double>(guest_f32(object + kPolarPreviousEyeOffset + 8)),
                     static_cast<double>(save.eye[0]), static_cast<double>(save.eye[1]),
                     static_cast<double>(save.eye[2]), static_cast<double>(g_cameraSeparation),
                     squaredSeparation == 0.0f
                         ? "   <-- prev == cur: the camera did not move this tick, so no "
                           "alpha can change this sub-frame's view"
                         : "");
    }
    return save;
}

void restore_camera(const CameraSave& save) {
    if (!save.object)
        return;
    for (int component = 0; component < 3; ++component) {
        guest_w_f32(save.object + kPolarEyeOffset + static_cast<u32>(component) * 4,
                    save.eye[component]);
        guest_w_f32(save.object + kPolarTargetOffset + static_cast<u32>(component) * 4,
                    save.target[component]);
    }
    for (int index = 0; index < 12; ++index) {
        guest_w_f32(save.object + kPolarViewMatrixOffset + static_cast<u32>(index) * 4,
                    save.view[index]);
    }
}

float camera_separation() {
    return g_cameraSeparation;
}

// TMario does not reach TLiveActor's generic calcRootMatrix branch. Its pose becomes matrices in
// calcAnim(2), which is normally reached from the movement cue that a sub-frame must not replay.
// Call that retail body directly after pose substitution, then preserve the five derived fields it
// mutates persistently: the two waist smoothers and mMarioScreenPos. The optional object snapshot
// names any additional leaked word instead of assuming these five are complete forever.
void apply_player(CPUState& cpu, u32 graphics, bool saveDerivedState) {
    if (!player_enabled())
        return;
    const u32 object = find_player();
    if (!object) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            lucent::info("i60player", "SBR_INTERP60_PLAYER: the player object could not be "
                                      "derived from gpMarioPos -- calcAnim was NOT called and this "
                                      "run tests nothing about the player.");
        }
        return;
    }
    if (saveDerivedState) {
        snapshot_player(object);
        g_playerObject = object;
        g_playerDerivedState[0] = guest_f32(object + kMarioWaistRollOffset);
        g_playerDerivedState[1] = guest_f32(object + kMarioWaistPitchOffset);
        for (int component = 0; component < 3; ++component) {
            g_playerDerivedState[2 + component] =
                guest_f32(object + kMarioScreenPositionOffset + static_cast<u32>(component) * 4);
        }
    }
    const u32 savedStack = static_cast<u32>(cpu.gpr[1]);
    cpu.gpr[3] = object;
    cpu.gpr[4] = 2;
    cpu.gpr[5] = graphics;
    func_80244800(cpu);
    cpu.gpr[1] = savedStack;
    static long applications = 0;
    if (++applications <= 3 || (applications % 900) == 0) {
        lucent::info("i60player",
                     "calcAnim(2) #{} on player 0x{:08x} at pose ({:.2f},{:.2f},{:.2f})",
                     applications, object, static_cast<double>(guest_f32(object + kPositionOffset)),
                     static_cast<double>(guest_f32(object + kPositionOffset + 4)),
                     static_cast<double>(guest_f32(object + kPositionOffset + 8)));
    }
}

void restore_player() {
    if (!g_playerObject)
        return;
    report_player_diff(g_playerObject);
    guest_w_f32(g_playerObject + kMarioWaistRollOffset, g_playerDerivedState[0]);
    guest_w_f32(g_playerObject + kMarioWaistPitchOffset, g_playerDerivedState[1]);
    for (int component = 0; component < 3; ++component) {
        guest_w_f32(g_playerObject + kMarioScreenPositionOffset + static_cast<u32>(component) * 4,
                    g_playerDerivedState[2 + component]);
    }
    g_playerObject = 0;
}

} // namespace sb::interp60
