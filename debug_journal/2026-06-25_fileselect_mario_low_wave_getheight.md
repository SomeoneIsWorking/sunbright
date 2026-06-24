# 2026-06-25 — File-select Mario "too low" FIXED: TMapObjWave::getHeight was a return-0 stub

## Result
On the stage-15 file-select, Mario rendered ~100 world-units too low (mostly below the
visible area, feet off the bottom of frame). Root cause: `TMapObjWave::getHeight` was a
**stub returning 0.0f** (`native/boot_stubs/movebg_stubs.cpp`). Ported it faithfully
(`reference/sms/src/MoveBG/MapObjWave.cpp`). Mario now stands front-center on the beach,
matching the GX oracle. Frame: `scratch/frames/boot_0420_marioH.png` vs
`scratch/oracle/fileselect_gx_oracle.png`.

## Root-cause chain (each step measured, not guessed)
1. `SB_SEL_POS` / `SB_MARIO_DBG`: Mario `mPosition=(950,100,-1000)` but his drawn body root
   `calcAnim baseMtx.t=(950,0,-1000)` — the Y was zeroed somewhere between position and draw.
2. `TMario::calcBaseMtx` (MarioDraw.cpp:1775 common, non-swimming path) computes
   `baseY = mPosition.y + gpMapObjWave->getHeight(x, mFloorPosition.z, z) - mFloorPosition.z`.
   Confirmed faithful to the binary: disasm of `calcBaseMtx` (0x80245450) loads `mFloorPosition.z`
   (offset 0xF0), not `.y` — the decomp's `.z` is correct.
3. `thinkWaterSurface` (MarioMove.cpp:2111): when Mario is NOT in water, `mFloorPosition.z =
   mPosition.y`. So `baseY = mPosition.y + getHeight(...) - mPosition.y = getHeight(...)`. Mario's
   drawn Y is *exactly* `gpMapObjWave->getHeight()` — he is positioned on the water/ground surface.
4. Instrumented `getHeight` (SB_WAVE_DBG): the real `gpMap->checkGroundExactY` at Mario's column
   returns `h=100.0` (the beach floor IS loaded) with BG type `0x8701` (non-water). But the STUB
   ignored all that and returned 0 → `baseY=0` → Mario sinks 100 units.

## The faithful port (RE'd via Ghidra @0x801dd568)
`getHeight(x,y,z)` = query `gpMap->checkGroundExactY(x, 50+y, z, &bg)` (50.0f = SDA2[-0x24b0]);
- **non-water BG → return y unchanged** (the file-select beach: object rests at its own world Y),
- **water-surface BG → return the surface plane height h**,
- sea-water (0x102/0x103) additionally bobs via an animated wave (getWaveHeight: `m3c·sinf(m24·x/2π+m64)
  + m40·sinf(m28·z/2π+m68)`), driven by the load()/perform()/updateHeightAndAlpha() wave state
  machine — **DEFERRED** (only exercised/verifiable in the Delfino sea; ports go in boot order).
  Until then the sea branch uses the static plane height and logs once. Not reached at file-select.

SDA2 constants resolved live from the running oracle (r2/_SDA2_BASE_ = 0x80416BA0, read via probe
`/cur` gpr2 at OSContext+0x8): SDA2[-0x24b0]=50.0, [-0x24b8]=0.15915507 (1/2π), [-0x2530]=0.0.

## Verification (numbers, not eyeballs)
`SB_MARIO_XF` settled frame 270, Mario body shapes (pkt=59): view-space root y −360 → **−260**
(+100 = exactly mPosition.y); ndcY 1.06–1.10 (off-screen bottom) → **0.62–0.82** (in frame),
matching the oracle range (oracle head≈0.52, feet≈0.93). Frame render confirms Mario standing
front-center on the beach below the A/B/C blocks, like the oracle.

## Tooling added/kept
- `SB_WAVE_DBG` was a temporary diagnostic (removed). `SB_MARIO_XF` (settled per-shape
  worldpos/ndc/tex dump) kept.
- Ghidra dumps: `scratch/decomp/801dd568.c` (getHeight), `801dd694.c` (getWaveHeight),
  `801dcc08.c` (load), `801dce60.c` (perform), `801dcf04.c` (updateHeightAndAlpha) — the latter
  three are the deferred sea-wave subsystem.

## Still divergent on the file-select (next)
- Banner "Select data" + "Corrupt/New/New" label TEXT (J2D BMG strings unresolved).
- Palm tree (oracle has a large palm at right; not prominent in sms-boot).
- The two-Mario artifact in `SB_MARIO_XF` (mdl=59 at ndcX −0.18 AND mdl=4 at −0.41) — a second
  body model instance entered; the visible frame shows one Mario, low priority.
