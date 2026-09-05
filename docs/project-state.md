# Project state

Factual capability ledger for Sunbright. Epic intent is `docs/project-goals.md`; architecture and
ordering are `docs/architecture.md` and `docs/port/migration.md`; atomic work is `docs/issues/`.

## Comparison baseline

The user-visible baseline is the unmodified NTSC-U GameCube release (`GMSE01`) running on original
hardware or Dolphin with console execution, GX rendering, 4:3 framing, and normally 30 Hz
presentation. The repository's former gameplay executor is absent; Sunbright
currently has no gameplay executable while its shared runtime executor is missing.

## Current focus

S001 is the current focus: boot exact `GMSE01` through `gcnport` and Dolphin's JIT, then prove the
runtime `J3DShape::draw` hook at `0x802e0390` and its one-call original-body path.

## Capability inventory

| ID | Capability / observable outcome | State | Dependencies | Goals |
| --- | --- | --- | --- | --- |
| S001 | Exact `GMSE01` boots under `gcnport`/Dolphin JIT and reaches the `J3DShape::draw` runtime hook at `0x802e0390` | missing | S002, S003 | G003, G004 |
| S002 | `gcnport` supplies a title-neutral Dolphin dynarec executor with image identity, bounded exits, invalidation, and diagnostics | missing | — | G003 |
| S003 | Sunbright native overrides and original calls use robust image-scoped runtime dispatch | missing | S002 | G003, G004 |
| S004 | The PC-native semantic renderer covers the complete visible J3D/J2D/particle/effect stream | partial | S003 | G004 |
| S005 | Native decomp adapters and recovered source provide independent semantic and behavior evidence | partial | — | G002, G004 |
| S006 | Smooth presentation covers every eligible moving source and keeps native-rate modes separate | partial | S001, S004 | G001 |
| S007 | Reached decomp behavior is upstream-converged, named, and implemented from evidence | partial | — | G002 |
| S008 | The native/dynarec product passes representative interactive gameplay conformance | missing | S001, S004, S011, S016 | G001, G003, G004 |
| S009 | Offline generator, emitted corpus, static dispatcher/runtime glue, tests, and launch paths are absent | verified | — | G003 |
| S010 | Independent Dolphin/decomp/binary oracle evidence can locate first divergence and prove controls | verified | — | G002, G003 |
| S011 | Native audio is complete and integrated with the native/dynarec gameplay product | partial | S001 | G003, G005 |
| S012 | Application lifecycle, typed configuration, Lucent logging, and structure boundaries are mechanically enforced | partial | — | G005 |
| S013 | Zero-argument launcher provisions and runs only the native/dynarec product from a user-supplied image | blocked | S002, S008, S009, S012 | G003, G005 |
| S017 | JIT gameplay is qualified independently on x86_64, Apple Silicon macOS AArch64, and Android arm64-v8a | missing | S008 | G003, G005 |
| S018 | Asset-free automation verifies the redistributable native renderer and tooling on supported hosts | partial | S012 | G005 |
| S014 | Desktop packages provide no-terminal first-run setup and contain no game content | missing | S013 | G005 |
| S015 | Widescreen renders additional world coverage through projection/viewport/scissor ownership | partial | S001, S004 | G001, G004, G005 |
| S016 | Native input, controls, saves, and settings work through one typed product policy | partial | S001, S012 | G003, G005 |

## Capability details

### S001 — first dynamic title discriminator

Missing capability: authenticate exact `GMSE01`, start it through `gcnport` and Dolphin's shipping
JIT, report nonzero translated-block execution, invoke a runtime override at `0x802e0390`, submit its
semantic J3D value, and execute the original body through a one-call override suppression. Link and
selector inspection must prove ordinary cold blocks compiled before execution and every bounded
fallback has a typed reason, guest PC, block/instruction counters, and JIT denominators. Boot alone
does not advance S008.

### S002 — gcnport Dolphin executor

Missing capability: create the shared GameCube framework around Dolphin's maintained runtime dynarec.
It must own authenticated image/module generations, CPU/thread state, hooks, original calls, bounded
host exits, executable-memory invalidation, and diagnostic counters without duplicating Dolphin's
decoder, code cache, memory, or device implementations. It must permit only DuckStation-style
bounded fallback after JIT compile/safe-execution refusal and keep interpreter-only execution an
explicit diagnostic mode.

### S003 — native override dispatch

Missing capability: move Sunbright's useful native registrations onto the `gcnport` runtime table.
The key must prevent stale address reuse; installs/removals must revoke direct links; nested guest
calls must re-enter the dispatcher; and `superCall` must suppress only the current override for one
ordinary JIT call. Positive and negative controls must cover cache miss/hit, chaining, invalidation,
and a disabled override.

### S004 — PC-native semantic rendering

