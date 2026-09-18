// Copyright 2026 Sunbright contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Standalone maintainer tool: attempts an exact GMSE01 boot through gcnport's public
// PowerPC::GcnPort adapter (BootAuthenticatedImage / ExecuteJitBlock), entirely from Sunbright.
// This intentionally never touches gcnport's own repository or tests: gcnport must never link or
// embed a copyrighted game asset, so the GMSE01 image bytes are read from disk here and passed
// across gcnport's public API boundary as an in-memory span, exactly like a title consumer would.
//
// This does not attempt disc/apploader emulation. gcnport's BootAuthenticatedImage is deliberately
// scoped to a raw in-memory image plus an explicit load address and entry point (see
// shared/gcnport/docs/dolphin-embedding-contract.md); it owns no DVD/volume/apploader pipeline.
// This tool therefore boots GMSE01's already-extracted main.dol directly: it parses the DOL header,
// assembles one contiguous flat image spanning the DOL's lowest to highest loaded address (gaps
// zero-filled, matching how the loader would place each section), and boots at the DOL's own
// entry point. This exercises real GMSE01 code without the disc/apploader/OS-init pipeline gcnport
// does not yet implement; it does not claim to reach gameplay.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
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
                    static_cast<unsigned long long>(index + 1), caption, text);
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
// One --dump-guest request: where to look in guest RAM once the run has stopped, and how much.
struct GuestMemoryWindow {
    u32 address = 0;
    u32 words = 0;
};

void ReportGuestBacktrace(const Memory::MemoryManager& memory, u32 stack_pointer) {
    constexpr u32 FRAME_RETURN_ADDRESS = 4;
    constexpr u32 GUEST_RAM_START = 0x80000000;
    constexpr u32 GUEST_RAM_END = 0x81800000;
    constexpr u32 MAX_FRAMES = 12;

    u32 frame = stack_pointer;
    for (u32 depth = 0; depth < MAX_FRAMES; ++depth) {
        if (frame < GUEST_RAM_START || frame >= GUEST_RAM_END || (frame & 3) != 0) {
            std::printf("gmse01_boot:     stack ends at 0x%08x after %u frame(s)\n", frame, depth);
            return;
        }
        const u32 caller = memory.Read_U32(frame);
        if (caller == 0) {
            std::printf("gmse01_boot:     stack base reached after %u frame(s)\n", depth);
            return;
        }
        if (caller <= frame) {
            std::printf("gmse01_boot:     back-chain does not grow at 0x%08x; stopping\n", frame);
            return;
        }
        std::printf("gmse01_boot:     #%u lr=0x%08x\n", depth,
                    memory.Read_U32(caller + FRAME_RETURN_ADDRESS));
        frame = caller;
    }
    std::printf("gmse01_boot:     stopped after %u frames; the stack is deeper\n", MAX_FRAMES);
}

