# Four-image Mario masked-toon material reaches the PC-native renderer

## Finding

The perspective-reached five-stage Mario material uses four distinct authored images: main surface,
hand mask, alternate rack surface, and toon ramp. Its PC-native representation is not a colour-stage
interpreter: the mask chooses the main or rack image at the authored 8-bit threshold, the toon ramp
and signed diffuse lighting form a weighted layer, and static plus directional highlights are added.
Fog, alpha, cull, depth, and blend remain ordinary renderer policy.

## Root cause found during admission

The initial classifier fixture encoded the first stage as consuming colour channel 4. The live stage
order is `01/01/ff`: it consumes no colour channel. That mismatch caused every real instance to be
rejected as an unsupported texture binding despite matching the recorded image bindings and program.
The classifier and its control fixture now require `ff` there. A separate mistaken requirement for a
second point light was also removed: the material's second *colour channel* is directional specular,
not a mandatory second point light.

## Evidence

The guarded recomp diagnostic run on 2026-08-31 observed 200 calls of the exact four-image program.
All 200 classified and decoded; 150 reached a perspective scene and submitted through the PC-native
shader. The other 50 had no perspective context and were deliberately withheld. The live GPU watcher
reported no fault and the guarded process exited zero.
