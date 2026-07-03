# Sunbright — GameCube Static Recompiler → PC-Native Port of Super Mario Sunshine

This file holds **hard rules and pointers only**. Session commentary, "FIXED" narratives, and
architecture history live in `debug_journal/` and `docs/`. Every rule below is standing —
don't propose alternatives, don't argue around them.

---

## 🔀 DIRECTION PIVOT (2026-07-04) — TWO-TIER RENDER: seam-oracle now, direct-engine later

The prior "NATIVE_PC = ship state, NO EMULATION CHASING" doctrine (2026-07-03) applied to the
wrong tier. As of 2026-07-04, the design is:

1. **Tier 1 — GX-seam SDL3 renderer (current `sms-boot NATIVE_PC` sink) becomes the IN-PROCESS
   ORACLE.** Its whole job is to reproduce Dolphin-GX pixel output from the same GX-seam
   captured state (blend/z/TEV/lights/matrices/texture/EFB). This IS emulation of the GC's
   fixed-function pipeline. Push it to full Dolphin-GX pixel parity. Chasing that parity is the
   goal here — not a bandaid.
2. **Tier 2 — Dolphin videovulkan sink (`SB_RENDER=oracle`)**. Already exists in the same
   binary as Tier 1. Ground truth for measuring Tier-1 pixel divergence at a state-pinned
   frame. Not the product; fixes never land here.
3. **Tier 3 — a FUTURE no-GX PC-native engine (not yet built) ships instead.** It bypasses the
   GX seam entirely: game logic emits PC renders directly (native skybox actor, native water,
   native 2D panes, native character shaders). That NEW engine will be A/B'd IN-PROCESS against
   the Tier-1 seam oracle — same binary, same state, two renderers running so pixel divergence
   is 100% renderer-only. That is where "port intent, do NOT emulate GX" applies.

Interpret the sections below with this in mind:
- Rules that used to say "NATIVE_PC must not emulate GX" now apply to the FUTURE Tier-3 engine.
- The current SDL3 renderer that consumes GX-seam captures MUST emulate faithfully — that IS the
  seam oracle. "No emulation chasing" was correct only because we planned NATIVE_PC to ship;
  we're not shipping it anymore, it's an instrument. `SB_RENDER=oracle` (Dolphin videovulkan
  in-process) remains ground truth for measuring seam-oracle divergence.
- Prior journal entries labeled "hand-tuning bandaid" that made the seam oracle match Dolphin
  are re-evaluated on their own merit now — the goal is Dolphin parity, so byte-for-byte
  reproduction of TEV/EFB/phase-composite is on the table. It is still preferred to do this
  by porting the RE'd game logic faithfully; ENDPOINT-COLOUR hand-tuning is still a bandaid
  (it papers over an RE gap rather than closing it).

The "sole active target — title-screen parity" gate still holds, now measured against the
GX-seam oracle vs Dolphin-videovulkan oracle at a state-pinned frame. As of 2026-07-04 commit
0039f89, that comparison is meaningful for the first time (STATE PARITY GREEN, mean_abs 29.82).

---

## 🚫 NO EMULATION CHASING — RE the intent, PC-native emit (HARD RULE, FUTURE Tier-3 engine only)

**When the Tier-3 no-GX engine exists**, it implements the game's INTENDED effect in native
SDL3 GPU code. It does NOT try to reproduce the GC's fixed-function pipeline. TEV combiners
don't map to modern shaders; EFB copies / ph1-4-6 "composite" pipelining don't map to SDL3 GPU
targets; "GC lighting" chan-ctrl formulas don't map to native GLSL. Every past "make GX faithful"
fix on the WRONG tier produced subtler wrongness. See
`debug_journal/2026-06-30_fileselect_overbright_is_efb_target_structure.md`.

Tier-3 discipline (for when the direct engine is being built), in order:
1. **See the visible defect** in a screenshot of the Tier-2 direct engine.
2. **RE the effect** — what actor renders it on GC, its VISUAL INTENT (skybox, ripple, 2D pane,
   save-slot card), its inputs (positions, textures, lights, game state). If the actor isn't
   RE'd yet, RE'ing it IS the first task.
3. **Emit PC-native from game logic** — the Tier-2 game code (still `reference/sms` decomp
   under `SMS_NATIVE_PLATFORM`) calls native paint APIs directly instead of the GX SDK. Do
   NOT route through `sb_tev_gen_fragment` or J3D→GX→emulator translation.
