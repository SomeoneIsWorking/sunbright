// ============================================================================
// native_si.cpp — PC-native Serial Interface (controller) layer for SMS.
//
// Opt-in:  SUNBRIGHT_NATIVE_SI=1   (default OFF)
//
// WHAT THIS OWNS / WHY
// --------------------
// The GameCube SI block (MMIO 0xCC006400+) drives controller polling: the OS
// arms a VI-synced poll, the SI device DMAs each pad's button bytes into the SI
// data registers, and a transfer-complete / RDST interrupt latches them. SMS
// reads them every frame via JUTGamePad::read -> PADRead (Pad.c) which funnels
// through SIGetType / SIGetStatus / SIBusy / SIGetResponse, and the boot reset
// state machine (PADReset -> DoReset -> SIGetTypeAsync -> PADTypeAndStatusCallback
// -> SITransfer(CmdReadOrigin) -> PADOriginCallback -> PADEnable) brings a channel
// online by issuing real SI transfers and waiting for the SI interrupt.
//
// That whole register+interrupt path is a Dolphin black box that, like the DSP
// mailbox read, can wedge under the hybrid: it depends on MSR-gated critical
// sections (every SIBios entry wraps OSDisableInterrupts/Restore), on a
// CoreTiming-scheduled transfer-complete event firing, and on the RDST poll
// interrupt arriving — none of which is deterministic when our governor parks
// emulated time. (CompleteTransfer in SIBios.c reads __SIRegs[13]/[14]/[32+i]
// off 0xCC006400 directly; SIInterruptHandler is __OS_INTERRUPT_PI_SI.)
//
// Input VALUES are NOT touched here: they already reach the game through
// Dolphin's input override (main_sdl.cpp pad_override -> EmulatedController ->
// Pad::GetStatus). This file only OWNS the SI register/poll/transfer-complete
// machinery so completion is synchronous and translation-independent: SI
// transfers complete in-line (callback invoked immediately), controller type /
// status are served from a fixed model, and the per-frame pad bytes are
// synthesized straight from Pad::GetStatus in EXACTLY the wire format the guest's
// MakeStatus already decodes. No SI registers are read, no SI interrupt is
// relied on, so the SI layer can never black-box-wedge.
//
// WIRE FORMAT (must match guest SPEC2_MakeStatus, AnalogMode 0x300 == mode 3, and
// Dolphin CSIDevice_GCController::GetData/MapPadStatus — both verified):
//   data[0] (hi)  = stickY | stickX<<8 | (button | PAD_USE_ORIGIN)<<16
//   data[1] (low) = triggerRight | triggerLeft<<8 | substickY<<16 | substickX<<24
// Centers are 0x80 (the guest subtracts 128); button is the GC PAD_* bitmask.
//
// ADDRESSES (reference/sms_gmse01_funcs.txt + reference/sms/src/dolphin/si/SIBios.c;
// SITransfer/SIGetResponse are unnamed in funcs.txt — resolved from the recomp
// disassembly: SIGetResponse is the function right after SIGetResponseRaw):
//   80368318  SIBusy                    -> never busy (we complete synchronously)
//   80368338  SIIsChanBusy              -> never busy
//   80368cb0  __SITransfer(chan,out,outB,in,inB,cb) -> synth response + sync callback
//   80368ebc  SIGetStatus(chan)         -> per-chan status nibble (RDST / NO_RESPONSE)
//   80368f38  SISetCommand(chan,cmd)    -> no-op (no SI register write)
//   80368f4c  SITransferCommands()      -> no-op
//   80368fc8  SIEnablePolling(poll)     -> accept, return poll mask (no register)
//   803689b4  SIEnablePollingInterrupt  -> no-op, report disabled
//   803696f8  SIGetType(chan)           -> chan0 SI_GC_CONTROLLER, else NO_RESPONSE
//   803698bc  SIGetTypeAsync(chan,cb)   -> set type + invoke type callback synchronously
//   803691a4  SIGetResponse(chan,data)  -> chan0 synth pad bytes, return TRUE
//
// REQUIRED SEAM
// -------------
//   NONE. Self-registers via SUNBRIGHT_OVERRIDE; gated entirely by the env flag,
//   checked once at first dispatch. Overrides are not consulted on the
//   SUNBRIGHT_DISABLE_RECOMP oracle, so the oracle is unaffected regardless.
//
// VERIFY (headless)
// -----------------
//   1. Build (NEW file needs a reconfigure first — file(GLOB) is configure-time):
//        cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON
//        cmake --build build -j$(nproc)
//   2. Run headless with the flag and drive input over the probe:
//        SUNBRIGHT_HEADLESS=1 SUNBRIGHT_NATIVE_SI=1 SUNBRIGHT_PROBE=1 \
//          SUNBRIGHT_FASTBOOT=1 ./build/sunbright &
//        curl 'http://127.0.0.1:17654/pad?do=A&ms=200'      # jump
//        curl 'http://127.0.0.1:17654/pad?do=UP&ms=600'     # walk
//      Mario must respond (jump / move) exactly as without the flag — confirming
//      pad type/status/poll/data all flow through the native SI path.
//   3. A/B: same run WITHOUT SUNBRIGHT_NATIVE_SI must behave identically. No new
//      crash/hang at boot (reset state machine still enables chan 0) and no input
//      regression.
// ============================================================================

