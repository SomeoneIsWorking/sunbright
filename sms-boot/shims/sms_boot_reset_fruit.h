// sms_boot_reset_fruit.h — pure, Dolphin-free unit for the MoveBG/MapObjBall TResetFruit port.
//
// TResetFruit::perform (@0x801e21d0) is a thin dispatch: it handles a Pinna Park (stage 7) Yoshi-
// touch special case, else delegates to TMapObjGeneral::perform. Only the "should the stage-7
// branch fire?" predicate has pure math worth unit-testing; the rest is virtual dispatch and
// vtable calls the port keeps faithful.
//
// Called by the shipping port (decomp/sms/src/MoveBG/MapObjBall.cpp) so the test validates the
// real function, not a fork.

#pragma once

namespace sb {

// The stage-7 branch fires ONLY when: current stage == 7 AND (state != 6) AND velocity magnitude
// squared <= a rest-threshold. Any of those false → branch skipped (fall through to parent).
// Wrapping this as a predicate makes it unit-testable and names the (subtle) condition — the RE
// has a nested `if !(cond_bail_out)` which is easy to mis-invert.
//
//   stage      = current stage byte (gpMarDirector->unk7C in the RE, r13-0x6048+0x7C)
//   state      = TMapObjBase::mState (short at +0xFC in the RE)
//   vel_sq     = velocity.x*.x + velocity.y*.y + velocity.z*.z
//   threshold  = SDA2 rest threshold ([-0x23F8] in the RE — resolves to a small positive float)
inline bool reset_fruit_should_enter_pinna_park_branch(int stage, int state, float vel_sq, float threshold) {
    if (stage != 7)              return false;   // not Pinna Park
    if (state == 6)              return false;   // taken (fruit already picked up)
    if (vel_sq > threshold)      return false;   // still moving fast → treat as in-flight
    return true;                                 // at rest in Pinna → enter Yoshi-touch machine
}

}  // namespace sb
