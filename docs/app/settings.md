# Application settings contract

This document defines the settings boundary for the native/dynarec product. Current capability state
is S012, S013, and S016 in `docs/project-state.md`.

## Ownership

One configuration owner ingests command-line arguments, environment/`.env`, persisted settings, and
platform defaults. It validates them once and produces an immutable typed application configuration.
The composition root passes only the relevant typed values to each subsystem.

Product modules never call `getenv`, parse command-line strings, read the persisted file, or query UI
elements. The settings UI edits a typed draft through an application-policy interface; it does not
own renderer, executor, frame-rate, audio, input, or persistence semantics.

The UI is composed from focused document, window, navigation, and settings components according to
Sunbright's own lifecycle and behavior. No other project's directory, component names, or visual
layout are an architectural template. Existing third-party fonts or style assets retain their
license notices independently of code structure.

## Product settings

The final player-facing policy includes:

- presentation mode: original cadence, interpolated display cadence, or a separately qualified
  native simulation rate;
- display/window mode, resolution, fullscreen, and widescreen policy;
- audio device/volume and supported mix options;
- physical controller/keyboard bindings and future virtual-control policy;
- effect choices such as heat haze; and
- game-image selection/reset plus save/configuration locations.

The gameplay execution engine and semantic renderer are not settings. The product always uses
`gcnport`/Dolphin JIT for non-native code and the native semantic renderer. Diagnostic oracle or
interpreter targets are separate builds and never persisted as player choices.

## Persistence and precedence

Player configuration and saves live in the operating system's per-application user-data locations,
never the checkout, working directory, AppImage mount, build tree, or scratch directory. The
persisted format is versioned and fail-fast: unknown versions, malformed fields, invalid enums, and
out-of-range values are reported at the boundary without partially applying a configuration.

Developer precedence is explicit argument, environment/`.env`, persisted setting, then platform
default. Environment overrides do not rewrite persisted values. Game-image validation happens before
composition commits a new selection; a failed direct-file or nested-ZIP selection preserves the
previous valid install.

## Lifecycle

The configuration owner exists before the executor, renderer, audio, input, or UI. The UI may apply
settings whose owners support a safe runtime transition; settings requiring resource recreation are
saved and clearly marked for restart. Closing the UI returns control through the application event
owner rather than running a second modal game loop.

Existing typed persistence and UI layout controls are implementation evidence, not authority to copy
the old host wholesale. Migration keeps the validated policy and tests that still apply, moves them
behind the target interfaces, and removes execution-engine choices and duplicated parsing.
