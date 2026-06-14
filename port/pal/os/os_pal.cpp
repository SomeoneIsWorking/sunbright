// ===========================================================================
// port/pal/os/os_pal.cpp — portable Platform-Abstraction-Layer implementations
// of the small GameCube OS-kernel + data/instruction-cache SDK functions the
// SMS engine references but that have NO real hardware on a PC.
//
// WHY THIS EXISTS
// ---------------
// A link-surface scan of libsmsport_core.a leaves a handful of GameCube OS SDK
// symbols UNDEFINED. They are not "engine code" we can recompile — they are the
// SDK's thin wrappers over GameCube hardware/firmware that simply has no
// counterpart on an x86-64 / arm64 host. This module supplies portable native
// equivalents with the EXACT GameCube SDK signatures (so the engine links) and
// behaviour faithful to a single-threaded PC port.
//
// All these SDK entry points are declared `extern "C"` by the dolphin/os
// headers, so defining them via those declarations gives them C linkage
// automatically (no name mangling) — a signature mismatch here would break the
// final link, so we include the REAL SDK headers and match them verbatim.
//
// FUNCTION -> MAPPING (rationale per group)
// -----------------------------------------
//   OSGetTime / OSGetTick
//       Host monotonic clock (std::chrono::steady_clock) scaled to the GC
//       time-base tick rate. On real hardware the time base counts at
//       bus_clock / 4. The retail GameCube bus clock is 162 MHz, so the time
//       base runs at 40.5 MHz. OSGetTime returns a 64-bit tick count (OSTime =
//       s64); OSGetTick returns the low 32 bits (OSTick = u32). Monotonic and
//       starts at ~0 at first call. This is the right layer: the engine treats
//       these as a wall-clock/profiling source, not as cycle-exact hardware.
//
//   OSDisableInterrupts / OSEnableInterrupts / OSRestoreInterrupts
//       No-ops returning the previous interrupt-enable BOOL. The PAL is
//       single-threaded for now, so there are no async interrupts to mask;
//       these are used by the engine purely to bracket critical sections. We
//       track a logical "interrupts enabled" flag so the
//       disable -> ... -> restore(prev) contract holds and callers observe the
//       same return values they expect (disable returns the prior state; enable
//       returns the prior state and sets enabled; restore sets to the level
//       passed and returns the prior state).
//
//   OSPanic
//       Fatal: print "file:line msg" to stderr, then abort(). Matches the SDK
//       contract (it never returns).
//
//   OSReport
//       Diagnostic logging -> vfprintf to stderr (OSReport maps to printf in
//       the engine, per project notes).
//
//   OSGetResetSwitchState
//       Return FALSE (0): there is no physical reset button on a PC, and the
//       button is not held.
//
//   DCFlushRange / DCStoreRange / DCInvalidateRange / DCFlushRangeNoSync /
//   DCStoreRangeNoSync / DCZeroRange (zeroes — see note) / DCTouchRange /
//   ICInvalidateRange
//       The host CPU is cache-coherent and there is no separate CPU/GPU memory
//       view to synchronise here, so the range cache-maintenance ops are
//       no-ops. EXCEPTION: DCZeroRange actually zeroes the range on hardware
//       (dcbz over the span) — that is an observable memory effect, not a cache
//       hint, so we honour it with a memset.
//
//   __OSBusClock / __OSCoreClock (NOT defined here)
//       The SDK exposes these as globals. The port's shadow <dolphin/os.h>
//       already provides tentative definitions for them, so defining them here
//       too would be a duplicate-symbol error. OSGetTime/OSGetTick below do NOT
//       depend on them — they scale via the kTimeBaseHz constant (the retail
//       40.5 MHz time-base rate) — so the PAL's timing is correct regardless of
//       what those globals hold. (If the engine later needs the real bus-clock
//       value at runtime, the right home for it is a one-time init that writes
//       __OSBusClock = 162000000, not a tentative-definition collision.)
// ===========================================================================

