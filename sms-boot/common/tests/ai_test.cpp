// ai_test.cpp — TDD harness for the AI (Audio Interface) seam (ai_impl.cpp).
//
// Tests the SDK-observable contract without any GameCube hardware or Dolphin:
//   - AIInit sets documented defaults (vol=0, dsp_rate=32k, stream_rate=48k,
//     play_state=stop, callbacks=null)
//   - AIRegisterDMACallback swap-and-return-previous semantics
//   - AIInitDMA records addr+length (round-trip via getters)
//   - AISetDSPSampleRate / AISetStreamSampleRate round-trip; stream silently
//     ignores non-48KHz values per SDK contract
//   - Volume and play-state setters record faithfully
//   - AIStartDMA / AIStopDMA toggle the enable flag
//   - AIRegisterStreamCallback swap-and-return-previous semantics
//   - AIReset clears all state (test isolation)

#include <dolphin/ai.h>
#include <cstdio>

static int g_fail = 0, g_checks = 0;

static void chki(long got, long want, const char* what) {
    ++g_checks;
    if (got != want) {
        ++g_fail;
        std::printf("  FAIL: %s  got=%ld  want=%ld\n", what, got, want);
    }
}

// Dummy callbacks — distinct addresses so we can compare them as values.
static void dummy_dma_a(void) {}
static void dummy_dma_b(void) {}
static void dummy_stream_a(u32) {}
static void dummy_stream_b(u32) {}

// ---------------------------------------------------------------------------
// AIInit defaults
// ---------------------------------------------------------------------------
static void test_init_defaults(void) {
    AIReset();
    AIInit(nullptr);

    chki(AICheckInit(),             TRUE,                    "AICheckInit after AIInit");
    chki(AIGetStreamVolLeft(),      0,                       "default vol left = 0");
    chki(AIGetStreamVolRight(),     0,                       "default vol right = 0");
    chki(AIGetDSPSampleRate(),      AI_SAMPLERATE_32KHZ,     "default DSP rate = 32k");
    chki(AIGetStreamSampleRate(),   AI_SAMPLERATE_48KHZ,     "default stream rate = 48k");
    chki(AIGetStreamPlayState(),    AI_STREAM_STOP,           "default play state = STOP");
    chki(AIGetDMAEnableFlag(),      FALSE,                   "default DMA not active");
    chki(AIGetStreamSampleCount(),  0,                       "default sample count = 0");
}

// AIInit is idempotent: calling it twice must not overwrite state changed after
// the first call.
static void test_init_idempotent(void) {
    AIReset();
    AIInit(nullptr);
    AISetStreamVolLeft(127);
    AIInit(nullptr);  // second call — must not reset vol back to 0
    chki(AIGetStreamVolLeft(), 127, "AIInit idempotent: vol not reset on second call");
}

// ---------------------------------------------------------------------------
// AIRegisterDMACallback: swap + return previous
// ---------------------------------------------------------------------------
static void test_register_dma_callback(void) {
    AIReset();

    // First registration: previous callback must be NULL.
    AIDCallback prev = AIRegisterDMACallback(dummy_dma_a);
    chki((long)(void*)prev, (long)(void*)nullptr, "dma cb: first prev == NULL");

    // Second registration: previous callback must be dummy_dma_a.
    prev = AIRegisterDMACallback(dummy_dma_b);
    chki((long)(void*)prev, (long)(void*)dummy_dma_a, "dma cb: second prev == dummy_dma_a");

    // Swap back to NULL; previous must be dummy_dma_b.
    prev = AIRegisterDMACallback(nullptr);
    chki((long)(void*)prev, (long)(void*)dummy_dma_b, "dma cb: third prev == dummy_dma_b");
}

// ---------------------------------------------------------------------------
// AIInitDMA: addr + length round-trip
// ---------------------------------------------------------------------------
static void test_init_dma_roundtrip(void) {
    AIReset();

    // length must be a multiple of 32; use 1024.
    AIInitDMA(0x80200000u, 1024u);
    chki((long)AIGetDMAStartAddr(), (long)0x80200000u, "DMA start addr round-trip");
    chki((long)AIGetDMALength(),    (long)1024u,        "DMA length round-trip");

    // Update with a different address and length.
    AIInitDMA(0x80300000u, 64u);
    chki((long)AIGetDMAStartAddr(), (long)0x80300000u, "DMA start addr updated");
    chki((long)AIGetDMALength(),    (long)64u,          "DMA length updated");
}

// ---------------------------------------------------------------------------
// AIStartDMA / AIStopDMA
// ---------------------------------------------------------------------------
static void test_dma_enable_flag(void) {
    AIReset();

    chki(AIGetDMAEnableFlag(), FALSE, "DMA not active before AIStartDMA");
    AIInitDMA(0x80100000u, 32u);
    AIStartDMA();
    chki(AIGetDMAEnableFlag(), TRUE,  "DMA active after AIStartDMA");
    AIStopDMA();
    chki(AIGetDMAEnableFlag(), FALSE, "DMA not active after AIStopDMA");
}

// ---------------------------------------------------------------------------
// AISetDSPSampleRate / AIGetDSPSampleRate round-trip
// ---------------------------------------------------------------------------
static void test_dsp_sample_rate(void) {
    AIReset();
    AIInit(nullptr);  // starts at 32KHz

    // Set to 48KHz.
    AISetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    chki(AIGetDSPSampleRate(), AI_SAMPLERATE_48KHZ, "DSP rate set to 48k");

    // Set back to 32KHz.
    AISetDSPSampleRate(AI_SAMPLERATE_32KHZ);
    chki(AIGetDSPSampleRate(), AI_SAMPLERATE_32KHZ, "DSP rate set back to 32k");

    // An out-of-range value must not corrupt the stored rate.
    AISetDSPSampleRate(99u);
    chki(AIGetDSPSampleRate(), AI_SAMPLERATE_32KHZ, "DSP rate unchanged for invalid value");
}

