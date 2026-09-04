# Sunbright architecture

Sunbright is one native/dynarec product for exact `GMSE01`. Native owners replace selected title or
host behavior; Dolphin's runtime PowerPC JIT executes ordinary guest instructions directly from the
user's authenticated image. There is no offline guest-code generation step. A bounded interpreter
fallback is permitted only for blocks the JIT explicitly refuses to compile or execute safely.

## Runtime flow

```text
user GMSE01 image
        |
        v
gcnport image validation + Dolphin machine
        |
        v
guest PC -> runtime override lookup -------------------------+
        | hit                                                |
        v                                                    |
Sunbright native override -> semantic/native service owner   |
        | optional original call                             |
        +-------------------------> Dolphin JIT <-------------+
                                      |
                                      v
                         translated block cache -> dispatcher
```

The JIT owns ordinary guest execution. The native table is consulted at a complete runtime identity
and guest address before a translated path can bypass the decision. A native original call suppresses
only its current override for one call and enters the ordinary JIT dispatcher. Hook changes revoke
affected direct links.

## Dependency direction

```text
composition root
  -> typed config + Lucent logger
  -> gcnport executor -> Dolphin runtime/JIT
  -> title identity + override registry
       -> native services
       -> renderer-neutral adapters -> native-render
  -> input / audio / UI / saves
```

Dependencies point inward through narrow interfaces. Product modules do not reach through the
executor into Dolphin globals, parse process configuration, write diagnostics directly, or acquire a
second GPU/window owner.

## Owners

### gcnport

The shared GameCube framework owns the title-neutral executor around Dolphin: runtime image/module
identity, CPU/thread state transitions, guest-address hook dispatch, original calls, bounded host
exits, invalidation, and execution diagnostics. Dolphin retains its decoder, JIT backends, code
cache, memory, and device implementations. `jit-common` does not wrap a mechanism Dolphin already
owns.

### Sunbright application

The title owns exact `GMSE01` validation, the set of native overrides, application policy, native
services, semantic adapters, input actions, UI, saves, and composition. The entry point constructs
focused RAII owners with explicit dependencies and destroys them in reverse order; it does not
implement their work.

Configuration has one owner that ingests CLI, environment/`.env`, persisted files, and platform
defaults into an immutable typed value. Logging has one Lucent-backed owner. No other product module
reads the environment or writes stdout/stderr/debug APIs directly.

### Native renderer

`native-render/` owns ordinary scene values, decoded images, semantic material/pass logic, GPU
resources, targets, and presentation. Runtime adapters copy only renderer-neutral values and stable
resource identities. Guest addresses, decomp object pointers, GX/FIFO commands, BP/XF registers, TEV
programs, EFB-copy protocols, and Dolphin/Aurora renderer state stop before this boundary.

The first JIT-integrated adapter is `J3DShape::draw` at `0x802e0390`. Its existing binary/decomp
layout knowledge and PC-native mesh/material implementation are retained; only execution ownership
moves from the generated-address table to `gcnport`'s runtime hook contract.

### Native services

Audio, input, storage, settings, timing, and other selected host behavior are independent title
owners. Each override reproduces the guest ABI and calls back through the executor when original game
behavior remains required. A native service does not become a second CPU or hardware emulator.

### Decomp and oracles

`decomp/sms` is readable recovered source, a source of precise names/formulas, and an independent
native-layout adapter for controls. It is not another shipping engine. `extern/dolphin_fork` and
`tools/oracle/` provide independent emulator observations. Aurora and the project-owned GX renderer
may answer bounded compatibility questions while semantic coverage migrates; they are not product
rendering fallbacks.

## Interpreter boundary

An explicit interpreter-only mode is diagnostic. Gameplay may enter an interpreter only from a JIT
refusal carrying a typed reason and guest PC; block and instruction counters are reported with total
JIT denominators and control returns to JIT dispatch. First-pass interpretation, interpreting while
waiting for compilation, filling in a missing host backend, and unbounded fallback are forbidden.
Fallback-heavy and zero-JIT runs do not count as gameplay or performance evidence.

## Host qualification

x86_64, Apple Silicon macOS AArch64, and Android arm64-v8a are separate release targets. Each must
prove the shipping JIT's executable-memory policy, instruction-cache coherence, ABI transitions,
exceptions/signals, packaging, representative gameplay, and fallback ratios.

## Lifetime model

One application instance owns configuration and logging, then the Dolphin/gcnport executor, title
overrides/services, GPU platform, renderer clients, UI, and control endpoints. Destruction reverses
that order so clients release targets before the platform and native callbacks detach before the
executor dies. Background library threads may exist, but guest execution and title state remain
owned by the application instance and cross boundaries only through explicit synchronized APIs.

## Mechanical boundaries

The normal verifier must enforce:

- a 1,200-line first-party source cap, with oversized existing files frozen and ratcheted down;
- native-render independence from GX, Aurora, Dolphin renderer state, and title layouts;
- environment/CLI ingestion only in the configuration owner;
- stdout/stderr/platform debug output only in the logging boundary;
- no retired executor object, dispatcher, or alternate selector in gameplay;
- no untyped, uncounted, first-pass, compile-wait, missing-backend, or unbounded interpreter path;
- no offline guest-code generation/build rule or static launcher path; and
- Clang formatting, Clang-Tidy, tests, and portable paths.

Detailed migration order and evidence gates are in `docs/port/migration.md`.
