# 2026-07-15 — File-select parity vs the oracle: mostly faithful, 3 real residuals

## WATER SPECKLE (replay-oracle artifact) — pinned to ONE draw via new SB_SKIP_HASH tool

The `.dff`-replay's file-select ocean renders as harsh white speckle (NOT in live-native — live water
matches true Dolphin, so this is a REPLAY-ORACLE artifact, not a shipped-game bug). Built `SB_SKIP_HASH=
<hex>[,...]` (aurora ad93196 — drop draws by shader-config hash, the only isolation available on the
marker-less replay path) and pinned it: **hash `6f65bd280aa2fcc7`** — a screen-covering 4-vert quad, 2×/frame,
sampling a **256×256 I4** texture with **minFilt=5 (LIN_MIP_LIN)**; its byte-identical SMOOTH sibling (draw
#286, `7c53028b…`) differs ONLY in the sampler (minFilt=1 → LOD 0). Skipping the culprit removes the speckle
(white% 75→0). So aurora is sampling this texture's upper MIPS as noise. Ruled out: mip_count too high
(clamped to dim-max, no change — verified cold-start) and the I4 tile decode (offsets/padding correct).
REMAINING cause (deep, replay-oracle-only): either the `.dff` doesn't carry this texture's mip DATA (garbage
past level 0) or aurora's LOD/`lod_bias` (`tex0_size_bias.z` in the shader) over-selects mips vs GC. NEXT:
dump this texObj's lod_bias/max_lod + the decoded mip levels; or test forcing LOD 0 for it (matches the
sibling). Fixing it also cleans the `.dff` pixel oracle for the sea (which contaminated the Mario measure).

## ✅ MATCHED-STATE COMPARISON DONE RIGHT — isolates the ONE real render divergence: MARIO PALENESS

The sound matched-state test (native `SB_FIFO_REPLAY(fsel_try_7300.dff)` vs Dolphin FIFO-player playback
of the SAME dff = `dolphin_fsel_true.png`) renders IDENTICAL recorded draws, so every pixel difference is
PURE aurora render fidelity — ZERO state confound. Result (`scratch/shots/replay_matched.png`):
- **FRAMING IS FAITHFUL.** Banner / shine×01 / New / A-B-C cubes / palm / OPTIONS sign / Mario all at the
  SAME positions. ⇒ The "framing/OPTIONS-cut-off divergence" chased earlier was live-native GAME STATE
  (camera pan position / capture timing) vs the dff's recorded moment — NOT a render bug. File-select
  framing/projection is correct. (Watch the 1280×896 replay vs 640×480 dolphin XFB-scale when diffing
  numerically — align visually, not by naive resize.)
- **WATER**: native has the white speckle, dolphin is smooth turquoise = the known aurora EFB-copy REPLAY
  artifact (separate; live-native water already matched true Dolphin, journal above).
- **MARIO IS PALE/WASHED in native** (pale overalls) vs dolphin's vibrant blue+red — AT MATCHED STATE, same
  draws (mario region dABS 40, the largest non-water). ⇒ a REAL aurora RENDER bug (lighting/TEV computation
  renders the same Mario draws too bright/desaturated), NOT timing/game-logic. This is
  [[mario-paleness-l1-not-cause-2026-07-04]] confirmed render-side ("attack L0/L2 diffuse").

### RE REDIRECT (2026-07-15): paleness is AURORA-SIDE lighting eval, likely SKINNED NORMALS — not game-side L0/L2

The prior RE ([[mario-paleness-l1-not-cause-2026-07-04]]) ruled out L1 + ambient and suspected GAME-SIDE
L0/L2 diffuse. The matched-state result REFINES that: the REPLAY renders Mario pale using the `.dff`'s
RETAIL GX light commands — game-side `TLightMario::setLight` is BYPASSED in replay. So the bug is NOT the
game-side light setup; it's how AURORA EVALUATES the (correct, retail) lighting. And it's Mario-SPECIFIC
(cubes/palm/static objects render fine in the same replay), and Mario is the only SKINNED object here ⇒
prime suspect: aurora's per-vertex NORMAL handling for SKINNED draws (envelope/weighted normal matrix),
which drives the diffuse `N·L`; wrong normals → wrong diffuse → over-bright/desaturated. (Note CLAUDE.md:
GXLoadPosMtxIndx/GXLoadNrmMtxIndx3x3 are "verified non-degenerate" — so suspect the shader's normal
TRANSFORM / lighting use, or normal renormalization, not the load.) NEXT: in the replay, compare a SKINNED
lit draw (Mario) vs a NON-skinned lit draw (cube) — same lights — and inspect aurora's WGSL vertex-lighting
+ normal path (extern/aurora/lib/gx/shader.cpp / gx.cpp). This supersedes the "add SB_NO_L0/L2_LIGHT
game-side toggles" plan (those act on game-side setLight, which replay doesn't use).

### TOOL COMPLETE (2026-07-15): fork `--load-state-at` renders a save state deterministically

