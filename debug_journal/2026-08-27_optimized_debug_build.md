# Optimized Debug is the playable build

## Symptom

Both launchers forced CMake `Release`. That made the game playable, but also selected `NDEBUG` and
therefore removed assertions and GX debug groups while Aurora added Dawn's `skip_validation` and
`disable_robustness` toggles. Stock CMake `Debug` kept diagnostics but compiled the generated guest
and renderer at `-O0`.

## Root cause

Optimization and diagnostics were encoded as one build-mode choice even though they are independent
requirements. Two masked defects appeared as soon as that coupling was removed:

1. The recomp host value-initialized `AuroraConfig` but never assigned `logLevel`. The enum's zero
   value is `LOG_DEBUG`, so Debug compiled three per-command GX diagnostics and then printed them all.
   Release only appeared correct because `NDEBUG` removed those calls.
2. `AURORA_GFX_DEBUG_GROUPS` stored a complete `vector<string>` on every recorded renderer command
   and copied it again while encoding every command. A plaza tick contains tens of thousands of
   commands; diagnostic labels cannot make each one own heap strings.

## Fix

- `cmake/SunbrightBuildPolicy.cmake` makes Debug `-O2` while retaining assertions and compiler debug
  information. Generated guest code uses line tables rather than full local-variable records, so
  guest function names/backtraces remain available without pathological compiler memory use.
- Both launchers configure Debug on every launch. Release is still available for explicit packaging.
- `AURORA_GPU_DIAGNOSTICS=off|standard|full` now owns Dawn validation/robustness and GX labels
  independently of `NDEBUG`. Standard requests partial backend validation and keeps WebGPU API
  validation and robustness. The pinned prebuilt Dawn reports that Vulkan validation layers were
  not built in, so the request is visible but cannot supply those backend-layer messages.
- `host/aurora_config.cpp` owns the tested recomp renderer config and sets `LOG_INFO` explicitly.
- Aurora interns the debug-group stack once per stack revision in each pass. Commands retain a
  32-bit snapshot ID, and the encoder only changes its Dawn group stack when that ID changes.

## Controls and runtime evidence

- I035 reads CMake's emitted compile commands. It accepts the actual optimized Debug profile and
  rejects four synthetic controls: missing guest optimization, missing GPU diagnostics, `NDEBUG`
  in Debug, and a missing representative translation unit. A separate Release configure retained
  the diagnostics definitions.
- `aurora_config_test` fails for the old value-initialized `LOG_DEBUG` policy.
- `DebugGroupSnapshots` feeds 100,000 commands through one unchanged stack revision and proves that
  only one string snapshot is retained; stack changes and the empty sentinel are separate controls.
- The first bounded Debug run exposed the missing log level and aborted on Aurora's five-second
  render-worker timeout. After setting `LOG_INFO`, the same 120-present stage-1 run exited 0 with
  successful external GPU monitoring.
- Matched 240-present optimized-Debug and Release runs both exited 0 with the same standard GPU
  diagnostics. Their steady intervals were similarly slow while several unrelated Clang builds
  occupied the host. A follow-up Release control retained API/backend validation and robustness but
  compiled out debug groups; it did not improve the interval. This falsifies build mode and label
  transport as the cause of that sample. Per C016/C058, contended wall-clock timing is not admissible
  bottleneck attribution; absolute renderer throughput remains issue #9.
- Aurora's full serial test suite exposed two stale controls left behind by earlier hardware fixes.
  `GXSetViewportJitter` still encoded the obsolete 340 origin while the retail decomp and FIFO
  decoder use 342, so the SDK encoder was corrected. Specular attenuation necessarily encodes
  `diffFn=GX_DF_NONE`, so that test now asserts the representable hardware state. All 229 Aurora
  tests and all 20 recomp tests pass.
