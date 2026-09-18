// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gcnport/guest_context.h"
#include "gcnport/native_hooks.h"

#include <sunbright/title_adapter/guest_j3d_shape.h>

// What one invocation of the GMSE01 boot diagnostic asks for, and the parser that produces it.
//
// This is a separate owner from the run itself because the two answer different questions. The run
// composes gcnport, installs hooks and reports counters; this decides what the user asked for and
// refuses anything malformed. Keeping them apart is what stops the boot tool from becoming the
// place every new probe's flag, its request field and its installation all land together.

namespace sunbright::gcnport_boot {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// One --dump-guest request: where to look in guest RAM once the run has stopped, and how much.
struct GuestMemoryWindow {
    u32 address = 0;
    u32 words = 0;
};

// The SDK's own `retraceCount` (vi.c), incremented by __VIRetraceHandler and read back by
// VIGetRetraceCount at 0x803504ec, whose single `lwz r3, -0x58f0(r13)` resolves it through SDA1 to
// this address. It advances only when a VI interrupt is both raised by the hardware and dispatched
// into the title's handler, so it measures end-to-end delivery rather than the instant of a sample.
constexpr u32 GUEST_RETRACE_COUNT = 0x8040e8d0;

// One --watch-guest request: a small guest window sampled as the run goes, reported only when its
// contents change.
//
// --dump-guest answers "what does this look like when the run ends", which is the wrong question
// for a state machine. A title that reached its title screen and one that reached it and fell back
// look identical in a final dump, and a run long enough to be interesting produces far too many
// samples to print unconditionally. So: print the first sample, print every change, and print
// nothing in between. A window that never changes says so by producing exactly one line, which is a
// real answer and a different one from a window that was never sampled.
struct GuestWatch {
    u32 address = 0;
    u32 words = 0;

    std::vector<u32> previous;
    u64 samples = 0;
    u64 changes = 0;
};

// One --count-calls request: a guest function whose every entry is counted by a native hook.
//
// A native implementation standing in for a guest function is the seam this whole port is built on,
// and counting entries is the smallest thing that exercises it end to end on the real title: the
// JIT has to plant the guard at that address, dispatch has to reach the callback, and the
// callback's RunOriginalOnce has to hand the original body back to the translated code so the title
// carries on behaving exactly as it did. Zero is a real answer here and is printed as one -- "the
// hook never fired" and "the hook was never installed" are different failures, and a silent counter
// cannot tell them apart, so installation is verified at install time and a zero afterwards means
// the address genuinely was never dispatched.
//
// Installed through gcnport::DolphinRuntimeAdapter rather than Dolphin's raw hook ABI, so the real
// title exercises the same adapter a native override will be mounted on -- the adapter's own test
// drives a synthetic image, and this is the only thing that drives it against GMSE01.
struct CountedCall {
    u32 address = 0;
    u64 entries = 0;

    gcnport::HookResult operator()(gcnport::GuestContext&) {
        entries += 1;
        return gcnport::HookResult::call_original_once();
    }
};

// One --super-call request: the complete native -> original -> native round trip at a guest
// function, on the real title.
//
// --count-calls proves a native hook is reached and hands the body back to the translated code.
// That is the easy half. The half a native override actually needs is this one: run native code,
// call the real guest function as a subroutine, get control back with its result still in the
// register file, and only then decide what the caller sees. Nothing about that is provable from a
// counter, because a counter never looks at what the body did.
//
// Bounded on purpose, in two directions. `max_round_trips` caps how many entries take the
// synchronous path, because that path drives the interpreter through the whole callee and turning
// a function entered a quarter of a million times into an interpreted one would measure the
// diagnostic rather than the title; past the cap the hook goes back to handing the body to the JIT.
// `instruction_budget` caps one call, and exceeding it is a hard fault in gcnport rather than a
// truncated call -- a body that does not return within its budget means the budget or the address
// is wrong, and half an executed function is not a result to carry on from.
struct SuperCall {
    u32 address = 0;
    u64 max_round_trips = 0;
    u32 instruction_budget = 0;

    u64 entries = 0;
    u64 round_trips = 0;
    u64 original_instructions = 0;
    // An average over several calls hides the shape of the answer: a function whose calls all cost
    // the same and one that took a wildly different path on one of them produce the same mean, and
    // only the second is telling you the address or the budget is wrong.
    u32 shortest_original = 0;
    u32 longest_original = 0;
    // Every distinct value the body returned, with how often. The first return value alone answers
    // "did the call work"; it cannot answer "does this function keep telling the title the same
    // thing", which is the question a native override standing in for it has to get right.
    std::map<u32, u64> return_values;
    static constexpr std::size_t MAX_DISTINCT_RETURN_VALUES = 16;
    u64 return_values_not_tracked = 0;

    gcnport::HookResult operator()(gcnport::GuestContext& guest) {
        entries += 1;
        if (round_trips >= max_round_trips) {
            return gcnport::HookResult::call_original_once();
        }

        const gcnport::InterpretedBlock block = guest.call_original(instruction_budget);
        round_trips += 1;
        original_instructions += block.instruction_count;
        if (round_trips == 1 || block.instruction_count < shortest_original) {
            shortest_original = block.instruction_count;
        }
        if (block.instruction_count > longest_original) {
            longest_original = block.instruction_count;
        }
        // r3 is the PowerPC ABI's first return register, so this is the value the guest caller is
        // about to act on -- read by native code, after the original ran, inside the same callback.
        // That ordering is the whole point.
        const u32 returned = guest.general_register(3);
        if (return_values.size() < MAX_DISTINCT_RETURN_VALUES || return_values.contains(returned)) {
            return_values[returned] += 1;
        } else {
            return_values_not_tracked += 1;
        }
        // The body has already run, and PC/NPC were restored around it, so returning to the caller
        // is exactly what the function itself would have done next.
        return gcnport::HookResult::return_to_caller();
    }
};

// A guest address on the command line: 32-bit, hexadecimal, and refused rather than truncated.
// strtoull saturates at ULLONG_MAX on overflow, so errno is the only thing that separates an
// out-of-range argument from a legitimately large one.
[[nodiscard]] bool ParseGuestAddress(const char* text, char** end, u32& address);

// Everything one invocation of this tool asks for, past the image itself. These arrived as
// positional parameters until there were five of them, at which point the call site said nothing
// about which flag each one came from.
struct BootRequest {
    u64 block_budget = 0;
    std::string disc_image_path;
    bool report_counters_on_fault = true;
    std::vector<GuestMemoryWindow> dump_windows;
    std::vector<u32> counted_call_addresses;
    std::vector<SuperCall> super_calls;
    std::vector<GuestWatch> guest_watches;
    std::vector<u32> material_probe_addresses;
    u64 material_probe_reports = 0;
    std::vector<u32> lighting_probe_addresses;
    u64 lighting_probe_reports = 0;
    std::vector<u32> model_probe_addresses;
    u64 model_probe_reports = 0;
    std::vector<u32> shape_probe_addresses;
    u64 shape_probe_reports = 0;
    u32 shape_probe_system = sb::title_adapter::GMSE01_J3D_SYS;
};

// Reads the watched window and reports it if this is the first sample or anything in it moved.
// Returns nothing: a watch that sees no change is not an error and has nothing to say.
// Parses argv past the image path into `request`. Answers false when a flag is unknown or a value
// malformed, having already said which on stderr: a run that quietly used a different budget, or
// quietly mounted no disc, would be indistinguishable from one that genuinely reached a different
// boundary.
[[nodiscard]] bool parse_boot_options(int argc, char** argv, BootRequest& request);

} // namespace sunbright::gcnport_boot