The matched-state oracle is now complete end-to-end (fork c26bbbe): `--save-state-at <field>` WRITES a
reproducible .sav; `--load-state-at <field>` FRESH-boots + `State::LoadAs` mid-run (headless boot-FROM-state
doesn't step the core) + renders + self-exits. Verified: renders file-select (water [96,188,201]),
BIT-IDENTICAL across runs (meanABS 0.000). Also fixed the `is_set` vs `is_set_by_user` guard bug (optparse
seeds defaults into the value map, so `!is_set("save_state_at")` was always false → dead block). Pipeline:
save state → `--load-state-at`+DumpFrames = pixel oracle; save state → FIFO-record = matched draw oracle.

### ⚠️ MARIO PALENESS — REASSESSED as MINOR/borderline; the dramatic version was MEASUREMENT ARTIFACT

After building tools (SB_TEV_STOP, SB_LIGHT_DBG, SB_NRM_VIZ) and ruling out normals/ambient/L1/texture/
matColor/TEV-op/dead-light, the FIX attempts didn't move Mario. Then the honest reassessment: the
dramatic "washed white" was largely ARTIFACT, not a real render defect:
- The `mario_matched_crop` that looked washed was native-replay (896) DOWNSCALED to 400H — the downscale
  lightened/blurred Mario. At FULL RES (`scratch/shots/mariofix2_crop.png`) Mario renders FINE: red hat,
  blue overalls, reasonably saturated.
- The 1.24× (overalls) / 1.49× (hat) region ratios were CONTAMINATED: Mario is small and surrounded by the
  white water-speckle EFB-replay artifact; my coordinate boxes kept catching background/gloves, not his body
  (the "tight overalls" box even read [255,255,255] = a glove/highlight). Where Mario IS cleanly measurable
  (leg/skin, robust median), he MATCHES the oracle at **~1.02**.
⇒ The "Mario paleness" is at most a SUBTLE over-bright within the water-artifact measurement noise — NOT a
significant defect. I over-portrayed it from downscaled + background-contaminated comparisons (same failure
mode as the earlier palm-trunk "paleness", which was also a crop artifact). The genuine remaining
file-select render issue is the WATER EFB-copy REPLAY artifact (which also contaminated these measurements).
Tools + the dead-light 0/0 guard (safe robustness) are kept. LESSON: measure Mario with a clean
background-free crop (or the water fixed) before claiming a lighting defect.

### (INFLATED — see reassessment above) MARIO PALENESS — SURFACED by SB_TEV_STOP tool: aurora DYNAMIC DIFFUSE over-bright (~2× diffuse)

Built `SB_TEV_STOP=<N>` (aurora 11c709d) — TEV-stage bisector, outputs the combiner state after stage N.
Rendering Mario at N=0..4 showed his final overalls color emerges at STAGE 4 (which uses `RASC` = the lit
vertex color). Then a matched-state per-region measure (native `SB_FIFO_REPLAY` vs Dolphin playback of the
SAME dff, at correct locations, native full-res — no downscale confound) is decisive:
- palm-trunk ratio **1.00**, sand **1.02** (MATCH — the earlier "palm paleness" was a crop/resize artifact)
- cube gold **1.07**, **Mario overalls 1.24** (native [207,220,238] vs Dolphin [164,174,197]).
Mario is uniquely ~24% over-bright. Static/ambient-only map objects (LightOff) MATCH; Mario is the only
DYNAMICALLY-lit thing (TLightMario → L0/L1). ⇒ the bug is aurora's **dynamic diffuse lighting eval**,
over-contributing for Mario. Back-of-envelope: if Dolphin lighting≈0.5(amb)+0.15(diff) and aurora≈0.5+0.30,
that's ~**2× the diffuse** — a clean factor suggesting a light DOUBLE-COUNTED or an extra light summed
(Mario mask=03 = lights 0,1; L1 is the effect light — if aurora sums an L1 contribution that GC has at
zero/half, or L0 twice, that's the 2×). CONFIRMS the old L0/L2-diffuse suspicion, now matched-state +
quantified. Also corrected: the "washed white" impression was EXAGGERATED by the 896→480 downscale; the
real defect is a moderate uniform ~1.24× over-bright (clean crop `scratch/shots/mario_matched_crop.png`).
NEXT (close to fix): isolate WHICH light over-contributes — dump aurora's per-light diffuse (attn, diff,
light.color, lightMask) for a Mario draw vs the .dff's light setup; check if aurora sums a light GC masks/zeros.

### MARIO PALENESS — CHECKPOINT (2026-07-15): precisely localized, 6 hypotheses ruled out, cosmetic

Ruled out, each verified (not guessed): **normals** (SB_NRM_VIZ shows correct smooth normals), **ambient**
(0x80/0x28 correct), **L1** (prior), **texture-decode** (overalls tex_49 RGB5A3 → correct blue),
**matColor** (white REG, same as fine cubes), and **TEV op codegen** (aurora's `tev_op`, shader.cpp:364,
implements all 8 compare ops; `COMP_GR16_GT`=op10 that Mario's tev=5 material uses looks correct:
`select(0,c, GR16(a)>GR16(b)) + d`). So the wash-to-white is a SUBTLE divergence in aurora's MULTI-STAGE
TEV COMBINE of Mario's specific materials (the tev=5 material's stage interplay: TEXA-compare mask → C2/C1
KONST mixes with SUBHALF biases → RASC lighting modulate), not any single op. This is deep aurora
TEV-combine-fidelity RE for a COSMETIC, non-blocking residual — CHECKPOINTED here (marathon session,
context pressure). The precise next step for a fresh, focused effort:
1. Extend the draw-dump `[tex]` line to report ALL bound texmaps (currently only texMap0) — small aurora
   change — to pin the exact overalls-binding draw + its stage.
2. Reconstruct that draw's full generated WGSL (SB_SHADER_DUMP/SB_SHADER_HASH) and walk it stage-by-stage
   against the GC TEV spec for the material, finding the one stage whose output diverges (suspects: the
   SUBHALF-bias handling, the KONST-reg mix, or texmap ordering in the multi-tex modulate).
3. Verify with the matched-state tool (`--load-state-at` Dolphin render vs native replay of the same state)
   on Mario specifically.
Tools ready: SB_NRM_VIZ, SB_TEV_DUMP, SB_TEX_DUMP, draw_diff.py, --save-state-at/--load-state-at, uncapped
SB_DRAW_DUMP_FRAME. File-select is otherwise FAITHFUL (framing + lighting confirmed; water is a separate
aurora EFB-copy REPLAY tooling artifact, not a live-native bug).

### MARIO PALENESS — narrowed to aurora TEV-COMBINER-OP fidelity (texture + matColor also ruled out)

Full-frame draw+TEV+texture dump of the replay (SB_DRAW_DUMP_FRAME=0 + SB_TEV_DUMP + SB_TEX_DUMP): two
MORE hypotheses ruled out. (1) **Texture decode RULED OUT** — Mario's overalls texture (tex_49, 16×32
fmt5/RGB5A3) decodes to correct saturated denim blue (center (85,102,153)); only 2 blue textures in the
300-tex dump, both decode blue. (2) **matColor RULED OUT** — Mario's draws are `mat=(1,1,1,1)` REG, IDENTICAL
to the cubes that render fine; no per-vertex color (clr0=0, vtx default white). Mario is a skinned mesh =
many low-vert (≤23) tri-strips per matrix, `ch0[light=1 matSrc=0 ambSrc=0 mat=(1,1,1,1) amb=(0.5) mask=03]`.
⇒ The overalls blue enters via TEXTURE and is combined through Mario's TEV. Two Mario materials: a `tev=2`
skin material and a `tev=5` multi-texture material (317 draws) with `op=COMPARE` (stage0), `bias=SUBHALF`
terms, and muted KONST regs (reg1≈lavender (0.59,0.59,0.71), reg2≈grey-blue (0.62,0.63,0.66)). At MATCHED
state aurora gets the SAME TEV setup + textures as Dolphin, so the wash-to-white = aurora evaluating one of
these combiner ops (COMPARE / SUBHALF-bias / KONST-select / multi-texmap modulate) differently from GC. The
cubes render fine because their TEV is simpler. NEXT (deep, cosmetic): the draw-dump `[tex]` only logs
texMap0, but the overalls bind on a higher texmap — extend it to dump ALL bound texmaps to pin the exact
overalls draw, then compare aurora's per-stage TEV op eval to the GC TEV spec for that material. This is
aurora TEV-op-fidelity RE (deep); the paleness is cosmetic/non-blocking and now precisely localized.

### MARIO PALENESS — SKINNED-NORMAL HYPOTHESIS FALSIFIED (2026-07-15, via new SB_NRM_VIZ tool)