// Walks the SDK's active-thread queue and reports where every thread is parked.
//
// "Every thread is blocked" is the state the idle loop reports, but it does not say what they are
// blocked ON, and that is the only question worth asking once VI interrupts are confirmed to be
// arriving. The OS keeps the list at OS_BASE_CACHED|0x00DC (`__OSActiveThreadQueue` in os.h); each
// OSThread carries its saved context at offset 0, so context.srr0 (OS_CONTEXT_SRR0 = 408) is the
// address the thread will resume at and context.lr (OS_CONTEXT_LR = 132) names its caller. Those
// two resolve against reference/sms_gmse01_funcs.txt into the exact SDK or game routine each
// thread is waiting in.
//
// The queue is walked with a hard cap and an explicit report of what was found, including the
// "found nothing" cases: an empty queue and a queue whose links leave RAM are different failures
// from a queue full of legitimately waiting threads, and a silent return could not tell them apart.
// Prints a window of guest memory, then follows each word in it that looks like a guest pointer.
//
// Dolphin's JIT keeps guest registers in host registers and only writes back the ones an access
// needs, so the register file sampled inside a fault handler is part live and part stale -- it
// disagreed with the faulting effective address the alert itself reported. Guest RAM is not: it is
// the same memory the title wrote. Following one level of pointers is what turns "this field is
// wrong" into "the object it points at was never constructed", which a flat hex window cannot say.
void ReportGuestMemory(const Memory::MemoryManager& memory, u32 address, u32 words, u32 depth) {
    constexpr u32 GUEST_RAM_START = 0x80000000;
    constexpr u32 GUEST_RAM_END = 0x81800000;
    constexpr u32 WORDS_PER_LINE = 4;
    constexpr u32 FOLLOWED_WORDS = 8;
    constexpr u32 MAX_DEPTH = 2;

    const std::string indent(static_cast<size_t>(depth) * 2 + 2, ' ');
    if (address < GUEST_RAM_START || address % 4 != 0 || address + words * 4 > GUEST_RAM_END) {
        std::printf("gmse01_boot:%s0x%08x is not a word-aligned guest RAM window of %u word(s)\n",
                    indent.c_str(), address, words);
        return;
    }

    for (u32 word = 0; word < words; word += WORDS_PER_LINE) {
        std::printf("gmse01_boot:%s0x%08x:", indent.c_str(), address + word * 4);
        for (u32 column = 0; column < WORDS_PER_LINE && word + column < words; ++column) {
            std::printf(" %08x", memory.Read_U32(address + (word + column) * 4));
        }
        std::printf("\n");
    }

    if (depth >= MAX_DEPTH) {
        return;
    }
    for (u32 word = 0; word < words && word < FOLLOWED_WORDS; ++word) {
        const u32 value = memory.Read_U32(address + word * 4);
        if (value < GUEST_RAM_START || value >= GUEST_RAM_END || value % 4 != 0) {
            continue;
        }
        std::printf("gmse01_boot:%s+0x%02x -> 0x%08x\n", indent.c_str(), word * 4, value);
        ReportGuestMemory(memory, value, WORDS_PER_LINE * 2, depth + 1);
    }
}

void ReportActiveThreads(const Memory::MemoryManager& memory) {
    constexpr u32 GUEST_ACTIVE_THREAD_QUEUE = 0x800000dc;
    constexpr u32 THREAD_CONTEXT_R1 = 4;
    constexpr u32 THREAD_CONTEXT_LR = 132;
    constexpr u32 THREAD_CONTEXT_SRR0 = 408;
    // state and attr are adjacent u16 fields, so the big-endian word at 0x2C8 holds state in its
    // high half and attr in its low half. Reading the word and masking the low half yields attr,
    // not state, and attr's values (0 and OS_THREAD_ATTR_DETACHED) are plausible-looking state
    // numbers -- which is exactly how the first version of this walker reported five threads as
    // READY while the scheduler idled, a contradiction that was the instrument's, not the guest's.
    constexpr u32 THREAD_STATE_AND_ATTR = 0x2c8;
    constexpr u32 THREAD_SUSPEND = 0x2cc;
    constexpr u32 THREAD_PRIORITY = 0x2d0;
    constexpr u32 THREAD_QUEUE = 0x2dc;
    constexpr u32 THREAD_MUTEX = 0x2f0;
    constexpr u32 THREAD_LINK_ACTIVE_NEXT = 0x2fc;
    constexpr u32 GUEST_RAM_START = 0x80000000;
    constexpr u32 GUEST_RAM_END = 0x81800000;
    constexpr u32 MAX_THREADS = 32;

    u32 thread = memory.Read_U32(GUEST_ACTIVE_THREAD_QUEUE);
    if (thread == 0) {
        std::printf("gmse01_boot:   active thread queue is empty\n");
        return;
    }

    u32 reported = 0;
    for (; thread != 0 && reported < MAX_THREADS; ++reported) {
        if (thread < GUEST_RAM_START || thread >= GUEST_RAM_END) {
            std::printf("gmse01_boot:   thread link leaves RAM at 0x%08x after %u thread(s)\n",
                        thread, reported);
            return;
        }
        // state is a bitmask: 1 READY, 2 RUNNING, 4 WAITING, 8 MORIBUND. For a WAITING thread the
        // queue pointer is the whole answer -- it identifies the object being waited on, and
        // resolving that address names the subsystem that owes the wake-up.
        const u32 state_and_attr = memory.Read_U32(thread + THREAD_STATE_AND_ATTR);
        std::printf("gmse01_boot:   thread 0x%08x state=%u attr=%u suspend=%d prio=%u queue=0x%08x "
                    "mutex=0x%08x srr0=0x%08x lr=0x%08x\n",
                    thread, state_and_attr >> 16, state_and_attr & 0xffff,
                    static_cast<int>(memory.Read_U32(thread + THREAD_SUSPEND)),
                    memory.Read_U32(thread + THREAD_PRIORITY),
                    memory.Read_U32(thread + THREAD_QUEUE), memory.Read_U32(thread + THREAD_MUTEX),
                    memory.Read_U32(thread + THREAD_CONTEXT_SRR0),
                    memory.Read_U32(thread + THREAD_CONTEXT_LR));
        ReportGuestBacktrace(memory, memory.Read_U32(thread + THREAD_CONTEXT_R1));
        thread = memory.Read_U32(thread + THREAD_LINK_ACTIVE_NEXT);
    }
    if (thread != 0) {
        std::printf("gmse01_boot:   stopped after %u threads; the queue is longer or circular\n",
                    reported);
    }
}

