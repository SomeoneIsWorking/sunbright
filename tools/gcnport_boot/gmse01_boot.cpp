// Copyright 2026 Sunbright contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Standalone maintainer tool: attempts an exact GMSE01 boot through gcnport's public
// PowerPC::GcnPort adapter (BootAuthenticatedImage / ExecuteJitBlock), entirely from Sunbright.
// This intentionally never touches gcnport's own repository or tests: gcnport must never link or
// embed a copyrighted game asset, so the GMSE01 image bytes are read from disk here and passed
// across gcnport's public API boundary as an in-memory span, exactly like a title consumer would.
//
// gcnport's BootAuthenticatedImage is scoped to a raw in-memory image plus an explicit load address
// and entry point (see shared/gcnport/docs/dolphin-embedding-contract.md), so this tool boots
// GMSE01's already-extracted main.dol directly: it parses the DOL header, assembles one contiguous
// flat image spanning the DOL's lowest to highest loaded address (gaps zero-filled, matching how
// the loader would place each section), and boots at the DOL's own entry point.
//
// --disc additionally hands gcnport a path to the retail disc image, which mounts the real volume
// behind the title's DVD reads and lets gcnport configure the console from it (region, and
// Dolphin's shipped per-game settings). The title's own asset loads then resolve against the real
// filesystem rather than failing, so this reaches the opening movie and the attract loop beyond it.
// The disc path is a string across gcnport's public API; no game bytes enter gcnport's repository.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <mbedtls/sha256.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"
#include "Common/MsgHandler.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/CoreTiming.h"
#include "Core/HW/DVD/DVDInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/ProcessorInterface.h"
#include "Core/PowerPC/GcnPortRuntime.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"
#include "boot_options.h"
#include "gcnport/dolphin_adapter.h"
#include "guest_frame_renderer.h"
#include "guest_lighting_probe.h"
#include "guest_material_probe.h"
#include "guest_model_probe.h"
#include "guest_projection_probe.h"
#include "guest_report.h"
#include "guest_shape_probe.h"

namespace sunbright::gcnport_boot {
namespace {
// Diagnostic-only: guest code this tool boots may fault on a host page the minimal (non-disc,
// non-apploader) boot path never mapped or initialized -- e.g. a memory access to a region
// InitFastmemArena/HW::Init would ordinarily back. A raw crash would lose the ExecutionCounters
// evidence of what DID run before that point, which is the entire finding this tool exists to
// report. This handler is deliberately narrow: it reads the one live RuntimeSession's counters and
// writes them with async-signal-safe primitives, then re-raises the default handler. It is not a
// recovery mechanism and the process still terminates on the fault.
PowerPC::GcnPort::RuntimeSession* g_diagnostic_runtime = nullptr;

void WriteDecimal(char* buffer, std::size_t buffer_size, unsigned long long value) {
    std::snprintf(buffer, buffer_size, "%llu", value);
}

extern "C" void ReportCountersOnFault(int signal_number) {
    if (g_diagnostic_runtime != nullptr) {
        const auto counters = g_diagnostic_runtime->GetExecutionCounters();
        char line[256];
        char compiled[32];
        char executions[32];
        char cold[32];
        char cache_hit[32];
        WriteDecimal(compiled, sizeof(compiled), counters.jit_blocks_compiled);
        WriteDecimal(executions, sizeof(executions), counters.jit_block_executions);
        WriteDecimal(cold, sizeof(cold), counters.cold_block_executions);
        WriteDecimal(cache_hit, sizeof(cache_hit), counters.cache_hit_block_executions);
        const int length = std::snprintf(
            line, sizeof(line),
            "gmse01_boot: fault before shutdown -- jit_blocks_compiled=%s "
            "jit_block_executions=%s cold_block_executions=%s cache_hit_block_executions=%s\n",
            compiled, executions, cold, cache_hit);
        if (length > 0)
            static_cast<void>(write(STDERR_FILENO, line, static_cast<std::size_t>(length)));
    }
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}

struct DolImage {
    std::vector<u8> flat;
    u32 load_address = 0;
    u32 entry_point = 0;
};

u32 ReadBigEndianU32(const std::vector<u8>& bytes, std::size_t offset) {
    return (static_cast<u32>(bytes[offset]) << 24) | (static_cast<u32>(bytes[offset + 1]) << 16) |
           (static_cast<u32>(bytes[offset + 2]) << 8) | static_cast<u32>(bytes[offset + 3]);
}

// Parses a raw GameCube DOL (the format Sunbright's own tooling already extracts from the retail
// disc) into one contiguous flat image plus the DOL's own load address and entry point. This is a
// narrow, self-contained parser -- not a copy of Dolphin's DOL loader -- because gcnport's public
// contract takes a flat span, and duplicating Dolphin's internal DolReader class across the library
// boundary is unnecessary for this one-shot diagnostic.
DolImage LoadDolAsFlatImage(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "gmse01_boot: cannot open DOL image '%s'\n", path.c_str());
        std::exit(1);
    }
    const std::vector<u8> header_and_body((std::istreambuf_iterator<char>(file)),
                                          std::istreambuf_iterator<char>());
    if (header_and_body.size() < 0x100) {
        std::fprintf(stderr, "gmse01_boot: '%s' is too small to be a DOL image\n", path.c_str());
        std::exit(1);
    }

