# 2026-06-20 — Sirena "Manta Storm" goo: native coverage producer ✅ (first visible engine win)

## Outcome
The Sirena goo renders natively under ngx present. PC-native port of
`TPollutionCounterLayer::countTexDegree`'s coverage feedback into ngx's EFB side buffer; the goo plane
(already captured by ngx) samples it as its I8 mask → bright green/yellow goo on the bay.
`runtime/overrides/pollution_inspect.cpp` (`produce_coverage`), pure math in `runtime/ngx/ngx_pollution.h`,
render_test unit `pollution`. Default-on; disable `SUNBRIGHT_NO_POLLUTION=1`.

## Corrections to the prior characterization (the handoff was wrong on two points)
1. **The decomp is NOT stubbed** for countTexDegree / drawTex / initGXforPollutionLayer — full source in
   `reference/sms/src/Map/PollutionCount.cpp`. Disasm of 0x8019b3a0 confirms it matches
   (`lhz 0x32; lhz 0x30; lbz 0x85; lbz 0x84 → initGXforPollutionLayer`). No re-RE was needed.
2. **The feedback DECAYS, it does not grow.** Live (baseline DUMP_TEX): type=4, flags=0, C0=8, C1=128.
   That selects initGXforPollutionLayer's "else" branch: `new = prev - (C1 > prev ? C0 : 0)` — holds any
   texel ≥128, erodes the rest to 0. The "f0→f30 flood-to-full" the handoff read as growth is the
   load-time SEED filling the buffer (NOT the feedback); the feedback merely holds it.

## Ground truth (baseline NGX_PRESENT=0 + SUNBRIGHT_DUMP_TEX, per-frame full-scan of unk54)
- Steady state = **uniformly saturated**: nz=262144 (entire 512×512), mean **254.7**, 99.6% at 254.
  The earlier "47875-texel blob" was only frames 0–1 (the beach-outline seed); it floods to full by ~f8.
- The on-screen goo extent is bounded by the goo PLANE geometry (the beach), NOT the coverage texture —
  which is exactly why `SUNBRIGHT_POLL_FORCE` (blind full fill) coincidentally matched the baseline.
- Every texel of the area/depth map (`unk5C.mMap` @80a760a0, GC-tiled, unk8=9) has depth>0 here
  (polluted 201005 + prohibit 61139 = 262144, depth==0 = 0), so "saturate where polluted" == full.

## The native producer (faithful, real game data, not a blind fill)
Per active layer (unk178[i]!=0), keyed by `unk54` EA, each countTexDegree call:
- **Seed once.** type==4 (the Manta-Storm pollution type — decomp loads `ms_thunder` for it): saturate
  the polluted region of the layer's OWN area map (`mMap[tiled_index(x,y,unk8)] ? 254 : 0`). Other types:
  seed from guest unk54 RAM (de-tiled) — 0 under present → those scenes (plaza/airstrip) untouched.
- **Per-frame feedback** = `sb_pollution::feedback_step` (the real drawTex TEV, render_test-verified):
  type==7 GROW `prev+(prev>C1?C0:0)`; flags&2 `prev-2`; else `prev-(C1>prev?C0:0)`.
- Encode I8→ARGB (v,v,v,v), `sb_ngx_efb_store_copy(ea)` + invalidate **every frame** (a skip-when-
  unchanged optimization BROKE the render — the texcache bind falls back to empty guest RAM; reverted).
- STOPGAP (named in code): the type==4 saturated seed stands in for the boss/scenario load-time
  full-pollute, which lives in unported boss/event logic. The per-frame stamp rasterization
  (drawTexStamp/doTask cleaning, joint/model/revival) is also TODO — idle Manta-Storm has zero active
  stamps (verified tex/joint/revival/model = 0 every frame), so it's correct for this scene; dynamic
  spray-cleaning needs the stamp path next.

## Verification (cited, not vibes)
- **Frame-aligned ON/OFF A/B** (both NGX_PRESENT=1 fastboot STAGE=6, same emulated t≈25s, only
  SUNBRIGHT_NO_POLLUTION differs): producer ON → bright goo across the bay; OFF → goo absent, dark
  water (`scratch/screenshots/poll_ts_t25` vs `poll_off_t25`). Producer is the cause.
- **Visual match** to the GX baseline goo (`poll_gx_t25.gx.png`) — same bright green/yellow swirl.
- Host buffer verified full (nz=262144, mean 254) vs the DUMP_TEX oracle.
- render_test `pollution` PASS (feedback branches + GC tiling vs hand-computed truth).
- The save-state A/B (`ab_oracle.sh scratch/sirena_goo.sav`) reported 30% but is **camera-drift
  confounded** — Sirena fastboot is an active intro auto-pan, so the oracle settled to the goo-field
  view and ngx to the wall view (different frames). NOT a fidelity number; don't cite it. A clean numeric
  Sirena oracle needs a save in a SETTLED (non-animating) state with goo in view (gameplay automation).

## Don't re-chase
- "feedback grows / decomp is stubbed / re-RE the TEV from disasm" — all false; decomp is full & matches.
- "coverage is a small blob" — no, steady state is uniformly saturated (whole 512×512 @254).
- Per-frame push is mandatory (skip-when-unchanged drops the goo).
- STAGE=6 fastboot spawns past an intro auto-pan; the settled view is a wall (little goo) — the goo
  field is visible mid-pan (~t20-30). Don't conclude "no goo" from the settled wall view.
