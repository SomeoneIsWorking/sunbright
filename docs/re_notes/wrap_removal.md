# Removing the linker `--wrap` interception (fork-direct hooks)

Date: 2026-06-13

## What changed
The Sunbright runtime used to intercept Dolphin functions with linker `--wrap` flags
(`-Wl,--wrap=<mangled symbol>` in `CMakeLists.txt` → `__wrap_<sym>` / `__real_<sym>` in the
runtime). That mechanism is gone for everything except `JitTrampoline`. Each intercepted Dolphin
function is now a **thin shim in the fork** that consults a function-pointer slot:

```cpp
void Foo(args) {                       // fork shim
  if (sb_slot_foo) { sb_slot_foo(this, args); return; }   // hook installed
  Foo_impl(args);                       // else: original body
}
```

- Slots live in `externals/dolphin/Source/Core/Common/SunbrightHooks.h` (`sb_slot_*`,
  **default null = original behavior**, so the fork builds/works standalone and the offline tools
  link).
- The original body is preserved verbatim, exposed as either a member `Foo_impl` (when it touches
  private members) or a free `extern "C" sb_<area>_impl(void* self, ...)` wrapper.
- The runtime's hook bodies keep their original files but are renamed `__wrap_X` → `sb_hook_X`
  (plain `extern "C"`), and any former `__real_X(...)` call becomes the matching `*_impl(...)`.
- `runtime/sunbright_hooks.cpp` (`sb_install_hooks()`, called once at the top of `main()` in
  `runtime/main_sdl.cpp`) points each `sb_slot_*` at its `sb_hook_*`. Names are deliberately
  distinct (`sb_slot_` pointer vs `sb_hook_` function) to avoid a symbol collision.

Behavior is byte-identical: an installed hook runs exactly the old `__wrap_` body, and `*_impl`
runs exactly the old `__real_` (Dolphin original) body.

## The one exception — JitTrampoline (still `--wrap`)
`JitTrampoline` is defined in `Source/Core/Core/PowerPC/JitCommon/JitBase.cpp` and called only
from `Source/Core/Core/PowerPC/Jit64/JitAsm.cpp` and `.../JitArm64/JitAsm.cpp`. All of that is
inside `Source/Core/Core/PowerPC/`, which the project forbids modifying. There is no non-PowerPC
seam to insert a fork hook, so it keeps the single surviving
`-Wl,--wrap=_Z13JitTrampolineR7JitBasej` (`runtime/jit_hook.cpp`).

## Per-symbol conversion table
| Dolphin symbol (member) | Fork file edited | Shim → impl | Runtime hook (`sb_hook_*`) | Slot (`sb_slot_*`) |
|---|---|---|---|---|
| `PixelEngineManager::SetToken` | `VideoCommon/PixelEngine.cpp` (+`.h`: `SetToken_Impl`) | `SetToken` → `SetToken_Impl` (via free `sb_pe_set_token_impl`) | `runtime/pe_token_wrap.cpp` `sb_hook_pe_set_token` | `sb_slot_pe_set_token` |
| `GPFifoManager::Write8/16/32/64` | `Core/HW/GPFifo.cpp` | `WriteN` → free `sb_gpfifo_writeN_impl` (FastWriteN+CheckGatherPipe) | `runtime/gpfifo_wrap.cpp` `sb_hook_gpfifo_writeN` | `sb_slot_gpfifo_writeN` |
| `DSPManager::GenerateDSPInterrupt` | `Core/HW/DSP.cpp` (+`.h`: `_Impl`) | `GenerateDSPInterrupt` → `GenerateDSPInterrupt_Impl` (free `sb_dsp_gen_interrupt_impl`) | `runtime/overrides/aid_native.cpp` `sb_hook_dsp_gen_interrupt` | `sb_slot_dsp_gen_interrupt` |
| `DSPManager::UpdateAudioDMA` | `Core/HW/DSP.cpp` (+`.h`: `_Impl`) | `UpdateAudioDMA` → `UpdateAudioDMA_Impl` (free `sb_dsp_update_audio_dma_impl`) | `aid_native.cpp` `sb_hook_dsp_update_audio_dma` | `sb_slot_dsp_update_audio_dma` |
| `DSPManager::GenerateDSPInterruptFromDSPEmu` | `Core/HW/DSP.cpp` (+`.h`: `_Impl`) | `…FromDSPEmu` → `…FromDSPEmu_Impl` (free `sb_dsp_gen_interrupt_from_emu_impl`) | `aid_native.cpp` `sb_hook_dsp_gen_interrupt_from_emu` | `sb_slot_dsp_gen_interrupt_from_emu` |
| `CoreTimingManager::ScheduleEvent` | `Core/CoreTiming.cpp` (+`.h`: `_Impl`) | `ScheduleEvent` → `ScheduleEvent_Impl` (free `sb_ct_schedule_event_impl`) | `runtime/coretiming_trace.cpp` `sb_hook_ct_schedule_event` | `sb_slot_ct_schedule_event` |
| `CoreTimingManager::RemoveEvent` | `Core/CoreTiming.cpp` (+`.h`: `_Impl`) | `RemoveEvent` → `RemoveEvent_Impl` (free `sb_ct_remove_event_impl`) | `coretiming_trace.cpp` `sb_hook_ct_remove_event` | `sb_slot_ct_remove_event` |
| `Mixer::PushSamples` | `AudioCommon/Mixer.cpp` (+`.h`: `_Impl`) | `PushSamples` → `PushSamples_Impl` (free `sb_mixer_push_samples_impl`) | `runtime/mixer_trace.cpp` `sb_hook_mixer_push_samples` | `sb_slot_mixer_push_samples` |
| `Mixer::Mix` | `AudioCommon/Mixer.cpp` (+`.h`: `_Impl`) | `Mix` → `Mix_Impl` (free `sb_mixer_mix_impl`) | `mixer_trace.cpp` `sb_hook_mixer_mix` | `sb_slot_mixer_mix` |
| `Mixer::PushStreamingSamples` | `AudioCommon/Mixer.cpp` (+`.h`: `_Impl`) | `…StreamingSamples` → `…_Impl` (free `sb_mixer_push_streaming_impl`) | `mixer_trace.cpp` `sb_hook_mixer_push_streaming` | `sb_slot_mixer_push_streaming` |
| `Mixer::SetDMAInputSampleRateDivisor` | `AudioCommon/Mixer.cpp` (+`.h`: `_Impl`) | `…Divisor` → `…_Impl` (free `sb_mixer_set_dma_divisor_impl`) | `mixer_trace.cpp` `sb_hook_mixer_set_dma_divisor` | `sb_slot_mixer_set_dma_divisor` |
| `Mixer::SetStreamInputSampleRateDivisor` | `AudioCommon/Mixer.cpp` (+`.h`: `_Impl`) | `…Divisor` → `…_Impl` (free `sb_mixer_set_stream_divisor_impl`) | `mixer_trace.cpp` `sb_hook_mixer_set_stream_divisor` | `sb_slot_mixer_set_stream_divisor` |
| `Mixer::SetStreamingVolume` | `AudioCommon/Mixer.cpp` (+`.h`: `_Impl`) | `…Volume` → `…_Impl` (free `sb_mixer_set_streaming_volume_impl`) | `mixer_trace.cpp` `sb_hook_mixer_set_streaming_volume` | `sb_slot_mixer_set_streaming_volume` |
| `DSP::HLE::UCodeFactory` | `Core/HW/DSPHLE/UCodes/UCodes.cpp` | factory checks `sb_slot_ucode_factory` first; non-null return → wrap in `unique_ptr`; null → original switch | `runtime/overrides/zelda_ucode_native.cpp` `sb_hook_ucode_factory` (returns `void*`/null) | `sb_slot_ucode_factory` |
| `ZeldaAudioRenderer::FetchVPB` | `Core/HW/DSPHLE/UCodes/Zelda.cpp` | body runs, then calls `sb_slot_zelda_fetch_vpb` at the END (tracer reads only) | `runtime/vpb_trace.cpp` `sb_hook_zelda_fetch_vpb` | `sb_slot_zelda_fetch_vpb` |
| `JitTrampoline` | **(none — keeps `--wrap`)** | — | `runtime/jit_hook.cpp` `__wrap__Z13JitTrampolineR7JitBasej` | — |

