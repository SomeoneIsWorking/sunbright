# 2026-07-15 — Giant blocky PRESS-START letters (grows over idle time): swapped anim args

User report: after idling at the title, huge pixelated dark-blue letter shapes accumulate
over the logo and worsen over time (screenshot: giant blocky S/Q glyphs). Reproduced
headless (5-min SB_TURBO run, dumps every 600 presents): corrupted letter shapes appear
after attract cycles and grow each cycle (`scratch/bin/corr_grid.png`).

## Root cause

The 11 `s_0X` sparkle panes ARE the "PRESS START!" letters (11 glyphs). Their idle
twinkle loop (TCardLoad titleDraw case 4, state 3) re-arms a size animation every ~800
frames. The decomp called:

    setCenteredSize(25, w*2, h*2, w, h)   // target = 2x CURRENT bounds

i.e. every twinkle cycle DOUBLED the pane — exponential growth, giant blocky glyphs
(the letter textures are ~32px; at 8-16x they're the pixelated blobs in the report).

US disasm (titleDraw state-3 branch @0x8016c6d8): `TCoord2D::setValue` receives
**target=(w,h), start=(2w,2h)** and the immediate `J2DPane::resize` gets (2w,2h) — the
sparkle pops in at 2x and SHRINKS BACK to its current size, leaving bounds unchanged.
The decomp had target/initial swapped at THIS call site only (state 4's pop-in uses the
correct order, based on mSparkleInitialBounds). `TExPane::setPaneSize` itself was
verified against its US body @0x801798f0 (setValue(target,start) + resize(start) +
pending flag — the header's "fabricated" impl is actually correct); the swap was purely
at the CardLoad state-3 call site.

## Falsified along the way

- Ghost pass (SB_SKIP_GHOST=1 long run): corruption identical — not the ghost.
- Texture-cache aliasing (texObjId reuse): native ids are unique-incrementing; not it.
- TCoord2D convergence: CLBChaseGeneralConstantSpecifySpeed clamps at the target
  (no float-equality overshoot hazard).

## Verification

5-min turbo run post-fix: PRESS START letters twinkle at constant size across attract
cycles; no growth (`scratch/bin/longfix_montage.png`).

## Related (same session)

- Default keyboard bindings were ALL `PAD_KEY_INVALID` in aurora — an "active" keyboard
  pad did nothing. Real defaults added (aurora pad.cpp): WASD = stick, IJKL = C-stick,
  arrows = D-pad, Space=A, LCtrl=B, E=X, Q=Y, C=Z, **Enter=START**, F=L, LShift=R.
