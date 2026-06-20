// dvd_seam.h — native reimplementation of the GC DVD disc-IO API.
//
// SDK header replaced: reference/sms/include/dolphin/dvd.h
// Used surface: 48 distinct / 190 calls. Hot: DVDReadPrio(19), DVDClose(15),
// DVDConvertPathToEntrynum(9), DVDOpen(7), DVDChangeDir(5), DVDReset(6).
// Plus DVDLow* (raw drive) used by the DTK audio-stream path.
//
// MAPPING (see README.md "DVD"):
//   The seam reads the disc image's FST (file-string-table) and serves files from
//   host storage. JKRDvdFile / JKRDvdArchive sit on top. Async DVD reads can be
//   served synchronously and the completion callback fired inline (the engine only
//   needs the *callback contract* honored, not real seek latency) OR on a host IO
//   thread for overlap. Assets are big-endian on disc; byte-swap to native at load
//   (the loaders / asset-swap step own that — DVD just delivers raw bytes).
//
// PRIOR ART: the recomp build already extracts the FST + Yaz0-decompresses + walks
// RARC archives (tools/jingle/ "sunbright-jingle" extractor, jingle.py). Reuse that
// FST/Yaz0/RARC code for the native loader.
//
// The DVDCommandBlock / DVDFileInfo structs keep their SDK layout (the game embeds
// them); internally a DVDFileInfo maps to an open host file + offset/length.
#pragma once
#include "platform_types.h"

namespace sb::platform::dvd {

struct CommandBlock;   // opaque, SDK DVDCommandBlock layout
struct FileInfo;       // opaque, SDK DVDFileInfo layout
using Callback = void (*)(s32 result, FileInfo* fileInfo);

// ---- bring-up -----------------------------------------------------------
void Init();           // DVDInit -> open the disc image, parse the FST
bool MountImage(const char* path);  // native-only: point the seam at a disc image / extracted root
void Reset();          // DVDReset

// ---- name <-> entrynum (dvd.h) ------------------------------------------
s32  ConvertPathToEntrynum(const char* path);   // DVDConvertPathToEntrynum (TODO)
bool ChangeDir(const char* dirName);            // DVDChangeDir (TODO)

// ---- open / close / read (dvd.h) ----------------------------------------
bool Open(const char* fileName, FileInfo* out);   // DVDOpen (TODO)
bool FastOpen(s32 entrynum, FileInfo* out);        // DVDFastOpen (TODO)
bool Close(FileInfo* fi);                          // DVDClose (TODO)
s32  ReadPrio(FileInfo* fi, void* addr, s32 length, s32 offset, s32 prio);  // DVDReadPrio (TODO, sync)
s32  ReadAsyncPrio(FileInfo* fi, void* addr, s32 length, s32 offset,
                   Callback cb, s32 prio);          // DVDReadAsyncPrio (TODO)
s32  GetCommandBlockStatus(const CommandBlock* cb);// DVDGetCommandBlockStatus (TODO)
s32  CancelAsync(CommandBlock* cb, void* cbFn);    // DVDCancelAsync (TODO)
bool CheckDisk();                                  // DVDCheckDisk -> always true natively (TODO)

// ---- low-level / streaming (dvd.h DVDLow*, DTK audio) -------------------
// DVDLowAudioStream / DVDLowRequestAudioStatus / DVDLowAudioBufferConfig drive the
// disc's ADPCM (DTK) audio stream. Natively: decode the streamed audio from the
// image and feed the audio device (audio_seam). DVDLowStopMotor/Seek/Reset -> no-op
// or trivial. TODO phase-2: only the stream calls the engine actually uses.

} // namespace sb::platform::dvd