#include "../overrides.h"
#include "../intrinsics.h"
#include <cstdlib>
#include <cstring>

#ifdef HAVE_DOLPHIN_CORE

#include "Core/HW/GCPad.h"
#include "InputCommon/GCPadStatus.h"

namespace {

// ── env gate ────────────────────────────────────────────────────────────────
inline bool native_si_on() {
    static const bool on = [] {
        const char* e = getenv("SUNBRIGHT_NATIVE_SI");
        return e && e[0] == '1';
    }();
    return on;
}

// ── SI / PAD constants (dolphin/si.h, GCPadStatus.h) ─────────────────────────
constexpr u32 SI_ERROR_NO_RESPONSE = 0x0008u;
constexpr u32 SI_ERROR_RDST        = 0x0020u;
constexpr u32 SI_GC_CONTROLLER     = 0x08000000u | 0x01000000u;  // SI_TYPE_GC | SI_GC_STANDARD
constexpr u32 SI_MAX_CHAN          = 4u;

constexpr u32 PAD_USE_ORIGIN = 0x0080u;  // hi-bit the guest expects set every frame

// GC pad bitmask (PadButton in GCPadStatus.h == GC wire bits, 1:1 with PAD_*).
// Pad::GetStatus already returns these bits in GCPadStatus::button, so no remap
// is needed: the guest's MakeStatus consumes the same bitmask.

// Only port 0 is wired (SI port 0 forced to a Standard Controller in main_sdl.cpp).
inline bool chan_present(u32 chan) { return chan == 0; }

// Synthesize the two SI response words for `chan` from the live input path,
// in the guest/Dolphin mode-3 wire format. Writes data[0], data[1] to `out_ea`.
void synth_response(u32 chan, u32 out_ea) {
    GCPadStatus p = Pad::GetStatus((int)chan);

    // hi  = stickY | stickX<<8 | (button | PAD_USE_ORIGIN)<<16   (MapPadStatus)
    const u32 hi = (u32)p.stickY
                 | ((u32)p.stickX << 8)
                 | (((u32)p.button | PAD_USE_ORIGIN) << 16);

    // low = triggerRight | triggerLeft<<8 | substickY<<16 | substickX<<24  (mode 3)
    const u32 low = (u32)p.triggerRight
                  | ((u32)p.triggerLeft << 8)
                  | ((u32)p.substickY << 16)
                  | ((u32)p.substickX << 24);

    sb_w32(out_ea + 0, hi);
    sb_w32(out_ea + 4, low);
}

// SI command byte sits in the top 8 bits of the first output word.
inline u32 si_cmd(u32 output_ea) { return (sb_r32(output_ea) >> 24) & 0xFFu; }

// Run a guest SI/PAD callback synchronously: cb(chan, error/sr, context).
// Mirrors native_card.cpp::run_callback. The callee preserves non-volatiles, so a
// scratch copy of the register file is safe. context (r5) is left as-is (the guest
// callbacks SMS registers ignore it / treat it opaquely).
void run_si_callback(CPUState& cpu, u32 cb, u32 chan, u32 sr) {
    if (cb < 0x80000000u) return;
    CPUState c = cpu;
    c.gpr[3] = chan;
    c.gpr[4] = sr;
    call_ppc(c, cb);
}

}  // namespace

