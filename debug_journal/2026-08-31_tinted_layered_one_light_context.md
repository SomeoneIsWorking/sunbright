# Tinted-layered title materials use one authored light

## Root cause

The title audit exposed 208 exact tinted-layered classification attempts that all failed only with
`MissingLightingContext`. Their J3D channels, texture orders, two stage programs, colour registers,
and raster policy matched the existing semantic material exactly. The shared classifier required
`pointLightCount >= 2`, but the live stage-light publisher validly supplied only the primary light
for these title draws; the second light is optional stage state, not part of this material equation.

## Change

`j3d_tinted_layered_material` now accepts one or two valid point lights and preserves the published
count. `model.cpp` validates the same one-or-two-light contract, so no adapter fabricates an effect
light and the existing two-light Delfino path remains unchanged. The focused classifier test covers
both counts and rejects an empty context.

## Evidence

After the change, a guarded 120-present title audit exited 0 and advanced all 52 one-light instances
through classification, two-image decode, perspective readiness, and native submission. Coverage
rose from 1,144 to 1,352 models and lit models from 1,040 to 1,248. No GPU fault or reset was
reported. The audit was run with:

`SB_HEADLESS=1 SB_WATCHDOG_SECS=120 SB_SEMANTIC_FRAME_MODE=audit SB_LOG=semantic ./run.sh --diagnostic --stage 15 --quit-after 120`
