# Native TEV swap state finally reaches the shipping shader (2026-08-27)

## Root cause

The FIFO frontend had already parsed the complete GX TEV swap state: each stage's RAS/TEX row
selector and all four component-remap rows. `tev_eval.cpp` also applied it correctly, with
SDK-derived identity, AAAA, and RGAA controls in `tev_eval_test.cpp`. The shipping SDL3-GPU path
ended one boundary earlier: `SbrNativeTevUniform` carried neither the selectors nor the rows, so
`geom.frag.glsl` fed unswizzled texture and raster colours into every TEV argument.

Aurora independently applies the same two selected rows before TEXC/TEXA/RASC/RASA evaluation in
`extern/aurora/lib/gx/shader.cpp`. The decomp SDK's `GXSetTevSwapMode` and
`GXSetTevSwapModeTable` register packing is the source of the already-tested parser fields. This
is therefore a missing shipping handoff, not a guessed colour correction.

## Fix and close controls

`SbrNativeTevUniform` now appends four std140 `ivec4` rows. The per-stage packed word transports
`swapRas` and `swapTex` in unused bits above the existing map/coordinate/RAS selectors. The
fragment shader remaps the sampled texel and selected raster channel before either the colour or
alpha combiner reads them. The embedded SPIR-V was regenerated from that GLSL.

The focused packer test was red first: it failed to compile because the shipping uniform exposed
no swap table at all. It then proved that non-identity selectors and a reversed row survive the
shipping packer. The existing independent `tev_eval` test supplies the semantic controls: an
identity row preserves input, AAAA turns a black-RGB/white-alpha raster channel into white, and
RGAA routes texture alpha into B/A. The modified GLSL compiles with `glslc` and the binary passes
`spirv-val --target-env vulkan1.0`; the full recomp target links with the new uniform ABI.

## What this does not prove

No GPU/game run was performed in this lane while GPU work was serialized for the reset
investigation. More importantly, the old `SBR_AB` score is not a valid before/after gate: Aurora's
asynchronous readback can arrive one or two presents after capture while `render_compare.cpp`
retains only the latest native frame. Its self-test bypasses that join. The round-robin ablation
sweep also samples operations on different game frames and does not suppress its table when the
no-op control fails. C040/C041 were therefore falsified and I003/I008 distrusted. Native/Aurora
visual measurement resumes only after a shared capture-time frame identity reaches both sides and
unknown, missing, duplicate, or mismatched identities fail closed.

## Exact-frame harness repair

Aurora now attaches the recording packet's `frameId`, replay-source ID, and replay-emission flag to
the readback job before asynchronous worker/map delivery. Sunbright reserves that exact ID while
the packet is open and stores native baseline, optional variant, and Aurora pixels in a bounded
rendezvous. The comparison becomes consumable only after the native producer seals it, so both
callback-before-variant and callback-after-seal ordering are valid; no arrival-order fallback or
replay-source substitution exists. Unknown IDs are ignored only for the expected smoothness-only
captures, while duplicates, capacity exhaustion, a selected frame with no baseline, and variants
submitted after sealing refuse explicitly.

The attribution output is also fail-closed on `control:no-op`: no table is printed before that
variant byte-matches its own exact native baseline, and any later mismatch suppresses the table for
the rest of the run. This repairs each row's pairing, but does not rehabilitate I008's old ranking:
the round-robin still evaluates different operations on different scene frames. Cross-operation
attribution remains exploratory until variants share a frozen frame population.
