# 2026-07-10 (continuation 2) — TMarDirector::direct() Ghidra-verified byte-faithful:
# closes the "cadence/ordering" hypothesis; phase-1 stale-flush divergence is NOT control-flow

Continuing the phase-1 stale-flush investigation from `2026-07-10_unified_frame_trace_seq.md`
(supersedes the "carry-over" framing) and picking up the explicit open item from
`2026-07-10_gameloop_fader_ortho_clobber_finding.md` §4 ("pacing/cadence divergence in the
`unk54 -= vsyncRate` tick-accumulator ... a further RE task, not a guessable one-line patch").

## 1. Empirical: native's retrace counter advances by exactly 2 per gameLoop iteration (expected, NOT a bug)

Captured `SB_PLIST_ORDER_DBG=1` unconditional at stage 15 title, retrace span 204..2628
(`scratch/logs/cadence_run.log`, 857827 lines). Every DISTINCT `retrace=` value across the
whole run increases by exactly 2 from the previous distinct value (checked mechanically:
`awk 'NR>1{print $1-prev}{prev=$1}'` over the unique-value stream → 1313 occurrences, all
`2`, zero of any other delta). `"Draw Buffer Group"` (unk40/phase1) fires exactly once per
observed (even) retrace value, 1213 times over 1213 distinct retrace samples, zero misses,
zero doubles — **not** a period-2 skip in rendering. This is just SMS's normal 30 fps logic
tick sampled against a 60 Hz NTSC field counter (`VIGetRetraceCount` counts fields; the game
ticks once per 2 fields) — expected, unremarkable. Ruled out as the cadence divergence.

## 2. Ghidra-decompiled the real retail `TMarDirector::direct()` (US GMSE01, `FUN_80299838`)
and diffed it against `MarDirectorDirect.cpp` line by line

`FUN_80299838` wasn't yet a function in the project's Ghidra DB (no symbol; identified by
call graph in `2026-06-11_choppy_music.md`, lr=`0x802A6170` = `TApplication::gameLoop`'s
`mDirector->direct()` callsite). Created + decompiled via the existing
`scratch/CreateAndDecomp.py` harness:
```
ALL_FUNCS=scratch/allfuncs_direct.txt DECOMP_TARGETS=scratch/decomp_targets_direct.txt \
DECOMP_OUT=scratch/decomp pyghidraRun -H scratch/ghidra_proj sms \
  -process sms_flat.bin -noanalysis -scriptPath scratch -postScript CreateAndDecomp.py -okToDelete
```
→ `scratch/decomp/80299838.c` (191 lines, gitignored, re-derivable with the command above).

**Result: byte-structurally identical to the current port, confirmed against
`MarDirector.hpp`'s verified field offsets** (`mPerformListGX`=0x1C, `Silhouette`=0x20,
`GXPost`=0x24, `Movement`=0x28, `CalcAnim`=0x2C, `unk30`=0x30, `unk34`=0x34, `unk38`=0x38,
`unk3C`=0x3C, `unk40`=0x40):

- The state-machine loop (`unk4C&0x4000`), the `unk54 -= 5` / `+= 600/vsyncTimes` accumulator,
  and the "set 0x4000 → call `unk34`(+0x34)->perform(0xffffffff) → **return immediately**"
  path (decompiled: `FUN(**(param_1+0x34)+0x20)(...,0xffffffff,...); ...; return uVar10;`)
  are identical to `MarDirectorDirect.cpp:261-271`'s `unk34->perform(0xffffffff,...); break;`.