    struct Section {
        u32 file_offset;
        u32 address;
        u32 size;
    };
    std::vector<Section> sections;
    for (int i = 0; i < 7; ++i) {
        const u32 size = ReadBigEndianU32(header_and_body, 0x90 + 4 * i);
        if (size == 0)
            continue;
        sections.push_back({ReadBigEndianU32(header_and_body, 0x00 + 4 * i),
                            ReadBigEndianU32(header_and_body, 0x48 + 4 * i), size});
    }
    for (int i = 0; i < 11; ++i) {
        const u32 size = ReadBigEndianU32(header_and_body, 0xAC + 4 * i);
        if (size == 0)
            continue;
        sections.push_back({ReadBigEndianU32(header_and_body, 0x1C + 4 * i),
                            ReadBigEndianU32(header_and_body, 0x64 + 4 * i), size});
    }
    if (sections.empty()) {
        std::fprintf(stderr, "gmse01_boot: '%s' has no non-empty DOL sections\n", path.c_str());
        std::exit(1);
    }

    u32 lowest = sections.front().address;
    u32 highest = sections.front().address + sections.front().size;
    for (const Section& section : sections) {
        lowest = std::min(lowest, section.address);
        highest = std::max(highest, section.address + section.size);
    }

    DolImage image;
    image.load_address = lowest;
    image.entry_point = ReadBigEndianU32(header_and_body, 0xE0);
    image.flat.assign(highest - lowest, 0);
    for (const Section& section : sections) {
        if (static_cast<u64>(section.file_offset) + section.size > header_and_body.size()) {
            std::fprintf(stderr, "gmse01_boot: '%s' section overruns the file\n", path.c_str());
            std::exit(1);
        }
        std::memcpy(image.flat.data() + (section.address - lowest),
                    header_and_body.data() + section.file_offset, section.size);
    }
    return image;
}

// Answers Dolphin's alerts without a human, and counts them.
//
// Dolphin's default handler prompts on stdin ("Ignore and continue?"), which in a headless
// diagnostic is not a prompt but a hang: a run that should have produced a measurement instead
// waits forever for an answer nobody is there to give. Measured: an MMIO assertion stopped a
// 250M-block run dead. Silencing them outright would be worse, because an assertion is exactly the
// kind of finding this tool exists to surface, so each one is printed once with its text and
// tallied, and the run continues as if "Ignore" had been chosen.
std::atomic<u64> g_alerts_reported{0};

bool ReportAlertWithoutPrompting(const char* caption, const char* text, bool /*yes_no*/,
                                 Common::MsgType /*style*/) {
    const u64 index = g_alerts_reported.fetch_add(1);
    constexpr u64 MAX_DISTINCT_ALERTS_PRINTED = 20;
    if (index < MAX_DISTINCT_ALERTS_PRINTED) {
        std::printf("gmse01_boot: dolphin alert #%llu [%s] %s\n",
                    static_cast<unsigned long long>(index) + 1, caption, text);
        // Dolphin's invalid-access alert names the faulting PC and the address it could not
        // translate, and neither says which object was wrong. This handler runs on the CPU thread
        // inside the failing access, so the register file still holds the pointers the instruction
        // was walking -- the `this` in r31 and the member it loaded into r3 are what identify a
        // half-constructed object, and reconstructing them afterwards from a PC alone is guesswork.
        const auto& ppc_state = Core::System::GetInstance().GetPPCState();
        for (u32 row = 0; row < 8; ++row) {
            std::printf("gmse01_boot:   r%-2u=0x%08x r%-2u=0x%08x r%-2u=0x%08x r%-2u=0x%08x\n", row,
                        ppc_state.gpr[row], row + 8, ppc_state.gpr[row + 8], row + 16,
                        ppc_state.gpr[row + 16], row + 24, ppc_state.gpr[row + 24]);
        }
        std::printf("gmse01_boot:   lr=0x%08x ctr=0x%08x srr0=0x%08x srr1=0x%08x dar=0x%08x\n",
                    ppc_state.spr[SPR_LR], ppc_state.spr[SPR_CTR], ppc_state.spr[SPR_SRR0],
                    ppc_state.spr[SPR_SRR1], ppc_state.spr[SPR_DAR]);
        std::fflush(stdout);
    }
    return true;
}

// Walks a guest PowerPC stack from a saved stack pointer and prints the return addresses.
//
// A blocked thread's wait queue is an address, and for a queue that is a field inside some object
// there is no global to resolve it against -- tools/re/dataref.py confirms no instruction in the
// image forms those addresses directly. The call path is the readable answer instead: PowerPC's ABI
// makes the word at r1 the caller's stack pointer and the word at offset 4 of THAT frame the return
// address into the caller, so following the chain names every function between the thread's entry
// point and the wait it is sitting in. tools/re/addr2sym.py turns the printed addresses into names.
//
// Each step is bounded and each way of stopping is reported, because a truncated backtrace that
// looks complete is worse than none: the chain must stay inside RAM, stay word-aligned, and move
// strictly upward, which is what distinguishes a finished walk from a corrupt or circular one.
void SampleGuestWatch(const Memory::MemoryManager& memory, GuestWatch& watch, u64 blocks_run,
                      u32 retrace_count) {
    std::vector<u32> current(watch.words);
    for (u32 word = 0; word < watch.words; ++word) {
        current[word] = memory.Read_U32(watch.address + word * 4);
    }
    watch.samples += 1;
    if (current == watch.previous) {
        return;
    }
    const bool first = watch.previous.empty();
    if (!first) {
        watch.changes += 1;
    }
    std::printf("gmse01_boot: watch 0x%08x %s at block %llu (retrace %u):", watch.address,
                first ? "first sample" : "changed", static_cast<unsigned long long>(blocks_run),
                retrace_count);
    for (const u32 word : current) {
        std::printf(" %08x", word);
    }
    std::printf("\n");
    watch.previous = std::move(current);
}

