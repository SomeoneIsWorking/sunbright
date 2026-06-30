# 2026-06-30 — Title/file-select per-pass parity: harness built, title is CORRECT, confounds found

User directive this session: "ensure the title screen and file select completely match first — not
visually, but by doing geometry and lighting comparisons, compare individual render passes" + "Add
tests against these too (TDD first)".

## What landed (committed + pushed)
1. **HUD ported** (reference/sms 99d14ec, parent f0fe434): `TGCConsole2::perform` was an empty stub →
   ported its &8 draw branch; `drive_hud()` in scene_drive drives it. Plaza now shows the full
   coin/shine/life/FLUDD HUD (`imm_batches 7→111`). (Pre-dates the title pivot; kept.)
2. **Lensflare crash fix** (reference/sms aa0ba47): the stage-15 title/option scene SIGSEGV'd in
   `CLBCalcNearNinePos` — `TLensFlare::perform` passes `out_euler=nullptr`, the decomp wrote
   `out_euler->x` unconditionally. Fix: alias `*out_euler` to a local scratch when null (byte-identical
   for the non-null CameraBGCheck caller). Title now renders for capture.
3. **Cross-engine LIGHTING parity** (parent 15fc71e): the `SUNBRIGHT_PARITY_DUMP` ngx ORACLE emitter
   now also emits `projType`+`lights`+`amb`+`matc` (was geometry-only); `parity_sweep.py`
   `_summarize`/`_diff_summary` compare median light-count/ambient/material/projType.
   **TDD**: `tools/render/parity_sweep_test.py` (ctest `parity_sweep_logic`) — written RED then green.

## ⛔ CORRECTION (USER, 2026-06-30) — the title is NOT fully correct; do NOT judge it visually
An earlier draft of this file claimed "the title renders correctly, don't fix it" based on the PNG
(`scratch/frames/title_native_evidence.png`). **That was wrong** — the user (observing the running
system = ground truth) says the title screen does NOT fully match. I rationalized the harness data
(363 vs 1722 on-screen 3D verts, 8 vs 3 lights, white ambient) away as "confounds" to fit the image.
The data is very likely pointing at REAL divergences: the native title's 3D scene draws only ~363
on-screen J3D verts vs the oracle's ~1722 — i.e. it is probably MISSING the 3D Isle-Delfino
backdrop (island/sea/beach) that sits behind/below the logo, and/or mis-lighting it (white ambient
washes shading out). The logo+shine+palm are 2D (J2D) and render; the 3D backdrop is the suspect.
TREAT THE TITLE AS BROKEN and find the divergence with geometry+lighting comparison, NOT the eye.
(The harness LIMITATIONS in the next section are real — fix them so the comparison is trustworthy —
but they are NOT an excuse to declare the title correct.)

## ⛔⛔ THE ORACLE IS INVALID — it used NGX, which the USER says is BROKEN (2026-06-30)
The "oracle" side of the parity comparison this session used `SUNBRIGHT_PARITY_DUMP`, which is emitted
from the **ngx J3D capture** (runtime/overrides/ngx_j3d_shape.cpp) — NOT Dolphin-GX. USER: "make sure
you are not comparing against NGX. NGX is broken." So EVERY cross-engine number below (363 vs 1722
verts, 8 vs 3 lights, ambient) is sms-boot-vs-BROKEN-ngx and is NOT a valid measure of title fidelity.
**The valid Dolphin-GX ground truth is the real GX COMMAND STREAM** (the gather-pipe bytes Dolphin's
GPU processes), captured via runtime/gx_stream.cpp + gx_parse.h (already built on Dolphin's
OpcodeDecoder; currently gated off in purejit — re-enable capture-only for the oracle), decoded into
per-pass geometry/matrices/lights. NOT ngx, NOT xfmem (async-lagged). USER also wants ngx ERADICATED
(it's broken; build/sunbright should be pure Dolphin-GX = the clean oracle; sms-boot = the renderer).
The lighting-parity harness machinery (parity_sweep.py _diff_summary + parity_sweep_test.py) is still
good — only the ORACLE SOURCE must change from ngx to the Dolphin GX command stream.

## ⚠ HARNESS CONFOUNDS (these compound the invalid-oracle problem above)
Repro: native `SB_STAGE=15 SB_PARITY_DUMP=...` (build-native/sms-boot); oracle
`SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_NGX_PRESENT=0 SUNBRIGHT_NGX_SHAPE=1
SUNBRIGHT_PARITY_DUMP=...` (build/sunbright + ROM argv); `parity_sweep.py diff oracle.jsonl native.jsonl`.
Title settled-window medians: oracle nbatch=19 onscr=1722 lights=3 amb=[0,0,0]; native nbatch=120
(62 w/o drive_chr) onscr=363 lights=8 amb=[1,1,1] projType native=0 oracle=1.

Three independent confounds make these NOT comparable as-is:
1. **Capture-scope asymmetry (nbatch/nverts/onscr).** The native parity dump's `batches` = the J3D
   3D scene ONLY; it EXCLUDES the 2D imm logo/text/HUD (separate gx_imm path). The oracle ngx capture
   groups + filters differently (and may count J2DPicture draws). So native 363 vs oracle 1727 on-screen
   verts is mostly "what each path counts as geometry", not a real deficit. (Confirmed: the title looks
   complete despite 363.) Native's high total (5835) with low on-screen is the sky-dome + (with
   drive_chr) off-screen file-block/Mario junk that doesn't belong pre-Start.
