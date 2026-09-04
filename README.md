# Sunbright

Sunbright is a work-in-progress native/dynarec PC port of the NTSC-U (`GMSE01`) release of
*Super Mario Sunshine*. The intended product executes maintained native overrides directly and uses
Dolphin's runtime PowerPC JIT, integrated through the shared `gcnport` framework, for every remaining
guest path. Rendering is moving to a PC-native, game-semantic renderer above GX.

## Status

The gameplay product is not runnable yet. The retired executor and its selectors have been removed.
`./run.sh` deliberately refuses by naming the
incomplete shared `gcnport` Dolphin-JIT adapter instead of launching an evidence host or stale binary.

The first implementation checkpoint is:

- authenticate and boot exact `GMSE01` through Dolphin's JIT;
- execute nonzero dynamically translated blocks;
- intercept `J3DShape::draw` at `0x802e0390` through the runtime dispatcher;
- submit the already-established semantic J3D operation;
- execute the original guest body through the JIT; and
- prove ordinary cold blocks compile before execution and report bounded fallback counters with
  denominators; and
- prove the gameplay binary contains no retired executor artifacts or alternate selector.

That checkpoint proves wiring only. Gameplay and performance require representative interactive
evidence on x86_64, Apple Silicon macOS AArch64, and Android arm64-v8a independently.

## Intended features

Sunbright's complete feature/status inventory is [`docs/project-state.md`](docs/project-state.md).
The main intended differences from the original GameCube release are:

- one native/dynarec gameplay executable using the player's original game image;
- PC-native semantic rendering for J3D, J2D, particles, lights, cameras, and effects;
- widescreen rendering that exposes additional world coverage without final-image stretching;
- smooth presentation between original simulation ticks, plus separately qualified native-rate modes;
- native audio, input, saves, configuration, and in-game settings; and
- desktop packages that contain no game files and provide a no-terminal first-run picker.

Current semantic-renderer work already has verified renderer-neutral 2D/UI, image decoding, multiple
J3D mesh/material families, lights, fog, particles, and GPU controls. Those results are preserved as
implementation evidence, but they have not yet been integrated into the new gameplay executor.

## Development entry points

- [`docs/project-state.md`](docs/project-state.md) — current verified, partial, and missing outcomes.
- [`docs/project-goals.md`](docs/project-goals.md) — durable definition of success.
- [`docs/architecture.md`](docs/architecture.md) — execution and ownership boundaries.
- [`docs/codemap.md`](docs/codemap.md) — where responsibilities live or should move.
- [`docs/port/migration.md`](docs/port/migration.md) — ordered migration and acceptance gates.

`./run.sh` is the stable product command. Until `gcnport` exists it exits with an explicit missing-
executor error; it does not expose an alternate runtime selector.

The redistributable native-component gate is `uv run --frozen python tools/verify.py`. It runs the
portable self-tests on every supported desktop CI host and the Linux kernel/RADV diagnostics only
on Linux. Game-image-dependent RE instrument checks are deliberately separate:
`uv run --frozen python tools/verify_re.py` requires the user-supplied GMSE01 DOL and refuses rather
than reporting an empty result when it is absent.

## Game files and licensing

No game image or reconstructable copyrighted game data belongs in this repository or a release.
The finished product will accept a user-supplied exact `GMSE01` image, validate it before execution,
and store only player configuration, saves, and disposable JIT caches in OS user-data locations.

Sunbright is not affiliated with or endorsed by Nintendo.
