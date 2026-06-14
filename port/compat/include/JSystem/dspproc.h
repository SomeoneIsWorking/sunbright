// SHADOW HEADER (64-bit port) of reference/sms/include/JSystem/dspproc.h —
// keep in sync. 64-bit-port change: DsetupTable's four buffer-address params
// (channel buffer / two filter tables / FX buffer) carry HOST pointers on the
// native build (JASDSPInterface passes (u32)CH_BUF etc.), so they are widened
// from u32 to uintptr_t. The leading count arg stays u32. PAL ITEM: the
// DsetupTable definition (DSP-task layer, dspproc.c — not yet ported) must match
// this widened signature. The remaining DSP-task entry points are left u32
// pending their first ported caller. Verbatim otherwise.
#ifndef JSYSTEM_DSPPROC_H
#define JSYSTEM_DSPPROC_H

#include <dolphin/types.h>
#include <stdint.h>

void DSPSendCommands(u32*, u32);
void DSPReleaseHalt();
void DSPWaitFinish();
void Dswap(u32, u32, u32);
void Dmix(u32, u32, u32, s16);
void Dcopy(u32, u32, u32);
void DloadBuffer1(u32, u32);
void DloadBuffer(u32, u32, u32);
void DsaveBuffer(u16, u32, u32);
void DsetLoopState(u32);
void DadpcmDec(u32, u32, u32, u32);
void DloadFilter(u32, u32);
void DcopyfromARAM(u32, u32, u32);
void DoscStart(unsigned char, u16, u16);
void DoscStop(unsigned char);
void DoscStore(u32);
void Dmixer(u32, u32);
void Dresampletest();
void Dadpcmtest(u32);
void DsetupTable(u32, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
void DsetMixerLevel(float);
void DsyncFrame(u32, u32, u32);
void wait_callback(u16);
void DwaitFrame();
void DiplSec(u32, void (*)(u16));
void DagbSec(u32, void (*)(u16));
void dummy_callback(u16);

#endif