// ---------------------------------------------------------------------------
// AISetStreamSampleRate: SDK silently ignores non-48KHz values
// ---------------------------------------------------------------------------
static void test_stream_sample_rate(void) {
    AIReset();
    AIInit(nullptr);  // starts at 48KHz

    // Setting 48KHz explicitly must be accepted.
    AISetStreamSampleRate(AI_SAMPLERATE_48KHZ);
    chki(AIGetStreamSampleRate(), AI_SAMPLERATE_48KHZ, "stream rate: 48k accepted");

    // Setting 32KHz must be silently ignored — value stays at 48KHz.
    AISetStreamSampleRate(AI_SAMPLERATE_32KHZ);
    chki(AIGetStreamSampleRate(), AI_SAMPLERATE_48KHZ,
         "stream rate: 32k silently ignored (SDK contract)");

    // Any other invalid value also silently ignored.
    AISetStreamSampleRate(99u);
    chki(AIGetStreamSampleRate(), AI_SAMPLERATE_48KHZ,
         "stream rate: invalid value silently ignored");
}

// ---------------------------------------------------------------------------
// Volume setters
// ---------------------------------------------------------------------------
static void test_volumes(void) {
    AIReset();

    AISetStreamVolLeft(200u);
    chki(AIGetStreamVolLeft(), 200, "vol left = 200");

    AISetStreamVolRight(42u);
    chki(AIGetStreamVolRight(), 42, "vol right = 42");

    // Boundary: 0 and 255.
    AISetStreamVolLeft(0u);
    chki(AIGetStreamVolLeft(), 0, "vol left = 0");
    AISetStreamVolLeft(255u);
    chki(AIGetStreamVolLeft(), 255, "vol left = 255");
}

// ---------------------------------------------------------------------------
// AISetStreamPlayState
// ---------------------------------------------------------------------------
static void test_play_state(void) {
    AIReset();

    AISetStreamPlayState(AI_STREAM_START);
    chki(AIGetStreamPlayState(), AI_STREAM_START, "play state = START");

    AISetStreamPlayState(AI_STREAM_STOP);
    chki(AIGetStreamPlayState(), AI_STREAM_STOP, "play state = STOP");
}

// ---------------------------------------------------------------------------
// AIResetStreamSampleCount
// ---------------------------------------------------------------------------
static void test_stream_sample_count(void) {
    AIReset();

    // The count starts at 0; reset should keep it 0.
    AIResetStreamSampleCount();
    chki(AIGetStreamSampleCount(), 0, "sample count = 0 after reset");
}

// ---------------------------------------------------------------------------
// AIRegisterStreamCallback: swap + return previous
// ---------------------------------------------------------------------------
static void test_register_stream_callback(void) {
    AIReset();

    AISCallback prev = AIRegisterStreamCallback(dummy_stream_a);
    chki((long)(void*)prev, (long)(void*)nullptr, "stream cb: first prev == NULL");

    prev = AIRegisterStreamCallback(dummy_stream_b);
    chki((long)(void*)prev, (long)(void*)dummy_stream_a,
         "stream cb: second prev == dummy_stream_a");

    prev = AIRegisterStreamCallback(nullptr);
    chki((long)(void*)prev, (long)(void*)dummy_stream_b,
         "stream cb: third prev == dummy_stream_b");
}

// ---------------------------------------------------------------------------
// AIReset clears all state
// ---------------------------------------------------------------------------
static void test_reset_clears_state(void) {
    // Dirty up the state.
    AIInit(nullptr);
    AISetStreamVolLeft(99u);
    AISetStreamVolRight(88u);
    AISetStreamPlayState(AI_STREAM_START);
    AISetDSPSampleRate(AI_SAMPLERATE_48KHZ);
    AIRegisterDMACallback(dummy_dma_a);
    AIInitDMA(0xDEAD0000u, 64u);
    AIStartDMA();

    // Reset must clear everything.
    AIReset();

    chki(AICheckInit(),            FALSE, "reset: AICheckInit == FALSE");
    chki(AIGetStreamVolLeft(),     0,     "reset: vol left = 0");
    chki(AIGetStreamVolRight(),    0,     "reset: vol right = 0");
    chki(AIGetStreamPlayState(),   0,     "reset: play state = 0");
    chki(AIGetDSPSampleRate(),     0,     "reset: DSP rate = 0");
    chki(AIGetStreamSampleRate(),  0,     "reset: stream rate = 0");
    chki(AIGetDMAEnableFlag(),     FALSE, "reset: DMA not active");
    chki(AIGetDMAStartAddr(),      0,     "reset: DMA start addr = 0");
    chki(AIGetDMALength(),         0,     "reset: DMA length = 0");
    chki((long)(void*)AIRegisterDMACallback(nullptr),
         (long)(void*)nullptr, "reset: DMA callback = NULL");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(void) {
    std::printf("platform_ai_test\n");

    test_init_defaults();
    test_init_idempotent();
    test_register_dma_callback();
    test_init_dma_roundtrip();
    test_dma_enable_flag();
    test_dsp_sample_rate();
    test_stream_sample_rate();
    test_volumes();
    test_play_state();
    test_stream_sample_count();
    test_register_stream_callback();
    test_reset_clears_state();

    std::printf("  %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
