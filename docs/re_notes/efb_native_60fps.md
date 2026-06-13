# RE: native per-field EFB-copy textures for TRUE 60fps screen-space effects

Goal (user directive): make the EFB-feedback effects (plaza water refraction, mirror, graffiti
coverage, dash-blur, heat-haze/shimmer, underwater filter) render NATIVELY and at TRUE 60fps under
`SUNBRIGHT_INTERP60`. This is the OPPOSITE of strategy b1 (skip the copy / freeze the effect at
30fps) in `interp_screenspace_strategy.md`, which the user REJECTED. We OWN the EFB copy per field:
the in-between field gets its OWN per-field EFB-copy texture so each field's effect is computed from
its own (interpolated-camera) EFB and nothing is overwritten.

Read FIRST (correct + complete on the chain, do not re-derive):
- `docs/re_notes/efb_dynamic_texture_chain.md` — the full chain map: addresses, the four
  `TEfbCtrlTex` instances, `mImagePtr`@`TEfbCtrlTex+0x2C` = the FIXED copy-dst address, consumers.
- `docs/re_notes/water_rendering.md` — the screen-texture refraction mechanism.
- `docs/re_notes/interp_screenspace_strategy.md` — note its recommended strategy b1 is REJECTED.

Deliverable: `runtime/overrides/efb_native.cpp` (new file) + this doc. NO existing file edited.
Build-verified clean in `build-efb`.

---

## 0. Root cause (recap, confirmed)

Each `TEfbCtrlTex` copies the EFB to a FIXED guest address `mImagePtr` (`TEfbCtrlTex+0x2C`,
disasm-confirmed `lwz r0,0x2C(r29)` @ `0x802f8cf8`) via `GXCopyTex(mImagePtr)` in its copy phase
(`param_1 & 0x8`, `TEfbCtrlTex::perform` `0x802f8bac`). The consumers bind THAT SAME address through
a `GXTexObj` and sample it. Dolphin keys EFB-copy textures by guest address
(`copy_to_ram=false` -> pure VRAM texture-cache entry keyed by `destAddr`). On the 60fps in-between
the copy re-runs against the INTERPOLATED-camera EFB and OVERWRITES the real field's texture at the
same address -> the consumer flickers every other field. The in-between IS a correct
interpolated-camera frame (interp60 blends draw matrices); the ONLY bug is the shared address.

---

## 1. Consumer GXTexObj resolution per instance + the image-ptr offset

### The shared fact (`setTexAttb`, `JDREfbCtrl.cpp:65`)
`TEfbCtrlTex::setTexAttb(const GXTexObj& obj)` does
`GXGetTexObjAll(&obj, &mImagePtr, ...)` — it COPIES the consumer's GXTexObj image pointer INTO
`mImagePtr`. So for every instance, **`mImagePtr` (TEfbCtrlTex+0x2C) equals the decoded image base
of the consumer's GXTexObj.** That is the join key the native fix uses: match consumers by
"GXTexObj whose encoded image base == a tracked `mImagePtr`."

### GXTexObj internal layout + the image-ptr offset (`src/dolphin/gx/GXTexture.c`)
`__GXTexObjInt { u32 mode0; u32 mode1; u32 image0; u32 image3; ... }` — so:

| field  | byte offset |
|--------|-------------|
| mode0  | 0x00 |
| mode1  | 0x04 |
| image0 | 0x08 |
| **image3 (holds the image base)** | **0x0C** |

`GXInitTexObj`: `imageBase = (image_ptr >> 5) & 0x01FFFFFF; SET_REG_FIELD(0x24A, t->image3, 21, 0,
imageBase)` — the image base lives in **image3 bits[20:0]** (21-bit GC field). The top byte
(bits[31:24]) holds the BP-register id, written by `GXLoadTexObjPreLoaded`
(`SET_REG_FIELD(0x408, t->image3, 8, 24, ...)`) — **must be preserved** when rewriting.
Dolphin decodes `address = texImage3.image_base << 5` (24-bit field, `BPMemory.h:1074`,
`TextureInfo.cpp:30`). Cache key = that address.

### Each consumer loads via its GXTexObj EVERY frame (so rewriting image3 redirects it)
`GXLoadTexObj(obj, id)` -> `GXLoadTexObjPreLoaded(obj, region, id)` (`0x8035ffb8`), which writes
`t->image3` to the BP register on every call. Confirmed per instance below.

