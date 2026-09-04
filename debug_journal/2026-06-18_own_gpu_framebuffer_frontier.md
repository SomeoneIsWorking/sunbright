# Own the GPU / framebuffer — EFB-readback effects on ngx (frontier, RE'd, building)

2026-06-18, user directive: "gpu and framebuffer." The last big native-engine gap. Under ngx present,
ngx renders the scene and Dolphin's EFB is empty (ngx skips the guest GX draws). The guest's
EFB-readback effects therefore read nothing and misbehave. Goal: make ngx's rendered framebuffer
(color + depth) the EFB the guest reads back.

## The EFB-readback surface (recovered from GMSE01 cross-references)
| guest fn | addr | consumer | reads | effect |
|---|---|---|---|---|
| GXPeekZ      | 0x8035dcf0 | TSunModel::getZBufValue (8002ea70) | DEPTH at (x,y) | sun occlusion → lens-flare/glow |
| GXPeekARGB   | 0x8035dccc | TMario::drawSyncCallback (8024d25c) | COLOR at (x,y) | Mario color sample |
| GXCopyTex    | 0x8035ee5c | TEfbCtrlTex::perform, TBathWaterMeshRenderer ×3, draw_mist, drawMantaShadow, Hx_GetFrBuffer | EFB region → guest TEXTURE | mirror / bathwater reflection / mist / manta shadow / heat-haze |
(GXSetTexCopySrc 0x8035e388 / GXSetTexCopyDst 0x8035e48c set the copy rect+dst before GXCopyTex.)

## getZBufValue (the first vertical slice) — RE'd
Loops 17 times (cmpi r26,0x11): per sample point it reads (x,y) as two lha from a table at this+0xB4
(x) / this+0x180 (vis-byte out), calls `GXPeekZ(x, y, &localZ)` (r5=&localZ on stack), then compares
localZ: `addis r0,z,0xff01; cmpli r0,0xffff` ⇒ tests z against ~0xFFFFFF (far / nothing drawn = sun
visible). Stores a visibility byte per point. Net: count of unoccluded sample points → glow strength.
So GXPeekZ must return ngx scene depth in **GC 24-bit Z** (0xFFFFFF = far/empty).

## Architecture (the key constraint + the plan)
TIMING: the guest calls GXPeek*/GXCopyTex MID-FRAME (after issuing the scene draws, before ngx renders
at present on the video thread). So ngx's framebuffer for THIS frame doesn't exist yet when the guest
reads. SERVE FROM THE PREVIOUS FRAME's ngx framebuffer (1-frame lag — imperceptible for occlusion/
reflection; the sun & camera barely move per field). This is the pragmatic, correct-enough design.

PRIMITIVE: after PresentRenderer renders the 3D scene (depth+color valid, before HUD), GPU→CPU read
back the depth (and later color) target into a host buffer, double-buffered, published under a mutex.
The guest-thread overrides read the published (last-frame) buffer.

VERTICAL SLICE 1 — GXPeekZ from ngx depth (sun occlusion):
1. PresentRenderer: copy depth_img (D32_SFLOAT) → host-visible staging buffer after the 3D pass;
   keep a CPU float depth array (res_scale·640 × res_scale·448), publish at frame end (double-buffer).
2. Override GXPeekZ (0x8035dcf0) purejit-safe: map guest EFB (x,y) → ngx coords (×res_scale), sample
   the published depth d∈[0,1], convert to GC Z = round(d · 0xFFFFFF), write to *r5 (big-endian u32).
   (If no frame yet → return 0xFFFFFF = far so the sun reads "visible", a safe default.)
3. Verify: Delfino has a sun; getZBufValue→GXPeekZ should fire. Check the override fires + returns a
   spread of z values (occluded points < 0xFFFFFF behind geometry, sky points = 0xFFFFFF). Sun glow
   should fade behind buildings. (Headless: log z histogram; headed: user sees the flare occlude.)

