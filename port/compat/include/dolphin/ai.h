// SHADOW HEADER (64-bit port) of reference/sms/include/dolphin/ai.h — keep in
// sync. (This is the AI audio-interface DMA API, NOT dolphin/os*·vi*·hw_regs*,
// so it is shadowable.) 64-bit-port change: AIInitDMA's `start_addr` is the DMA
// source buffer address; in the native build the DAC ring buffers are HOST
// main-RAM pointers (JASAiCtrl passes `(u32)dac[2]` / `(u32)useRspMadep`, both
// s16* host buffers), so a u32 truncates the pointer on LP64. Widened
// AIInitDMA's start_addr — and AIGetDMAStartAddr's return (the same address read
// back) — from u32 to uintptr_t. owner/length/sample-rate/trigger/volume stay as
// declared. PAL ITEM: the AIInitDMA / AIGetDMAStartAddr definitions (the AI DMA
// driver in port/pal/audio — not yet ported) must match these widened
// signatures. Verbatim otherwise.
#ifndef _DOLPHIN_AI_H_
#define _DOLPHIN_AI_H_

#include <dolphin/types.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*AISCallback)(u32 count);
typedef void (*AIDCallback)();

#define AI_STREAM_START 1
#define AI_STREAM_STOP  0

#define AI_SAMPLERATE_32KHZ 0
#define AI_SAMPLERATE_48KHZ 1

AIDCallback AIRegisterDMACallback(AIDCallback callback);
void AIInitDMA(uintptr_t start_addr, u32 length);
BOOL AIGetDMAEnableFlag(void);
void AIStartDMA(void);
void AIStopDMA(void);
u32 AIGetDMABytesLeft(void);
uintptr_t AIGetDMAStartAddr(void);
u32 AIGetDMALength(void);
BOOL AICheckInit(void);
AISCallback AIRegisterStreamCallback(AISCallback callback);
u32 AIGetStreamSampleCount(void);
void AIResetStreamSampleCount(void);
void AISetStreamTrigger(u32 trigger);
u32 AIGetStreamTrigger(void);
void AISetStreamPlayState(u32 state);
u32 AIGetStreamPlayState(void);
void AISetDSPSampleRate(u32 rate);
u32 AIGetDSPSampleRate(void);
void AISetStreamSampleRate(u32 rate);
u32 AIGetStreamSampleRate(void);
void AISetStreamVolLeft(u8 vol);
u8 AIGetStreamVolLeft(void);
void AISetStreamVolRight(u8 vol);
u8 AIGetStreamVolRight(void);
void AIInit(u8* stack);
void AIReset(void);

#ifdef __cplusplus
}
#endif

#endif
