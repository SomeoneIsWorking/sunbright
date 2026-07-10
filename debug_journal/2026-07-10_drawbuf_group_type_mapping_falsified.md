# 2026-07-10 (continuation 4) — Draw Buffer Group type-mapping hypothesis: FALSIFIED

Task framing: `...performlists_disc_decode.md` §3 left one question explicitly open --
whether disc's per-stage `scene.bin` types `DrawBuf Mirror Opa`/`DrawBuf Mirror Xlu`
(the buffers holding the actual reflection geometry, observed ungated at runtime) as
`MirrorMapDrawBuf` (a real missing-gate construction bug) or as plain `DrawBufObj`
(matching native, not a bug). This session decodes the disc bytes directly to answer it.

## 0. Correction to the prior session's premise: wrong source file

`...performlists_disc_decode.md` assumed "Draw Buffer Group" (and its DrawBuf children)
come from the per-stage `/scene/map/scene.bin`. Decoded that file directly this session
(new dump hook `SB_SCENE_DUMP=<path>`, `MarDirectorSetupObjects.cpp` at the
`JKRGetResource("/scene/map/scene.bin")` site) with a new generic decoder,
`tools/oracle/decode_scene_bin.py` (same `TNameRef::genObject` wire format as
`decode_performlists.py`, generalized to walk arbitrary nested container types, plus a
`TSmJ3DScn`/`"MarScene"` special case for `TLightMap`'s inline sub-block that
`TSmJ3DScn::loadSuper` reads between the object's name and its `[count][children]` list).
Full decode of stage 15's `scene.bin` (5550 bytes, all 57 nodes) and `tables.bin` (1387
bytes, 22 nodes) — **neither contains a "Draw Buffer Group" node, nor any `DrawBuf *`
node, at all.**

The real source: `TMarDirector::loadResource()` (`MarDirectorLoadResource.cpp:66-79`)
loads `/data/scenecmn.bin` (a raw, uncompressed DVD file directly under `/data`, exactly
like `/data/PerformLists.bin` -- no Yaz0/RARC needed) into `gpSceneCmnDat`, and
`setupObjects()` (`MarDirectorSetupObjects.cpp:228`) feeds it to
`TNameRefGen::getInstance()->load(stream)` as `sceneCommon` -- a **stage-independent**
common NameRef tree holding `"Root View Obj"`, `"ゲームオブジェクト"`, and
`"Draw Buffer Group"`. This is loaded ONCE regardless of stage, which is why the prior
session's runtime trace of `"Draw Buffer Group"`'s 34-ish children was real (it's the
right object) but its guess at the disc source file was wrong.

## 1. Decoded `/data/scenecmn.bin` directly (new dump hook `SB_SCENECMN_DUMP=<path>`,
`MarDirectorLoadResource.cpp`) -- 11745 bytes, 205 nodes total

`tools/oracle/decode_scene_bin.py scratch/bin/scenecmn.bin --find "Draw Buffer Group"`
finds it at `/Root NameRef/Root View Obj/Draw Buffer Group`, type `GroupObj`, **26 direct
children, verbatim disc order**:

| # | name | declared type (disc) |
|---|------|----------------------|
| 0 | DrawBuf Sky Opa | DrawBufObj |
| 1 | DrawBuf Sky Xlu | DrawBufObj |
| 2 | DrawBuf MapOpa | DrawBufObj |
| 3 | DrawBuf MapXlu | DrawBufObj |
| 4 | DrawBuf Map 半透明優先 (opa) | DrawBufObj |
| 5 | DrawBuf Map 半透明優先 (xlu) | DrawBufObj |
| 6 | DrawBuf Map 半透明優先2 (opa) | DrawBufObj |
| 7 | DrawBuf Map 半透明優先2 (xlu) | DrawBufObj |
| 8 | DrawBuf StaticMapObj SunOpa | DrawBufObj |
| 9 | DrawBuf StaticMapObj SunXlu | DrawBufObj |
| 10 | DrawBuf StaticMapObj ShadowOpa | DrawBufObj |
| 11 | DrawBuf StaticMapObj ShadowXlu | DrawBufObj |
| 12 | DrawBuf Graffito | DrawBufObj |
| 13 | **DrawBuf Mirror Opa** | **DrawBufObj** |
| 14 | **DrawBuf Mirror Xlu** | **DrawBufObj** |
| 15 | DrawBuf MirrorSky Opa | MirrorMapDrawBuf |
| 16 | DrawBuf MirrorSky Xlu | MirrorMapDrawBuf |
| 17 | DrawBuf MirrorAlways Opa | MirrorMapDrawBuf |
| 18 | DrawBuf MirrorAlways Xlu | MirrorMapDrawBuf |
| 19 | DrawBuf ChrOpa | DrawBufObj |
| 20 | DrawBuf ChrXlu | DrawBufObj |
| 21 | DrawBuf LensFlare | DrawBufObj |
| 22 | Last Xlu | DrawBufObj |
| 23 | DrawBuf Indirect | DrawBufObj |
| 24 | DrawBuf AfterIndirect Opa | DrawBufObj |
| 25 | DrawBuf AfterIndirect Xlu | DrawBufObj |