SLICE 2 — GXPeekARGB from ngx color (same readback, color target).
SLICE 3 — GXCopyTex: copy the ngx color region into the guest texture (TMEM/guest-RAM dst that the
effect's later GX draw — itself ngx-captured — samples). Biggest/most-reused; do last. Needs the
color readback + writing GC-tiled texels into the guest copy-dst, OR capturing the dst and having ngx
sample its own color target as the texture when that tex is later bound. The latter (ngx-internal
render-to-texture aliasing) avoids a full guest-RAM round-trip — preferred.

## Notes / gotchas
- Depth convention: confirm ngx's projection depth direction (near=0/far=1 vs reverse) so far maps to
  0xFFFFFF. The pollution pass uses VK_COMPARE_OP_GREATER — check vk_mesh depth setup.
- res_scale: SUNBRIGHT_RES_SCALE (default 3) — ngx targets are scale× the guest 640×448. Map coords.
- Full-frame readback per present is a real cost + GPU sync; acceptable first cut. Optimize later to a
  region (GXSetTexCopySrc rect) or on-demand. The depth readback can be downsampled (point sampling).
- These run under no-recomp: GXPeek*/GXCopyTex overrides must be purejit-safe (mark + static-init).

## SLICE 1 PROGRESS (2026-06-18) — depth readback BUILT, NOT yet verified firing
Built the foundation (committed):
- PresentRenderer: depth_img += TRANSFER_SRC; a host-visible depth staging buffer (resized w/ target);
  after the render pass, if armed (g_efb_want>0) barrier depth→TRANSFER_SRC + vkCmdCopyImageToBuffer,
  then after the fence map+publish g_efb_depth[] (mutex, 1-frame lag). Lazy: zero cost until armed.
- extern "C" sb_ngx_efb_request_readback() / sb_ngx_efb_peek_depth(gx,gy,&d) (guest EFB 640×448 →
  res-scaled readback coords).
- runtime/overrides/efb_readback_native.cpp: GXPeekZ (0x8035dcf0) purejit-safe full-replacement →
  arms the readback + returns ngx depth as 24-bit GC Z (1.0→0xFFFFFF far). SUNBRIGHT_DBG_EFB logs.
Builds clean, boots Delfino (emu 20), NO crash/validation error.

⚠ NOT VERIFIED firing: GXPeekZ logged 0 calls in fastboot Delfino (emu 16–20, 6 presents). The
override is correct + safe (dormant if uncalled; strictly better than Dolphin's empty-EFB read if
called), but end-to-end (sun glow occludes) is UNCONFIRMED. THE REAL BLOCKER (RE'd): GXPeekZ is
called from TSunMgr::**drawSyncCallback** (0x8002e270) — a GX DRAW-SYNC CALLBACK invoked via the
TDrawSyncManager token mechanism (virtual; --xref finds only direct branches, so it shows the
adjustor thunk only). Under ngx present the guest GX draws are skipped, so the drawsync token that
would fire this callback likely never reaches Dolphin's FIFO → the callback never runs → getZBufValue/
GXPeekZ never run. So the sun-occlusion path is gated on **GX-drawsync-callback delivery under ngx**,
not just on EFB readback. NEXT: confirm whether drawSyncCallback fires under ngx (instrument it /
TSunMgr::perform 0x8002e2d0); if not, the fix is to deliver the guest's drawsync callbacks under ngx
(the tokens are issued even when ngx owns the draw) — a prerequisite for the whole EFB-readback family.
Also: the sun may simply be off-screen/inactive in the default fastboot plaza camera — verify in a
sun-facing view before concluding the callback is dead.

## SLICE-1 BLOCKER FALSIFIED + SLICES REORDERED (2026-06-18, session 2) — READ THIS
The previous session's "blocker" (GX-drawsync-callback delivery is dead under ngx) is **WRONG**.
Instrumented the chain headless (fastboot Delfino, ngx present; observers in efb_readback_native.cpp
gated on SUNBRIGHT_DBG_EFB, run-original-around so the real path still runs):
- **TSunMgr::drawSyncCallback (0x8002e270) FIRES every frame** (1000+/run), unk14=1. TSunMgr::perform
  fires too. So the TDrawSyncManager token mechanism DOES deliver guest drawsync callbacks under ngx.
- **GXPeekZ still logs 0** — not because the callback is dead, but because TSunModel::getZBufValue
  skips every GXPeekZ: all 17 sun sample points (gpSunModel+0xB4, the s16 screen positions) are
  **(-1,-1)**. calcDispRatioAndScreenPos_ DOES run (the +0xF8 float positions are perturbed off the
  init (10000,10000)), but CLBCalc2DFPos projects the sun center to the off-screen sentinel
  (10000,10000) → CLBScreenFPosToSPos clamps to (-1,-1). **The sun is simply off-screen** in the
  default fastboot plaza camera, and camera yaw (cright ×8) does NOT bring it into frame (it's high in
  the sky; the plaza cam won't tilt up). gpSunModel ptr is always at the fixed SDA slot [0x8040d0c8].
  ⇒ GXPeekZ (sun occlusion) is the WORST verification target headless. Slice-1 primitive is built +
  verified at the readback level (below) but its consumer can't be exercised in fastboot plaza.
- **Dolphin's EFB IS empty under ngx present — CONFIRMED** (not just assumed): /abshot2 GX side
  (ab2.gx.ppm = Dolphin's GX XFB) is **100% black (mean 0.0, all 286720 px)** while ngx is a real
  image (mean 103). So the premise of this whole frontier holds; EFB-readback consumers read black.
- **Depth readback PRIMITIVE VERIFIED working**: forced it armed every present under DBG_EFB +
  histogram (ngx_present.cpp). Captures 640×448 depth, real hyperbolic perspective spread
  (min 0.9785, max 1.0, geometry crammed in [0.9,1.0] as expected for near≈1/far≈300000 GC proj;
  1576 px exactly 1.0 = true-far sky). GPU→CPU copy + publish path is sound.

### THE LIVE CONSUMERS (what actually fires in the default plaza — the real targets)
- **GXPeekARGB (0x8035dccc) FIRES every frame** at screen-center (~320,224) from TMario::
  drawSyncCallback. BUT it reads the **ALPHA** byte: `(argb & 0xff000000)==0x10000000` → Mario tagged
  alpha 0x10 == not-occluded (MARIO_FLAG_OCCLUDED). Serving it needs ngx's framebuffer ALPHA to carry
  the GC 0x10 Mario tag — i.e. ngx must faithfully output Mario's TEV alpha. Verify what ngx alpha is
  at (320,224) before assuming a color readback suffices.
- **GXCopyTex (0x8035ee5c) FIRES ~2189×/22s** — by far the most active; mix of clear=0/clear=1, dsts in
  MEM1 (0x80xxxxxx) and some low (0x01xxxxxx — classify these). The mirror/bathwater/mist/manta/
  heat-haze family. Reads back COLOR (clean — ngx has color, no alpha-tag subtlety). Most impactful.

### REVISED slice order (was GXPeekZ→ARGB→CopyTex): build the COLOR readback next (foundational for
both ARGB and CopyTex), probe ngx's framebuffer ARGB at (320,224) to see if Mario's 0x10 alpha is
present. If yes → GXPeekARGB is the cleanest live end-to-end slice. Then GXCopyTex (the big one).
GXPeekZ stays built/dormant (correct + safe; just unexercised until a sun-facing scene).

## COLOR READBACK BUILT + GXPeekARGB BLOCKED ON ALPHA FIDELITY (2026-06-18, session 2)
Built the COLOR readback (ngx_present.cpp, mirror of the depth one): after the render pass, barrier
the present color target SHADER_READ_ONLY→TRANSFER_SRC, vkCmdCopyImageToBuffer → host staging, barrier
back; publish packed ARGB8888 to g_efb_color[] (mutex, 1-frame lag). extern "C"
sb_ngx_efb_request_color() / sb_ngx_efb_peek_color(gx,gy,&argb). Forced-armed under DBG_EFB + an
ARGB/alpha histogram. Builds, runs, **0 Vulkan validation errors**.
- center(320,224)=ffd0cdd9, (160,150)=ff10c710 — real scene color, RGB faithful.
- **ALPHA across the whole 640×448: =0x10: 0 px, =0xff: 119464, other: 167256.** ngx's framebuffer
  alpha NEVER equals 0x10. TMario::drawSyncCallback's occlusion test is `(argb&0xff000000)==0x10000000`
  (Mario tagged alpha 0x10). ngx does NOT reproduce the GC EFB alpha-tag convention → **GXPeekARGB
  occlusion cannot be served by a plain color/alpha readback.** It needs ngx to faithfully output
  Mario's TEV/PE alpha (0x10) into the present alpha channel — a separate ngx-alpha-fidelity task.
  ⇒ GXPeekARGB stays diagnostic-only (do NOT wire it to the color readback — would force always-
  occluded). Owning ngx EFB-alpha is its own frontier item.

## GXCopyTex RE — the EXACT plaza target (2026-06-18, session 2)
TEfbCtrlTex::perform (JDREfbCtrl.cpp) is the canonical EFB→texture copy: GXSetTexCopySrc(rect) +
GXSetTexCopyDst(w,h,fmt,mip) + GXCopyTex(mImagePtr, doClear). mImagePtr = the guest texture data ptr
of a GXTexObj later bound by a J3D material → that surface samples the captured framebuffer.
Instrumented the live plaza (GXSetTexCopySrc 0x8035e388 / GXSetTexCopyDst 0x8035e48c / GXCopyTex
0x8035ee5c, stash+join). In default fastboot Delfino there is **exactly ONE** clear=0 readback geometry,
recurring ~per-frame (~19/s):
  **src rect [l=0 t=0 w=640 h=448] (full screen) → dst tex 320×224 fmt=4 (GX_TF_RGB565), dst=0x01037bc0**
dst 0x01037bc0 is a PHYSICAL addr → guest virtual **0x81037bc0** (MEM1; |0x80000000). Read it live: it
is **ALL ZEROS** — GXCopyTex reads Dolphin's empty EFB and writes BLACK into the effect's texture
(the bug, concretely). (A full-screen→half-res RGB565 per-frame capture = a full-screen post effect;
which surface samples 0x01037bc0 not yet pinned, but irrelevant to the writeback correctness.)

### The slice (writeback approach — self-contained, reuses ngx's existing decoder):
GXCopyTex override (purejit-safe, clear=0): take ngx's published color (g_efb_color 640×448),
box-downsample to dst_w×dst_h, encode GC-tiled in dst fmt (RGB565 = 4×4 tiles, BE u16), write to
phys|0x80000000. ngx's texture_for() then decodes it from guest RAM as usual for the sampling surface.
DETERMINISTIC VERIFY (no eyeballing): 0x81037bc0 goes all-zero → non-zero texels; ngx's own RGB565
decoder round-trips them back to the scene. Honor doClear (clear=1 path stays Dolphin/no-op as now).
Generalize formats later (RGB5A3/RGBA8/I8/IA8) — RGB565 is the only one live in plaza.

## GXCopyTex WRITEBACK BUILT + VERIFIED BYTE-CORRECT (2026-06-18, session 2)
runtime/overrides/efb_readback_native.cpp — real overrides (gated on SUNBRIGHT_NGX_PRESENT) on
GXSetTexCopySrc (0x8035e388) / GXSetTexCopyDst (0x8035e48c) / GXCopyTex (0x8035ee5c). Capture the
src rect + dst dims/fmt; on GXCopyTex run the original (sb_run_original_around, keeps Dolphin GP
consistent) then in the after-callback OVERWRITE the dst texture with ngx scene color:
sb_ngx_efb_copy_region (ngx_present.cpp) box-downsamples g_efb_color (640×448) → dst dims; the
override GC-tiles it (4×4 BE u16) + pixel-encodes per dst fmt, writes to (phys&0x3FFFFFFF)|0x80000000.
Served formats: **GX_TF_RGB565 (4)** + **GX_TF_RGB5A3 (5)** — the two live plaza copy formats; others
fall through to original-only (no regression). 1-frame lag (inherent — guest peeks mid-frame).
VERIFIED byte-correct (round-trip self-check, hand-checked vs the shipping tex_decode.cpp inverse):
  RGB565  ea=81037bc0 320×224 (full screen): src cdc8da → stored 0xCE5B ✓, nz≈71655/71680
  RGB5A3  ea=810f5380 256×256:               src fb8e00 → stored 0xFE20 ✓, 656d3e → 0xB1A7 ✓, nz≈65409/65536
Both copies were ALL-BLACK before (Dolphin empty EFB); now hold the ngx scene. 0 Vulkan validation
errors, no crash. The COLOR readback primitive (g_efb_color + sb_ngx_efb_copy_region) is the foundation.

### END-TO-END WIRED — texcache invalidation done (2026-06-18, session 2)
The texcache DID key by address with NO dirty tracking (texture_for line ~730 returns the cached view
immediately) AND never evicted (grew unbounded) — so an EFB-copy texture at a fixed address that
changes every frame would serve the stale first/black decode. FIXED: TexEntry gains `addr`; the
GXCopyTex override calls sb_ngx_efb_invalidate_tex(ea) after writing; render() at frame start swaps
out the dirty MEM1-offset set and evicts (destroy img/mem/view + erase) the matching texcache entries
so texture_for re-decodes them from the freshly-written guest RAM. Eviction at frame start is safe
(prior frame's fence already waited → its images are free).
VERIFIED: "[efb] texcache evict: 2 dirty, 1 entries re-decoded" — a ngx-rendered surface samples the
EFB-copy texture and now re-decodes the LIVE scene each frame (was the stale black upload). 0 Vulkan
validation errors, no crash, stable 21s. The full chain is closed: ngx color readback → box-downsample
→ RGB565/RGB5A3 GC-tile → guest RAM → invalidate → re-decode → sampled by the effect surface.
(Only 1 of the 2 dirty addrs is in the texcache → only one EFB-copy effect's surface is ngx-captured
right now; the other's sampling surface isn't bound/captured this scene — expected, not a bug.)

### CONSUMER IDENTIFIED + VERIFIED END-TO-END (2026-06-18, session 2, user: "finish the consumer")
The screenspace effect consuming the full-screen RGB565 capture (0x01037bc0) is **3D batch 155,
texmap1** — a 159-vertex near-full-screen mesh (NDC x[-1,1] y[-1,0.75]) sampling the framebuffer
capture on the SECOND texture unit = a **screenspace refraction** (the plaza WATER surface sampling
the scene behind it; texmap0 = its distortion/base, texmap1 = the EFB capture). NOT a J2D quad — found
via a batch-scan diagnostic (texture_for is also called from prepare_j2d, but the consumer is 3D).
The 256×256 RGB5A3 capture (0x010f5380) has no live consumer in the default plaza view (mirror, likely
off-screen) — invalidated but unsampled, harmless.
VERIFIED end-to-end with a single-process A/B: `/efbcopy?on=0|1` toggles the writeback at runtime
(g_sb_efb_copy_on; OFF still invalidates so the effect re-decodes the original's black). Diff ngx(ON)
vs ngx(OFF): sky/HUD band (rows 0..56) = 0.0 (unaffected), change localized to **rows 264..294**
(NDC y ≈ -0.18..-0.31, lower-center) = exactly the water surface. The writeback changes the image ONLY
where the water is → the screenspace refraction now consumes the LIVE scene (was black). Modest delta
(~8/px) because water refraction is a translucent blend, as expected. Tools kept: /efbcopy A/B toggle,
DBG_EFB consumer-scan (3D + J2D) + evict-addr log.

### Remaining (not blocking):
- Other copy formats (RGBA8 fmt=6 / I8 / IA8 / CMPR) if other scenes use them — RGB565+RGB5A3 are the
  only live plaza formats; others fall through to original-only (no regression).
- DONE: extracted the tiling+encode into runtime/render/ngx_efb_copy.h (the SHIPPING functions
  copytex_writeback calls) + a render_test unit `efb_copy` — round-trips encode→GC-tile→sb_tex_decode
  for RGB565+RGB5A3 within quantization + a tile_offset16 spec-check. 8/8 render_test units pass.
- GXPeekZ (sun, off-screen in fastboot) + GXPeekARGB (needs ngx alpha-tag fidelity) still dormant.

## NEXT PORT SCOPED: immediate-mode GX geometry in ngx (2026-06-18, "keep porting")
ngx renders from the J3D object model; immediate-mode GX draws have no J3D object → ngx MISSES them.
Confirmed LIVE + verifiable in fastboot plaza: **GXDrawCube (0x803627fc) fires ~1470×/run** from two
TMario call sites (lr=8024d8fc, 8024d96c = the Mario occlusion-probe boxes, MarioMain.cpp ~199/212).
GXDrawSphere (0x80362268) = 0 calls in plaza (sky is J3D here). So the port IS verifiable via Mario
occlusion (not an off-screen-sun dead end). GXDrawCube = GXBegin(GX_QUADS, VTXFMT3, 24): 24 fixed
unit-cube corners (GXPosition3f32, ×0.577) transformed by the current pos matrix (boxDrawPrepare puts
it at Mario). The occlusion box writes ONLY alpha: GXSetColorUpdate(FALSE)+GXSetAlphaUpdate(TRUE)+
GXSetDstAlpha(ENABLE,0x10)+z-test. So ngx must render the cube HONORING the color/alpha write masks +
constant dst-alpha (else it'd paint a visible box on Mario). Chain unblocked by this: GXDrawCube→alpha
0x10 in ngx fb→GXPeekARGB (served from ngx color readback)→MARIO_FLAG_OCCLUDED→silhouette (also
GXDrawCube). Slice plan + approach in scratch/handoff_2026-06-18_immediate_mode_gx_geometry.md.
DBG_EFB now also counts GXDrawCube/Sphere ([imm] lines).

## ⇒ NEXT: GXCopyTex (0x8035ee5c) is THE viable live slice — reads back COLOR (ngx RGB is faithful),
2189×/22s in the plaza, the mirror/bathwater/mist/manta/heat-haze family. The color readback primitive
(g_efb_color + sb_ngx_efb_peek_color) is the foundation. Remaining work: classify the calls (clear=0
= the real EFB→texture readbacks; clear=1 = clears), honor GXSetTexCopySrc(rect)/GXSetTexCopyDst
(fmt,dst), and write ngx color into the guest texture as GC-tiled texels — OR the ngx-internal
render-to-texture aliasing (capture the copy-dst, have ngx sample its own color target when that tex is
later bound; avoids the guest-RAM round-trip). Verify: the mirror/water surface shows the ngx scene.

## TOOL NOTE: --xref/--callees find DIRECT branches only (not vtable/indirect/function-pointer calls).
Virtual methods (perform/drawSyncCallback/draw) show 0 or only-the-thunk. For indirect dispatch, find
the vtable or the registration site instead. (Possible future tool: scan for the function's address as
a 32-bit word in .data → vtable/callback-table references.)