// ── BOOL SIBusy(void) ────────────────────────────────────────────────────────
SUNBRIGHT_OVERRIDE(ov_SIBusy, 0x80368318u) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x80368318u); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    cpu.gpr[3] = 0;  // FALSE: native transfers complete in-line, SI is never busy
}

// ── BOOL SIIsChanBusy(s32 chan) ──────────────────────────────────────────────
SUNBRIGHT_OVERRIDE(ov_SIIsChanBusy, 0x80368338u) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x80368338u); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    cpu.gpr[3] = 0;  // FALSE
}

// ── u32 SIGetType(s32 chan) ──────────────────────────────────────────────────
SUNBRIGHT_OVERRIDE(ov_SIGetType, 0x803696f8u) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x803696f8u); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    const u32 chan = cpu.gpr[3];
    cpu.gpr[3] = chan_present(chan) ? SI_GC_CONTROLLER : SI_ERROR_NO_RESPONSE;
}

// ── u32 SIGetTypeAsync(s32 chan, SITypeAndStatusCallback cb) ─────────────────
//   Resolve type immediately and call the type-and-status callback (chan, type)
//   synchronously, exactly as the SI interrupt path would once the type transfer
//   completed. This advances the PAD reset state machine deterministically.
SUNBRIGHT_OVERRIDE(ov_SIGetTypeAsync, 0x803698bcu) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x803698bcu); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    const u32 chan = cpu.gpr[3];
    const u32 cb   = cpu.gpr[4];
    const u32 type = chan_present(chan) ? SI_GC_CONTROLLER : SI_ERROR_NO_RESPONSE;
    run_si_callback(cpu, cb, chan, type);   // SITypeAndStatusCallback(chan, type)
    cpu.gpr[3] = type;
}

// ── u32 SIGetStatus(s32 chan) ────────────────────────────────────────────────
//   Returns the per-channel status nibble the caller masks (PADRead checks
//   SI_ERROR_NO_RESPONSE; SIGetResponseRaw checks SI_ERROR_RDST). A present
//   controller always has fresh poll data, so report RDST set and NO_RESPONSE
//   clear; an absent channel reports NO_RESPONSE.
SUNBRIGHT_OVERRIDE(ov_SIGetStatus, 0x80368ebcu) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x80368ebcu); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    const u32 chan = cpu.gpr[3];
    cpu.gpr[3] = chan_present(chan) ? SI_ERROR_RDST : SI_ERROR_NO_RESPONSE;
}

