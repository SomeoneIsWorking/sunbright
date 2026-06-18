# Own the GPU / framebuffer — EFB-readback effects on ngx (frontier, RE'd, building)

2026-06-18, user directive: "gpu and framebuffer." The last big native-engine gap. Under ngx present,
ngx renders the scene and Dolphin's EFB is empty (ngx skips the guest GX draws). The guest's
EFB-readback effects therefore read nothing and misbehave. Goal: make ngx's rendered framebuffer
(color + depth) the EFB the guest reads back.

## The EFB-readback surface (RE'd via sunbright-recomp --xref)
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

## TOOL NOTE: --xref/--callees find DIRECT branches only (not vtable/indirect/function-pointer calls).
Virtual methods (perform/drawSyncCallback/draw) show 0 or only-the-thunk. For indirect dispatch, find
the vtable or the registration site instead. (Possible future tool: scan for the function's address as
a 32-bit word in .data → vtable/callback-table references.)
