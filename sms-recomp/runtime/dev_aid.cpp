// dev_aid.cpp — AID, the audio-DMA engine (0xCC005030..0xCC00503C).
//
// AID lives in the DSP register block, not the AI block. It is the DAC's feeder: a source
// address in main memory, a block count, and an enable bit. Once enabled it walks the buffer
// 32 bytes at a time (8 stereo 16-bit frames), and when the count wraps it RE-ARMS itself from
// the latched source/count and raises the AID interrupt. That interrupt is the game's audio
// heartbeat: __AID_Callback -> JAS syncAudio -> updateDac -> the next DSP frame is prepared.
//
// Until this file existed, 0x5030..0x503A were swallowed by dev_aram.cpp as inert halfwords.
// Nothing counted down, nothing raised AID, so the guest configured the DMA once and then went
// quiet forever. That is why sms-recomp is silent — the mixer is downstream of a heartbeat that
// was never beating.
//
// PACING. There is no CPU-cycle clock here to schedule against, so the engine is stepped from
// the per-frame seam (sbr_audio_frame) and paced against the host audio backlog: we advance
// exactly as many blocks as the DAC has consumed. Free-running was measured at ~75x realtime in
// the retired Dolphin-hosted port; the host device queue is the only honest clock available.

#include "mmio.h"
#include "intrinsics.h"

#include <aurora/audio.h>
#include <lucent/log.h>

#include "guest_sched.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

extern u8* g_ram_base;
extern void call_ppc(CPUState& cpu, u32 address);
extern void rt_dump_guest_stack(const char* why);