| # | Effect | Consumer GXTexObj — where it lives | Binds each frame via | image3 byte addr |
|---|--------|-----------------------------------|----------------------|------------------|
| 2a | plaza water refraction `TModelWaterManager::drawRefracAndSpec` `0x8027c12c`; **also** dash blur `TAfterEffect::perform` `0x8022d4f8`, shimmer `TShimmer::perform` `0x8019f83c`, underwater filter `TMapObjWaterFilter::perform` `0x801ea840` | **ONE shared `JUTTexture`** = the "スクリーンテクスチャ" `TScreenTexture::unk10` (`gpScreenTexture->unk10`). Its `GXTexObj mTexObj` is at `JUTTexture+0x00` (`JUTTexture.hpp`). Water caches it as `unk5D34` (`ModelWaterManager.cpp:194` `= search<TScreenTexture>("スクリーンテクスチャ")->getTexture()`). | water `unk5D34->load(GX_TEXMAP0)` (`:1474`); JUTTexture::load -> `GXLoadTexObj(&mTexObj,id)` | `JUTTexture + 0x0C` |
| 2b | mirror surface (`Map/MapMirror`) | `TMirrorCamera::unk60` (a `GXTexObj` at `MapMirror.hpp` offset 0x60 of the cam) | `GXLoadTexObj(&unk60, GX_TEXMAP0)` (`MapMirror.cpp:38`) | `mirrorCam + 0x6C` |
| 2c | "graffito check" | mImagePtr never set (stays null) -> GXCopyTex is a no-op | — | n/a |
| 2d | graffiti coverage ×N (pollution count) | per-layer GXTexObj built from the layer `ResTIMG` (`MarDirectorInitECT.cpp:52-71`; `PollutionCount.cpp` `GXLoadTexObj`) | `GXLoadTexObj(&GStack..., id)` (`PollutionCount.cpp:86,448`) | (transient stack GXTexObj rebuilt each draw from the ResTIMG) |

**The decisive simplification:** the native fix does NOT need any of these consumer GXTexObj
addresses statically. It matches by encoded image base at the single `GXLoadTexObjPreLoaded` funnel
(see §2), so it covers 2a's four consumers (one shared JUTTexture), 2b's mirror, and 2d's graffiti
(even though 2d rebuilds its GXTexObj on the stack each draw — the match is on the encoded base, not
the object address) — with no enumeration risk.

---

## 2. The native fix — intercept the two GX funnels (in `efb_native.cpp`)

Instead of rewriting `TEfbCtrlTex+0x2C` (which would need mutate+restore and a third override on the
already-double-owned `0x802f8bac`), the fix intercepts the two GX register-writer funnels, swapping
to ALT only while the in-between is being drawn:

1. **Copy side — `GXCopyTex` `0x8035ee5c`** (`GXFrameBuf.c:452`, `phyAddr = dest & 0x3FFFFFFF`,
   dest = the `mImagePtr` arg): on the in-between, if `gpr[3] (dest)` is a tracked orig copy-dest,
   swap `gpr[3] -> dest ^ 0x00400000` before calling the body. Fully transient — only the register
   arg changes, no guest state mutated.

2. **Consumer side — `GXLoadTexObjPreLoaded` `0x8035ffb8`** (the single image3->BP writer): on the
   in-between, if `obj->image3` low-21 base matches a tracked orig base, rewrite `obj->image3` to the
   ALT base (preserving bits[31:21]), call the body (sends ALT to the GPU), then RESTORE image3 so
   the guest GXTexObj is byte-identical for the next real field.

`track_copy(tefbctrltex_ptr)` records the orig `mImagePtr` (read at `+0x2C`) + its encoded base into
a small per-frame set. `begin/end_inbetween` arm/disarm the swaps and clear the set.

ALT base XOR: `encode_base(orig) ^ (0x400000>>5) = encode_base(orig) ^ 0x20000`, which equals
`encode_base(orig ^ 0x400000)` (XOR is linear through the >>5 since bit 22 maps to bit 17, inside the
21-bit field). Verified arithmetically.

---

## 3. Alt-address decision: `ALT = orig ^ 0x00400000`

