// card_seam.h — native reimplementation of the GC CARD (memory-card) API.
//
// SDK headers replaced: reference/sms/include/dolphin/card/*.h, card.h
// Used surface: 36 distinct / 87 calls. Hot: CARDClose(7), CARDIsValidBlockNo(8),
// CARDGetStatus(3), CARDCheckExAsync(3), CARDMountAsync, CARDOpen, CARDRead/Write,
// CARDProbeEx, CARDUnmount, CARDFormat/Create/Delete, CARDSetIcon*, CARDRand/Srand.
//
// MAPPING (see README.md "CARD"):
//   A host file-backed save image standing in for the physical card. Probe/mount
//   succeed against a host .raw image (auto-create a blank, formatted card on first
//   run). Read/write hit the file; async variants complete inline (or on an IO
//   thread) and fire the callback. The card FS logic (dir/FAT/checksum) is the
//   game's own CARD code running natively over our block IO.
//
// PRIOR ART: runtime/overrides/native_card.cpp (recomp build) already does exactly
// this — probe/mount/read-segment/write-page/erase-sector over a host .raw, blank
// auto-created on first run, synchronous completions. Reference it for the on-disk
// format, block/sector sizes, and the async-completion contract.
//
// The CARDFileInfo / CARDStat structs keep their SDK layout.
#pragma once
#include "platform_types.h"

namespace sb::platform::card {

struct FileInfo;   // opaque, SDK CARDFileInfo layout
struct Stat;       // opaque, SDK CARDStat layout
using Callback = void (*)(s32 chan, s32 result);

// ---- bring-up -----------------------------------------------------------
void Init();                       // CARDInit
bool SetImagePath(const char* path);  // native-only: where the host .raw save lives

// ---- probe / mount (card/CARDMount.h, card.h) ---------------------------
s32  ProbeEx(s32 chan, s32* memSize, s32* sectorSize);   // CARDProbeEx (TODO)
s32  MountAsync(s32 chan, void* workArea, Callback detach, Callback attach); // CARDMountAsync (TODO)
s32  Unmount(s32 chan);                                   // CARDUnmount (TODO)

// ---- open / rdwr (card/CARDOpen.h, CARDRead.h, CARDWrite.h) --------------
s32  Open(s32 chan, const char* fileName, FileInfo* fi);  // CARDOpen (TODO)
s32  Close(FileInfo* fi);                                  // CARDClose (TODO)
s32  Read(FileInfo* fi, void* addr, s32 length, s32 offset);          // CARDRead (TODO, sync)
s32  ReadAsync(FileInfo* fi, void* addr, s32 length, s32 offset, Callback cb);  // CARDReadAsync (TODO)
s32  Write(FileInfo* fi, void* addr, s32 length, s32 offset);         // CARDWrite (TODO, sync)
s32  WriteAsync(FileInfo* fi, void* addr, s32 length, s32 offset, Callback cb); // CARDWriteAsync (TODO)

// ---- status / fs ops (CARDStat.h, CARDCreate/Delete/Format) -------------
s32  GetStatus(s32 chan, s32 fileNo, Stat* stat);   // CARDGetStatus (TODO)
s32  SetStatusAsync(s32 chan, s32 fileNo, Stat* stat, Callback cb); // CARDSetStatusAsync (TODO)
s32  CheckExAsync(s32 chan, s32* xferBytes, Callback cb);  // CARDCheckExAsync (TODO)
s32  Create(s32 chan, const char* fileName, u32 size, FileInfo* fi, Callback cb); // CARDCreateAsync (TODO)
s32  Delete(s32 chan, const char* fileName);        // CARDDelete (TODO)
s32  Format(s32 chan);                              // CARDFormat (TODO)

// ---- prng (card.h CARDRand/Srand) — pure, used for serial gen ----------
u32  Rand();   void Srand(u32 seed);   // TODO (LCG per SDK)

} // namespace sb::platform::card
