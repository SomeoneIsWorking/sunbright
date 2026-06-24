# 2026-06-24 — file-select: two render fixes + the GX pixel oracle

Continuation of the "keep working, perfect boot→file-select" line (handoff_keep_working.md),
after the ANK1 BCK swapper unblock (be3bd48). Three landings this session.

## 1. Cap texture white → setResTIMG LP64 offset sign-extend (commit 2eaa2ad)
The recurring `[texres] REJECT texNo=0 fmt=0xe 256x256 imgOff=0xffdd93a4` (2× per run) was
Mario's cap models. `MarioCap::TMarioCap` copies Mario's BODY texNo=0 (CMPR 256×256) into
ma_cap1/ma_cap3 via `J3DTexture::setResTIMG(0, *bodyTex)`, which rebases imageDataOffset/
paletteOffset into a 32-bit POINTER DELTA (`orig + &src − &dest`). On a 32-bit GC that wraps
cleanly in u32; on our LP64 host setResTIMG computes it in 64-bit then truncates to u32 →
frequently negative (0xffdd93a4 = −0x22636C). The native reader did `(char*)t + (u32)offset`
(zero-extend → +4GB garbage), so the gate rejected → cap white.

FIX (`native/render/sms_boot_material.cpp`): sign-extend `(char*)t + (int32_t)offset` (exact
inverse of the truncated delta); replace the offset-magnitude heuristic with a real host-memory
readability probe (`host_readable`: write()/EFAULT) over the computed source span before the
tiled decoder walks it — still SEGV-safe vs corrupt headers. REJECT 2→0, no crash. Same LP64
class as the file-overlay 32-bit-offset landmine. The hybrid path (ngx_j3d_shape.cpp) is NOT
affected — it reads guest BE RAM with 32-bit guest addresses.

## 2. Diagonal sea/beach dither → drop redundant drive_map() (commit 9e0614d)
The settled file-select showed a diagonal blue/white dither over the sea+beach. Found by VALUES
(SB_BATCH_DBG, not eye): `scene_drive` drew every map surface TWICE per present — `drive_map()`
(DrawBuf MapOpa/MapXlu) AND `scene->perform(0x8)` (TSmJ3DScn draws the full map). Two identical
opaque surfaces (z_write=1, GX_LEQUAL) at equal depth z-fight per pixel. 40 batches with sea/
white/ground keys each appearing 2× (b10≡b34 teal sea, b11≡b35 white, b8≡b32 ground). Bisected
with SB_NO_PERFORM / SB_NO_MAP / SB_NO_SKY gates: perform(0x8) alone draws the complete map+Mario
with no dups; drive_map is pure duplicate. drive_map was added when perform drew an empty map; the
option-camera calc pass + POS-sentinel fix since then made perform draw the full map.

FIX (`native/src/scene_drive.cpp`): remove the drive_map() call (kept [[maybe_unused]]). KEEP
drive_sky() — perform draws no full-screen sky backdrop. scene_batches 40→31, dither gone.

## 3. THE GX PIXEL ORACLE for stage-15 file-select (commit ccf229b)
`tools/render/fileselect_oracle.sh`: the MAIN sunbright build (real game under Dolphin JIT) with
SUNBRIGHT_NGX_PRESENT=0 renders via Dolphin GX = ground truth. SUNBRIGHT_STAGE=15 fastboots to
option.arc (stops at PRESS START); the script presses Start ONCE via probe /pad (NOT AUTOSTART,
which auto-selects past the screen into the opening movie), settles, dumps →
scratch/oracle/fileselect_gx_oracle.png. Cross-validated: the GX title matches sms-boot's title.

GROUND TRUTH: "Select data" banner + 3 blocks A/B/C (Corrupt/New/New) + OPTIONS; EYE-LEVEL camera
across a TAN beach at a TEAL sea, palm tree right, island left, Mario front-center.

DIVERGENCES (sms-boot vs oracle), now adjudicated:
1. CAMERA = dominant bug. sms-boot looks DOWN at a blue SEA DOME; truth = eye-level beach. The
   long-open camera-source blocker. FIX FIRST — most else is downstream.
2. Beach TAN in truth, flat WHITE in sms-boot (untextured/wrong sand).
3. Sea teal in truth, deep blue in sms-boot (vtx color IS teal; TEV outputs blue).
4. Blocks: truth has banner + A/B/C cubes + labels; sms-boot shows bare blue bars (dummy panes?).

NEXT: own the file-select (option) camera — CPolarSubCamera/ctrlOptionCamera_, driven via
gpCamera->perform(0x1) in scene_drive.cpp; its eye/target/up are wrong. Build a camera VALUE
oracle (GX-run /ngxproj or gpCamera eye/target vs sms-boot's C_MTXLookAt inputs), then beach/sea.
