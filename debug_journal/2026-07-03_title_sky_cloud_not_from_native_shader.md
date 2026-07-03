# Title screen sky top-left residual: white clouds are NOT from `native_sky_fill`
Date: 2026-07-03  |  Branch: main  |  Head: (this commit)

## Symptom
`title_overbright.py` top-left cell (0,0) SIGNED delta = **[-142, -88, -24]** RGB —
i.e. sms-boot's top-left is much whiter/brighter than the oracle's clean blue sky
(oracle ≈ (80, 155, 225), native ≈ (222, 243, 249)). Handoff blamed native's fBm cloud
shader in `gx_sdlgpu.cpp` and asked for tuning.

## What I tried
Retuned `kNativeSkyFs` — pushed threshold 0.48→0.58→0.66, cut mix strength 0.85→0.65,
narrowed the cloud band. **Cell (0,0) delta did not move by even one unit.** Suspicious.

## Proof the sky shader is NOT the culprit
Replaced the entire fragment shader body with `vec3 rgb = vec3(1.0, 0.0, 0.0);` (pure
red). Frame PPM md5 CHANGED (confirms shader is executing), and native's sky region
turned bright red. **BUT the white cloud pattern was STILL visible on top of the red.**

Then ran with `SB_SKIP_RANGE=0-999` (drop every scene batch) + the red shader:
result = pure red sky, NO white cloud anywhere. `scratch/screenshots/sbs_title.png` at
that moment showed pure red with only 2D imm HUD (Select data / New / A/B/C / OPTIONS
/ zzz) surviving.

## Conclusion
**The mid-frame white cloud pattern over native's sky is drawn by SCENE BATCHES,
not by `sb_native_sky_paint()`.** Retuning the fBm shader can never close cell (0,0).

## Falsification of prior memory
Memory `sky-bmd-batches-offscreen-2026-07-03` claims "sky.bmd batches all project
off-screen; native full-screen sky gradient LANDED". That memory is either wrong or
scoped narrower than sky.bmd — SOMETHING projects on-screen and paints white clouds
over native's sky. Candidates:
- sky.bmd batches DO project on-screen at least partially (memory stale/wrong).
- A different sky/cloud actor (TSkyCloud? TMap backdrop) draws the puffs.
- The soft-focus / EFB composite quad (`b76` / shaderKey hi32 `eb5c8e74`) paints white
  clouds via its tex sample. Batch b76 has been extensively RE'd but its texture
  content in this scene wasn't re-checked after the sea-mirror skip.

## Bisection status (INCOMPLETE)
- `SB_SKIP_RANGE=0-999` → all scene batches skipped → NO clouds (RED sky).
- `SB_SKIP_RANGE=0-43` → half skipped → CLOUDS STILL PRESENT. So cloud draws sit in
  batch index range **44..86**.

Did NOT bisect further per manager directive: extend the tool to auto-name the
divergent batches' owner (drawbuf name / ndcY-in-sky-region / texmean = near-white),
rather than binary-searching batch indexes by hand.

## Next-session tool extension (Barış manager directive 2026-07-03)
Instead of manual `SB_SKIP_RANGE` bisection, extend `SB_BATCH_DBG` (native/render/
sms_boot_present.cpp:603–671) to auto-flag "candidate sky/cloud divergence" batches:
- Compute per-batch 4×4 cell coverage from ndc bounds (already computed as
  `ndcX[..]` / `ndcY[..]`).
- Auto-classify each batch: "top-row" (ndcY.min < -0.4) AND "near-white" (mean rgb
  > 0.75) AND either untextured or textured with texmean near-white → flag as
  cloud-candidate.
- Print `drawbuf="<name>"` at the top of the flagged entries so the OWNER is right
  there (memory `title-vs-scenario-select-taxonomy` says title = gameplay stage 15;
  drawbuf names should include TSky-related nodes).
- Bonus: also print `key=` hi32 so it can be piped straight to `SB_SKIP_KEY=<hex>`
  for a one-command "does dropping this batch close cell (0,0)?" verification.

Then a single `SB_BATCH_DBG=-1 tools/render/title_sbs.sh` yields the origin, no
manual bisection needed.

## Water color residual (parked, Task #2)
Still pale cyan on native vs oracle's rich turquoise. Not touched this session.
Same tool extension will help — flag "row-2 bright-cyan-diverging" batches.

## Falsified handoff scope
Handoff called cloud tuning the "dominant residual" attack. Retuning was a dead end.
Real fix path = identify the scene-batch OWNER and either port PC-native (per the
2026-07-03 no-emulation-chasing rule) or gate off if it's a mis-projected sky.bmd
batch that was supposed to project off-screen but doesn't.

## Files touched
- `native/render/gx_sdlgpu.cpp:544..550` — cloud shader tuned (retained as slightly
  sparser than before, no metric win, but not worse either). No functional fix.
- `debug_journal/2026-07-03_title_sky_cloud_not_from_native_shader.md` — this file.
- Handoff written to `scratch/handoff_title_cloud_batch_hunt.md`.
