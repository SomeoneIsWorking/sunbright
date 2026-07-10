# 2026-07-10 (continuation 3) — PerformLists.bin disc decode: confirms the order hypothesis
# was correctly falsified; "Draw Buffer Group" is not disc data at all

Task framing at session start: an entry-by-entry order comparison of "Draw Buffer Group"
(disc vs native), motivated by the same premise
`2026-07-10_gameloop_fader_ortho_clobber_finding.md` opened with (mirror camera dispatched
"after the flushes"). That premise was already falsified in this same thread
(`...gameloop_fader_ortho_clobber_finding.md` §1, confirmed again by the Ghidra
control-flow diff in `...direct_control_flow_ghidra_verified_falsifies_ordering.md`). This
session adds independent, disc-level confirmation from the actual data file (not a live
trace), which closes the loop from a different angle and should be read as corroboration,
not a new investigation branch.

## 0. "Draw Buffer Group" is not a PerformLists.bin entry — it never was

Traced the two loader templates involved:
- `TViewObjPtrListT<T,U>::load` (`JDRViewObjPtrList.hpp:19-34`): reads `count = readS32()`,
  then `count` nested `TNameRef::genObject`+`load()` records. Strictly append-only forward
  pass; a factory miss (`genObject` returns null) just isn't pushed — no reorder, no retry
  elsewhere.
- `TPerformList::load` (`PerformList.cpp:26-75`): reads `(name, u32 filter)` pairs until its
  own payload slice is exhausted. Same append-only shape; a `TNameRefGen::search` miss is
  logged (`[plload] DROPPED`) and skipped, again with no reordering.

`"Draw Buffer Group"` is `JDrama::TNameRefGen::search<TViewObjPtrListT<TViewObj>>("Draw
Buffer Group")` at `MarDirectorSetupObjects.cpp:394-396` — a **search**, not a construction
from `PerformLists.bin`. The object was already built earlier in `setupObjects()` from
`/scene/map/scene.bin` (line 322's `genObject`+`load()` call, a per-stage map archive
resource, not `PerformLists.bin`). It's then manually `unk40->push_back(drawBufferGroup, 8)`
at line 427 — **hardcoded C++, not data** — making `unk40` (a `TPerformList` used as a
single-entry wrapper) the thing `TMarDirector::direct()` dispatches as phase 1
(`MarDirectorDirect.cpp:322`, confirmed Ghidra-faithful in the prior session's diff).

Consequence: there is no "disc order" for Draw Buffer Group vs the mirror camera to compare
at the `PerformLists.bin` level, because they live in two structurally different lists
(`unk40`'s one member vs `mPerformListGX`'s 69 members) dispatched at two different fixed
CODE phases (phase 1 vs phase 4) that are identical in retail and native (Ghidra-verified
last session). Framing this as a `PerformLists.bin` load-order question was the same category
error the "order hypothesis" session already worked through from the live-trace side.

## 1. Extracted and decoded the real `/data/PerformLists.bin` (4668 bytes, uncompressed,
`/data/PerformLists.bin` @ ISO offset 1335714944 per FST walk)

New tool: `tools/oracle/decode_performlists.py` — a from-scratch decoder of the exact
`TNameRef::genObject` + `TPerformList::load` wire format (verified byte-for-byte against the
first ~40 bytes by hand before trusting the full decode: `u32 len=0x123c` (=4668, matches
file size) → `GroupObj`/`"PerformLists"` → `s32 count=7` → 7 nested `PerformList` records,
first one `PerformList Movement` with declared length 0x129 matching exactly). Full dump:
`scratch/oracle/performlists_dump.txt` (gitignored, regenerate with the tool + a disc
extract).

7 lists, disc order, verbatim (abbreviated to the load-bearing ones):
- **PerformList Movement** (13 entries, all filter `0x1`, one `0x40000001`)
- **PerformList CalcAnim** (14 entries, filter `0x2`/`0x6`/`0x40000002`)
- **PerformList GX** (69 entries) — the one that matters here. First 22 entries (idx 0-21)
  are the **mirror pass**: `PERF鏡ステージ`(0x80), `鏡カメラ`(0x10, sets mirror projection),
  `J3D System Set View Mtx`(0x4), `DrawBuf Mirror Opa/Xlu`(0x480 = collect, not draw),
  `鏡シーン`(0x206, mirror-scene traversal), ... `DrawBuf MirrorSky Opa/Xlu`,
  `DrawBuf Mirror Opa/Xlu`(0x8 = real draw), `DrawBuf MirrorAlways Opa/Xlu`(0x8), ... Then
  idx 23 `camera 1`(0x10, world projection) through idx 68 are the **world pass**:
  `DrawBuf Sky Opa/Xlu`(0x8) at idx 33/34, `DrawBuf MapOpa`(0x8) at idx 38, etc.
  **On disc, within this one list, mirror-camera-and-draw genuinely precedes
  world-camera-and-draw** — consistent with the retail FIFO capture's SEG0(mirror)-before-
  SEG1(world) shape.
- **PerformList Silhouette** (13), **PerformList GX Post** (78), **Shine PfLst Mov** (2),
  **Shine PfLst Anm** (3).

## 2. Native's live dispatch preserves PerformList GX's disc order exactly (no reorder)

Cross-checked against the existing `[plist-order]` trace
(`scratch/logs/frametrace_window.txt`, seq 79428-79433, `SB_TRACE_SEQ=1
SB_PLIST_ORDER_DBG_AFTER=<n>`): `鏡カメラ`(flags=0x10) → `J3D System Set View Mtx`(0x4) →
`DrawBuf Mirror Opa`(0x480) → `DrawBuf Mirror Xlu`(0x480) → `鏡シーン`(0x206) fire in exactly
disc index order (1,2,3,4,5) with byte-identical filter values. No reorder, no
prepend/append-on-miss inversion — the loader is faithful, matching the prior sessions'
Ghidra-verified control-flow finding from the other direction.

