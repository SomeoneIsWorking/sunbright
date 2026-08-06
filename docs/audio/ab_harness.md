# Live audio A/B harness — side-by-side oracle/native divergence hunter

Goal: replace per-bug whack-a-mole with a harness that runs the **native** build and the
**oracle** (`SUNBRIGHT_DISABLE_RECOMP=1`) side by side, drives both with identical inputs at
identical *game-progress* points, aligns their **audio event streams** intelligently (frame
timings never match), **stops at the first real divergence**, and reports the cause with
enough context to fix it.

## Why events, not PCM or polling
- PCM diffing can say *that* audio differs, never *why*. Event streams carry the cause
  (which wave, which key/velocity, which BMS track, what pitch/gain).
- Polling (`/vpb`, `/njas` at 10 Hz) misses short notes and samples envelopes at random
  phases — it produced two triage rounds of measurement artifacts (see CLAUDE.md harness
  gotchas). Events are emitted at the moment of truth (voice start/end), missing nothing.

## Event sources (both write the same JSONL schema)
- **Oracle**: `runtime/vpb_trace.cpp` (`--wrap` on `ZeldaAudioRenderer::FetchVPB`, the
  per-voice per-frame HLE entry). `SUNBRIGHT_AB_EVENTS=<path>`: on a voice's `enabled`
  rising edge emit `von` {t_ms, voice, ratio (4.12), hash}; on falling edge / `done` emit
  `voff` {dur_frames, peak_vol}. `hash` = FNV-1a of the first 64 ARAM bytes at the VPB base
  address — the **join key**, identical bytes to the native side's `Wave.srcHash` (first 64
  raw `.aw` bytes).
- **Native**: `runtime/native_jas.cpp` same env var: `von` {t_ms, hash, ratio, bank, prog,
  key, vel, seq_off (BMS offset of the noteon), id (owning SE/BGM sound id)}; `voff`
  {dur_subframes, peak_vol}; plus **anchor events**: `bgm` {id, name} and `se` {id} starts
  and stops. The native extras are what turns "voice differs" into a *cause* (which BMS
  instruction, which sound id, which instrument).

## Alignment (the "intelligent" part)
- **Progress-based driving, not wall-clock**: the input script is a list of steps
  `{trigger, action}`; a trigger is an event predicate (e.g. `bgm id=80010001` = entered
  Delfino). Each side advances through the script *at its own pace* — both sides perform the
  same actions at the same game state, so streams stay comparable despite boot/scene drift.
- **Per-hash matching with a drift offset**: events are matched per wave-hash in order;
  the harness maintains a global oracle↔native time offset (EMA over recent matches,
  anchored on `bgm` events) and matches an oracle event to a native event within
  ±`window` (default 4 s) of the predicted time.
- **Calibration**: volume units differ per side; the harness normalizes per-population
  (median of matched peak_vol ratios) before flagging gain divergences.

## Divergence classes (stop on first, unless --no-stop)
- `MISSING_NATIVE` — oracle voice with no native match in window (unported path).
- `EXTRA_NATIVE` — native voice the oracle never plays (overfiring/leak).
- `PITCH` — matched but ratio differs > 50 cents.
- `GAIN` — matched but normalized peak gain differs > 8 dB.
- `LIFE` — matched but voice lifetime ratio > 3× (lifecycle/envelope bugs).

## On divergence
SIGSTOP both instances (state stays inspectable via each probe port), then report:
the diverging event with its native context (bank:prog key seq_off id), the last N matched
events (what was still in sync), both sides' live voice tables (`/njas`, `/vpb`), and the
suspected class. Residuals live in `tools/audio/ab_residuals.json` — every entry MUST
carry a `why` (benign reason); the harness skips them.

## Running
```
tools/audio/ab_harness.py [--secs 240] [--script tools/audio/ab_script_delfino.json]
                          [--no-stop] [--sequential]
```
Both instances run simultaneously (native probe :17654, oracle :17655; distinct event
files under scratch/logs/). `--sequential` falls back to one-at-a-time capture+offline
compare if simultaneous proves unstable on a machine.

## Clocks (IMPORTANT — wall time is banned here)
- **Oracle events stamp EMULATED time** (CoreTiming ticks → ms, `vpb_trace.cpp
  ab_now_ms`). The oracle instance's wall clock is meaningless: one stale persisted
  `EmulationSpeed=0` (now fixed in main_sdl.cpp, set explicitly every run) had it
  silently turboing at a fluctuating 2–10x, which manufactured the false "SE tree runs
  12x slower natively" finding (see docs/re_notes/audio_re_findings.md dead-ends).
- **Native events stamp the engine's own audio clock** (subframes rendered x 2.4977 ms)
  — immune to the unpaced boot phase.
- **Durations are in JAS subframes on BOTH sides** (oracle: emu-ms/2.4977), so LIFE
  compares are unit-identical.
- The two clocks have a constant-ish offset (boot length difference); the harness EMA +
  bgm anchors absorb it. Wall-timed driving (autostart pulses, script waits) stretches
  segments differently per side — expect offset steps at scene changes, not drift.

## Known limits / residual classes
- RNG-driven SEs (rand effects, random pitch wobble) differ per-run by design — pitch
  threshold absorbs ±1 semitone of `IE_RAND`; bigger wobbles belong in residuals.
- Scene-ambient voices depend on camera/Mario position; identical scripts keep them
  comparable but not bit-identical — GAIN threshold is deliberately loose (8 dB).
- Streams (0xC… ids) are guest-side on both paths and excluded.