4. **Verify in-process against the Tier-1 seam oracle** for the visible intent. Pixel-level
   parity is not required (the whole point of Tier-2 is to bypass GC's fixed function); INTENT
   parity is.

## 🎯 Tier-1 (seam oracle) discipline — chase Dolphin-GX parity

**RE'd bodies land as CLEAN C in `reference/sms/src/`** — proper types, named fields, hand-authored
bodies, mirroring soh3d's `oot3d-decomp/src/code/` shape. NOT markdown docs, NOT raw Ghidra dumps.
Per arc: RE → commit clean C to `reference/sms` → port sunbright native referencing that C →
verify pixel parity against `SB_RENDER=oracle` at a state-pinned frame. Two commits per arc.
RE'd but no `reference/sms` commit = incomplete arc.

**Byte-exact reproduction of Dolphin-GX output IS allowed and encouraged.** That includes
faithful TEV combiner emulation, EFB copy semantics, phase-composite (ph1/ph4/ph6) mechanics,
and GC lighting chan-ctrl formulas — implement whatever produces the same pixel Dolphin-GX
produces from the same state. Prior `banned mechanisms` (`SB_FS_COMPOSITE`, `SB_SKIP_PH6_MAPXLU`,
`snapshot_efb`) are UN-BANNED for the seam oracle — they may be legitimate faithful ports if
they're the actual game-logic route. Judge each on RE fidelity, not on the doctrinal ban.

**No hand-tuning endpoint colours to match oracle pixels.** Even for the seam oracle: getting
Dolphin parity by hand-picking RGB endpoints or α caps is a bandaid that hides the real RE gap.
The right route is RE the material/TEV/blend/vertex chain; the wrong route is `if (this pixel
looks off, add 20 to R)`. Sampling oracle pixels is legitimate DIAGNOSTIC ("what color IS it?"),
never a ship-state value.

**Metric discipline.** `tools/render/title_overbright.py` reports both `channel_mean_delta`
(LABELED MISLEADING — signed per-channel mean then |·| cancels sky-overbright + ground-underbright)
and `mean_abs_pixel_delta` (TRUE per-pixel |native − oracle|). Move the second one.

## 🎯 TITLE-SCREEN PARITY IS THE SOLE ACTIVE TARGET (Tier-1 seam oracle vs Dolphin-videovulkan)

No partial-close-and-move-on. FULL title-screen pixel parity between the Tier-1 GX-seam SDL3
renderer (default `SB_RENDER=native` in `sms-boot`) and Dolphin-videovulkan
(`SB_RENDER=oracle` in-process or `build/sunbright`) is a HARD GATE on moving to any other
scene. Once title parity closes, the seam oracle is trusted as a proxy for Dolphin-GX, and
Tier-3 (the future no-GX PC engine) can begin.

Baseline (2026-07-04, commit 0039f89): STATE PARITY GREEN across all 21 pin_diff fields,
mean_abs pixel Δ 29.82. Interior of frame essentially bit-exact; sky/horizon/water rows drive
the residual. Named next arcs:
- Sky cloud coverage — oracle covers full sky, native shows sparse strip. Likely EFB
  ph1/ph4/ph6 composite mechanism the seam oracle must now port faithfully (was previously
  banned; UN-BANNED as of the 2026-07-04 pivot for the seam oracle).
- Horizon strip right column — cell (1,3) `|Δ|=69`, related to sky cloud coverage.
- Water bottom row cells (3,0-3,3) `|Δ|~30`.

Each is a discrete RE-then-port arc against the Dolphin-GX oracle. Worker picks which surfaces
cleanest. If residual persists after byte-exact RE, the substrate ABOVE (view/projection/actor/
material/capture ordering) needs the next RE.

## 🛑 THE ONLY ALLOWED DEBUGGING PATH

For any bug/crash/hang:
1. **Find where** it happens.
2. **Find its root cause** (name it — symptom gone ≠ fixed).
3. Then, because recomp is eradicated (see architecture below), the only path is:
   **Reverse-engineer that behavior, port it to a PC-native override, fix it there.**

`force_jit` / `SUNBRIGHT_FORCE_JIT` / interpreter fallback are **diagnostics for bisection only**,
never the fix. "JIT works but recomp doesn't" / "the generated C reads faithful yet behaves
differently" are *deductions* that a bug exists, not *identified* defects — they fall to the
own-it-natively path.

**NO BANDAIDS.** No magic constant/offset to make output line up, no special-casing the failing
input, no `try/except`-swallow, no retry/sleep-to-fix-a-race, no commenting-out a failing check,
no hardcoded expected value, no "temporary" workaround. If the fix is genuinely too big right
now, name the proper fix and let the user decide — never slip a hack in as if it were the fix.

## 💥 FAIL FAST — parse/contract failures CRASH at the root cause, never return nil

Bad magic, unparseable header, precondition violated, unexpected null, out-of-range value →
CRASH RIGHT THERE with a clear message. Do NOT return nil/0/empty and let it propagate. A
swallowed failure defers the crash to a confusing downstream null-deref far from the real cause.
Message must name the location and dump offending values (magic bytes, bad size, null pointer's
origin) so the root cause is readable from the panic alone. Use `OSPanic`/assert, guard with
`SMS_NATIVE_PLATFORM` so original decomp behavior is preserved off-platform. An honest
"not implemented yet" stub may return a sentinel ONLY if loudly logged/asserted.

## 🔧 TOOLING / VERIFICATION FIRST

**If the harness that would verify a change is missing or broken, FIX/BUILD THE HARNESS BEFORE
the change.** No exceptions.
- A green-looking tool that silently compares against garbage is worse than none — it manufactures
  false conclusions (see `debug_journal/2026-06-19_verification_oracle_built.md`, and the
  `abshot2` all-black-oracle trap in memory).
- When a tool can be fed degenerate/empty/stale input, it MUST detect and refuse loudly.
- "I can't cheaply verify this in a reachable scene" is a STOP: build the reachability/oracle
  tooling first, or pick a target that IS verifiable.
- Parity tooling must surface NAMED divergences (per-draw / per-attribute / per-state field) —
  aggregate counts force guessed fixes. Same for divergence ORIGIN (writer PC, stack trace,
  upstream propagation). Extend the diagnostic FIRST; don't hand-decomp each divergence.
- **Same-state capture harness required** for any parity screenshot — pinned frame / camera /
  lights / inputs / RNG or fail-fast.

### Pick the RIGHT verification for current state (user directive 2026-07-01)

Two layers, applied at different times:
1. **UNIT TEST from RE (spec-derived expected values) — ALWAYS ships WITH a port.** The
   verification harness IS a unit test whose expected values you HAND-DERIVE from disassembly/
   decomp (what the original computes), Dolphin-free / no ROM / no GPU. Mandatory for every
   ported function. See `render_test` / `sms_boot_setlight` pattern.
2. **ORACLE / whole-system parity differential (vs Dolphin-GX) — only once the subsystem is
   COMPLETE enough to run end-to-end.** Standing up parity comparison on something with KNOWN
   implementation gaps is the recurring TRAP: every "divergence" the oracle reports is
   ambiguous (missing feature vs real bug). Don't diagnose why the car won't start when the
   engine isn't installed.

**Order: implement missing pieces → unit-test each ported piece from RE → THEN turn on
oracle/parity to catch REAL fidelity bugs.**

### TDD per divergence — close-test first, from the NAMED divergence not the fix

Each parity iteration: write a failing close-test authored from the NAMED divergence, verify red
on HEAD, then fix, verify green, commit test+fix together. Refactor commits are behavior-neutral.
Unit test green ≠ visible fix — a spec test proves logic, not that the function runs when the
divergent pixel is drawn. Each visible defect needs its own close-test that transitions fail→pass.

## 🧭 Ghidra is the default RE tool

Ghidra 12.0.4 available bare from `$PATH` (`analyzeHeadless`, `pyghidra`). Do NOT hard-code any
filesystem path in docs/scripts/sends — the `<home>/.local/bin/` symlinks are the interface, upgrades
touch only the symlink target. **Never invoke GUI variants** (`ghidraRun`, `pyghidraw`,
`ghidraGUI`) — they spawn X11 windows regardless of subcommand.

Prefer Ghidra decompiler (headless via `decomp-port` skill's DecompDump.py, or interactive) over
hand-disasm. Hand-disasm is legitimate only for spot verification when Ghidra falsified something,
and NAME why in the turn. The lui/addiu sign-extension trap is a known Ghidra listing gotcha —
cross-check listing vs decompiler view.

Comment-code parity ≠ verification. In port/decomp-driven native reimpls, cross-check against
the decomp source, not against your own header comment (a self-consistent buggy comment + buggy
code was a real 2026-07-03 psxport miss).

---

## Architecture — TWO BINARIES + TWO TIERS (with a THIRD tier coming)

1. **`sms-boot` = the RE'd-decomp host.** Runs `reference/sms` decomp as PC-native C++ over
   guest-layout RAM. Contains up to three render tiers via `sb::engine::mode()` (see
   `runtime/engine.h`), chosen at runtime from `SB_RENDER=native|oracle` (default `native`):
   - **Tier 1 (`NATIVE_PC`, default)** — GX-seam SDL3 GPU renderer (`native/render/` + pure
     decoders in `runtime/ngx/`). Consumes game GX SDK calls captured into `GXState` structs
     and rasterizes via SDL3-GPU. **Serves as the in-process ORACLE for Tier 3**; its target
     is Dolphin-GX pixel parity. Faithful GX/TEV/EFB reproduction is the goal here (see the
     2026-07-04 pivot at the top of this file).
   - **Tier 2 (`GX_ORACLE`)** — Dolphin's `videovulkan` backend linked into the same binary,
     game's GX calls routed through it. GROUND TRUTH for Tier-1 seam-oracle parity. Not the
     product. Fixes NEVER land here. Same Dolphin video pipeline that `build/sunbright` uses.
   - **Tier 3 (FUTURE, no name yet)** — no-GX PC-native engine. Game logic emits PC renders
     directly, bypassing the GX seam. This is the eventual ship product. It will A/B in-process
     against Tier 1 at a state-pinned frame to measure "renderer intent divergence." Not built
     yet; unlocked only once Tier-1 title parity closes.

   Build paths: root-CMake `build/native/sms-boot` includes Tiers 1+2; standalone
   `build-native/sms-boot` (`cmake -S native -B build-native`) is Tier 1 only.
   `./run.sh` launches whichever is present.
2. **`build/sunbright` = pure Dolphin-GX ORACLE** (external process, ground truth). Runs the
   game under Dolphin's JIT, renders via Dolphin's GX pipeline. Same rasterization as Tier 2
   in-process — they're equivalent references. Used by cross-process harnesses (title_pinned.sh)
   when the in-process Tier-2 sink is not yet stable enough.

**Recomp is ERADICATED from the game binary (2026-06-30, 00a573f).** `generated/` is no longer
linked; `sunbright_purejit_mode()` is unconditionally true. `sunbright-recomp` is kept as
**offline static-analysis tooling only** (`--xref` / `--callees`). Every bug is now
own-it-natively (RE + PC-native override) or a Dolphin-JIT issue.

**NGX renderer name.** The old NGX Dolphin-hybrid capture renderer is deleted (00a573f). The
name "ngx" survives ONLY on the **pure shipping decoders** in `runtime/ngx/` that sms-boot
reuses. Do NOT resurrect the Dolphin-hybrid renderer.

**The valid geometry/lighting ORACLE is the Dolphin GX COMMAND STREAM**, never xfmem
(async-lagged, proven 2026-06-16 — see memory `xfmem-not-cpu-oracle`). `runtime/gx_stream.cpp`
captures gather-pipe bytes; `runtime/gx_parse.h` decodes via Dolphin's own OpcodeDecoder.

**FIDELITY by VALUE per render pass, NEVER by eye** (`tools/render/parity_sweep.py`). Compare
like-pass to like-pass (native drive_sky / scene-perform / drive_chr / drive_hud; oracle
EFB-gen / draw-buffer): geometry (matrices, vert counts/bbox) + lighting (GX lights, ambient,
chan-ctrl).

**sms-boot threading = SDL main thread + ONE game thread, NO others**
(memory `[[sms-boot-threading-architecture]]`, `native/src/boot.cpp`). Game always runs on its
own thread; process-main owns windowing/present + SDL event pump. "Window vs headless" =
whether a window is shown (`SB_WINDOW`); "turbo vs real-time" = pacing (`SB_TURBO`). The game's
OSThread async-loaders run synchronously via the cooperative scheduler (`os_impl.cpp`);
standing goal = eliminate async worker threads.

**Guest-layout native engine, no Dolphin (end state).** Engine objects stay guest-RAM, GC-layout
(32-bit BE pointers, GC offsets) in the shared arena. PC owns engine code as native C++ that
operates ON that guest layout (`native_jas`, `sms_drawsync_lossproof`, `native_card`, the native
renderer). Gameplay stays recompiled on the same memory. Boundary = plain function-call
overrides over shared guest memory; NO handles/getters/marshalling.

⛔ The "flip" / host-layout-engine architecture was tried and deleted. See
`docs/DO_NOT_REVISIT_FLIP.md`.

**Historical detail** for the eradication pivot, call model, JIT hook, memory bridge, and
hybrid execution: `debug_journal/2026-06-18_no_recomp_jit_native_pivot.md`,
`docs/architecture.md`, `docs/dolphin_independence.md`, `docs/native_threading.md`.

---

## Build, run, environment

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_VULKAN=ON
cmake --build build -j$(nproc)          # or /build

./run.sh [rom.rvz]                       # native product; ROM via $SUNBRIGHT_ROM / .env / rom.rvz drop-in
```

`run.sh` pins `SDL_VIDEODRIVER=x11`, defaults to Vulkan, 3× internal res, 16:9 widescreen.
Native-Wayland fails ("failed to initialize video backend"); XWayland works for both OGL + Vulkan.
Keyboard → GC pad: Enter=Start, Z=A, X=B, C=X, V=Y, Q=Z, A=L, S=R, arrows=stick, F11=fullscreen.
Kill a stuck run with `timeout -s KILL N`.

Env-var reference (fastboot, backend, dump, autostart, probe, res-scale, widescreen, trace,
diff, discover-pointers, dbg_*) lives in `docs/architecture.md`. Diagnostic env vars must be
routed through the tracked registry (not ad-hoc `getenv`); prune dead ones.

**Frame-dump perf trap:** Dolphin persists `DumpFrames` to `<home>/.config/dolphin-emu/`. One past
`SUNBRIGHT_DUMP=1` run leaves it on, throttling every later run to ~0.15× real-time.
`main_sdl.cpp` sets both flags explicitly each run. If a run is mysteriously slow, check
per-thread CPU: hot `FrameDumper` = this.

**Probe REPL** at `http://127.0.0.1:17654` (env `SUNBRIGHT_PROBE=1`). Prefer REPL endpoints
(`/metrics`, `/r`, `/fn`, `/stack`, `/cur`, `/loadstate`, `/savestate`, `/pad`, `/help`) over
env-gated `fprintf` + rebuild. See `runtime/probe_server.cpp`.

**Oracle A/B harness:** `tools/render/ab_oracle.sh <save.sav>` (save-state, preferred) or
`tools/render/oracle_ab.sh [emu_secs]` (fastboot plaza only) → `tools/render/ab_diff.py` yields
mean abs pixel delta + 4×4 grid + heatmap. A fix MUST drop this number. Refuses empty/black
frames (exit 3) so it can't report a meaningless number vs a dead oracle. Details:
`docs/render_ab_harness.md`, `docs/audio_ab_harness.md`.

## Skills

`/recompile` (offline analyzer regen) · `/analyze-rom` · `/build` · `/update-docs` ·
`/patch-func ADDR`.

## Historical / subsystem detail — pointers

- **Native audio engine** (JAS port, WSYS/BARC/BMS, SE/BGM/3D layer, M1-M3): `docs/native_audio_engine.md`, `docs/audio_data_formats.md`, `docs/audio_ab_harness.md`.
- **Renderer / TDD / oracle history**: `docs/native_port_plan.md`, `docs/render_ab_harness.md`, `debug_journal/2026-06-19_verification_oracle_built.md`, `debug_journal/2026-06-30_title_parity_confounds.md`.
- **Recomp era architecture** (call model, JIT hook, memory bridge, hybrid execution, interrupt-delivery hazard, differential harness): `debug_journal/2026-06-18_no_recomp_jit_native_pivot.md`, `docs/architecture.md`, `docs/dolphin_integration.md`.
- **Fastboot / file-select / title / gameplay landings**: see `debug_journal/` (indexed by date + topic).
- **Instruction coverage table**: `docs/architecture.md` (or `tools/recompiler/ppc_decoder.cpp` for authoritative status).

## Findings registry

Learned a non-obvious fact worth keeping? Add it to `debug_journal/<date>_<topic>.md` and commit
alongside the code. Dead ends too, not just wins. Fix stale notes when a later diagnosis
supersedes them — a confidently-wrong note is worse than none. Do NOT append session commentary
to this file; that's what the journal is for.
