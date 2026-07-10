# 2026-07-10 — Airport asset chain: Yaz0 BE-length truncation (systemic) + TShimmer fail-fast

Continuation past the CARD arc: Load-game → intro movie → airport0.arc → TShimmer SEGV.

## ★★★★★ Yaz0 decompressed-length BE misread — SILENT ARCHIVE TRUNCATION (fixed, c9438d50)

`JKRDvdRipper.cpp decompSZS_subroutine:272` read `SYaz0Header::length` via a host-LE struct
load. Failure mode is vicious:
- True length low byte 0x00 (airport0.arc: 0x4B1300 → misread 0x134B00, ~26%): decompression
  stops silently at the misread length and REPORTS SUCCESS — everything past the cutoff is
  stale allocator memory.
- True length low byte nonzero: misread is usually HUGE → clamped down to maxDest → accidentally
  correct. This is why the title/plaza arcs all "worked".
`JKRDecompExpandSize` reads the same field BE-correctly — two independent reads of one field,
only one was ever swapped. Fix: assemble bytes 4-7 BE under SMS_NATIVE_PLATFORM.
**Any past mystery corruption in large SZS assets may be explained by this — re-test before
chasing old ghosts.**

## How it was found (method note — this worked well)

TShimmer::load crashed on a null J3DModelData for /scene/mapObj/ShimmerLow.bmd. Deep RE proved
the RARC lookup chain byte-exact-correct (name clean, hash match, offset math verified against
an independently extracted airport0.arc) — but the bytes at mArchiveData+0x378a40 were a
24-byte-stride pointer pattern, not 'J3D2bmd3'. Hardware watchpoint discipline: armed at
decompression entry on the target address → NEVER fired → the range was never written → not a
clobberer but an omission → walked the buffer for the content cutoff → 0x134B00 = byteswap of
0x4B1300. (lldb scripts kept: scratch/lldb_clobber.py, lldb_decomp_trace.py, lldb_watch_live.py.
gdb remains broken on these cores; use lldb.)

## Also landed (same commit)

TShimmer::load now OSPanics naming the path when the model loader returns null (lensglow
precedent) instead of SEGVing at J3DModel::entryModelData+20.

## Current frontier

`OSPanic J3DAnmLoader.cpp:38: anm swap incomplete: 0/1 blocks covered, first uncovered tag=''
type=btk1 (bad block size)` on ShimmerLow.btk — investigation in flight. NOTE: this panic fires
on a file that decompresses correctly now; check whether the 'bad block size' is a real swapper
gap or ANOTHER downstream victim of a different size-field misread.

## Update (same day): the btk1 panic was ANOTHER Yaz0-truncation victim + one real swapper gap

- ShimmerLow.btk's on-disc bytes swap CLEAN in a standalone harness (scratch/anm_test/) —
  the runtime tag=0x00000000 meant a zero-filled buffer: the SAME Yaz0 BE-length misread
  lived on in the ARAM decompressor siblings `JKRAram.cpp:376` + `JKRDvdAramRipper.cpp:297`.
  Fixed identically (reference/sms `afe62bb6`). Lesson: when one instance of a bug class is
  found, grep for ALL copies of the pattern before moving on — this was the 2nd and 3rd copy.
- One genuine swapper gap: J3D anm FINAL blocks declare mSize including trailing pad the GC
  loader never dereferences; map.btk (single TTK1, mSize=388, file 416, block@0x20) overshoots
  its file by 4 pad bytes — flush against the next RARC entry on GC, harmless there. anm_swap's
  block walk now hard-errors only for non-final blocks and clamps the final block to the buffer
  (superproject `36f2bab`).
- Frontier after both: SIGSEGV in `TMapEventSink::load` (Map/MapEventSink.cpp:167) via
  TStrategy::load ← TViewObjPtrListT::load during airport setupObjects — previously masked.

## Raw JSUInputStream read class: BE scene data copied unswapped (reference/sms 453e06e9)

The frontier SEGV in `TMapEventSink::load` was the next instance of a systemic class:
`JSUInputStream::read(&value, N)` is a raw byte copy — only the typed accessors
(readU32/readS32/...) byteswap under SMS_NATIVE_PLATFORM (see the JSU_BE* macros in
`JSUInputStream.hpp`). Every decomp site that raw-reads a BE scalar from scene/asset data gets
a byteswapped value on the LE host. Root instance here (verified in lldb):
`TMapEventSink::initWithBuildingNum` left `unk24 = 0x01000000` (BE 1 unswapped) →
`getBuilding(0)` indexed child 16,777,216 → `kill()` on null this during airport setupObjects.
Precedent fix: `MapCollisionData.cpp` (`TMapCollisionData::init`).

**Fixed sites (all in reference/sms `453e06e9`; new `JSU_BE32_INPLACE` helper for f32 fields):**
MapEventSink (unk24; initBuilding u16 pollution layer/child indices — GC truncates the BE u32
to u16, so swap-then-truncate; Bianco floats), MessageLoader (BMG FourCC switch 'INF1'/'DAT1'
never matched on LE → subtitles/balloon messages dead; header dwords, INF1 count, EntryInfo
scalars), J2DPane mKind/mInfoTag FourCCs, JDRCamera TOrthoProj fields, HelpActor message id,
PositionHolder/MapWarp/MapObjManager float triples, MapMirror slot counts, MapObjCloud +
RollBlock RGBA dwords (`& 0xff` picked the wrong byte), MapObjGrass/MapObjPollution counts,
RailBlock lift params, Item event ids, Enemy loaders (enemymanager, enemytable, launcher,
telesa, smallEnemy, emario).

**Audited and cleared (do not re-fix):** JPAEmitter/JPAField (.jpa blobs are pre-swapped
position-aware by sb_jpa_swap_to_host — their raw reads see host-endian data), JUTResource
(1-byte reads + raw name bytes), FlagManager (reads save data the native build itself wrote —
round-trip consistent; GC-format save import would need a decision, deferred), CardManager
`sector->read` (not a JSU stream). Already fixed in earlier commits: PerformList (27fc0ffa),
ParamInst, JDRActor/JDRPlacement/JDRViewport, ScrnFader, MessageUtil, Strategy, J2DScreen.

**Build gotcha:** reference/sms headers are STAGED at configure time
(`sms-boot/CMakeLists.txt` file(COPY) → `staged_sms_include`); a header edit needs
`cmake -B build` reconfigure or the old header keeps compiling (same class as the
file(GLOB) reconfigure rule).

**Frontier after fix:** the TMapEventSink SEGV is gone; boot runs title → save-file pick
(SB_SEL_PICK head-butt) → movie teardown → airport0.arc mount → genObject, and now stops at a
clean scaffold panic: `OSPanic ring3_stubs.cpp:128: TMapObjTree::initMapObj not ported`.
That is the next port item (exit 134, deliberate fail-fast, not corruption). Latest surviving
frame (SB_DUMP_FRAME_EVERY series): the save-file picker renders correctly
(scratch/screenshots/wf_rawreadfix_last.png).