namespace {

constexpr u32 AID_BASE = 0xCC005030u;
constexpr u32 AID_END  = 0xCC00503Cu;

constexpr u32 AID_START_HI    = 0x5030;   // source address, high halfword
constexpr u32 AID_START_LO    = 0x5032;   // low halfword; 32-byte aligned
constexpr u32 AID_BLOCKS_LEN  = 0x5034;   // present on hardware, unused by the SDK
constexpr u32 AID_CONTROL_LEN = 0x5036;   // bit 15 = enable, bits 0..14 = block count
constexpr u32 AID_BLOCKS_LEFT = 0x503A;   // read-only: blocks remaining, zero-based

constexpr u16 CTRL_ENABLE = 0x8000;
constexpr u16 CTRL_BLOCKS = 0x7FFF;

// One AID block is 32 bytes = 8 stereo frames of signed 16-bit samples.
constexpr u32 kBlockBytes  = 32;
constexpr u32 kBlockFrames = 8;

// The DAC is fed at 32 kHz by the game's own AISetDSPSampleRate; the host stream matches so no
// resampling happens here (SDL would do it, but a rate mismatch would also silently change the
// pacing arithmetic below).
constexpr u32 kSampleRate = 32000;
constexpr u32 kChannels   = 2;

// The live AI-DMA callback pointer (AIRegisterDMACallback's storage, r13-0x5874). Read fresh on
// every delivery: THP re-registers its own callback while a movie plays and restores the JAS one
// afterwards, so a cached pointer would deliver to a dead handler.
constexpr u32 AID_CALLBACK_GLOBAL = 0x8040E94Cu;

u16 g_reg[8];                                  // 0x5030..0x503E as halfwords

u16& reg(u32 off) { return g_reg[(off - AID_START_HI) >> 1]; }

// Latched-on-enable state, exactly as the hardware works: writing the source/count registers
// while a transfer is running does NOT disturb it; the new values are picked up at the wrap.
u32 g_cur_addr        = 0;
u32 g_remaining_blocks = 0;

// Rates, not totals, are what says whether this engine is healthy; see the 1 Hz report at the
// bottom of sbr_audio_frame.
unsigned long g_blocks = 0, g_wraps = 0, g_delivered = 0;

// ---------------------------------------------------------------------------------------
// Output tap

bool g_audio_open = false;
std::FILE* g_raw = nullptr;

void audio_init() {
    if (g_audio_open) return;
    if (!aurora_audio_open(kSampleRate, kChannels)) {
        lucent::error("aid", "aurora_audio_open({}, {}) failed — there is no host audio device to "
                             "pace the DMA against, and the engine's only clock is that backlog",
                      kSampleRate, kChannels);
        std::abort();
    }
    g_audio_open = true;

    if (const char* path = std::getenv("SBR_AUDIO_RAW")) {
        g_raw = std::fopen(path, "wb");
        if (!g_raw) {
            lucent::error("aid", "SBR_AUDIO_RAW={} could not be opened for writing", path);
            std::abort();
        }
        lucent::info("aid", "raw interleaved s16 dump -> {}", path);
    }
    lucent::info("aid", "host audio open at {} Hz, {} ch", kSampleRate, kChannels);
}

// Emit one 32-byte block. Guest memory is big-endian; the host stream is native-endian s16.
void emit_block(u32 addr) {
    const u32 off = addr & 0x01FFFFFFu;
    if (off + kBlockBytes > 0x01800000u) {
        lucent::error("aid", "DMA source 0x{:08x} is outside MEM1", addr);
        std::abort();
    }
    const u8* src = g_ram_base + off;

    int16_t frames[kBlockFrames * kChannels];
    for (unsigned i = 0; i < kBlockFrames * kChannels; ++i)
        frames[i] = (int16_t)(u16)((src[i * 2] << 8) | src[i * 2 + 1]);

    aurora_audio_push(frames, kBlockFrames);
    if (g_raw) std::fwrite(frames, sizeof(frames), 1, g_raw);
}

// ---------------------------------------------------------------------------------------
// Interrupt delivery
//
// Same synchronous-handler idiom as dev_di.cpp::deliver_complete: the guest handler runs as a
// nested call inside whatever host code stepped the engine, so the register file is saved and
// restored around it — that is what a hardware interrupt does with OSContext, and guest MEMORY
// changes persist as they must.
//
// RE-ENTRANCY IS BOUNDED TO ONE DEEP. The callback re-programs the DMA and can drive the engine
// far enough to wrap again from inside itself; hardware unwinds between interrupts and we would
// not, so a nested request is queued as a FLAG (not a count — a level that is still asserted when
// the handler returns is one interrupt, not several) and drained by the outermost call.
bool g_delivering = false;
bool g_pending    = false;

void deliver_aid() {
    if (g_delivering) { g_pending = true; return; }

    // Nothing registered yet. Hardware would raise the interrupt and __AIDHandler would find a
    // null callback and return; there is nothing to fake here.
    if (!sb_r32(AID_CALLBACK_GLOBAL)) return;

    // The LIVE state of whichever guest thread holds the token, NOT the g_cpu that dev_di uses:
    // gsched_init COPIES the host's initial CPUState, so g_cpu is a pre-boot snapshot whose r2
    // and r13 (the small-data bases) are still zero. DVDLowIntrHandler survives that because it
    // only needs r4; __AID_Callback does not — it read a small-data global through r13 and
    // faulted at 0xffffa3ac (= 0 - 0x5c54) the first time it was delivered. An interrupt on
    // hardware borrows the interrupted thread's register file, which is exactly this one.
    CPUState& cpu = gsched_cpu();

    g_delivering = true;
    unsigned rounds = 0;
    do {
        g_pending = false;
        if (++rounds > 64) {
            lucent::error("aid", "the AI-DMA callback re-armed {} times without settling", rounds);
            rt_dump_guest_stack("AID interrupt storm");
            std::abort();
        }
        const u32 cb = sb_r32(AID_CALLBACK_GLOBAL);
        if (!cb) break;
        const CPUState saved = cpu;
        call_ppc(cpu, cb);
        cpu = saved;
        ++g_delivered;
    } while (g_pending);
    g_delivering = false;
}

// ---------------------------------------------------------------------------------------
// The engine

void step_blocks(u32 n) {
    for (u32 i = 0; i < n; ++i) {
        emit_block(g_cur_addr);
        ++g_blocks;

        if (g_remaining_blocks != 0) {
            --g_remaining_blocks;
            g_cur_addr += kBlockBytes;
        }
        if (g_remaining_blocks == 0) {
            // Re-arm from the latched registers, then raise. Doing it in this order matters: the
            // handler routinely re-programs the DMA from inside the callback, and hardware has
            // already reloaded by the time the interrupt is seen.
            g_cur_addr         = ((u32)reg(AID_START_HI) << 16) | reg(AID_START_LO);
            g_remaining_blocks = reg(AID_CONTROL_LEN) & CTRL_BLOCKS;
            ++g_wraps;
            // One interrupt per wrap, as hardware does. A 70-block buffer at 32 kHz is 17.5 ms,
            // so at any video frame rate below 57 fps MORE THAN ONE cycle legitimately completes
            // per host frame and the guest must see each of them — collapsing them to one halved
            // the observed tick rate (2616 wraps but only 1600 deliveries, 43/s against the
            // 57/s the DAC actually consumed) and the DAC ran dry.
            //
            // What keeps this from becoming a burst is the PACING in sbr_audio_frame: it never
            // pushes past the backlog target, so the number of wraps one call can produce is
            // bounded by that target, not by how long the host stalled. A stall therefore drops
            // interrupts (as a masked level does on hardware) instead of replaying them.
            deliver_aid();
        }
    }
}

u32 aid_read(u32 ea, unsigned width) {
    const u32 o = ea & 0xFFFFu;
    if (width == 2) {
        // remaining_blocks_count is zero-based on hardware; software waits for it to reach 0.
        if ((o & ~1u) == AID_BLOCKS_LEFT)
            return g_remaining_blocks > 0 ? g_remaining_blocks - 1 : 0;
        return reg(o & ~1u);
    }
    if (width == 4) return ((u32)aid_read(ea, 2) << 16) | aid_read(ea + 2, 2);
    lucent::error("aid", "unsupported {}-byte read @ 0x{:08x}", width, ea);
    std::abort();
}

void aid_write(u32 ea, unsigned width, u32 value) {
    const u32 o = ea & 0xFFFFu;

    if (width == 4) {
        aid_write(ea, 2, value >> 16);
        aid_write(ea + 2, 2, value & 0xFFFF);
        return;
    }
    if (width != 2) {
        lucent::error("aid", "unsupported {}-byte write @ 0x{:08x}", width, ea);
        std::abort();
    }

    const u32 a = o & ~1u;
    lucent::debug("aid", "W 0x{:04x} <- 0x{:04x}", a, (u16)value);
    if (a == AID_BLOCKS_LEFT) {
        lucent::error("aid", "write of 0x{:04x} to the read-only blocks-left register", value);
        std::abort();
    }

    if (a == AID_CONTROL_LEN) {
        const bool was_enabled = (reg(AID_CONTROL_LEN) & CTRL_ENABLE) != 0;
        reg(AID_CONTROL_LEN) = (u16)value;
        // Latch source and count only on the 0->1 enable edge. Reprogramming while running is
        // picked up at the next wrap, which is what the SDK's double-buffering relies on.
        if (!was_enabled && (value & CTRL_ENABLE)) {
            g_cur_addr         = ((u32)reg(AID_START_HI) << 16) | reg(AID_START_LO);
            g_remaining_blocks = (u16)(value & CTRL_BLOCKS);
            audio_init();
            lucent::debug("aid", "armed: {} blocks from 0x{:08x}", g_remaining_blocks, g_cur_addr);
        }
        return;
    }

    reg(a) = (u16)value;
    (void)AID_BLOCKS_LEN;
}

} // namespace