void RunBoot(const DolImage& image, u64 block_budget, const std::string& disc_image_path,
             bool report_counters_on_fault, const std::vector<GuestMemoryWindow>& dump_windows) {
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
    if (report_counters_on_fault) {
        std::signal(SIGSEGV, ReportCountersOnFault);
    }

    const auto booted = PowerPC::GcnPort::BootAuthenticatedImage(
        system, identity, image.flat, image.load_address, image.entry_point,
        PowerPC::GcnPort::GameCubeBootOptions{.apply_os_init = true,
                                              .apply_hardware_init = true,
                                              .disc_image_path = disc_image_path,
                                              .apply_media_init = true,
                                              .run_apploader = !disc_image_path.empty()});
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
        // Boot then stops in GMSE01's DVD error screen: the SDK strings "An error has occurred.
        // Turn the power OFF ...", "The Disc could not be read." and "Reading Disc..." are rendered
        // through the IPL font, which is what produces the run's "Trying to access Windows-1252
        // fonts" notice and the stream of one-byte reads from addresses that are themselves ASCII
        // codes. That is the correct and expected result: BootAuthenticatedImage places a flat DOL
        // image in memory and deliberately exposes no DVD volume, so the title's first disc access
        // fails and the SDK falls into its disc-error path. A disc device is the next adapter, and
        // it is one Sunbright owns -- the game image must never reach gcnport (see
        // docs/issues/0037).
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
        // Throughput drops to ~40,000 blocks/second once boot is inside the disc-error screen. That
        // is the guest spin-waiting on timers, not a runtime defect: its slices end at the next
        // scheduled hardware event rather than at a block, and each font read it makes raises an
        // invalid-access report. It is a property of the wait this tool currently ends in, and is
        // expected to go once a disc device exists.
        constexpr u32 STEPPED_BLOCKS = 32;
        constexpr u64 BATCH_BLOCKS = 1000000;
        constexpr u64 DEFAULT_MAX_BLOCKS = 160000000;
        const u64 max_blocks = block_budget != 0 ? block_budget : DEFAULT_MAX_BLOCKS;
        u64 blocks_run = 0;
        u32 previous_stall_pc = 0;
        u64 previous_stall_ticks = 0;
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
            if (batch.backend_fault || batch.blocks_executed == 0) {
                std::printf("gmse01_boot: batch stopped at pc=0x%08x: %s\n", batch.guest_pc,
                            batch.detail.c_str());
                break;
            }

            // A batch that retires its blocks but never leaves one PC is the shape every stall in
            // this tool has taken so far, and "it is still at the same address" on its own has
            // never been enough to tell which of them it is. Report the state that distinguishes
            // them, so a negative result is a reading rather than a silence: whether the guest can
            // take an interrupt at all (MSR.EE and any pending exception), what the interrupt
            // controller is asserting and allowing (ProcessorInterface's cause and mask), and
            // whether a disc is actually mounted. A title idling in SelectThread with no runnable
            // thread looks identical to a crash loop without them.
            //
            // These come from the owning device objects, not from a memory read of the MMIO
            // addresses: Memory::Read_U32 does not serve MMIO and answered every one of these with
            // "Invalid range in CopyFromEmu" and a zero, which reads exactly like a quiet interrupt
            // controller.
            if (batch.guest_pc == previous_stall_pc) {
                const auto& ppc_state = system.GetPPCState();
                const auto& processor_interface = system.GetProcessorInterface();
                const auto& memory = system.GetMemory();
                // pi_cause and exceptions are instantaneous samples, and a handler that has already
                // run leaves both at zero -- so on their own they cannot tell "no interrupt was
                // ever raised" from "every interrupt was raised, handled and cleared". Two
                // cumulative readings make them legible. The guest tick count says how much console
                // time an interval covered: a GameCube VI retrace is one interrupt per 486MHz/60
                // ~= 8.1 million ticks, so the ticks elapsed give the number of retraces the
                // interval should have contained. GUEST_RETRACE_ COUNT is the SDK's own
                // `retraceCount` (vi.c), incremented by __VIRetraceHandler and read back by
                // VIGetRetraceCount at 0x803504ec, whose single `lwz r3, -0x58f0(r13)` resolves it
                // through SDA1 to 0x8040e8d0. It only advances when a VI interrupt is both raised
                // by the hardware and dispatched into the title's handler, so it measures
                // end-to-end delivery rather than the instant of the sample. GUEST_CURRENT_THREAD
                // is the OS global the scheduler clears before idling; NULL there confirms the stop
                // is SelectThread's idle loop and not a crash loop that happens to sit at one
                // address.
                constexpr u32 GUEST_RETRACE_COUNT = 0x8040e8d0;
                constexpr u32 GUEST_CURRENT_THREAD = 0x800000e4;
                const u64 ticks = system.GetCoreTiming().GetTicks();
                const u32 retrace_count = memory.Read_U32(GUEST_RETRACE_COUNT);
                const u32 current_thread = memory.Read_U32(GUEST_CURRENT_THREAD);
                std::printf(
                    "gmse01_boot: stalled at pc=0x%08x msr=0x%08x (ee=%u) exceptions=0x%08x "
                    "pi_cause=0x%08x pi_mask=0x%08x disc_inside=%d cur_thread=0x%08x "
                    "retraces=%u (+%u) ticks=%llu (+%llu)\n",
                    batch.guest_pc, ppc_state.msr.Hex, static_cast<u32>(ppc_state.msr.EE),
                    ppc_state.Exceptions, processor_interface.GetCause(),
                    processor_interface.GetMask(),
                    static_cast<int>(system.GetDVDInterface().IsDiscInside()), current_thread,
                    retrace_count, retrace_count - previous_retrace_count,
                    static_cast<unsigned long long>(ticks),
                    static_cast<unsigned long long>(ticks - previous_stall_ticks));
                // The running thread has no saved OSThread context to walk -- ReportActiveThreads
                // below reads each queued thread's saved SRR0/LR, and the one actually executing is
                // the one whose saved copy is stale. Its live link register and stack pointer are
                // in the PPC state, and the caller is what identifies a hot leaf helper: a batch
                // that keeps ending inside a soft-divide or cache-flush loop says which routine is
                // hot, never which routine called it.
                std::printf("gmse01_boot:   running lr=0x%08x r1=0x%08x\n", ppc_state.spr[SPR_LR],
                            ppc_state.gpr[1]);
                ReportGuestBacktrace(memory, ppc_state.gpr[1]);
                ReportActiveThreads(memory);
                previous_stall_ticks = ticks;
                previous_retrace_count = retrace_count;
            }
            previous_stall_pc = batch.guest_pc;
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
        std::printf("gmse01_boot: dolphin_alerts=%llu\n",
                    static_cast<unsigned long long>(g_alerts_reported.load()));

        // A per-title settings layer that silently failed to load looks exactly like one that
        // loaded and changed nothing, so report the value this run actually ran with. GMS.ini's
        // EFBToTextureEnable=False is the setting Super Mario Sunshine needs; true here means the
        // shipped layer never reached Config.
        std::printf("gmse01_boot: gfx_skip_efb_copy_to_ram=%d gfx_efb_access=%d\n",
                    static_cast<int>(Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM)),
                    static_cast<int>(Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE)));

        for (const GuestMemoryWindow& window : dump_windows) {
            std::printf("gmse01_boot: guest memory at 0x%08x (%u word(s))\n", window.address,
                        window.words);
            ReportGuestMemory(system.GetMemory(), window.address, window.words, 0);
        }

        std::signal(SIGSEGV, SIG_DFL);
        g_diagnostic_runtime = nullptr;
    }

    PowerPC::GcnPort::ShutdownBootedImage(system);
    File::DeleteDirRecursively(profile_path);
}
} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const auto usage = [argv]() {
        std::fprintf(stderr,
                     "usage: %s <path-to-extracted-main.dol> [--disc <disc-image>] "
                     "[--max-blocks <n>] [--raw-faults] [--dump-guest <hex-addr>[:<words>] ...]\n",
                     argv[0]);
    };
    if (argc < 2 || argv[1][0] == '-') {
        usage();
        return 1;
    }

    // Optional overrides. Every one of these refuses a malformed value rather than silently falling
    // back to its default: a run that quietly used a different budget, or quietly mounted no disc,
    // would be indistinguishable from one that genuinely reached a different boundary.
    u64 block_budget = 0;
    std::string disc_image_path;
    bool report_counters_on_fault = true;
    std::vector<GuestMemoryWindow> dump_windows;
    for (int argument = 2; argument < argc; ++argument) {
        const std::string_view name = argv[argument];
        // The one flag that takes no value. Handled before the value check below, which would
        // otherwise reject it for having nothing after it.
        if (name == "--raw-faults") {
            report_counters_on_fault = false;
            continue;
        }
        if (argument + 1 >= argc) {
            std::fprintf(stderr, "gmse01_boot: %s needs a value\n", argv[argument]);
            return 1;
        }
        const char* const value = argv[++argument];

        if (name == "--disc") {
            disc_image_path = value;
        } else if (name == "--max-blocks") {
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(value, &end, 0);
            // strtoull saturates at ULLONG_MAX on overflow, so an out-of-range argument would
            // otherwise be accepted as a huge budget rather than refused; errno is the only way to
            // tell them apart.
            if (end == value || *end != '\0' || parsed == 0 || errno == ERANGE) {
                std::fprintf(stderr,
                             "gmse01_boot: --max-blocks must be a positive value, got '%s'\n",
                             value);
                return 1;
            }
            block_budget = parsed;
        } else if (name == "--dump-guest") {
            // <hex-addr>[:<words>]. A default window is one cache line, which is enough to
            // recognise an object header and its first members without hiding a typo in a wall of
            // zeroes.
            constexpr u32 DEFAULT_WORDS = 8;
            constexpr u32 MAX_WORDS = 4096;
            GuestMemoryWindow window;
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed_address = std::strtoull(value, &end, 16);
            if (end == value || parsed_address > 0xffffffffull || errno == ERANGE ||
                (*end != '\0' && *end != ':')) {
                std::fprintf(stderr,
                             "gmse01_boot: --dump-guest needs <hex-addr>[:<words>], got '%s'\n",
                             value);
                return 1;
            }
            window.address = static_cast<u32>(parsed_address);
            window.words = DEFAULT_WORDS;
            if (*end == ':') {
                const char* const words_text = end + 1;
                errno = 0;
                const unsigned long long parsed_words = std::strtoull(words_text, &end, 0);
                if (end == words_text || *end != '\0' || parsed_words == 0 ||
                    parsed_words > MAX_WORDS || errno == ERANGE) {
                    std::fprintf(stderr,
                                 "gmse01_boot: --dump-guest word count must be 1..%u, got '%s'\n",
                                 MAX_WORDS, words_text);
                    return 1;
                }
                window.words = static_cast<u32>(parsed_words);
            }
            dump_windows.push_back(window);
        } else {
            std::fprintf(stderr, "gmse01_boot: unknown option '%s'\n", argv[argument - 1]);
            usage();
            return 1;
        }
    }

    const DolImage image = LoadDolAsFlatImage(argv[1]);

    // Dolphin's BLR-return optimization installs a guard in the current CPU thread's own stack; a
    // dedicated thread gives this tool the same fully mapped stack contract Dolphin's shipping CPU
    // thread has (matching gcnport's own GcnPortRuntimeTest.cpp convention).
    std::thread cpu_thread(RunBoot, image, block_budget, disc_image_path, report_counters_on_fault,
                           dump_windows);
    cpu_thread.join();
    return 0;
}
