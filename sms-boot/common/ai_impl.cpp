// ai_impl.cpp — native AI (Audio Interface) seam.
//
// Models the GC Audio Interface SDK contract (from reference/sms/src/dolphin/ai/ai.c
// and reference/sms/include/dolphin/ai.h) as plain host-side state.
//
// On GameCube, AI was driven by MMIO registers (__AIRegs[], __DSPRegs[]) and
// hardware DMA/sample-counter interrupt logic.  On native PC there is no such
// hardware: native_audio.cpp owns the actual audio pipeline.  This seam captures
// the SDK-observable state (volumes, rates, DMA parameters, callbacks) so that
// game code calling AISet*/AIGet*/AIRegister* gets the documented contract without
// touching any hardware register.

#include <dolphin/ai.h>
#include <cassert>

// ---------------------------------------------------------------------------
// SDK-observable AI state (singleton, in-TU).
// ---------------------------------------------------------------------------
struct NativeAIState {
    AIDCallback dma_callback;      // AIRegisterDMACallback
    AISCallback stream_callback;   // AIRegisterStreamCallback
    u32  dma_start_addr;           // AIInitDMA start address
    u32  dma_length;               // AIInitDMA length (must be multiple of 32)
    bool dma_active;               // set by AIStartDMA, cleared by AIStopDMA
    u32  dsp_sample_rate;          // AI_SAMPLERATE_32KHZ or AI_SAMPLERATE_48KHZ
    u32  stream_sample_rate;       // AI_SAMPLERATE_48KHZ only (other values ignored)
    u32  stream_play_state;        // AI_STREAM_START or AI_STREAM_STOP
    u8   stream_vol_left;
    u8   stream_vol_right;
    u32  stream_sample_count;      // read by AIGetStreamSampleCount / AIResetStreamSampleCount
    u32  stream_trigger;           // AISetStreamTrigger (no real HW here; stored for ABI)
    bool init_flag;
};

static NativeAIState g_ai{};

extern "C" {

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void AIInit(u8* /*stack*/) {
    // Idempotent per SDK contract.
    if (g_ai.init_flag) return;

    // Defaults from the decomp (ai.c AIInit):
    //   stream vol left/right = 0
    //   stream sample rate    = AI_SAMPLERATE_48KHZ
    //   DSP sample rate       = AI_SAMPLERATE_32KHZ
    //   play state            = AI_STREAM_STOP
    //   callbacks             = NULL
    g_ai.stream_vol_left      = 0;
    g_ai.stream_vol_right     = 0;
    g_ai.stream_sample_rate   = AI_SAMPLERATE_48KHZ;
    g_ai.dsp_sample_rate      = AI_SAMPLERATE_32KHZ;
    g_ai.stream_play_state    = AI_STREAM_STOP;
    g_ai.dma_callback         = nullptr;
    g_ai.stream_callback      = nullptr;
    g_ai.stream_sample_count  = 0;
    g_ai.stream_trigger       = 0;
    g_ai.dma_start_addr       = 0;
    g_ai.dma_length           = 0;
    g_ai.dma_active           = false;
    g_ai.init_flag            = true;

    // The GC stack arg was used for interrupt-handler stack switching — a pure
    // hardware concern.  On native PC there is no AI interrupt handler so the
    // stack pointer is irrelevant.
}

// Reset all state (used by test harness to get a clean slate; also exposed by
// the original SDK for a full hardware reset).
void AIReset(void) {
    g_ai = NativeAIState{};
}

BOOL AICheckInit(void) {
    return g_ai.init_flag ? TRUE : FALSE;
}

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    // SDK contract: atomically swap, RETURN the previous callback.
    AIDCallback old = g_ai.dma_callback;
    g_ai.dma_callback = callback;
    return old;
}

void AIInitDMA(u32 start_addr, u32 length) {
    // SDK ASSERTMSGLINE: length must be a multiple of 32 bytes.
    assert((length & 0x1Fu) == 0u && "AIInitDMA: length must be multiple of 32 bytes");
    g_ai.dma_start_addr = start_addr;
    g_ai.dma_length     = length;
}

