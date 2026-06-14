// ===========================================================================
// port/pal/io/dvd_stub.cpp — PAL stubs for the GameCube DVD file API.
//
// Companion to pal/io/vi_dvd_stub.cpp (DVDConvertPathToEntrynum). This file
// resolves the DVD file-I/O entry points the SMS audio/asset path references
// (JASDvdThread, JKRDvdFile, JKRDvdAramRipper, JUTDirectFile):
//   DVDOpen, DVDFastOpen, DVDClose, DVDReadPrio, DVDReadAsyncPrio,
//   DVDGetCommandBlockStatus.
//
// SIGNATURES: from <dolphin/dvd.h> (extern "C"), so they resolve verbatim.
//
// WHY STUBS: the native port has NO GameCube DVD filesystem. Real asset loading
// goes through the endian-safe RARC reader (port/assets/rarc.{h,cpp}) and host
// file I/O, which is a separate concern. The DVD path is the legacy on-disc FS.
//
// BEHAVIOR (bring-up placeholders — FLAG FOR PM):
//   - DVDOpen / DVDFastOpen: return FALSE (0) — "file not found / cannot open".
//     Callers handle open-failure (skip / fall back to the native asset path).
//   - DVDClose: return TRUE (1) — closing nothing succeeds.
//   - DVDReadPrio: return -1 (DVD_RESULT_FATAL_ERROR) — synchronous read of an
//     unopened file fails; no bytes transferred.
//   - DVDReadAsyncPrio: return FALSE — the async request was not queued. (We do
//     NOT invoke the callback: nothing was queued, so there is no completion;
//     calling it with a fake result on an arbitrary thread could corrupt engine
//     state. Callers gate on the FALSE return.)
//   - DVDGetCommandBlockStatus: return DVD_STATE_END (0) — "idle / no command
//     pending", so a poll-for-completion loop terminates instead of spinning.
//
//   FLAG FOR PM: every DVD file operation reports failure/empty. Any engine path
//   that still loads assets via the DVD FS (rather than the native RARC/host
//   path) will see "cannot open" until native DVD/file I/O is built.
// ===========================================================================

#include <dolphin/dvd.h>

extern "C" {

BOOL DVDOpen(char* /*fileName*/, DVDFileInfo* /*fileInfo*/) {
    return 0;  // FALSE: no DVD filesystem; cannot open.
}

BOOL DVDFastOpen(s32 /*entrynum*/, DVDFileInfo* /*fileInfo*/) {
    return 0;  // FALSE: no DVD filesystem; cannot open.
}

BOOL DVDClose(DVDFileInfo* /*fileInfo*/) {
    return 1;  // TRUE: closing a non-open handle trivially "succeeds".
}

long DVDReadPrio(struct DVDFileInfo* /*fileInfo*/, void* /*addr*/,
                 long /*length*/, long /*offset*/, long /*prio*/) {
    return -1;  // DVD_RESULT_FATAL_ERROR: synchronous read fails (no FS).
}

BOOL DVDReadAsyncPrio(DVDFileInfo* /*fileInfo*/, void* /*addr*/, s32 /*length*/,
                      s32 /*offset*/, DVDCallback /*callback*/, s32 /*prio*/) {
    // Request not queued. Do NOT call the callback (see header: no completion
    // exists, and an out-of-band callback on an arbitrary thread is unsafe).
    return 0;  // FALSE
}

s32 DVDGetCommandBlockStatus(struct DVDCommandBlock* /*block*/) {
    return 0;  // DVD_STATE_END: idle / nothing pending -> poll loops terminate.
}

}  // extern "C"
