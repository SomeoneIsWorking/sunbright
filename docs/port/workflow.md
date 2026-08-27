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
     `decomp/sms` decomp, MEASURE live values (probes, dumps). Name the mechanism before fixing.
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

- **Project knowledge first**: the installed project-info skill's `info.py brief <symptom words>`
  searches claims, instruments, issues, journals and the codemap before a result is re-derived.
- **RE / disassembly**: Ghidra headless is authoritative; use `tools/re/port_dossier.py` for a
  function dossier and `reference/sms_gmse01_funcs.txt` to resolve US addresses. The matching
  decomp source is `decomp/sms/`.
- **Runtime/value evidence**: use the existing `SB_LOG=<channel>` registry, state oracle, graphics
  registry, and probe endpoints. Performance candidates come from no-loss sampling and deterministic
  internal work counts such as `SB_DRAW_STATS`, never host elapsed-time averages.
- **Unit tests**: `ctest --test-dir build --output-on-failure` and
  `ctest --test-dir build-recomp --output-on-failure`; every extracted pure unit gets a spec-derived
  positive and a known-difference/negative control.
- **Bounded runs**: `./run.sh --diagnostic --stage 1 --quit-after <presents>`. The guarded runner is
  selected only by the launcher policy. This path enforces headless rendering, a
  submission ceiling, a wall-clock safety cap, and the kernel amdgpu fault check. Do not assemble a
  turbo command manually. If a launched process must be stopped, capture its PID and use the
  safe-kill helper; never `pkill` a shared binary name.
- **Native-render parity only**: `./run-render.sh` supplies the complete guarded environment. It is
  not the default runtime path and requires the explicit renderer approval gate.

## Ownership pattern (how to port a behavior)
1. Find the behavior's entry + data in `decomp/sms` (+ disassemble the US binary to confirm).
2. Extract the pure part into a header, unit-test it against spec-computed values.
3. Drive/own it from the native path (e.g. `scene_drive.cpp` drives the real perform flow; an
   override replaces a stubbed function). Read the game's own objects from memory; never tap an
   async/emulated layer.
4. Add a value detector and VERIFY the ported values match the reference. Commit + push the milestone.

## THE PARITY LOOP — RE → PORT → IMPLEMENT → TDD → FIX (user directive 2026-06-26)
The named, proven cycle. Run it continuously, one divergence at a time, until parity is GREAT.
Never stop; commit durable state (in-repo notes, commits) as you go so any fresh session can pick up.

1. **RE** — find WHERE the divergence comes from and name the exact mechanism. Backtrace the live
   draw/value (e.g. a per-draw trace switch (the equivalent today is `SB_LOG=<channel>`) printed `SMS_FillScreenAlpha <- TModelWaterManager::
   drawWaterVolume`), then read the decomp (`decomp/sms/...`) + disassemble the US DOL to confirm
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
