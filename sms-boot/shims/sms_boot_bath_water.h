// sms_boot_bath_water.h — pure, Dolphin-free unit for the Map/BathWaterManager
// TBathWaterPreprocessor port. Extracts the DISPATCH PREDICATE for the vtable call so a unit
// test can hand-derive its truth from the RE (scratch/decomp_bathwater/801aa5a8.c).
//
// TBathWaterPreprocessor::perform is a thin wrapper that on the DRAW-flag bit dispatches
// TBathWaterManager's renderer->render(graphics, bathtubData, waters, params, count). Every
// pointer in the chain must be non-null for the call to happen; the predicate names those
// gates so the test can walk every combination.
//
// Called by the shipping port (decomp/sms/src/Map/BathWaterManager.cpp) so the test
// validates the real function.

#pragma once
#include <cstdint>

namespace sb {

// True iff TBathWaterPreprocessor::perform should dispatch its renderer->render() call.
// Named gates match the decompile @0x801aa5a8:
//   flags & 8              — the DRAW bit (only fires on draw pass)
//   mgr_present            — this->unk10 (TBathWaterManager*) is non-null
//   bathtub_data_present   — mgr->unk24 (bathtub data base ptr) is non-null
//   renderer_present       — mgr->unk30 (TBathWaterRenderer*) is non-null
//
// A wrong short-circuit here would either dispatch when it shouldn't (crash on null deref)
// or fail to dispatch when it should (silent no-render). Both are named explicitly in the
// tests.
inline bool bath_water_preprocessor_should_dispatch(uint32_t flags,
                                                    bool mgr_present,
                                                    bool bathtub_data_present,
                                                    bool renderer_present) {
    if ((flags & 8u) == 0) return false;
    if (!mgr_present)      return false;
    if (!bathtub_data_present) return false;
    if (!renderer_present) return false;
    return true;
}

}  // namespace sb
