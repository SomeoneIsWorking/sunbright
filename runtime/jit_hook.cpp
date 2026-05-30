// JIT interception via linker --wrap on JitTrampoline's mangled symbol.
// No Dolphin source is modified.
//
// --wrap=_Z13JitTrampolineR7JitBasej instructs the linker to:
//   - Replace all references to _Z13JitTrampolineR7JitBasej with
//     __wrap__Z13JitTrampolineR7JitBasej  (our hook below)
//   - Rename the original definition to
//     __real__Z13JitTrampolineR7JitBasej  (Dolphin's original code)
//
// extern "C" prevents the compiler from further mangling the __wrap_/__real_ names.

#include "Core/PowerPC/JitCommon/JitBase.h"
#include "sunbright_bridge.h"

extern "C" void __real__Z13JitTrampolineR7JitBasej(JitBase& jit, u32 em_address);

extern "C" void __wrap__Z13JitTrampolineR7JitBasej(JitBase& jit, u32 em_address) {
    if (SunbrightBridge::IsRecompiled(em_address)) {
        SunbrightBridge::Run(em_address);
        return;
    }
    __real__Z13JitTrampolineR7JitBasej(jit, em_address);
}