// Stepped once per presented frame from overrides/native_frame.cpp (declared weak there, so this
// strong definition wins). This object is pulled into the link by aid_device_init(), which
// dev_aram.cpp calls — a weak reference alone would not extract it from the static archive.
extern "C" void sbr_audio_frame() {
    if (!g_audio_open) return;
    if (!(reg(AID_CONTROL_LEN) & CTRL_ENABLE)) return;

    // PACE AGAINST THE DEVICE: top the queue back up to a fixed target, which makes the engine
    // advance at exactly the rate the DAC drains — 32000 frames/s, i.e. 57.1 buffer cycles/s —
    // with no clock of its own. Free-running was measured at ~75x realtime in the retired
    // Dolphin-hosted port.
    //
    // The target must exceed the LONGEST host frame interval or the queue runs dry between
    // calls: at 100 ms it survives a 10 fps stall. Measured at 33 ms it underran continuously
    // (queue observed at 7 frames every frame; 45.8 s of audio emitted in 60 s of wall time).
    constexpr u32 kTargetFrames = kSampleRate / 10;      // 100 ms of slack
    const u32 queued = aurora_audio_queued_frames();
    if (queued >= kTargetFrames) return;

    const u32 want_frames = kTargetFrames - queued;
    step_blocks((want_frames + kBlockFrames - 1) / kBlockFrames);

    // Once a second, not once a frame: the numbers that matter here are RATES, and the backlog
    // has to be read as a trend (climbing = free-running, collapsing = starving, flat = paced).
    static u64 last_report = 0;
    static unsigned long last_wraps = 0, last_delivered = 0;
    const u64 secs = (u64)std::time(nullptr);
    if (secs != last_report) {
        last_report = secs;
        lucent::debug("aid", "queued={} blocks={} wraps={} (+{}/s) delivered={} (+{}/s) "
                             "cb=0x{:08x}",
                      queued, g_blocks, g_wraps, g_wraps - last_wraps, g_delivered,
                      g_delivered - last_delivered, sb_r32(AID_CALLBACK_GLOBAL));
        last_wraps = g_wraps;
        last_delivered = g_delivered;
    }
}

void aid_device_init() {
    std::memset(g_reg, 0, sizeof(g_reg));
    mmio_register(MmioDevice{AID_BASE, AID_END, "aid", &aid_read, &aid_write});
}