Built `SB_NRM_VIZ` (aurora 3b7300e, env-gated normal visualization — outputs mv_nrm as fragment color).
Rendered file-select replay with it: **Mario's normals are SMOOTH and CORRECT** — a continuous color
gradient wrapping the hat dome / nose / arms / legs, no speckle/discontinuity/garbage; cubes are clean
flat-shaded faces (`scratch/shots/nrmviz_mario.png`). ⇒ skinned-Mario's normals are HEALTHY; the paleness
is NOT wrong normals. So the ruled-out set is now: normals, ambient (verified 0x80/0x28 correct), L1
([[mario-paleness-l1-not-cause-2026-07-04]]). Since normals→diff (N·L) are correct and the lights are
retail-correct (from the .dff), the per-vertex LIT color should match Dolphin — so the paleness is
DOWNSTREAM of vertex lighting: Mario's **TEV combiner**, **texture decode**, **material (matColor)**, or
**specular (L2)**, whichever differs from Dolphin's eval of the same draws. NEXT: isolate Mario's draws in
the perspective buffers (buf?/MapOpa, by vert count / mtxIdx — he's not Chr-tagged) and dump their TEV
stages + bound texture + matColor (SB_TEV_DUMP), comparing aurora's combine to what those GX commands
should produce. Cosmetic/non-blocking.

### (SUPERSEDED — falsified above) MARIO PALENESS RE — narrowed to aurora SKINNED-normal path

Ruled out: aurora's direct XF normal-matrix parse (command_processor.cpp:328, addr 0x400-0x459) reads the
3×3 correctly into pnMtx[].nrm and is SHARED with static objects (cubes render fine) ⇒ not the Mario bug.
The shader normalizes mv_nrm (shader.cpp:1079) ⇒ not an un-normalized-normal over-brighten. So the
Mario-specific factor is the INDEXED matrix path: skinned Mario loads per-vertex normal matrices via
`CP_LOAD_INDX` (opcode ~0x28 → XF 0x400 range) from an in-memory array, NOT direct GXLoadNrmMtxImm.
SUSPECT: aurora's indexed normal-matrix load writes/derives the wrong matrix (or the skinned normal ends
up wrong) → wrong diffuse N·L → over-bright/washed. NEXT (needs fresh context — deep aurora XF/skinning):
(a) use the shader normal-viz debug (shader.cpp:1625 `prev = vec4f(in.nrm, ...)`) or add a per-vertex mv_nrm
dump on a Mario skinned draw to see if skinned normals are wrong vs static; (b) trace aurora's
CP_LOAD_INDX handling for the XF-D (normal) matrix array; (c) with the tool now working, capture a matched
Mario oracle (`--load-state-at` Dolphin render + FIFO-record→native replay) for a clean side-by-side.
Cosmetic/non-blocking; well-characterized as aurora skinned-normal diffuse.

### DATA (2026-07-15): file-select Mario is NOT a ChrOpa draw — he's in the PERSPECTIVE scene buffers

Uncapped full-frame dump of live-native file-select (frame 1200/1500, SB_DRAW_DUMP_FRAME + SB_TEV_DUMP):
the frame has NO `ChrOpa` (lit 3D character) draws. All `Chr`-tagged draws are 2D orthographic `ChrXlu`
UI sprites (unlit, mat white). The 466-draw frame is: `buf?` (154, persp), `DrawBuf Mirror Opa` (142),
`ChrXlu` (81, all proj=O 2D), `MapOpa` (37), `<TLightDrawBuffer::Opa>` (24), `Sky Xlu` (12), `StaticMapObj
ShadowOpa` (6), `MapXlu` (5), `Mirror Xlu` (4). The lit perspective draws use `light=1 amb=(0.5) mask=03`.
⇒ file-select Mario is drawn among the PERSPECTIVE scene buffers (`buf?`/`MapOpa`), NOT Chr-tagged — so
"grep Chr" doesn't find him. The lit CUBES are in those same buffers and render FINE, while Mario is pale,
and Mario is the only SKINNED object → still points at aurora's skinned-normal diffuse.

