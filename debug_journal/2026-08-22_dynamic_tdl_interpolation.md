# Dynamic TDL indexed-quad interpolation

## Symptom

Interpolated 60 FPS still jittered on question markers, splash droplets and FLUDD water refraction.
`TDLTexQuad` and `TDLColorTexQuad` rebuild big-endian XYZ-f32 position arrays under an identity
matrix, so ordinary matrix interpolation cannot move them.

## Root cause

The first indexed-array seam paired an entire TDL batch by its object address. That assumption is
false: particle births and deaths change batch membership and array length while surviving members
remain the same moving objects. The live analog-R control exposed both failures. It first proved one
batch is deliberately drawn in multiple render passes in a tick, then showed whole-array size
mismatches for dynamic membership.

The first metadata implementation also used Aurora's interpolation tick to delimit game-side keys.
That clock is not the TDL lifecycle, so no key list reached the renderer. Retail already provides the
authoritative boundary: `TDLTexQuad::reset`, followed by requests, followed by one or more draws.

## Fix

- `TQuestionManager::request` records the requesting actor from the seven statically enumerated US
  callsites; the later `makeDL` loop transfers that owner to each question quad.
- `TSplashManager::makeDL` supplies its stable retail splash slot.
- `TModelWaterManager::calcDrawVtx` supplies a per-particle sidecar ID. The sidecar is compacted in
  the `garbageCollect` override using the retail `lifetime > 0` rule, and the survivor count is
  asserted against the retail result.
- The recomp FIFO sends one u64 key per four positions. Aurora retains history per `(batch,key)`,
  interpolates surviving groups independently, and leaves newborn groups at the current pose.
- Same-tick repeated passes alias only when bytes, layout, population and key order are identical;
  any changed pose under one tag is fatal.

## Controls and live evidence

The synthetic control requires all of the following: big-endian 0→20 motion produces 5/10/15 at
alpha .25/.5/.75; identical same-tick passes reuse a sample; changed same-tick bytes are refused;
and `[A,B] → [B,C]` pairs `B` by identity while leaving newborn `C` current. A positional or
whole-array implementation fails the last control.

The live control was:

```text
./run-safe.sh SBR_STAGE=1 SBR_LERP60=1 SBR_QUIT_AFTER=800 \
  SBR_PAD_SCRIPT=60:RTRIGGER=255+CSTICK=100/0
```

At the 400-tick report: 50 keyed arrays, 706 quad groups, 583 consecutive survivors, 123 births,
zero stale groups, zero layout mismatches and zero unkeyed arrays. At exit: 472 interpolated arrays,
four first sightings, one correct reappearance after an absence, and zero camera-only TDL draws.
The process exited 0 and the kernel reported no amdgpu timeout, reset or fault.

## Falsifier

Reopen this finding if a live TDL run reports any unkeyed array, any layout mismatch, a continuously
visible TDL draw as camera-only, or if the membership-change control stops following `B` by key.
