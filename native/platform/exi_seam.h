// exi_seam.h — native shim for the GC EXI (external bus) API.
//
// SDK header replaced: reference/sms/include/dolphin/exi.h
// Used surface: 26 distinct / 165 calls. Hot: EXIUnlock(26), EXIDeselect(19),
// EXISync(17), EXIImm(15), EXISelect(13), EXIImmEx(11), EXILock(10), EXIProbe(7).
//
// EXI is the TRANSPORT under CARD (memcard on EXI0/1) and OS-RTC/SRAM (EXI0 chan2).
// In the native port the CARD seam (card_seam.h) and the RTC/SRAM bits of the OS
// seam (os_seam.h) are implemented DIRECTLY on host files — they do NOT funnel
// through a faithful EXI bus model. So this seam exists only so that any *direct*
// EXI* caller links; almost all direct callers live inside SDK CARD/RTC internals
// that we replace. Treat these as benign shims unless a real direct caller surfaces.
//
// Phase-2 GUIDANCE: do NOT build a faithful EXI model. If a direct EXI caller is
// found that is NOT inside replaced CARD/RTC code, escalate to the lead — it likely
// means an unported subsystem (e.g. a peripheral) rather than a missing EXI impl.
#pragma once
#include "platform_types.h"

namespace sb::platform::exi {

// Thin shims — succeed benignly. TODO phase-2 only if a real direct caller appears.
void Init();                                     // EXIInit
s32  Lock(s32 chan, s32 dev, void* cb);          // EXILock
s32  Unlock(s32 chan);                           // EXIUnlock
s32  Select(s32 chan, s32 dev, u32 freq);        // EXISelect
s32  Deselect(s32 chan);                         // EXIDeselect
s32  Imm(s32 chan, void* buf, s32 len, u32 mode, void* cb);   // EXIImm
s32  ImmEx(s32 chan, void* buf, s32 len, u32 mode);           // EXIImmEx
s32  Dma(s32 chan, void* buf, s32 len, u32 mode, void* cb);   // EXIDma
s32  Sync(s32 chan);                             // EXISync
s32  Probe(s32 chan);                            // EXIProbe
u32  GetID(s32 chan, s32 dev, u32* id);          // EXIGetID

} // namespace sb::platform::exi
