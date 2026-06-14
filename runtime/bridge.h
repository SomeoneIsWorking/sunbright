#pragma once
// =============================================================================
// runtime/bridge.h — the native-engine <-> recomp marshalling thunk.
//
// Lets a normal host-C++ function (a port/ source-ported ENGINE function: host
// pointers, host ABI) be dropped in as the native implementation of a guest
// address, callable by recompiled game logic. It adapts the guest PowerPC EABI
// (a CPUState register file) to the host C++ call and back:
//
//   SUNBRIGHT_BRIDGE(0x803xxxxx, &JKRDecomp::orderSync);
//
// is equivalent to a hand-written SUNBRIGHT_OVERRIDE that reads gpr3.. / fpr1..,
// translates pointer args guest->host, calls orderSync, writes the return to
// gpr3, and returns via lr — but generated from the function's signature.
//
// This is the function-call half of the bridge designed in
// docs/native_recomp_bridge.md (used for SERVICE subsystems — those the game
// only CALLS and never field-derefs; object-graph subsystems are out of scope by
// construction). Reverse dispatch (native->recomp) and handle-based identity are
// separate pieces in that doc.
//
// PPC EABI arg passing (the subtle part): integer/pointer args go in r3..r10 and
// float/double args go in f1..f8 as TWO INDEPENDENT sequences — a float does not
// consume a GPR slot, nor an int an FPR slot. Returns: int/ptr in r3, fp in f1.
//
// SCOPE (thin-slice): scalar integer/enum/bool/pointer/float/double args, and
// void / scalar-int / fp returns. Not yet: struct-by-value args, 64-bit-int
// register pairing, varargs, pointer/struct returns (a returned guest pointer
// needs host->guest; a returned host object needs a handle — see the doc).
// static_asserts fire on the unsupported cases so they are caught at the call.
// =============================================================================
#include "cpu_state.h"
#include "overrides.h"

#include <tuple>
#include <type_traits>
#include <utility>

// guest main-RAM address -> host pointer (provided by memory_bridge at runtime;
// stubbed by the unit test). ea==0 must map to nullptr.
extern void* sb_guest_to_host(u32 ea);

// recomp dispatch back to the caller (declared in intrinsics.h in the real build).
extern void call_ppc(CPUState& cpu, u32 address);

namespace sbbridge {

// Pull one argument of type T from the guest EABI banks, advancing the matching
// counter (gpr for int/ptr, fpr for float) — the two advance independently.
template <class T>
T extract_arg(CPUState& cpu, int& gpr, int& fpr) {
	if constexpr (std::is_floating_point_v<T>) {
		return static_cast<T>(cpu.fpr[fpr++].ps0);
	} else if constexpr (std::is_pointer_v<T>) {
		u32 ea = cpu.gpr[gpr++];
		return reinterpret_cast<T>(sb_guest_to_host(ea));
	} else {
		static_assert(std::is_integral_v<T> || std::is_enum_v<T>,
		              "SUNBRIGHT_BRIDGE: unsupported argument type "
		              "(struct-by-value / reference not yet marshalled)");
		return static_cast<T>(cpu.gpr[gpr++]);
	}
}

template <class R, class... Args>
void invoke(R (*fn)(Args...), CPUState& cpu) {
	int gpr = 3, fpr = 1;
	// A braced-init-list is guaranteed left-to-right evaluation, so the args are
	// pulled in declaration order — required for correct EABI slot assignment.
	std::tuple<Args...> args{ extract_arg<Args>(cpu, gpr, fpr)... };
	if constexpr (std::is_void_v<R>) {
		std::apply(fn, args);
	} else if constexpr (std::is_floating_point_v<R>) {
		cpu.fpr[1].ps0 = static_cast<double>(std::apply(fn, args));
	} else {
		static_assert(std::is_integral_v<R> || std::is_enum_v<R>,
		              "SUNBRIGHT_BRIDGE: unsupported return type "
		              "(returned guest pointer needs host->guest; a returned host "
		              "object needs a handle — see docs/native_recomp_bridge.md)");
		cpu.gpr[3] = static_cast<u32>(std::apply(fn, args));
	}
	call_ppc(cpu, cpu.lr);
}

// The registered override: a captureless thunk specialized on the target fn so no
// per-site state is needed (Fn is a compile-time function-pointer constant).
template <auto Fn>
void thunk(CPUState& cpu) { invoke(Fn, cpu); }

}  // namespace sbbridge

#define SB_BRIDGE_CAT2(a, b) a##b
#define SB_BRIDGE_CAT(a, b) SB_BRIDGE_CAT2(a, b)

// Register `fnptr` (a host free/static function) as the native impl of guest
// `addr`. Place in any .cpp linked into the launcher, like SUNBRIGHT_OVERRIDE.
#define SUNBRIGHT_BRIDGE(addr, fnptr)                                          \
	static const bool SB_BRIDGE_CAT(_sbbridge_reg_, __LINE__) =                 \
	    (register_override(static_cast<u32>(addr), &sbbridge::thunk<fnptr>),    \
	     true)
