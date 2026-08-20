# RmlUi settings and frame policy — 2026-08-20

## Ownership

The host settings layer now follows Dusklight: typed policy in `sms-recomp/app/`, RmlUi ownership in
`sms-recomp/ui/`, resources under `res/`, and a thin host composition point. This removes the prior
condition where launch scripts and interpolation code each parsed their own enable switch.

## Two controls that caught real defects

The first bounded UI run stalled. The root cause was treating `aurora_update()` as an iterator:
calling it again inside a `while` repolled SDL and returned a fresh array ending in `AURORA_NONE`
forever. Aurora returns one sentinel-terminated event array; walking that array once fixed the
stall. A debugger stack showed the main thread in SDL/X11 event polling, and the corrected two-frame
headless control exits cleanly.

The first file dump was uniformly black despite RmlUi reporting a loaded font and rendered frames.
That was an instrument mismatch, not evidence that the document was empty: `SB_DUMP_FRAME` copies
Aurora's game `present_source` before the Rml render target is composited onto the swapchain. The
replacement control checks RmlUi's computed geometry directly. After constraining the panel to the
viewport it reports a 1088x768 panel, a 180x49.2 Play button, and 7/7 visible choices in a 1280x960
offscreen target. The safe wrapper reported zero amdgpu reset/timeout/fault lines.

`SB_HEADLESS` originally hid an X11 window instead of being windowless: `run-recomp.sh` forced the
X11 SDL driver, and WebGPU initialization required a compatible WSI surface before the later
headless present guards could run. The shared launcher policy now selects SDL's offscreen driver and Aurora asks
for a surfaceless adapter in headless mode, creates the same offscreen render targets, and never
creates or configures a WSI surface. The control passes with no display server available.

## Unsupported modes are not aliases

Aurora's stream replay can produce exactly one midpoint. `interpolate_recorded_frame` updates its
pairing tables and previous camera, and `install_replay_snapshot` consumes the only snapshot. An
unlocked mode cannot call that path repeatedly without corrupting the meaning of later alphas.
`Interpolated Unlocked` therefore remains visible but unavailable, with the missing reusable
read-only interpolation plan named in the UI and docs. It is not coerced to capped 60.

The SDL3-GPU native renderer is likewise still offscreen parity work. Selecting Native enables that
preview for the session and the menu says Aurora remains displayed. Native presentation ownership
is a separate renderer milestone, not something UI plumbing can honestly manufacture.
