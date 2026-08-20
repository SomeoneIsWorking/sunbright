# Host settings and RmlUi

Sunbright's primary runtime follows Dusklight's application ownership split:

- `sms-recomp/app/settings.{h,cpp}` is the typed persistence authority.
- `sms-recomp/app/frame_rate.{h,cpp}` owns frame-cadence semantics.
- `sms-recomp/ui/` owns RmlUi documents, scoped event listeners, and the prelaunch loop.
- `res/rml/settings.rcss` owns presentation; `res/LICENSES/` records copied asset licensing.
- Runtime seams include `app/` policy directly. They never read Rml elements or duplicate string
  parsing.

`./run.sh` is the product entry point and delegates to `play.sh`; `run-decomp.sh` is the explicit
decomp/Aurora oracle. Headless runs skip the prelaunch screen. The shared launcher policy selects
SDL's offscreen video driver for `SB_HEADLESS`, and Aurora requests a surfaceless WebGPU adapter, so
windowless runs do not require X11, Wayland, or another display server. `SBR_SKIP_PRELAUNCH=1`
skips the menu without changing window policy.

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
the file.

## Renderer choices

| Choice | Current behavior |
|---|---|
| Aurora | Displayed GX command-stream renderer and parity authority. |
| Native | Enables the existing SDL3-GPU offscreen parity preview for the session. Aurora remains the displayed picture. The menu says this explicitly; native display ownership waits for renderer parity and a real presentation seam. |

The native choice is intentionally not presented as complete renderer switching. CPU readback or
compositing the preview through Aurora would conceal the missing native presentation boundary and
is not an acceptable substitute.

## Framerate choices

| Choice | Simulation | Presentation | Status |
|---|---|---|---|
| Vanilla | Game-requested retrace interval, normally 30 Hz | One present per tick | Wired |
| Interpolated 60 FPS | Original 30 Hz logic | One midpoint plus exact frame | Wired |
| Interpolated Unlocked | Original 30 Hz logic | Display-timed arbitrary subframes | **Unavailable** |
| Native 60 FPS | Game retrace interval overridden from two fields to one | One present per 60 Hz game tick | Wired |
| Native Unlocked | Same game-native one-field override | Host pacing removed | Wired |

The native override is at the real semantic boundary: immediately before calling the recompiled
`JDrama::TVideo::waitForRetrace`, its requested `r4` field count becomes one. The original game
body therefore observes the override; this is not an independent host-side 60 Hz simulation.

Interpolated Unlocked is unavailable because Aurora's replay engine captures one snapshot,
mutates object/camera pairing history while generating one alpha, emits one exact replay, and
consumes the snapshot. Reusing that path several times would pair later subframes against data
already advanced by the first. The required implementation is a reusable, read-only per-tick
interpolation plan plus display-timed multi-emission replay. The menu refuses this mode rather than
silently running capped 60.

## Windowless verification

`SBR_UI_SELFTEST=N` renders exactly `N` settings frames and exits before guest memory, disc, or DOL
loading. It checks the computed panel, Play button, and all seven choices after RmlUi layout; any
required zero-area control fails. Run it through `run-safe.sh`, which keeps the window hidden and
checks the kernel's GPU-reset log. Aurora's `SB_DUMP_FRAME` captures the game `present_source`
before the Rml overlay is composited, so a black dump is expected for a UI-only frame and is not a
UI pixel oracle.
