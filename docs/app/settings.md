# Host settings and RmlUi

Sunbright's primary runtime follows Dusklight's application ownership split:

- `sms-recomp/app/settings.{h,cpp}` is the typed persistence authority.
- `sms-recomp/app/frame_rate.{h,cpp}` owns frame-cadence semantics.
- `sms-recomp/ui/` follows Dusklight's `Document → Window → SettingsMenu` split. `ui::Runtime` owns
  the persistent in-game document, Escape routing, and the modal pause loop.
- `res/rml/{window,tabbing,settings}.rcss` owns the copied/adapted Dusklight presentation;
  `res/LICENSES/` records its CC0 source and the copied Fira Sans font licensing.
- Runtime seams include `app/` policy directly. They never read Rml elements or duplicate string
  parsing.

`./run.sh` is the product entry point and delegates to `play.sh`; `run-decomp.sh` is the explicit
decomp/Aurora oracle. The game begins immediately after host initialization; RmlUi is initialized
hidden and is never a startup gate. Escape opens the renderer/framerate window during play; Escape
or its close button closes it. The frame seam stays inside the UI modal loop while visible, so guest
simulation is paused. The shared launcher policy selects SDL's offscreen video driver for
`SB_HEADLESS`, and Aurora requests a surfaceless WebGPU adapter, so windowless runs do not require
X11, Wayland, or another display server.

## Persistence and overrides

The platform user-data directory comes from `SDL_GetPrefPath`; `sunbright.ini` is a strict,
versioned text file:

```ini
version=1
renderer=aurora
framerate=vanilla
```

Unknown keys, malformed lines, and unsupported versions fail at the config boundary. UI changes
save immediately. `SBR_FRAME_RATE` accepts `vanilla`, `interpolated-60`,
`interpolated-unlocked`, `native-60`, or `native-unlocked`; the legacy `SBR_60FPS` and
`SBR_LERP60` inputs select `interpolated-60`. `SBR_SDLGPU=1` selects the native preview for a
diagnostic session. Environment input overrides the persisted effective value without rewriting
the file. The persisted `*-unlocked` spellings remain for config compatibility; the UI calls those
modes **Match Refresh**. `SBR_DISPLAY_HZ=<rate>` supplies the active display rate to windowless
tests; normal runs query the current SDL display mode and refresh it when the window moves.

## Renderer choices

| Choice | Current behavior |
|---|---|
| Aurora | Displayed GX command-stream renderer and parity authority. |
| Native | Enables the existing SDL3-GPU offscreen parity preview for the session. Selecting it is the per-session human approval required by the renderer's GPU-safety gate. Aurora remains the displayed picture. |

The native choice is intentionally not presented as complete renderer switching. CPU readback or
compositing the preview through Aurora would conceal the missing native presentation boundary and
is not an acceptable substitute.

Renderer and supported framerate selections apply at the next simulation-tick boundary and save
immediately. Interpolation is not a one-way startup latch: switching it off disables Aurora replay;
switching it on forces the first new tick exact before later ticks interpolate, so stale pairing
history is never displayed. Environment overrides remain authoritative for diagnostic sessions.

## Framerate choices

| Choice | Simulation | Presentation | Status |
|---|---|---|---|
| Vanilla | Game-requested retrace interval, normally 30 Hz | One present per tick | Wired |
| Interpolated 60 FPS | Original 30 Hz logic | One midpoint plus exact frame | Wired |
| Interpolated Match Refresh | Original 30000/1001 Hz logic | Interpolation samples matching the active display rate | Wired |
| Native 60 FPS | 60 Hz game logic with BetterSunshineEngine-derived timing fixes | One present per 60 Hz game tick | Wired; BSE HX timer-initializer gap below |
| Native Match Refresh | Game logic at the active display rate with the same continuous BSE timing formulas | One present per game tick | Wired; BSE HX timer-initializer gap below |

The native override is at the real semantic boundary: immediately before calling the recompiled
`JDrama::TVideo::waitForRetrace`, the runtime applies BetterSunshineEngine's three-part base timing
contract: the requested retrace interval, SMS's animation-rate constant, and ModelGate's per-tick
step. The original game body therefore observes the override. Changing only the first value was the
root cause of the former double-speed Native 60 mode.

`sms-recomp/bse/` owns BSE's targeted compatibility behavior, separately from host cadence and
render interpolation. It currently keeps boid travel speed stable, preserves the fixed-delta
AnimalBird and Boss Eel animations, fixes TJointCoin/Sand Bird animation rate, scales the textbox
entry timer, and ports `HX_MotionUpdate`. The retail recompiled bodies remain linked and are
super-called outside BSE's exact call-site scope.

Match Refresh means the monitor's current nominal refresh rate, not an unpaced loop. Native Match
Refresh runs the SMS simulation itself at that rate through the same `TVideo::waitForRetrace`
override used by Native 60. Interpolated Match Refresh keeps SMS logic at 30000/1001 Hz and carries
fractional presentation credit between ticks: 120 Hz normally emits four samples, while 144 Hz
alternates four and five without long-term drift. Aurora commits object, vertex, billboard, and
camera history once, retains that tick's replay snapshot, and resamples the immutable pair for each
alpha; the last emission is exact and consumes the snapshot. Its CPU self-test proves `.25` and
`.75` produce distinct poses without advancing the tick or pairing counters.

BSE's remaining FPS-source gap is the family of discrete HX wipe timer/frame-rate initializers
installed as mid-function PPC call-site patches. Sunbright cannot faithfully express those with a
function-entry override. The proper owner is an instruction-level static-recompiler patchpoint
mechanism (or full verified replacements for every owning function); detecting magic timer values
at runtime would be a bandaid and is deliberately not used. Sunbright's existing native projection,
HUD, EFB-copy, and screen-effect widescreen implementation remains authoritative; importing BSE's
guest widescreen patches would create a second, less complete owner for the same policy.

## Windowless verification

`SBR_UI_SELFTEST=N` exits before guest memory, disc, or DOL loading. It pushes Escape through SDL,
requires the shipping Aurora event route to open the in-game window, renders exactly `N` settings
frames, checks the 1088x768 window and all seven choices have nonzero computed area inside the
viewport, then pushes Escape again and requires the production modal loop to close. Run it through
`run-safe.sh`, which keeps the window hidden and checks the kernel's GPU-reset log. The control must
name a Vulkan adapter; Aurora's sandbox-only Null fallback cannot allocate its staging buffers and
is not a valid layout result. `SB_DUMP_FRAME` captures the game `present_source` before Rml overlay
composition, so a black UI-only dump is expected and is not a UI pixel oracle.