#include <dolphin/os.h>   // exact GC SDK signatures (all extern "C")

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// GameCube clock constants (retail). Used to scale the host clock into GC
// time-base ticks for OSGetTime/OSGetTick.
//   bus clock   = 162 MHz
//   core clock  = 486 MHz  (3x bus)
//   time base   = bus / 4  = 40.5 MHz   (OSTime/OSTick tick rate)
// (__OSBusClock/__OSCoreClock globals are provided by the shadow <dolphin/os.h>
// — see the header comment above; we deliberately do not redefine them.)
// ---------------------------------------------------------------------------
namespace {
// Time-base frequency = bus clock / 4 = 40.5 MHz.
constexpr long long kTimeBaseHz = 162000000ll / 4;  // 40,500,000

// Logical interrupt-enable state for the single-threaded PAL. Real hardware
// boots with interrupts enabled; the engine's critical sections rely on the
// disable/restore pairing rather than the absolute value, but we still model it
// faithfully so the returned "previous state" is correct.
bool g_interrupts_enabled = true;

// Monotonic host clock, scaled to GC time-base ticks. Anchored at first use so
// the returned tick count starts near zero (the game uses it as a relative
// timer / profiling source).
OSTime host_ticks_now() {
	using clock = std::chrono::steady_clock;
	static const clock::time_point start = clock::now();
	const long long ns =
	    std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start)
	        .count();
	// ticks = ns * kTimeBaseHz / 1e9, done in 128-bit-safe order to avoid
	// overflow for long sessions: ns is well under 2^63 / kTimeBaseHz is small.
	// Use __int128 if available, else split.
#if defined(__SIZEOF_INT128__)
	return static_cast<OSTime>((static_cast<__int128>(ns) * kTimeBaseHz) /
	                           1000000000ll);
#else
	// Split: ticks = sec*Hz + (ns%1e9)*Hz/1e9
	const long long sec  = ns / 1000000000ll;
	const long long frac = ns % 1000000000ll;
	return static_cast<OSTime>(sec * kTimeBaseHz +
	                           (frac * kTimeBaseHz) / 1000000000ll);
#endif
}
}  // namespace

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
extern "C" OSTime OSGetTime(void) {
	return host_ticks_now();
}

extern "C" OSTick OSGetTick(void) {
	// OSTick is u32: the low 32 bits of the time base, as on hardware.
	return static_cast<OSTick>(static_cast<u64>(host_ticks_now()) & 0xFFFFFFFFu);
}

// ---------------------------------------------------------------------------
// Interrupts (single-threaded PAL: track logical state, no real masking)
// ---------------------------------------------------------------------------
extern "C" BOOL OSDisableInterrupts(void) {
	const BOOL prev = g_interrupts_enabled ? TRUE : FALSE;
	g_interrupts_enabled = false;
	return prev;
}

extern "C" BOOL OSEnableInterrupts(void) {
	const BOOL prev = g_interrupts_enabled ? TRUE : FALSE;
	g_interrupts_enabled = true;
	return prev;
}

extern "C" BOOL OSRestoreInterrupts(BOOL level) {
	const BOOL prev = g_interrupts_enabled ? TRUE : FALSE;
	g_interrupts_enabled = (level != FALSE);
	return prev;
}

// ---------------------------------------------------------------------------
// Diagnostics / fatal
// ---------------------------------------------------------------------------
extern "C" void OSReport(const char* msg, ...) {
	va_list ap;
	va_start(ap, msg);
	std::vfprintf(stderr, msg, ap);
	va_end(ap);
}

extern "C" void OSPanic(const char* file, int line, const char* msg, ...) {
	std::fprintf(stderr, "OSPanic: %s:%d ", file ? file : "<null>", line);
	va_list ap;
	va_start(ap, msg);
	std::vfprintf(stderr, msg, ap);
	va_end(ap);
	std::fputc('\n', stderr);
	std::fflush(stderr);
	std::abort();  // OSPanic never returns
}

// ---------------------------------------------------------------------------
// Reset switch (no physical button on PC)
// ---------------------------------------------------------------------------
extern "C" BOOL OSGetResetSwitchState(void) {
	return FALSE;
}

// ---------------------------------------------------------------------------
// Data / instruction cache maintenance.
// Host is cache-coherent with no separate CPU/GPU memory view to sync, so the
// range maintenance ops are no-ops. DCZeroRange is the one with an OBSERVABLE
// memory effect (it zeroes the span on hardware), so it is honoured.
// ---------------------------------------------------------------------------
extern "C" void DCInvalidateRange(void* /*addr*/, u32 /*nBytes*/) {}
extern "C" void DCFlushRange(void* /*addr*/, u32 /*nBytes*/) {}
extern "C" void DCStoreRange(void* /*addr*/, u32 /*nBytes*/) {}
extern "C" void DCFlushRangeNoSync(void* /*addr*/, u32 /*nBytes*/) {}
extern "C" void DCStoreRangeNoSync(void* /*addr*/, u32 /*nBytes*/) {}
extern "C" void DCTouchRange(void* /*addr*/, u32 /*nBytes*/) {}
extern "C" void ICInvalidateRange(void* /*addr*/, u32 /*nBytes*/) {}

extern "C" void DCZeroRange(void* addr, u32 nBytes) {
	// dcbz over the span zeroes it on hardware — a real memory effect.
	if (addr && nBytes) std::memset(addr, 0, nBytes);
}