(22 `DrawBufObj` + 4 `MirrorMapDrawBuf` = 26; the remaining ~8 of the ~34 children the
prior session's `SB_J3D_DBG` trace showed are the 4 `TLightDrawBuffer` opa/xlu pairs
`addChildGroupObj`'d in by `TLightWithDBSetManager`'s ctor in C++, not disc entries.)

## 2. Type -> class mapping (`getNameRef`, `MarNameRefGen.cpp` / `JDRNameRefGen.cpp`)

```cpp
// JDRNameRefGen.cpp:66-67
if (strcmp(name, "DrawBufObj") == 0)
    return new TDrawBufObj;
// MarNameRefGen.cpp:143-144
if (strcmp(name, "MirrorMapDrawBuf") == 0)
    return new TMirrorMapDrawBuf;
```
Deterministic `strcmp` dispatch, no ambiguity, no fallback path for either name.

## 3. Runtime cross-check (`SB_NAMEREF_DBG=1`, one boot, `JDRNameRef.cpp:45-46`'s existing
one-line genObject type log)

Per boot cycle (game loops the title repeatedly under `SB_TURBO`; counts are N x 21
cycles): **462 `"DrawBufObj"` hits / 21 = 22**, **84 `"MirrorMapDrawBuf"` hits / 21 = 4**
-- exactly matching the disc counts above. Native constructs precisely what the disc
declares, for every one of the 26 members.

## Verdict: hypothesis FALSIFIED -- no type-mapping gap

`DrawBuf Mirror Opa`/`DrawBuf Mirror Xlu` are ungated (`TDrawBufObj`, not
`TMirrorMapDrawBuf`) in native **because the disc itself declares them as plain
`DrawBufObj`** -- confirmed at the byte level from `/data/scenecmn.bin`, not inferred.
`genObject`/`getNameRef`'s dispatch is faithful; there is no missing-gate wiring bug, no
`getNameRef` case mapping the gated type name to the wrong class, and nothing to fix here.
This closes the last open item from `...performlists_disc_decode.md` §3 as a negative
result: the phase-1 "ghost pass" divergence (still open, see
`2026-07-10_title_backdrop_black_verdict.md` for the actual confirmed root cause -- a
wrong global orthographic projection/viewport bound for every 3D draw) is **not**
explained by a Draw-Buffer-Group construction-type defect. Do not re-open this specific
angle; if `DrawBuf Mirror Opa/Xlu` need gating for correctness, that would be a genuine
missing-gate PORT (the retail binary's own dispatch also uses plain `TDrawBufObj` for
these two names per this disc data, so any gating requirement lives in different retail
code, not in `scenecmn.bin`'s type declarations).

## Tooling landed
- `tools/oracle/decode_scene_bin.py` -- generic `TNameRef::genObject` wire-format decoder
  (container-shape auto-detection + `TSmJ3DScn`/`TLightMap` special case), reusable for
  `scene.bin`/`tables.bin`/`scenecmn.bin`/any future `TNameRef`-tree disc dump. Refuses
  truncated/empty input loudly (`< 8` bytes).
- `SB_SCENE_DUMP=<path>` (`MarDirectorSetupObjects.cpp`, per-stage `scene.bin`),
  `SB_TABLES_DUMP=<path>` (same file, `tables.bin`), `SB_SCENECMN_DUMP=<path>`
  (`MarDirectorLoadResource.cpp`, stage-independent `scenecmn.bin`) -- one-line,
  env-gated, `SMS_NATIVE_PLATFORM`-guarded raw dumps of the decompressed in-memory blob
  to a caller-given path; no ROM extraction/Yaz0/RARC tooling needed since these are
  read post-decompression from the running process.