NEXT (isolate Mario's skinned draws): within the perspective lit draws, Mario = the high-vertex skinned
shapes using indexed pos/nrm matrices (posmtx varies per draw, many verts). Filter `buf?`/`MapOpa`
`light=1` draws by vert count / mtxIdx variation to find his skin draws, dump their normals + per-light
diffuse. OR (cleaner, and it's the next boot-order target anyway) study Mario in GAMEPLAY where he's a
clear `ChrOpa` lit character (`SB_SEL_PICK=<n>` + longer pad script to reach the airport, memory notes).
The paleness is a REAL aurora render residual (cosmetic, non-blocking) — well-characterized as skinned-normal
/ diffuse eval, aurora-side.

⇒ **THE file-select render target is now singular and real: fix Mario paleness in aurora's rendering.**
Since the draws are identical to dolphin's, the divergence is how aurora PROCESSES Mario's lit draws
(the ch0 diffuse light math / TEV / material), downstream of the GX state. NEXT RE: dump Mario's lit
draws' full state from the replay (SB_DRAW_DUMP=1 SB_DRAW_DUMP_FRAME=<N> now uncapped + SB_TEV_DUMP) and
compare aurora's computed lighting/TEV to what those GX commands should produce — the bug is in aurora's
GX lighting/TEV eval (gx.cpp / shader gen), or an unapplied light/material reg. Everything below
(paleness-as-ambient, framing-as-bug) is SUPERSEDED by this matched-state result.

## ❌ PALENESS LEAD FALSIFIED (2026-07-15, same day) — the draw_diff "0x28-absent" was a 200-cap ARTIFACT

The lead below ("native never emits 0x28 ambient") is FALSE. Root cause of the false lead: `SB_DRAW_DUMP`
hard-caps at 200 draws (aurora command_processor.cpp), so the native log was the FIRST 200 draws only —
and the 0x28-ambient draws (StaticMapObj SunOpa, ~1725× per the full-frame amb-trace) come LATER, so they
were never in the sample. The earlier full-frame `amb-trace` (aurora GXSetChanAmbColor value log) PROVED
native emits BOTH `80808000` (0x80, 15521×) AND `28282800` (0x28, 1725×) — matching the oracle's 0x80/0x28
RGB. So native's ambient RGB is CORRECT; the per-light-set index is NOT the paleness bug. (The only ambient
delta that stands is the ALPHA: native 0x..00 vs oracle 0x..ff — non-visible, the getAmbColor alpha-scale
field nuance already noted.)

TOOLING FIX (so this cap can't confound again): `SB_DRAW_DUMP=1 SB_DRAW_DUMP_FRAME=<N>` now dumps the FULL
ch0 `[draw-dump]` line for exactly frame N, UNCAPPED (aurora 3598759, shared `s_ddFrameActive` gate;
verified 466 lines incl. amb=0x28). Use that, not bare SB_DRAW_DUMP (200-cap), for any full-frame draw_diff.

## 🧱 THE REAL BLOCKER: matched-state comparison — need the save-state RELOAD-RENDER (crux)

Every file-select divergence I "found" turned out to be an UNMATCHED-STATE artifact: the paleness was a
200-cap sampling artifact + framing misalignment; the framing itself is a camera-state mismatch. Native
file-select is NOT a still frame — a settle check (present 1400 vs 3000) shows whole-frame meanABS 19.8
(all regions move), but that is CONFOUNDED by the **attract-loop timeout** (memory: ~45s idle → attract
movie ≈ 2700 frames, so 3000 is a DIFFERENT scene, not "still panning"). So there is no reliable "settled"
native frame to grab, and the camera pans + Mario idles + attract can fire. ⇒ Ad-hoc frame-count captures
CANNOT give matched state — confirmed empirically, repeatedly.

The only sound fix is FREEZING one deterministic state and rendering it: the fork `--save-state-at` WRITE
path works (24.6MB reproducible .sav), but RELOAD-RENDER is broken — booting the fork headless with
`--save_state=<file>` loads the core but doesn't STEP VI fields (no frames). **NEXT (the crux blocker to
build past):** make the headless boot-from-state STEP the core (investigate why NoGUI doesn't run emulation
after a state load — likely starts paused / MainLoop not advancing the CPU). Once it renders, one save state
gives: a canonical pixel oracle (framedump) AND a matched draw oracle (FIFO-record from the same state) —
and native, driven to the same event, is finally comparable. Until then, STOP hunting file-select pixel
divergences against unmatched captures.

**BUT — a matched-state test that needs NO save-state (do this FIRST):** native `SB_FIFO_REPLAY(dff)` and
Dolphin FIFO-player playback of the SAME dff render the IDENTICAL recorded draws → matched by construction
(same camera, same Mario pose, same everything). So compare native-replay(fsel_try_7300.dff) vs
`dolphin_fsel_true.png` (Dolphin playback of the same dff): any pixel difference is PURE aurora RENDER
fidelity (framing, lighting, materials) — ZERO state confound. This splits the question cleanly:
 - If framing/lighting MATCH there (only water differs = known EFB-replay artifact) ⇒ aurora renders
   file-select faithfully; the LIVE-native framing difference is game-logic (live camera state) — THEN the
   save-state matched-state tooling is what's needed to chase that.
 - If they DIFFER there ⇒ a real aurora render bug (projection/framing/lighting), RE it directly.
NEXT TICK: run that same-dff render-parity compare (native SB_FIFO_REPLAY vs dolphin_fsel_true) with the
new uncapped full-frame draw_diff + a pixel diff, water excluded. No new tooling required.

**Paleness status: OPEN and possibly NOT REAL.** The palm-trunk [189,209,214] vs Dolphin [165,179,164]
region box sits at the FRAME EDGE and native's value is BLUE-tinted (214 B) — likely SKY contamination in
the box, not a real trunk difference. RE-VERIFY with a clean, sky-free trunk crop before treating paleness
as a defect at all. Do NOT chase the per-light-set ambient index (falsified here).

## (FALSIFIED — see above) PALENESS LEAD (2026-07-15, via new tools/oracle/draw_diff.py) — ambient 0x80 vs 0x28 per light-set

Built `tools/oracle/draw_diff.py` (native-vs-oracle per-draw render-state diff, committed) to stop
hand-grepping. First run (native-live SB_DRAW_DUMP vs oracle fsel_try_7300.dff replay, --group none):
**native lit draws emit `amb=(0.50)` (0x80) or `(1.00)`; the oracle emits `amb=(0.16,0.16,0.16)` (0x28)
prominently — a value native NEVER emits.** Ambient 0.50 vs 0.16 = ~3× too bright ⇒ the palm/Mario
PALENESS lead. (Confound: live-vs-.dff draw sets differ + SB_DRAW_DUMP caps ~200 draws; but STATIC-object
lighting is frame-independent and native's total ABSENCE of 0x28 is the signal.)

**Hypothesis (RE-able as pure game-logic, NO pixel oracle needed):** the retail game has multiple light
sets (TLightWithDBSetManager: player/mapobj/object/indirect), each with its own `mAmbBaseIdx` into the
"Ambient Group" TAmbAry; different objects get different scene ambient (0x28 for some, 0x80 for others).
`TLightCommon::setLight` → `GXSetChanAmbColor(getAmbColor(idx))`, and `getAmbColor` reads
`AmbAry->mAmbColors[idx + mAmbBaseIdx]`. If native's per-set `mAmbBaseIdx` (and/or the idx passed to
setLight) is collapsed/zero, every set reads the same AmbAry entry (0x80) → native never produces 0x28.
NEXT RE (Ghidra, no pixel compare): verify each TLightWithDBSet's `mAmbBaseIdx`/`mLightBaseIdx` are set
to the retail per-set values (disasm the set construction / setLightType), and that getAmbColor's index
matches the disasm — cross-check vs the scene's AmbAry contents. This is checkable against the
disassembly + scene data directly, sidestepping the matched-state pixel confound. See
[[light-dbset-porting-gaps-2026-07-04]], [[tlightwithdbsetmanager-bitmasks-corrected-2026-07-04]],
[[mario-paleness-l1-not-cause-2026-07-04]] (ambient was "ruled out" there pre-setLight-fix — re-open:
the per-set ambient INDEX, not the value, is the new suspect).

## ⚠️ RETRACTION (2026-07-15) — "PARITY COMPLETE" below was PREMATURE; full-frame compare shows residuals

The "complete" claim was based on a WATER-region numerical match + eyeballing the rest. A proper
full-frame native-vs-true-Dolphin compare (`scratch/shots/fsel_vs_dolphin.png`, native resized to the
Dolphin 640×480) shows the water matches (ΔABS 3.2) and under-lighting is fixed, but UNCONFIRMED
residuals remain: palm-trunk paler in native ([189,209,214] vs [165,179,164], Δ35), Mario pose +
paleness differ, and horizontal framing differs (native cuts off the OPTIONS sign; blocks shifted).
Hypotheses to run down (NOT yet resolved): (a) framing + Mario pose = capture-timing (the file-select
camera PANS; native@1400-presents vs Dolphin .dff@field-7300 may be different pan positions — align both
to the settled save-blocks camera state before judging); (b) palm-trunk + Mario paleness may be the
KNOWN L0/L2-diffuse paleness residual ([[mario-paleness-l1-not-cause]] — ambient/L1 already ruled out
there), NOT the ambient fix (amb-trace confirmed native emits the oracle's 0x80); (c) region-box
contamination (palm-trunk box at the frame edge may include sky). Do NOT call file-select done until
these are separated real-vs-artifact. TMapObjWave + water stay resolved.

## (below, superseded by the retraction above) FILE-SELECT PARITY vs the TRUE Dolphin oracle (2026-07-15)

All three original residuals are resolved/dismissed, verified against a REAL Dolphin framedump
(not the artifact-prone aurora replay):
1. **Palm under-lit — FIXED** (the missing `TLightCommon::setLight` scene-ambient; see below).
2. **A/B/C cubes under-lit — FIXED** (same ambient fix).
3. **Ocean sun-glare — NOT A NATIVE DEFECT** (dismissed). It was entirely an aurora `SB_FIFO_REPLAY`
   artifact. Captured the TRUE Dolphin render of file-select (fork `dolphin-emu-nogui -p headless -e
   fsel_try_7300.dff -C Dolphin.Movie.DumpFrames=True` under Xvfb → `scratch/shots/dolphin_fsel_true.png`):
   the true GC water is **turquoise** `[96.6,188.3,200.7]`, 5.3% whitish — and NATIVE water is
   `[96,193,204]`, 7% whitish. They MATCH on every channel. The "prominent white water band" existed
   ONLY in the aurora `.dff` replays (harsh speckle/whitewash from aurora mis-replaying the mirror /
   frame-feedback EFB copies + the `eb5c8e74` composite). Native's water is faithful; there was no bug
   to fix. TMapObjWave (faithful, subtle) and the EFB composite were both correctly NOT the cause.

**Net:** native file-select renders faithfully vs the true Dolphin oracle (water, palm, cubes, Mario,
sky, UI all match; only slot-A save-data differs = Dolphin memcard shine×01 vs native blank New, which
is correct). The multi-pass RE (EFB-composite → TMapObjWave → wave.bti → oracle-validity) converged by
ruling causes OUT honestly rather than landing a wrong fix for a non-existent defect.

**Two follow-ups (NOT file-select-parity blockers, separate arcs):**
- (minor) ambient ALPHA=0 vs 0xff — `getAmbColor` alpha-scale field nuance (0x18 vs 0x1C), non-visible.
- (tooling) the aurora `SB_FIFO_REPLAY` mis-renders EFB-copy/composite water as speckle/whitewash — a
  DIAGNOSTIC-tool fidelity bug (the .dff pixel oracle is unreliable for any scene with mirror/feedback
  EFB copies). Worth fixing for future oracle work; independent of native game parity.

---
### (below: the investigation history that led here)


User directive: file-select parity, compare against the oracle. Built the oracle harness,
captured both sides at FULL RES, and compared. The file-select (stage-15 save-blocks,
reached by pressing START at the title) renders the full beach/ocean/sky/palm scene + the
"Select data." UI faithfully. There ARE real residuals (below).

## ⚠️ Downscaled/`_640` captures LIED — always compare at full res

Early `_640` downscales showed a WHITE background + "faded" Mario and I nearly chased a
"missing background" defect. Both were DOWNSCALE ARTIFACTS: the full-res native
(`scratch/shots/fsel_blocks_1400.png`) clearly renders the whole beach scene, and Mario is
fine (his big white gloves are correct). A naive pixel-diff was ALSO junk (67% "different")
because native dumps 1280x960 (480 lines) and the aurora replay dumps 1280x896 (448 EFB
lines) — scaling both to a common size vertically MISALIGNS every UI element. LESSON: view
full res; for a real pixel diff, capture both sides at the SAME height first.

## The oracle harness (durable, reusable)

File-select is INPUT-GATED (needs a START press) so the deterministic field-count fifo
recorder can't reach it. Added to the Dolphin fork (now a SUBMODULE): DolphinNoGUI
**`--pad-start-at <field> --pad-start-frames <n>`** (fork 2059b5b) injects a headless GC
START over a VI-field window (CSIDevice_GCController::GetPadStatus), driven by the existing
vi_end_field counter. Working recipe:
`--pad-start-at=7300 --pad-start-frames=20 --fifo-record-after=7800 --fifo-record-frames=3`
(START ≥7300 reaches the save-blocks = ~3040 draws; 6800 was too early = settled title,
1258 draws). Replay the .dff through aurora (`SB_FIFO_REPLAY=...dff SB_SYNC_PIPELINES=1`) =
pixel oracle. Cached: `scratch/oracle/fifo/fsel_try_7300.dff`.
Native: `SB_STAGE=15 SB_PAD_SCRIPT="600:START 610:-" SB_DUMP_FRAME=... SB_DUMP_FRAME_AFTER=1400`.

## Real residuals (native vs oracle, both full-res) — the parity worklist

1. **Ocean sun-glare missing.** Oracle sea has bright white sun-glint/foam across the
   mid-distance water; native sea is a uniform saturated turquoise (no glare). = the
   reflective-sea sun-glint/additive-ripple path (TMapObjWave / TMirrorCamera / sea notes)
   not fully rendering here.
2. **Palm tree under-lit.** Native palm TRUNK is a dark near-black silhouette + fronds dark
   green; oracle trunk is light/textured + fronds lighter. Native palm is too dark
   (lighting or the trunk texture/material).
3. **A/B/C file cubes under-lit.** Native cubes are darker/dimmer brown; oracle cubes are
   brighter golden. Same darkening as the palm — likely a shared scene-lighting/ambient
   difference on the file-select 3D objects.

Not defects: file-block content (native all "New" = blank save is correct; oracle slot 1 =
shine×01 from Dolphin's memcard) and Mario anim-pose (mismatched capture instants).

## ✅ RESOLVED (2026-07-15): residuals 2+3 fixed — `setLight` was missing the scene-ambient set

**Root cause (the REAL one, after ruling out the material path below):** `TLightCommon::setLight`
(@US 0x80229a30) ends by applying the scene ambient to the ch0 ambient register:
`GXSetChanAmbColor(GX_COLOR0A0, getAmbColor(idx))` (disasm tail @0x80229c64-0x80229c88 —
`getAmbColor` takes the RAW idx r30, NOT the doubled light-getter index gi=idx*2 r31). Our
native port OMITTED that call. And `setLight` STARTS with `ReInitializeGX()`, which sets
`GXSetChanAmbColor(GX_COLOR0A0, black)`. So every setLight left the ambient register at 0.

Why that darkened the scene: the file-select map objects (palm, A/B/C cubes, MapStaticObj)
are loaded WITHOUT `J3DMLF_MaterialColorLightOn` (literal game-data flags: `0x10210000`/
`0x10220000`/`0x10020000`, `MapStaticObject kMdlF_PE1`, `{"amenbo_model1.bmd",0x10210000}` —
identical on retail), so their color block is `J3DColorBlockLightOff`, whose `setAmbColor`/
`load` are NO-OPS: per-material ambient is faithfully DISCARDED. These lit LightOff materials
(chanctrl `light=1`) rely entirely on the GLOBAL ambient register — which `setLight` is
supposed to set to the scene ambient and didn't. Result: not-directly-lit faces (palm trunk,
shadowed cube faces) rendered BLACK.

**Fix:** appended the missing `GXSetChanAmbColor(GX_COLOR0A0, getAmbColor(idx))` to
`TLightCommon::setLight` (reference/sms `MarioUtil/LightUtil.cpp`). `TLightMario::setLight`
delegates to the base, so both are fixed. This is NOT file-select-specific — it restores
correct ambient for EVERY lit J3D scene (title 3D, gameplay). Faithful RE completion.

**Verified (before→after→oracle, all headless):** the palm trunk went from a black silhouette
to light green/tan; the A/B/C cubes from dim to bright golden (letters visible); both now MATCH
the Dolphin oracle. amb-trace: native now emits `80808000` (0x80 ambient, 15521×) + `28282800`
(0x28, StaticMapObj SunOpa) on the TLightDrawBuffer/Mirror marks — exactly the oracle's
`808080ff`/`282828ff` RGB. Full-frame: 20.1% of pixels brightened (delta +48/+66/+37 RGB over
changed pixels); mean brightness moved to bracket the oracle (G 149→162 vs 159, B 188→195 vs 191).
Screenshots: scratch/shots/ambfix_before.png vs ambfix2_after.png vs amb_oracle.png.

### ⚠️ Remaining sub-residual (secondary, NOT visible-blocking): ambient ALPHA = 0 vs oracle 0xff

Native emits `80808000` (a=0) where the oracle has `808080ff` (a=0xff). RGB matches (the
visible fix); only the ambient ALPHA differs. Cause: `getAmbColor` scales `c.a *= mAlphaScale`
and the ctor leaves the scale field 0. TWO RE nuances found (unfixed, next step): (1) `getLightColor`
reads the alpha-scale from **0x1C** (`mAlphaScale`) but `getAmbColor` reads it from **0x18**
(`unk18`) — the port uses `mAlphaScale` for BOTH; and (2) both fields are 0 at file-select in
native, yet the oracle alpha is 0xff, so the correct scale field must be set to ~1.0 at runtime
by some setter our port doesn't run. Ambient-alpha only feeds lit ALPHA-channel draws (ambSrc=REG),
so it's not visibly blocking here — but it IS a faithfulness gap. Fix path: correct `getAmbColor`
to read 0x18, then find who sets that field (verify the full 0x80229cec disasm for r3-reassignment
first — the `lfs 0x18(r3)` may not be `this`-relative).

## (ORIGINAL, now superseded) ROOT CAUSE hypothesis: AMBIENT COLOR loads as 0

Dual per-draw state dump (SB_DRAW_DUMP, native file-select vs oracle .dff replay) nailed it.
The dominant lit-perspective draw signature:
- ORACLE: `ch0[light=1 matSrc=0 ambSrc=0 mat=(1,1,1,1) amb=(0.50,0.50,0.50) mask=03]`
- NATIVE: `ch0[light=1 matSrc=0 ambSrc=0 mat=(1,1,1,1) amb=(0.00,0.00,0.00) mask=03]`

The ONLY difference is the **ambient color: 0.5 (oracle) vs 0.0 (native)**. With lit,
matSrc/ambSrc=REG, output = mat×(ambient + Σlights); native's zero ambient makes every
not-directly-lit surface (palm trunk, shadowed cube faces) render BLACK — exactly the
symptom. This is almost certainly NOT file-select-specific — it's every lit J3D scene
(the title just has few lit-3D draws so it hid there). HIGH VALUE fix.

Narrowing (do NOT rush — a wrong swap here breaks ALL model loading):
- The material ambient comes from `J3DMaterial::load` (J3DMaterial.cpp:274-276 emits
  `mAmbColor[i].color` via J3DGDSetChanAmbColor), set from `J3DMaterialFactory::newAmbColor`
  (J3DMaterialFactory.cpp:242) = `mpAmbColor[initData->mAmbColorIdx[stage]]` (or the default).
- The default is `j3dDefaultAmbInfo = {0x32,0x32,0x32,0x32}` (≈0.2) — so native's 0.00 is NOT
  the default fallback; native reads `mpAmbColor[idx] == 0` where the oracle's is 0x80. Same
  BMD, so native's ambient LOAD is wrong: either the u16 `mAmbColorIdx` reads byte-swapped
  (BMD MAT3 is big-endian) → wrong entry, or `block.mpAmbColor` offset resolves wrong (LP64),
  or the MAT3 swapper (`readMaterial`) doesn't cover the ambient array/indices. `matColor`
  reads white in both, but that's the newMatColor default (0xFF), so it doesn't prove the
  index path works.
- NEXT DIAGNOSTIC (before fixing): for one file-select material, print native's
  `mpAmbColorNum`, the `mpAmbColor[]` entries, and `initData->mAmbColorIdx[0]` — compare to
  the raw BMD MAT3 bytes. That says definitively index-swap vs array-offset vs default-path.

### RULED OUT (2026-07-15, SB_AMB_DBG trace in newAmbColor — kept as an env-gated diagnostic)

The trace shows the material factory LOADS the ambient CORRECTLY: `mat=0 st=0
ambIdx=0x0000 -> arr=(80,80,80,32)` = 0x80 = 0.5 (ch0). And `J3DGDSetChanAmbColor`
(JRenderer.cpp:46) EMITS it correctly (packs r<<24|g<<16|b<<8|a → XF ambient reg; not a
stub). So the material load + emission are NOT the bug — the 0.5 is correct at creation.

⇒ The amb=0 at DRAW time is a DOWNSTREAM OVERRIDE: something re-sets GXSetChanAmbColor to 0
for the scene draw-buffers. The dominant amb=0 draw markers are `DrawBuf Mirror Opa`,
`<TLightDrawBuffer::Opa>`, `DrawBuf MapOpa`, `buf?` — i.e. the SCENE-LIGHTING / draw-buffer
path (TLightDrawBuffer / TLightWithDBSet / the map light setup), which ties into the existing
lighting-porting gaps ([[light-dbset-porting-gaps-2026-07-04]], calcLightBorder,
[[tlightwithdbsetmanager-bitmasks-corrected]]). Real game emits amb=0.5 there; native emits 0.

NEXT: trace ALL GXSetChanAmbColor callers (value + caller) during a file-select frame to find
WHO sets ambient=0 for the Mirror/Map/TLightDrawBuffer draws (add a caller trace in aurora's
GXSetChanAmbColor or the XF-ambient register write). Then port/fix that scene-ambient setup.
Do this fresh — it's in the TLightWithDBSet lighting arc, not a one-liner.

## Residual 1 (ocean sun-glare/foam) — CHARACTERIZED: the sea-mirror EFB composite (deferred arc)

The oracle's bright white sun-glint/foam across the mid-distance water is the **sea-mirror EFB
composite** (shader key hi32 `0xeb5c8e74`, drawbuf `DrawBuf MapXlu`), fully characterized in
`debug_journal/2026-07-03_water_sea_mirror_efb_composite.md`. In GC it samples a pre-copied
EFB→TEXTURE snapshot of the sea-mirror render and blends it (bm=1/4/2) dreamily over the water —
that IS the reflection/glare. Under `SMS_NATIVE_PLATFORM` we do not emulate GC EFB→TEXTURE copies,
so the bound texture arrived near-black, the TEV (`GX_CS_SCALE_2`) saturated to white, and the
composite painted the whole frame white — so it was deliberately DROPPED (skip batches with
`(shaderKey>>32)==0xeb5c8e74`). Dropping it is faithful given "paint pure white ≠ the RE'd intent",
but the COST is exactly this residual: flat turquoise water, no reflection highlight.

**To resolve it = implement the EFB-copy sea-mirror reflection pass** (render mirror view →
EFB→TEXTURE copy → rebind as the composite's tex → run the real `eb5c8e74` TEV/blend). This is
WITHIN the renderer doctrine ("understood modern equivalent acceptable at genuinely opaque HARDWARE
seams — EFB copy mechanics, XFB present"). It's a SUBSTANTIAL aurora arc, not a one-liner, and it's
the same missing machinery as the title's mirror-capture + logo-reflection EFB copies. This is the
next file-select parity step; start it with fresh context (orient on aurora's EFB-copy handling
first). Do NOT re-attempt the retired `SB_FS_COMPOSITE` segmented-render/snapshot_efb path (falsified
2026-07-03) or any endpoint-gradient hand-tune of the sea (violates no-hand-tuning).

### RE-SCOPE (2026-07-15, orientation pass — the arc is SMALLER than "implement EFB copies")

Three findings that narrow it:
1. **Aurora ALREADY has EFB-copy machinery** — `GXCopyTex`, `GXSetTexCopySrc/Dst`, and a working
   `copy_tex()` resolve/cache (`extern/aurora/lib/dolphin/gx/GXFrameBuffer.cpp:128` — "Pass 1 (source)
   → GXCopyTex(clear) → Pass 2 samples the copy"). NOT a from-scratch EFB implementation.
2. **The 2026-07-03 `SB_SKIP_KEY` / `native/render/sms_boot_present.cpp` composite-drop is RETIRED**
   (gone with the one-runtime consolidation — that file no longer exists). The `eb5c8e74` sea-mask
   composite now flows through the NORMAL draw-buffer path ("sea-MASK material c97c48, key eb5c8e74"
   in `JDRDrawBufObj.cpp:113` / `J3DDrawBuffer.cpp:530`). So it is no longer force-dropped — it draws,
   sampling whatever texture is bound.
3. ⇒ **Real next diagnostic:** at native file-select, dump the `eb5c8e74` draw's bound tex0 (a live
   sea-mirror snapshot, or still near-black?) and check whether the sea-MIRROR render pass + its
   `GXCopyTex` run and populate that texture. If they run and copy_tex populates it → the composite
   should already show reflection (chase why it doesn't). If not → wire the mirror pass (TMirrorCamera
   / mirror draw-buffer → GXCopyTex into the composite's sampled texture). Live render-debugging — best fresh.

### Diagnostic pass 2 (2026-07-15): GXCopyTex FIRES — the copy machinery is active

`SB_COPY_DBG=1` at native file-select: **GXCopyTex fires 2×/frame** — 1410 pairs over the run,
strictly alternating `dest=A clear=1` then `dest=B clear=0` (two stable dests). Plus the separate
disp-copy (640×448) for present. So the EFB→TEXTURE copy runs — residual #1 is NOT "the copy never
happens." Narrows to: (a) does the copied CONTENT hold a real sea-mirror render (vs black/clear), and
(b) does the `eb5c8e74` composite bind+sample that dest and produce the glare? The visible frame still
shows flat turquoise water (no glint), so the reflection isn't reaching the pixels — break is in
content, binding, or the composite's TEV/blend.

**TOOLING GAP for the next step:** `SB_BATCH_DBG` (the batch-attribution dump that characterized
`key=eb5c8e74`, `texmean0=9,9,9,9` in 2026-06/07) is RETIRED — it no longer exists. And `SB_SKIP_MIRROR_*`
is about the SKY material's GX_MIRROR texture-wrap quads, NOT the sea-mirror camera (red herring). So
inspecting the composite draw's sampled-texture content now needs either shader-hash matching
(`SB_SHADER_DUMP` prints `[draw-shader] hash=`; match hi32 `eb5c8e74`) OR re-adding per-draw batch
attribution (bound tex-id + texmean) keyed to the copied dests A/B. That instrument is the WORKFLOW-FIRST
prerequisite for the next diagnostic: dump the content of copied dests A/B and the composite's sampled
tex0, to decide content-vs-binding-vs-TEV. Build that first, then diagnose.

### Diagnostic pass 3 (2026-07-15): the copy-sampling composite contributes ~NOTHING to the water

`SB_SKIP_COPY_QUAD=1` (drops every draw flagged `g_sbDrawSamplesCopy` = samples an EFB-copy texture)
vs the baseline: the WATER region changes by <0.05 per channel (mean base [98.5,200.9,207.9] →
skip [98.5,200.8,207.9]); only 1.37% of water pixels move at all. ⇒ The EFB-copy-sampling
composite produces essentially NO visible water output in native right now. Combined with the flat
turquoise water (no glint), the sea-mirror reflection is simply not being produced — even though
GXCopyTex fires (pass 2) and the drop is retired (re-scope). So the copied texture the composite
samples is empty/black, OR the composite isn't the sea one / isn't flagged as a copy-sampler.
(HONESTY CAVEAT: this test only drops draws detected as copy-samplers; if the sea composite isn't
so-detected it wasn't dropped and this is inconclusive FOR THAT DRAW — but the water is flat either
way, so the reflection is absent regardless.)

Also clarified: `SB_SHADER_HASH` prints `xxh3(shaderConfig)`, NOT the old `eb5c8e74` shaderKey — so
it can't match the historical key directly.

### 🔁 RE pass (2026-07-15, "do the RE work") — RESIDUAL 1 RE-ATTRIBUTED: it's TMapObjWave foam, NOT the EFB composite

Doing the RE reversed the attribution. The oracle's sea white-glint/foam is almost certainly
**TMapObjWave** (the animated Mario-centred sea-foam grid, `MoveBG/MapObjWave.cpp` — FULLY PORTED),
NOT the `eb5c8e74` EFB composite (which `SB_SKIP_COPY_QUAD` proved contributes ~nothing → red herring
for the glint; the 2026-07-03 note was about a different artifact, the whole-screen white-WASH).

Evidence (SB_WAVE_DBG + SB_WAVE_SOLID at native file-select):
- TMapObjWave IS created + loaded (`mMap=15`, coef 30/25, strip 26, half 2600) + performs (0x3001/0x8)
  + `draw()` runs 3× (origin=(950,100,-1000), Mario-centred grid). So it draws.
- `SB_WAVE_SOLID=1` (forces the grid opaque, `GX_BM_NONE`, alpha-compare ALWAYS) BRIGHTENS the water
  region +65R/+20G/+17B (163,220,225 vs the normal 98,201,208). So the grid GEOMETRY + COLOR land on
  the water correctly — only the ALPHA/blend kills it in normal mode.
- Normal blend `GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_SRCCLR`: out = src·srcA + dst·srcRGB. With srcA≈0
  and srcRGB≈white, out≈dst → invisible (exactly the flat-turquoise symptom).

Where the alpha dies: `mAlpha=255` (ctor), vertex `RASA = 255·fade` (opaque at centre) — NOT the
problem. The TEV alpha chain is stage0 `A = TEXA·RASA`, stage1 `A = TEXA·APREV = TEXA²·RASA`. So the
final blend alpha is **TEXA²·RASA**, dominated by `wave.bti`'s texture ALPHA. ⇒ HYPOTHESIS (strong,
not yet verified): `wave.bti` (`/scene/map/map/wave.bti`, loaded via `JKRFileLoader::getGlbResource` +
`JUTTexture::load`) has its alpha channel reading ~0 in native — likely a BTI ResTIMG header/format
byteswap or alpha-format load bug (cf. [[fileselect-textured-2d]] "standalone J2D TIMG is raw BE, swap
at getResource"; getGlbResource may not swap the BTI header → wrong `format`/alpha).

NEXT (verify then fix): (a) dump `wave.bti`'s loaded format + a sample of its ALPHA channel as native
sees it (is it ~0? is `format` a sane GXTexFmt with alpha, e.g. IA8/RGB5A3?); (b) quick falsifier —
temporarily force the grid alpha to bypass TEXA (or set the TEV alpha to RASA only) and confirm the
foam/glint appears matching the oracle; (c) if confirmed, fix the wave.bti alpha load (header swap /
format) — a real BE/format fix, no hand-tuning. This is a TRACTABLE texture-alpha bug, a much smaller
arc than the EFB-reflection implementation the prior passes feared.

#### ⚠️ SELF-CORRECTION (2026-07-15, verify pass) — TMapObjWave is FAITHFUL; re-attribution was premature

Verified the above hypothesis and it does NOT hold as a "bug". `SB_TEX_DUMP` of the loaded `wave.bti`
(the ONLY 128×256 fmt0/I4 texture) shows it loads with REAL, faithful content: alpha/intensity
min 0 / **max 102** / mean 9.93 / **28.3% nonzero** (not all-zero → NOT a data-load bug). Max 102 =
`ExpandTo8<4>(6)` — the source I4 texels genuinely max at 6/15, i.e. the foam texture is intrinsically
DIM + sparse (matches the 2026-06-25 "dark, sparse, faint specks" characterization). Aurora's I4
decoder is correct (`a = intensity`, texture_convert.cpp:220).

Now the TEV math is decisive: final blend alpha = `TEXA²·RASA`. With `TEXA ≤ 0.4` (102/255) and squared,
the peak foam alpha is ≤0.16 and the MEAN is ~0 — so TMapObjWave's foam is FAITHFULLY near-invisible,
on native AND (same math + same texture) on GC. ⇒ The `SB_WAVE_ALPHA_RASA` "foam appeared" result was
MISLEADING: it replaced TEXA with vertex-fade alpha, making the WHOLE grid semi-opaque — that is NOT
what GC does. TMapObjWave is a faithful, subtle effect; it is almost certainly NOT the source of the
oracle's prominent white water band.

#### 🛑 WORKFLOW-FIRST BLOCKER (2026-07-15) — the file-select WATER "oracle" is INVALID (aurora-replay artifact)

Went to quantify the water residual and hit a foundational problem: **there is no trustworthy pixel
oracle for the file-select water.** Every "oracle" image compared against so far is an aurora
`SB_FIFO_REPLAY` of the `.dff` (aurora RENDERING the recorded fifo), NOT a real Dolphin framedump —
and aurora's replay produces WATER ARTIFACTS there (the mirror/frame-feedback EFB copies + the
`eb5c8e74` composite render as harsh white-and-dark SPECKLE / whitewash). Evidence:
- The two cached "oracles" DISAGREE wildly on the same water band: `fsel_oracle.png` = mean
  [111,166,183] / 29% whitish (bluish), `amb_oracle.png` = [215,219,215] / 74% whitish (near-white
  speckle). Both are 1280×**896** = aurora's replay dimension (448 EFB lines ×2), both aurora renders.
- The `water_compare.png` oracle strip is TV-static speckle, not a smooth sun-glint — an artifact
  signature, not a natural highlight.
- `scratch/oracle/dffplay_user/Dump/Frames/` (Dolphin's OWN FIFO-player playback = the real render) is
  EMPTY. So no real-Dolphin water reference exists.

⇒ The entire "oracle has a prominent white water band" premise was built on aurora replay artifacts.
Native's smooth turquoise water may be CLOSER to the true Dolphin output than any of these. Residual 1
(ocean water) is **UNMEASURABLE until a real Dolphin framedump of file-select exists** — comparing
against the aurora `.dff` replay is invalid for the water region (never debug a blackbox: the
instrument is broken). WORKFLOW-FIRST next step: capture a TRUE Dolphin file-select framedump —
either Dolphin FIFO-player playback of `fsel_try_7300.dff` with `Dolphin.Movie.DumpFrames=True`
(→ dffplay_user/Dump/Frames/, per MANIFEST), or a direct fork boot to file-select via `--pad-start-at`
+ headless framedump. Only with that real reference can the water residual be judged at all. Until
then, do NOT chase the water against the replay — and TMapObjWave stays ruled faithful.

**(historical) Corrected open question:** WHAT produces the oracle's prominent white water foam/glint? Leading
candidate (unverified): the 2026-06-25 **b30 "full-width white overlay"** — a SEPARATE 256×256 texture,
`bm=1/4/2`, `ntex=2`, asymmetric UV — which is a DIFFERENT draw from the 128×256 I4 grid and back then
DID render (as diagonal stripes). It may have since stopped drawing, or its texture/UV changed. NEXT
(do NOT claim a root cause first): (1) rigorous native-baseline vs oracle WATER-region diff to quantify
the ACTUAL residual (the "prominent band" was eyeballed — confirm it's real and how large); (2) identify
what draws the b30-style full-width foam overlay in the current native frame and in the oracle .dff, and
whether it renders. Only then name the cause. Keep TMapObjWave OFF the suspect list (faithful).

### (SUPERSEDED by the RE pass above) Residual 1 as an EFB-reflection implementation arc

Everything reversible/diagnostic is done. Ruled out: missing copy (fires 2×/frame), the retired
force-drop, and "composite already works" (it contributes ~nothing). What remains is IMPLEMENTATION:
render the sea-MIRROR view of the scene into a texture (TMirrorCamera / the mirror draw-buffer) →
GXCopyTex it → ensure the `eb5c8e74` composite samples THAT and blends it visibly. This is the same
unported machinery as the title's mirror-capture + logo-reflection EFB copies (renderer-doctrine
sanctions the seam). It's a substantial rendering feature, not a continuation edit — worth a fresh
session and possibly a priority check (finish this vs. move to the Delfino/gameplay arc or the audio
arc). Start by REVERSE-ENGINEERING how retail renders the file-select sea reflection (which camera/
draw-buffer, what it copies, what the composite's TEV expects) before writing any native pass.

- For a clean quantified pixel diff, first fix the capture-height mismatch (dump native at
  448 or the oracle at 480) — the 960-vs-896 misalignment inflated the earlier 67% number.

---

## ✅ RESOLUTION (2026-07-15) — TRUE Dolphin oracle built; water arc CLOSED as faithful

Cleared the WORKFLOW-FIRST blocker (no trustworthy file-select pixel oracle). Built a genuine
Dolphin render of file-select with the fork's `--load-state-at` (fresh-boot → `State::LoadAs`
mid-run → keep stepping → framedump AVI), loading `scratch/oracle/state/fsel_settled.sav`. Recipe
in scratch/oracle/MANIFEST.md (2026-07-15 update). Oracle frame:
`scratch/oracle/loadstate_probe/png/fsel_dolphin_end.png`.

**Water is FAITHFUL — the speckle was an aurora-replay artifact, now falsified against real Dolphin.**
The true oracle water is smooth turquoise with NO white speckle band. Every prior "prominent white
water band / whitewash" observation came from an aurora `SB_FIFO_REPLAY` of the `.dff` (mirror/EFB-copy
speckle), NOT a real render. TMapObjWave was already ruled faithful (TEXA²·RASA ⇒ near-invisible foam).
⇒ The entire ocean-water/EFB-reflection residual is CLOSED: native's smooth turquoise water matches the
real Dolphin oracle. Do NOT re-open the file-select water against `.dff` replays.

**Environment parity (pose-independent regions, native `SB_STAGE=15`+START vs true oracle):**
- water (y320-360, full width): dRGB [-5.7, +9.4, +11.4] — faithful turquoise
- beach (y430-470):             dRGB [+9.2, +4.4, +4.1]
- right sky (above palm):        dRGB [-2.0, -12.3, +4.1]
All within ~10-12 levels; a small ~5-10-level native-brighter systematic (same class as the old
resolved title residual). File-select ENVIRONMENT (sky/water/beach/palm/blocks) is at parity.

**Only remaining open item: Mario's body color, POSE-CONFOUNDED.** Native's file-select capture (START
pressed → "Select data." save-slot overlay, blank card → New/New/New panels) has Mario in a CROUCHED
idle pose (white undershirt/gloves showing) while the oracle save-state is the pre-dialog landing with
Mario STANDING front-on (blue overalls showing). Comparing different animation frames is invalid — the
earlier "Mario pale/washed" was measured across this pose mismatch AND (before) against the speckle-
contaminated replay. NOT yet a confirmed defect. NEXT (tractable, tooling): dump native file-select at
several `SB_DUMP_FRAME_AFTER` values to catch Mario STANDING front-on (matching the oracle pose), then
compare overalls/hat saturation. Only then judge whether any real Mario desaturation exists. The mip
`SB_SKIP_HASH`/dimension-clamp work (aurora ad93196) stays as correct robustness but was chasing the
replay artifact — not a game-path fix.