void RunBoot(const DolImage& image, const BootRequest& request) {
    const std::string profile_path = File::CreateTempDir();
    if (profile_path.empty()) {
        std::fprintf(stderr, "gmse01_boot: failed to create an isolated Dolphin user directory\n");
        std::exit(1);
    }
    // Dolphin resolves the GameCube IPL font substitutes and the DSP ROM and coefficient tables
    // through File::GetSysDirectory(). Refuse rather than boot without them: left unresolved, a
    // title's OSGetFontTexture path reads through null font pointers -- measured as reads from
    // addresses 0x10..0x45 inside the SDK's font code -- and Dolphin says so only in a warning that
    // is easy to miss among a title's own output. The build links the checkout's Sys directory
    // beside this executable, which is where GetSysDirectory looks.
    const std::string font_path = File::GetSysDirectory() + "GC/font_western.bin";
    if (!File::Exists(font_path)) {
        std::fprintf(
            stderr,
            "gmse01_boot: no GameCube IPL font data at '%s'; a booted title's font path would "
            "read through null pointers\n",
            font_path.c_str());
        std::exit(1);
    }
    UICommon::SetUserDirectory(profile_path);
    Common::RegisterMsgAlertHandler(ReportAlertWithoutPrompting);

    PowerPC::GcnPort::ExecutionIdentity identity;
    // A real content digest of the exact booted bytes, matching the "caller-computed digest"
    // contract BootAuthenticatedImage documents (it does not itself recompute or verify the hash).
    mbedtls_sha256_ret(image.flat.data(), image.flat.size(), identity.image.sha256.data(), 0);
    identity.module_generation = 1;

    Core::System& system = Core::System::GetInstance();
    // apply_gamecube_os_init=true: reproduce the exact GameCube MSR/HID/BAT register setup every
    // retail title's real BS2/IPL performs before jumping to a disc's DOL entry point (gcnport's
    // PowerPC::GcnPort::BootAuthenticatedImage, backed by CBoot::SetupGameCubeBS2Registers, which
    // reuses CBoot::EmulatedBS2_GC's own SetupMSR/SetupHID/SetupBAT -- see
    // shared/gcnport/docs/dolphin-embedding-contract.md). Without it MSR.DR/IR stay at their
    // power-on-reset value of 0 (real mode), so GMSE01's own effective addresses (0x80xxxxxx) get
    // used as physical addresses far outside the console's 24 MiB of RAM and fault immediately.
    //
    // GMSE01's own linked __start does NOT need this tool to seed r1/r2/r13: its first instructions
    // (decomp/sms's __init_registers, see src/dolphin/os/__start.c) load r1/r2/r13 from the DOL's
    // own linked _stack_addr/_SDA2_BASE_/_SDA_BASE_ immediates via lis/ori before any memory
    // access, so a manually guessed stack pointer here would be redundant at best and wrong at
    // worst. apply_gamecube_hardware_init=true: also run Dolphin's own maintained
    // HW::Init/HW::Shutdown, which is what actually builds the MMIO::Mapping handler table
    // (MemoryManager::InitMMIO) every GameCube hardware register needs. Without it, GMSE01's own
    // __init_hardware code faults through an uninitialized handler the first time it touches one
    // (observed: SIGSEGV inside MMIO::WriteHandler<u32>::Write writing to physical 0x0C003004,
    // GameCube ProcessorInterface's PI_INTERRUPT_MASK -- see
    // shared/gcnport/docs/dolphin-embedding-contract.md, "GameCube hardware bring-up (MMIO handler
    // table)"). This still boots no host video/audio/input backend and touches no host disk: the
    // flag forces NullSound, "no memory card", and "no controller" before HW::Init runs.
    //
    // The diagnostic SIGSEGV reporter is installed HERE, immediately before
    // BootAuthenticatedImage's call to EMM::InstallExceptionHandler(), and NEVER re-armed
    // afterward. This ordering is load-bearing, not cosmetic: EMM::InstallExceptionHandler()
    // (Source/Core/Core/ MemTools.cpp) saves whatever SIGSEGV disposition is currently installed
    // into old_sa_segv and installs its own sigsegv_handler, which calls
    // JitInterface::HandleFault() on every SIGSEGV -- this is Dolphin's own normal fastmem MMU/MMIO
    // mechanism: an unbacked-page access (e.g. a GameCube hardware register with no fastmem-backed
    // page) deliberately faults so the handler can backpatch the JIT-generated load/store into a
    // slow C++ MMIO call. Only when HandleFault() returns false (a genuinely unhandled fault) does
    // sigsegv_handler restore old_sa_segv and re-raise. A prior version of this tool called
    // std::signal(SIGSEGV, ReportCountersOnFault) AFTER BootAuthenticatedImage, which -- because
    // signal() and sigaction() share one per-process disposition slot -- silently replaced
    // Dolphin's already-installed sigsegv_handler outright, so every subsequent SIGSEGV (including
    // ordinary, recoverable fastmem MMIO backpatch faults) went straight to this diagnostic handler
    // and was fatally reported instead of serviced. That misdiagnosed a real GMSE01 boot as
    // reaching an unresolvable DSP MMIO gap (physical 0x0C00500A, DSP_CONTROL) at block ~6400; with
    // the handler correctly installed first and left untouched, Dolphin's own JIT backpatches that
    // exact access and GMSE01 runs to this tool's full 16384-block bound with zero crashes -- there
    // is no DSP bring-up gap in gcnport to fix. See this repository's
    // docs/issues/0037-migrate-sunbright-execution-to-gcnport-dolphin-d.md, "fourth continuation".
    //
    // --raw-faults leaves this handler out. Dolphin's own sigsegv_handler stays installed and keeps
    // servicing ordinary fastmem backpatch faults, so only a genuinely unhandled fault reaches the
    // process default -- which is what a debugger needs in order to stop at the fatal one instead
    // of at whichever recoverable fastmem trap happened to come first. The cost is losing the
    // counter report on a crash, which is exactly what this handler exists to preserve, so it is
    // opt-in.
    if (request.report_counters_on_fault) {
        std::signal(SIGSEGV, ReportCountersOnFault);
    }

    const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
        system, identity, image.flat, image.load_address, image.entry_point,
        PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true,
                                              .apply_hardware_init = true,
                                              .disc_image_path = request.disc_image_path,
                                              .apply_media_init = true,
                                              .run_apploader = !request.disc_image_path.empty()});
    if (!booted.ok) {
        std::fprintf(stderr, "gmse01_boot: BootAuthenticatedImage failed: %s\n",
                     booted.detail.c_str());
        std::exit(1);
    }

    std::printf("gmse01_boot: booted at entry 0x%08x, load address 0x%08x, image size %zu bytes\n",
                image.entry_point, image.load_address, image.flat.size());

    {
        PowerPC::GcnPort::RuntimeSession runtime(system, identity);
        g_diagnostic_runtime = &runtime;

        // Hook mutations require a stopped CPU safe point, which is where this is: the session
        // exists and nothing has dispatched yet. Each counter's storage has to outlive every
        // dispatch and keep a stable address, because the runtime holds the raw pointer as the
        // callback's context -- hence the indirection rather than a vector of values that would
        // rehome its elements on the next push_back.
        gcnport::DolphinRuntimeAdapter adapter(system, runtime);
        std::vector<std::unique_ptr<CountedCall>> counted_calls;
        for (const u32 address : request.counted_call_addresses) {
            counted_calls.push_back(std::make_unique<CountedCall>(CountedCall{.address = address}));
            adapter.install_hook({.identity = adapter.identity(), .address = address},
                                 std::ref(*counted_calls.back()));
            if (!runtime.HasNativeHook(address)) {
                std::fprintf(stderr,
                             "gmse01_boot: the hook at 0x%08x did not install; refusing to report "
                             "a count it could not have measured\n",
                             address);
                std::exit(1);
            }
            std::printf("gmse01_boot: counting entries to 0x%08x\n", address);
        }

        std::vector<std::unique_ptr<SuperCall>> super_calls;
        for (const SuperCall& requested : request.super_calls) {
            super_calls.push_back(std::make_unique<SuperCall>(requested));
            adapter.install_hook({.identity = adapter.identity(), .address = requested.address},
                                 std::ref(*super_calls.back()));
            if (!runtime.HasNativeHook(requested.address)) {
                std::fprintf(stderr,
                             "gmse01_boot: the hook at 0x%08x did not install; refusing to report "
                             "a round trip it could not have made\n",
                             requested.address);
                std::exit(1);
            }
            std::printf("gmse01_boot: calling the original at 0x%08x from native code for the "
                        "first %llu entr(ies), budget %u instruction(s)\n",
                        requested.address,
                        static_cast<unsigned long long>(requested.max_round_trips),
                        requested.instruction_budget);
        }

        std::vector<std::unique_ptr<sunbright::gcnport_boot::GuestShapeProbe>> shape_probes;
        for (const u32 address : request.shape_probe_addresses) {
            shape_probes.push_back(std::make_unique<sunbright::gcnport_boot::GuestShapeProbe>(
                request.shape_probe_system, request.shape_probe_reports));
            adapter.install_hook({.identity = adapter.identity(), .address = address},
                                 std::ref(*shape_probes.back()));
            if (!runtime.HasNativeHook(address)) {
                std::fprintf(stderr,
                             "gmse01_boot: the hook at 0x%08x did not install; refusing to report "
                             "shapes it could not have read\n",
                             address);
                std::exit(1);
            }
            std::printf("gmse01_boot: reading guest J3D shapes at 0x%08x (j3dSys 0x%08x, first "
                        "%llu reported in full)\n",
                        address, request.shape_probe_system,
                        static_cast<unsigned long long>(request.shape_probe_reports));
        }

        std::vector<std::unique_ptr<sunbright::gcnport_boot::GuestLightingProbe>> lighting_probes;
        for (const u32 address : request.lighting_probe_addresses) {
            lighting_probes.push_back(std::make_unique<sunbright::gcnport_boot::GuestLightingProbe>(
                request.lighting_probe_reports));
            adapter.install_hook({.identity = adapter.identity(), .address = address},
                                 std::ref(*lighting_probes.back()));
            if (!runtime.HasNativeHook(address)) {
                std::fprintf(stderr,
                             "gmse01_boot: the hook at 0x%08x did not install; refusing to report "
                             "stage lighting it could not have read\n",
                             address);
                std::exit(1);
            }
            std::printf("gmse01_boot: reading guest stage lighting at 0x%08x (first %llu reported "
                        "in full)\n",
                        address, static_cast<unsigned long long>(request.lighting_probe_reports));
        }

        std::vector<std::unique_ptr<sunbright::gcnport_boot::GuestProjectionProbe>>
            projection_probes;
        for (const u32 address : request.projection_probe_addresses) {
            projection_probes.push_back(
                std::make_unique<sunbright::gcnport_boot::GuestProjectionProbe>(
                    request.projection_probe_reports));
            adapter.install_hook({.identity = adapter.identity(), .address = address},
                                 std::ref(*projection_probes.back()));
            if (!runtime.HasNativeHook(address)) {
                std::fprintf(stderr,
                             "gmse01_boot: the hook at 0x%08x did not install; refusing to report "
                             "projections it could not have read\n",
                             address);
                std::exit(1);
            }
            std::printf("gmse01_boot: reading guest projections at 0x%08x (first %llu reported in "
                        "full)\n",
                        address, static_cast<unsigned long long>(request.projection_probe_reports));
        }

        // Installed before the model probes on purpose: the publisher claims a counting sink the
        // first time it publishes, and the process has one sink. Activating the bridge first is
        // what makes the draws land in a frame instead of in a counter.
        sunbright::gcnport_boot::FrameDrawBudget draw_budget(request.draw_limit);
        sunbright::gcnport_boot::GuestFrameRenderer frame_renderer(&draw_budget);
        std::vector<std::unique_ptr<sunbright::gcnport_boot::FrameSeamHook>> frame_seams;
        if (!request.frame_seam_addresses.empty()) {
            std::string error;
            if (!frame_renderer.start(request.frame_image_path, request.frame_image_index, error)) {
                std::fprintf(stderr,
                             "gmse01_boot: --render-frames could not start the renderer: %s\n",
                             error.c_str());
                std::exit(1);
            }
            for (const u32 address : request.frame_seam_addresses) {
                frame_seams.push_back(std::make_unique<sunbright::gcnport_boot::FrameSeamHook>(
                    sunbright::gcnport_boot::FrameSeamHook{.renderer = &frame_renderer,
                                                           .address = address}));
                adapter.install_hook({.identity = adapter.identity(), .address = address},
                                     std::ref(*frame_seams.back()));
                if (!runtime.HasNativeHook(address)) {
                    std::fprintf(stderr,
                                 "gmse01_boot: the hook at 0x%08x did not install; refusing to "
                                 "render frames it could never have sealed\n",
                                 address);
                    std::exit(1);
                }
                std::printf("gmse01_boot: rendering guest frames sealed at 0x%08x into a %ux%u "
                            "offscreen target\n",
                            address, sunbright::gcnport_boot::GuestFrameRenderer::FRAMEBUFFER_WIDTH,
                            sunbright::gcnport_boot::GuestFrameRenderer::FRAMEBUFFER_HEIGHT);
            }
        }

        std::vector<std::unique_ptr<sunbright::gcnport_boot::GuestModelProbe>> model_probes;
        for (const u32 address : request.model_probe_addresses) {
            model_probes.push_back(std::make_unique<sunbright::gcnport_boot::GuestModelProbe>(
                request.shape_probe_system, request.model_probe_reports, request.draw_mode,
                &draw_budget));
            adapter.install_hook({.identity = adapter.identity(), .address = address},
                                 std::ref(*model_probes.back()));
            if (!runtime.HasNativeHook(address)) {
                std::fprintf(stderr,
                             "gmse01_boot: the hook at 0x%08x did not install; refusing to report "
                             "draws it could not have composed\n",
                             address);
                std::exit(1);
            }
            std::printf("gmse01_boot: composing guest draws at 0x%08x through j3dSys 0x%08x (first "
                        "%llu reported in full)\n",
                        address, request.shape_probe_system,
                        static_cast<unsigned long long>(request.model_probe_reports));
        }

        std::vector<std::unique_ptr<sunbright::gcnport_boot::GuestMaterialProbe>> material_probes;
        for (const u32 address : request.material_probe_addresses) {
            material_probes.push_back(std::make_unique<sunbright::gcnport_boot::GuestMaterialProbe>(
                request.material_probe_reports));
            adapter.install_hook({.identity = adapter.identity(), .address = address},
                                 std::ref(*material_probes.back()));
            if (!runtime.HasNativeHook(address)) {
                std::fprintf(stderr,
                             "gmse01_boot: the hook at 0x%08x did not install; refusing to report "
                             "materials it could not have read\n",
                             address);
                std::exit(1);
            }
            std::printf("gmse01_boot: reading guest J3D materials at 0x%08x (first %llu reported "
                        "in full)\n",
                        address, static_cast<unsigned long long>(request.material_probe_reports));
        }

        std::vector<GuestWatch> guest_watches = request.guest_watches;
        for (const GuestWatch& watch : guest_watches) {
            std::printf("gmse01_boot: watching 0x%08x (%u word(s)) for changes\n", watch.address,
                        watch.words);
        }

        // Bounded: this is a diagnostic boot attempt, not a gameplay loop.
        //
        // GMSE01 now runs through OS bring-up without a single fault and stops at a precisely
        // identified boundary. The earlier reading of this tool's output -- a permanent busy-wait
        // at pc=0x80343484 caused by a hardware condition no adapter boot could satisfy -- was
        // wrong on both counts, and was an artefact of a gcnport defect that froze CoreTiming's
        // global timer (fixed in extern/gcnport; see its ExecuteJitBlock regression). With the
        // timer advancing, the ARAM DMA completion interrupt that loop waits on is raised normally
        // and boot walks straight on through __OSInitAudioSystem, the RAM clear, and OSMemory's
        // protection setup.
        //
        // Boot used to stop in GMSE01's DVD error screen, because BootAuthenticatedImage placed a
        // flat DOL image in memory and exposed no DVD volume, so the title's first disc access
        // failed and the SDK fell into its disc-error path. --disc ended that: the title's own
        // TApplication::drawDVDErr, measured through --super-call across 1,428 consecutive frames,
        // returns 0 on 1,393 of them and 'em_3' ("Reading Disc...") on 35. Those 35 are frames the
        // title spends drawing that message instead of updating itself while DVDGetDriveStatus()
        // reports the drive busy -- transient, and what a real console does while the drive seeks.
        //
        // The budget has to clear System/Application.cpp's two OSProtectRange calls, which flush
        // 0x80000000 and 0x7d000000 bytes of address space. DCFlushRange walks those 32 bytes at a
        // time, so they alone retire 131,334,144 iterations of a single four-instruction block --
        // measured to land within 1% of that figure. Anything below that cannot reach the disc
        // boundary at all, which is exactly why a 16,384-block bound previously read as a permanent
        // stall.
        //
        // Dispatch is therefore split. The first blocks go one at a time so early boot stays
        // legible block by block, which is what this tool exists for; the rest go through
        // ExecuteJitBlocks, which lets the dispatcher chain direct-linked blocks natively. One
        // block per host call costs a host round trip per block: that path measured ~180,000
        // blocks/second here, against ~9,900,000 blocks/second batched through the same flush loop,
        // and it is the difference between reaching the disc boundary in ~34 seconds and in over
        // twelve minutes. Running both paths in one invocation also keeps each of them exercised.
        //
        // Throughput used to drop to ~40,000 blocks/second once boot entered that error screen --
        // the guest spin-waiting on timers, its slices ending at the next scheduled hardware event
        // rather than at a block, with every font read raising an invalid-access report. With a
        // disc mounted it no longer happens: a 600M-block run holds ~9,600,000 blocks/second from
        // start to finish and reports no invalid guest accesses at all.
        constexpr u32 STEPPED_BLOCKS = 32;
        constexpr u64 BATCH_BLOCKS = 1000000;
        constexpr u64 DEFAULT_MAX_BLOCKS = 160000000;
        const u64 max_blocks =
            request.block_budget != 0 ? request.block_budget : DEFAULT_MAX_BLOCKS;
        u64 blocks_run = 0;
        u32 previous_sample_pc = 0;
        u64 previous_sample_ticks = 0;
        u32 previous_retrace_count = 0;
        const auto started_at = std::chrono::steady_clock::now();

        for (; blocks_run < std::min<u64>(STEPPED_BLOCKS, max_blocks); ++blocks_run) {
            const auto outcome = runtime.ExecuteJitBlock();
            std::printf("gmse01_boot: block %llu kind=%d pc=0x%08x instructions=%u\n",
                        static_cast<unsigned long long>(blocks_run), static_cast<int>(outcome.kind),
                        outcome.guest_pc, outcome.instruction_count);
            if (outcome.kind == PowerPC::GcnPort::JitBlockKind::BackendFault) {
                std::printf("gmse01_boot: backend fault at block %llu, pc=0x%08x: %s\n",
                            static_cast<unsigned long long>(blocks_run), outcome.guest_pc,
                            outcome.detail.c_str());
                break;
            }
        }

        while (blocks_run < max_blocks) {
            const auto batch =
                runtime.ExecuteJitBlocks(std::min<u64>(BATCH_BLOCKS, max_blocks - blocks_run));
            blocks_run += batch.blocks_executed;
            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at)
                    .count();
            std::printf("gmse01_boot: %llu blocks, pc=0x%08x, %.1f s (%.0f blocks/s)\n",
                        static_cast<unsigned long long>(blocks_run), batch.guest_pc, elapsed,
                        elapsed > 0.0 ? blocks_run / elapsed : 0.0);
            if (!guest_watches.empty()) {
                const u32 retrace_count = system.GetMemory().Read_U32(GUEST_RETRACE_COUNT);
                for (GuestWatch& watch : guest_watches) {
                    SampleGuestWatch(system.GetMemory(), watch, blocks_run, retrace_count);
                }
            }
            if (batch.backend_fault || batch.blocks_executed == 0) {
                std::printf("gmse01_boot: batch stopped at pc=0x%08x: %s\n", batch.guest_pc,
                            batch.detail.c_str());
                break;
            }

            // A batch that retires its blocks but never leaves one PC was the shape of every stall
            // this tool saw while the boot could not get past its first seconds, and this report
            // used to be named for that conclusion. It is not a stall discriminator. Once the title
            // reaches its main loop a million-block batch ends inside a hot leaf -- a cache-flush
            // or soft-divide loop -- often enough that consecutive batches share a PC while the
            // guest advances normally: a four-billion-block run printed 204 "stalled" reports, with
            // a full thread walk under each, while its VI retrace count climbed past 20,000.
            //
            // The repeated PC stays as the cheap trigger for taking a sample; what the sample is
            // CALLED comes from the one reading that separates the two cases, whether the title is
            // still being handed VI retraces (GUEST_RETRACE_COUNT, above).
            // A guest that retires a million blocks and takes retraces
            // while doing it is progressing by definition; one that retires them and takes none is
            // either still in pre-VI boot or genuinely stuck, and only that case earns the state
            // dump below.
            if (batch.guest_pc == previous_sample_pc) {
                const auto& memory = system.GetMemory();
                const u64 ticks = system.GetCoreTiming().GetTicks();
                const u32 retrace_count = memory.Read_U32(GUEST_RETRACE_COUNT);
                const u32 retraces_delivered = retrace_count - previous_retrace_count;
                if (retraces_delivered != 0) {
                    std::printf("gmse01_boot: progressing at pc=0x%08x retraces=%u (+%u) "
                                "ticks=%llu (+%llu)\n",
                                batch.guest_pc, retrace_count, retraces_delivered,
                                static_cast<unsigned long long>(ticks),
                                static_cast<unsigned long long>(ticks - previous_sample_ticks));
                } else {
                    // Report the state that tells the remaining cases apart, so a negative result
                    // is a reading rather than a silence: whether the guest can take an interrupt
                    // at all (MSR.EE and any pending exception), what the interrupt controller is
                    // asserting and allowing (ProcessorInterface's cause and mask), and whether a
                    // disc is actually mounted. A title idling in SelectThread with no runnable
                    // thread looks identical to a crash loop without them.
                    //
                    // These come from the owning device objects, not from a memory read of the MMIO
                    // addresses: Memory::Read_U32 does not serve MMIO and answered every one of
                    // these with "Invalid range in CopyFromEmu" and a zero, which reads exactly
                    // like a quiet interrupt controller.
                    //
                    // pi_cause and exceptions are instantaneous samples, and a handler that has
                    // already run leaves both at zero -- so on their own they cannot tell "no
                    // interrupt was ever raised" from "every interrupt was raised, handled and
                    // cleared". The cumulative tick reading makes them legible: a GameCube VI
                    // retrace is one interrupt per 486MHz/60 ~= 8.1 million ticks, so the ticks
                    // elapsed give the number of retraces the interval should have contained.
                    // GUEST_CURRENT_THREAD is the OS global the scheduler clears before idling;
                    // NULL there confirms the stop is SelectThread's idle loop and not a crash loop
                    // that happens to sit at one address.
                    constexpr u32 GUEST_CURRENT_THREAD = 0x800000e4;
                    const auto& ppc_state = system.GetPPCState();
                    const auto& processor_interface = system.GetProcessorInterface();
                    const u32 current_thread = memory.Read_U32(GUEST_CURRENT_THREAD);
                    std::printf("gmse01_boot: no retrace delivered at pc=0x%08x msr=0x%08x (ee=%u) "
                                "exceptions=0x%08x pi_cause=0x%08x pi_mask=0x%08x disc_inside=%d "
                                "cur_thread=0x%08x retraces=%u ticks=%llu (+%llu)\n",
                                batch.guest_pc, ppc_state.msr.Hex,
                                static_cast<u32>(ppc_state.msr.EE), ppc_state.Exceptions,
                                processor_interface.GetCause(), processor_interface.GetMask(),
                                static_cast<int>(system.GetDVDInterface().IsDiscInside()),
                                current_thread, retrace_count,
                                static_cast<unsigned long long>(ticks),
                                static_cast<unsigned long long>(ticks - previous_sample_ticks));
                    // The running thread has no saved OSThread context to walk --
                    // ReportActiveThreads below reads each queued thread's saved SRR0/LR, and the
                    // one actually executing is the one whose saved copy is stale. Its live link
                    // register and stack pointer are in the PPC state, and the caller is what
                    // identifies a hot leaf helper: a batch that keeps ending inside a soft-divide
                    // or cache-flush loop says which routine is hot, never which routine called it.
                    std::printf("gmse01_boot:   running lr=0x%08x r1=0x%08x\n",
                                ppc_state.spr[SPR_LR], ppc_state.gpr[1]);
                    sunbright::gcnport_boot::ReportGuestBacktrace(memory, ppc_state.gpr[1]);
                    sunbright::gcnport_boot::ReportActiveThreads(memory);
                }
                previous_sample_ticks = ticks;
                previous_retrace_count = retrace_count;
            }
            previous_sample_pc = batch.guest_pc;
        }

        const auto counters = runtime.GetExecutionCounters();
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
        std::printf("gmse01_boot: retired %llu blocks in %.1f s (%.0f blocks/s)\n",
                    static_cast<unsigned long long>(blocks_run), seconds,
                    seconds > 0.0 ? blocks_run / seconds : 0.0);
        std::printf("gmse01_boot: jit_blocks_compiled=%llu jit_block_executions=%llu "
                    "cold_block_executions=%llu cache_hit_block_executions=%llu "
                    "fallback_events=%llu\n",
                    static_cast<unsigned long long>(counters.jit_blocks_compiled),
                    static_cast<unsigned long long>(counters.jit_block_executions),
                    static_cast<unsigned long long>(counters.cold_block_executions),
                    static_cast<unsigned long long>(counters.cache_hit_block_executions),
                    static_cast<unsigned long long>(counters.fallback_events));
        // Fallbacks by reason, with denominators. A single total cannot distinguish "the JIT met
        // one opcode it does not implement, in a hot loop" from "the runtime refused to fetch
        // blocks", and those want opposite work. Every reason is printed, including the ones that
        // did not occur: a reason missing from the output would be indistinguishable from a
        // reason this build never classifies.
        for (std::size_t reason = 0; reason < PowerPC::GcnPort::kJitRefusalReasonCount; ++reason) {
            const u64 events = counters.fallback_events_by_reason[reason];
            std::printf(
                "gmse01_boot:   fallback %-24s %12llu (%.4f%% of %llu blocks executed)\n",
                PowerPC::GcnPort::ToString(static_cast<PowerPC::GcnPort::JitRefusalReason>(reason)),
                static_cast<unsigned long long>(events),
                counters.jit_block_executions > 0
                    ? 100.0 * static_cast<double>(events) /
                          static_cast<double>(counters.jit_block_executions)
                    : 0.0,
                static_cast<unsigned long long>(counters.jit_block_executions));
        }

        // Which guest addresses produced them. All-one-reason totals above say the class of
        // refusal; only an address says which routine, and a truncated list would otherwise be
        // indistinguishable from a complete one -- so the session's own untracked count is
        // reported whether or not it is zero.
        const auto& fallback_sites = runtime.GetFallbackSites();
        std::printf("gmse01_boot:   fallback sites tracked=%zu untracked_events=%llu\n",
                    fallback_sites.size(),
                    static_cast<unsigned long long>(counters.fallback_sites_not_tracked));
        std::vector<std::pair<u32, PowerPC::GcnPort::FallbackSite>> ranked(fallback_sites.begin(),
                                                                           fallback_sites.end());
        std::ranges::sort(ranked, [](const auto& left, const auto& right) {
            return left.second.events > right.second.events;
        });
        constexpr std::size_t MOST_FREQUENT_SITES_PRINTED = 12;
        for (std::size_t rank = 0; rank < ranked.size() && rank < MOST_FREQUENT_SITES_PRINTED;
             ++rank) {
            std::printf("gmse01_boot:     0x%08x %12llu  %s\n", ranked[rank].first,
                        static_cast<unsigned long long>(ranked[rank].second.events),
                        PowerPC::GcnPort::ToString(ranked[rank].second.reason));
        }

        for (const auto& counted : counted_calls) {
            std::printf("gmse01_boot: hook 0x%08x entered %llu time(s)%s\n", counted->address,
                        static_cast<unsigned long long>(counted->entries),
                        counted->entries == 0 ? "  -- installed, never dispatched" : "");
        }

        for (const auto& super : super_calls) {
            std::printf("gmse01_boot: super-call 0x%08x entered %llu time(s), %llu round trip(s) "
                        "through the original body, %llu interpreted instruction(s)\n",
                        super->address, static_cast<unsigned long long>(super->entries),
                        static_cast<unsigned long long>(super->round_trips),
                        static_cast<unsigned long long>(super->original_instructions));
            if (super->round_trips == 0) {
                std::printf("gmse01_boot:   no body was called and no return value was read\n");
                continue;
            }
            std::printf("gmse01_boot:   shortest call %u instruction(s), longest %u\n",
                        super->shortest_original, super->longest_original);
            for (const auto& [value, count] : super->return_values) {
                std::printf("gmse01_boot:     returned r3=0x%08x to its native caller %llu time(s)"
                            "\n",
                            value, static_cast<unsigned long long>(count));
            }
            if (super->return_values_not_tracked != 0) {
                std::printf("gmse01_boot:     %llu further return(s) past %zu distinct values -- "
                            "the list above is truncated, not complete\n",
                            static_cast<unsigned long long>(super->return_values_not_tracked),
                            SuperCall::MAX_DISTINCT_RETURN_VALUES);
            }
        }
        for (const auto& probe : shape_probes) {
            probe->report();
        }
        for (const auto& probe : lighting_probes) {
            probe->report();
        }
        for (const auto& probe : projection_probes) {
            probe->report();
        }
        for (const auto& probe : material_probes) {
            probe->report();
        }
        for (const auto& probe : model_probes) {
            probe->report();
        }
        for (const auto& seam : frame_seams) {
            std::printf("gmse01_boot: frame seam 0x%08x entered %llu time(s)%s\n", seam->address,
                        static_cast<unsigned long long>(seam->entries),
                        seam->entries == 0 ? "  -- installed, never dispatched" : "");
        }
        if (frame_renderer.started()) {
            frame_renderer.report();
            std::string error;
            if (!frame_renderer.finish(error)) {
                std::printf("gmse01_boot: the frame renderer did not shut down cleanly: %s\n",
                            error.c_str());
            }
        }

        for (const GuestWatch& watch : guest_watches) {
            std::printf("gmse01_boot: watch 0x%08x sampled %llu time(s), changed %llu time(s)\n",
                        watch.address, static_cast<unsigned long long>(watch.samples),
                        static_cast<unsigned long long>(watch.changes));
        }

        std::printf("gmse01_boot: synchronous_original_calls=%llu "
                    "synchronous_original_instructions=%llu\n",
                    static_cast<unsigned long long>(counters.synchronous_original_calls),
                    static_cast<unsigned long long>(counters.synchronous_original_instructions));

        std::printf("gmse01_boot: dolphin_alerts=%llu\n",
                    static_cast<unsigned long long>(g_alerts_reported.load()));

        // A per-title settings layer that silently failed to load looks exactly like one that
        // loaded and changed nothing, so report the value this run actually ran with. GMS.ini's
        // EFBToTextureEnable=False is the setting Super Mario Sunshine needs; true here means the
        // shipped layer never reached Config.
        std::printf("gmse01_boot: gfx_skip_efb_copy_to_ram=%d gfx_efb_access=%d\n",
                    static_cast<int>(Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM)),
                    static_cast<int>(Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE)));

        for (const GuestMemoryWindow& window : request.dump_windows) {
            std::printf("gmse01_boot: guest memory at 0x%08x (%u word(s))\n", window.address,
                        window.words);
            sunbright::gcnport_boot::ReportGuestMemory(system.GetMemory(), window.address,
                                                       window.words, 0);
        }

        std::signal(SIGSEGV, SIG_DFL);
        g_diagnostic_runtime = nullptr;
    }

    PowerPC::GcnPort::ShutdownBootedImage(system);
    File::DeleteDirRecursively(profile_path);
}
} // namespace
} // namespace sunbright::gcnport_boot

int main(int argc, char** argv) {
    using namespace sunbright::gcnport_boot;

    std::setvbuf(stdout, nullptr, _IONBF, 0);

    BootRequest request;
    if (!parse_boot_options(argc, argv, request)) {
        return 1;
    }
    const DolImage image = LoadDolAsFlatImage(argv[1]);

    // Dolphin's BLR-return optimization installs a guard in the current CPU thread's own stack; a
    // dedicated thread gives this tool the same fully mapped stack contract Dolphin's shipping CPU
    // thread has (matching gcnport's own GcnPortRuntimeTest.cpp convention).
    std::thread cpu_thread(RunBoot, std::cref(image), std::cref(request));
    cpu_thread.join();
    return 0;
}