void AIStartDMA(void) {
    // On GC: sets the DMA-enable bit in __DSPRegs[27].
    // Native: marks the flag; the actual audio DMA is handled by native_audio.cpp.
    g_ai.dma_active = true;
}

void AIStopDMA(void) {
    g_ai.dma_active = false;
}

BOOL AIGetDMAEnableFlag(void) {
    return g_ai.dma_active ? TRUE : FALSE;
}

u32 AIGetDMAStartAddr(void) {
    return g_ai.dma_start_addr;
}

u32 AIGetDMALength(void) {
    return g_ai.dma_length;
}

u32 AIGetDMABytesLeft(void) {
    // No real DMA ring on PC; native_audio.cpp manages its own buffers.
    return 0;
}

// ---------------------------------------------------------------------------
// Stream sample counter
// ---------------------------------------------------------------------------

u32 AIGetStreamSampleCount(void) {
    return g_ai.stream_sample_count;
}

void AIResetStreamSampleCount(void) {
    // On GC: writes a reset bit to __AIRegs[0].
    // Native: reset the tracked counter; no HW register to write.
    g_ai.stream_sample_count = 0;
}

// ---------------------------------------------------------------------------
// Stream trigger (sample-count interrupt threshold)
// ---------------------------------------------------------------------------

void AISetStreamTrigger(u32 trigger) {
    // On GC: writes __AIRegs[3] (MMIO threshold for AIS interrupt).
    // Native: no interrupt hardware; store for ABI completeness.
    g_ai.stream_trigger = trigger;
}

u32 AIGetStreamTrigger(void) {
    return g_ai.stream_trigger;
}

// ---------------------------------------------------------------------------
// Stream play state
// ---------------------------------------------------------------------------

void AISetStreamPlayState(u32 state) {
    g_ai.stream_play_state = state;
}

u32 AIGetStreamPlayState(void) {
    return g_ai.stream_play_state;
}

// ---------------------------------------------------------------------------
// Sample rates
// ---------------------------------------------------------------------------

void AISetDSPSampleRate(u32 rate) {
    // Valid values: AI_SAMPLERATE_32KHZ (0) or AI_SAMPLERATE_48KHZ (1).
    // On GC: complex MMIO sequence with SRC (sample-rate-converter) init.
    // Native: store the rate; native_audio.cpp uses this to configure its sink.
    if (rate == AI_SAMPLERATE_32KHZ || rate == AI_SAMPLERATE_48KHZ) {
        g_ai.dsp_sample_rate = rate;
    }
}

u32 AIGetDSPSampleRate(void) {
    return g_ai.dsp_sample_rate;
}

void AISetStreamSampleRate(u32 rate) {
    // SDK (decomp ai.c): for non-48KHz values the function logs
    // "OBSOLETED. Only 48KHz streaming from disk is supported!" and returns
    // WITHOUT storing.  Only AI_SAMPLERATE_48KHZ is ever accepted.
    if (rate == AI_SAMPLERATE_48KHZ) {
        g_ai.stream_sample_rate = rate;
    }
    // Non-48KHz values silently ignored per SDK contract.
}

u32 AIGetStreamSampleRate(void) {
    return g_ai.stream_sample_rate;
}

// ---------------------------------------------------------------------------
// Stream volume
// ---------------------------------------------------------------------------

void AISetStreamVolLeft(u8 vol) {
    g_ai.stream_vol_left = vol;
}

u8 AIGetStreamVolLeft(void) {
    return g_ai.stream_vol_left;
}

void AISetStreamVolRight(u8 vol) {
    g_ai.stream_vol_right = vol;
}

u8 AIGetStreamVolRight(void) {
    return g_ai.stream_vol_right;
}

// ---------------------------------------------------------------------------
// Stream callback
// ---------------------------------------------------------------------------

AISCallback AIRegisterStreamCallback(AISCallback callback) {
    // Same swap-and-return-previous contract as AIRegisterDMACallback.
    AISCallback old = g_ai.stream_callback;
    g_ai.stream_callback = callback;
    return old;
}

} // extern "C"