Existing implementation evidence covers a renderer-neutral ordered J2D stream (pictures, gradient
rectangles, windows, immediate draws, and resource-font glyphs), all eleven tiled game image formats
and three palette formats, rigid and multi-matrix J3D meshes, multiple ordinary textured/lit/layered/
masked/effect material families, cull/depth/alpha/blend policy, high-level camera projection, stage
lights, directional specular, linear fog, and standard particle billboards. Watched GPU controls and
bounded title/stage audits recorded nonzero output; C077–C096 contain the detailed scopes and
falsifiers.

Gap: surviving adapters are native/decomp evidence and are not attached to the new `gcnport` product.
Material families, non-billboard particles, image producers, screen effects, full-frame ordering, and
visible presentation remain incomplete. The new JIT seam must preserve the same value-only contract;
no old body or GX compatibility path may become a silent fallback after its semantic owner is proven.

### S005 — decomp evidence adapters

The native-layout adapters compile against `decomp/sms` and have exercised the same semantic values
as the guest-layout adapters without sharing objects. Bounded decomp/Aurora runs have reached title,
file-select, Delfino flow, thousands of semantic 2D operations, tens of thousands of J3D submissions,
lights, fog, and representative material families. Claims C079–C096 record exact observed scopes.

Gap: `decomp/sms` still contains upstream divergence, unnamed fields, and reachable incomplete bodies;
some live scene evidence is blocked by issue 30's retained-GX invalid wrap state. The decomp remains
evidence and readable source rather than a second shipping runtime.

### S006 — smooth presentation

The existing interpolation work records stable identities for J3D shapes, matrices, cameras,
billboards, and reached indexed quads. Instrument I038 and C076 provide a controlled camera/sea
comparison showing a slight region-level improvement and a planted forced-snap opposite answer.

Gap: residual palm/sky motion is not joined to a stable draw identity, the graphics census is not
complete, the implementation is not integrated with the new executor/renderer, and native-rate modes
remain performance-limited in heavy Delfino intervals.

### S007 — decomp expansion

The decomp is runnable as an evidence path and contains binary-derived implementations and native
host-safety adaptations. Existing RE notes retain exact GMSE01 addresses, object layouts, formulas,
and behavior for camera, water, J2D, J3D, audio, effects, threading, and game systems.

Gap: upstream convergence debt, established-but-unnamed fields, and reachable incomplete bodies
remain. Each pass must rebase, converge matching ownership units, then extend only from evidence.

### S008 — representative gameplay conformance

Missing capability: drive a bounded, interactive gameplay scenario through the real gameplay target
with native renderer and native owners active. Compare guest PC/register state, relevant memory,
timing/interrupt/service events, audio, input, and presented frames against an independent oracle;
report JIT blocks, cache activity, invalidations, overrides, and denominators. Qualify correctness and
frame-time behavior on every released host architecture.

### S009 — retired executor removal

Evidence: the retired executor, its artifacts, selectors, tests, and launch scripts are deleted.
`tools/migration_boundary.py` scans the first-party tree and has planted positive and negative
controls that prove those surfaces cannot return. No replacement executor was fabricated.

### S010 — independent evidence infrastructure

Evidence: `extern/dolphin_fork` retains the maintained independent emulator core and observation
hooks; the Python parsers under `tools/oracle/`,
`decomp/sms`, the GMSE01 symbol/address corpus, RE notes, claims, and controlled native/GPU tests
provide independent behavior and layout evidence. The instrument ledger records trusted and
distrusted tools explicitly. This capability proves mechanisms within named scopes, not whole-game
parity.

### S011 — native audio

The decomp evidence path has an audible native JAS voice renderer. The prior guest-layout path also
proved the Zelda-class ucode contract, seven sub-frame cadence, AFC/PCM decoding, resampling, and L/R
mixing with audible music and effects. `docs/audio/` retains the binary field maps and observed
residuals.

Gap: select one title-owned native audio implementation for the native/dynarec product, connect it to
the new lifecycle and typed configuration, and verify music, effects, streaming/movie audio,
positional/aux routing, teardown, and bounded output against the oracle.

### S012 — application structure and policy

The repository has tracked Clang formatting/tidy configuration, a typed immutable launcher parser,
source-size ratchets, native-render dependency checks, and a migration-boundary scanner. The scanner
rejects retired executor paths/selectors, non-Python scripts except `run.sh`, direct product output
outside the logger owner, and environment reads outside the configuration owner.

Gap: the target gameplay composition root and cohesive RAII owners do not exist. Lucent-backed
product logging and the final typed persisted configuration owner must be implemented with gcnport.

### S013 — default launcher

Blocker: S002, the shared `gcnport` Dolphin-JIT executor exists but its complete embedding adapter is
not implemented yet.

Blocked by S002: `./run.sh` is a slim locked-Python shim and currently refuses by naming the missing
shared gcnport Dolphin-JIT executor. Once the executor exists it must validate exact `GMSE01`, keep
the JIT/native selection invariant, name missing native dependencies with platform commands, and
never run tests.

