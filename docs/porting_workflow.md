# My porting workflow (the "continue" loop) — HARD RULE

This is the workflow to run on every "continue" and whenever a unit of work finishes and there is
more to port. It is a hard rule (user directive 2026-06-23), summarized in `CLAUDE.md` →
"🔁 THE 'continue' PORTING LOOP". Goal: turn the blackbox into something OWNED, value by value,
never by eyeballing, never by stopping.

## The loop (run top to bottom; the first NO is the next task)

1. **Is the TOOLING enough?** Can I observe and VERIFY the current divergence as a NUMBER/VALUE?
   - NO → build the tooling FIRST. A value-divergence detector (capture native value vs the
     reference value, log which element diverges), a verification harness, an oracle. Never decide
     by looking at a frame — that's banned (TDD-NOT-eyeballing). If a tool can be fed garbage, make
     it refuse loudly.
2. **Is the RE enough?** Do I understand the ORIGINAL behavior — the data layout, the exact code
   path, the values it produces, WHY?
   - NO → do more RE. Disassemble (`sunbright-recomp --disasm/--xref/--callees`), read the
     `reference/sms` decomp, MEASURE live values (probes, dumps). Name the mechanism before fixing.
3. **Is the OWNERSHIP enough?** Is the behavior ported to a PC-native path that produces the CORRECT
   verified values?
   - NO → more RE + port it. Own the path natively; no blackbox, no bandaid, no magic constant.

## There is no "blocked"
Blocking is essentially impossible. The only real blocker is an external catastrophe (ROM deleted,
disk dead). Everything else — "I can't verify this", "two viable paths", "I'd need to build X first",
"the sync is hard", "I'm not 100% sure", "this is big" — is **the next task, not a stop**. Do
whatever is needed to unblock yourself: build the tool, find the address, RE the unknown, pick the
path that advances ownership, go. Never punt the decision to the user.

## Concrete tools/seams in THIS project (so the loop is fast)
- **RE / disassembly**: `./build-freshtest/sunbright-recomp scratch/disc/sms.iso --disasm <hexaddr>
  [n]` · `--xref <addr> --funcs reference/sms_gmse01_funcs.txt` (callers) · `--callees <addr>`.
  Data symbols: `reference/sms_gmsj01_symbols.txt` is JP (GMSE01 = US differs; map by role/xref).
  DOL data reader: `scratch/doldump.py`.
- **Decomp source of truth**: `reference/sms/` (the game's own C++; the native engine compiles it).
- **Native value detectors / probes (the verification layer)**: `SB_CAM_DBG` ([cam-oracle],
  [proj-diverge]), `SB_J3D_DBG` ([cov], [sky-sphere], [stage-light], [mat], [litprobe]),
  `sb_gx_get_projection` / `sb_gx_get_cur_posmtx` / `sb_gx_get_chan_matcolor` exports, the per-shape
  coverage/ndc probes in `sms_boot_j3d_capture.cpp`. Add a new export+log whenever you need to SEE a value.
- **Render oracle (sms-boot vs vanilla GC)**: `tools/render/boot_vs_vanilla.sh` (vanilla = sunbright
  `NGX_PRESENT=0` = real guest GX, no widescreen/gecko → pure-vanilla 4:3) + `ab_diff.py` (per-region
  divergence). KNOWN GAP: cross-engine frame SYNC unsolved (different frame clocks, no shared save) —
  when this matters, BUILD the sync (detect vanilla's first plaza render, or read+match `gpCamera`),
  don't punt.
- **Unit tests (pure-fn TDD)**: `ctest --test-dir build-native -E platform_test` (28+ tests); add a
  `render_test`/`platform_*` unit for every pure unit you extract, asserting spec-computed values.
- **Run sms-boot headless**: `cmake --build build-native --target sms-boot -j$(nproc)` then
  `setarch -R env SUNBRIGHT_DISC=scratch/disc/sms.iso SB_THP_FAST=1 SB_WATCHDOG_SECS=0 SB_J3D_DBG=1
  SB_FRAME_DUMP=1 SB_FRAME_DUMP_ON_SCENE=1 SB_FRAME_DUMP_MAX=2 SB_HOST_ALLOC_CAP_MB=3072
  ./build-native/sms-boot` (scene ~VI 6121; frames → `scratch/frames/boot_*.ppm`). Always
  `pkill -9 -x sms-boot` after. Logs have NUL → `grep -a`.

## Ownership pattern (how to port a behavior)
1. Find the behavior's entry + data in `reference/sms` (+ disassemble the US binary to confirm).
2. Extract the pure part into a header, unit-test it against spec-computed values.
3. Drive/own it from the native path (e.g. `scene_drive.cpp` drives the real perform flow; an
   override replaces a stubbed function). Read the game's own objects from memory; never tap an
   async/emulated layer.
4. Add a value detector and VERIFY the ported values match the reference. Commit + push the milestone.

## THE PARITY LOOP — RE → PORT → IMPLEMENT → TDD → FIX (user directive 2026-06-26)
The named, proven cycle. Run it continuously, one divergence at a time, until parity is GREAT.
Never stop; commit durable state (in-repo notes, commits) as you go so any fresh session can pick up.

1. **RE** — find WHERE the divergence comes from and name the exact mechanism. Backtrace the live
   draw/value (e.g. `SB_IMM_TRACE_SOLID` printed `SMS_FillScreenAlpha <- TModelWaterManager::
   drawWaterVolume`), then read the decomp (`reference/sms/...`) + disassemble the US DOL to confirm
   the precise semantics (e.g. `GXSetColorUpdate(GX_FALSE)` => writes NO colour). No fix until named.
2. **PORT** — transcribe the original behaviour faithfully (control flow / state / GX semantics)
   from the decomp + DOL. STAGE A may stub pure-rendering helpers, but the CONTROL/STATE is exact
   (e.g. Hx_Circle's timer->done machine; the dst-alpha write-mask + forced-alpha rules).
3. **IMPLEMENT** — wire it into the native path (the SDL3-GPU pipeline, the imm capture, an override).
4. **TDD** — BEFORE trusting it, write a spec-truth test that FAILS if it's wrong: a parity test
   (`native/render/tests/parity/*_test.cpp`, assert readback pixels vs hand-computed GX truth) for
   renderer behaviour, or a game-behaviour test (`platform_*_test`, assert the exact state sequence /
   frame counts from the disassembly) for logic. Prove SENSITIVITY (a wrong constant fails it).
   Never "see-then-assert"; never eyeball.
5. **FIX / VERIFY** — make the test green, then confirm the live divergence (the value/number) moved
   the right way. Commit + push the milestone. If the proper fix is too big now, land an in-code
   `STOPGAP:` (honest absent-effect, never a faked value) and record the proper fix. Loop to next.

Reference instance (read when unsure): the Delfino plaza "white wash" -> RE pinned
`SMS_FillScreenAlpha`'s `GXSetColorUpdate(FALSE)` -> ported PE write-mask + dst-alpha semantics ->
implemented in `gx_sdlgpu` pipeline + imm capture -> `dst_alpha_test` parity rung (masked composite,
pixel-exact, sensitive) -> wash gone, plaza clean; the unportable mask-geometry piece is a marked
`STOPGAP`. Commits c39a523..93e5293.

## Record findings durably
In-repo (`debug_journal/`, this file, `CLAUDE.md`) for project-critical facts (travels with the
repo); `<home>/.claude` memory for cross-session/cross-machine pointers. Record dead ends too.
