// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_report.h"

#include <cstdio>
#include <string>

#include "Core/HW/Memmap.h"

namespace sunbright::gcnport_boot {

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

} // namespace sunbright::gcnport_boot