## Install point
`runtime/sunbright_hooks.cpp` :: `sb_install_hooks()`, called at the top of `main()`
(`runtime/main_sdl.cpp`, right after `sunbright_load_dotenv()`). Idempotent.

## Notes / fidelity details
- **`UCodeFactory`**: the hook returns a raw `UCodeInterface*` (or null = fall through). The fork
  wraps a non-null return into the caller's `unique_ptr`. This avoids returning `unique_ptr`
  across the `extern "C"` boundary. Oracle runs (`SUNBRIGHT_DISABLE_RECOMP`) return null → pure
  Dolphin, unchanged.
- **`FetchVPB`**: under `--wrap` the tracer ran `__real_` then read `vpb`. Now the fork runs the
  full body and calls the hook at the end (post-fill); the hook only reads. Same observable order.
- **`aid_native.cpp`**: besides the hooks, it delivers captured interrupts by calling the raw
  Dolphin body directly — those direct calls now go through `sb_dsp_gen_interrupt_impl` (the old
  `__real_` target), so they still bypass the AID-strip logic exactly as before.
- **`Mixer::Mix`** is also called internally by `MixSurround`; under `--wrap` that internal call
  was redirected to `__wrap_` too. The shim preserves that (internal `Mix` call → hook), so the
  flow-meter/sink path is unchanged.
- **Offline tools**: `sunbright-recomp` links `core` (where the DSP/GPFifo/CoreTiming/UCode/Zelda
  slots are defined, default-null) and never installs hooks → original behavior. `sunbright-jingle`
  links only `discio` and references none of these → no unresolved symbols.

## Build wiring
- `CMakeLists.txt`: `runtime/sunbright_hooks.cpp` added to the `sunbright` target; all `--wrap`
  flags removed except `_Z13JitTrampolineR7JitBasej`.
- New fork header: `externals/dolphin/Source/Core/Common/SunbrightHooks.h` (committed in the
  submodule → push the fork → bump the parent gitlink).

## Verification done
`g++ -fsyntax-only` (real build flags from `build-ws/compile_commands.json`) on every edited fork
TU (PixelEngine, GPFifo, DSP, CoreTiming, Mixer, UCodes, Zelda) and every runtime TU
(pe_token_wrap, gpfifo_wrap, coretiming_trace, mixer_trace, vpb_trace, aid_native,
zelda_ucode_native, sunbright_hooks, main_sdl, jit_hook) — all clean. Full build not run (slow
fork rebuild; user to verify).
