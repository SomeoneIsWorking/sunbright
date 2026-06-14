// SHADOW HEADER (64-bit port) of reference/sms/include/JSystem/dsptask.h —
// keep in sync. 64-bit-port change: DsyncFrame2's 2nd/3rd args are the DSP
// output-buffer start/end HOST addresses (JASDSPBuf passes u32(dsp_buf[...])),
// widened from u32 to uintptr_t. The 1st arg (subframe count) stays u32.
// PAL ITEM: the DsyncFrame2 definition (DSP-task layer, osdsp_task.c — not yet
// ported) must match this widened signature; keep the duplicate declaration in
// osdsp_task.h consistent if that header is later pulled into the core.
// Verbatim otherwise.
#ifndef JSYSTEM_DSPTASK_H
#define JSYSTEM_DSPTASK_H

#include <dolphin/types.h>
#include <stdint.h>

void DspBoot(void (*)(void*));
int DSPSendCommands2(u32*, u32, void (*)(u16));
int DspStartWork(u32, void (*)(u16));
void DspFinishWork(u16);
void DsyncFrame2(u32, uintptr_t, uintptr_t);

#endif