- **RAM-backed or VRAM-cache?** With `GFX_HACK_SKIP_EFB_COPY_TO_RAM` = **true** (Dolphin default,
  `GraphicsSettings.cpp:198`) and copy-to-VRAM supported (Vulkan/OGL), non-XFB EFB copies compute
  `copy_to_ram = !bSkipEFBCopyToRam || !copy_to_vram = false` (`TextureCacheBase.cpp:2195`). So these
  copies write **NO guest RAM** — they are pure VRAM texture-cache entries keyed by `destAddr`
  (`BPStructs.cpp:246` `destAddr = copyTexDest << 5`). This is exactly the XFB situation the proven
  XFB alt-address fix relies on (CLAUDE.md "interp60-object-level").
- **Therefore ALT = orig ^ 0x00400000 is safe:** it writes no RAM, only forms a distinct cache key.
  `0x400000 >> 5 = 0x20000` fits the 21-bit GC image-base field and the 24-bit Dolphin field; XOR of
  bit 22 keeps the address inside MEM1 (orig is a MEM1 heap/ResTIMG address < 0x01800000 physical).
  No collision with the real field's texture (the whole point) and none with other tracked origs
  unless two origs differ by exactly 0x400000 (not observed; the screen tex, mirror tex, and each
  graffiti ResTIMG are distinct heap allocations far apart). The matcher uses exact-orig compare for
  the copy side, so even a hypothetical alias would not cross-redirect.
- **⚠ Caveat for the doc-reader:** if EFB-copy-to-RAM is ever ENABLED (EFBToTextureEnable off, or
  copy-to-VRAM unsupported), the copy would memcpy `covered_range` bytes to ALT and `orig^0x400000`
  could clobber live heap. In that case reserve a dedicated scratch address (one per tracked
  instance) instead of the XOR. The current default config does not hit this. The file documents
  this in its header.

---

## 4. EXACT integration edits for the main session (to `runtime/overrides/interp_redraw.cpp`)