// ── BOOL __SITransfer(chan, output, outBytes, input, inBytes, callback) ───────
//   The completion funnel. On hardware this kicks an SI DMA and the transfer-
//   complete interrupt later writes `input` and runs `callback`. Here we serve it
//   synchronously: produce the response for the issued command, then invoke the
//   callback in-line with sr=0 (success). Handles the boot reset commands
//   (type 0x00, read-origin 0x41, calibrate 0x42, probe/fix 0x4E) and any
//   poll-driven controller read. No SI registers, no interrupt.
SUNBRIGHT_OVERRIDE(ov___SITransfer, 0x80368cb0u) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x80368cb0u); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    const u32 chan     = cpu.gpr[3];
    const u32 output   = cpu.gpr[4];
    const u32 input    = cpu.gpr[6];
    const u32 inBytes  = cpu.gpr[7];
    const u32 callback = cpu.gpr[8];

    if (!chan_present(chan)) {
        // No device: NO_RESPONSE so the reset machine DoResets to the next chan.
        run_si_callback(cpu, callback, chan, SI_ERROR_NO_RESPONSE);
        cpu.gpr[3] = 1;  // TRUE: transfer was "accepted"/issued
        return;
    }

    const u32 cmd = si_cmd(output);

    if (cmd == 0x00u) {
        // SI type-and-status read: 3 response bytes hold the device type. The
        // reset machine masks (type & ~0xff), so place SI_GC_CONTROLLER's top
        // bytes; low byte (status) cleared.
        if (inBytes >= 4) sb_w32(input, SI_GC_CONTROLLER);
        else for (u32 i = 0; i < inBytes && i < 3; i++)
                 sb_w8(input + i, (SI_GC_CONTROLLER >> (24 - 8 * i)) & 0xFFu);
    } else if (cmd == 0x41u || cmd == 0x42u) {
        // Read-origin (0x41) / calibrate (0x42): 10-byte PADStatus origin. Leave
        // it neutral (centered sticks, zero triggers) so the guest's ClampS8/U8
        // pass the live pad through unchanged. UpdateOrigin then subtracts 128 to
        // arrive at a zero origin. memset the whole origin buffer first.
        for (u32 i = 0; i < inBytes; i++) sb_w8(input + i, 0x00u);
        // PADStatus layout: button(2) stickX stickY substickX substickY tl tr ...
        // Center the four analog axes (offsets 2..5) at 0x80.
        if (inBytes > 2) sb_w8(input + 2, 0x80u);
        if (inBytes > 3) sb_w8(input + 3, 0x80u);
        if (inBytes > 4) sb_w8(input + 4, 0x80u);
        if (inBytes > 5) sb_w8(input + 5, 0x80u);
    } else if (cmd == 0x4Eu) {
        // Wireless fix-device: no wireless pads here; just zero the response.
        for (u32 i = 0; i < inBytes; i++) sb_w8(input + i, 0x00u);
    } else {
        // Poll / direct controller read (e.g. 0x40 analog-mode reads): synthesize
        // live pad bytes if the buffer is the 8-byte two-word response.
        if (inBytes >= 8) synth_response(chan, input);
        else for (u32 i = 0; i < inBytes; i++) sb_w8(input + i, 0x00u);
    }

    run_si_callback(cpu, callback, chan, 0u);  // SICallback(chan, sr=0, context)
    cpu.gpr[3] = 1;  // TRUE
}

// ── BOOL SIGetResponse(s32 chan, void* data) ─────────────────────────────────
//   PADRead's per-frame data funnel. Serve the live pad bytes directly.
SUNBRIGHT_OVERRIDE(ov_SIGetResponse, 0x803691a4u) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x803691a4u); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    const u32 chan = cpu.gpr[3];
    const u32 data = cpu.gpr[4];
    if (!chan_present(chan) || data < 0x80000000u) { cpu.gpr[3] = 0; return; }
    synth_response(chan, data);
    cpu.gpr[3] = 1;  // TRUE: fresh response available
}

// ── u32 SIEnablePolling(u32 poll) ────────────────────────────────────────────
//   Accept the poll request without touching the SI poll register; return the
//   poll mask the caller expects (it stores the result but only acts on the bits).
SUNBRIGHT_OVERRIDE(ov_SIEnablePolling, 0x80368fc8u) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x80368fc8u); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    // echo the poll mask back unchanged (r3 already holds it); no SI poll
    // register write, no RDST interrupt to arm — native reads are pull, not push.
}

// ── BOOL SIEnablePollingInterrupt(BOOL enable) ───────────────────────────────
SUNBRIGHT_OVERRIDE(ov_SIEnablePollingInterrupt, 0x803689b4u) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x803689b4u); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    cpu.gpr[3] = 0;  // report "was disabled"; native path needs no SI poll IRQ
}

// ── void SITransferCommands(void) ────────────────────────────────────────────
SUNBRIGHT_OVERRIDE(ov_SITransferCommands, 0x80368f4cu) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x80368f4cu); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    // no-op: no command latch register to kick
}

// ── void SISetCommand(s32 chan, u32 command) ─────────────────────────────────
SUNBRIGHT_OVERRIDE(ov_SISetCommand, 0x80368f38u) {
    if (!native_si_on()) { RecompFunc f = recomp_raw(0x80368f38u); if (f) f(cpu); else call_ppc(cpu, cpu.lr); return; }
    // no-op: motor/analog-mode command goes nowhere; rumble is not modeled here
}

#endif  // HAVE_DOLPHIN_CORE