- The render (`else`) branch calls, **in this exact order**: `+0x40` (unk40) →
  `+0x38` (unk38) → `+0x3C` (unk3C) → `+0x1C` (mPerformListGX) → conditionally `+0x20`
  (Silhouette) → `+0x24` (mPerformListGXPost) — identical to
  `MarDirectorDirect.cpp:292-315`'s `unk40 → unk38 → unk3C → mPerformListGX → Silhouette →
  mPerformListGXPost`. No reordering, no dropped call, no different phase attachment.
- The loop bottom (`changeState()` + `unk4C &= ~0x9fff` i.e. clear bits 0x6000) and the
  `while(true)` re-loop into the movement/entry branch are identical to
  `MarDirectorDirect.cpp:318-319` + the enclosing `for(;;)`.

**Conclusion: the RENDER-consumes-the-PREVIOUS-call's-`unk34`-collect structure (i.e. `unk40`
always draws whatever was entered one `direct()` call earlier, never same-call) is not a
porting artifact — it is exactly what the retail DOL does, every single call, forever, in
steady state.** This DEFINITIVELY FALSIFIES the last open hypothesis in
`2026-07-10_gameloop_fader_ortho_clobber_finding.md` (a cadence/ordering divergence in which
`gameLoop` iteration contains render vs. entry): there is no such divergence to find — the
control flow is 100% Ghidra-verified faithful at the instruction level, both for
`TApplication::gameLoop` (previous session, §3 of that journal) and now for
`TMarDirector::direct()` itself (this session).

## 3. Implication: the phase-1 stale-flush divergence must be a DATA divergence, not control-flow

Since `unk40`'s draw genuinely runs, every frame, over whatever `unk34` collected one call
prior — on GC too — the only way retail's real FIFO capture
(`scratch/oracle/fifo/title_press_start_vi_stable_gxseq.txt`) can show literally zero draws
before its first `GXSetProjection` (mirror camera, seq 8) is if **the buffers `unk40` reads
are genuinely, correctly empty at that point on real hardware** — i.e. something about WHAT
gets entered into `DrawBuf Sky Opa/Xlu`, `MapOpa/Xlu`, `Mirror Opa/Xlu`, `LensFlare` differs
between native and GC, not WHEN the entry/draw calls fire (those are now proven identical).

Checked and ruled out one candidate mechanism this session: a second, scene-graph-driven
entry path double-populating the same `TDrawBufObj`s. `drawBufferGroup` (the `unk40` target)
is inserted under `gpLightManager`, itself inserted into `normalScene` (`通常シーン`) — i.e. it
is *also* a member of the real JDrama scene graph, not just `unk34`'s named-lookup list. But
grepping `reference/sms/src` for `通常シーン` finds it referenced only at
`MarDirectorSetupObjects.cpp`'s one-time `setupObjects()` call (search + insert) — nothing
else ever calls `normalScene->perform(...)` directly per frame. The now-dead
`sb_boot_drive_scene()` (a retired Path-B hand-driver, `sms-boot/runtime/sdk_stubs.cpp:422`,
literally `{}` since the 2026-07-07 one-runtime consolidation) is NOT the double-entry
mechanism either — it does nothing. So no second per-frame entry path was found in the
current source; this specific hypothesis is closed, not just deferred.

## 4. Not fixed this session — naming the real next step, per no-bandaids

No source change was made. The control-flow question that motivated the last three sessions'
investigations (order bug? cadence bug? scene-graph double-entry?) is now closed by direct
evidence in all three cases. The remaining, still-open question is a DATA-level one:
**exactly which packets end up in these buffers when, on a per-object `entry()` basis** — the
next session should instrument `J3DDrawBuffer::entryMatSort`/`entryNonSort` etc. (already
has an `SB_ENTRY_MAT`-style backtrace hook) to log, per named buffer, per `direct()` call,
how many *distinct* objects call `entry()` and whether any object enters MORE than once per
`unk34` collect window (a double-entry would explain buffers staying non-empty across what
GC's data-dependent logic — e.g. a visibility/culling flag that's true natively but false on
GC's actual runtime state at title — leaves empty). This is a bounded, mechanical next step,
not a guess; inserting a manual buffer-clear or reordering the perform-list now (without this
data-level evidence) would be exactly the "insert a call to make output line up" pattern this
project bans.

## Diagnostics / artifacts from this session

- `scratch/logs/cadence_run.log` — unconditional `SB_PLIST_ORDER_DBG=1`, stage 15, retrace
  204..2628 (gitignored; re-derive with `SB_HEADLESS=1 SB_STAGE=15 SB_SCENARIO=0
  SB_PLIST_ORDER_DBG=1`).
- `scratch/decomp/80299838.c` — Ghidra decompile of the real US retail `TMarDirector::direct`
  (gitignored; re-derive with the `pyghidraRun` command in §2). Confirms
  `reference/sms/src/System/MarDirectorDirect.cpp` byte-faithful at the control-flow level.
