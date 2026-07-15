// pin_state.h — cross-engine state-pin harness (native side).
//
// SB_PIN_STATE=<path.json> forces the native game camera into the state dumped
// from the Dolphin oracle (fork --dump-state-json), so a native-vs-oracle pixel
// diff is pure render fidelity (no camera/framing confound). The JSON is produced
// by extern/dolphin_fork MainNoGUI --dump-state-json.
//
// NOTE: values must be applied to the NATIVE struct fields BY NAME (the native
// CPolarSubCamera has host LP64 layout, NOT the guest offsets) — so the caller
// (CPolarSubCamera::perform) reads the pinned values here and assigns its own
// unk124/unk148/mUp/mFovy fields.
#pragma once

// If SB_PIN_STATE is active, fills pos/up/tgt (each xyz) + fovy with the oracle
// camera state and returns true; returns false (no-op) otherwise.
bool sb_pin_get_camera(float pos[3], float up[3], float tgt[3], float* fovy);
