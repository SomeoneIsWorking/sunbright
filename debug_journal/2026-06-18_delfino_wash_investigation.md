# Delfino floor "wash" — render-fidelity investigation (2026-06-18, session 12)

User: make ngx render more correctly; might be missing effects, not just brightness. Compare against
GX render-state, not output pixels.

## Findings (method = /gxstate render-STATE diff vs GX + two-run GX-off-vs-ngx pixel A/B)
- **Localized**: the wash is the PLAZA FLOOR (bottom half), ~2.2× too bright (Dolphin-GX oracle luma
  ~90, ngx ~205). Sky/buildings (top) are roughly correct (~o104/n99). NOT a global gamma issue.
- **Ambient render-state divergence FIXED** (commit 6bd521f): /gxstate showed EVERY lit material with
  ngx ambient=(128,128,128) vs GX/xfmem (0,0,0) — ngx used the stale GLOBAL GXSetChanAmbColor reg for
  every material. Fix: LightOn(CLON) materials read their own block ambient (+0x0C); LightOff(CLOF)
  use 0 (matches xfmem). /gxstate AMBIENT now PASS. (SUNBRIGHT_NGX_AMBGLOBAL=1 = old behavior.)
- **The wash is NOT ambient/lighting** (ruled out): the floor is heavily lit (2 lights, diff=SIGN →
  illum≈N·L₀+N·L₁≈1.4 → CLAMPS to 1.0), so ambient 128→0 leaves it unchanged (SUNBRIGHT_NGX_AMB0
  confirmed: floor still 205). Lighting eq (ngx_light.h) is spec-correct. TEV color-op SCALE handling
  (tev_shader.cpp) is faithful. ⇒ the wash is a **TEV COMBINER** divergence ("flat materials too
  bright"): same lights/rasColor as the oracle, but ngx's combiner output is ~2× for the lit floor.

## The MEASUREMENT WALL under no-recomp (why the combiner is hard to pin)
- GX bpmem/xfmem state is GP-THREAD, ASYNC-LAGGED (memory xfmem-not-cpu-oracle). Added a /gxstate
  bpmem combiner-vs-ngx diff (commit 4ce02ac) but per-material color_env DIFFs may be LAG, not bugs —
  printed with a caveat; do NOT chase them blindly.
- The reliable zero-drift oracle (same-present /abshot2 gx-vs-ngx) is BROKEN: ngx owns the frame, so
  Dolphin GX is black (verified gx XFB 0% non-black).
- TRIED SUNBRIGHT_NGX_GXALSO (run the original GX setters+draw via the reentrant primitive so Dolphin
  renders alongside ngx) — runs at ~0.4× (no JIT thrash) and the draws execute, BUT the gx side is
  STILL BLACK: Dolphin's GP frame-cycle (EFB→XFB copy/present) does not complete when ngx owns the
  present. So same-present GX render needs the deeper GP-frame-cycle plumbing, not just running the
  originals. REVERTED GXALSO (non-functional).

## RELIABLE paths to actually fix the combiner wash (next session)
1. **ngx's combiner regs (color_env/alpha_env) ARE reliable** — read synchronously from the J3D
   material object (the same data Dolphin uses), NOT lagged GP state. So verify the TRANSLATION
   color_env→GLSL against the GC TEV spec (a render-test unit like test_lighting/test_projection;
   /gxstate already prints the decoded fields + the generated fragment GLSL). If the translation is
   correct, the wash is in the INPUTS:
2. **Texture decode** — reliably checkable via the Dolphin texture oracle (tex_decode_selftest / /tex,
   /texat). If the floor tile decodes too bright vs Dolphin → that's the wash (the floor is full-lit,
   so out≈texture). RELIABLE (not async).
3. **TEV color/konst register VALUES** (CPREV/C0..C2, KONST0..3) — /gxstate prints ngx's; a too-bright
   const that a stage adds/modulates would wash. Compare vs the material's TevReg/konst data
   (object-model, reliable) — not bpmem.
4. Two-run GX-off-vs-ngx pixel A/B is reliable for the STATIC floor (already used) — good for
   confirming a fix moves the number, bad for per-field state.

## Done this session
- Ambient state-divergence fixed (6bd521f). /gxstate bpmem combiner diff tool + caveat (4ce02ac).
- Wash root-caused to the TEV combiner (ambient/lighting/scale ruled out), reliable-path plan above.