Three call-sites. All in `runtime/overrides/interp_redraw.cpp`. (efb_native.cpp exposes the four
C-ABI symbols; declare them once near the top of interp_redraw.cpp's anonymous-namespace externs.)

### Edit A — declare the helpers (after the existing `extern "C" void func_...` block, ~line 74)
```cpp
// Native per-field EFB-copy textures (runtime/overrides/efb_native.cpp): give the in-between its
// own EFB-copy textures so screen-space effects render at true 60fps (docs/re_notes/efb_native_60fps.md).
extern "C" void sb_efb_native_track_copy(u32 tefbctrltex_ptr);
extern "C" void sb_efb_native_begin_inbetween();
extern "C" void sb_efb_native_end_inbetween();
```

### Edit B — record real-field copies in the existing `ov_efbctrltex_perform` (the natural seam)
The existing override at `0x802f8bac` (interp_redraw.cpp:108) is the per-instance copy hook. On the
REAL field (not the in-between), when the copy phase is about to run, record this instance's copy
dest. Change:
```cpp
SUNBRIGHT_OVERRIDE(ov_efbctrltex_perform, 0x802f8bacu) {
    if (g_interp60_in_redraw && g_i60.skip_efbcopy) {
        g_i60.efbcopy_skipped++;
        cpu.gpr[4] &= ~0x8u;          // drop the EFB->screen-texture copy; keep setup
    }
    func_802f8bac(cpu);
}
```
to:
```cpp
SUNBRIGHT_OVERRIDE(ov_efbctrltex_perform, 0x802f8bacu) {
    if (!g_interp60_in_redraw && (cpu.gpr[4] & 0x8u))
        sb_efb_native_track_copy(cpu.gpr[3]);   // record real-field copy dest (this=gpr[3])
    if (g_interp60_in_redraw && g_i60.skip_efbcopy) {
        g_i60.efbcopy_skipped++;
        cpu.gpr[4] &= ~0x8u;          // (legacy b1 probe; leave skip_efbcopy default 0)
    }
    func_802f8bac(cpu);
}
```
Note `cpu.gpr[3]` is the `TEfbCtrlTex* this`; track_copy reads `+0x2C` itself. Keep
`g_i60.skip_efbcopy` at its default 0 (the native fix replaces b1; do NOT also drop the copy).

### Edit C — bracket the in-between draw loop with begin/end
In `ov_interp_endRendering`, around the `kDrawLists` re-issue loop. The loop starts at the
`for (u32 li = 0; ...)` near line 308 (after `if (g_i60.mode == 3 && !g_i60.is_cut)
interp60_blend_registry();`) and the in-between draw ends after `call_ppc(cpu, GX_INVALIDATE_TEXALL);`
(line 327) where `g_interp60_in_redraw = false;`.

Add `sb_efb_native_begin_inbetween();` immediately BEFORE `g_interp60_in_redraw = true;` (line 299),
and `sb_efb_native_end_inbetween();` immediately AFTER `g_interp60_in_redraw = false;` (line 329).
Concretely:
```cpp
    sb_efb_native_begin_inbetween();    // <-- ADD (arm copy/consumer redirect to ALT)
    g_interp60_in_redraw = true;
    if (g_i60.mode == 3 && !g_i60.is_cut) interp60_blend_registry();
    for (u32 li = 0; li < sizeof(kDrawLists)/sizeof(kDrawLists[0]); li++) {
        ...
    }
    g_i60.cur_list = -1;
    call_ppc(cpu, GX_INVALIDATE_TEXALL);
    if (sil_mgr) g_i60.sil_after = mem_rf32(sil_mgr + 0x48);
    g_interp60_in_redraw = false;
    sb_efb_native_end_inbetween();      // <-- ADD (disarm + clear the tracked set)
```
`begin` must wrap the WHOLE in-between draw (the copy `&0x8` runs inside the `+0x1C`/`+0x38`/`+0x3C`
lists and the consumer loads run in the water/dash/etc. draws across `+0x1C`/`+0x24`), so begin
before the loop and end after `GXInvalidateTexAll`. The second `endRendering` copy (the alt-XFB
present) runs AFTER `end_inbetween`, which is correct — it is the XFB present, not an EFB-feedback
copy, and is already handled by the existing alt-XFB machinery.

### Optional Edit D — a debug counter in `runtime/interp60.h`
Not required (efb_native.cpp keeps its own counters, readable via `sb_efb_native_status`). If a
`/interp60` line is wanted, add to `struct Interp60Dbg`:
```cpp
    // native per-field EFB-copy (efb_native.cpp)
    unsigned long efb_native_copies = 0, efb_native_loads = 0;
```
and in `interp60_probe` print `sb_efb_native_status(...)` (declare
`extern "C" int sb_efb_native_status(char*, int);`) into the output buffer. Purely cosmetic.

---

## 5. Why this is faithful (not a bandaid)
On hardware each effect is one EFB copy + one consumer draw per game frame at the address the engine
chose. Here BOTH the real and the in-between run the engine's own copy + consumer draw against their
own EFB; the only thing we change is the destination/source address of the in-between's pair, so the
two fields do not alias in the texture cache. No magic constant tuning the look, no skipped copy, no
frozen surface, no special-cased input — the in-between is a full, correct, independently-addressed
per-field render. This is strategy (a) done correctly (per-field copies), which the user requested.

## 6. A/B + verification
- `SUNBRIGHT_EFB_NATIVE=0` disables the redirect (A/B against the flicker). Default ON.
- After integration, `sb_efb_native_status` should show `track_calls > 0` (real-field copies
  recorded), `copies_redirected > 0` and `loads_redirected > 0` per in-between (the redirect fires).
  `copies_redirected == 0` while `track_calls > 0` ⇒ the GXCopyTex funnel isn't hit on the
  in-between (check the kDrawLists membership of the copy `&0x8` link). `loads_redirected == 0` while
  `copies_redirected > 0` ⇒ the consumer binds a GXTexObj whose image base != the tracked orig
  (re-check setTexAttb timing / the consumer path).
- User verifies 60fps water/mirror/graffiti with no flicker, HEADED.

## 7. Open / to verify at integration time
- Confirm the copy `&0x8` link and the water `&0x80` consumer link are actually in the lists the
  in-between re-issues (`kDrawLists = {0x40,0x38,0x3C,0x1C,0x20,0x24}`) — they are per
  `efb_dynamic_texture_chain.md §4`, but the loaded `/data/PerformLists.bin` is authoritative.
- 2d graffiti: confirm the pollution counter (a 30Hz measurement) is not skewed by the in-between
  coverage copy now landing at ALT. The counter reads on the real field's path (orig), so it should
  be unaffected; verify if a graffiti-heavy scene mismeasures.
- If two tracked origs ever differ by exactly 0x400000 (none observed), the consumer-side base match
  could alias; the copy side is exact-orig so it won't. Reserve distinct scratch alts if it occurs.