### S014 — packaged setup

Missing capability: produce asset-free desktop packages whose first launch opens a native picker,
accepts the primary image directly or one bounded nested ZIP, validates the complete exact-title
install, preserves the previous valid choice on failure, and persists data in OS user locations.

### S015 — widescreen

Existing native work owns projection, HUD placement, and several screen/effect boundaries and has
GMSE01-specific RE notes and controls. It does not rely on final-image stretching.

Gap: integrate that policy with the single dynarec/semantic-renderer product, enumerate every
horizontal culling/scissor/screen-effect boundary, and verify additional world coverage and 4:3
faithfulness through deterministic geometry-based controls.

### S016 — input, saves, and settings

Existing paths have keyboard/controller translation, native memory-card work, persisted renderer/
frame-rate/effect settings, and an in-game settings UI with layout controls.

Gap: move their surviving behavior behind one typed immutable product policy and focused RAII owners;
remove execution-engine choices, route physical and future virtual controls through one action model,
store saves/settings in OS user data, and verify them through the native/dynarec gameplay path.

### S017 — host JIT qualification

Missing capability: qualify x86_64, Apple Silicon macOS AArch64, and Android arm64-v8a separately
with the shipping JIT. Evidence must cover executable-memory publication/protection, instruction-
cache coherence, ABI transitions, exceptions/signals, packaging, representative interactive
gameplay, and fallback ratios. A fallback-heavy or zero-JIT run and one AArch64 OS cannot stand in
for another host backend.

### S018 — asset-free automation

The canonical Python verifier checks the migration boundary, source structure, live documentation,
registry paths, deterministic shader regeneration, decomp symbol tooling, Clang formatting/tidy,
the Ninja build, and the native-renderer test suite without game files. The tracked workflow invokes
that same owner on Linux x86_64, Windows x86_64, and the macOS Apple Silicon runner with pinned
actions, Python, uv, and SDL inputs. This proves only redistributable native components and tooling;
it does not claim boot or gameplay.

Shader provenance uses one host-neutral Python provisioner for Linux, Windows, and macOS. It pins
the maintained shaderc v2026.1 fork plus the exact compatible glslang, SPIRV-Tools, and SPIRV-Headers
commits, verifies every downloaded source archive against its recorded SHA-256 before bounded traversal-safe
extraction, builds with Ninja under the locked Python interpreter, and refuses any missing or stale
installed tool instead of consulting `PATH`. All 13 embedded shader headers match this exact
compiler/validator pair locally.

The shaderc dependency is `SomeoneIsWorking/shaderc` at
`50f71a748725b3df267128e519ef6c59881fc33e`, published as `v2026.1-port.1` and based on upstream
`301b4ede53d59b68bf55f95bb26412d9233c8187`. Its bounded string-to-word copy preserves NUL truncation
and zero padding while avoiding the deprecated CRT `strncpy` call rejected by the Windows
Clang/MSVC-target build under `-Werror`. The source change and its regression tests live in the
fork; the provisioner applies no patches. The fork's remote `main` tracks newer upstream code and
is deliberately not the consumed release. The versioned fix passes all 12 copy cases and all 103
compiler tests locally. macOS CI selects AppleClang explicitly; Homebrew LLVM supplies only the
formatting and lint tools.

Self-tests declare game-image and host instrumentation requirements. The asset-free gate reports
the selected, skipped, and discovered denominators, runs Linux kernel/RADV instruments only on
Linux, and omits only the explicitly declared GMSE01-image check. `tools/verify_re.py` owns that
separate check and hard-refuses a missing DOL, so hosted automation cannot turn unavailable game
evidence into a passing empty result.

Android CI is blocked and deliberately has no placeholder job: Sunbright has no Android Gradle/NDK
consumer, package identity, or `gcnport` arm64-v8a executor to build. Add the Android job only when
those real owners exist, and route it through the shared Android build contract. JIT gameplay and
performance remain missing on every host under S017 regardless of these component jobs.

Hosted run `33959423142` confirms Windows builds the pinned shaderc fork and passes the 13-shader
UTF-8 header check. Header I/O explicitly uses UTF-8, writes LF, and accepts Git's CRLF checkout
through universal-newline reading. The shipping self-test covers file round trips and altered-word
rejection, including Python UTF-8 mode disabled under the ASCII locale.

The same Windows run then exposed MSVC STL's vectorized `std::find` instantiation on the image
cache's 16-byte key. The key's default memberwise equality remains unchanged; the unused
current-frame key vector and its search are removed. `CachedImage::lastUsedFrame` remains the
single eviction-use owner. Local Clang 22.1.8 verification passes the real watched semantic GPU
suite, including repeated same-frame image resolution, resident reuse, and changed-revision
readback controls; three focused image/platform CTests and both touched translation units'
format/tidy checks also pass.

Gap: Native Windows confirmation awaits the next hosted run. Android remains
missing until its real application and executor boundaries exist.