Also re-dumped native's actual constructed **"Draw Buffer Group"** child list directly
(`SB_J3D_DBG=1`, `scratch/logs/wf_plorder.log`, the `[setupObjects] drawBufferGroup=...
children:` block emitted once at `setupObjects()`): 34 children, `DrawBuf Sky
Opa/Xlu/MapOpa/MapXlu/... /StaticMapObj SunOpa/SunXlu/ShadowOpa/ShadowXlu/Graffito/Mirror
Opa/Xlu/MirrorSky Opa/Xlu/MirrorAlways Opa/Xlu/ChrOpa/Xlu/LensFlare/Last
Xlu/Indirect/AfterIndirect Opa/Xlu/<TLightDrawBuffer::Opa/Xlu>×4` — its order/source was
guessed here as `/scene/map/scene.bin`, but that guess was WRONG and is corrected in
`2026-07-10_drawbuf_group_type_mapping_falsified.md`: it's actually `/data/scenecmn.bin`
(stage-independent, loaded once in `TMarDirector::loadResource()`), decoded directly in
that follow-up session.

## 3. One new granular fact: which draw buffers are actually `TMirrorMapDrawBuf`-gated

Ran `SB_MIRRORBUF_DBG=1` (title, stage 15, headless, ~70s unpaced) —
**note: this log contains raw Shift-JIS bytes and needs `grep -a`, confirmed the hard way
after an initial `grep -c` silently returned nothing (matches the standing tooling note in
`2026-07-10_title_backdrop_black_verdict.md`'s "Practical note")**. Result (60-line cap,
4 distinct names, ~15 hits each):
```
[mirrorbuf] name='DrawBuf MirrorAlways Opa' flag=0x8 mCurrentMirrorIndex=-1 draws=0
[mirrorbuf] name='DrawBuf MirrorAlways Xlu' flag=0x8 mCurrentMirrorIndex=-1 draws=0
[mirrorbuf] name='DrawBuf MirrorSky Opa'    flag=0x8 mCurrentMirrorIndex=-1 draws=0
[mirrorbuf] name='DrawBuf MirrorSky Xlu'    flag=0x8 mCurrentMirrorIndex=-1 draws=0
```
So `DrawBuf MirrorSky {Opa,Xlu}` and `DrawBuf MirrorAlways {Opa,Xlu}` ARE
`TMirrorMapDrawBuf` instances and their gate (`mCurrentMirrorIndex != -1`) correctly
suppresses the draw at phase 1 (`draws=0`, as intended — matches `MapMirror.cpp:380-395`'s
gate logic exactly). But **`DrawBuf Mirror Opa`/`DrawBuf Mirror Xlu`** (the buffers that
actually hold the reflection geometry — `[dbhead] phase=1 ... packets=14 name="DrawBuf
Mirror Opa"` / `packets=2 name="DrawBuf Mirror Xlu"` in `frametrace_window.txt`) **never
appear in the `SB_MIRRORBUF_DBG` log at all**, across the whole run — i.e. they are plain,
ungated `TDrawBufObj`s in native, not `TMirrorMapDrawBuf`. Whether disc's `/scene/map/scene.bin`
also types them as plain `DrawBufObj` (matching native — not a bug) or as `MirrorMapDrawBuf`
(a real missing-gate construction bug) is unresolved; that requires decoding the per-stage
`scene.bin` (SZS/Yay0-compressed, a separate archive per stage), which is out of this
session's scope.

This is a narrower, real observation but **does not change the established root cause**:
per `...direct_control_flow_ghidra_verified_falsifies_ordering.md` §3's `TDrawBufObj::perform`
bit semantics (`0x8`=draw, `0x480`=collect for next frame, both decomp-faithful and
Ghidra-confirmed), phase 1's spurious content shows up uniformly across mirror-gated AND
ungated buffers alike (Sky Xlu/MapOpa are not mirror buffers at all and still flush nonzero
packets at phase 1) — so a missing mirror gate on 2 buffer names is not sufficient to explain
the whole phase-1 divergence and is not the thing to chase next.

**Follow-up resolved this fully** (`2026-07-10_drawbuf_group_type_mapping_falsified.md`):
decoded `/data/scenecmn.bin` directly and confirmed `DrawBuf Mirror Opa`/`DrawBuf Mirror
Xlu` are declared `DrawBufObj` (plain, ungated) ON DISC — native's construction is
faithful, not a bug. The "unresolved" framing directly below is superseded.

## 4. Conclusion — no fix; this session's finding is corroborating, not new

Per the existing thread's own conclusion (`...ordering.md` §3): the remaining open question
is a **data-level** one (which objects `entry()` into which draw buffers, and when, per
`direct()` call — not perform-list construction/dispatch order, which is now doubly confirmed
faithful, from both the live-trace side (prior sessions) and the raw disc-byte side (this
session)). No source change made. `TPerformList::load`/`TViewObjPtrListT::load` are correct
and disc-order-preserving; nothing here is a "loader bug" — the phase separation between
`unk40` (Draw Buffer Group) and `mPerformListGX` is intentional retail structure, and within
`mPerformListGX` disc order already puts mirror before world exactly as it should.

## Tooling landed
- `tools/oracle/decode_performlists.py` — standalone decoder for `/data/PerformLists.bin`'s
  `TNameRef::genObject`/`TPerformList::load` wire format; refuses empty/truncated/malformed
  input loudly (fail-fast, no silent empty dump). Reusable for any future perform-list disc
  order question without needing a live trace.