2. **Publish-phase sampling (ambient/matc/projType).** Both emitters read the GX ambient/projType
   REGISTER at frame publish — i.e. AFTER the 2D logo/HUD draw (ortho). So native amb=[1,1,1] is the
   HUD's ambient, oracle projType=1 is the logo's ortho — NEITHER is the 3D scene's value. Pure phase.
3. **Maskable light count (8 vs 3).** Native's stage-light loader (scene_drive.cpp:621) loads ALL
   "Light Group" entries (≤8) into GX_LIGHTi; the oracle's real path loads 3. BUT materials select
   lights via their cc0 light MASK — extra loaded-but-unmasked lights don't shade. So 8-vs-3 is a
   reliable MEASUREMENT but may be a non-divergence in OUTPUT (same class as the parked-fog finding).
   Light count IS the only reliable cross-engine signal today; the rest are confounded (now noted in
   the parity_sweep.py summary output).

## The path to a TRUSTWORTHY per-pass comparator (the user's actual ask) — NEXT WORK
To compare "individual render passes" rigorously, both emitters must capture the SAME SCOPE at the
SAME phase, segmented by pass:
- **Scope-match:** include the 2D imm/J2D draws in the native parity dump (or exclude J2D from the
  oracle) so "geometry" means the same thing on both sides.
- **Phase-match lighting:** snapshot lights+ambient at 3D-SCENE draw time (native: right after the
  scene light load in scene_drive, before drive_hud; oracle: when the persp/scene proj is active),
  not at publish. Better: emit PER-BATCH ambient + light-mask (the batch's material chan state) so
  lighting is compared per material/pass, not as one frame register.
- **Per-pass tags:** segment batches by pass — native by drive call (sky / scene / chr / hud), oracle
  by EFB-gen/draw-buffer. Compare like-pass to like-pass.
- **Lit-color signal:** both engines compute per-vertex lit color0 (native sb_light_vertex_color0,
  oracle ngx light_color0); a per-batch lit-color checksum is the rigorous lighting parity signal,
  immune to the register/phase confound.
Then re-run title + file-select. The title is visually correct, so expect SMALL real divergences;
file-select (post-Start: 3 file blocks + banner + Mario) is the less-verified target — prioritize it.

## Reusable commands
- Native title dump: `SB_STAGE=15 SB_PARITY_DUMP=scratch/passes/native_title.jsonl
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_START=600 SB_FRAME_DUMP_MAX=30` + the offscreen/SDLGPU/TURBO env.
- Oracle title dump: `SUNBRIGHT_FASTBOOT=1 SUNBRIGHT_STAGE=15 SUNBRIGHT_NGX_PRESENT=0
  SUNBRIGHT_NGX_SHAPE=1 SUNBRIGHT_PARITY_DUMP=scratch/passes/oracle_title.jsonl ./build/sunbright "$ROM"`.
- File-select = same + press Start once via probe `/pad?do=start&ms=250` (see fileselect_oracle.sh).
- ⚠ Building build/sunbright needs `pulse/pulseaudio.h`; if missing, `cmake -B build -U USE_PULSE`
  reconfigures cubeb to skip the pulse backend (done this session).
