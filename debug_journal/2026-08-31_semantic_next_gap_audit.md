# Semantic renderer next-gap audit — 2026-08-31

The recomp semantic audit was extended to print both texture bindings, all three leading TEV
stage signatures, both colour-channel pairs, and the per-program progress counters. This closes a
real attribution gap: a two-texture hand program had previously looked like the already-covered
single-channel layered family because the report exposed only one texture and one stage.

The short title audit found the highest-frequency unresolved hand program using two colour
channels (`0686/0706` and `0212/0400`), two textures, and two stages:

* stage 0: `c009fae8 c108ffd0`, texture coordinate/map 1/1, colour channel 0;
* stage 1: `c208a08f c308f070` (or the alpha-preserving `c208a08f c308ff80` variant), texture
  coordinate/map 0/0, colour channel 1.

The program was observed 12 times in the title run but had zero perspective observations,
resource-ready events, or semantic submissions. It is therefore a setup/orthographic candidate,
not evidence of a visible perspective gap. The stage-one audit reached the same boundary: its
largest unresolved two-channel hand family had 300 observations and zero perspective entries.
The existing perspective two-texture layered and tinted-layered families remained accepted and
submitted.

Both watched runs exited with status 0 and the GPU watcher reported no kernel GPU fault or reset.
The Vulkan object-tracking warning at shutdown is a host cleanup warning, not a GPU death/reset.
