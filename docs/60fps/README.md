# 60 Hz presentation and simulation

Sunbright's shipped target is faithful 30 Hz game simulation with a native 60 Hz presentation
option. This feature is currently **missing** because gameplay itself is blocked on the shared
`gcnport` Dolphin-JIT executor. The retained material here is binary and visual evidence for the
future native implementation, not a runnable implementation.

## Required behavior

- Faithful mode preserves the title's authored simulation cadence.
- Native-rate mode may raise game-owned timing only after each dependent subsystem has an explicit
  semantic owner and a differential control.
- Interpolated presentation renders real in-between geometry. Repeating or blending final images is
  not interpolation.
- A presentation miss does not advance simulation, skip lifecycle work, or fabricate state.
- Camera cuts, teleports, births, topology changes, and stale identities snap deliberately.
- Screen-feedback effects advance according to their authored history rather than accumulating
  twice per game tick.
- Widescreen and interpolation remain independent features.

## Retained binary anchors

| Address | Owner | Required use |
|---|---|---|
| `0x802e0390` | `J3DShape::draw` | Runtime semantic-render hook and stable model identity boundary |
| `0x800335d4` | `CPolarSubCamera::warpPosAndAt(Vec&, Vec&)` | Camera-discontinuity signal |
| `0x80033390` | `CPolarSubCamera::warpPosAndAt(f32, s16)` | Camera-discontinuity signal |
| `0x8019f83c` | `TShimmer::perform` | Heat-haze/screen-feedback identity |
| `0x8027c12c` | `TModelWaterManager::drawRefracAndSpec` | Water-refraction identity |
| `0x8022d4f8` | `TAfterEffect::perform` | Dash-blur history owner |

The addresses are exact for `GMSE01`. Runtime hooks must be scoped by the complete title identity,
must reproduce the guest ABI, and must be installed through `gcnport` rather than patched host-code
addresses.

## Geometry ownership

Rigid J3D models use stable draw identity plus previous/current pose. Deforming geometry needs the
source vertex stream itself; matrix interpolation cannot move positions already baked into direct
vertices. The retained evidence distinguishes these populations:

- J3D indexed models: interpolate model pose and normal transforms.
- JPA billboards: preserve particle identity and world-position history.
- flags, ropes, wave grids, and other deforming meshes: pair compatible vertex topology and
  interpolate decoded source attributes.
- J2D, glyphs, HUD, and fades: remain authored 2D unless a named native policy owns their motion.

Detailed population observations remain in `docs/graphics/graphics_db.tsv`. The current
renderer-neutral model and shader contracts live in `native-render/`; they do not ingest guest
pointers, GX state, or emulator renderer objects.

## Measurement rules

A 60 Hz claim needs all of the following:

1. Nonzero counts with denominators for every admitted, snapped, missing-history, stale, and
   topology-mismatch class.
2. A forced opposite control that visibly or numerically changes the produced frame.
3. Consecutive-present measurements from one deterministic run; indices from different cadences do
   not identify the same guest moment.
4. Real gameplay reachability through the shipping JIT path. Diagnostic-only and fallback-heavy
   runs do not qualify the feature.
5. Frame-time percentiles and sustained behavior on x86_64, Apple Silicon macOS AArch64, and Android
   arm64-v8a independently.

Useful retained analysis tools are `tools/interp/cadence.py`, `tools/interp/frame_regions.py`, and
`tools/interp/subframe_position.py`. They analyze captured images; they do not implement gameplay or
presentation.

## Related evidence

- `docs/60fps/immediate_mode.md` — direct-vertex and particle distinctions.
- `docs/60fps/effects.md` — feedback and projected-texture constraints.
- `docs/60fps/screen_effects.md` — exact screen-effect catalog.
- `docs/60fps/widescreen_effects.md` — widescreen-specific presentation ownership.
- `docs/re_notes/water_refraction_projection.md` — water projection evidence.

Implementation begins only after `gcnport` boots exact `GMSE01` through Dolphin's JIT and the
renderer-neutral `J3DShape::draw` hook is proven in ordinary gameplay.
